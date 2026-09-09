/* py.cc — implementation of the thin CPython embedding API (C++).
 * Pinned against CPython v3.14.6. See py.h for the contract: the typed
 * value protocol, the result envelopes, and the Python -> Go dispatch. */

#include "py.h"

#include <Python.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/* The host-socket and host-subprocess libc shims (socket()/connect()/
 * getaddrinfo() and the posix_spawn family/waitpid/pipe) are provided by
 * wasmify: it deploys and links them when bridge.HostSockets /
 * bridge.HostSubprocess is set in wasmify.json. */

/* ---- Python -> Go dispatch -------------------------------------------------
 * The wasmify callback import: the ONE host entry point every Python -> Go
 * call goes through. Declared here rather than via the generated bridge
 * header so py.cc stays self-contained; the signature must match
 * bridge/api_bridge.h's WASM_IMPORT(wasmify, callback_invoke). The i64 result
 * packs (resp_ptr << 32) | resp_len; the response buffer transfers to us (we
 * free it), while the request buffer only lends: the host reads it during
 * the call and we keep ownership. */
__attribute__((import_module("wasmify"), import_name("callback_invoke")))
extern "C" int64_t wasmify_callback_invoke(int32_t callback_id, int32_t method_id,
                                           void *req, size_t req_len);

