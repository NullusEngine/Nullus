# Contract: Texture Encoder

## Purpose

Texture encoders convert processed uncompressed mip chains into artifact-ready payloads. The importer owns fallback policy; encoders only report success or diagnostic failure.

## Interface

```cpp
struct TextureEncodeRequest
{
    TextureBuildSettings buildSettings;
    TextureMipChain sourceMips;
};

struct TextureEncodeResult
{
    bool succeeded;
    TextureArtifactData artifact;
    std::vector<TextureLoadDiagnostic> diagnostics;
};

class ITextureEncoder
{
public:
    virtual std::string_view GetId() const = 0;
    virtual uint32_t GetVersion() const = 0;
    virtual bool SupportsFormat(RHI::TextureFormat format) const = 0;
    virtual TextureEncodeResult Encode(const TextureEncodeRequest& request) const = 0;
};
```

## Required Encoders

- `rgba8-passthrough`: deterministic encoder for RGBA8 artifacts.
- `directxtex-bc`: Windows editor/tool-time encoder facade for BC1, BC3, BC5, BC7 using DirectXTex.

The DirectXTex implementation must live in an optional Windows editor/tool target and must not be linked by runtime `NLS_Render`.

## DirectXTex Policy

The DirectXTex facade must derive flags/options from texture intent:

- sRGB color textures use sRGB-aware input/output options so filtering and BC output preserve color-space intent.
- Linear color textures avoid sRGB conversion.
- Normal maps, masks, and other non-color data use data-preserving compression options, including uniform channel weighting where RGB channels are independent data.
- BC5 normal-map output must not be encoded with color-space conversion.
- Encoder option bits participate in build identity and are exposed in diagnostics.

## Invariants

- Output format equals `request.buildSettings.resolvedFormat`.
- Output mip count equals requested mip count.
- Output mip dimensions equal source mip dimensions.
- Output pitches satisfy `TextureFormatDescriptor`.
- Encoder version changes when output bytes can change for the same input.
- DirectXTex version and encoder options participate in `TextureBuildSettings.buildIdentity`.
- DirectXTex source version, license metadata, and CMake target name must be pinned in `ThirdParty/DirectXTex/` or an equivalent dependency manifest.
- Non-Windows builds must compile without the DirectXTex facade and must report encoder-unavailable diagnostics instead of failing runtime rendering builds.

## Error Handling

An encoder must not silently choose a different output format. If it cannot encode the requested format, it returns `succeeded=false` and a diagnostic. The importer may retry with fallback settings.
