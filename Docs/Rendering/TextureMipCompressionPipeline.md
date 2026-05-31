# Texture Mip And Compression Pipeline

This document records the validation boundary for the texture mipmap and compression rollout.

## Supported First Scope

- Offline artifact schema supports descriptor-driven RGBA8, RGBA16F, BC1, BC3, BC5, and BC7 payloads.
- When the pinned DirectXTex source is available, the Windows editor/tool-time `directxtex-bc` encoder has unit-level evidence for BC1, BC3, BC5, and BC7 artifact payloads.
- Texture artifacts preserve build metadata: target platform, build identity, encoder id/version, array layers, and cube face ordering metadata.
- Windows/DX12 unit coverage validates format descriptors, sRGB view selection, block-compressed upload planning, compressed row copies, and RHI metadata propagation through `TextureLoader`.
- Runtime loading must require backend format capability for compressed artifacts. If capability is missing, loading fails closed and logs an unsupported compressed texture artifact warning.

## Reserved Extension Points

- BC6H, ASTC4x4, and ETC2 RGBA8 are descriptor/registry extension points only.
- Vulkan, ASTC, ETC2, BC6H, cubemap runtime loading, and streaming mips require separate backend or encoder evidence before support can be claimed.
- Unit tests do not replace a Windows/DX12 runtime capture for final compressed texture support claims.

## Focused Validation

Use this suite for the current texture pipeline checkpoint:

```powershell
Build\bin\Debug\NullusUnitTests.exe --gtest_filter="AssetImporterFacadeTests.TextureImporterSettings*:AssetImportPipelineTests.*Texture*:AssetMaterialConversionTests.*Texture*:DX12TextureUploadUtilsTests.*:DX12FormatUtilsTests.*"
```

Before claiming real backend support, also capture runtime evidence on Windows/DX12 for RGBA8, RGBA16F, BC1, BC3, BC5, and BC7 artifact loading.
