/* py.cc — implementation of the thin CPython embedding API (C++).
 * Pinned against CPython v3.14.6. See py.h for the contract. */

#include "py.h"

#include <Python.h>

#include <cstdio>
#include <cstdlib>

/* The host-socket and host-subprocess libc shims (socket()/connect()/
 * getaddrinfo() and the posix_spawn family/waitpid/pipe) are provided by
 * wasmify: it deploys and links them when bridge.HostSockets /
 * bridge.HostSubprocess is set in wasmify.json. They used to live here. */

/* From Include/internal/pycore_ceval.h (v3.14.6):
 *     #define _PY_ASYNC_EXCEPTION_BIT (1U << 3)
 * eval_breaker / async_exc are public PyThreadState fields
 * (Include/cpython/pystate.h), so no internal headers are needed here. */
#define PYEMBED_ASYNC_EXCEPTION_BIT (1u << 3)

namespace {

struct PyEmbed {
    PyObject *globals = nullptr;       /* persistent module globals (REPL)   */
    PyObject *kbd_interrupt = nullptr; /* borrowed: PyExc_KeyboardInterrupt  */
    PyThreadState *tstate = nullptr;   /* main thread state for this runtime */
};

/* One CPython runtime per wasm instance. */
PyEmbed *g_embed = nullptr;

PyEmbed *resolve(uint64_t h) {
    auto *e = reinterpret_cast<PyEmbed *>(static_cast<uintptr_t>(h));
    return (e != nullptr && e == g_embed) ? e : nullptr;
}

std::string str_to_std(PyObject *s) {
    if (s == nullptr) return std::string();
    Py_ssize_t n = 0;
    const char *u = PyUnicode_AsUTF8AndSize(s, &n);
    if (u == nullptr) { PyErr_Clear(); return std::string(); }
    return std::string(u, static_cast<size_t>(n));
}

/* Format the current exception (PyErr is set) via traceback.format_exception.
 * Clears the error. */
std::string format_exception() {
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb);
    if (type == nullptr) return std::string();
    PyErr_NormalizeException(&type, &value, &tb);
    if (tb != nullptr) PyException_SetTraceback(value, tb);

    std::string result;
    PyObject *mod = PyImport_ImportModule("traceback");
    if (mod != nullptr) {
        PyObject *lines = PyObject_CallMethod(
            mod, "format_exception", "OOO",
            type, value ? value : Py_None, tb ? tb : Py_None);
        if (lines != nullptr) {
            PyObject *empty = PyUnicode_FromString("");
            PyObject *joined = empty ? PyUnicode_Join(empty, lines) : nullptr;
            if (joined != nullptr) { result = str_to_std(joined); Py_DECREF(joined); }
            Py_XDECREF(empty);
            Py_DECREF(lines);
        }
        Py_DECREF(mod);
    }
    if (result.empty() && value != nullptr) {
        PyObject *r = PyObject_Repr(value);
        if (r != nullptr) { result = str_to_std(r); Py_DECREF(r); }
    }
    PyErr_Clear();
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(tb);
    return result;
}

PyObject *redirect_stream(const char *name, PyObject **sink) {
    PyObject *sys = PyImport_ImportModule("sys");
    PyObject *io = PyImport_ImportModule("io");
    PyObject *old = nullptr, *buf = nullptr;
    if (sys != nullptr && io != nullptr) {
        old = PyObject_GetAttrString(sys, name);
        buf = PyObject_CallMethod(io, "StringIO", nullptr);
        if (buf != nullptr) PyObject_SetAttrString(sys, name, buf);
    }
    Py_XDECREF(sys);
    Py_XDECREF(io);
    *sink = buf;
    return old;
}

void json_escape(const std::string &in, std::string &out) {
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

std::string json_field(const char *key, const std::string &val, bool comma) {
    std::string s = "\"";
    s += key;
    s += "\":\"";
    json_escape(val, s);
    s += "\"";
    if (comma) s += ",";
    return s;
}

std::string collect_stream(const char *name, PyObject *buf, PyObject *old) {
    std::string out;
    if (buf != nullptr) {
        PyObject *v = PyObject_CallMethod(buf, "getvalue", nullptr);
        if (v != nullptr) { out = str_to_std(v); Py_DECREF(v); }
    }
    PyObject *sys = PyImport_ImportModule("sys");
    if (sys != nullptr && old != nullptr) PyObject_SetAttrString(sys, name, old);
    Py_XDECREF(sys);
    Py_XDECREF(old);
    Py_XDECREF(buf);
    return out;
}

} // namespace

/* ---- public API --------------------------------------------------------- */

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
        delete e;
        return 0;
    }

    e->tstate = PyThreadState_Get();
    e->kbd_interrupt = PyExc_KeyboardInterrupt; /* immortal, borrowed */
    e->globals = PyDict_New();
    if (e->globals == nullptr) { delete e; return 0; }
    PyDict_SetItemString(e->globals, "__builtins__", PyEval_GetBuiltins());

    g_embed = e;
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(e));
}

std::string py_eval(uint64_t h, const char *src) {
    std::string repr, out, err, errbuf;
    bool ok = true;

    PyEmbed *e = resolve(h);
    if (e == nullptr || src == nullptr) {
        return "{\"ok\":false,\"repr\":\"\",\"stdout\":\"\",\"stderr\":\"\","
               "\"error\":\"invalid interpreter handle\"}";
    }

    bool is_expr = true;
    PyObject *code = Py_CompileString(src, "<eval>", Py_eval_input);
    if (code == nullptr) {
        PyErr_Clear();
        is_expr = false;
        code = Py_CompileString(src, "<exec>", Py_file_input);
    }
    if (code == nullptr) {
        errbuf = format_exception();
        ok = false;
    } else {
        PyObject *o_sink = nullptr, *e_sink = nullptr;
        PyObject *o_old = redirect_stream("stdout", &o_sink);
        PyObject *e_old = redirect_stream("stderr", &e_sink);

        PyObject *result = PyEval_EvalCode(code, e->globals, e->globals);
        Py_DECREF(code);

        if (result == nullptr) {
            errbuf = format_exception();
            ok = false;
        } else {
            if (is_expr && result != Py_None) {
                PyObject *r = PyObject_Repr(result);
                if (r != nullptr) { repr = str_to_std(r); Py_DECREF(r); }
            }
            Py_DECREF(result);
        }
        out = collect_stream("stdout", o_sink, o_old);
        err = collect_stream("stderr", e_sink, e_old);
    }

    std::string js = "{\"ok\":";
    js += ok ? "true," : "false,";
    js += json_field("repr", repr, true);
    js += json_field("stdout", out, true);
    js += json_field("stderr", err, true);
    js += json_field("error", errbuf, false);
    js += "}";
    return js;
}

void py_close(uint64_t h) {
    PyEmbed *e = resolve(h);
    if (e == nullptr) return;
    Py_XDECREF(e->globals);
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
