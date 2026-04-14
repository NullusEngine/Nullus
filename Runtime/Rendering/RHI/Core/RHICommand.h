#pragma once

#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHISync.h"

namespace NLS::Render::RHI
{
    struct NLS_RENDER_API RHIViewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    struct NLS_RENDER_API RHIRect2D
    {
        int32_t x = 0;
        int32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct NLS_RENDER_API RHIColorClearValue
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
    };

    struct NLS_RENDER_API RHIDepthStencilClearValue
    {
        float depth = 1.0f;
        uint32_t stencil = 0;
    };

    struct NLS_RENDER_API RHIRenderPassColorAttachmentDesc
    {
        std::shared_ptr<class RHITextureView> view;
        LoadOp loadOp = LoadOp::Clear;
        StoreOp storeOp = StoreOp::Store;
        RHIColorClearValue clearValue{};
    };

    struct NLS_RENDER_API RHIRenderPassDepthStencilAttachmentDesc
    {
        std::shared_ptr<class RHITextureView> view;
        LoadOp depthLoadOp = LoadOp::Clear;
        StoreOp depthStoreOp = StoreOp::Store;
        LoadOp stencilLoadOp = LoadOp::DontCare;
        StoreOp stencilStoreOp = StoreOp::DontCare;
        RHIDepthStencilClearValue clearValue{};
    };

    struct NLS_RENDER_API RHIRenderPassDesc
    {
        std::vector<RHIRenderPassColorAttachmentDesc> colorAttachments;
        std::optional<RHIRenderPassDepthStencilAttachmentDesc> depthStencilAttachment;
        RHIRect2D renderArea{};
        std::string debugName;
    };

    struct NLS_RENDER_API RHIBufferCopyRegion
    {
        uint64_t srcOffset = 0;
        uint64_t dstOffset = 0;
        uint64_t size = 0;
    };

    struct NLS_RENDER_API RHIBufferToTextureCopyDesc
    {
        std::shared_ptr<class RHIBuffer> source;
        std::shared_ptr<class RHITexture> destination;
        uint64_t bufferOffset = 0;
        uint32_t mipLevel = 0;
        uint32_t arrayLayer = 0;
        RHIOffset3D textureOffset{};
        RHIExtent3D extent{};
    };

    struct NLS_RENDER_API RHITextureCopyDesc
    {
        std::shared_ptr<class RHITexture> source;
        std::shared_ptr<class RHITexture> destination;
        RHISubresourceRange sourceRange{};
        RHISubresourceRange destinationRange{};
        RHIOffset3D sourceOffset{};
        RHIOffset3D destinationOffset{};
        RHIExtent3D extent{};
    };

    struct NLS_RENDER_API RHIBufferBarrier
    {
        std::shared_ptr<class RHIBuffer> buffer;
        ResourceState before = ResourceState::Unknown;
        ResourceState after = ResourceState::Unknown;
        PipelineStageMask sourceStageMask = PipelineStageMask::AllCommands;
        PipelineStageMask destinationStageMask = PipelineStageMask::AllCommands;
        AccessMask sourceAccessMask = AccessMask::MemoryRead | AccessMask::MemoryWrite;
        AccessMask destinationAccessMask = AccessMask::MemoryRead | AccessMask::MemoryWrite;
    };

    struct NLS_RENDER_API RHITextureBarrier
    {
        std::shared_ptr<class RHITexture> texture;
        ResourceState before = ResourceState::Unknown;
        ResourceState after = ResourceState::Unknown;
        RHISubresourceRange subresourceRange{};
        PipelineStageMask sourceStageMask = PipelineStageMask::AllCommands;
        PipelineStageMask destinationStageMask = PipelineStageMask::AllCommands;
        AccessMask sourceAccessMask = AccessMask::MemoryRead | AccessMask::MemoryWrite;
        AccessMask destinationAccessMask = AccessMask::MemoryRead | AccessMask::MemoryWrite;
    };

    struct NLS_RENDER_API RHIBarrierDesc
    {
        std::vector<RHIBufferBarrier> bufferBarriers;
        std::vector<RHITextureBarrier> textureBarriers;
    };

    class NLS_RENDER_API RHICommandBuffer : public RHIObject
    {
    public:
        virtual void Begin() = 0;
        virtual void End() = 0;
        virtual void Reset() = 0;
        virtual bool IsRecording() const = 0;
        // Returns the native command buffer handle (e.g., VkCommandBuffer for Vulkan)
        virtual void* GetNativeCommandBuffer() const = 0;

        virtual void BeginRenderPass(const RHIRenderPassDesc& desc) = 0;
        virtual void EndRenderPass() = 0;
        virtual void SetViewport(const RHIViewport& viewport) = 0;
        virtual void SetScissor(const RHIRect2D& rect) = 0;
        virtual void BindGraphicsPipeline(const std::shared_ptr<RHIGraphicsPipeline>& pipeline) = 0;
        virtual void BindComputePipeline(const std::shared_ptr<RHIComputePipeline>& pipeline) = 0;
        virtual void BindBindingSet(uint32_t setIndex, const std::shared_ptr<RHIBindingSet>& bindingSet) = 0;
        virtual void PushConstants(ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data) = 0;
        virtual void BindVertexBuffer(uint32_t slot, const RHIVertexBufferView& view) = 0;
        virtual void BindIndexBuffer(const RHIIndexBufferView& view) = 0;
        virtual void Draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0) = 0;
        virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0, int32_t vertexOffset = 0, uint32_t firstInstance = 0) = 0;
        virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
        virtual void CopyBuffer(const std::shared_ptr<RHIBuffer>& source, const std::shared_ptr<RHIBuffer>& destination, const RHIBufferCopyRegion& region) = 0;
        virtual void CopyBufferToTexture(const RHIBufferToTextureCopyDesc& desc) = 0;
        virtual void CopyTexture(const RHITextureCopyDesc& desc) = 0;
        virtual void Barrier(const RHIBarrierDesc& barrier) = 0;
    };

    class NLS_RENDER_API RHICommandPool : public RHIObject
    {
    public:
        virtual QueueType GetQueueType() const = 0;
        virtual std::shared_ptr<RHICommandBuffer> CreateCommandBuffer(std::string debugName = {}) = 0;
        virtual void Reset() = 0;
    };
}
