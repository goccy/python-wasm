#!/bin/bash
# wasi-configure.sh — configure CPython for wasm32-wasi using BY-NAME compilers
# so that wasmify's PATH-based compiler wrappers intercept every clang/clang++
# invocation that `make` issues.
#
# Mirrors cpython/Tools/wasm/wasi/__main__.py::configure_wasi_python, but
# substitutes the full wasi-sdk clang paths with the bare names `clang` /
# `clang++` and prepends $WASI_SDK_PATH/bin to PATH. Under `wasmify build`,
# CC/CXX are further overridden to the wrapper dir; the wrapper then resolves
# WASMIFY_REAL_clang (= $WASI_SDK_PATH/bin/clang, found on PATH) and execs it.
#
# Env in:
#   WASI_SDK_PATH   path to wasi-sdk (clang 22)
# Runs from the project root (dir containing cpython/).
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
CPY="$HERE/cpython"
: "${WASI_SDK_PATH:=$HOME/.config/wasmify/bin/wasi-sdk}"
SYSROOT="$WASI_SDK_PATH/share/wasi-sysroot"
HOST_TRIPLE="wasm32-wasip1"
# Must match the directory CPython's wasi tool builds build-python into. That
# tool (Tools/wasm/wasi) names it by the BUILD_GNU_TYPE sysconfig variable of
# the python3 that runs it (e.g. x86_64-pc-linux-gnu) — NOT `cc -dumpmachine`,
# which spells it differently on Linux (x86_64-linux-gnu). Ask the same python3
# for the same variable so the two never disagree (a shell running under
# Rosetta on Apple Silicon, for instance, makes config.guess answer x86_64
# while an arm64 python3 builds into aarch64-apple-darwin*); fall back to
# config.guess, then dumpmachine.
BUILD_TRIPLE="$(python3 -c 'import sysconfig; print(sysconfig.get_config_var("BUILD_GNU_TYPE") or "")' 2>/dev/null || true)"
if [ -z "$BUILD_TRIPLE" ]; then
  if [ -x "$CPY/config.guess" ] || [ -f "$CPY/config.guess" ]; then
    BUILD_TRIPLE="$(sh "$CPY/config.guess")"
  else
    BUILD_TRIPLE="$(cc -dumpmachine)"
  fi
fi

CROSS="$CPY/cross-build"
BUILD_DIR="$CROSS/$BUILD_TRIPLE"
WASI_DIR="$CROSS/$HOST_TRIPLE"

echo "== build triple: $BUILD_TRIPLE"
echo "== wasi sdk:     $WASI_SDK_PATH"

