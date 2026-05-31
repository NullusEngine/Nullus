#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI::DX12
{
    struct NLS_RENDER_API DX12TextureUploadSubresource
    {
        uint32_t mipLevel = 0;
        uint32_t arrayLayer = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        size_t dataOffset = 0;
        size_t rowPitch = 0;
        size_t slicePitch = 0;
        size_t nativeRowPitch = 0;
        size_t nativeSlicePitch = 0;
    };

    struct NLS_RENDER_API DX12TextureUploadPlan
    {
        std::vector<DX12TextureUploadSubresource> subresources;
        size_t totalBytes = 0;
        size_t nativeTotalBytes = 0;
    };

    struct NLS_RENDER_API DX12TextureUploadFormatSemantics
    {
        uint32_t sourceBytesPerPixel = 0u;
        uint32_t nativeBytesPerPixel = 0u;
        bool expandsPackedRgbToRgba8 = false;
        uint8_t expandedAlpha = 255u;
        std::string description;
    };

    struct NLS_RENDER_API DX12TextureUploadRowCopyResult
    {
        bool succeeded = false;
        size_t sourceBytesConsumed = 0u;
        size_t destinationBytesWritten = 0u;
    };

    enum class DX12InitialUploadResourceKind : uint8_t
    {
        Buffer,
        Texture
    };

    struct NLS_RENDER_API DX12InitialUploadRequest
    {
        DX12InitialUploadResourceKind resourceKind = DX12InitialUploadResourceKind::Buffer;
        struct TextureSubresourceData
        {
            const void* data = nullptr;
            size_t dataSize = 0u;
        };

        const void* data = nullptr;
        size_t dataSize = 0;
        size_t destinationOffset = 0;
        DX12TextureUploadPlan texturePlan{};
        std::vector<TextureSubresourceData> textureSubresources;
        std::string debugName;
    };

    NLS_RENDER_API DX12TextureUploadFormatSemantics GetDX12TextureUploadFormatSemantics(TextureFormat format);
    NLS_RENDER_API DX12TextureUploadRowCopyResult CopyDX12TextureUploadRow(
        TextureFormat format,
        const void* source,
        size_t sourceSize,
        void* destination,
        size_t destinationSize,
        uint32_t width);
    NLS_RENDER_API DX12TextureUploadPlan BuildDX12TextureUploadPlan(const RHITextureDesc& desc);
    NLS_RENDER_API DX12InitialUploadRequest BuildDX12InitialBufferUploadRequest(
        const RHIBufferDesc& desc,
        const void* initialData);
    NLS_RENDER_API DX12InitialUploadRequest BuildDX12InitialBufferUploadRequest(
        const RHIBufferDesc& desc,
        const RHIBufferUploadDesc& uploadDesc);
    NLS_RENDER_API DX12InitialUploadRequest BuildDX12InitialTextureUploadRequest(
        const RHITextureDesc& desc,
        const void* initialData);
    NLS_RENDER_API DX12InitialUploadRequest BuildDX12InitialTextureUploadRequest(
        const RHITextureDesc& desc,
        const RHITextureUploadDesc& uploadDesc);
}
