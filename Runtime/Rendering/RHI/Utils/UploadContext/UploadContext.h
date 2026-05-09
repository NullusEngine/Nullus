#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHICommand.h"

namespace NLS::Render::RHI
{
    struct NLS_RENDER_API UploadAllocation
    {
        std::shared_ptr<RHIBuffer> stagingBuffer;
        void* cpuAddress = nullptr;
        uint64_t gpuOffset = 0;
        size_t size = 0;
        size_t alignment = 0;
        std::string debugName;
    };

    struct NLS_RENDER_API UploadBufferRequest
    {
        std::shared_ptr<RHIBuffer> destination;
        uint64_t destinationOffset = 0;
        const void* data = nullptr;
        size_t size = 0;
        size_t alignment = 1;
        std::string debugName;
    };

    struct NLS_RENDER_API UploadTextureRequest
    {
        std::shared_ptr<RHITexture> destination;
        const void* data = nullptr;
        size_t dataSize = 0;
        uint32_t mipLevel = 0;
        uint32_t arrayLayer = 0;
        uint32_t rowPitch = 0;
        uint32_t slicePitch = 0;
        RHIExtent3D extent{};
        std::string debugName;
    };

    struct NLS_RENDER_API UploadSubmission
    {
        bool accepted = false;
        std::shared_ptr<RHICompletionToken> completion;
        std::string diagnostic;
    };

    struct NLS_RENDER_API UploadBatchRequest
    {
        std::vector<UploadBufferRequest> bufferUploads;
        std::vector<UploadTextureRequest> textureUploads;
        std::string debugName;
    };

    struct NLS_RENDER_API UploadBatchSubmission : UploadSubmission
    {
        size_t acceptedBufferUploads = 0u;
        size_t acceptedTextureUploads = 0u;
    };

    class NLS_RENDER_API UploadContext
    {
    public:
        virtual ~UploadContext() = default;
        virtual void BeginFrame(uint64_t frameIndex) = 0;
        virtual void EndFrame(uint64_t completedFrameIndex) = 0;
        virtual UploadAllocation Allocate(size_t size, size_t alignment = 1, std::string debugName = {}) = 0;
        virtual UploadBatchSubmission SubmitUploadBatch(RHICommandBuffer& commandBuffer, const UploadBatchRequest& request) = 0;
        virtual UploadSubmission SubmitUploadBuffer(RHICommandBuffer& commandBuffer, const UploadBufferRequest& request) = 0;
        virtual UploadSubmission SubmitUploadTexture(RHICommandBuffer& commandBuffer, const UploadTextureRequest& request) = 0;
        virtual bool UploadBuffer(RHICommandBuffer& commandBuffer, const UploadBufferRequest& request) = 0;
        virtual bool UploadTexture(RHICommandBuffer& commandBuffer, const UploadTextureRequest& request) = 0;
        virtual void CollectGarbage(uint64_t completedFrameIndex) = 0;
    };

    NLS_RENDER_API std::shared_ptr<UploadContext> CreateDefaultUploadContext(size_t ringCapacity = 4 * 1024 * 1024);
}
