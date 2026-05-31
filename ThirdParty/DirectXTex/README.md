# DirectXTex

Nullus reserves `DirectXTex::DirectXTex` as the optional Windows editor/tool-time BC encoder dependency.

- Pinned source drop: `jul2025`
- Pinned commit: `32b2a8e`
- Upstream: `microsoft/DirectXTex`
- License: MIT
- Runtime boundary: `NLS_Render` must not link DirectXTex for texture loading.
- Packaging: `ThirdParty/DirectXTex/src` is expected to be a pinned Git submodule or an equivalent vendored source drop at the pinned commit.

Place the vendored source under `ThirdParty/DirectXTex/src/DirectXTex/` before enabling the real encoder. Until then, CMake still provides `DirectXTex::DirectXTex` with `NLS_HAS_DIRECTXTEX=0`, and the editor encoder facade reports `directxtex-bc` as unavailable.
