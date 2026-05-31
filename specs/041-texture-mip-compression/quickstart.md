# Quickstart: Texture Mipmap And Compression Pipeline

## Prerequisites

- Windows developer environment with the existing `Build/` CMake configuration.
- `NullusUnitTests` target available.
- Windows/DX12 runtime available for final backend evidence.

## Automated Validation

Run focused unit tests after each implementation phase:

```powershell
cmake --build Build --target NullusUnitTests --config Debug
Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetMaterialConversionTests.TextureArtifact*:DX12TextureUploadUtilsTests.*:AssetImporterFacadeTests.TextureImporterSettings*"
```

After importer integration, run:

```powershell
Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.TextureLoaderReads*"
```

## Baseline Evidence

- 2026-05-30: `cmake --build Build --target NullusUnitTests --config Debug` succeeded before the first texture mip/compression code changes in this branch.
- 2026-05-30: After foundational Phase 2 work, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*"` passed with `18` tests.
- 2026-05-30: After `US1` artifact and mip-generator work, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetMaterialConversionTests.TextureArtifact*:AssetMaterialConversionTests.*Mip*"` passed with `7` tests.
- 2026-05-30: After `US2` settings/resolver/build-identity work, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*"` passed with `12` tests.
- 2026-05-30: After RGBA16F half-float mip-generator preservation and external texture import passthrough resolver plumbing, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetMaterialConversionTests.TextureArtifact*:AssetMaterialConversionTests.*Mip*:AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*"` passed with `23` tests. This does not yet validate encoded HDR source import or BC encoder output.
- 2026-05-30: Cross-check including DX12 descriptor/upload helpers, resolver diagnostics, and runtime fail-closed guards, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*:RHITypesTests.ReportsBytesPerPixelForExpandedTextureFormats:AssetMaterialConversionTests.TextureArtifact*:AssetMaterialConversionTests.*Mip*:AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*:TextureResourceLifecycleTests.FailedTextureArtifactUploadReturnsNull:TextureResourceLifecycleTests.CompressedTextureArtifactFailsClosedUntilRuntimeUploadIsImplemented:TextureResourceLifecycleTests.DefaultTextureViewCoversUploadedMipChain"` passed with `46` tests.
- 2026-05-30: Full regression after structured resolver diagnostics, `Build/bin/Debug/NullusUnitTests.exe` ran `1960` tests: `1959` passed and `1` FBX-SDK-dependent test skipped.
- 2026-05-30: Resolver diagnostics red/green: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.TextureFormatResolverReportsFallbackAndFailureDiagnostics"` passed with `1` test after first failing to compile because `ResolveTextureBuildSettingsWithDiagnostics` did not exist.
- 2026-05-30: After structured resolver diagnostics, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*"` passed with `14` tests.
- 2026-05-30: After external texture import surfaced resolver diagnostics, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.ExternalGltfNormalTexturesUseNormalMipIntent"` passed with `1` test and `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*"` passed with `14` tests.
- 2026-05-30: After `US3` runtime artifact/load/upload plumbing, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*:AssetMaterialConversionTests.*Texture*"` passed with `47` tests. Coverage includes BC/RGBA16F artifact serialize/deserialize, BC block upload planning, compressed row copy, RHI metadata propagation through `TextureLoader`, and unsupported backend warnings. This is still unit-level RHI evidence, not a real Windows/DX12 frame/runtime capture.
- 2026-05-30: After `US4` reserved-platform extension work, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.TextureFormatResolverKeepsReservedPlatformsAndFormatsExplicitlyUnsupported:AssetImportPipelineTests.TextureEncoderRegistryFindsEncodersByIdAndFormat:AssetMaterialConversionTests.TextureArtifactPreservesBuildMetadataAndCubeFaceOrdering:DX12FormatUtilsTests.DescribesFirstScopeTextureFormats"` passed with `4` tests.
- 2026-05-30: `US4` checkpoint, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.TextureArtifact*"` passed with `20` tests.
- 2026-05-30: Phase 7 focused regression, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.*Texture*:DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*"` passed with `64` tests.
- 2026-05-30: After review fixes for encoder/format coupling, artifact metadata, mip/subresource schema semantics, DX12 view fail-closed behavior, command-buffer upload row-pitch guards, and subresource-span initial uploads that avoid repacking full mip chains into a second CPU buffer, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="DX12TextureUploadUtilsTests.BuildsInitialTextureUploadRequestFromSubresourceSpans:DX12TextureUploadUtilsTests.*:AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.*Texture*:DX12FormatUtilsTests.*:TextureResourceLifecycleTests.*TextureArtifact*:TextureResourceLifecycleTests.DefaultTextureViewCoversUploadedMipChain"` passed with `69` tests.
- 2026-05-30: After wiring the real DirectXTex editor encoder with vendored upstream `jul2025` source, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.DirectXTexTextureEncoderProduces*"` passed with `2` tests covering BC1, BC3, BC5, and BC7 encoded artifact payloads.
- 2026-05-30: Focused texture regression after enabling DirectXTex encoding, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.*Texture*:DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*:TextureResourceLifecycleTests.*TextureArtifact*:TextureResourceLifecycleTests.DefaultTextureViewCoversUploadedMipChain"` passed with `71` tests.
- 2026-05-30: Full `Build/bin/Debug/NullusUnitTests.exe` was attempted after the focused texture regression. It reached late non-texture suites but failed with a CRT debug heap assertion (`_CrtIsValidHeapPointer` / `is_block_type_valid`) after CLI/backend diagnostic tests. Treat full-suite status as unresolved; the texture/mip/compression focused suite above is the current validation evidence for this feature.
- 2026-05-31: `cmake -S . -B Build` succeeded after DirectXTex dependency pin checks; CMake built the vendored `DirectXTex.vcxproj` from `ThirdParty/DirectXTex/src` at commit `32b2a8e`.
- 2026-05-31: `cmake --build Build --target NullusUnitTests --config Debug -- /m:1` succeeded after the DirectXTex encoder API was changed to reference source mip data instead of deep-copying request payloads, after DX12 copy/upload guard fixes, and after `.ntex` fail-closed loader tightening.
- 2026-05-31: `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.DirectXTexTextureEncoderProduces*"` passed with `2` tests. The encoder now feeds DirectXTex through `DirectX::Image` views over existing RGBA mip memory and still produces BC1, BC3, BC5, and BC7 artifact payloads.
- 2026-05-31: Focused texture regression, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.*Texture*:AssetMaterialConversionTests.*Mip*:DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*:TextureResourceLifecycleTests.*TextureArtifact*:TextureResourceLifecycleTests.DefaultTextureViewCoversUploadedMipChain"` passed with `72` tests.
- 2026-05-31: Targeted review-fix regression, `Build/bin/Debug/NullusUnitTests.exe --gtest_filter="AssetImportPipelineTests.ExternalGltfNormalTexturesUseNormalMipIntent:AssetImportPipelineTests.ExternalObjModelImportWritesTextureArtifactPayloads:AssetMaterialConversionTests.TextureLoaderWarnsWhenCompressedArtifactBackendIsUnsupported:DX12TextureUploadUtilsTests.Builds3DTexturePlanWithoutTreatingDepthAsArrayLayers"` passed with `4` tests.

