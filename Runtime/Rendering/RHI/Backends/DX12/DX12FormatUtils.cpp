#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    DXGI_FORMAT ToDXGIFormat(TextureFormat format)
    {
        switch (format)
        {
        case TextureFormat::R8: return DXGI_FORMAT_R8_UNORM;
        case TextureFormat::RG8: return DXGI_FORMAT_R8G8_UNORM;
        case TextureFormat::RGB8:
        case TextureFormat::RGBA8: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::R16F: return DXGI_FORMAT_R16_FLOAT;
        case TextureFormat::RG16F: return DXGI_FORMAT_R16G16_FLOAT;
        case TextureFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::R32F: return DXGI_FORMAT_R32_FLOAT;
        case TextureFormat::RG32F: return DXGI_FORMAT_R32G32_FLOAT;
        case TextureFormat::RGBA32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::Depth32F: return DXGI_FORMAT_D32_FLOAT;
        case TextureFormat::Depth24Stencil8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
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
        default:
            return 0u;
        }
    }

    bool IsDepthStencilFormat(TextureFormat format)
    {
        return format == TextureFormat::Depth24Stencil8 || format == TextureFormat::Depth32F;
    }
#endif
}
