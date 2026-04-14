#pragma once

#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"

namespace NLS::Render::RHI
{
    class NLS_RENDER_API RHIAdapter : public RHIObject
    {
    public:
        virtual NativeBackendType GetBackendType() const = 0;
        virtual std::string_view GetVendor() const = 0;
        virtual std::string_view GetHardware() const = 0;
    };

    class NLS_RENDER_API RHIDevice : public RHIObject
    {
    public:
        virtual const std::shared_ptr<RHIAdapter>& GetAdapter() const = 0;
        virtual const RHIDeviceCapabilities& GetCapabilities() const = 0;
        virtual NativeRenderDeviceInfo GetNativeDeviceInfo() const = 0;
        virtual bool IsBackendReady() const = 0;
        virtual std::shared_ptr<RHIQueue> GetQueue(QueueType queueType) = 0;
        virtual std::shared_ptr<RHISwapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;
        virtual std::shared_ptr<RHIBuffer> CreateBuffer(const RHIBufferDesc& desc, const void* initialData = nullptr) = 0;
        virtual std::shared_ptr<RHITexture> CreateTexture(const RHITextureDesc& desc, const void* initialData = nullptr) = 0;
        virtual std::shared_ptr<RHITextureView> CreateTextureView(const std::shared_ptr<RHITexture>& texture, const RHITextureViewDesc& desc) = 0;
        virtual std::shared_ptr<RHISampler> CreateSampler(const SamplerDesc& desc, std::string debugName = {}) = 0;
        virtual std::shared_ptr<RHIBindingLayout> CreateBindingLayout(const RHIBindingLayoutDesc& desc) = 0;
        virtual std::shared_ptr<RHIBindingSet> CreateBindingSet(const RHIBindingSetDesc& desc) = 0;
        virtual std::shared_ptr<RHIPipelineLayout> CreatePipelineLayout(const RHIPipelineLayoutDesc& desc) = 0;
        virtual std::shared_ptr<RHIShaderModule> CreateShaderModule(const RHIShaderModuleDesc& desc) = 0;
        virtual std::shared_ptr<RHIGraphicsPipeline> CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc) = 0;
        virtual std::shared_ptr<RHIComputePipeline> CreateComputePipeline(const RHIComputePipelineDesc& desc) = 0;
        virtual std::shared_ptr<RHICommandPool> CreateCommandPool(QueueType queueType, std::string debugName = {}) = 0;
        virtual std::shared_ptr<RHIFence> CreateFence(std::string debugName = {}) = 0;
        virtual std::shared_ptr<RHISemaphore> CreateSemaphore(std::string debugName = {}) = 0;

        // Readback support - read pixels from a texture
        // Used by picking/system text rendering that needs to read back from textures
        virtual void ReadPixels(
            const std::shared_ptr<RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data) = 0;

        // UI rendering support - default implementations for formal RHI devices
        virtual bool PrepareUIRender() { return true; }
        virtual void ReleaseUITextureHandles() {}
        // Sets the current command buffer for UI rendering - used by Vulkan and other explicit RHI backends
        virtual void SetCurrentCommandBuffer(void* commandBuffer) {}
    };
}