namespace {

/* ---- interpreter state --------------------------------------------------- */

struct PyEmbed {
    PyObject *main = nullptr;          /* strong: the __main__ module          */
    PyObject *globals = nullptr;       /* borrowed from main: its __dict__      */
    PyObject *kbd_interrupt = nullptr; /* borrowed: PyExc_KeyboardInterrupt    */
    PyThreadState *tstate = nullptr;   /* main thread state for this runtime   */
};

/* One CPython runtime per wasm instance. */
PyEmbed *g_embed = nullptr;

/* The Go-side callback id every Python -> Go call from this instance
 * dispatches to. 0 = no dispatcher registered (py_set_go_dispatcher not
 * called). */
int32_t g_go_cb = 0;

PyEmbed *resolve(uint64_t h) {
    auto *e = reinterpret_cast<PyEmbed *>(static_cast<uintptr_t>(h));
    return (e != nullptr && e == g_embed) ? e : nullptr;
}

/* ---- typed value codec ---------------------------------------------------
 * The node/envelope wire format documented in py.h. Little-endian, packed;
 * every value crosses with its kind, objects cross by handle. */

enum : uint8_t {
    NODE_NONE     = 0,
    NODE_BOOL     = 1,
    NODE_INT      = 2,
    NODE_BIGINT   = 3,
    NODE_FLOAT    = 4,
    NODE_COMPLEX  = 5,
    NODE_STR      = 6,
    NODE_BYTES    = 7,
    NODE_OBJECT   = 8,
    NODE_HOSTFUNC = 9,
};

enum : uint8_t {
    KIND_OBJECT    = 0,
    KIND_LIST      = 1,
    KIND_TUPLE     = 2,
    KIND_DICT      = 3,
    KIND_SET       = 4,
    KIND_FROZENSET = 5,
    KIND_FUNCTION  = 6,
    KIND_CLASS     = 7,
    KIND_MODULE    = 8,
};

enum : uint8_t { FLAG_CALLABLE = 1 };

enum : uint8_t { ENV_OK = 0, ENV_RAISED = 1, ENV_EXIT = 2 };

void put_u8(std::string &b, uint8_t v) { b.push_back(static_cast<char>(v)); }
void put_u16(std::string &b, uint16_t v) {
    for (int i = 0; i < 2; i++) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u32(std::string &b, uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_i32(std::string &b, int32_t v) { put_u32(b, static_cast<uint32_t>(v)); }
void put_u64(std::string &b, uint64_t v) {
    for (int i = 0; i < 8; i++) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_i64(std::string &b, int64_t v) { put_u64(b, static_cast<uint64_t>(v)); }
void put_f64(std::string &b, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    put_u64(b, bits);
}
void put_bytes(std::string &b, const char *p, uint32_t len) {
    put_u32(b, len);
    if (len) b.append(p, len);
}
void put_str(std::string &b, const std::string &s) {
    put_bytes(b, s.data(), static_cast<uint32_t>(s.size()));
}

/* NodeReader walks a node buffer with bounds checking; any overrun flips
 * fail and every later read yields zero, so decoding is total. */
struct NodeReader {
    const unsigned char *p;
    const unsigned char *end;
    bool fail;
    NodeReader(const char *data, uint32_t len)
        : p(reinterpret_cast<const unsigned char *>(data)), end(p + len), fail(false) {}
    bool need(size_t n) {
        if (fail || static_cast<size_t>(end - p) < n) { fail = true; return false; }
        return true;
    }
    uint8_t get_u8() { if (!need(1)) return 0; return *p++; }
    uint16_t get_u16() {
        if (!need(2)) return 0;
        uint16_t v = static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8;
        p += 2;
        return v;
    }
    uint32_t get_u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(p[i]) << (8 * i);
        p += 4;
        return v;
    }
    uint64_t get_u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
        p += 8;
        return v;
    }
    int64_t get_i64() { return static_cast<int64_t>(get_u64()); }
    double get_f64() {
        uint64_t bits = get_u64();
        double v;
        memcpy(&v, &bits, 8);
        return v;
    }
    const char *get_bytes(uint32_t len) {
        if (!need(len)) return nullptr;
        const char *q = reinterpret_cast<const char *>(p);
        p += len;
        return q;
    }
};

/* ---- strings --------------------------------------------------------------- */

/* unicode_to_std encodes a str as UTF-8 with surrogatepass so any str —
 * including one holding lone surrogates — round-trips byte-exactly. */
bool unicode_to_std(PyObject *u, std::string &out) {
    PyObject *b = PyUnicode_AsEncodedString(u, "utf-8", "surrogatepass");
    if (b == nullptr) return false;
    out.assign(PyBytes_AS_STRING(b), static_cast<size_t>(PyBytes_GET_SIZE(b)));
    Py_DECREF(b);
    return true;
}

/* unicode_from is the inverse: bytes the host produced back into a str. */
PyObject *unicode_from(const char *p, uint32_t len) {
    return PyUnicode_Decode(p ? p : "", static_cast<Py_ssize_t>(len), "utf-8", "surrogatepass");
}

/* str_of stringifies via PyObject_Str; empty (error cleared) on failure. */
std::string str_of(PyObject *o) {
    std::string out;
    PyObject *s = PyObject_Str(o);
    if (s == nullptr) { PyErr_Clear(); return out; }
    if (!unicode_to_std(s, out)) PyErr_Clear();
    Py_DECREF(s);
    return out;
}

/* type_name renders a type the way Python prints it: the qualified name,
 * prefixed by the module unless that is builtins ("list", "collections.
 * OrderedDict", "__main__.Counter"). */
std::string type_name(PyTypeObject *t) {
    std::string q, m;
    PyObject *qn = PyType_GetQualName(t);
    if (qn != nullptr) { unicode_to_std(qn, q); Py_DECREF(qn); }
    PyObject *mn = PyType_GetModuleName(t);
    if (mn != nullptr) { unicode_to_std(mn, m); Py_DECREF(mn); }
    PyErr_Clear();
    if (m.empty() || m == "builtins") return q;
    return m + "." + q;
}

/* ---- object-handle registry (identity-preserving pins) ----------------------
 * Pins the actual object: the registry holds one strong reference, ids are
 * deduplicated by object address so the same object always crosses under the
 * same id, and pin counts align the guest reference with host-side wrapper
 * liveness (the host releases pins via py_release). While an object is
 * pinned it cannot be freed, so its address cannot be reused by another
 * live object and the by-address map stays exact. */
std::unordered_map<uint64_t, PyObject *> g_reg_by_id;
std::unordered_map<PyObject *, uint64_t> g_reg_by_addr;
std::unordered_map<uint64_t, uint64_t> g_reg_pins;
uint64_t g_reg_next = 0;

uint64_t reg_pin(PyObject *o) {
    uint64_t id;
    auto it = g_reg_by_addr.find(o);
    if (it == g_reg_by_addr.end()) {
        id = ++g_reg_next;
        g_reg_by_id[id] = Py_NewRef(o);
        g_reg_by_addr[o] = id;
    } else {
        id = it->second;
    }
    g_reg_pins[id]++;
    return id;
}

/* reg_lookup returns a BORROWED reference, or NULL for an unknown id. */
PyObject *reg_lookup(uint64_t id) {
    auto it = g_reg_by_id.find(id);
    return it == g_reg_by_id.end() ? nullptr : it->second;
}

void reg_release(uint64_t id) {
    auto it = g_reg_by_id.find(id);
    if (it == g_reg_by_id.end()) return;
    if (--g_reg_pins[id] > 0) return;
    PyObject *o = it->second;
    g_reg_by_addr.erase(o);
    g_reg_by_id.erase(it);
    g_reg_pins.erase(id);
    /* Dropping the last pin can run __del__; an exception there is reported
     * through sys.unraisablehook, exactly like any other refcount drop. */
    Py_DECREF(o);
}

void reg_clear() {
    std::vector<PyObject *> held;
    held.reserve(g_reg_by_id.size());
    for (auto &e : g_reg_by_id) held.push_back(e.second);
    g_reg_by_id.clear();
    g_reg_by_addr.clear();
    g_reg_pins.clear();
    g_reg_next = 0;
    for (PyObject *o : held) Py_DECREF(o);
}

/* ---- classification ----------------------------------------------------- */

/* kind_of maps an object that crosses by handle onto its node kind, using
 * CPython's own type checks (subclasses included). "function" is what
 * inspect.isroutine accepts. The order only matters for disjointness, and
 * CPython refuses classes that inherit two incompatible built-in layouts, so
 * at most one container check can match. */
uint8_t kind_of(PyObject *o) {
    if (PyType_Check(o)) return KIND_CLASS;
    if (PyModule_Check(o)) return KIND_MODULE;
    if (PyList_Check(o)) return KIND_LIST;
    if (PyTuple_Check(o)) return KIND_TUPLE;
    if (PyDict_Check(o)) return KIND_DICT;
    if (PyFrozenSet_Check(o)) return KIND_FROZENSET;
    if (PySet_Check(o)) return KIND_SET;
    if (PyFunction_Check(o) || PyMethod_Check(o) || PyCFunction_Check(o) ||
        Py_IS_TYPE(o, &PyMethodDescr_Type) || Py_IS_TYPE(o, &PyWrapperDescr_Type))
        return KIND_FUNCTION;
    return KIND_OBJECT;
}

/* encode_handle appends an object node for o, pinning it (the host owns the
 * new pin). */
void encode_handle(PyObject *o, std::string &out) {
    uint64_t id = reg_pin(o);
    put_u8(out, NODE_OBJECT);
    put_u64(out, id);
    put_u8(out, kind_of(o));
    put_u8(out, PyCallable_Check(o) ? FLAG_CALLABLE : 0);
    std::string tn = type_name(Py_TYPE(o));
    if (tn.size() > 0xFFFF) tn.resize(0xFFFF);
    put_u16(out, static_cast<uint16_t>(tn.size()));
    out.append(tn);
}

/* encode_object appends one node for o. Exact instances of the immutable
 * built-ins cross by value; everything else (subclass instances included)
 * crosses by handle. Never raises: a value that cannot be encoded by value
 * falls back to a handle. */
void encode_object(PyObject *o, std::string &out) {
    if (o == nullptr || o == Py_None) {
        put_u8(out, NODE_NONE);
        return;
    }
    if (PyBool_Check(o)) {
        put_u8(out, NODE_BOOL);
        put_u8(out, o == Py_True ? 1 : 0);
        return;
    }
    if (PyLong_CheckExact(o)) {
        int overflow = 0;
        long long v = PyLong_AsLongLongAndOverflow(o, &overflow);
        if (overflow == 0 && !(v == -1 && PyErr_Occurred())) {
            put_u8(out, NODE_INT);
            put_i64(out, static_cast<int64_t>(v));
            return;
        }
        PyErr_Clear();
        PyObject *mag = PyNumber_Absolute(o);
        if (mag != nullptr) {
            const int flags = Py_ASNATIVEBYTES_LITTLE_ENDIAN | Py_ASNATIVEBYTES_UNSIGNED_BUFFER;
            Py_ssize_t n = PyLong_AsNativeBytes(mag, nullptr, 0, flags);
            if (n > 0) {
                std::string buf(static_cast<size_t>(n), '\0');
                if (PyLong_AsNativeBytes(mag, &buf[0], n, flags) >= 0) {
                    Py_DECREF(mag);
                    put_u8(out, NODE_BIGINT);
                    put_u8(out, overflow < 0 ? 1 : 0);
                    put_str(out, buf);
                    return;
                }
            }
            Py_DECREF(mag);
        }
        PyErr_Clear();
        encode_handle(o, out);
        return;
    }
    if (PyFloat_CheckExact(o)) {
        put_u8(out, NODE_FLOAT);
        put_f64(out, PyFloat_AS_DOUBLE(o));
        return;
    }
    if (PyComplex_CheckExact(o)) {
        put_u8(out, NODE_COMPLEX);
        put_f64(out, PyComplex_RealAsDouble(o));
        put_f64(out, PyComplex_ImagAsDouble(o));
        return;
    }
    if (PyUnicode_CheckExact(o)) {
        std::string s;
        if (unicode_to_std(o, s)) {
            put_u8(out, NODE_STR);
            put_str(out, s);
            return;
        }
        PyErr_Clear();
        encode_handle(o, out);
        return;
    }
    if (PyBytes_CheckExact(o)) {
        put_u8(out, NODE_BYTES);
        put_bytes(out, PyBytes_AS_STRING(o), static_cast<uint32_t>(PyBytes_GET_SIZE(o)));
        return;
    }
    encode_handle(o, out);
}

/* encode_list appends a node list for the items of a tuple or list. */
void encode_seq(PyObject *seq, std::string &out) {
    Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
    put_u32(out, static_cast<uint32_t>(n));
    PyObject **items = PySequence_Fast_ITEMS(seq);
    for (Py_ssize_t i = 0; i < n; i++) encode_object(items[i], out);
}

/* encode_kwargs appends a kwargs list for a dict of str -> value (NULL or
 * empty dict -> count 0). */
void encode_kwargs(PyObject *kwargs, std::string &out) {
    if (kwargs == nullptr || !PyDict_Check(kwargs)) {
        put_u32(out, 0);
        return;
    }
    put_u32(out, static_cast<uint32_t>(PyDict_GET_SIZE(kwargs)));
    Py_ssize_t pos = 0;
    PyObject *k, *v;
    while (PyDict_Next(kwargs, &pos, &k, &v)) {
        std::string name;
        if (PyUnicode_Check(k)) unicode_to_std(k, name);
        PyErr_Clear();
        put_str(out, name);
        encode_object(v, out);
    }
}

/* decode_node turns one wire node into a NEW reference. On malformed input
 * or a stale handle, sets err and returns NULL (no Python error set). */
PyObject *decode_node(NodeReader &r, std::string &err) {
    uint8_t tag = r.get_u8();
    if (r.fail) { err = "malformed value node"; return nullptr; }
    switch (tag) {
    case NODE_NONE:
        return Py_NewRef(Py_None);
    case NODE_BOOL:
        return PyBool_FromLong(r.get_u8() != 0);
    case NODE_INT:
        return PyLong_FromLongLong(static_cast<long long>(r.get_i64()));
    case NODE_BIGINT: {
        uint8_t sign = r.get_u8();
        uint32_t len = r.get_u32();
        const char *p = r.get_bytes(len);
        if (r.fail) { err = "malformed bigint node"; return nullptr; }
        PyObject *v;
        if (len == 0) {
            v = PyLong_FromLong(0);
        } else {
            v = PyLong_FromNativeBytes(p, len,
                                       Py_ASNATIVEBYTES_LITTLE_ENDIAN | Py_ASNATIVEBYTES_UNSIGNED_BUFFER);
        }
        if (v != nullptr && sign) {
            PyObject *neg = PyNumber_Negative(v);
            Py_DECREF(v);
            v = neg;
        }
        if (v == nullptr) { PyErr_Clear(); err = "invalid bigint node"; }
        return v;
    }
    case NODE_FLOAT:
        return PyFloat_FromDouble(r.get_f64());
    case NODE_COMPLEX: {
        double re = r.get_f64();
        double im = r.get_f64();
        return PyComplex_FromDoubles(re, im);
    }
    case NODE_STR: {
        uint32_t len = r.get_u32();
        const char *p = r.get_bytes(len);
        if (r.fail) { err = "malformed str node"; return nullptr; }
        PyObject *s = unicode_from(p, len);
        if (s == nullptr) { PyErr_Clear(); err = "invalid UTF-8 in str node"; }
        return s;
    }
    case NODE_BYTES: {
        uint32_t len = r.get_u32();
        const char *p = r.get_bytes(len);
        if (r.fail) { err = "malformed bytes node"; return nullptr; }
        return PyBytes_FromStringAndSize(p ? p : "", static_cast<Py_ssize_t>(len));
    }
    case NODE_OBJECT: {
        uint64_t id = r.get_u64();
        (void)r.get_u8();                 /* kind: advisory on decode  */
        (void)r.get_u8();                 /* flags: advisory on decode */
        uint16_t tlen = r.get_u16();
        (void)r.get_bytes(tlen);          /* type name: advisory       */
        if (r.fail) { err = "malformed object node"; return nullptr; }
        PyObject *o = reg_lookup(id);
        if (o == nullptr) {
            char buf[64];
            snprintf(buf, sizeof(buf), "stale Python object handle %llu",
                     static_cast<unsigned long long>(id));
            err = buf;
            return nullptr;
        }
        return Py_NewRef(o);
    }
    case NODE_HOSTFUNC:
        err = "hostfunc nodes are not supported";
        return nullptr;
    default:
        err = "unknown value node tag";
        return nullptr;
    }
}

/* decode_single decodes exactly one node from a buffer. */
PyObject *decode_single(const char *buf, uint32_t len, std::string &err) {
    NodeReader r(buf, len);
    return decode_node(r, err);
}

/* decode_items decodes a node list into a vector of new references; on
 * failure every decoded item is released and err is set. */
bool decode_items(const char *buf, uint32_t len, std::vector<PyObject *> &items, std::string &err) {
    NodeReader r(buf, len);
    uint32_t count = r.get_u32();
    if (r.fail) { err = "malformed node list"; return false; }
    items.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        PyObject *o = decode_node(r, err);
        if (o == nullptr) {
            for (PyObject *x : items) Py_DECREF(x);
            items.clear();
            return false;
        }
        items.push_back(o);
    }
    return true;
}

/* decode_args decodes a node list into a NEW tuple (the call's positional
 * arguments). */
PyObject *decode_args(const char *buf, uint32_t len, std::string &err) {
    std::vector<PyObject *> items;
    if (!decode_items(buf, len, items, err)) return nullptr;
    PyObject *t = PyTuple_New(static_cast<Py_ssize_t>(items.size()));
    if (t == nullptr) {
        for (PyObject *x : items) Py_DECREF(x);
        PyErr_Clear();
        err = "out of memory";
        return nullptr;
    }
    for (size_t i = 0; i < items.size(); i++) PyTuple_SET_ITEM(t, i, items[i]); /* steals */
    return t;
}

/* decode_kwargs decodes a kwargs list into a NEW dict, or NULL with err
 * empty when the list is empty (PyObject_Call accepts NULL kwargs). */
PyObject *decode_kwargs(const char *buf, uint32_t len, std::string &err) {
    NodeReader r(buf, len);
    uint32_t count = r.get_u32();
    if (r.fail || count == 0) {
        if (r.fail && len > 0) err = "malformed kwargs list";
        return nullptr;
    }
    PyObject *d = PyDict_New();
    if (d == nullptr) { PyErr_Clear(); err = "out of memory"; return nullptr; }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t nlen = r.get_u32();
        const char *np = r.get_bytes(nlen);
        if (r.fail) { err = "malformed kwargs list"; Py_DECREF(d); return nullptr; }
        PyObject *name = unicode_from(np, nlen);
        if (name == nullptr) { PyErr_Clear(); err = "invalid kwarg name"; Py_DECREF(d); return nullptr; }
        PyObject *v = decode_node(r, err);
        if (v == nullptr) { Py_DECREF(name); Py_DECREF(d); return nullptr; }
        int rc = PyDict_SetItem(d, name, v);
        Py_DECREF(name);
        Py_DECREF(v);
        if (rc < 0) { PyErr_Clear(); err = "cannot build kwargs"; Py_DECREF(d); return nullptr; }
    }
    return d;
}

