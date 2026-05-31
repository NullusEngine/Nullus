#include "Rendering/RHI/Backends/DX12/DX12TextureUploadUtils.h"

#include <algorithm>
#include <cstring>

namespace NLS::Render::RHI::DX12
{
    DX12TextureUploadFormatSemantics GetDX12TextureUploadFormatSemantics(TextureFormat format)
    {
        DX12TextureUploadFormatSemantics semantics{};
        semantics.sourceBytesPerPixel = GetTextureFormatBytesPerPixel(format);
        semantics.nativeBytesPerPixel = semantics.sourceBytesPerPixel;
        semantics.expandedAlpha = 255u;

        if (format == TextureFormat::RGB8)
        {
            semantics.nativeBytesPerPixel = 4u;
            semantics.expandsPackedRgbToRgba8 = true;
            semantics.description = "TextureFormat::RGB8 is uploaded as packed RGB8 source data and expanded to opaque RGBA8 for DX12 native storage";
            return semantics;
        }

        semantics.description = "Texture format uses matching source and DX12 native upload layout";
        return semantics;
    }

    DX12TextureUploadRowCopyResult CopyDX12TextureUploadRow(
        TextureFormat format,
        const void* source,
        size_t sourceSize,
        void* destination,
        size_t destinationSize,
        uint32_t width)
    {
        DX12TextureUploadRowCopyResult result{};
        if (source == nullptr || destination == nullptr || width == 0u)
            return result;

        const auto semantics = GetDX12TextureUploadFormatSemantics(format);
        const size_t sourceRowBytes = semantics.expandsPackedRgbToRgba8
            ? static_cast<size_t>(width) * static_cast<size_t>(semantics.sourceBytesPerPixel)
            : static_cast<size_t>(CalculateTextureRowPitch(format, width));
        const size_t nativeRowBytes = semantics.expandsPackedRgbToRgba8
            ? static_cast<size_t>(width) * static_cast<size_t>(semantics.nativeBytesPerPixel)
            : static_cast<size_t>(CalculateTextureRowPitch(format, width));
        if (sourceRowBytes == 0u || nativeRowBytes == 0u)
            return result;
        if (sourceSize < sourceRowBytes || destinationSize < nativeRowBytes)
            return result;

        const auto* sourceBytes = static_cast<const uint8_t*>(source);
        auto* destinationBytes = static_cast<uint8_t*>(destination);
        if (semantics.expandsPackedRgbToRgba8)
        {
            for (uint32_t pixelIndex = 0; pixelIndex < width; ++pixelIndex)
            {
                const size_t sourceIndex = static_cast<size_t>(pixelIndex) * 3u;
                const size_t destinationIndex = static_cast<size_t>(pixelIndex) * 4u;
                destinationBytes[destinationIndex + 0u] = sourceBytes[sourceIndex + 0u];
                destinationBytes[destinationIndex + 1u] = sourceBytes[sourceIndex + 1u];
                destinationBytes[destinationIndex + 2u] = sourceBytes[sourceIndex + 2u];
                destinationBytes[destinationIndex + 3u] = semantics.expandedAlpha;
            }
        }
        else
        {
            std::memcpy(destinationBytes, sourceBytes, nativeRowBytes);
        }

        result.succeeded = true;
        result.sourceBytesConsumed = sourceRowBytes;
        result.destinationBytesWritten = nativeRowBytes;
        return result;
    }