# ---------------------------------------------------------------------------
# Host-capability CPython source patches (idempotent).
#
# Enabling a host capability sometimes needs a small CPython source change
# that can't be expressed as config. They live as patches/<capability>/ here
# (rather than uncommitted edits in the cpython submodule) so a fresh checkout
# / submodule reset reproduces them, and each directory is applied only when
# its capability is opted in in wasmify.json; a patch already present is
# skipped.
#
#   patches/host-subprocess/  posixmodule.c's posix_spawn helper functions are
#                             guarded by HAVE_EXECV (off on wasi), and
#                             subprocess.py hard-refuses to run on the "wasi"
#                             platform before reaching the posix_spawn path.
#   patches/host-sockets/     socketmodule.c's socket.getnameinfo needs a
#                             helper that upstream only compiles alongside
#                             gethostbyname; wasmify's shim provides
#                             getnameinfo without gethostbyname.
# ---------------------------------------------------------------------------
apply_patches() {
  local dir="$1"
  for patch in "$dir"/*.patch; do
    [ -f "$patch" ] || continue
    if git -C "$CPY" apply --reverse --check "$patch" 2>/dev/null; then
      echo "== cpython patch already applied: $(basename "$patch")"
    elif git -C "$CPY" apply --check "$patch" 2>/dev/null; then
      git -C "$CPY" apply "$patch" && echo "== applied cpython patch: $(basename "$patch")"
    else
      echo "!! WARNING: could not apply cpython patch: $(basename "$patch")" >&2
    fi
  done
}
if [ -f "$HERE/wasmify.json" ] && grep -qE '"HostSubprocess"[[:space:]]*:[[:space:]]*true' "$HERE/wasmify.json"; then
  apply_patches "$HERE/patches/host-subprocess"
fi
if [ -f "$HERE/wasmify.json" ] && grep -qE '"HostSockets"[[:space:]]*:[[:space:]]*true' "$HERE/wasmify.json"; then
  apply_patches "$HERE/patches/host-sockets"
fi

# ---------------------------------------------------------------------------
# 1. Host build-python (needed by cross compile; uses native gcc, not wasi).
#    Reuse if already present.
# ---------------------------------------------------------------------------
pick_build_py() {
  if [ -f "$BUILD_DIR/python" ] && [ -x "$BUILD_DIR/python" ]; then
    echo "$BUILD_DIR/python"
  elif [ -f "$BUILD_DIR/python.exe" ] && [ -x "$BUILD_DIR/python.exe" ]; then
    echo "$BUILD_DIR/python.exe"
  fi
}
BUILD_PY="$(pick_build_py)"
if [ -z "$BUILD_PY" ]; then
  echo "== building host build-python"
  python3 "$CPY/Tools/wasm/wasi" build-python
  BUILD_PY="$(pick_build_py)"
fi
echo "== build-python: $BUILD_PY"

# Generated marker that Tools/wasm/wasi writes for static extension modules.
# NOTE: we deliberately keep _socket ENABLED — network access is controlled at
# the WASI host layer via a whitelist, not by removing the module. socketmodule
# compiles cleanly against the bare wasi sysroot (its cmsg code is gated behind
# CMSG_* macros that the bare sysroot leaves undefined); it only breaks when
# wasmify's posix-compat stubs define those macros, so the wasm build runs with
# WASMIFY_NO_POSIX_COMPAT=1.
printf '# Generated by python-wasm wasi-configure.\n# Required to statically build extension modules.' \
  > "$CPY/Modules/Setup.local"

# ---------------------------------------------------------------------------
# 2. Locate the host python's lib.* dir to derive the sysconfig data path.
# ---------------------------------------------------------------------------
LIB_DIR="$(ls -d "$BUILD_DIR"/build/lib.* | head -1)"
PYVER="${LIB_DIR##*-}"
SYSCONFIG_DATA="$HOST_TRIPLE relative"  # placeholder; built below
WASI_REL="cross-build/$HOST_TRIPLE"
SYSCONFIG_DATA_DIR="$WASI_REL/build/lib.wasi-wasm32-$PYVER"

# HOSTRUNNER is only invoked by CPython's `test` targets (running python.wasm
# under wasmtime); `make all` does its freezing/sysconfig with the host
# build-python, so a wasm-build does NOT need wasmtime. Don't hard-require it
# here (the toolchain image ships without it) — fall back to the bare name so
# the config string is well-formed; it is simply never executed during the build.
WASMTIME="$(command -v wasmtime || echo wasmtime)"
HOSTRUNNER="$WASMTIME run --wasm max-wasm-stack=16777216 --dir $CPY::/ --env PYTHONPATH=/$SYSCONFIG_DATA_DIR"

# ---------------------------------------------------------------------------
# 3. Configure the wasi build with BY-NAME compilers.
# ---------------------------------------------------------------------------
mkdir -p "$WASI_DIR"
cd "$WASI_DIR"

# Put wasi-sdk/bin first so the bare names resolve to wasi clang when wasmify's
# wrapper execs WASMIFY_REAL_clang (looked up on PATH).
export PATH="$WASI_SDK_PATH/bin:$PATH"
export CONFIG_SITE="$CPY/Tools/wasm/wasi/config.site-wasm32-wasi"
export HOSTRUNNER
export PKG_CONFIG_PATH=""
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export WASI_SYSROOT="$SYSROOT"

# BY-NAME compilers AND archiver — this is the whole point. Using the bare
# names `clang`/`clang++`/`ar`/`ranlib` (with $WASI_SDK_PATH/bin on PATH so
# they resolve to the wasi-sdk tools) means wasmify's PATH-based wrappers
# intercept EVERY tool `make` invokes, including the archive step. NOTE: it
# must be `AR=ar`, not `AR=llvm-ar` — wasmify only wraps the canonical name
# `ar`; with `llvm-ar` the archive step bypasses the wrapper, build.json gets
# no archive step, and the final `wasm-build` link finds no `.a` to link.
# wasi-sdk's `ar` IS llvm-ar, so the resolved tool is identical.
CONFIGURE="$(python3 -c "import os.path;print(os.path.relpath('$CPY/configure','$WASI_DIR'))")"

# Host-subprocess needs <spawn.h>/<sys/wait.h>, which wasi-libc omits. We do
# NOT provide them to this configure/make (the host-arch-equivalent baseline
# build): the pyconfig.h patch below gates HAVE_POSIX_SPAWN etc. behind
# WASMIFY_HOST_SUBPROCESS, a macro wasmify defines ONLY at wasm-build, where it
# also supplies the stub headers via -I. So the baseline build stays plain
# CPython and the wasm-build phase alone enables host subprocess.
echo "== configuring wasi (by-name CC=clang CXX=clang++ AR=ar)"
"$CONFIGURE" \
  --host="$HOST_TRIPLE" \
  --build="$BUILD_TRIPLE" \
  --with-build-python="$BUILD_PY" \
  CC=clang \
  CXX=clang++ \
  CPP="clang-cpp" \
  AR=ar \
  RANLIB=ranlib

# Enable host-provided outbound sockets in the generated pyconfig.h.
#
# Gated on the SAME opt-in as the bridge socket shim: wasmify.json's
# bridge.HostSockets. When false (or absent), we leave HAVE_SOCKET etc. off so
# socketmodule keeps CPython's ENOTSUP stubs and the wasm imports only standard
# wasi (portable). When true, wasmify's host-sockets shim (compiled with
# -DWASMIFY_HOST_SOCKETS) supplies socket()/connect()/getaddrinfo() backed by
# host imports, so we flip the macros on and route socketmodule through the
# same <netdb.h> the shim uses (see the block below for the two shapes).
# Keeping the two in sync avoids a link error (socketmodule referencing
# socket() with no shim providing it). Idempotent: re-running configure
# re-patches.
PYCONFIG="$WASI_DIR/pyconfig.h"
HOST_SOCKETS_OPTIN=0
if [ -f "$HERE/wasmify.json" ] && grep -qE '"HostSockets"[[:space:]]*:[[:space:]]*true' "$HERE/wasmify.json"; then
  HOST_SOCKETS_OPTIN=1
fi
if [ "$HOST_SOCKETS_OPTIN" = "1" ] && [ -f "$PYCONFIG" ] && ! grep -q PYWASM_HOST_SOCKET_DECLS "$PYCONFIG"; then
  echo "== patching pyconfig.h for host-provided sockets"
  perl -0pi -e 's{/\* \#undef HAVE_SOCKET \*/}{#define HAVE_SOCKET 1}' "$PYCONFIG"
  perl -0pi -e 's{/\* \#undef HAVE_CONNECT \*/}{#define HAVE_CONNECT 1\n#define HAVE_CONNECTTO 1}' "$PYCONFIG"
  perl -0pi -e 's{/\* \#undef HAVE_GETADDRINFO \*/}{#define HAVE_GETADDRINFO 1}' "$PYCONFIG"
  cat >> "$PYCONFIG" <<'PYCONF_EOF'

/* python-wasm: host-provided outbound socket API (definitions in wasmify's
 * host-sockets shim, calling host imports backed by Go's net package).
 * wasi-libc omits these under __wasip1__.
 *
 * Two shapes, selected by WASMIFY_HOST_SOCKETS — the macro wasmify defines
 * for every compile it drives when bridge.HostSockets is on (the captured
 * `wasmify build` and the wasm-build replay alike), together with its
 * <netdb.h> stub on the include path:
 *
 *   with the macro: that <netdb.h> is the layout contract wasmify's socket
 *   shim fills getaddrinfo() results into. Route socketmodule through the
 *   same header: HAVE_NETDB_H makes socketmodule.c #include <netdb.h>,
 *   HAVE_ADDRINFO makes Modules/addrinfo.h skip its own (RFC 2553-ordered)
 *   struct addrinfo, and the AI_/EAI_ values and the getaddrinfo family
 *   prototypes come from the shared header, so the module and the shim agree
 *   on struct addrinfo by construction. HAVE_GETNAMEINFO selects the shim's
 *   getnameinfo() over CPython's fallback Modules/getnameinfo.c, which
 *   assumes a full netdb (servent/getservbyport) once netdb.h exists (the
 *   host-sockets patch lets socket.getnameinfo build without gethostbyname).
 *   Only socket()/connect() — which the sysroot's <sys/socket.h> guards out
 *   — are declared here.
 *
 *   without the macro (a compile wasmify does not drive): no netdb.h exists
 *   anywhere, so declare the standalone constants and prototypes that let
 *   socketmodule compile with HAVE_GETADDRINFO against the bare sysroot;
 *   struct addrinfo comes from Modules/addrinfo.h. */
#ifndef PYWASM_HOST_SOCKET_DECLS
#define PYWASM_HOST_SOCKET_DECLS
#ifndef SO_ERROR
#define SO_ERROR 4
#endif
#ifdef WASMIFY_HOST_SOCKETS
#ifndef HAVE_NETDB_H
#define HAVE_NETDB_H 1
#endif
#ifndef HAVE_ADDRINFO
#define HAVE_ADDRINFO 1
#endif
#ifndef HAVE_GETNAMEINFO
#define HAVE_GETNAMEINFO 1
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct sockaddr;
int socket(int, int, int);
int connect(int, const struct sockaddr *, unsigned int);
#ifdef __cplusplus
}
#endif
#else /* !WASMIFY_HOST_SOCKETS */
#ifndef EAI_NONAME
#define EAI_ADDRFAMILY 1
#define EAI_AGAIN 2
#define EAI_BADFLAGS 3
#define EAI_FAIL 4
#define EAI_FAMILY 5
#define EAI_MEMORY 6
#define EAI_NODATA 7
#define EAI_NONAME 8
#define EAI_SERVICE 9
#define EAI_SOCKTYPE 10
#define EAI_SYSTEM 11
#define EAI_BADHINTS 12
#define EAI_PROTOCOL 13
#define EAI_MAX 14
#endif
#ifndef AI_PASSIVE
#define AI_PASSIVE 0x00000001
#define AI_CANONNAME 0x00000002
#define AI_NUMERICHOST 0x00000004
#define AI_NUMERICSERV 0x00000008
#define AI_MASK (AI_PASSIVE|AI_CANONNAME|AI_NUMERICHOST|AI_NUMERICSERV)
#define AI_ALL 0x00000100
#define AI_V4MAPPED_CFG 0x00000200
#define AI_ADDRCONFIG 0x00000400
#define AI_V4MAPPED 0x00000800
#define AI_DEFAULT (AI_V4MAPPED_CFG|AI_ADDRCONFIG)
#endif
#ifdef __cplusplus
extern "C" {
#endif
struct sockaddr;
struct addrinfo;
int socket(int, int, int);
int connect(int, const struct sockaddr *, unsigned int);
int getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
const char *gai_strerror(int);
int getnameinfo(const struct sockaddr *, unsigned int, char *, unsigned int, char *, unsigned int, int);
#ifdef __cplusplus
}
#endif
#endif /* WASMIFY_HOST_SOCKETS */
#endif /* PYWASM_HOST_SOCKET_DECLS */
PYCONF_EOF
fi

