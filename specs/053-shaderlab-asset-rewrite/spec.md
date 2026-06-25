# 053 ShaderLab Asset Rewrite

## Goal

Build a ShaderLab-style shader/material runtime under `Runtime/Rendering/ShaderLab/` and migrate shader/material ownership to that path as the only supported runtime/editor path.

The first phase must provide a working minimum loop for `.shader` parsing, material defaults and keywords, LightMode pass lookup, stable variant/artifact keys, PSO key separation, and atomic hot reload semantics. The migration phase must remove legacy shader/material production and consumption from imported materials, editor previews, resource managers, and renderer material binding instead of preserving a compatibility bridge.

## Scope

- Add a new `ShaderLabAsset` import/runtime model.
- Make `ShaderLabAsset` and `ShaderLabMaterial` the only shader/material asset model used by importers, editor previews, and scene renderers.
- Parse a Unity ShaderLab-like `.shader` syntax with raw HLSL blocks.
- Support one or more `SubShader` blocks and multiple `Pass` blocks.
- Support properties: `Float`, `Int`, `Range`, `Vector`, `Color`, `Texture2D`, `TextureCube`.
- Support tags and render state: `Cull`, `ZWrite`, `ZTest`, `Blend`.
- Extract `#pragma vertex`, `#pragma fragment`, `#pragma shader_feature`, and `#pragma multi_compile`.
- Insert `#line` before extracted HLSL when building compiler input.
- Reuse existing DXC compilation, shader reflection, artifact cache, and RHI data types where practical.
- Keep Shader Variant cache and Graphics Pipeline cache as separate concepts.
- Provide sample ShaderLab assets and the first built-in Nullus HLSL library transforms.
- Stop generating legacy `.mat` material payloads and legacy `:Shaders/*.hlsl` material shader references.

## Non-Goals

- Do not preserve old material/shader compatibility as a runtime fallback.
- Do not add a legacy migration bridge for old project assets; old assets may fail until reauthored or regenerated as ShaderLab assets.
- Do not implement Unity-compatible Surface Shader, `UsePass`, `GrabPass`, full fallback chains, PBR, or full custom inspectors.
- Do not precompile every keyword combination during import.
- Do not infer HLSL cbuffer offsets by hand; reflection owns layout.

## Required Behavior

### Parser

The parser is recursive-descent, not a whole-file regex parser. It tracks source file, line, column, and byte offset. `HLSLPROGRAM` starts a raw block that is copied verbatim until `ENDHLSL`; the parser does not tokenize HLSL contents.

Diagnostics include a location and a clear message. Error cases include unmatched braces, missing `ENDHLSL`, duplicate properties, and unsupported property types.

### Runtime Asset

`ShaderLabAsset` owns immutable runtime data snapshots. Pass lookup uses `Tags { "LightMode" = "..." }`, not fixed pass indices. Missing `LightMode` uses the first matching unnamed/default pass only when explicitly requested; renderer-facing lookup fails closed by returning no pass.

### Material

`ShaderLabMaterial` stores a shader snapshot, stable property IDs derived from property names, typed values, orphan values, render queue override, and keyword set. Hot reload migrates values by property name, creates defaults for new properties, and preserves removed properties as orphans.

### Variant and Artifact Keys

Keyword hashing is stable and order-independent. Shader variant identity includes shader GUID, subshader index, pass index, stage, keyword hash, backend, and shader model. Artifact identity additionally includes entry point, HLSL hash, include dependency hash, compiler fingerprint, target, and compile arguments hash.

### Pipeline Keys

Shader artifact keys exclude render target layout. Graphics pipeline keys include program ID, render state hash, vertex layout hash, render target layout hash, topology, and sample count.

### Hot Reload

Hot reload parses a candidate shader, validates/compiles requested variants through an injected compiler facade, and atomically swaps the asset snapshot only if validation succeeds. Existing pass handles are generation-aware and never expose dangling pointers. Failed reload keeps the old shader and stores diagnostics.

## Success Criteria

- New unit tests cover parser, material migration, variant/artifact keys, pass/pipeline keys, and hot reload.
- Imported material artifacts are emitted as ShaderLab material payloads and reference `.shader` shader assets.
- Runtime/editor material loading uses ShaderLab material artifacts only.
- Scene renderer and editor preview material paths consume ShaderLab materials only.
- Renderer pass selection uses `LightMode` tags rather than fixed pass indices.
- Legacy shader/material tests are either migrated to ShaderLab expectations or removed with the legacy path.
- New example `.shader` files demonstrate unlit color, unlit texture, alpha test, transparent, and forward plus depth-only passes.
