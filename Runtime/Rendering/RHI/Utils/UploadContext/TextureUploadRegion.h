#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"

namespace NLS::Render::RHI
{
    struct TextureUploadRegionLayout
    {
        uint32_t rowBytes = 0;
        uint32_t rows = 0;
        uint32_t rowPitch = 0;
        uint32_t slicePitch = 0;
        uint64_t requiredBytes = 0;
    };

    // A slice pitch is the distance between adjacent Z slices, not the volume size.
    inline bool ValidateTextureUploadRegion(const RHITextureDesc& texture, uint32_t mip,
        uint32_t layer, RHIOffset3D offset, RHIExtent3D extent, uint32_t rowPitch,
        uint32_t slicePitch, uint64_t dataSize, TextureUploadRegionLayout& result,
        std::string& error)
    {
        result = {};
        const auto fail = [&](const char* reason) { error = reason; return false; };
        if (mip >= texture.mipLevels || mip >= 32u ||
            layer >= GetTextureLayerCount(texture.dimension, texture.arrayLayers))
            return fail("Texture upload mip or array layer is out of range");
        const bool volume = texture.dimension == TextureDimension::Texture3D;
        const uint32_t width = (std::max)(texture.extent.width >> mip, 1u);
        const uint32_t height = (std::max)(texture.extent.height >> mip, 1u);
        const uint32_t depth = volume ? (std::max)(texture.extent.depth >> mip, 1u) : 1u;
        if (!extent.width || !extent.height || !extent.depth || offset.x > width ||
            offset.y > height || offset.z > depth || extent.width > width - offset.x ||
            extent.height > height - offset.y || extent.depth > depth - offset.z)
            return fail("Texture upload region is outside the destination mip");
        const auto* format = GetTextureFormatDescriptor(texture.format);
        if (!format || !format->blockWidth || !format->blockHeight || !format->bytesPerBlock)
            return fail("Texture upload format has no byte layout");
        if (format->isCompressed && (volume || offset.x % format->blockWidth ||
            offset.y % format->blockHeight ||
            (extent.width % format->blockWidth && offset.x + extent.width != width) ||
            (extent.height % format->blockHeight && offset.y + extent.height != height)))
            return fail("Compressed texture upload region is not block aligned");
        const uint64_t rowBytes = ((uint64_t(extent.width) + format->blockWidth - 1) /
            format->blockWidth) * format->bytesPerBlock;
        result.rows = (extent.height + format->blockHeight - 1) / format->blockHeight;
        const uint64_t resolvedRow = rowPitch ? rowPitch : rowBytes;
        const uint64_t resolvedSlice = slicePitch ? slicePitch : resolvedRow * result.rows;
        if (resolvedRow < rowBytes || resolvedSlice < resolvedRow * result.rows ||
            resolvedRow > UINT32_MAX || resolvedSlice > UINT32_MAX)
            return fail("Texture upload row or slice pitch is invalid");
        result.rowBytes = static_cast<uint32_t>(rowBytes);
        result.rowPitch = static_cast<uint32_t>(resolvedRow);
        result.slicePitch = static_cast<uint32_t>(resolvedSlice);
        result.requiredBytes = uint64_t(extent.depth - 1) * resolvedSlice +
            uint64_t(result.rows - 1) * resolvedRow + rowBytes;
        if (result.requiredBytes > dataSize)
            return fail("Texture upload data does not contain the requested slices");
        error.clear();
        return true;
    }
}
