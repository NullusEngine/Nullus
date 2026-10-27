# Vendored Zstandard

- Version: **1.5.6** (`zstd-1.5.6.tar.gz` from the official Facebook/Meta release page).
- Contents: upstream `lib/common`, `lib/compress`, `lib/decompress`, `lib/dictBuilder`, the
  public headers, `LICENSE`, `COPYING`, and the upstream `lib/README.md`.
- The upstream build system is intentionally not vendored. `CMakeLists.txt` in this directory
  compiles the library with pinned options:
  - `ZSTD_LEGACY_SUPPORT=0` (frame format is fixed by the vendored copy)
  - `ZSTD_DISABLE_ASM=1` (identical output on every host)
  - no `ZSTD_MULTITHREAD` (the `.npak` writer uses a single-context encoder so the same input
    produces byte-identical packages)
- Nullus's `.npak` container records this version in its header. Bumping the vendored copy is a
  format-affecting change and must go through the container version policy.

Do not replace this directory with a system install: `NullusBuild`, the editor, and the player
runtime must agree on exact compression bytes.