/* ---- result envelopes ----------------------------------------------------- */

/* env_fail is a raised envelope for a failure detected by the bridge itself
 * (stale handle, malformed node): reported as a RuntimeError with no
 * traceback and no exception instance. */
std::string env_fail(const std::string &msg) {
    std::string out;
    put_u8(out, ENV_RAISED);
    put_str(out, "RuntimeError");
    put_str(out, msg);
    put_str(out, std::string());
    put_u8(out, NODE_NONE);
    return out;
}

/* format_traceback renders exc via traceback.format_exception (error
 * cleared, empty string on failure). */
std::string format_traceback(PyObject *exc) {
    std::string result;
    PyObject *mod = PyImport_ImportModule("traceback");
    if (mod != nullptr) {
        PyObject *lines = PyObject_CallMethod(mod, "format_exception", "O", exc);
        if (lines != nullptr) {
            PyObject *empty = PyUnicode_FromString("");
            PyObject *joined = empty ? PyUnicode_Join(empty, lines) : nullptr;
            if (joined != nullptr) { unicode_to_std(joined, result); Py_DECREF(joined); }
            Py_XDECREF(empty);
            Py_DECREF(lines);
        }
        Py_DECREF(mod);
    }
    PyErr_Clear();
    return result;
}

/* report_init_exception prints the exception a failed Py_InitializeFromConfig
 * left pending — type, message, and the Python frames — straight to the C
 * stderr. PyStatus only carries a fixed message ("failed to initialize
 * importlib"); the pending exception says WHICH import or check failed. The
 * runtime is only partly initialised at that point (no sys.stderr, so
 * PyErr_Print cannot be used), hence the hand-rolled walk over public C API
 * only. Best-effort: anything that cannot be rendered is skipped. */
