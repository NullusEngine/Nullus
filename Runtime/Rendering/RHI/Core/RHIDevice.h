#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"

namespace NLS::Render::RHI
{
    enum class RHIReadbackStatusCode : uint8_t
    {
        Success,
        InvalidArgument,
        UnsupportedFormat,
        DeviceLost,
        BackendFailure
    };

    struct NLS_RENDER_API RHIReadbackResult
    {
        RHIReadbackStatusCode code = RHIReadbackStatusCode::InvalidArgument;
        std::string message;
        std::shared_ptr<RHICompletionToken> completion;

        bool Succeeded() const { return code == RHIReadbackStatusCode::Success; }
    };

    struct NLS_RENDER_API RHIBufferReadbackDesc
    {
        // Caller declares the expected source state, queue waits, and keeps data alive until the completion token finishes.
        std::shared_ptr<class RHIBuffer> source;
        ResourceState sourceState = ResourceState::Unknown;
        std::vector<std::shared_ptr<class RHISemaphore>> waitSemaphores;
        uint64_t sourceOffset = 0u;
        uint64_t size = 0u;
        void* data = nullptr;
        std::string debugName;
    };

    class NLS_RENDER_API RHIAdapter : public RHIObject
    {
    public:
        virtual NativeBackendType GetBackendType() const = 0;
        virtual std::string_view GetVendor() const = 0;
        virtual std::string_view GetHardware() const = 0;
    };

    class NLS_RENDER_API RHIUIDeviceBridge
    {
    public:
        virtual ~RHIUIDeviceBridge() = default;
        virtual bool PrepareUIRender() = 0;
        virtual void ReleaseUITextureHandles() = 0;
        virtual void SetCurrentCommandBuffer(NativeHandle commandBuffer) = 0;
    };

    class NLS_RENDER_API RHIDevice : public RHIObject
    {
    public:
        RHIDevice() : m_cacheIdentity(AllocateCacheIdentity()) {}
        uint64_t GetCacheIdentity() const { return m_cacheIdentity; }

        virtual const std::shared_ptr<RHIAdapter>& GetAdapter() const = 0;
        virtual const RHIDeviceCapabilities& GetCapabilities() const = 0;
        virtual NativeRenderDeviceInfo GetNativeDeviceInfo() const = 0;
        virtual bool IsBackendReady() const = 0;
        virtual std::shared_ptr<RHIQueue> GetQueue(QueueType queueType) = 0;
        virtual std::shared_ptr<RHISwapchain> CreateSwapchain(const SwapchainDesc& desc) = 0;
        virtual std::shared_ptr<RHIBuffer> CreateBuffer(const RHIBufferDesc& desc, const RHIBufferUploadDesc& uploadDesc) = 0;
        // Initial upload data is caller-owned and only guaranteed to stay valid for this call.
        // Backend implementations must synchronously copy it into backend-owned upload/GPU memory
        // before returning, or fail the call.
        virtual std::shared_ptr<RHITexture> CreateTexture(const RHITextureDesc& desc, const RHITextureUploadDesc& uploadDesc) = 0;
        virtual std::shared_ptr<RHIBuffer> CreateBuffer(const RHIBufferDesc& desc)
        {
            return CreateBuffer(desc, RHIBufferUploadDesc{});
        }
        virtual std::shared_ptr<RHITexture> CreateTexture(const RHITextureDesc& desc)
        {
            return CreateTexture(desc, RHITextureUploadDesc{});
        }
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
        virtual RHIUpdateResult UpdateTexture(const RHITextureUpdateDesc& desc)
        {
            (void)desc;
            return { RHIUpdateStatusCode::Unsupported, "RHI device does not support in-place texture updates" };
        }

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
        virtual RHIReadbackResult ReadPixelsChecked(
            const std::shared_ptr<RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data)
        {
            (void)texture;
            (void)x;
            (void)y;
            (void)width;
            (void)height;
            (void)format;
            (void)type;
            (void)data;
            return {
                RHIReadbackStatusCode::BackendFailure,
                "RHI device does not implement status-returning ReadPixelsChecked"
            };
        }
        virtual RHIReadbackResult BeginReadPixels(
            const std::shared_ptr<RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data)
        {
            return ReadPixelsChecked(texture, x, y, width, height, format, type, data);
        }
        // Starts an asynchronous buffer readback. Ordinary frame paths should poll the token instead of waiting.
        virtual RHIReadbackResult BeginReadBuffer(const RHIBufferReadbackDesc& desc)
        {
            (void)desc;
            return {
                RHIReadbackStatusCode::BackendFailure,
                "RHI device does not implement async buffer readback"
            };
        }

        virtual RHIUIDeviceBridge* GetUIBridgeDevice() { return nullptr; }

    private:
        static uint64_t AllocateCacheIdentity()
        {
            static std::atomic_uint64_t nextIdentity { 1u };
            auto identity = nextIdentity.fetch_add(1u, std::memory_order_relaxed);
            if (identity == 0u)
                identity = nextIdentity.fetch_add(1u, std::memory_order_relaxed);
            return identity;
        }

        const uint64_t m_cacheIdentity;
    };

    inline RHIDeviceFeatureState GetUIOverlayFrameGraphFeature(const RHIDevice* device)
    {
        if (device == nullptr)
        {
            return {
                false,
                "UI overlay FrameGraph is unavailable because there is no active RHI device"
            };
        }

        return device->GetCapabilities().GetFeature(RHIDeviceFeature::UIOverlayFrameGraph);
    }
}
