/* py.h — thin CPython embedding API exported to wasm / Go.
 *
 * This is the ONLY surface wasmify exports from libpython. CPython's full
 * C API stays internal; Go callers see just these functions. Pinned against
 * CPython v3.14.6.
 *
 * It is a C++ header (compiled into the wasmify bridge as C++): string
 * OUTPUTS use `std::string`, matching the bridge generator's string-output
 * handling, and string / byte INPUTS use `const char*` plus an explicit
 * length (the bridge passes `.c_str()` on the fully decoded buffer, so
 * embedded NULs survive — the typed value protocol below is binary). The
 * interpreter handle is an opaque integer token (uint64), which keeps the
 * generator unambiguous (a pointer-to-opaque-struct parameter is otherwise
 * misread as an output param) and is the conventional FFI handle idiom.
 *
 * Threading model: one wasm instance == one CPython runtime == one
 * interpreter. Multiple interpreters == multiple wasm2go module instances,
 * each with its own linear memory. No CPython sub-interpreters, no threads:
 * the main thread state holds the GIL for the instance's whole life.
 *
 * ---- Typed value protocol ------------------------------------------------
 *
 * Every value crossing the boundary is a TYPED BINARY NODE (little-endian,
 * packed). Nothing is stringified and nothing rides JSON: immutable
 * built-in values cross by value with their kind, every other object crosses
 * by handle.
 *
 *   node   := tag:u8 payload
 *     0 none      —
 *     1 bool      u8 (0/1)
 *     2 int       i64
 *     3 bigint    u8 sign (1 = negative), u32 len, len bytes of the
 *                 magnitude, little-endian (an int outside the i64 range)
 *     4 float     f64 (IEEE 754 bits)
 *     5 complex   f64 real, f64 imag
 *     6 str       u32 len, len bytes of UTF-8 encoded with the
 *                 "surrogatepass" handler, so lone surrogates round-trip
 *     7 bytes     u32 len, len bytes
 *     8 object    u64 handle, u8 kind, u8 flags, u16 type_len, type bytes
 *                 (kind: 0 object, 1 list, 2 tuple, 3 dict, 4 set,
 *                 5 frozenset, 6 function, 7 class, 8 module; flags:
 *                 bit 0 = callable(); type is the type's name as Python
 *                 prints it — "list", "__main__.Counter" — and travels
 *                 guest->host only: the host writes type_len 0)
 *     9 hostfunc  u32 id — reserved for host->guest use (a host function
 *                 crossing inline as an argument); not produced yet
 *
 *   list   := count:u32 node*
 *   kwargs := count:u32 (name_len:u32 name node)*
 *
 * Classification (what becomes a value node and what becomes a handle):
 * only EXACT instances of the immutable built-ins — None, bool, int, float,
 * complex, str, bytes — cross by value; an instance of a SUBCLASS of one of
 * them (an IntEnum member, a str subclass) is an object with its own
 * identity and attributes and crosses by handle. Handle kinds are decided
 * by CPython's own type checks and include subclasses (an OrderedDict is a
 * dict handle whose type name says "collections.OrderedDict"); "function"
 * is what inspect.isroutine accepts: functions, builtin functions, bound
 * methods, and method descriptors. Everything else — instances, exceptions,
 * generators, bytearray, range, ... — is a plain object handle.
 *
 * Objects are never serialised: the guest pins the actual object in a
 * per-interpreter registry (a strong reference held, id deduplicated by
 * object address so the same object always gets the same id) and only the
 * id crosses. Sending a handle back dereferences to THE SAME object, so
 * identity and mutation survive round trips. Each object node handed to the
 * host carries one pin the HOST owns; it releases pins via py_release.
 *
 * Every operation below that returns std::string returns a RESULT ENVELOPE:
 *
 *   envelope := status:u8 payload
 *     0 ok      payload is the operation's result (see each function)
 *     1 raised  type:(u32 len bytes) message:(u32 len bytes)
 *               traceback:(u32 len bytes) exc:node — an uncaught Python
 *               exception: the exception type's name as Python prints it,
 *               str(exc), the formatted traceback, and the exception
 *               instance itself (an object handle, or none when the failure
 *               was raised by the bridge rather than by Python code, e.g. a
 *               stale handle)
 *     2 exit    i32 code — a SystemExit the operation caught (the code as
 *               CPython would report it to the shell); the interpreter is
 *               unwound and stays usable
 *
 * ---- Python -> Go ----------------------------------------------------------
 *
 * py_new_function materialises a Go function as an ordinary Python builtin
 * function object. Calling it encodes the positional arguments as a list and
 * the keyword arguments as kwargs, dispatches over the wasmify callback import
 * to the host callback registered with py_set_go_dispatcher (method id = the
 * function id), and decodes the response:
 *
 *   response := status:u8 payload
 *     0 ok      node — the call's return value
 *     1 raised  u8 has_exc; has_exc 1: node (an exception instance handle,
 *               re-raised as-is); has_exc 0: u32 len, len bytes of a message
 *               raised as RuntimeError
 */
