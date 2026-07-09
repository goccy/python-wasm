# Container image shipping every toolchain wasmify needs (wasi-sdk + binaryen +
# buf + the wasmify CLI itself). Override locally to pin a SHA or to iterate on a
# wasmify branch — e.g. `make wasm IMAGE=localhost:5001/wasmify:local`.
IMAGE ?= ghcr.io/goccy/wasmify:v0.4.7

# Resource limits for the container that runs the pipeline. A full CPython wasm
# build peaks well under MEMORY; CPUS bounds make's parallelism.
MEMORY ?= 14g
CPUS   ?= 8

# The wasmify image is published linux/amd64 only. On an arm64 host (Apple
# Silicon) set DOCKER_PLATFORM=linux/amd64 to run it under emulation:
#   make wasm DOCKER_PLATFORM=linux/amd64
DOCKER_PLATFORM ?=
PLATFORM_FLAG := $(if $(DOCKER_PLATFORM),--platform=$(DOCKER_PLATFORM),)

# Bundle module path / go directive stamped into the wasm2go bundle's go.mod.
# The bundle is released as a self-contained Go module, so it needs a go.mod
# declaring the import path its own `import` / `//go:linkname` sites embed.
# wasmify writes that path into wasmify.json's bridge.Wasm2GoImportPath, which
# the codegen reads back; derive it from the JSON so the two never drift.
WASM2GO_BUNDLE_DIR    := build/wasm2go/internal/wasm2go
WASM2GO_BUNDLE_GO_VER := 1.25.0

# CPython-specific build env, exported into every pipeline phase:
#   WASMIFY_NO_EMSCRIPTEN_DEFINE — CPython has real __EMSCRIPTEN__ branches
#     (with EM_JS); the implicit -D__EMSCRIPTEN__ would mis-route them.
#   WASMIFY_NO_POSIX_COMPAT — the full posix-compat overlay's sys/socket.h
#     breaks socketmodule; CPython is wasi-native and compiles against the bare
#     sysroot plus the host-socket pyconfig decls.
# wasi-sdk is NOT passed here: wasmify installs and resolves it itself, and
# wasi-configure.sh defaults to the same shared XDG path.
BUILD_ENV = WASMIFY_NON_INTERACTIVE=1 WASMIFY_NO_EMSCRIPTEN_DEFINE=1 WASMIFY_NO_POSIX_COMPAT=1

# Full pipeline replayed inside the container, top-to-bottom. Unlike a bazel
# project, CPython needs a configure phase first: scripts/wasi-configure.sh
# builds the host build-python, runs the wasm cross-configure, and patches
# pyconfig.h (host sockets/subprocess). `wasmify build` then replays the
# captured `make`; gen-proto + buf generate emit the wasm2go bundle; bundle-gomod
# finalises it as a Go module so the release tarball is self-contained.
WASMIFY_PIPELINE = \
	make tools && \
	$(BUILD_ENV) bash scripts/wasi-configure.sh && \
	$(BUILD_ENV) wasmify build --non-interactive && \
	wasmify generate-build && \
	wasmify parse-headers --header py.h && \
	wasmify gen-proto && \
	$(BUILD_ENV) wasmify wasm-build --with-bridge --optimize --non-interactive && \
	rm -rf build && \
	buf generate && \
	make bundle-gomod
# `rm -rf build` so `buf generate` writes the wasm2go bundle into a CLEAN tree:
# protoc-gen-wasmify-go overwrites the files it emits but never deletes stale
# ones, and the bundle's file SET depends on the wasm's size (a sub-threshold
# wasm yields a single-package layout with wasm2go_compat.go + top-level asm; a
# larger wasm yields the base/ + pN multi-package layout). Mixing the two leftover
# sets in one package does not compile.

.PHONY: all wasm wasm-clean tools bundle-gomod stdlib-zip image-pull help

# Install the tools wasmify.json declares (wasi-sdk; pre-baked in the image).
# Safe to re-run; already-installed tools are skipped.
tools:
	wasmify ensure-tools ./cpython --output-dir .

# Build the embeddable stdlib zip from the cpython submodule's Lib/ tree. CI's
# build.yml/release.yml run this same script when staging artifacts; this target
# is for local parity. Deterministic: same checkout -> byte-identical output.
# NOTE: run `bash scripts/wasi-configure.sh` first so Lib/subprocess.py carries
# the host-subprocess patch — otherwise the zip embeds vanilla subprocess.py and
# breaks go-python's subprocess path.
stdlib-zip:
	python3 scripts/make-stdlib-zip.py cpython/Lib python_stdlib.zip

# Build python.wasm + the wasm2go bundle from a clean checkout, exactly the way
# .github/workflows/build.yml does. Outputs:
#   .wasmify/wasm-build/output/python.wasm
#   build/wasm2go/                                <- wasm2go bridge + bundle
#   build/wasm2go/internal/wasm2go/go.mod         <- bundle module manifest
wasm:
	docker run --rm $(PLATFORM_FLAG) \
		-v $(CURDIR):/work -w /work \
		--memory=$(MEMORY) --cpus=$(CPUS) \
		-e WASMIFY_NON_INTERACTIVE=1 \
		$(IMAGE) \
		bash -c '$(WASMIFY_PIPELINE)'

# Write go.mod into the wasm2go bundle so the released tarball is a
# self-contained Go module. Parses bridge.Wasm2GoImportPath out of wasmify.json
# with grep+sed (no jq in the image; no Go toolchain needed — a literal manifest).
bundle-gomod:
	@if [ ! -d "$(WASM2GO_BUNDLE_DIR)" ]; then \
		echo "$(WASM2GO_BUNDLE_DIR) does not exist — run 'make wasm' first" >&2; \
		exit 1; \
	fi
	@path=$$(grep -E '"Wasm2GoImportPath"[[:space:]]*:' wasmify.json \
		| head -1 \
		| sed -E 's/.*"Wasm2GoImportPath"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/'); \
	if [ -z "$$path" ]; then \
		echo "wasmify.json bridge.Wasm2GoImportPath is empty; cannot stamp bundle go.mod" >&2; \
		exit 1; \
	fi; \
	printf 'module %s\n\ngo %s\n' "$$path" "$(WASM2GO_BUNDLE_GO_VER)" \
		> $(WASM2GO_BUNDLE_DIR)/go.mod; \
	echo "wrote $(WASM2GO_BUNDLE_DIR)/go.mod (module $$path)"

# Drop everything wasmify regenerates so the next `make wasm` runs from scratch.
# The committed inputs (wasmify.json, buf.{yaml,gen.yaml}, proto/wasmify, py.cc, py.h,
# patches/, scripts/, the cpython submodule) survive.
wasm-clean:
	rm -rf .wasmify api-spec.json build.json proto/python.proto bridge build \
	       .wasmify/wasm-build/output/python.wasm

# Refresh the cached toolchain image.
image-pull:
	docker pull $(PLATFORM_FLAG) $(IMAGE)

all: wasm

help:
	@echo 'Targets:'
	@echo '  wasm         Build python.wasm + wasm2go bundle inside $(IMAGE)'
	@echo '  wasm-clean   Drop generated artefacts; keep committed inputs'
	@echo '  image-pull   docker pull $(IMAGE)'
	@echo ''
	@echo 'Variables:'
	@echo '  IMAGE           = $(IMAGE)'
	@echo '  MEMORY          = $(MEMORY)'
	@echo '  CPUS            = $(CPUS)'
	@echo '  DOCKER_PLATFORM = $(DOCKER_PLATFORM)   (set to linux/amd64 on arm64 hosts)'
