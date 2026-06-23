# python-wasm

[wasmify](https://github.com/goccy/wasmify) project that compiles
[CPython](https://github.com/python/cpython) to `wasm32-wasip1` and emits the
pure-Go (wasm2go) bindings [go-python](https://github.com/goccy/go-python)
consumes.

## Outputs

The CI workflow at `.github/workflows/build.yml` and the local `make wasm`
target both produce, from a clean checkout:

| File | Where | What |
| --- | --- | --- |
| `python.wasm` | `.wasmify/wasm-build/output/python.wasm` | The wasi-sdk-built CPython interpreter (thin embedding API in `py.cc`), optimised by binaryen wasm-opt. |
| `python_wasm2go.tar.gz` | `build/wasm2go/internal/wasm2go/` | The transpiled pure-Go bundle, a self-contained Go module (`github.com/goccy/pythonwasm2go`). go-python downloads and extracts it. |
| `python_wasm2go.go` | `build/wasm2go/python.go` | `protoc-gen-wasmify-go`'s wasm2go bridge. |

Everything above is **regenerated** on every build; only the inputs below are
committed.

## Committed inputs

```
wasmify.json            # wasmify decisions: project metadata, build_commands, bridge config
cpython/                # git submodule pinned to the upstream commit we build against
buf.yaml                # buf module
buf.gen.yaml            # protoc-gen-wasmify-go invocation (wasm2go bundle)
proto/wasmify/          # wasmify proto options the generated proto imports
py.cc, py.h             # the thin C++ embedding API (py_new / py_eval / ...) exported to the bridge
patches/                # CPython source patches (host-provided posix_spawn)
scripts/wasi-configure.sh   # the CPython-specific configure phase (host build-python + wasm cross-configure + pyconfig patches)
```

Everything else (`build.json`, `api-spec.json`, `proto/python.proto`,
`bridge/`, `build/`, `python.wasm`) is in `.gitignore` and reproduced by
`make wasm`.

## Building locally

The full pipeline runs inside a wasmify image bundling wasi-sdk, binaryen, buf,
and the wasmify CLI — you install none of those on your host:

```sh
make wasm                              # uses ghcr.io/goccy/wasmify:v0.4.3
make wasm DOCKER_PLATFORM=linux/amd64  # on Apple Silicon (the image is amd64-only)
make wasm-clean                        # drop regenerated outputs, keep committed inputs
```

`make wasm` runs (under `docker run --rm -v $PWD:/work …`) the same sequence as
CI. Unlike a bazel project, CPython needs a configure phase first:

```
make tools                  # ensure wasi-sdk (pre-baked in the image)
scripts/wasi-configure.sh   # host build-python + wasm cross-configure + pyconfig patches
wasmify build               # replay the captured `make` (CPython wasi cross-build)
wasmify generate-build      # build.log -> build.json
wasmify parse-headers       # py.h -> api-spec.json
wasmify gen-proto           # api-spec.json -> proto/ + bridge/
wasmify wasm-build \
    --with-bridge --optimize    # build.json + bridge -> python.wasm + wasm-opt
buf generate                # proto/ -> build/wasm2go/ (the wasm2go bundle)
make bundle-gomod           # stamp the bundle's go.mod
```

## CI

`.github/workflows/build.yml` runs the same pipeline inside the wasmify image
on `push` to `main`, `v*` tags, and `workflow_dispatch`. It uploads the
artefacts, attaches `actions/attest-build-provenance` SLSA attestations, and on
a version tag publishes `python.wasm`, `python_wasm2go.go`,
`python_wasm2go.tar.gz`, and `python_wasm2go.sha256` as Release assets.
