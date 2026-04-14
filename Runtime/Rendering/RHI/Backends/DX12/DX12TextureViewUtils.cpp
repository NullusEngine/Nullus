#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    namespace
    {
        DXGI_FORMAT ToDxgiFormat(const TextureFormat format)
        {
            switch (format)
            {
            case TextureFormat::RGB8:
            case TextureFormat::RGBA8:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            case TextureFormat::RGBA16F:
                return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case TextureFormat::Depth24Stencil8:
                return DXGI_FORMAT_D24_UNORM_S8_UINT;
            default:
                return DXGI_FORMAT_R8G8B8A8_UNORM;
            }
        }
    }

    DX12TextureViewDescriptorSet BuildDX12TextureViewDescriptorSet(
        const RHITextureDesc& textureDesc,
        const RHITextureViewDesc& viewDesc)
    {
        DX12TextureViewDescriptorSet descriptors;
        const bool isDepth = viewDesc.format == TextureFormat::Depth24Stencil8;
        const bool isCube =
            viewDesc.viewType == TextureViewType::Cube ||
            textureDesc.dimension == TextureDimension::TextureCube;
        const UINT mipLevels = viewDesc.subresourceRange.mipLevelCount > 0
            ? viewDesc.subresourceRange.mipLevelCount
            : 1u;

        descriptors.hasSrv = !isDepth;
        if (descriptors.hasSrv)
        {
            descriptors.srvDesc.Format = ToDxgiFormat(viewDesc.format);
            descriptors.srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (isCube)
            {
                descriptors.srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                descriptors.srvDesc.TextureCube.MipLevels = mipLevels;
                descriptors.srvDesc.TextureCube.MostDetailedMip = viewDesc.subresourceRange.baseMipLevel;
                descriptors.srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
            }
            else
            {
                descriptors.srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                descriptors.srvDesc.Texture2D.MipLevels = mipLevels;
                descriptors.srvDesc.Texture2D.MostDetailedMip = viewDesc.subresourceRange.baseMipLevel;
                descriptors.srvDesc.Texture2D.PlaneSlice = 0;
                descriptors.srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            }
        }

        descriptors.hasRtv =
            !isDepth &&
            HasTextureUsage(textureDesc.usage, TextureUsageFlags::ColorAttachment);
        if (descriptors.hasRtv)
        {
            descriptors.rtvDesc.Format = ToDxgiFormat(viewDesc.format);
            if (isCube)
            {
                descriptors.rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                descriptors.rtvDesc.Texture2DArray.MipSlice = viewDesc.subresourceRange.baseMipLevel;
                descriptors.rtvDesc.Texture2DArray.FirstArraySlice = viewDesc.subresourceRange.baseArrayLayer;
                descriptors.rtvDesc.Texture2DArray.ArraySize =
                    viewDesc.subresourceRange.arrayLayerCount > 0
                    ? viewDesc.subresourceRange.arrayLayerCount
                    : 1u;
                descriptors.rtvDesc.Texture2DArray.PlaneSlice = 0;
            }
            else
            {
                descriptors.rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                descriptors.rtvDesc.Texture2D.MipSlice = viewDesc.subresourceRange.baseMipLevel;
                descriptors.rtvDesc.Texture2D.PlaneSlice = 0;
            }
        }

        descriptors.hasDsv =
            isDepth &&
            HasTextureUsage(textureDesc.usage, TextureUsageFlags::DepthStencilAttachment);
        if (descriptors.hasDsv)
        {
            descriptors.dsvDesc.Format = ToDxgiFormat(viewDesc.format);
            if (isCube)
            {
                descriptors.dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                descriptors.dsvDesc.Texture2DArray.MipSlice = viewDesc.subresourceRange.baseMipLevel;
                descriptors.dsvDesc.Texture2DArray.FirstArraySlice = viewDesc.subresourceRange.baseArrayLayer;
                descriptors.dsvDesc.Texture2DArray.ArraySize =
                    viewDesc.subresourceRange.arrayLayerCount > 0
                    ? viewDesc.subresourceRange.arrayLayerCount
                    : 1u;
            }
            else
            {
                descriptors.dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                descriptors.dsvDesc.Texture2D.MipSlice = viewDesc.subresourceRange.baseMipLevel;
            }
        }

        return descriptors;
    }
#endif
}
