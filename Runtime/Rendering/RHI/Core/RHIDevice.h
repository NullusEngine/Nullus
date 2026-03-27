#pragma once

#include "Rendering/RHI/Core/RHISwapchain.h"

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
    };
}