void report_init_exception() {
    PyObject *exc = PyErr_GetRaisedException();
    if (exc == nullptr) {
        fprintf(stderr, "[py] no pending Python exception\n");
        return;
    }
    std::string msg = str_of(exc);
    fprintf(stderr, "[py] pending exception: %s: %s\n", Py_TYPE(exc)->tp_name, msg.c_str());
    for (PyObject *cause = exc; cause != nullptr;) {
        PyObject *tb = PyException_GetTraceback(cause);
        for (PyObject *t = tb; t != nullptr && PyTraceBack_Check(t);) {
            auto *tbo = reinterpret_cast<PyTracebackObject *>(t);
            PyFrameObject *frame = tbo->tb_frame;
            if (frame != nullptr) {
                PyCodeObject *code = PyFrame_GetCode(frame);
                std::string file, name;
                if (code != nullptr) {
                    if (code->co_filename) unicode_to_std(code->co_filename, file);
                    if (code->co_name) unicode_to_std(code->co_name, name);
                    Py_DECREF(code);
                }
                PyErr_Clear();
                fprintf(stderr, "[py]   File \"%s\", line %d, in %s\n", file.c_str(),
                        PyFrame_GetLineNumber(frame), name.c_str());
            }
            t = reinterpret_cast<PyObject *>(tbo->tb_next);
        }
        Py_XDECREF(tb);
        PyObject *next = PyException_GetCause(cause);
        if (next == nullptr) next = PyException_GetContext(cause);
        if (cause != exc) Py_DECREF(cause);
        cause = next;
        if (cause != nullptr) {
            std::string cmsg = str_of(cause);
            fprintf(stderr, "[py] caused by: %s: %s\n", Py_TYPE(cause)->tp_name, cmsg.c_str());
        }
    }
    Py_DECREF(exc);
}

/* system_exit_code mirrors CPython's handling of an uncaught SystemExit:
 * None -> 0, an int -> that value, anything else is printed to sys.stderr
 * and exits 1. */
int32_t system_exit_code(PyObject *exc) {
    PyObject *code = PyObject_GetAttrString(exc, "code");
    if (code == nullptr) { PyErr_Clear(); return 1; }
    int32_t rc;
    if (code == Py_None) {
        rc = 0;
    } else if (PyLong_Check(code)) {
        rc = static_cast<int32_t>(PyLong_AsLong(code));
        if (PyErr_Occurred()) { PyErr_Clear(); rc = -1; }
    } else {
        PyObject *stderr_ = PySys_GetObject("stderr"); /* borrowed */
        if (stderr_ != nullptr && stderr_ != Py_None) {
            if (PyFile_WriteObject(code, stderr_, Py_PRINT_RAW) == 0)
                PyFile_WriteString("\n", stderr_);
        }
        PyErr_Clear();
        rc = 1;
    }
    Py_DECREF(code);
    return rc;
}

/* append_raised consumes the current Python exception and appends the
 * raised (or, for SystemExit, exit) envelope to out. The exception instance
 * crosses as a handle so the host can inspect it and hand it back. */
void append_raised(std::string &out) {
    PyObject *exc = PyErr_GetRaisedException();
    if (exc == nullptr) {
        out += env_fail("operation failed without a Python exception");
        return;
    }
    if (PyErr_GivenExceptionMatches(exc, PyExc_SystemExit)) {
        put_u8(out, ENV_EXIT);
        put_i32(out, system_exit_code(exc));
        Py_DECREF(exc);
        return;
    }
    put_u8(out, ENV_RAISED);
    put_str(out, type_name(Py_TYPE(exc)));
    put_str(out, str_of(exc));
    put_str(out, format_traceback(exc));
    encode_object(exc, out);
    Py_DECREF(exc);
}

/* node_result wraps a NEW reference (or NULL with a Python error set) as an
 * envelope carrying one node. Consumes the reference. */
std::string node_result(PyObject *o) {
    std::string out;
    if (o == nullptr) {
        append_raised(out);
        return out;
    }
    put_u8(out, ENV_OK);
    encode_object(o, out);
    Py_DECREF(o);
    return out;
}

/* list_result wraps a NEW sequence reference as an envelope carrying the
 * node list of its items. Consumes the reference. */
std::string list_result(PyObject *seq) {
    std::string out;
    if (seq == nullptr) {
        append_raised(out);
        return out;
    }
    PyObject *fast = PySequence_Fast(seq, "expected a sequence");
    Py_DECREF(seq);
    if (fast == nullptr) {
        append_raised(out);
        return out;
    }
    put_u8(out, ENV_OK);
    encode_seq(fast, out);
    Py_DECREF(fast);
    return out;
}