#ifndef PYEMBED_H
#define PYEMBED_H

#include <cstdint>
#include <string>

/* Initialize the CPython runtime (isolated config) and return an opaque
 * interpreter handle (0 on failure). Call once per wasm instance.
 *
 * `stdlib_dir` is the directory holding the Python standard library (the
 * `Lib/` tree: encodings/, io.py, codecs.py, ...). It becomes the sole
 * module search path. The isolated config ignores PYTHONPATH, so the host
 * MUST pass this (the dir is reached through the runtime's WASI filesystem
 * mount). Pass NULL/empty to fall back to CPython's default path discovery
 * (usually fails in the sandbox — provide the path).
 *
 * The isolated config ignores the environment, with one deliberate
 * exception: PYTHONVERBOSE in the guest environment turns on the import
 * trace, and a failed initialisation reports the pending exception on
 * stderr — the way to see why a runtime did not come up. */
uint64_t py_new(const char *stdlib_dir);

/* Evaluate `src` in the persistent __main__ namespace (REPL-like state
 * persistence). ok payload:
 *
 *   result:node stdout:(u32 len + bytes) stderr:(u32 len + bytes)
 *
 * result is the value of the LAST statement when it is an expression
 * statement, none otherwise. A raised / exit envelope is followed by the same
 * stdout/stderr pair — what the code printed before failing is still
 * delivered. */
std::string py_eval(uint64_t h, const char *src);

/* Destroy the interpreter: drop every registry pin, then finalize the
 * runtime. */
void py_close(uint64_t h);

/* ---- Interruption support ------------------------------------------------
 *
 * Mirrors PyThreadState_SetAsyncExc() done as plain memory writes, so a host
 * watchdog goroutine can raise KeyboardInterrupt in a running interpreter
 * WITHOUT executing any wasm/C code on that instance (which would corrupt the
 * shared linear-memory C stack). To interrupt, the host performs:
 *
 *   *(uint32_t *)py_async_exc_addr(h)   = py_keyboard_interrupt_obj(h);
 *   atomic_or((uint32_t *)py_eval_breaker_addr(h), 8); // _PY_ASYNC_EXCEPTION_BIT
 *
 * CPython checks eval_breaker on every bytecode backward edge (3.9+), so it
 * raises KeyboardInterrupt at the next loop iteration — including a pure
 * `while True: pass`. PyExc_KeyboardInterrupt is immortal in 3.14, so storing
 * it needs no refcount bookkeeping. Addresses are 32-bit linear-memory
 * offsets (wasm32). The async-exception bit is the constant 8 (1u<<3). */
uint32_t py_eval_breaker_addr(uint64_t h);      /* &tstate->eval_breaker */
uint32_t py_async_exc_addr(uint64_t h);         /* &tstate->async_exc    */
uint32_t py_keyboard_interrupt_obj(uint64_t h); /* PyExc_KeyboardInterrupt */

/* ---- Object protocol -----------------------------------------------------
 *
 * Every operation takes object HANDLES (u64 ids from object nodes) for the
 * objects it acts on and typed nodes for values. Unless noted, the ok payload
 * is one node. */

/* import name. ok: node (the module). */
std::string py_import(uint64_t h, const char *name, uint32_t name_len);

