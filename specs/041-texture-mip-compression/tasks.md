# Tasks: Texture Mipmap And Compression Pipeline

**Input**: Design documents from `specs/041-texture-mip-compression/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/
**Tests**: Required by FR-013 and repository workflow. Write focused tests before behavior changes where stable test entrypoints exist.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel with other tasks in the same phase because it touches different files or only adds tests.
- **[Story]**: User-story mapping for traceability.

## Phase 1: Setup

**Purpose**: Establish branch-local scaffolding and baseline evidence.

- [X] T001 Record current focused baseline by running `cmake --build Build --target NullusUnitTests --config Debug` and saving notable failures or pass status in `specs/041-texture-mip-compression/quickstart.md`
- [X] T002 [P] Add dependency-free first-scope source/header entries to `Runtime/Rendering/CMakeLists.txt` for `Runtime/Rendering/Assets/TextureBuildSettings.h`, `Runtime/Rendering/Assets/TextureBuildSettings.cpp`, `Runtime/Rendering/Assets/TextureFormatResolver.h`, `Runtime/Rendering/Assets/TextureFormatResolver.cpp`, `Runtime/Rendering/Assets/TextureMipGenerator.h`, `Runtime/Rendering/Assets/TextureMipGenerator.cpp`, `Runtime/Rendering/Assets/TextureEncoder.h`, and `Runtime/Rendering/Assets/TextureEncoder.cpp`
- [X] T003 [P] Add DirectXTex as an optional Windows editor/tool-time BC encoder dependency with pinned version, license metadata, target name, and non-Windows unavailable stub in `ThirdParty/DirectXTex/`, `ThirdParty/CMakeLists.txt`, and `Project/Editor/Assets/TextureEncoding/DirectXTexTextureEncoder.cpp`

## Phase 2: Foundational

**Purpose**: Blocking shared primitives required by all stories.

- [X] T004 Create `Tests/Unit/DX12FormatUtilsTests.cpp` with failing descriptor tests for RGBA8, RGBA16F, BC1, BC3, BC5, and BC7 pitch/block metadata plus sRGB-capability metadata
- [X] T005 Add BC1, BC3, BC5, and BC7 enum values, keep RGBA16F descriptor coverage, add texture color-space metadata enums, and implement descriptor helpers in `Runtime/Rendering/RHI/RHITypes.h`
- [X] T006 Audit every `GetTextureFormatBytesPerPixel` consumer and migrate import/upload/resource sizing paths that can see compressed formats to descriptor pitch helpers in `Runtime/Rendering/Assets/TextureArtifact.cpp`, `Runtime/Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.cpp`, `Runtime/Rendering/Resources/Texture.cpp`, and `Runtime/Rendering/Resources/Texture2D.cpp`
- [X] T007 Add backend format capability tests for sampled/upload/sRGB-view support and unsupported-format diagnostics in `Tests/Unit/DX12FormatUtilsTests.cpp`
- [X] T008 Implement per-format capability records and DX12 `D3D12_FEATURE_FORMAT_SUPPORT` population in `Runtime/Rendering/RHI/RHITypes.h`, `Runtime/Rendering/RHI/Backends/DX12/DX12Device.cpp`, and `Runtime/Rendering/RHI/Backends/DX12/DX12FormatUtils.cpp`
- [X] T009 Add RHI color-space carrier and DX12 SRV/resource compatibility tests for RGBA8, BC1, BC3, BC7 sRGB and BC5 linear behavior in `Tests/Unit/DX12FormatUtilsTests.cpp`
- [X] T010 Add `TextureColorSpaceIntent` or equivalent field to `RHITextureDesc` and/or `RHITextureViewDesc`, and wire DX12 sRGB view selection through `Runtime/Rendering/RHI/Core/RHIResource.h`, `Runtime/Rendering/RHI/Backends/DX12/DX12FormatUtils.cpp`, and `Runtime/Rendering/RHI/Backends/DX12/DX12TextureViewUtils.cpp`
- [X] T011 Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*"` and confirm descriptor, capability, and mapping tests pass

## Phase 3: User Story 1 - Build Importable Textures With Mips (Priority: P1) MVP

**Goal**: Import textures into deterministic native artifacts with offline mip chains.

**Independent Test**: Decode generated `.ntex` artifacts for color, normal, mask, UI, HDR, and tiny textures without renderer execution.

### Tests for User Story 1

- [X] T012 [US1] Add artifact compatibility tests for existing native schema v3/payload v2 RGBA8 artifacts plus native schema v4/payload v3 descriptor-driven subresource validation in `Tests/Unit/AssetMaterialConversionTests.cpp`
- [X] T013 [US1] Add mip generator tests for 1024x1024 full chain, single-mip UI texture, 1x1 tiny texture, and RGBA16F HDR preservation in `Tests/Unit/AssetMaterialConversionTests.cpp`
- [X] T014 [US1] Add normal-map mip renormalization test in `Tests/Unit/AssetMaterialConversionTests.cpp`

