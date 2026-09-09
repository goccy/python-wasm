# CPython source patches (host capabilities)

These patches adapt the vendored CPython (the `cpython/` submodule) to the
host capabilities wasmify provides on wasm32-wasi. They are grouped by the
capability that needs them and applied automatically by
`scripts/wasi-configure.sh` when `wasmify.json` opts that capability in
(`bridge.HostSubprocess: true` for `host-subprocess/`, `bridge.HostSockets:
true` for `host-sockets/`); the apply is idempotent (skipped if already
applied).

They live here, rather than as uncommitted edits in the submodule, so a fresh
checkout / `git submodule update` / CPython version bump reproduces them.

## `host-subprocess/`

| patch | what / why |
|---|---|
| `0001-posixmodule-posix_spawn-helpers.patch` | `posix_spawn`'s argv/env helpers (`parse_arglist`, `parse_envlist`, `free_string_array`) are guarded by `HAVE_EXECV`/`HAVE_SPAWNV`, which are off on wasi. Add `HAVE_POSIX_SPAWN` to those guards so enabling `os.posix_spawn` compiles. |
| `0002-subprocess-wasi-posix_spawn.patch` | `subprocess` hard-refuses on `sys.platform == "wasi"` (`_can_fork_exec` is False) before reaching the `posix_spawn` path. Allow the posix_spawn path, populate `_del_safe` from the real `os` functions when `waitpid` exists, and skip the `setsigdef` step (no `sigset_t` support on this build). |

The actual process spawning is provided by wasmify's host-subprocess shim
(compiled with `-DWASMIFY_HOST_SUBPROCESS`) backed by the
`proc_spawn`/`proc_wait` host imports. See `scripts/wasi-configure.sh` for the
matching pyconfig.h flags.

## `host-sockets/`

| patch | what / why |
|---|---|
| `0001-socketmodule-getnameinfo-without-gethostbyname.patch` | `socket.getnameinfo` (compiled whenever `HAVE_GETNAMEINFO` is on) calls the `sock_decode_hostname` helper, which upstream only compiles alongside the `gethostbyname`/`gethostbyaddr` family. wasmify's socket shim provides `getnameinfo()` but none of `gethostby*`, so make the helper available under `HAVE_GETNAMEINFO` too. |

The socket functions themselves come from wasmify's host-sockets shim
(compiled with `-DWASMIFY_HOST_SOCKETS`) together with its `<netdb.h>` stub;
`scripts/wasi-configure.sh` routes `socketmodule` through that header
(`HAVE_NETDB_H`, `HAVE_ADDRINFO`, `HAVE_GETNAMEINFO`) so both sides share one
`struct addrinfo` layout.

Regenerate after editing the submodule sources with:

    git -C cpython diff -- Modules/posixmodule.c > patches/host-subprocess/0001-posixmodule-posix_spawn-helpers.patch
    git -C cpython diff -- Lib/subprocess.py     > patches/host-subprocess/0002-subprocess-wasi-posix_spawn.patch
    git -C cpython diff -- Modules/socketmodule.c > patches/host-sockets/0001-socketmodule-getnameinfo-without-gethostbyname.patch
