#include "Assets/TextureEncoding/DirectXTexTextureEncoder.h"
#include "Rendering/Assets/TextureBuildSettings.h"

#ifndef NLS_HAS_DIRECTXTEX
#define NLS_HAS_DIRECTXTEX 0
#endif

#ifndef NLS_DIRECTXTEX_VERSION
#define NLS_DIRECTXTEX_VERSION "unavailable"
#endif

#if NLS_HAS_DIRECTXTEX
#include <DirectXTex.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#endif

namespace NLS::Editor::Assets
{
namespace
{
#if NLS_HAS_DIRECTXTEX
DXGI_FORMAT ToDirectXTexSourceFormat(const NLS::Render::RHI::TextureFormat format)
{
    switch (format)
    {
    case NLS::Render::RHI::TextureFormat::RGBA8:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT ToDirectXTexOutputFormat(const NLS::Render::RHI::TextureFormat format)
{
    switch (format)
    {
    case NLS::Render::RHI::TextureFormat::BC1:
        return DXGI_FORMAT_BC1_UNORM;
    case NLS::Render::RHI::TextureFormat::BC3:
        return DXGI_FORMAT_BC3_UNORM;
    case NLS::Render::RHI::TextureFormat::BC5:
        return DXGI_FORMAT_BC5_UNORM;
    case NLS::Render::RHI::TextureFormat::BC7:
        return DXGI_FORMAT_BC7_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

NLS::Render::Assets::TextureArtifactColorSpace ToArtifactColorSpace(
    const NLS::Render::RHI::TextureColorSpace colorSpace)
{
    return colorSpace == NLS::Render::RHI::TextureColorSpace::SRGB
        ? NLS::Render::Assets::TextureArtifactColorSpace::Srgb
        : NLS::Render::Assets::TextureArtifactColorSpace::Linear;
}

DirectX::TEX_COMPRESS_FLAGS BuildCompressFlags(const NLS::Render::Assets::TextureEncodeRequest& request)
{
    using NLS::Render::RHI::TextureColorSpace;
    using NLS::Render::RHI::TextureFormat;

    uint32_t flags = DirectX::TEX_COMPRESS_PARALLEL;
    const auto& buildSettings = *request.buildSettings;
    if (buildSettings.colorSpace == TextureColorSpace::SRGB)
        flags |= DirectX::TEX_COMPRESS_SRGB;

    if (buildSettings.resolvedFormat == TextureFormat::BC5 ||
        buildSettings.textureIntent == "normal" ||
        buildSettings.textureIntent == "mask")
    {
        flags |= DirectX::TEX_COMPRESS_UNIFORM;
    }

    if (buildSettings.encoderOptionsHash.find("quick") != std::string::npos)
        flags |= DirectX::TEX_COMPRESS_BC7_QUICK;

    return static_cast<DirectX::TEX_COMPRESS_FLAGS>(flags);
}

bool IsSingleLayer2DTexture(const NLS::Render::Assets::TextureArtifactData& artifact)
{
    return artifact.depth == 1u &&
        artifact.arrayLayers == 1u &&
        artifact.dimension == NLS::Render::RHI::TextureDimension::Texture2D &&
        !artifact.mips.empty();
}

void AppendDiagnostic(
    NLS::Render::Assets::TextureEncodeResult& result,
    std::string message)
{
    result.diagnostics.push_back({ "encoding", std::move(message) });
}

std::vector<DirectX::Image> BuildSourceImageViews(
    const NLS::Render::Assets::TextureArtifactData& sourceMips,
    const DXGI_FORMAT sourceFormat)
{
    std::vector<DirectX::Image> images;
    images.reserve(sourceMips.mips.size());
    for (const auto& mip : sourceMips.mips)
    {
        DirectX::Image image{};
        image.width = mip.width;
        image.height = mip.height;
        image.format = sourceFormat;
        image.rowPitch = mip.rowPitch;
        image.slicePitch = mip.slicePitch;
        image.pixels = const_cast<uint8_t*>(mip.pixels.data());
        images.push_back(image);
    }

    return images;
}

DirectX::TexMetadata BuildSourceMetadata(
    const NLS::Render::Assets::TextureArtifactData& sourceMips,
    const DXGI_FORMAT sourceFormat)
{
    DirectX::TexMetadata metadata{};
    metadata.width = sourceMips.width;
    metadata.height = sourceMips.height;
    metadata.depth = 1u;
    metadata.arraySize = 1u;
    metadata.mipLevels = sourceMips.mips.size();
    metadata.format = sourceFormat;
    metadata.dimension = DirectX::TEX_DIMENSION_TEXTURE2D;
    return metadata;
}

bool CopyCompressedMips(
    const DirectX::ScratchImage& compressedImage,
    const NLS::Render::Assets::TextureEncodeRequest& request,
    NLS::Render::Assets::TextureEncodeResult& result)
{
    const auto& metadata = compressedImage.GetMetadata();
    const auto& sourceMips = *request.sourceMips;
    const auto& buildSettings = *request.buildSettings;
    if (metadata.mipLevels != sourceMips.mips.size() ||
        metadata.arraySize != 1u)
    {
        AppendDiagnostic(result, "DirectXTex returned unexpected compressed texture topology");
        return false;
    }

    result.artifact = {};
    result.artifact.width = sourceMips.width;
    result.artifact.height = sourceMips.height;
    result.artifact.depth = 1u;
    result.artifact.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    result.artifact.arrayLayers = 1u;
    result.artifact.format = buildSettings.resolvedFormat;
    result.artifact.colorSpace = ToArtifactColorSpace(buildSettings.colorSpace);
    result.artifact.targetPlatform = buildSettings.targetPlatform;
    result.artifact.buildIdentity = NLS::Render::Assets::BuildTextureBuildIdentity(buildSettings);
    result.artifact.encoderId = std::string("directxtex-bc");
    result.artifact.encoderVersion = buildSettings.encoderVersion;
    result.artifact.mips.reserve(sourceMips.mips.size());
    result.artifact.subresources.reserve(sourceMips.mips.size());

    uint64_t dataOffset = 0u;
    for (size_t mipIndex = 0u; mipIndex < sourceMips.mips.size(); ++mipIndex)
    {
        const auto& sourceMip = sourceMips.mips[mipIndex];
        const auto* image = compressedImage.GetImage(sourceMip.level, 0u, 0u);
        if (image == nullptr || image->pixels == nullptr)
        {
            AppendDiagnostic(result, "DirectXTex did not return a compressed mip image");
            return false;
        }

        const uint32_t expectedRowPitch = NLS::Render::RHI::CalculateTextureRowPitch(
            result.artifact.format,
            sourceMip.width);
        const uint32_t expectedSlicePitch = NLS::Render::RHI::CalculateTextureSlicePitch(
            result.artifact.format,
            sourceMip.width,
            sourceMip.height,
            1u);
        if (expectedRowPitch == 0u ||
            expectedSlicePitch == 0u ||
            image->rowPitch < expectedRowPitch ||
            image->slicePitch < expectedSlicePitch)
        {
            AppendDiagnostic(result, "DirectXTex compressed mip pitch is smaller than Nullus format descriptor pitch");
            return false;
        }

        std::vector<uint8_t> pixels(expectedSlicePitch);
        const uint32_t blockRows = (std::max)(1u, (sourceMip.height + 3u) / 4u);
        for (uint32_t row = 0u; row < blockRows; ++row)
        {
            std::memcpy(
                pixels.data() + static_cast<size_t>(row) * expectedRowPitch,
                image->pixels + static_cast<size_t>(row) * image->rowPitch,
                expectedRowPitch);
        }

        result.artifact.mips.push_back({
            sourceMip.level,
            sourceMip.width,
            sourceMip.height,
            expectedRowPitch,
            expectedSlicePitch,
            std::move(pixels)
        });
        result.artifact.subresources.push_back({
            sourceMip.level,
            0u,
            sourceMip.width,
            sourceMip.height,
            1u,
            NLS::Render::Assets::TextureArtifactCubeFace::None,
            expectedRowPitch,
            expectedSlicePitch,
            dataOffset,
            expectedSlicePitch
        });
        dataOffset += expectedSlicePitch;
    }

    return true;
}
#endif

class DirectXTexTextureEncoder final : public NLS::Render::Assets::ITextureEncoder
{
public:
    std::string_view GetId() const override { return "directxtex-bc"; }
    uint32_t GetVersion() const override { return kDirectXTexTextureEncoderVersion; }

    bool SupportsFormat(const NLS::Render::RHI::TextureFormat format) const override
    {
        return format == NLS::Render::RHI::TextureFormat::BC1 ||
            format == NLS::Render::RHI::TextureFormat::BC3 ||
            format == NLS::Render::RHI::TextureFormat::BC5 ||
            format == NLS::Render::RHI::TextureFormat::BC7;
    }

    NLS::Render::Assets::TextureEncodeResult Encode(const NLS::Render::Assets::TextureEncodeRequest& request) const override
    {
        NLS::Render::Assets::TextureEncodeResult result;
        if (request.buildSettings == nullptr || request.sourceMips == nullptr)
        {
            result.diagnostics.push_back({
                "encoding",
                "directxtex-bc requires non-null build settings and source mips"
            });
            return result;
        }

        const auto& buildSettings = *request.buildSettings;
        const auto& sourceMips = *request.sourceMips;
        if (!SupportsFormat(buildSettings.resolvedFormat))
        {
            result.diagnostics.push_back({
                "encoding",
                "directxtex-bc does not support requested texture format"
            });
            return result;
        }

#if NLS_HAS_DIRECTXTEX
        if (!IsSingleLayer2DTexture(sourceMips))
        {
            AppendDiagnostic(result, "directxtex-bc only supports single-layer 2D texture artifacts in this encoder slice");
            return result;
        }

        const DXGI_FORMAT sourceFormat = ToDirectXTexSourceFormat(sourceMips.format);
        const DXGI_FORMAT outputFormat = ToDirectXTexOutputFormat(buildSettings.resolvedFormat);
        if (sourceFormat == DXGI_FORMAT_UNKNOWN || outputFormat == DXGI_FORMAT_UNKNOWN)
        {
            AppendDiagnostic(result, "directxtex-bc requires RGBA8 source mips and BC1/BC3/BC5/BC7 output format");
            return result;
        }

        const auto sourceImages = BuildSourceImageViews(sourceMips, sourceFormat);
        const auto sourceMetadata = BuildSourceMetadata(sourceMips, sourceFormat);

        DirectX::ScratchImage compressedImage;
        const HRESULT compressResult = DirectX::Compress(
            sourceImages.data(),
            sourceImages.size(),
            sourceMetadata,
            outputFormat,
            BuildCompressFlags(request),
            DirectX::TEX_THRESHOLD_DEFAULT,
            compressedImage);
        if (FAILED(compressResult))
        {
            AppendDiagnostic(result, "DirectXTex failed to compress texture mip chain");
            return result;
        }

        if (!CopyCompressedMips(compressedImage, request, result))
            return result;

        result.succeeded = true;
#else
        result.diagnostics.push_back({
            "encoding",
            "DirectXTex jul2025 optional dependency is unavailable; BC encoding is disabled for this editor build"
        });
#endif
        return result;
    }
};
}

const char* GetDirectXTexTextureEncoderToolVersion()
{
    return "directxtex:" NLS_DIRECTXTEX_VERSION;
}

std::shared_ptr<NLS::Render::Assets::ITextureEncoder> CreateDirectXTexTextureEncoder()
{
    return std::make_shared<DirectXTexTextureEncoder>();
}
}