### Implementation for User Story 1

- [X] T015 [US1] Extend `TextureArtifactData` with schema-ready metadata, array-layer count, and subresource records while preserving existing RGBA8 decode compatibility in `Runtime/Rendering/Assets/TextureArtifact.h`
- [X] T016 [US1] Update `SerializeTextureArtifact` and `DeserializeTextureArtifact` with a dual-version read path, subresource ordering, and descriptor validation in `Runtime/Rendering/Assets/TextureArtifact.cpp`
- [X] T017 [US1] Create `TextureMipGenerator` with color, normal, mask, UI, and HDR intent handling in `Runtime/Rendering/Assets/TextureMipGenerator.h` and `Runtime/Rendering/Assets/TextureMipGenerator.cpp`
- [X] T018 [US1] Route `DecodeTextureArtifactFromEncodedImage` through `TextureMipGenerator` while preserving current default full mip behavior in `Runtime/Rendering/Assets/TextureArtifact.cpp`
- [X] T019 [US1] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetMaterialConversionTests.TextureArtifact*:AssetMaterialConversionTests.*Mip*"` and confirm US1 tests pass

## Phase 4: User Story 2 - Select Platform Formats Automatically (Priority: P2)

**Goal**: Resolve high-level texture settings into valid platform formats with deterministic fallbacks.

**Independent Test**: Resolve settings for color, normal, mask, HDR, UI, tiny/alignment, and override cases without renderer execution.

### Tests for User Story 2

- [X] T020 [US2] Add settings serialization tests for `resizePolicy`, `explicitFormat`, normalized platform override ordering, and platform mip override in `Tests/Unit/AssetImporterFacadeTests.cpp`
- [X] T021 [US2] Add resolver tests for Windows/DX12 color/no-alpha, color/alpha, normal, mask, HDR-to-RGBA16F, UI, tiny/alignment policy, explicit override, capability fallback, and unsupported override cases in `Tests/Unit/AssetImportPipelineTests.cpp`
- [X] T022 [US2] Add deterministic build identity positive and negative tests for source GUID/path identity, source hash, normalized settings, override ordering, importer version, postprocessor version, dependency hash, platform, resolved format, mip policy, color space, encoder id/version/options, DirectXTex version, and schema version in `Tests/Unit/AssetImportPipelineTests.cpp`

### Implementation for User Story 2

- [X] T023 [US2] Extend `TextureImporterSettings` and `TexturePlatformOverride` in `Project/Editor/Assets/AssetImporterSettings.h`
- [X] T024 [US2] Update setting parse/serialize helpers in `Project/Editor/Assets/AssetImporterSettings.cpp` and `Project/Editor/Assets/AssetImporterFacade.cpp`
- [X] T025 [US2] Create `TextureBuildSettings` and deterministic identity builder in `Runtime/Rendering/Assets/TextureBuildSettings.h` and `Runtime/Rendering/Assets/TextureBuildSettings.cpp`
- [X] T026 [US2] Create `TextureFormatResolver` with Windows/DX12 capability-driven rules, RGBA16F HDR fallback, top-level block-alignment policy, and diagnostics in `Runtime/Rendering/Assets/TextureFormatResolver.h` and `Runtime/Rendering/Assets/TextureFormatResolver.cpp`
- [X] T027 [US2] Integrate format resolution into external texture import path in `Project/Editor/Assets/ExternalAssetImporter.cpp`
- [X] T028 [US2] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*"` and confirm US2 tests pass

## Phase 5: User Story 3 - Load Compressed Artifacts Through RHI (Priority: P3)

**Goal**: Runtime loads RGBA8, RGBA16F, and BC artifacts through RHI with correct pitch and color-space handling.

**Independent Test**: Load generated artifacts and inspect `RHITextureDesc`, `RHITextureViewDesc`, upload plan, and unsupported-format diagnostics on Windows/DX12.

### Tests for User Story 3

- [X] T029 [US3] Add BC and RGBA16F artifact serialize/deserialize tests for BC1, BC3, BC5, BC7, RGBA16F, and sRGB metadata in `Tests/Unit/AssetMaterialConversionTests.cpp`
- [X] T030 [US3] Add DX12 upload-plan tests for BC1 4-bit blocks, BC3/BC5/BC7 8-bit blocks, RGBA16F, single mip, and multi-mip in `Tests/Unit/DX12TextureUploadUtilsTests.cpp`
- [X] T031 [US3] Add texture loader tests for compressed artifact metadata, sRGB propagation, and unsupported backend diagnostics in `Tests/Unit/AssetMaterialConversionTests.cpp`

### Implementation for User Story 3

