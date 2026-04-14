#pragma once

#include "Rendering/RHI/Core/RHICommand.h"

namespace NLS::Render::RHI
{
    class ResourceStateTracker;
    class DescriptorAllocator;
    class UploadContext;

    struct NLS_RENDER_API RHISubmitDesc
    {
        std::vector<std::shared_ptr<class RHICommandBuffer>> commandBuffers;
        std::vector<std::shared_ptr<class RHISemaphore>> waitSemaphores;
        std::vector<std::shared_ptr<class RHISemaphore>> signalSemaphores;
        std::shared_ptr<class RHIFence> signalFence;
    };

    struct NLS_RENDER_API RHIPresentDesc
    {
        std::shared_ptr<class RHISwapchain> swapchain;
        uint32_t imageIndex = 0;
        std::vector<std::shared_ptr<class RHISemaphore>> waitSemaphores;
        // UI rendering signal semaphore - Driver sets this from UIManager::ResolveUISignalSemaphore()
        // Backend should wait on this semaphore before presenting to ensure UI rendering completes first
        void* uiSignalSemaphore = nullptr;
    };

    struct NLS_RENDER_API RHIAcquiredImage
    {
        uint32_t imageIndex = 0;
        std::shared_ptr<class RHITextureView> imageView;
        bool suboptimal = false;
    };

    class NLS_RENDER_API RHISwapchain : public RHIObject
    {
    public:
        virtual const SwapchainDesc& GetDesc() const = 0;
        virtual uint32_t GetImageCount() const = 0;
        virtual std::optional<RHIAcquiredImage> AcquireNextImage(
            const std::shared_ptr<RHISemaphore>& signalSemaphore,
            const std::shared_ptr<RHIFence>& signalFence) = 0;
        virtual std::shared_ptr<RHITextureView> GetBackbufferView(uint32_t index) { return nullptr; } // Returns view for swapchain backbuffer
        virtual void Resize(uint32_t width, uint32_t height) = 0;
        virtual void* GetNativeSwapchainHandle() { return nullptr; } // For backend-specific access
    };

    class NLS_RENDER_API RHIQueue : public RHIObject
    {
    public:
        virtual QueueType GetType() const = 0;
        virtual void Submit(const RHISubmitDesc& submitDesc) = 0;
        virtual void Present(const RHIPresentDesc& presentDesc) = 0;
    };

    struct NLS_RENDER_API RHIFrameContext
    {
        uint32_t frameIndex = 0;
        uint32_t swapchainImageIndex = 0;
        bool hasAcquiredSwapchainImage = false;
        std::shared_ptr<RHICommandPool> commandPool;
        std::shared_ptr<RHICommandBuffer> commandBuffer;
        std::shared_ptr<RHIFence> frameFence;
        std::shared_ptr<RHISemaphore> imageAcquiredSemaphore;
        std::shared_ptr<RHISemaphore> renderFinishedSemaphore;
        std::shared_ptr<ResourceStateTracker> resourceStateTracker;
        std::shared_ptr<DescriptorAllocator> descriptorAllocator;
        std::shared_ptr<UploadContext> uploadContext;
        size_t uploadBytesReserved = 0;
        std::shared_ptr<RHITextureView> swapchainBackbufferView; // View for swapchain backbuffer, used when outputBuffer is nullptr
    };
}
