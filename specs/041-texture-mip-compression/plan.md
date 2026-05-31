# Implementation Plan: Texture Mipmap And Compression Pipeline

**Branch**: `041-texture-mip-compression` | **Date**: 2026-05-30 | **Spec**: [spec.md](spec.md)
**Input**: Feature specification from `specs/041-texture-mip-compression/spec.md`

## Summary

Nullus will add an offline texture build pipeline that generates deterministic mip chains, resolves high-level importer settings into platform-specific runtime formats, writes native `.ntex` artifacts, and loads those artifacts through RHI without assuming RGBA8 bytes-per-pixel layout.

The recommended design is a hybrid of Unity and UE:

- Use Unity-style importer UX: texture type, color-space intent, mip policy, compression intent, and per-platform overrides remain the user-facing controls.
- Use UE-style build architecture: importer settings resolve into immutable build settings, build settings select a platform encoder, and artifact identity includes source/settings/platform/encoder versions so rebuilds are deterministic.

The first implementation targets Windows/DX12 with RGBA8 fallback, RGBA16F HDR fallback, and BC1, BC3, BC5, BC7 schema/descriptor/upload-plan plumbing. End-to-end BC runtime support remains gated on real encoder output plus Windows/DX12 runtime evidence. Vulkan, ASTC, ETC2, BC6H, cubemaps, and streaming mips are reserved extension paths only until they have backend-specific validation.

## Technical Context

**Language/Version**: C++20 in Nullus runtime/editor; CMake project with generated reflection outputs under `Runtime/*/Gen/` and `Project/Editor/Gen/`.
**Primary Dependencies**: Existing Nullus runtime/editor modules, stb image decode path in `Runtime/Rendering/Assets/TextureArtifact.cpp`, DX12 backend on Windows, GoogleTest for unit coverage, and DirectXTex as the first Windows editor/tool-time BC encoder dependency. BC encoding remains isolated behind `TextureEncoder` so future encoders can be swapped without changing importer/runtime contracts. The DirectXTex facade is optional and Windows-only; runtime `NLS_Render` must not link DirectXTex.
**Storage**: Native `.ntex` artifacts inside the existing asset artifact container and library paths. Build identity is stored as artifact metadata or adjacent manifest fields where the current asset database already records importer version and target platform.
**Testing**: `NullusUnitTests` with targeted tests in `Tests/Unit/AssetMaterialConversionTests.cpp`, `Tests/Unit/AssetImportPipelineTests.cpp`, `Tests/Unit/AssetImporterFacadeTests.cpp`, and `Tests/Unit/DX12TextureUploadUtilsTests.cpp`. Runtime validation uses Windows/DX12 only for first support claims.
**Target Platform**: Windows/DX12 for supported compressed texture loading; non-Windows and non-DX12 remain compatible through RGBA8 fallback and explicit unsupported-format diagnostics.
**Project Type**: Desktop engine/editor/runtime with asset import pipeline and RHI backend integration.
**Performance Goals**: Runtime texture load performs no image resampling and no compression; upload pitch computation is O(subresource count). Offline import may be slower for high-quality BC7 but must be deterministic and cacheable by build identity.
**Constraints**: Do not hand-edit generated files. Keep existing uncompressed `.ntex` and direct image loading usable during rollout. Do not claim Vulkan/ASTC/ETC2 support without backend-specific validation. Avoid renderer-side format forks; format layout belongs in shared RHI descriptors, while per-device support belongs in backend format capability records.
**Scale/Scope**: One texture artifact schema evolution, one importer/build path, one shared RHI format metadata path, one validated backend family, and tests for representative color/normal/mask/UI/HDR decisions.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Spec-first major changes**: PASS. The change affects `Runtime/Rendering`, `Project/Editor/Assets`, tests, and backend upload behavior, so it is tracked under `specs/041-texture-mip-compression/`.
- **Validation matches subsystem**: PASS. The plan requires unit tests for artifact/schema/resolver/upload calculations and Windows/DX12 runtime evidence for compressed upload support. Reserved platforms are not reported as supported.
- **Generated code/backend boundaries**: PASS. No `Runtime/*/Gen/` or `Project/Editor/Gen/` files are hand-edited. Format handling is centralized in RHI descriptors and backend mappings.
- **Incremental verified delivery**: PASS. Tasks are split by user story and each story has independent tests/checkpoints.
- **Product runtime preservation**: PASS. Existing RGBA8 `.ntex` and direct loader behavior stay valid while compressed artifacts are staged.

## Project Structure

### Documentation (this feature)

```text
specs/041-texture-mip-compression/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── texture-artifact-schema.md
│   ├── texture-build-contract.md
│   ├── texture-encoder-contract.md
│   └── texture-format-descriptor.md
└── tasks.md
```

### Source Code (repository root)

