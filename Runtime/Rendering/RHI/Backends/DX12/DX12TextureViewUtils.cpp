#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"

#include <algorithm>

#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    namespace
    {
        UINT ResolveMipLevels(const RHITextureViewDesc& viewDesc)
        {
            return viewDesc.subresourceRange.mipLevelCount > 0
                ? viewDesc.subresourceRange.mipLevelCount
                : 1u;
        }

        UINT ResolveArraySize(const RHITextureDesc& textureDesc, const RHITextureViewDesc& viewDesc)
        {
            if (viewDesc.subresourceRange.arrayLayerCount > 0u)
                return viewDesc.subresourceRange.arrayLayerCount;
            return (std::max)(textureDesc.arrayLayers, 1u);
        }

        TextureViewType ResolveViewType(const RHITextureDesc& textureDesc, const RHITextureViewDesc& viewDesc)
        {
            if (viewDesc.viewType != TextureViewType::Auto)
                return viewDesc.viewType;

            switch (textureDesc.dimension)
            {
            case TextureDimension::Texture2DArray: return TextureViewType::Texture2DArray;
            case TextureDimension::TextureCube: return TextureViewType::Cube;
            case TextureDimension::TextureCubeArray: return TextureViewType::CubeArray;
            case TextureDimension::Texture3D: return TextureViewType::Texture3D;
            case TextureDimension::Texture1D:
            case TextureDimension::Texture2D:
            default:
                return TextureViewType::Texture2D;
            }
        }
    }

    DX12TextureViewDescriptorSet BuildDX12TextureViewDescriptorSet(
        const RHITextureDesc& textureDesc,
        const RHITextureViewDesc& viewDesc)
    {
        DX12TextureViewDescriptorSet descriptors;
        const bool isDepth = IsDepthStencilFormat(viewDesc.format);
        const TextureViewType resolvedViewType = ResolveViewType(textureDesc, viewDesc);
        const bool isCube = resolvedViewType == TextureViewType::Cube;
        const bool isCubeArray = resolvedViewType == TextureViewType::CubeArray;
        const bool is2DArray = resolvedViewType == TextureViewType::Texture2DArray;
        const bool is3D = resolvedViewType == TextureViewType::Texture3D;
        const UINT mipLevels = ResolveMipLevels(viewDesc);
        const UINT arraySize = ResolveArraySize(textureDesc, viewDesc);

        descriptors.hasSrv = !isDepth;
        if (descriptors.hasSrv)
        {
            descriptors.srvDesc.Format = ToDXGIFormat(viewDesc.format);
            descriptors.srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (isCube)
            {
                descriptors.srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                descriptors.srvDesc.TextureCube.MipLevels = mipLevels;
                descriptors.srvDesc.TextureCube.MostDetailedMip = viewDesc.subresourceRange.baseMipLevel;
                descriptors.srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
            }
            else if (isCubeArray)
            {
                descriptors.srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                descriptors.srvDesc.TextureCubeArray.MipLevels = mipLevels;
                descriptors.srvDesc.TextureCubeArray.MostDetailedMip = viewDesc.subresourceRange.baseMipLevel;
                descriptors.srvDesc.TextureCubeArray.First2DArrayFace = viewDesc.subresourceRange.baseArrayLayer;
                descriptors.srvDesc.TextureCubeArray.NumCubes = (std::max)(arraySize / 6u, 1u);
                descriptors.srvDesc.TextureCubeArray.ResourceMinLODClamp = 0.0f;
            }
            else if (is2DArray)
            {
                descriptors.srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                descriptors.srvDesc.Texture2DArray.MipLevels = mipLevels;
                descriptors.srvDesc.Texture2DArray.MostDetailedMip = viewDesc.subresourceRange.baseMipLevel;
                descriptors.srvDesc.Texture2DArray.FirstArraySlice = viewDesc.subresourceRange.baseArrayLayer;
                descriptors.srvDesc.Texture2DArray.ArraySize = arraySize;
                descriptors.srvDesc.Texture2DArray.PlaneSlice = 0;
                descriptors.srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
            }
            else if (is3D)
            {
                descriptors.srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                descriptors.srvDesc.Texture3D.MipLevels = mipLevels;
                descriptors.srvDesc.Texture3D.MostDetailedMip = viewDesc.subresourceRange.baseMipLevel;
                descriptors.srvDesc.Texture3D.ResourceMinLODClamp = 0.0f;
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
            descriptors.rtvDesc.Format = ToDXGIFormat(viewDesc.format);
            if (isCube || isCubeArray || is2DArray)
            {
                descriptors.rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                descriptors.rtvDesc.Texture2DArray.MipSlice = viewDesc.subresourceRange.baseMipLevel;
                descriptors.rtvDesc.Texture2DArray.FirstArraySlice = viewDesc.subresourceRange.baseArrayLayer;
                descriptors.rtvDesc.Texture2DArray.ArraySize = arraySize;
                descriptors.rtvDesc.Texture2DArray.PlaneSlice = 0;
            }
            else if (is3D)
            {
                descriptors.rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                descriptors.rtvDesc.Texture3D.MipSlice = viewDesc.subresourceRange.baseMipLevel;
                descriptors.rtvDesc.Texture3D.FirstWSlice = viewDesc.subresourceRange.baseArrayLayer;
                descriptors.rtvDesc.Texture3D.WSize = arraySize;
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
            descriptors.dsvDesc.Format = ToDXGIFormat(viewDesc.format);
            if (isCube || isCubeArray || is2DArray)
            {
                descriptors.dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                descriptors.dsvDesc.Texture2DArray.MipSlice = viewDesc.subresourceRange.baseMipLevel;
                descriptors.dsvDesc.Texture2DArray.FirstArraySlice = viewDesc.subresourceRange.baseArrayLayer;
                descriptors.dsvDesc.Texture2DArray.ArraySize = arraySize;
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