/* status_result wraps a CPython int status (< 0 = error set) as an empty
 * ok envelope or a raised one. */
std::string status_result(int rc) {
    std::string out;
    if (rc < 0) {
        append_raised(out);
        return out;
    }
    put_u8(out, ENV_OK);
    return out;
}

/* ---- shared op preamble -------------------------------------------------- */

/* ready checks the interpreter handle; on failure fills out with the error
 * envelope. */
bool ready(uint64_t h, std::string &out) {
    if (resolve(h) == nullptr) {
        out = env_fail("invalid interpreter handle");
        return false;
    }
    return true;
}

/* object_of resolves an object handle to a BORROWED reference; on failure
 * fills out with the error envelope. */
PyObject *object_of(uint64_t id, std::string &out) {
    PyObject *o = reg_lookup(id);
    if (o == nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "stale Python object handle %llu",
                 static_cast<unsigned long long>(id));
        out = env_fail(buf);
    }
    return o;
}

/* value_of decodes one node; on failure fills out with the error envelope. */
PyObject *value_of(const char *buf, uint32_t len, std::string &out) {
    std::string err;
    PyObject *o = decode_single(buf, len, err);
    if (o == nullptr) out = env_fail(err);
    return o;
}

/* name_of builds a str from a (ptr, len) identifier; on failure fills out. */
PyObject *name_of(const char *p, uint32_t len, std::string &out) {
    PyObject *s = unicode_from(p, len);
    if (s == nullptr) {
        PyErr_Clear();
        out = env_fail("invalid UTF-8 in name");
    }
    return s;
}

/* ---- stdout / stderr capture for py_eval ---------------------------------- */

PyObject *redirect_stream(const char *name, PyObject **sink) {
    PyObject *sys = PyImport_ImportModule("sys");
    PyObject *io = PyImport_ImportModule("io");
    PyObject *old = nullptr, *buf = nullptr;
    if (sys != nullptr && io != nullptr) {
        old = PyObject_GetAttrString(sys, name);
        buf = PyObject_CallMethod(io, "StringIO", nullptr);
        if (buf != nullptr) PyObject_SetAttrString(sys, name, buf);
    }
    PyErr_Clear();
    Py_XDECREF(sys);
    Py_XDECREF(io);
    *sink = buf;
    return old;
}

std::string collect_stream(const char *name, PyObject *buf, PyObject *old) {
    std::string out;
    if (buf != nullptr) {
        PyObject *v = PyObject_CallMethod(buf, "getvalue", nullptr);
        if (v != nullptr) { unicode_to_std(v, out); Py_DECREF(v); }
    }
    PyObject *sys = PyImport_ImportModule("sys");
    if (sys != nullptr && old != nullptr) PyObject_SetAttrString(sys, name, old);
    PyErr_Clear();
    Py_XDECREF(sys);
    Py_XDECREF(old);
    Py_XDECREF(buf);
    return out;
}

/* run_source executes src in the persistent namespace and returns a NEW
 * reference to the value of the last expression statement (None when the
 * source does not end in one), or NULL with the Python error set.
 *
 * Fast path: the whole source is one expression (the common Go -> Python
 * shape). Otherwise the source is parsed with the ast module, a trailing
 * expression statement is split off, the rest is executed, and the split
 * expression is evaluated last — the REPL's "value of the last statement". */
PyObject *run_source(PyEmbed *e, const char *src) {
    PyObject *code = Py_CompileString(src, "<eval>", Py_eval_input);
    if (code != nullptr) {
        PyObject *r = PyEval_EvalCode(code, e->globals, e->globals);
        Py_DECREF(code);
        return r;
    }
    PyErr_Clear();

    PyObject *ast = PyImport_ImportModule("ast");
    if (ast == nullptr) return nullptr;
    PyObject *tree = PyObject_CallMethod(ast, "parse", "sss", src, "<eval>", "exec");
    if (tree == nullptr) { Py_DECREF(ast); return nullptr; } /* the real SyntaxError */

    PyObject *result = nullptr;
    PyObject *last = nullptr;
    PyObject *body = PyObject_GetAttrString(tree, "body");
    PyObject *expr_cls = PyObject_GetAttrString(ast, "Expr");
    if (body != nullptr && expr_cls != nullptr && PyList_Check(body)) {
        Py_ssize_t n = PyList_GET_SIZE(body);
        if (n > 0) {
            PyObject *item = PyList_GET_ITEM(body, n - 1); /* borrowed */
            int is_expr = PyObject_IsInstance(item, expr_cls);
            if (is_expr == 1) {
                last = PyObject_GetAttrString(item, "value");
                if (last != nullptr && PyList_SetSlice(body, n - 1, n, nullptr) < 0) {
                    Py_CLEAR(last);
                }
            }
        }
    }
    PyErr_Clear();
    Py_XDECREF(body);
    Py_XDECREF(expr_cls);

    PyObject *compile = PyDict_GetItemString(PyEval_GetBuiltins(), "compile"); /* borrowed */
    if (compile == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, "builtins.compile is missing");
        Py_XDECREF(last);
        Py_DECREF(tree);
        Py_DECREF(ast);
        return nullptr;
    }
    code = PyObject_CallFunction(compile, "Oss", tree, "<eval>", "exec");
    Py_DECREF(tree);
    if (code != nullptr) {
        PyObject *r = PyEval_EvalCode(code, e->globals, e->globals);
        Py_DECREF(code);
        if (r != nullptr) {
            Py_DECREF(r);
            if (last != nullptr) {
                PyObject *expr = PyObject_CallMethod(ast, "Expression", "O", last);
                if (expr != nullptr) {
                    PyObject *ecode = PyObject_CallFunction(compile, "Oss", expr, "<eval>", "eval");
                    Py_DECREF(expr);
                    if (ecode != nullptr) {
                        result = PyEval_EvalCode(ecode, e->globals, e->globals);
                        Py_DECREF(ecode);
                    }
                }
            } else {
                result = Py_NewRef(Py_None);
            }
        }
    }
    Py_XDECREF(last);
    Py_DECREF(ast);
    return result;
}

/* ---- Python -> Go trampoline --------------------------------------------- */

const char *const kGoFuncCapsule = "go-python.func";

/* GoFuncDef is the per-function state behind a materialised host function:
 * the PyMethodDef the builtin function object points at (it must outlive
 * the function) and the host function id. It lives in a capsule that is the
 * function's __self__, so it is freed exactly when the function is. */
struct GoFuncDef {
    PyMethodDef def;
    int32_t id;
    std::string name;
};

void gofunc_capsule_free(PyObject *cap) {
    auto *d = static_cast<GoFuncDef *>(PyCapsule_GetPointer(cap, kGoFuncCapsule));
    delete d;
}