# Enable host-provided subprocess in the generated pyconfig.h — but ONLY at
# wasm-build, never in this baseline build.
#
# The whole block is wrapped in `#ifdef WASMIFY_HOST_SUBPROCESS`, a macro
# wasmify defines on every wasm-build compile (and nowhere else) when
# wasmify.json's bridge.HostSubprocess is true. The pyconfig.h file is shared by
# both the baseline `make` (which never sees the macro -> stays plain CPython,
# so posixmodule.c #includes neither spawn.h nor sys/wait.h) and the wasm-build
# replay (macro on -> HAVE_POSIX_SPAWN/HAVE_SYS_WAIT_H enabled, with spawn.h /
# sys/wait.h supplied by wasmify via -I and the symbols defined by wasmify's
# host-subprocess shim). This keeps the baseline build free of headers wasi-libc
# lacks, so nothing needs to touch the shared wasi-sdk sysroot.
#
# ADDCLOSEFROM_NP makes subprocess's _HAVE_POSIX_SPAWN_CLOSEFROM true so default
# close_fds keeps the posix_spawn path. Idempotent (guarded by the sentinel).
if [ -f "$PYCONFIG" ] && ! grep -q PYWASM_HOST_SUBPROCESS_DECLS "$PYCONFIG"; then
  echo "== patching pyconfig.h for host-provided subprocess (wasm-build only)"
  cat >> "$PYCONFIG" <<'PYCONF_EOF'

