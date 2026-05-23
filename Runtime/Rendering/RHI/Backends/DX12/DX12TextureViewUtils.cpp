#include "Rendering/RHI/Backends/DX12/DX12TextureViewUtils.h"

#include <algorithm>

#include "Rendering/RHI/Backends/DX12/DX12FormatUtils.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    namespace
    {
        DXGI_FORMAT ResolveSrvFormat(const TextureFormat format)
        {
            switch (format)
            {
            case TextureFormat::Depth24Stencil8:
                return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case TextureFormat::Depth32F:
                return DXGI_FORMAT_R32_FLOAT;
            default:
                return ToDXGIFormat(format);
            }
        }

        DXGI_FORMAT ResolveDsvFormat(const TextureFormat format)
        {
            switch (format)
            {
            case TextureFormat::Depth24Stencil8:
                return DXGI_FORMAT_D24_UNORM_S8_UINT;
            case TextureFormat::Depth32F:
                return DXGI_FORMAT_D32_FLOAT;
            default:
                return ToDXGIFormat(format);
            }
        }

        DXGI_FORMAT ResolveRtvFormat(const TextureFormat format)
        {
            return ToDXGIFormat(format);
        }

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

        descriptors.hasSrv =
            !isDepth ||
            HasTextureUsage(textureDesc.usage, TextureUsageFlags::Sampled);
        if (descriptors.hasSrv)
        {
            descriptors.srvDesc.Format = ResolveSrvFormat(viewDesc.format);
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
            descriptors.rtvDesc.Format = ResolveRtvFormat(viewDesc.format);
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
            descriptors.dsvDesc.Format = ResolveDsvFormat(viewDesc.format);
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

    bool DoesDX12BarrierRangeCoverWholeTexture(
        const RHITextureDesc& textureDesc,
        const RHISubresourceRange& subresourceRange)
    {
        const uint32_t mipLevels = (std::max)(textureDesc.mipLevels, 1u);
        const uint32_t layerCount = GetTextureLayerCount(textureDesc.dimension, textureDesc.arrayLayers);
        if (subresourceRange.mipLevelCount == 0u && subresourceRange.arrayLayerCount == 0u)
            return true;

        if (subresourceRange.baseMipLevel != 0u)
            return false;

        const uint32_t requestedMipCount = subresourceRange.mipLevelCount != 0u
            ? subresourceRange.mipLevelCount
            : (mipLevels - subresourceRange.baseMipLevel);
        const uint64_t mipEnd = (std::min)(
            static_cast<uint64_t>(subresourceRange.baseMipLevel) + static_cast<uint64_t>(requestedMipCount),
            static_cast<uint64_t>(mipLevels));
        if (mipEnd < mipLevels)
            return false;

        if (textureDesc.dimension == TextureDimension::Texture3D)
            return true;

        if (subresourceRange.baseArrayLayer != 0u)
            return false;

        const uint32_t requestedLayerCount = subresourceRange.arrayLayerCount != 0u
            ? subresourceRange.arrayLayerCount
            : (layerCount - subresourceRange.baseArrayLayer);
        const uint64_t layerEnd = (std::min)(
            static_cast<uint64_t>(subresourceRange.baseArrayLayer) + static_cast<uint64_t>(requestedLayerCount),
            static_cast<uint64_t>(layerCount));
        return layerEnd >= layerCount;
    }

    namespace
    {
        uint32_t ResolveBarrierPlaneCount(const TextureFormat format)
        {
            switch (format)
            {
            case TextureFormat::Depth24Stencil8:
                return 2u;
            default:
                return 1u;
            }
        }

        UINT CalculateDX12BarrierSubresourceIndex(
            const uint32_t mipLevel,
            const uint32_t arrayLayer,
            const uint32_t planeSlice,
            const uint32_t mipLevels,
            const uint32_t layerCount)
        {
            return static_cast<UINT>(
                mipLevel +
                arrayLayer * mipLevels +
                planeSlice * mipLevels * layerCount);
        }

    }

    bool TryResolveDX12BarrierSubresourceIndex(
        const RHITextureDesc& textureDesc,
        const RHISubresourceRange& subresourceRange,
        UINT& outSubresourceIndex)
    {
        const auto indices = BuildDX12BarrierSubresourceIndices(textureDesc, subresourceRange);
        if (indices.size() != 1u)
            return false;

        outSubresourceIndex = indices.front();
        return true;
    }

    std::vector<UINT> BuildDX12BarrierSubresourceIndices(
        const RHITextureDesc& textureDesc,
        const RHISubresourceRange& subresourceRange)
    {
        std::vector<UINT> indices;
        const uint32_t mipLevels = (std::max)(textureDesc.mipLevels, 1u);
        const uint32_t layerCount = GetTextureLayerCount(textureDesc.dimension, textureDesc.arrayLayers);
        const bool is3DTexture = textureDesc.dimension == TextureDimension::Texture3D;
        if (subresourceRange.baseMipLevel >= mipLevels ||
            (!is3DTexture && subresourceRange.baseArrayLayer >= layerCount))
        {
            return indices;
        }

        const uint32_t requestedMipCount = subresourceRange.mipLevelCount != 0u
            ? subresourceRange.mipLevelCount
            : (mipLevels - subresourceRange.baseMipLevel);
        const uint32_t requestedLayerCount = subresourceRange.arrayLayerCount != 0u
            ? subresourceRange.arrayLayerCount
            : (layerCount - subresourceRange.baseArrayLayer);
        const uint64_t mipEnd64 = (std::min)(
            NLS::Render::RHI::SubresourceRangeEnd(subresourceRange.baseMipLevel, requestedMipCount),
            static_cast<uint64_t>(mipLevels));
        const uint32_t mipEnd = static_cast<uint32_t>(mipEnd64);
        const uint32_t layerBegin = is3DTexture ? 0u : subresourceRange.baseArrayLayer;
        const uint64_t layerEnd64 = is3DTexture
            ? 1u
            : (std::min)(
                NLS::Render::RHI::SubresourceRangeEnd(subresourceRange.baseArrayLayer, requestedLayerCount),
                static_cast<uint64_t>(layerCount));
        const uint32_t layerEnd = static_cast<uint32_t>(layerEnd64);
        const uint32_t planeCount = ResolveBarrierPlaneCount(textureDesc.format);

        indices.reserve(
            static_cast<size_t>(mipEnd - subresourceRange.baseMipLevel) *
            static_cast<size_t>(layerEnd - layerBegin) *
            static_cast<size_t>(planeCount));
        for (uint32_t planeSlice = 0u; planeSlice < planeCount; ++planeSlice)
        {
            for (uint32_t arrayLayer = layerBegin; arrayLayer < layerEnd; ++arrayLayer)
            {
                for (uint32_t mipLevel = subresourceRange.baseMipLevel; mipLevel < mipEnd; ++mipLevel)
                {
                    indices.push_back(CalculateDX12BarrierSubresourceIndex(
                        mipLevel,
                        arrayLayer,
                        planeSlice,
                        mipLevels,
                        layerCount));
                }
            }
        }
        return indices;
    }
#endif
}
