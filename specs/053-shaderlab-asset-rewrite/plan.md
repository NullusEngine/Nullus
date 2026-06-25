# Implementation Plan

## Architecture

Create a new module namespace `NLS::Render::ShaderLab` under `Runtime/Rendering/ShaderLab/` and use it as the sole shader/material runtime path.

Main components:

- `ShaderLabSourceLocation`: file, line, column, byte offset.
- `ShaderLabDiagnostic`: parser/import/runtime diagnostics.
- `ShaderLabTypes`: import descriptors and runtime enums.
- `ShaderLabLexer`: tokenizes ShaderLab syntax outside HLSL blocks.
- `ShaderLabParser`: recursive-descent parser with raw HLSL extraction.
- `ShaderLabAsset`: immutable snapshot runtime object, pass lookup, generation-aware pass handles.
- `ShaderLabMaterial`: shader-bound values, defaults, keywords, hot reload migration, and runtime/editor material state.
- `ShaderLabVariant`: stable keyword, variant, artifact, and compile request keys.
- `ShaderLabPipelineKey`: render-state and graphics-pipeline key hashing independent from shader artifacts.
- `ShaderLabBindingLayout`: builds material binding layout from existing `Resources::ShaderReflection`.
- `ShaderLabHotReload`: atomic snapshot replacement with injected validator/compiler facade.

## Reuse Points

- Use `NLS::Guid` for shader IDs.
- Use `ShaderCompiler::ShaderStage`, `ShaderCompiler::ShaderTargetPlatform`, and `RHI::NativeBackendType` for stage/backend/target.
- Use `Resources::ShaderReflection` to construct material binding layout. Do not calculate cbuffer offsets without reflection.
- Rewire importer and editor material conversion paths to emit ShaderLab materials instead of legacy `.mat` payloads.
- Use existing `RHI::RHIGraphicsPipelineDesc` and pipeline hash behavior as reference, but keep ShaderLab pipeline key as an independent lightweight key.

## Build Integration

Runtime and unit tests are discovered by existing CMake globbing. No generated files are edited.

## Test Strategy

Add tests before implementation:

- Parser: names, properties, tags, render state, HLSL raw text, pragmas, errors and accurate locations.
- Material conversion: shader/material payload emission, ShaderLab artifact paths, shader references, and property serialization.
- Material: defaults, Set/Get, type mismatch, keyword toggles, reload migration and orphans.
- Variant: stable keyword hash, backend/target/fingerprint/include invalidation, macro generation.
- Pass/Pipeline: LightMode lookup, missing pass policy, Forward vs DepthOnly key split, render-state key inclusion, RT layout excluded from artifact keys.
- Hot reload: success swap, failure preserves old snapshot, safe pass handle behavior, PSO invalidation signal.

## Phases

1. Add failing tests and spec bundle.
2. Implement parser and descriptors.
3. Implement runtime asset/pass lookup/material/keywords.
4. Implement variant/artifact/pipeline keys and reflection binding layout helpers.
5. Migrate imported material payloads and editor/runtime material consumers to ShaderLab-only assets.
6. Implement hot reload facade and examples.
7. Run focused tests and full unit target.
8. Run plan-review quality gate.