/* python-wasm: host-provided subprocess, active only when wasmify defines
 * WASMIFY_HOST_SUBPROCESS (wasm-build compiles). spawn.h / sys/wait.h and the
 * posix_spawn/waitpid symbols are supplied by wasmify (stub headers via -I +
 * the host-subprocess shim). The baseline make build never sees the macro and
 * stays plain CPython. */
#ifndef PYWASM_HOST_SUBPROCESS_DECLS
#define PYWASM_HOST_SUBPROCESS_DECLS
#ifdef WASMIFY_HOST_SUBPROCESS
#ifndef HAVE_POSIX_SPAWN
#define HAVE_POSIX_SPAWN 1
#endif
#ifndef HAVE_POSIX_SPAWNP
#define HAVE_POSIX_SPAWNP 1
#endif
#ifndef HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP
#define HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCLOSEFROM_NP 1
#endif
#ifndef HAVE_SYS_WAIT_H
#define HAVE_SYS_WAIT_H 1
#endif
#ifndef HAVE_PIPE
#define HAVE_PIPE 1
#endif
#ifndef HAVE_WAITPID
#define HAVE_WAITPID 1
#endif
/* wasi-libc gates pipe()/pipe2() behind __wasilibc_unmodified_upstream, so the
 * prototypes are invisible even though the host shim defines the symbols.
 * Declare them here so posixmodule.c compiles against HAVE_PIPE. */
