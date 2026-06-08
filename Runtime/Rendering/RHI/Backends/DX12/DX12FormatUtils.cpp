#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    namespace
    {
        bool SupportsRuntimeInitialUpload(const TextureFormat format)
        {
            switch (format)
            {
            case TextureFormat::BC1:
            case TextureFormat::BC3:
            case TextureFormat::BC5:
            case TextureFormat::BC7:
                return true;
            case TextureFormat::BC6H:
            case TextureFormat::ASTC4x4:
            case TextureFormat::ETC2RGBA8:
                return false;
            default:
                return !IsTextureFormatCompressed(format);
            }
        }
    }

    DXGI_FORMAT ToDXGIFormat(TextureFormat format, TextureColorSpace colorSpace)
    {
        switch (format)
        {
        case TextureFormat::R8: return DXGI_FORMAT_R8_UNORM;
        case TextureFormat::RG8: return DXGI_FORMAT_R8G8_UNORM;
        case TextureFormat::RGB8:
        case TextureFormat::RGBA8:
            return colorSpace == TextureColorSpace::SRGB
                ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                : DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::R16F: return DXGI_FORMAT_R16_FLOAT;
        case TextureFormat::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::R32F: return DXGI_FORMAT_R32_FLOAT;
        case TextureFormat::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case TextureFormat::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::BC1:
            return colorSpace == TextureColorSpace::SRGB
                ? DXGI_FORMAT_BC1_UNORM_SRGB
                : DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC3:
            return colorSpace == TextureColorSpace::SRGB
                ? DXGI_FORMAT_BC3_UNORM_SRGB
                : DXGI_FORMAT_BC3_UNORM;
        case TextureFormat::BC5:
            return colorSpace == TextureColorSpace::SRGB
                ? DXGI_FORMAT_UNKNOWN
                : DXGI_FORMAT_BC5_UNORM;
        case TextureFormat::BC7:
            return colorSpace == TextureColorSpace::SRGB
                ? DXGI_FORMAT_BC7_UNORM_SRGB
                : DXGI_FORMAT_BC7_UNORM;
        case TextureFormat::Depth32F: return DXGI_FORMAT_D32_FLOAT;
        case TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    DXGI_FORMAT ToDX12ResourceFormat(TextureFormat format, TextureColorSpace colorSpace)
    {
        switch (format)
        {
        case TextureFormat::Depth32F: return DXGI_FORMAT_R32_TYPELESS;
        case TextureFormat::Depth24Stencil8: return DXGI_FORMAT_R24G8_TYPELESS;
        default: return ToDXGIFormat(format, colorSpace);
        }
    }

    DXGI_FORMAT ToDX12OptimizedClearFormat(TextureFormat format, TextureColorSpace colorSpace)
    {
        switch (format)
        {
        case TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default: return ToDXGIFormat(format, colorSpace);
        }
    }

    uint32_t GetDXGIFormatBytesPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
            return 1u;
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
            return 2u;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return 4u;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
            return 8u;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return 16u;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 0u;
        default:
            return 0u;
        }
    }

    bool IsDepthStencilFormat(TextureFormat format)
    {
        return format == TextureFormat::Depth24Stencil8 || format == TextureFormat::Depth32F;
    }

    TextureFormatCapability BuildDX12TextureFormatCapability(
        TextureFormat format,
        D3D12_FORMAT_SUPPORT1 support1,
        D3D12_FORMAT_SUPPORT2 support2,
        const bool supportsSrgbView,
        std::string diagnosticReason)
    {
        (void)support2;

        TextureFormatCapability capability{};
        capability.format = format;
        const bool isDepthStencil = IsDepthStencilFormat(format);
        const bool supportsTexture2D = (support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0;
        capability.sampled =
            supportsTexture2D &&
            ((support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0 ||
                (isDepthStencil && (support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0));
        capability.upload =
            supportsTexture2D &&
            SupportsRuntimeInitialUpload(format);
        capability.colorAttachment = (support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) != 0;
        capability.storage = (support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
        capability.supportsSrgbView = supportsSrgbView;
        capability.requiresAlignedTopLevelBlocks = IsTextureFormatCompressed(format);
        capability.supportsUnalignedBlockTextures = false;
        capability.diagnosticReason = (capability.sampled && capability.upload)
            ? std::string{}
            : std::move(diagnosticReason);
        return capability;
    }
#endif
}
