#include "Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.h"

#include <algorithm>

namespace NLS::Render::RHI::DX12
{
    DX12TextureUploadPlan BuildDX12TextureUploadPlan(const RHITextureDesc& desc)
    {
        DX12TextureUploadPlan plan;

        if (desc.extent.width == 0 || desc.extent.height == 0 || desc.mipLevels == 0)
            return plan;

        const uint32_t bytesPerPixel = GetTextureFormatBytesPerPixel(desc.format);
        const uint32_t arrayLayers = desc.dimension == TextureDimension::TextureCube
            ? GetTextureLayerCount(TextureDimension::TextureCube)
            : std::max(desc.arrayLayers, 1u);

        size_t dataOffset = 0;
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; ++arrayLayer)
        {
            uint32_t mipWidth = desc.extent.width;
            uint32_t mipHeight = desc.extent.height;
            uint32_t mipDepth = std::max(desc.extent.depth, 1u);

            for (uint32_t mipLevel = 0; mipLevel < desc.mipLevels; ++mipLevel)
            {
                const size_t rowPitch = static_cast<size_t>(mipWidth) * bytesPerPixel;
                const size_t slicePitch = rowPitch * static_cast<size_t>(mipHeight) * static_cast<size_t>(mipDepth);

                plan.subresources.push_back({
                    mipLevel,
                    arrayLayer,
                    mipWidth,
                    mipHeight,
                    mipDepth,
                    dataOffset,
                    rowPitch,
                    slicePitch
                });

                dataOffset += slicePitch;
                mipWidth = std::max(mipWidth / 2u, 1u);
                mipHeight = std::max(mipHeight / 2u, 1u);
                mipDepth = std::max(mipDepth / 2u, 1u);
            }
        }

        plan.totalBytes = dataOffset;
        return plan;
    }
}