#ifdef __cplusplus
extern "C" {
#endif
int pipe(int [2]);
int pipe2(int [2], int);
#ifdef __cplusplus
}
#endif
#endif /* WASMIFY_HOST_SUBPROCESS */
#endif /* PYWASM_HOST_SUBPROCESS_DECLS */
PYCONF_EOF
fi

# Create python.sh helper like the upstream script does.
cat > "$WASI_DIR/python.sh" <<EOF
#!/bin/sh
exec $HOSTRUNNER $WASI_DIR/python.wasm "\$@"
EOF
chmod 755 "$WASI_DIR/python.sh"

echo "== configure done; CC line in Makefile:"
grep -m1 '^CC=' "$WASI_DIR/Makefile"

# ---------------------------------------------------------------------------
# 4. Start the captured build from a clean object tree.
#
# `wasmify build` learns the compile and archive steps by wrapping the
# compiler while `make` runs, and its log holds ONLY what make executed this
# time. An incremental make over objects left by an earlier run therefore
# yields an incomplete capture: the objects make considered up to date never
# reach build.json, the archives wasmify rebuilds lack them, and the link
# silently turns their symbols into host imports (observed: a warm tree
# dropped the _hacl hash objects, and hashlib then called into stubs).
# Clean the wasm build dir here so the capture that follows is always the
# whole build. The host build-python (a different directory) is untouched.
# ---------------------------------------------------------------------------
echo "== cleaning the wasm build dir so the capture covers every step"
make -C "$WASI_DIR" clean >/dev/null
