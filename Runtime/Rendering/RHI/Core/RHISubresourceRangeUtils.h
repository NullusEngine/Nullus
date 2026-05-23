#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::RHI
{
    inline bool IsEmptySubresourceRange(const RHISubresourceRange& range)
    {
        return range.mipLevelCount == 0u && range.arrayLayerCount == 0u;
    }

    inline uint64_t SubresourceRangeEnd(const uint32_t base, const uint32_t count)
    {
        return static_cast<uint64_t>(base) + static_cast<uint64_t>(count);
    }

    inline RHISubresourceRange GetFullTextureSubresourceRange(const RHITextureDesc& textureDesc)
    {
        RHISubresourceRange range{};
        range.baseMipLevel = 0u;
        range.mipLevelCount = (std::max)(textureDesc.mipLevels, 1u);
        range.baseArrayLayer = 0u;
        range.arrayLayerCount = textureDesc.dimension == TextureDimension::Texture3D
            ? 1u
            : GetTextureLayerCount(textureDesc.dimension, textureDesc.arrayLayers);
        return range;
    }

    inline std::optional<RHISubresourceRange> NormalizeTextureSubresourceRange(
        const RHITextureDesc& textureDesc,
        RHISubresourceRange range)
    {
        if (IsEmptySubresourceRange(range))
            return GetFullTextureSubresourceRange(textureDesc);

        const uint32_t mipLevels = (std::max)(textureDesc.mipLevels, 1u);
        if (range.baseMipLevel >= mipLevels)
            return std::nullopt;

        const uint32_t requestedMipCount = range.mipLevelCount != 0u
            ? range.mipLevelCount
            : (mipLevels - range.baseMipLevel);
        const uint64_t mipEnd = (std::min)(
            SubresourceRangeEnd(range.baseMipLevel, requestedMipCount),
            static_cast<uint64_t>(mipLevels));
        range.mipLevelCount = static_cast<uint32_t>(mipEnd - range.baseMipLevel);
        if (range.mipLevelCount == 0u)
            return std::nullopt;

        if (textureDesc.dimension == TextureDimension::Texture3D)
        {
            range.baseArrayLayer = 0u;
            range.arrayLayerCount = 1u;
            return range;
        }

        const uint32_t layerCount = GetTextureLayerCount(textureDesc.dimension, textureDesc.arrayLayers);
        if (range.baseArrayLayer >= layerCount)
            return std::nullopt;

        const uint32_t requestedLayerCount = range.arrayLayerCount != 0u
            ? range.arrayLayerCount
            : (layerCount - range.baseArrayLayer);
        const uint64_t layerEnd = (std::min)(
            SubresourceRangeEnd(range.baseArrayLayer, requestedLayerCount),
            static_cast<uint64_t>(layerCount));
        range.arrayLayerCount = static_cast<uint32_t>(layerEnd - range.baseArrayLayer);
        if (range.arrayLayerCount == 0u)
            return std::nullopt;

        return range;
    }

    inline bool IsFullTextureSubresourceRange(const RHITextureDesc& textureDesc, const RHISubresourceRange& range)
    {
        const auto fullRange = GetFullTextureSubresourceRange(textureDesc);
        return range.baseMipLevel == fullRange.baseMipLevel &&
            range.mipLevelCount == fullRange.mipLevelCount &&
            range.baseArrayLayer == fullRange.baseArrayLayer &&
            range.arrayLayerCount == fullRange.arrayLayerCount;
    }

    inline bool DoesSubresourceRangeOverlap(
        const RHISubresourceRange& lhs,
        const RHISubresourceRange& rhs)
    {
        if (IsEmptySubresourceRange(lhs) || IsEmptySubresourceRange(rhs))
            return true;

        return lhs.baseMipLevel < SubresourceRangeEnd(rhs.baseMipLevel, rhs.mipLevelCount) &&
            rhs.baseMipLevel < SubresourceRangeEnd(lhs.baseMipLevel, lhs.mipLevelCount) &&
            lhs.baseArrayLayer < SubresourceRangeEnd(rhs.baseArrayLayer, rhs.arrayLayerCount) &&
            rhs.baseArrayLayer < SubresourceRangeEnd(lhs.baseArrayLayer, lhs.arrayLayerCount);
    }

    inline bool DoesSubresourceRangeCover(
        const RHISubresourceRange& coveringRange,
        const RHISubresourceRange& requestedRange)
    {
        if (IsEmptySubresourceRange(coveringRange))
            return true;
        if (IsEmptySubresourceRange(requestedRange))
            return false;

        return coveringRange.baseMipLevel <= requestedRange.baseMipLevel &&
            SubresourceRangeEnd(requestedRange.baseMipLevel, requestedRange.mipLevelCount) <=
                SubresourceRangeEnd(coveringRange.baseMipLevel, coveringRange.mipLevelCount) &&
            coveringRange.baseArrayLayer <= requestedRange.baseArrayLayer &&
            SubresourceRangeEnd(requestedRange.baseArrayLayer, requestedRange.arrayLayerCount) <=
                SubresourceRangeEnd(coveringRange.baseArrayLayer, coveringRange.arrayLayerCount);
    }

    inline std::optional<RHISubresourceRange> IntersectSubresourceRanges(
        const RHISubresourceRange& lhs,
        const RHISubresourceRange& rhs)
    {
        if (IsEmptySubresourceRange(lhs) || IsEmptySubresourceRange(rhs))
            return std::nullopt;
        if (!DoesSubresourceRangeOverlap(lhs, rhs))
            return std::nullopt;

        const uint32_t mipBegin = (std::max)(lhs.baseMipLevel, rhs.baseMipLevel);
        const uint32_t mipEnd = static_cast<uint32_t>((std::min)(
            SubresourceRangeEnd(lhs.baseMipLevel, lhs.mipLevelCount),
            SubresourceRangeEnd(rhs.baseMipLevel, rhs.mipLevelCount)));
        const uint32_t layerBegin = (std::max)(lhs.baseArrayLayer, rhs.baseArrayLayer);
        const uint32_t layerEnd = static_cast<uint32_t>((std::min)(
            SubresourceRangeEnd(lhs.baseArrayLayer, lhs.arrayLayerCount),
            SubresourceRangeEnd(rhs.baseArrayLayer, rhs.arrayLayerCount)));

        if (mipBegin >= mipEnd || layerBegin >= layerEnd)
            return std::nullopt;

        RHISubresourceRange intersection{};
        intersection.baseMipLevel = mipBegin;
        intersection.mipLevelCount = mipEnd - mipBegin;
        intersection.baseArrayLayer = layerBegin;
        intersection.arrayLayerCount = layerEnd - layerBegin;
        return intersection;
    }

    inline std::vector<RHISubresourceRange> SubtractSubresourceRange(
        const RHISubresourceRange& sourceRange,
        const RHISubresourceRange& subtractRange)
    {
        std::vector<RHISubresourceRange> remainderRanges;
        if (IsEmptySubresourceRange(sourceRange))
            return remainderRanges;
        if (IsEmptySubresourceRange(subtractRange))
            return remainderRanges;

        const auto overlap = IntersectSubresourceRanges(sourceRange, subtractRange);
        if (!overlap.has_value())
        {
            remainderRanges.push_back(sourceRange);
            return remainderRanges;
        }

        const auto& intersect = *overlap;
        const auto sourceMipEnd = static_cast<uint32_t>(SubresourceRangeEnd(sourceRange.baseMipLevel, sourceRange.mipLevelCount));
        const auto sourceLayerEnd = static_cast<uint32_t>(SubresourceRangeEnd(sourceRange.baseArrayLayer, sourceRange.arrayLayerCount));
        const auto intersectMipEnd = static_cast<uint32_t>(SubresourceRangeEnd(intersect.baseMipLevel, intersect.mipLevelCount));
        const auto intersectLayerEnd = static_cast<uint32_t>(SubresourceRangeEnd(intersect.baseArrayLayer, intersect.arrayLayerCount));

        if (sourceRange.baseMipLevel < intersect.baseMipLevel)
        {
            remainderRanges.push_back({
                sourceRange.baseMipLevel,
                intersect.baseMipLevel - sourceRange.baseMipLevel,
                sourceRange.baseArrayLayer,
                sourceRange.arrayLayerCount
            });
        }

        if (intersectMipEnd < sourceMipEnd)
        {
            remainderRanges.push_back({
                intersectMipEnd,
                sourceMipEnd - intersectMipEnd,
                sourceRange.baseArrayLayer,
                sourceRange.arrayLayerCount
            });
        }

        if (sourceRange.baseArrayLayer < intersect.baseArrayLayer)
        {
            remainderRanges.push_back({
                intersect.baseMipLevel,
                intersect.mipLevelCount,
                sourceRange.baseArrayLayer,
                intersect.baseArrayLayer - sourceRange.baseArrayLayer
            });
        }

        if (intersectLayerEnd < sourceLayerEnd)
        {
            remainderRanges.push_back({
                intersect.baseMipLevel,
                intersect.mipLevelCount,
                intersectLayerEnd,
                sourceLayerEnd - intersectLayerEnd
            });
        }

        return remainderRanges;
    }
}
