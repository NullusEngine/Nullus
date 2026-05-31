# Data Model: Texture Mipmap And Compression Pipeline

## TextureImporterSettings

User-facing editor settings, currently rooted in `Project/Editor/Assets/AssetImporterSettings.h`.

**Fields**:

- `textureType`: `default`, `normal`, `mask`, `ui`, `hdr`
- `srgbTexture`: whether color sampling should use sRGB intent
- `alphaIsTransparency`: whether alpha coverage should be preserved during mip generation
- `mipmapEnabled`: whether to generate mips beyond level 0
- `wrapMode`, `filterMode`: existing sampler defaults
- `maxTextureSize`: target maximum size before mip generation
- `resizePolicy`: `keep`, `scale-down`, `nearest-power-of-two`, `power-of-two`
- `compressionIntent`: `default`, `uncompressed`, `low-quality`, `normal-quality`, `high-quality`
- `explicitFormat`: optional requested output format string
- `platformOverrides`: list of `TexturePlatformOverride`

**Validation**:

- Unknown `textureType` resolves to `default` with a diagnostic.
- Unknown `compressionIntent` resolves to `default` with a diagnostic.
- `maxTextureSize == 0` means use project/platform default.
- Platform overrides are sorted by platform name for deterministic serialization.

## TexturePlatformOverride

Per-platform import override.

**Fields**:

- `platform`: canonical target name, initially `win64-dx12`
- `maxTextureSize`
- `resizePolicy`
- `format`: optional explicit format, such as `rgba8`, `bc1`, `bc3`, `bc5`, `bc7`
- `compressionQuality`: `low`, `normal`, `high`
- `mipmapEnabled`: optional override

**Validation**:

- Empty `platform` entries are ignored during serialization.
- Unsupported explicit formats produce diagnostics and fall back to the default resolver.

## TextureBuildSettings

Resolved immutable build settings used to produce an artifact.

**Fields**:

- `sourcePath`
- `sourceAssetGuid`
- `sourceContentHash`
- `importerVersion`
- `postprocessorVersion`
- `dependencyHash`
- `sourceWidth`, `sourceHeight`, `sourceWasHDR`, `sourceHasAlpha`
- `targetPlatform`: initially `win64-dx12`
- `textureIntent`
- `colorSpace`
- `mipmapEnabled`
- `maxTextureSize`
- `resizePolicy`
- `resolvedFormat`
- `compressionQuality`
- `encoderId`
- `encoderVersion`
- `encoderOptionsHash`
- `toolVersions`: encoder/toolchain versions that can alter output bytes, including DirectXTex for BC outputs
- `artifactSchemaVersion`
- `buildIdentity`

**Validation**:

- `resolvedFormat` must exist in `TextureFormatDescriptor`.
- BC formats require mip payloads whose row/slice pitches match block-compressed rules.
- `buildIdentity` must be stable for unchanged source/settings/platform/encoder version and must change when source identity, source content, importer logic, postprocessor logic, dependency hashes, normalized override ordering, encoder options, tool versions, or artifact schema version change.

## TextureFormatDescriptor

Shared RHI format metadata.

**Fields**:

- `format`: `TextureFormat`
- `name`: stable lowercase string
- `colorSpace`: `linear` or `srgb` runtime sampling intent
- `family`: `uncompressed`, `bc`, `depth-stencil`
- `blockWidth`, `blockHeight`, `blockDepth`
- `bytesPerBlock`
- `channels`
- `hasAlpha`
- `isHDR`
- `isCompressed`
- `isSampled`
- `isRenderTargetCapable`
- `dxgiFormat`: available in DX12 mapping only
- `dxgiSrgbFormat`: available in DX12 mapping when the format has an sRGB sampling variant

**Validation**:

- Uncompressed normalized formats use block size `1x1x1`.
- BC1 has 4x4 blocks and 8 bytes per block.
- BC3, BC5, and BC7 have 4x4 blocks and 16 bytes per block.
- Depth/stencil formats are not valid import artifact outputs.
- sRGB artifacts must select an sRGB-capable DX12 resource or SRV format when available.

## TextureBackendFormatCapability

Per-backend support record used by the resolver before it selects a compressed output.