/* go_func_call is the tp_call of every materialised host function: encode
 * the arguments, dispatch to the host over the wasmify callback import, and
 * decode the response — the return value, or an exception to raise. */
PyObject *go_func_call(PyObject *self, PyObject *args, PyObject *kwargs) {
    auto *d = static_cast<GoFuncDef *>(PyCapsule_GetPointer(self, kGoFuncCapsule));
    if (d == nullptr) return nullptr;
    if (g_go_cb == 0) {
        PyErr_SetString(PyExc_RuntimeError, "no Go dispatcher registered for this instance");
        return nullptr;
    }

    std::string req;
    encode_seq(args, req);
    encode_kwargs(kwargs, req);

    int64_t rc = wasmify_callback_invoke(g_go_cb, d->id,
                                         const_cast<char *>(req.data()), req.size());
    uint32_t resp_ptr = static_cast<uint32_t>(static_cast<uint64_t>(rc) >> 32);
    uint32_t resp_len = static_cast<uint32_t>(static_cast<uint64_t>(rc) & 0xFFFFFFFFu);
    if (resp_ptr == 0 || resp_len < 1) {
        PyErr_SetString(PyExc_RuntimeError, "Go function dispatch: empty response");
        return nullptr;
    }
    char *resp = reinterpret_cast<char *>(static_cast<uintptr_t>(resp_ptr));

    NodeReader r(resp, resp_len);
    uint8_t status = r.get_u8();
    std::string err;
    PyObject *result = nullptr;
    if (status == ENV_OK) {
        result = decode_node(r, err);
        if (result == nullptr) {
            PyErr_Format(PyExc_RuntimeError, "Go function dispatch: %s",
                         err.empty() ? "malformed response" : err.c_str());
        }
    } else {
        uint8_t has_exc = r.get_u8();
        if (has_exc) {
            PyObject *exc = decode_node(r, err);
            if (exc == nullptr) {
                PyErr_Format(PyExc_RuntimeError, "Go function dispatch: %s",
                             err.empty() ? "malformed response" : err.c_str());
            } else if (!PyExceptionInstance_Check(exc)) {
                PyErr_Format(PyExc_TypeError,
                             "Go function raised a non-exception object of type %s",
                             Py_TYPE(exc)->tp_name);
                Py_DECREF(exc);
            } else {
                PyErr_SetRaisedException(exc); /* steals */
            }
        } else {
            uint32_t elen = r.get_u32();
            const char *ep = r.get_bytes(elen);
            std::string msg = (ep != nullptr && !r.fail) ? std::string(ep, elen) : std::string();
            PyErr_SetString(PyExc_RuntimeError, msg.empty() ? "Go function failed" : msg.c_str());
        }
    }
    free(resp);
    return result;
}

} // namespace

/* ---- public API (py.h) --------------------------------------------------- */

uint64_t py_new(const char *stdlib_dir) {
    if (g_embed != nullptr) return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_embed));

    auto *e = new (std::nothrow) PyEmbed();
    if (e == nullptr) return 0;

#ifdef WASMIFY_HOST_SUBPROCESS
    /* Force subprocess onto the posix_spawn path. WASI has no fork/exec so
     * subprocess sets _can_fork_exec=False, and _use_posix_spawn() otherwise
     * defaults off; this env var (read at subprocess import time) selects the
     * posix_spawn path our bridge implements. Must be set before any
     * `import subprocess`, i.e. before the interpreter starts. */
    setenv("_PYTHON_SUBPROCESS_USE_POSIX_SPAWN", "1", 1);
#endif

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);

    /* The isolated config ignores the environment by design (the host decides
     * what the guest sees). PYTHONVERBOSE is honoured explicitly anyway: an
     * embedder that puts it in the guest environment gets the import trace on
     * stderr, which is the one way to see WHY an interpreter fails to boot
     * (Py_InitializeFromConfig only reports "failed to initialize importlib"). */
    if (const char *v = getenv("PYTHONVERBOSE"); v != nullptr && *v != '\0' && *v != '0') {
        config.verbose = 1;
    }

    /* Point the interpreter at the host-provided stdlib (the Lib/ tree). The
     * isolated config ignores PYTHONPATH, so this is the only way bootstrap
     * imports (encodings, io, codecs, ...) are found. */
    if (stdlib_dir != nullptr && stdlib_dir[0] != '\0') {
        config.module_search_paths_set = 1;
        wchar_t *w = Py_DecodeLocale(stdlib_dir, nullptr);
        if (w != nullptr) {
            PyWideStringList_Append(&config.module_search_paths, w);
            PyMem_RawFree(w);
        }
    }

    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        fprintf(stderr, "[py] Py_InitializeFromConfig failed: func=%s err_msg=%s exitcode=%d\n",
                status.func ? status.func : "(nil)",
                status.err_msg ? status.err_msg : "(nil)",
                status.exitcode);
        report_init_exception();
        delete e;
        return 0;
    }

    e->tstate = PyThreadState_Get();
    e->kbd_interrupt = PyExc_KeyboardInterrupt; /* immortal, borrowed */
    /* The persistent namespace is the real __main__ module, so code that
     * imports __main__, pickles, or inspects __name__ behaves as in a
     * script, and the host reaches the same namespace via py_import. */
    e->main = PyImport_AddModuleRef("__main__");
    if (e->main == nullptr) { PyErr_Clear(); delete e; return 0; }
    e->globals = PyModule_GetDict(e->main); /* borrowed */

    g_embed = e;
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(e));
}

std::string py_eval(uint64_t h, const char *src) {
    std::string out;
    PyEmbed *e = resolve(h);
    if (e == nullptr || src == nullptr) {
        out = env_fail(e == nullptr ? "invalid interpreter handle" : "null source");
        put_u32(out, 0);
        put_u32(out, 0);
        return out;
    }

    PyObject *o_sink = nullptr, *e_sink = nullptr;
    PyObject *o_old = redirect_stream("stdout", &o_sink);
    PyObject *e_old = redirect_stream("stderr", &e_sink);

    PyObject *result = run_source(e, src);
    if (result != nullptr) {
        put_u8(out, ENV_OK);
        encode_object(result, out);
        Py_DECREF(result);
    } else {
        append_raised(out);
    }

    std::string so = collect_stream("stdout", o_sink, o_old);
    std::string se = collect_stream("stderr", e_sink, e_old);
    put_str(out, so);
    put_str(out, se);
    return out;
}

void py_close(uint64_t h) {
    PyEmbed *e = resolve(h);
    if (e == nullptr) return;
    reg_clear();
    g_go_cb = 0;
    Py_CLEAR(e->main);
    e->globals = nullptr;
    g_embed = nullptr;
    Py_Finalize();
    delete e;
}