    DX12TextureUploadPlan BuildDX12TextureUploadPlan(const RHITextureDesc& desc)
    {
        DX12TextureUploadPlan plan;

        if (desc.extent.width == 0 || desc.extent.height == 0 || desc.mipLevels == 0)
            return plan;

        const auto formatSemantics = GetDX12TextureUploadFormatSemantics(desc.format);
        const bool isRgb8Expansion = formatSemantics.expandsPackedRgbToRgba8;
        const uint32_t arrayLayers = GetTextureLayerCount(desc.dimension, desc.arrayLayers);

        size_t dataOffset = 0;
        size_t nativeDataOffset = 0;
        for (uint32_t arrayLayer = 0; arrayLayer < arrayLayers; ++arrayLayer)
        {
            uint32_t mipWidth = desc.extent.width;
            uint32_t mipHeight = desc.extent.height;
            uint32_t mipDepth = desc.dimension == TextureDimension::Texture3D
                ? (std::max)(desc.extent.depth, 1u)
                : 1u;

            for (uint32_t mipLevel = 0; mipLevel < desc.mipLevels; ++mipLevel)
            {
                const size_t rowPitch = isRgb8Expansion
                    ? static_cast<size_t>(mipWidth) * 3u
                    : static_cast<size_t>(CalculateTextureRowPitch(desc.format, mipWidth));
                const size_t nativeRowPitch = isRgb8Expansion
                    ? static_cast<size_t>(mipWidth) * 4u
                    : static_cast<size_t>(CalculateTextureRowPitch(desc.format, mipWidth));
                const size_t slicePitch = isRgb8Expansion
                    ? rowPitch * static_cast<size_t>(mipHeight) * static_cast<size_t>(mipDepth)
                    : static_cast<size_t>(CalculateTextureSlicePitch(desc.format, mipWidth, mipHeight, mipDepth));
                const size_t nativeSlicePitch = isRgb8Expansion
                    ? nativeRowPitch * static_cast<size_t>(mipHeight) * static_cast<size_t>(mipDepth)
                    : static_cast<size_t>(CalculateTextureSlicePitch(desc.format, mipWidth, mipHeight, mipDepth));

                plan.subresources.push_back({
                    mipLevel,
                    arrayLayer,
                    mipWidth,
                    mipHeight,
                    mipDepth,
                    dataOffset,
                    rowPitch,
                    slicePitch,
                    nativeRowPitch,
                    nativeSlicePitch
                });

                dataOffset += slicePitch;
                nativeDataOffset += nativeSlicePitch;
                mipWidth = (std::max)(mipWidth / 2u, 1u);
                mipHeight = (std::max)(mipHeight / 2u, 1u);
                mipDepth = (std::max)(mipDepth / 2u, 1u);
            }
        }

        plan.totalBytes = dataOffset;
        plan.nativeTotalBytes = nativeDataOffset;
        return plan;
    }

    DX12InitialUploadRequest BuildDX12InitialBufferUploadRequest(
        const RHIBufferDesc& desc,
        const void* initialData)
    {
        RHIBufferUploadDesc uploadDesc;
        uploadDesc.data = initialData;
        uploadDesc.dataSize = initialData != nullptr ? desc.size : 0u;
        uploadDesc.destinationOffset = 0u;
        uploadDesc.debugName = desc.debugName;
        return BuildDX12InitialBufferUploadRequest(desc, uploadDesc);
    }

    DX12InitialUploadRequest BuildDX12InitialBufferUploadRequest(
        const RHIBufferDesc& desc,
        const RHIBufferUploadDesc& uploadDesc)
    {
        DX12InitialUploadRequest request;
        request.resourceKind = DX12InitialUploadResourceKind::Buffer;
        request.data = uploadDesc.data;
        if (request.textureSubresources.empty())
            request.dataSize = uploadDesc.HasData() ? uploadDesc.dataSize : 0u;
        request.destinationOffset = uploadDesc.destinationOffset;
        request.debugName = uploadDesc.debugName.empty() ? desc.debugName : uploadDesc.debugName;
        return request;
    }

    DX12InitialUploadRequest BuildDX12InitialTextureUploadRequest(
        const RHITextureDesc& desc,
        const void* initialData)
    {
        RHITextureUploadDesc uploadDesc;
        uploadDesc.data = initialData;
        uploadDesc.dataSize = initialData != nullptr ? BuildDX12TextureUploadPlan(desc).totalBytes : 0u;
        uploadDesc.debugName = desc.debugName;
        return BuildDX12InitialTextureUploadRequest(desc, uploadDesc);
    }

    DX12InitialUploadRequest BuildDX12InitialTextureUploadRequest(
        const RHITextureDesc& desc,
        const RHITextureUploadDesc& uploadDesc)
    {
        DX12InitialUploadRequest request;
        request.resourceKind = DX12InitialUploadResourceKind::Texture;
        request.data = uploadDesc.data;
        request.texturePlan = BuildDX12TextureUploadPlan(desc);
        request.textureSubresources.reserve(uploadDesc.subresources.size());
        for (const auto& subresource : uploadDesc.subresources)
        {
            request.textureSubresources.push_back({
                subresource.data,
                subresource.dataSize
            });
            request.dataSize += subresource.dataSize;
        }
        if (request.textureSubresources.empty())
            request.dataSize = uploadDesc.HasData() ? uploadDesc.dataSize : 0u;
        if (!request.textureSubresources.empty())
        {
            request.data = nullptr;
        }
        request.destinationOffset = 0u;
        request.debugName = uploadDesc.debugName.empty() ? desc.debugName : uploadDesc.debugName;
        return request;
    }
}