/* callable(*args, **kwargs). args is a node list, kwargs a kwargs list. */
std::string py_call(uint64_t h, uint64_t callable,
                    const char *args, uint32_t args_len,
                    const char *kwargs, uint32_t kwargs_len);

/* getattr(obj, name)(*args, **kwargs) in one crossing. */
std::string py_call_method(uint64_t h, uint64_t obj,
                           const char *name, uint32_t name_len,
                           const char *args, uint32_t args_len,
                           const char *kwargs, uint32_t kwargs_len);

/* getattr / setattr. ok payload of py_setattr: empty. */
std::string py_getattr(uint64_t h, uint64_t obj, const char *name, uint32_t name_len);
std::string py_setattr(uint64_t h, uint64_t obj, const char *name, uint32_t name_len,
                       const char *val, uint32_t val_len);

/* obj[key], obj[key] = val, del obj[key]. key / val are single nodes. ok
 * payload of py_setitem / py_delitem: empty. */
std::string py_getitem(uint64_t h, uint64_t obj, const char *key, uint32_t key_len);
std::string py_setitem(uint64_t h, uint64_t obj, const char *key, uint32_t key_len,
                       const char *val, uint32_t val_len);
std::string py_delitem(uint64_t h, uint64_t obj, const char *key, uint32_t key_len);

/* len(obj). ok: i64. */
std::string py_len(uint64_t h, uint64_t obj);

/* key in obj. ok: u8. */
std::string py_contains(uint64_t h, uint64_t obj, const char *key, uint32_t key_len);

/* str(obj) / repr(obj). ok: u32 len + UTF-8 bytes. */
std::string py_str(uint64_t h, uint64_t obj);
std::string py_repr(uint64_t h, uint64_t obj);

/* isinstance(obj, cls). ok: u8. */
std::string py_isinstance(uint64_t h, uint64_t obj, uint64_t cls);

/* list(obj) — every element the object yields when iterated. ok: node list.
 * Backs list / tuple / set contents and dict keys alike. */
std::string py_iter(uint64_t h, uint64_t obj);

/* dict operations. py_dict_get ok: u8 exists + node (none when absent — a
 * missing key is not an error). py_dict_keys / py_dict_values ok: node list.
 * py_dict_items ok: node list of 2 * len nodes (key, value, key, value...). */
std::string py_dict_get(uint64_t h, uint64_t obj, const char *key, uint32_t key_len);
std::string py_dict_keys(uint64_t h, uint64_t obj);
std::string py_dict_values(uint64_t h, uint64_t obj);
std::string py_dict_items(uint64_t h, uint64_t obj);

/* list.append(each of elems). ok payload: empty. */
std::string py_list_append(uint64_t h, uint64_t obj, const char *elems, uint32_t elems_len);

/* set.add(val) / set.discard(val). ok payload: empty. */
std::string py_set_add(uint64_t h, uint64_t obj, const char *val, uint32_t val_len);
std::string py_set_discard(uint64_t h, uint64_t obj, const char *val, uint32_t val_len);

/* Materialise host data as a guest aggregate in one crossing. elems is a node
 * list; items is a node list of 2 * n nodes (key, value, ...). ok: node. */
std::string py_new_list(uint64_t h, const char *elems, uint32_t elems_len);
std::string py_new_tuple(uint64_t h, const char *elems, uint32_t elems_len);
std::string py_new_set(uint64_t h, const char *elems, uint32_t elems_len);
std::string py_new_frozenset(uint64_t h, const char *elems, uint32_t elems_len);
std::string py_new_dict(uint64_t h, const char *items, uint32_t items_len);

/* Materialise the host function bound under func_id as a Python builtin
 * function named name (see "Python -> Go" above). ok: node (a function
 * handle). */
std::string py_new_function(uint64_t h, const char *name, uint32_t name_len, int32_t func_id);

/* Release registry pins: ids is a packed array of u64 handles (one pin each).
 * ok payload: empty. */
std::string py_release(uint64_t h, const char *ids, uint32_t ids_len);

/* Register the host callback id every Python -> Go call from this instance
 * dispatches to (the wasmify callback_invoke callback_id). */
void py_set_go_dispatcher(uint64_t h, int32_t callback_id);

#endif /* PYEMBED_H */