**Fields**:

- `backend`: initially `dx12`
- `format`
- `sampled`
- `upload`
- `colorAttachment`
- `storage`
- `supportsSrgbView`
- `requiresAlignedTopLevelBlocks`
- `supportsUnalignedBlockTextures`
- `diagnosticReason`

**Validation**:

- Windows/DX12 capabilities must be populated from `D3D12_FEATURE_FORMAT_SUPPORT` or an explicit unavailable diagnostic.
- Resolver decisions must use capability records rather than assuming a format is usable because a DXGI enum exists.
- BC output requires sampled/upload support and compatible top-level alignment policy.

## TextureColorSpaceIntent

Runtime color-space carrier that keeps artifact metadata enforceable after loading.

**Fields**:

- `colorSpace`: `linear` or `srgb`
- `appliesToView`: whether SRV format selection should use sRGB variants

**Validation**:

- `RHITextureDesc` or `RHITextureViewDesc` must carry this intent.
- DX12 SRV creation must map `RGBA8`, `BC1`, `BC3`, and `BC7` to sRGB variants when `colorSpace=srgb` and the descriptor supports sRGB views.
- `BC5` and other non-color data formats must remain linear even when the source asset has unrelated editor metadata.

## TextureMipChain

Build-time collection of uncompressed processed mips before final encoding.

**Fields**:

- `baseWidth`, `baseHeight`
- `intent`
- `colorSpace`
- `mips`: ordered list of `TextureSourceMip`

**Validation**:

- Level 0 dimensions equal processed base dimensions.
- Each next mip dimension is `max(previous / 2, 1)`.
- Full chain ends at `1x1` unless mip generation is disabled.

## TextureArtifact

Native serialized texture payload in the existing `NLSA` container.

**Fields**:

- `width`, `height`, `depth`
- `dimension`
- `arrayLayers`
- `format`
- `colorSpace`
- `mipCount`
- `buildIdentity`
- `encoderId`
- `encoderVersion`
- `targetPlatform`
- `subresources`: ordered list of `TextureArtifactSubresource`

**Validation**:

- Metadata format must be supported by artifact schema.
- Subresource records must match expected mip dimensions and layer/face ordering.
- `rowPitch` and `slicePitch` must match `TextureFormatDescriptor`.
- Payload size must equal sum of subresource `slicePitch` for 2D layer-0 textures in first scope.

## TextureArtifactSubresource

Serialized payload metadata for one mip/layer/face slice.

**Fields**:

- `level`
- `arrayLayer`
- `face`: `none`, `positive-x`, `negative-x`, `positive-y`, `negative-y`, `positive-z`, `negative-z`
- `width`, `height`, `depth`
- `rowPitch`
- `slicePitch`
- `dataOffset`
- `dataSize`
- `pixels`

**Validation**:

- First-scope 2D artifacts write exactly one subresource per mip with `arrayLayer=0` and `face=none`.
- Cubemaps use six faces per mip in the documented face order; texture arrays use ascending `arrayLayer`.
- `dataSize == slicePitch` for first-scope 2D subresources.
- Data ranges must stay within container payload bounds.
- Block-compressed pitch is `ceil(width / blockWidth) * bytesPerBlock`.

## TextureEncoder

Build-time encoder interface.

**Fields/Methods**:

- `id`
- `version`
- `supportedFormats`
- `Encode(TextureMipChain, TextureBuildSettings) -> TextureArtifactData or diagnostic`

**Validation**:

- Encoder output format equals `TextureBuildSettings.resolvedFormat`.
- Encoder output mips preserve order and dimensions.
- Encoder failures return diagnostics; importer decides fallback.

## TextureLoadDiagnostic

Structured diagnostic surfaced from import/build/load/upload.

**Fields**:

- `assetPath`
- `targetPlatform`
- `requestedFormat`
- `resolvedFormat`
- `stage`: `settings`, `decode`, `mip-generation`, `format-resolution`, `encoding`, `artifact`, `runtime-upload`
- `severity`: `info`, `warning`, `error`
- `message`
- `fallbackFormat`

**Validation**:

- Unsupported runtime formats use `error`.
- Import-time fallback uses `warning` unless no valid fallback exists.