```text
Runtime/Rendering/
├── Assets/
│   ├── TextureArtifact.h/.cpp
│   ├── TextureBuildSettings.h/.cpp
│   ├── TextureFormatResolver.h/.cpp
│   ├── TextureMipGenerator.h/.cpp
│   └── TextureEncoder.h/.cpp
├── RHI/
│   ├── RHITypes.h
│   ├── Core/RHIResource.h
│   └── Backends/DX12/
│       ├── DX12FormatUtils.h/.cpp
│       ├── DX12TextureUploadUtils.h/.cpp
│       ├── DX12TextureViewUtils.h/.cpp
│       ├── DX12Device.cpp
│       └── DX12Resource.cpp
└── Resources/
    ├── Texture2D.h/.cpp
    └── Loaders/TextureLoader.h/.cpp

Project/Editor/Assets/
├── AssetImporterSettings.h/.cpp
├── AssetImporterFacade.h/.cpp
├── ExternalAssetImporter.cpp
└── TextureEncoding/
    └── DirectXTexTextureEncoder.h/.cpp

ThirdParty/
└── DirectXTex/              # Planned optional Windows editor/tool-time encoder dependency

Tests/Unit/
├── AssetImporterFacadeTests.cpp
├── AssetImportPipelineTests.cpp
├── AssetMaterialConversionTests.cpp
├── DX12FormatUtilsTests.cpp # Planned new test file
└── DX12TextureUploadUtilsTests.cpp
```

**Structure Decision**: Reuse the existing `.ntex`, importer facade, external asset importer, `Texture2D`, and DX12 upload paths. Add small focused runtime asset helpers for build settings, mip generation, format resolution, and encoder abstraction instead of expanding `TextureArtifact.cpp` into a monolith.

## Phase 0: Research Decisions

Research output is captured in [research.md](research.md). The plan adopts:

- UE-like build/cook separation and deterministic derived-data identity.
- Unity-like importer settings and per-platform override model.
- Shared format descriptors with block-compressed semantics.
- Per-backend format capability records instead of enum-presence checks.
- Windows/DX12 BC support first; reserved extension points for Vulkan, ASTC, ETC2, cubemap mips, and streaming.

## Phase 1: Design Outputs

Design output is captured in:

- [data-model.md](data-model.md)
- [quickstart.md](quickstart.md)
- [texture-artifact-schema.md](contracts/texture-artifact-schema.md)
- [texture-build-contract.md](contracts/texture-build-contract.md)
- [texture-encoder-contract.md](contracts/texture-encoder-contract.md)
- [texture-format-descriptor.md](contracts/texture-format-descriptor.md)

## Implementation Phases

### Phase A: Shared Format Metadata And Artifact Schema

Add BC formats to `TextureFormat`, ensure `RGBA16F` participates in the descriptor table, introduce `TextureFormatDescriptor`, add `TextureColorSpaceIntent` to `RHITextureDesc` and/or `RHITextureViewDesc`, and migrate artifact validation from bytes-per-pixel math to block-footprint math. This phase must preserve existing RGBA8 artifacts and tests, audit every `GetTextureFormatBytesPerPixel` consumer, and make compressed/unknown formats fail closed in legacy BPP paths.

### Phase B: Offline Mip Builder

Move mip generation behind `TextureMipGenerator` and resolve import settings into `TextureBuildSettings`. Mips are generated before compression. Normal maps must renormalize sampled normal vectors; UI/cursor textures can remain single mip.

### Phase C: Format Resolver And Encoder Boundary

Implement high-level compression resolution for Windows/DX12 using explicit backend format capabilities: color without alpha can choose BC1, color with alpha can choose BC7 or BC3 by quality, normal maps choose BC5, mask maps choose BC1/BC5 depending channel policy, HDR uses RGBA16F until BC6H is deliberately added, and unsupported or backend-alignment-incompatible cases fall back with diagnostics. Add a dependency-free `TextureEncoder` abstraction, a deterministic RGBA8/RGBA16F passthrough encoder, and an optional Windows editor/tool DirectXTex BC encoder hook with intent-specific options.

### Phase D: Runtime Loader And DX12 Upload

Update `Texture2D`, `TextureLoader`, DX12 format mapping, `DX12TextureViewUtils`, and upload planning so compressed artifacts upload by blocks. DX12 mapping must select compatible sRGB-capable DXGI resource/SRV formats for color artifacts whose metadata requires sRGB sampling. Runtime must reject unsupported compressed formats explicitly instead of falling back silently.

### Phase E: Editor Integration And Validation

Persist additional texture import settings, route external texture imports through the new builder, add diagnostics to manifests, and validate with targeted tests plus Windows/DX12 runtime evidence.

## Post-Design Constitution Check

- **Spec-first major changes**: PASS. `spec.md`, `plan.md`, `research.md`, `data-model.md`, contracts, and `tasks.md` are in one spec bundle.
- **Validation matches subsystem**: PASS. `quickstart.md` and tasks distinguish automated tests from Windows/DX12 runtime evidence and make no unsupported backend claims.
- **Generated code/backend boundaries**: PASS. Tasks avoid generated files and route backend-specific behavior through DX12 utilities.
- **Incremental verified delivery**: PASS. User stories can be implemented in priority order with independent test checkpoints.
- **Product runtime preservation**: PASS. RGBA8 fallback, direct image compatibility, and explicit diagnostics are required throughout.

## Complexity Tracking

No constitution violations are required.
