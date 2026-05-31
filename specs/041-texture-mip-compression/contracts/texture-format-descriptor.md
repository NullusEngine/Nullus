# Contract: Texture Format Descriptor

## Purpose

`TextureFormatDescriptor` is the single source of truth for texture layout calculations shared by artifact validation, importer output, runtime upload planning, and backend mappings.

## Descriptor Fields

- `TextureFormat format`
- `const char* name`
- `TextureFormatFamily family`
- `uint32_t blockWidth`
- `uint32_t blockHeight`
- `uint32_t blockDepth`
- `uint32_t bytesPerBlock`
- `uint32_t channelCount`
- `bool isCompressed`
- `bool hasAlpha`
- `bool isHDR`
- `bool isDepthStencil`
- `bool supportsSrgbView`
- `bool supportsUpload`
- `bool sampled`
- `bool colorAttachment`
- `bool storage`
- `bool requiresAlignedTopLevelBlocks`

## Required Formats For First Scope

| Format | Family | Block | Bytes/block | Notes |
|--------|--------|-------|-------------|-------|
| `RGBA8` | uncompressed | 1x1x1 | 4 | Universal fallback |
| `RGBA16F` | uncompressed | 1x1x1 | 8 | First-scope HDR fallback |
| `BC1` | bc | 4x4x1 | 8 | Opaque/1-bit alpha color |
| `BC3` | bc | 4x4x1 | 16 | Explicit alpha color |
| `BC5` | bc | 4x4x1 | 16 | Normal/two-channel masks |
| `BC7` | bc | 4x4x1 | 16 | High-quality color with alpha |

## Helper Functions

- `GetTextureFormatDescriptor(TextureFormat format)`
- `IsTextureFormatCompressed(TextureFormat format)`
- `CalculateTextureRowPitch(TextureFormat format, uint32_t width)`
- `CalculateTextureSlicePitch(TextureFormat format, uint32_t width, uint32_t height, uint32_t depth)`
- `CalculateTextureMipExtent(uint32_t width, uint32_t height, uint32_t depth, uint32_t level)`
- `GetTextureFormatBytesPerPixel(TextureFormat format)` must fail closed or be limited to uncompressed formats after the migration. Every import/upload/artifact size consumer must be audited and moved to descriptor pitch helpers when compressed formats can appear.

## DX12 Mapping

- `RGBA8` maps to `DXGI_FORMAT_R8G8B8A8_UNORM` or `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB`
- `RGBA16F` maps to `DXGI_FORMAT_R16G16B16A16_FLOAT`
- `BC1` maps to `DXGI_FORMAT_BC1_UNORM` or `DXGI_FORMAT_BC1_UNORM_SRGB`
- `BC3` maps to `DXGI_FORMAT_BC3_UNORM` or `DXGI_FORMAT_BC3_UNORM_SRGB`
- `BC5` maps to `DXGI_FORMAT_BC5_UNORM`
- `BC7` maps to `DXGI_FORMAT_BC7_UNORM` or `DXGI_FORMAT_BC7_UNORM_SRGB`

`BC5` remains linear because it is used for normal/two-channel data. The first scope keeps `TextureFormat` values non-sRGB and chooses sRGB DXGI variants from texture color-space metadata carried by `RHITextureDesc` or `RHITextureViewDesc`.

DX12 SRV/resource compatibility must be explicit:

- Resource creation and SRV creation must agree on a compatible DXGI format path for linear versus sRGB views.
- `DX12TextureViewUtils.cpp` must participate in sRGB SRV selection; mapping only in `DX12FormatUtils.cpp` is insufficient.
- Unsupported sRGB view requests fail closed with diagnostics instead of silently sampling as linear.

## Backend Capability Contract

Descriptor data says how a format is laid out; backend capability data says whether that format can be used on a device. The first implementation must add a per-format DX12 capability table/query for RGBA8, RGBA16F, BC1, BC3, BC5, and BC7, populated from `D3D12_FEATURE_FORMAT_SUPPORT` where available.