## Current Evidence Boundaries

- RGBA8 and RGBA16F mip generation are covered by artifact-level unit tests.
- External model texture import now routes decoded texture artifacts through resolver settings, writes target platform/build identity/encoder metadata into `.ntex`, and surfaces resolver diagnostics through importer warnings. When the optional DirectXTex source is present, the external path resolves Windows/DX12 BC formats and encodes BC1/BC3/BC5/BC7 artifacts through `directxtex-bc`; otherwise it keeps RGBA8/RGBA16F passthrough behavior.
- Resolver diagnostics now cover preferred-format fallback, unsupported explicit formats, unknown explicit format strings, and HDR-preserving RGBA16F gaps through `ResolveTextureBuildSettingsWithDiagnostics`; external model texture import surfaces those diagnostics as `external-model-importer-texture-format-resolution`.
- BC1/BC3/BC5/BC7 descriptors, DX12 mappings, artifact payloads, upload planning, and RHI metadata propagation are covered by unit tests. A real Windows/DX12 runtime capture is still required before claiming end-to-end backend support.
- Runtime texture loading now fails closed with a warning when compressed artifacts are used without backend format capability; capable RHI devices receive the compressed format, mip count, upload data, and color-space intent.
- Reserved `BC6H`, `ASTC4x4`, and `ETC2_RGBA8` descriptors exist only as extension points with unsupported default capabilities. Resolver tests verify Vulkan/reserved-format diagnostics and RGBA8 fallback behavior, but no Vulkan, ASTC, ETC2, or BC6H runtime support is claimed.
- Texture artifacts now preserve target platform, build identity, encoder id/version, array-layer count, and cube face ordering metadata. `mipCount` means unique mip levels, while `subresourceCount` means stored subresources; cubemap metadata round-trips through serialization, but `TextureLoader` runtime cubemap compressed loading is not enabled by this feature slice.
- DirectXTex is pinned as an optional Windows editor/tool-time dependency (`jul2025`, MIT, `DirectXTex::DirectXTex`) with an unavailable stub when the vendored source is absent. When `ThirdParty/DirectXTex/src/DirectXTex/DirectXTex.h` is present, the editor encoder uses DirectXTex `DirectX::Image` source views + `Compress` to produce BC1/BC3/BC5/BC7 `.ntex` mip payloads.
- DirectXTex packaging metadata now records the pinned commit (`32b2a8e`) in `.gitmodules`, `ThirdParty/DirectXTex/README.md`, and `ThirdParty/DirectXTex/LICENSE.metadata`; CMake verifies the nested source checkout commit when its `.git` directory is present.
- The DirectXTex encoder still allocates DirectXTex compressed output scratch storage, but it no longer deep-copies the source RGBA mip chain into the encoder request or an intermediate DirectXTex source `ScratchImage`.
- Encoded HDR image import is not yet verified. The current HDR coverage is limited to half-float RGBA16F mip generation from already-decoded half-float texels.
- Representative import corpus for SC-001 currently consists of the existing automated OBJ/GLTF texture import samples covered by `AssetImportPipelineTests.*Texture*`; these passed in the 69-test focused regression. No external large texture corpus or Windows/DX12 RenderDoc runtime capture has been run in this checkpoint.

