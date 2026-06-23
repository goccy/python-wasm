/* py.h — thin CPython embedding API exported to wasm / Go.
 *
 * This is the ONLY surface wasmify exports from libpython. CPython's full
 * C API stays internal; Go callers see just these functions. Pinned against
 * CPython v3.14.6.
 *
 * It is a C++ header (compiled into the wasmify bridge as C++): string OUTPUTS
 * use `std::string*`, matching the bridge generator's string-output handling,
 * and string INPUTS use `const char*` (the bridge passes `.c_str()`). The
 * interpreter handle is an opaque integer token (uint64), which keeps the
 * generator unambiguous (a pointer-to-opaque-struct parameter is otherwise
 * misread as an output param) and is the conventional FFI handle idiom.
 *
 * Threading model: one wasm instance == one CPython runtime == one
 * interpreter. Multiple interpreters == multiple wasm2go module instances,
 * each with its own linear memory. No CPython sub-interpreters.
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
 * (usually fails in the sandbox — provide the path). */
uint64_t py_new(const char *stdlib_dir);

/* Evaluate `src` and return the result as a JSON object string:
 *
 *   {"ok":<bool>,"repr":<string>,"stdout":<string>,"stderr":<string>,
 *    "error":<string>}
 *
 * If `src` parses as a single expression, "repr" holds repr() of its value
 * (empty for statements / None). "stdout"/"stderr" hold anything written to
 * sys.stdout/sys.stderr during execution. On an uncaught exception, "ok" is
 * false and "error" holds the formatted traceback. Globals persist across
 * calls on the same handle (REPL-like).
 *
 * A single JSON string return is used because the bridge generator surfaces
 * only one response value to Go; bundling the outputs keeps one round-trip
 * and one atomic result. The Go wrapper unmarshals it. */
std::string py_eval(uint64_t h, const char *src);

/* Destroy the interpreter and finalize the runtime. */
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

#endif /* PYEMBED_H */
