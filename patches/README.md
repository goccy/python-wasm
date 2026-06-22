# CPython source patches (host subprocess)

These patches adapt the vendored CPython (the `cpython/` submodule) for
host-provided subprocess on wasm32-wasi. They are applied automatically by
`scripts/wasi-configure.sh` when `wasmify.json` has `bridge.HostSubprocess:
true`, and the apply is idempotent (skipped if already applied).

They live here, rather than as uncommitted edits in the submodule, so a fresh
checkout / `git submodule update` / CPython version bump reproduces them.

| patch | what / why |
|---|---|
| `0001-posixmodule-posix_spawn-helpers.patch` | `posix_spawn`'s argv/env helpers (`parse_arglist`, `parse_envlist`, `free_string_array`) are guarded by `HAVE_EXECV`/`HAVE_SPAWNV`, which are off on wasi. Add `HAVE_POSIX_SPAWN` to those guards so enabling `os.posix_spawn` compiles. |
| `0002-subprocess-wasi-posix_spawn.patch` | `subprocess` hard-refuses on `sys.platform == "wasi"` (`_can_fork_exec` is False) before reaching the `posix_spawn` path. Allow the posix_spawn path, populate `_del_safe` from the real `os` functions when `waitpid` exists, and skip the `setsigdef` step (no `sigset_t` support on this build). |

The actual process spawning is provided by the bridge shim
(`embed/pyembed.cc`, compiled with `-DWASMIFY_HOST_SUBPROCESS`) backed by the
`proc_spawn`/`proc_wait` host imports. See `scripts/wasi-configure.sh` for the
matching pyconfig.h flags.

Regenerate after editing the submodule sources with:

    git -C cpython diff -- Modules/posixmodule.c > patches/0001-posixmodule-posix_spawn-helpers.patch
    git -C cpython diff -- Lib/subprocess.py     > patches/0002-subprocess-wasi-posix_spawn.patch