uint32_t py_eval_breaker_addr(uint64_t h) {
    PyEmbed *e = resolve(h);
    if (e == nullptr || e->tstate == nullptr) return 0;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&e->tstate->eval_breaker));
}

uint32_t py_async_exc_addr(uint64_t h) {
    PyEmbed *e = resolve(h);
    if (e == nullptr || e->tstate == nullptr) return 0;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&e->tstate->async_exc));
}

uint32_t py_keyboard_interrupt_obj(uint64_t h) {
    PyEmbed *e = resolve(h);
    if (e == nullptr) return 0;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->kbd_interrupt));
}

/* ---- object protocol ------------------------------------------------------ */

std::string py_import(uint64_t h, const char *name, uint32_t name_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *n = name_of(name, name_len, out);
    if (n == nullptr) return out;
    PyObject *m = PyImport_Import(n);
    Py_DECREF(n);
    return node_result(m);
}

std::string py_call(uint64_t h, uint64_t callable,
                    const char *args, uint32_t args_len,
                    const char *kwargs, uint32_t kwargs_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *fn = object_of(callable, out);
    if (fn == nullptr) return out;
    std::string err;
    PyObject *a = decode_args(args, args_len, err);
    if (a == nullptr) return env_fail(err);
    PyObject *kw = decode_kwargs(kwargs, kwargs_len, err);
    if (kw == nullptr && !err.empty()) { Py_DECREF(a); return env_fail(err); }
    PyObject *r = PyObject_Call(fn, a, kw);
    Py_DECREF(a);
    Py_XDECREF(kw);
    return node_result(r);
}

std::string py_call_method(uint64_t h, uint64_t obj,
                           const char *name, uint32_t name_len,
                           const char *args, uint32_t args_len,
                           const char *kwargs, uint32_t kwargs_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *n = name_of(name, name_len, out);
    if (n == nullptr) return out;
    PyObject *fn = PyObject_GetAttr(o, n);
    Py_DECREF(n);
    if (fn == nullptr) return node_result(nullptr);
    std::string err;
    PyObject *a = decode_args(args, args_len, err);
    if (a == nullptr) { Py_DECREF(fn); return env_fail(err); }
    PyObject *kw = decode_kwargs(kwargs, kwargs_len, err);
    if (kw == nullptr && !err.empty()) { Py_DECREF(a); Py_DECREF(fn); return env_fail(err); }
    PyObject *r = PyObject_Call(fn, a, kw);
    Py_DECREF(a);
    Py_XDECREF(kw);
    Py_DECREF(fn);
    return node_result(r);
}

std::string py_getattr(uint64_t h, uint64_t obj, const char *name, uint32_t name_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *n = name_of(name, name_len, out);
    if (n == nullptr) return out;
    PyObject *r = PyObject_GetAttr(o, n);
    Py_DECREF(n);
    return node_result(r);
}

std::string py_setattr(uint64_t h, uint64_t obj, const char *name, uint32_t name_len,
                       const char *val, uint32_t val_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *n = name_of(name, name_len, out);
    if (n == nullptr) return out;
    PyObject *v = value_of(val, val_len, out);
    if (v == nullptr) { Py_DECREF(n); return out; }
    int rc = PyObject_SetAttr(o, n, v);
    Py_DECREF(n);
    Py_DECREF(v);
    return status_result(rc);
}

std::string py_getitem(uint64_t h, uint64_t obj, const char *key, uint32_t key_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *k = value_of(key, key_len, out);
    if (k == nullptr) return out;
    PyObject *r = PyObject_GetItem(o, k);
    Py_DECREF(k);
    return node_result(r);
}

std::string py_setitem(uint64_t h, uint64_t obj, const char *key, uint32_t key_len,
                       const char *val, uint32_t val_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *k = value_of(key, key_len, out);
    if (k == nullptr) return out;
    PyObject *v = value_of(val, val_len, out);
    if (v == nullptr) { Py_DECREF(k); return out; }
    int rc = PyObject_SetItem(o, k, v);
    Py_DECREF(k);
    Py_DECREF(v);
    return status_result(rc);
}

std::string py_delitem(uint64_t h, uint64_t obj, const char *key, uint32_t key_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *k = value_of(key, key_len, out);
    if (k == nullptr) return out;
    int rc = PyObject_DelItem(o, k);
    Py_DECREF(k);
    return status_result(rc);
}

std::string py_len(uint64_t h, uint64_t obj) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    Py_ssize_t n = PyObject_Length(o);
    if (n < 0) { append_raised(out); return out; }
    put_u8(out, ENV_OK);
    put_i64(out, static_cast<int64_t>(n));
    return out;
}

std::string py_contains(uint64_t h, uint64_t obj, const char *key, uint32_t key_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *k = value_of(key, key_len, out);
    if (k == nullptr) return out;
    int rc = PySequence_Contains(o, k);
    Py_DECREF(k);
    if (rc < 0) { append_raised(out); return out; }
    put_u8(out, ENV_OK);
    put_u8(out, rc ? 1 : 0);
    return out;
}

namespace {
std::string text_result(PyObject *s) {
    std::string out;
    if (s == nullptr) { append_raised(out); return out; }
    std::string text;
    bool ok = unicode_to_std(s, text);
    Py_DECREF(s);
    if (!ok) { append_raised(out); return out; }
    put_u8(out, ENV_OK);
    put_str(out, text);
    return out;
}
} // namespace

std::string py_str(uint64_t h, uint64_t obj) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    return text_result(PyObject_Str(o));
}

std::string py_repr(uint64_t h, uint64_t obj) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    return text_result(PyObject_Repr(o));
}

std::string py_isinstance(uint64_t h, uint64_t obj, uint64_t cls) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *c = object_of(cls, out);
    if (c == nullptr) return out;
    int rc = PyObject_IsInstance(o, c);
    if (rc < 0) { append_raised(out); return out; }
    put_u8(out, ENV_OK);
    put_u8(out, rc ? 1 : 0);
    return out;
}

std::string py_iter(uint64_t h, uint64_t obj) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    return list_result(PySequence_List(o));
}

std::string py_dict_get(uint64_t h, uint64_t obj, const char *key, uint32_t key_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *k = value_of(key, key_len, out);
    if (k == nullptr) return out;
    PyObject *v = PyObject_GetItem(o, k);
    Py_DECREF(k);
    if (v == nullptr) {
        if (!PyErr_ExceptionMatches(PyExc_KeyError)) { append_raised(out); return out; }
        PyErr_Clear();
        put_u8(out, ENV_OK);
        put_u8(out, 0);
        put_u8(out, NODE_NONE);
        return out;
    }
    put_u8(out, ENV_OK);
    put_u8(out, 1);
    encode_object(v, out);
    Py_DECREF(v);
    return out;
}

std::string py_dict_keys(uint64_t h, uint64_t obj) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    return list_result(PyMapping_Keys(o));
}

