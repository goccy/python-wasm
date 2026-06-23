#!/usr/bin/env python3
"""Build the embeddable Python standard-library zip from the CPython submodule.

python.wasm is a bare interpreter: the pure-Python half of the standard library
(encodings/, io.py, codecs.py, importlib/, ...) lives on disk, not in the wasm.
This produces `python_stdlib.zip`, a trimmed copy of `cpython/Lib/` that an
embedding application (go-python) unpacks at runtime as the module search path.
It is published as a release asset alongside python.wasm and the wasm2go bundle
and attested, so consumers verify it the same way — and it is locked to the
exact CPython version python.wasm was built from (the submodule is pinned).

Trim (matches what go-python has historically embedded):
  - drop the top-level dirs that are useless in an embedded interpreter:
    test, idlelib, tkinter, turtledemo, ensurepip, pydoc_data, __pycache__
  - drop any nested test/ or tests/ directory, and any __pycache__/
  - drop compiled *.pyc

The archive is written deterministically (sorted entries, fixed timestamps,
fixed permissions) so the same CPython checkout always yields byte-identical
bytes — handy for reproducibility, though the attestation chain only requires
that the released bytes are what the consumer verifies.

IMPORTANT: run this AGAINST A PATCHED Lib. python-wasm applies host-capability
patches (scripts/wasi-configure.sh) that edit Lib/subprocess.py; the zip must
carry that patched file so it matches the wasm. The CI pipeline runs
wasi-configure.sh before staging, so cpython/Lib is already patched there; the
staging step also guards the result. On a clean checkout the zip would embed
vanilla subprocess.py and silently break go-python's subprocess path.

Usage: make-stdlib-zip.py <cpython/Lib dir> <output zip>
"""

import os
import sys
import zipfile

# Excluded only when they are the FIRST path component.
TOP_LEVEL_EXCLUDES = {
    "__pycache__",
    "ensurepip",
    "idlelib",
    "pydoc_data",
    "test",
    "tkinter",
    "turtledemo",
}
# Excluded at ANY depth.
ANYWHERE_EXCLUDES = {"test", "tests", "__pycache__"}

# Fixed DOS timestamp (1980-01-01 00:00:00) for reproducible archives.
FIXED_DATE_TIME = (1980, 1, 1, 0, 0, 0)


def included(rel_path: str) -> bool:
    parts = rel_path.split("/")
    if parts[0] in TOP_LEVEL_EXCLUDES:
        return False
    if any(p in ANYWHERE_EXCLUDES for p in parts[:-1]):
        return False
    if rel_path.endswith(".pyc"):
        return False
    return True


def collect(lib_dir: str) -> list[str]:
    out = []
    for root, _dirs, files in os.walk(lib_dir):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), lib_dir)
            rel = rel.replace(os.sep, "/")
            if included(rel):
                out.append(rel)
    out.sort()
    return out


def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: make-stdlib-zip.py <Lib dir> <output zip>\n")
        return 2
    lib_dir, out_path = sys.argv[1], sys.argv[2]
    if not os.path.isdir(lib_dir):
        sys.stderr.write(f"not a directory: {lib_dir}\n")
        return 1

    rels = collect(lib_dir)
    with zipfile.ZipFile(out_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for rel in rels:
            info = zipfile.ZipInfo(filename=rel, date_time=FIXED_DATE_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16  # regular file, rw-r--r--
            with open(os.path.join(lib_dir, rel), "rb") as fh:
                zf.writestr(info, fh.read())
    sys.stderr.write(f"wrote {out_path}: {len(rels)} files\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