## Review Evidence

- 2026-05-30: Multi-agent plan-review R1 completed across architecture/performance, GPU/RHI correctness, code-quality/SSoT/schema, and industry benchmarking. Reported P1 findings were fixed: encoder/format compatibility, support-claim wording, artifact build metadata wiring, mip/subresource schema semantics, contiguous mip validation, DX12 command-buffer row-pitch fail-closed behavior, invalid SRV handle fail-closed behavior, 3D upload slice stepping, sRGB compressed capability validation, and SSoT format names.
- 2026-05-30: Deeper audit R2 found the remaining full-mip-chain CPU repacking issue and a newly introduced subresource-span `dataSize` regression. Both were fixed, with `DX12TextureUploadUtilsTests.BuildsInitialTextureUploadRequestFromSubresourceSpans` added to cover the new path. No open P0/P1 code findings remain in the current review scope.
- 2026-05-31: Multi-agent deeper audit R3 found P1 issues in DirectXTex source mip copying, external texture size bounds, optional DirectXTex fallback coverage, texture postprocessor identity, dependency packaging, DX12 3D upload row counts, `CopyBufferToTexture` validation, and `.ntex` fail-open behavior. These were fixed and revalidated with the 72-test focused regression plus targeted 4-test review-fix regression above.

## Scenario 1: RGBA8 mip chain

1. Import a 1024x1024 color texture with `mipmapEnabled=true` and `compressionIntent=uncompressed`.
2. Decode the generated `.ntex`.
3. Verify 11 mip levels and final `1x1` level.
4. Verify artifact format is `RGBA8`.

Expected result: import succeeds, build identity is stable across reimport, and runtime loader accepts the artifact.

## Scenario 2: Normal map default compression

1. Import a normal map with `textureType=normal`, `mipmapEnabled=true`, `compressionIntent=default`, target `win64-dx12`.
2. Resolve build settings without running renderer.
3. Verify resolved format is `BC5` when supported.
4. Verify generated mips are normal-map aware.

Expected result: import succeeds with BC5 or falls back to RGBA8 only with an explicit diagnostic.

## Scenario 3: Alpha color texture

1. Import a color texture with alpha and `compressionIntent=high-quality`.
2. Resolve for `win64-dx12`.
3. Verify resolved format is `BC7` when encoder/backend support is present.
4. Verify fallback chain records `RGBA8` if encoder support is disabled.

Expected result: no silent fallback; diagnostics name requested/resolved formats.

## Scenario 4: Tiny or block-alignment-limited compressed texture

1. Import a 2x2 texture with explicit `format=bc1`.
2. Build the artifact.
3. Verify BC row/slice pitch uses ceil block counts.
4. Verify importer selects RGBA8 fallback only when DX12 capability/policy disallows the top-level dimensions, or pads/encodes only if the selected policy explicitly allows it.

Expected result: artifact is valid and diagnostics explain the capability, fallback, or padding policy.

## Scenario 5: Future Runtime Compressed Upload on Windows/DX12

1. Load generated BC1, BC3, BC5, and BC7 `.ntex` artifacts through `TextureLoader`.
2. Verify `RHITextureDesc.format` matches artifact format, `mipLevels` matches artifact metadata, and color-space intent reaches `RHITextureDesc` or `RHITextureViewDesc`.
3. Verify DX12 upload planning computes row/slice pitches from block counts.
4. Verify sRGB color artifacts create compatible DX12 SRV/resource formats and BC5 remains linear.
5. Capture runtime evidence on Windows/DX12 before claiming support.

Expected result: compressed artifacts upload without reinterpretation. Unsupported backends report unsupported-format diagnostics.

## Scenario 6: HDR fallback

1. Import an HDR source with default compression for `win64-dx12`.
2. Resolve build settings without BC6H enabled.
3. Verify resolved format is `RGBA16F`, not `RGBA8`, unless the user explicitly allows range-losing fallback with a diagnostic.

Expected result: HDR range is preserved in first scope, and BC6H remains a reserved extension.

## Scenario 7: Reserved platform behavior

1. Configure `targetPlatform=vulkan` or request `astc`.
2. Resolve build settings.
3. Verify the system emits reserved/unsupported diagnostics and uses RGBA8 fallback where runtime viability requires it.

Expected result: no Vulkan/ASTC/ETC2 support claim is made.