std::string py_dict_values(uint64_t h, uint64_t obj) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    return list_result(PyMapping_Values(o));
}

std::string py_dict_items(uint64_t h, uint64_t obj) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *items = PyMapping_Items(o);
    if (items == nullptr) { append_raised(out); return out; }
    PyObject *fast = PySequence_Fast(items, "expected a sequence");
    Py_DECREF(items);
    if (fast == nullptr) { append_raised(out); return out; }
    Py_ssize_t n = PySequence_Fast_GET_SIZE(fast);
    put_u8(out, ENV_OK);
    put_u32(out, static_cast<uint32_t>(2 * n));
    PyObject **pairs = PySequence_Fast_ITEMS(fast);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *pair = pairs[i];
        PyObject *k = PySequence_GetItem(pair, 0);
        PyObject *v = PySequence_GetItem(pair, 1);
        PyErr_Clear();
        encode_object(k, out);
        encode_object(v, out);
        Py_XDECREF(k);
        Py_XDECREF(v);
    }
    Py_DECREF(fast);
    return out;
}

std::string py_list_append(uint64_t h, uint64_t obj, const char *elems, uint32_t elems_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    if (!PyList_Check(o)) return env_fail("object is not a list");
    std::vector<PyObject *> items;
    std::string err;
    if (!decode_items(elems, elems_len, items, err)) return env_fail(err);
    int rc = 0;
    for (PyObject *x : items) {
        if (rc == 0) rc = PyList_Append(o, x);
        Py_DECREF(x);
    }
    return status_result(rc);
}

std::string py_set_add(uint64_t h, uint64_t obj, const char *val, uint32_t val_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *v = value_of(val, val_len, out);
    if (v == nullptr) return out;
    int rc = PySet_Add(o, v);
    Py_DECREF(v);
    return status_result(rc);
}

std::string py_set_discard(uint64_t h, uint64_t obj, const char *val, uint32_t val_len) {
    std::string out;
    if (!ready(h, out)) return out;
    PyObject *o = object_of(obj, out);
    if (o == nullptr) return out;
    PyObject *v = value_of(val, val_len, out);
    if (v == nullptr) return out;
    int rc = PySet_Discard(o, v); /* 1 found, 0 absent, -1 error */
    Py_DECREF(v);
    return status_result(rc < 0 ? -1 : 0);
}

namespace {
/* build_from_items decodes a node list and hands the items (new refs) to
 * make, which builds the aggregate and consumes them. */
template <typename F>
std::string build_from_items(uint64_t h, const char *elems, uint32_t elems_len, F make) {
    std::string out;
    if (!ready(h, out)) return out;
    std::vector<PyObject *> items;
    std::string err;
    if (!decode_items(elems, elems_len, items, err)) return env_fail(err);
    return node_result(make(items));
}
} // namespace

std::string py_new_list(uint64_t h, const char *elems, uint32_t elems_len) {
    return build_from_items(h, elems, elems_len, [](std::vector<PyObject *> &items) -> PyObject * {
        PyObject *l = PyList_New(static_cast<Py_ssize_t>(items.size()));
        for (size_t i = 0; i < items.size(); i++) {
            if (l != nullptr) PyList_SET_ITEM(l, i, items[i]); /* steals */
            else Py_DECREF(items[i]);
        }
        return l;
    });
}

std::string py_new_tuple(uint64_t h, const char *elems, uint32_t elems_len) {
    return build_from_items(h, elems, elems_len, [](std::vector<PyObject *> &items) -> PyObject * {
        PyObject *t = PyTuple_New(static_cast<Py_ssize_t>(items.size()));
        for (size_t i = 0; i < items.size(); i++) {
            if (t != nullptr) PyTuple_SET_ITEM(t, i, items[i]); /* steals */
            else Py_DECREF(items[i]);
        }
        return t;
    });
}

namespace {
PyObject *fill_set(PyObject *s, std::vector<PyObject *> &items) {
    for (PyObject *x : items) {
        if (s != nullptr && PySet_Add(s, x) < 0) Py_CLEAR(s);
        Py_DECREF(x);
    }
    return s;
}
} // namespace

std::string py_new_set(uint64_t h, const char *elems, uint32_t elems_len) {
    return build_from_items(h, elems, elems_len, [](std::vector<PyObject *> &items) -> PyObject * {
        return fill_set(PySet_New(nullptr), items);
    });
}

std::string py_new_frozenset(uint64_t h, const char *elems, uint32_t elems_len) {
    return build_from_items(h, elems, elems_len, [](std::vector<PyObject *> &items) -> PyObject * {
        return fill_set(PyFrozenSet_New(nullptr), items);
    });
}

std::string py_new_dict(uint64_t h, const char *items_buf, uint32_t items_len) {
    return build_from_items(h, items_buf, items_len, [](std::vector<PyObject *> &items) -> PyObject * {
        PyObject *d = PyDict_New();
        if (d != nullptr && items.size() % 2 != 0) {
            Py_CLEAR(d);
            PyErr_SetString(PyExc_ValueError, "dict items must come in key/value pairs");
        }
        for (size_t i = 0; i + 1 < items.size(); i += 2) {
            if (d != nullptr && PyDict_SetItem(d, items[i], items[i + 1]) < 0) Py_CLEAR(d);
        }
        for (PyObject *x : items) Py_DECREF(x);
        return d;
    });
}

std::string py_new_function(uint64_t h, const char *name, uint32_t name_len, int32_t func_id) {
    std::string out;
    if (!ready(h, out)) return out;
    auto *d = new (std::nothrow) GoFuncDef();
    if (d == nullptr) return env_fail("out of memory");
    d->id = func_id;
    d->name.assign(name ? name : "", name_len);
    if (d->name.empty()) d->name = "<go function>";
    d->def.ml_name = d->name.c_str();
    d->def.ml_meth = reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(go_func_call));
    d->def.ml_flags = METH_VARARGS | METH_KEYWORDS;
    d->def.ml_doc = nullptr;
    PyObject *cap = PyCapsule_New(d, kGoFuncCapsule, gofunc_capsule_free);
    if (cap == nullptr) { delete d; return node_result(nullptr); }
    PyObject *fn = PyCFunction_NewEx(&d->def, cap, nullptr);
    Py_DECREF(cap); /* the function holds it as __self__ */
    return node_result(fn);
}

std::string py_release(uint64_t h, const char *ids, uint32_t ids_len) {
    std::string out;
    if (!ready(h, out)) return out;
    NodeReader r(ids, ids_len);
    while (r.p + 8 <= r.end) reg_release(r.get_u64());
    put_u8(out, ENV_OK);
    return out;
}

void py_set_go_dispatcher(uint64_t h, int32_t callback_id) {
    if (h == 0) return;
    g_go_cb = callback_id;
}