- [X] T032 [US3] Update `BuildDX12TextureUploadPlan` to compute source/native row and slice pitch from format blocks in `Runtime/Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.cpp`
- [X] T033 [US3] Update `CopyDX12TextureUploadRow` to copy compressed block rows directly and keep RGB8 expansion only for RGB8 in `Runtime/Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.cpp`
- [X] T034 [US3] Update initial texture upload validation to use descriptor-derived total sizes in `Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp`
- [X] T035 [US3] Update `Texture2D::SetTextureResource` to pass compressed artifact metadata, mip count, and color-space intent without bytes-per-pixel assumptions in `Runtime/Rendering/Resources/Texture2D.cpp`
- [X] T036 [US3] Update DX12 SRV/resource format selection for sRGB-compatible texture views in `Runtime/Rendering/RHI/Backends/DX12/DX12TextureViewUtils.cpp` and descriptor consumers
- [X] T037 [US3] Update `TextureLoader` to surface unsupported compressed format failures instead of silently falling back in `Runtime/Rendering/Resources/Loaders/TextureLoader.cpp`
- [X] T038 [US3] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*:AssetMaterialConversionTests.*Texture*"` and confirm US3 tests pass

## Phase 6: User Story 4 - Preserve Extension Paths For Later Platforms (Priority: P4)

**Goal**: Ensure Vulkan, ASTC, ETC2, cubemaps, and streaming can be added without a settings rewrite and without another artifact subresource rewrite.

**Independent Test**: Review resolver, descriptor, encoder, and artifact contracts for data-driven extension points and explicit unsupported diagnostics.

### Tests for User Story 4

- [X] T039 [US4] Add reserved-platform resolver tests for Vulkan, ASTC, ETC2, BC6H, and missing encoder cases in `Tests/Unit/AssetImportPipelineTests.cpp`
- [X] T040 [US4] Add artifact metadata tests that preserve target platform, encoder id/version fields, array-layer count, and cube face ordering metadata without enabling runtime cubemap compression in `Tests/Unit/AssetMaterialConversionTests.cpp`

### Implementation for User Story 4

- [X] T041 [US4] Add reserved format family descriptor entries or documented unsupported capability entries for ASTC/ETC2/BC6H without enabling runtime support in `Runtime/Rendering/RHI/RHITypes.h`
- [X] T042 [US4] Create encoder registry hooks that allow future ASTC/ETC2/BC6H encoders without changing importer settings in `Runtime/Rendering/Assets/TextureEncoder.h` and `Runtime/Rendering/Assets/TextureEncoder.cpp`
- [X] T043 [US4] Update `specs/041-texture-mip-compression/quickstart.md` with explicit evidence boundaries for Windows/DX12 versus reserved platforms
- [X] T044 [US4] Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.TextureArtifact*"` and confirm US4 tests pass

## Phase 7: Polish And Cross-Cutting Validation

**Purpose**: Final integration, documentation, and quality gates.

- [X] T045 [P] Update `Docs/AIWorkflow.md` or a focused rendering asset doc if the workflow for texture import validation changes
- [X] T046 [P] Run `cmake --build Build --target NullusUnitTests --config Debug`
- [X] T047 Run `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.*Texture*:DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*"`
- [ ] T048 Capture Windows/DX12 runtime evidence for RGBA8, RGBA16F, BC1, BC3, BC5, and BC7 artifact loading before claiming backend support
- [X] T049 Validate SC-001 compatibility by running the existing representative texture import corpus or documenting the exact sample set and pass rate in `specs/041-texture-mip-compression/quickstart.md`
- [X] T050 Run `/plan-review` quality gate on the implementation diff and fix all P0/P1 findings before commit

## Dependencies And Execution Order

### Phase Dependencies

- Phase 1 has no dependencies.
- Phase 2 blocks all user stories because every story depends on shared format metadata, backend capabilities, and color-space carriers.
- US1 depends on Phase 2.
- US2 depends on Phase 2 and can begin while US1 implementation is in progress, but importer integration in T027 should wait for the mip builder from US1.
- US3 depends on Phase 2 and artifact schema from US1.
- US4 depends on descriptor/resolver/encoder structure from US2 and US3.
- Phase 7 depends on selected user stories being complete.

### Parallel Opportunities

- T002 and T003 can run in parallel.
- T045 and T046 can run in parallel after selected implementation phases are complete.

Test tasks that edit the same test file are intentionally sequential even when conceptually independent, so multi-agent implementation does not create avoidable merge conflicts.

## Implementation Strategy

### MVP First

1. Complete Phase 1 and Phase 2.
2. Complete US1 to produce deterministic RGBA8/RGBA16F artifacts with offline mips.
3. Stop and validate with artifact decode tests before introducing BC compression.

### Incremental Delivery

1. Add shared descriptor, backend capability, color-space carrier, and artifact validation.
2. Add offline mip build path.
3. Add resolver and encoder boundary with RGBA8/RGBA16F fallback.
4. Add BC artifacts and DX12 upload.
5. Add reserved-platform tests and documentation.

### Commit Strategy

- Commit after each validated phase or story checkpoint.
- Do not commit generated-file edits.
- Run plan-review before any final commit or phase sign-off.
