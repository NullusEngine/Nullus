#include <gtest/gtest.h>

#include <array>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/FrameGraph/FrameGraphBuffer.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"
#include "Rendering/FrameGraph/FrameGraphTexture.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/Resources/ShaderType.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/SceneRendererFactory.h"
#include "Core/ServiceLocator.h"

namespace
{
    template<typename T>
    concept HasRawNativeTextureHandle = requires(T& texture)
    {
        texture.GetNativeTextureHandle();
    };

    template<typename T>
    concept HasTaggedNativeImageHandle = requires(T& texture)
    {
        { texture.GetNativeImageHandle() } -> std::same_as<NLS::Render::RHI::NativeHandle>;
    };

    class ContractAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "ContractAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override
        {
            return NLS::Render::RHI::NativeBackendType::DX12;
        }
        std::string_view GetVendor() const override { return "ContractVendor"; }
        std::string_view GetHardware() const override { return "ContractHardware"; }
    };

    class ContractCompletionToken final : public NLS::Render::RHI::RHICompletionToken
    {
    public:
        explicit ContractCompletionToken(NLS::Render::RHI::RHICompletionStatus status)
            : m_status(std::move(status))
        {
        }

        std::string_view GetDebugName() const override { return "ContractCompletionToken"; }
        NLS::Render::RHI::RHICompletionStatus Poll() override { return m_status; }
        bool IsComplete() override { return m_status.IsComplete(); }
        NLS::Render::RHI::RHICompletionStatus GetStatus() override { return m_status; }
        NLS::Render::RHI::RHICompletionStatus Wait(uint64_t = 0) override
        {
            ++waitCalls;
            return m_status;
        }

        size_t waitCalls = 0u;

    private:
        NLS::Render::RHI::RHICompletionStatus m_status;
    };

    class ContractFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        std::string_view GetDebugName() const override { return "ContractFence"; }
        bool IsSignaled() const override { return signaled; }
        void Reset() override
        {
            signaled = false;
            ++resetCalls;
        }
        bool Wait(uint64_t timeoutNanoseconds = 0) override
        {
            lastWaitTimeoutNanoseconds = timeoutNanoseconds;
            signaled = waitResult;
            ++waitCalls;
            return waitResult;
        }
        NLS::Render::RHI::NativeHandle GetNativeFenceHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this };
        }

        bool signaled = false;
        bool waitResult = true;
        uint64_t lastWaitTimeoutNanoseconds = 0u;
        size_t resetCalls = 0u;
        size_t waitCalls = 0u;
    };

    class ContractSemaphore final : public NLS::Render::RHI::RHISemaphore
    {
    public:
        std::string_view GetDebugName() const override { return "ContractSemaphore"; }
        bool IsSignaled() const override { return signaled; }
        void Reset() override
        {
            signaled = false;
            ++resetCalls;
        }
        NLS::Render::RHI::NativeHandle GetNativeSemaphoreHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this };
        }

        bool signaled = false;
        size_t resetCalls = 0u;
    };

    class ContractTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        ContractTexture() = default;

        explicit ContractTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override
        {
            return m_desc.debugName.empty() ? std::string_view("ContractTexture") : std::string_view(m_desc.debugName);
        }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return reportedState; }
        NLS::Render::RHI::NativeHandle GetNativeImageHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this };
        }

        NLS::Render::RHI::ResourceState reportedState = NLS::Render::RHI::ResourceState::Unknown;

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class ContractTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        ContractTextureView() = default;

        ContractTextureView(
            std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
            NLS::Render::RHI::RHITextureViewDesc desc)
            : m_texture(std::move(texture))
            , m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override
        {
            return m_desc.debugName.empty()
                ? std::string_view("ContractTextureView")
                : std::string_view(m_desc.debugName);
        }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
    };

    class ContractBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        explicit ContractBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override
        {
            return m_desc.debugName.empty() ? std::string_view("ContractBuffer") : std::string_view(m_desc.debugName);
        }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }
        NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override
        {
            return { NLS::Render::RHI::BackendType::DX12, this };
        }

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
    };

    class ContractCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "ContractCommandBuffer"; }
        void Begin() override
        {
            recording = true;
            ++beginCalls;
        }
        void End() override
        {
            recording = false;
            ++endCalls;
        }
        void Reset() override
        {
            recording = false;
            ++resetCalls;
        }
        bool IsRecording() const override { return recording; }
        NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
        void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetViewport(const NLS::Render::RHI::RHIViewport&) override {}
        void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
        void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override {}
        void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
        void BindBindingSet(uint32_t, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override {}
        void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t, const void*) override {}
        void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override {}
        void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
        void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
        void Dispatch(uint32_t, uint32_t, uint32_t) override {}
        void CopyBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
        void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
        void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrierDesc) override
        {
            ++barrierCalls;
            lastBarrierDesc = barrierDesc;
        }

        bool recording = false;
        size_t beginCalls = 0u;
        size_t endCalls = 0u;
        size_t resetCalls = 0u;
        size_t barrierCalls = 0u;
        NLS::Render::RHI::RHIBarrierDesc lastBarrierDesc;
    };

    class ContractCommandPool final : public NLS::Render::RHI::RHICommandPool
    {
    public:
        std::string_view GetDebugName() const override { return "ContractCommandPool"; }
        NLS::Render::RHI::QueueType GetQueueType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string = {}) override
        {
            return commandBuffer;
        }
        void Reset() override { ++resetCalls; }

        std::shared_ptr<ContractCommandBuffer> commandBuffer;
        size_t resetCalls = 0u;
    };

    class ContractQueue final : public NLS::Render::RHI::RHIQueue
    {
    public:
        std::string_view GetDebugName() const override { return "ContractQueue"; }
        NLS::Render::RHI::QueueType GetType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
        {
            ++submitCalls;
            lastSubmitDesc = submitDesc;
        }
        NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(
            const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
        {
            Submit(submitDesc);
            return nextSubmitResult;
        }
        void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            ++presentCalls;
            lastPresentDesc = presentDesc;
        }
        NLS::Render::RHI::RHIQueueOperationResult PresentChecked(
            const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            Present(presentDesc);
            return nextPresentResult;
        }

        size_t submitCalls = 0u;
        size_t presentCalls = 0u;
        NLS::Render::RHI::RHISubmitDesc lastSubmitDesc {};
        NLS::Render::RHI::RHIPresentDesc lastPresentDesc {};
        NLS::Render::RHI::RHIQueueOperationResult nextSubmitResult {};
        NLS::Render::RHI::RHIQueueOperationResult nextPresentResult {};
    };

    class BaseQueueContract final : public NLS::Render::RHI::RHIQueue
    {
    public:
        std::string_view GetDebugName() const override { return "BaseQueueContract"; }
        NLS::Render::RHI::QueueType GetType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        void Submit(const NLS::Render::RHI::RHISubmitDesc&) override { ++submitCalls; }
        void Present(const NLS::Render::RHI::RHIPresentDesc&) override { ++presentCalls; }

        size_t submitCalls = 0u;
        size_t presentCalls = 0u;
    };

    class ContractSwapchain final : public NLS::Render::RHI::RHISwapchain
    {
    public:
        ContractSwapchain()
            : backbufferView(std::make_shared<ContractTextureView>())
        {
            m_desc.width = 800u;
            m_desc.height = 600u;
        }

        explicit ContractSwapchain(std::weak_ptr<NLS::Render::RHI::RHITextureView> trackedView)
            : ContractSwapchain()
        {
            this->trackedView = std::move(trackedView);
        }

        std::string_view GetDebugName() const override { return "ContractSwapchain"; }
        const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
        uint32_t GetImageCount() const override { return 2u; }
        std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
            const std::shared_ptr<NLS::Render::RHI::RHISemaphore>&,
            const std::shared_ptr<NLS::Render::RHI::RHIFence>&) override
        {
            ++acquireCalls;
            return NLS::Render::RHI::RHIAcquiredImage{ acquiredImageIndex, backbufferView, false };
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override
        {
            lastBackbufferIndex = index;
            return backbufferView;
        }
        bool Resize(uint32_t width, uint32_t height) override
        {
            ++resizeCalls;
            resizeWidth = width;
            resizeHeight = height;
            trackedViewReleasedBeforeResize = trackedView.expired();
            fenceWaitsObservedAtResize = observedFence != nullptr ? observedFence->waitCalls : 0u;
            commandResetsObservedAtResize = observedCommandBuffer != nullptr ? observedCommandBuffer->resetCalls : 0u;
            commandPoolResetsObservedAtResize = observedCommandPool != nullptr ? observedCommandPool->resetCalls : 0u;
            m_desc.width = width;
            m_desc.height = height;
            return true;
        }

        std::weak_ptr<NLS::Render::RHI::RHITextureView> trackedView;
        std::shared_ptr<ContractTextureView> backbufferView;
        ContractFence* observedFence = nullptr;
        ContractCommandBuffer* observedCommandBuffer = nullptr;
        ContractCommandPool* observedCommandPool = nullptr;
        size_t resizeCalls = 0u;
        size_t acquireCalls = 0u;
        size_t fenceWaitsObservedAtResize = 0u;
        size_t commandResetsObservedAtResize = 0u;
        size_t commandPoolResetsObservedAtResize = 0u;
        uint32_t acquiredImageIndex = 1u;
        uint32_t lastBackbufferIndex = 0u;
        uint32_t resizeWidth = 0u;
        uint32_t resizeHeight = 0u;
        bool trackedViewReleasedBeforeResize = false;

    private:
        NLS::Render::RHI::SwapchainDesc m_desc {};
    };

    class ContractDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        using NLS::Render::RHI::RHIDevice::CreateBuffer;
        using NLS::Render::RHI::RHIDevice::CreateTexture;

        ContractDevice()
            : m_adapter(std::make_shared<ContractAdapter>())
            , m_queue(std::make_shared<ContractQueue>())
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.backendReady = true;
            m_capabilities.supportsGraphics = true;
            m_capabilities.supportsSwapchain = true;
            m_capabilities.supportsExplicitBarriers = true;
            m_capabilities.supportsCentralizedDescriptorManagement = true;
        }

        std::string_view GetDebugName() const override { return "ContractDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override
        {
            return m_queue;
        }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(
            const NLS::Render::RHI::SwapchainDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            return std::make_shared<ContractBuffer>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::RHITextureUploadDesc&) override
        {
            return std::make_shared<ContractTexture>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHITextureViewDesc& desc) override
        {
            return std::make_shared<ContractTextureView>(texture, desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
            const NLS::Render::RHI::SamplerDesc&,
            std::string = {}) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
            const NLS::Render::RHI::RHIBindingLayoutDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
            const NLS::Render::RHI::RHIBindingSetDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
            const NLS::Render::RHI::RHIPipelineLayoutDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
            const NLS::Render::RHI::RHIShaderModuleDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
            const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(
            const NLS::Render::RHI::RHIComputePipelineDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
            NLS::Render::RHI::QueueType,
            std::string = {}) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string = {}) override
        {
            return std::make_shared<ContractFence>();
        }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string = {}) override
        {
            return std::make_shared<ContractSemaphore>();
        }
        void ReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            uint32_t,
            uint32_t,
            uint32_t,
            uint32_t,
            NLS::Render::Settings::EPixelDataFormat,
            NLS::Render::Settings::EPixelDataType,
            void*) override
        {
            lastReadPixelsTexture = texture;
        }
        NLS::Render::RHI::RHIReadbackResult BeginReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            uint32_t,
            uint32_t,
            uint32_t,
            uint32_t,
            NLS::Render::Settings::EPixelDataFormat,
            NLS::Render::Settings::EPixelDataType,
            void*) override
        {
            ++beginReadPixelsCalls;
            lastReadPixelsTexture = texture;
            return nextBeginReadPixelsResult;
        }

        std::shared_ptr<ContractQueue> GetContractQueue() const { return m_queue; }

        size_t beginReadPixelsCalls = 0u;
        std::shared_ptr<NLS::Render::RHI::RHITexture> lastReadPixelsTexture;
        NLS::Render::RHI::RHIReadbackResult nextBeginReadPixelsResult {
            NLS::Render::RHI::RHIReadbackStatusCode::Success,
            {},
            std::make_shared<ContractCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
                NLS::Render::RHI::RHICompletionStatusCode::Success,
                {}
            })
        };

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        std::shared_ptr<ContractQueue> m_queue;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    class BaseReadbackContractDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        BaseReadbackContractDevice()
            : m_adapter(std::make_shared<ContractAdapter>())
        {
        }

        std::string_view GetDebugName() const override { return "BaseReadbackContractDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return {}; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(
            const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc&,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc&,
            const NLS::Render::RHI::RHITextureUploadDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
            const NLS::Render::RHI::RHITextureViewDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
            const NLS::Render::RHI::SamplerDesc&,
            std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
            const NLS::Render::RHI::RHIBindingLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
            const NLS::Render::RHI::RHIBindingSetDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
            const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
            const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
            const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(
            const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
            NLS::Render::RHI::QueueType,
            std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string = {}) override { return nullptr; }
        void ReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
            uint32_t,
            uint32_t,
            uint32_t,
            uint32_t,
            NLS::Render::Settings::EPixelDataFormat,
            NLS::Render::Settings::EPixelDataType,
            void*) override
        {
        }

        NLS::Render::RHI::RHIReadbackResult BeginReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            NLS::Render::Settings::EPixelDataFormat format,
            NLS::Render::Settings::EPixelDataType type,
            void* data) override
        {
            return NLS::Render::RHI::RHIDevice::BeginReadPixels(
                texture,
                x,
                y,
                width,
                height,
                format,
                type,
                data);
        }

        NLS::Render::RHI::RHIReadbackResult ReadPixelsChecked(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            NLS::Render::Settings::EPixelDataFormat format,
            NLS::Render::Settings::EPixelDataType type,
            void* data) override
        {
            return NLS::Render::RHI::RHIDevice::ReadPixelsChecked(
                texture,
                x,
                y,
                width,
                height,
                format,
                type,
                data);
        }

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    NLS::Render::Settings::DriverSettings MakeContractDriverSettings(bool threaded = false)
    {
        NLS::Render::Settings::DriverSettings settings;
        settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        settings.enableThreadedRendering = threaded;
        settings.threadedFrameSlotCount = 1u;
        settings.framesInFlight = 1u;
        return settings;
    }
}

TEST(RenderFrameworkContractTests, SwapchainResizeContractDrainsThreadedFramesAndReleasesBackbufferBeforeResize)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings(true));
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 101u;
    snapshot.targetsSwapchain = true;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessFrameSnapshot(driver, snapshot));

    auto retainedBackbufferView = std::make_shared<ContractTextureView>();
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakBackbufferView = retainedBackbufferView;
    auto swapchain = std::make_shared<ContractSwapchain>(weakBackbufferView);
    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto commandPool = std::make_shared<ContractCommandPool>();
    auto frameFence = std::make_shared<ContractFence>();
    commandPool->commandBuffer = commandBuffer;
    swapchain->observedFence = frameFence.get();
    swapchain->observedCommandBuffer = commandBuffer.get();
    swapchain->observedCommandPool = commandPool.get();

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.swapchainBackbufferView = retainedBackbufferView;
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    retainedBackbufferView.reset();

    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1280u, 720u);

    EXPECT_EQ(swapchain->resizeCalls, 0u);
    EXPECT_FALSE(weakBackbufferView.expired());
    EXPECT_EQ(frameFence->waitCalls, 0u);

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    package.targetsSwapchain = true;
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = snapshot.frameId;
    ASSERT_TRUE(lifecycle->TryBeginRenderScene(0u));
    ASSERT_TRUE(lifecycle->CompleteRenderScene(0u, package));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(0u));
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(0u, submissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(0u));

    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    driver.ResizePlatformSwapchain(1280u, 720u);

    EXPECT_EQ(swapchain->resizeCalls, 1u);
    EXPECT_TRUE(swapchain->trackedViewReleasedBeforeResize);
    EXPECT_GE(swapchain->fenceWaitsObservedAtResize, 1u);
    EXPECT_GE(swapchain->commandResetsObservedAtResize, 1u);
    EXPECT_GE(swapchain->commandPoolResetsObservedAtResize, 1u);
    EXPECT_EQ(frameContext.swapchainBackbufferView, nullptr);
    EXPECT_EQ(swapchain->resizeWidth, 1280u);
    EXPECT_EQ(swapchain->resizeHeight, 720u);
}

TEST(RenderFrameworkContractTests, SwapchainResizeKeepsPendingStateWhenFrameFenceDrainFails)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto swapchain = std::make_shared<ContractSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto backbufferView = std::make_shared<ContractTextureView>();
    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto frameFence = std::make_shared<ContractFence>();
    frameFence->signaled = false;
    frameFence->waitResult = false;

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.swapchainBackbufferView = backbufferView;
    frameContext.commandBuffer = commandBuffer;
    frameContext.frameFence = frameFence;

    driver.ResizePlatformSwapchain(1440u, 900u);
    NLS::Render::Context::DriverTestAccess::AgePendingSwapchainResize(
        driver,
        NLS::Render::Context::GetInteractiveSwapchainResizeDebounce());
    NLS::Render::Context::DriverUIAccess::PresentSwapchain(driver);

    EXPECT_GE(frameFence->waitCalls, 1u);
    EXPECT_GT(frameFence->lastWaitTimeoutNanoseconds, 0u);
    EXPECT_EQ(swapchain->resizeCalls, 0u);
    EXPECT_EQ(frameContext.swapchainBackbufferView, backbufferView);
    EXPECT_EQ(commandBuffer->resetCalls, 0u);
}

TEST(RenderFrameworkContractTests, ReadbackContractSeparatesBeginTokenFromCheckedWaitAndDiagnostics)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "ContractReadbackTexture";
    textureDesc.extent = { 4u, 4u, 1u };
    auto texture = std::make_shared<ContractTexture>(textureDesc);
    std::array<uint8_t, 16> pixels {};

    auto pendingCompletion = std::make_shared<ContractCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Pending,
        {}
    });
    explicitDevice->nextBeginReadPixelsResult = {
        NLS::Render::RHI::RHIReadbackStatusCode::Success,
        {},
        pendingCompletion
    };

    const auto beginResult = NLS::Render::Context::DriverRendererAccess::BeginReadPixels(
        driver,
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixels.data());

    EXPECT_TRUE(beginResult.Succeeded());
    EXPECT_EQ(beginResult.completion, pendingCompletion);
    EXPECT_EQ(pendingCompletion->waitCalls, 0u);
    EXPECT_EQ(explicitDevice->lastReadPixelsTexture, texture);

    auto failedCompletion = std::make_shared<ContractCompletionToken>(NLS::Render::RHI::RHICompletionStatus{
        NLS::Render::RHI::RHICompletionStatusCode::Failed,
        "readback fence failed"
    });
    explicitDevice->nextBeginReadPixelsResult = {
        NLS::Render::RHI::RHIReadbackStatusCode::Success,
        {},
        failedCompletion
    };

    const auto checkedResult = NLS::Render::Context::DriverRendererAccess::ReadPixelsChecked(
        driver,
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixels.data());

    EXPECT_EQ(failedCompletion->waitCalls, 1u);
    EXPECT_EQ(checkedResult.code, NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure);
    EXPECT_EQ(checkedResult.message, "readback fence failed");
}

TEST(RenderFrameworkContractTests, BaseRhiDeviceReadPixelsCheckedReportsUnsupportedInsteadOfFakeSuccess)
{
    BaseReadbackContractDevice device;
    std::array<uint8_t, 4> pixels {};
    auto texture = std::make_shared<ContractTexture>();

    const auto result = device.ReadPixelsChecked(
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixels.data());

    EXPECT_EQ(result.code, NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure);
    EXPECT_NE(result.message.find("does not implement"), std::string::npos);
}

TEST(RenderFrameworkContractTests, BaseRhiQueueCheckedOperationsReportUnsupportedInsteadOfFakeSuccess)
{
    BaseQueueContract queue;

    const auto submitResult = queue.SubmitChecked({});
    EXPECT_EQ(submitResult.code, NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure);
    EXPECT_NE(submitResult.message.find("does not implement"), std::string::npos);
    EXPECT_EQ(queue.submitCalls, 0u);

    const auto presentResult = queue.PresentChecked({});
    EXPECT_EQ(presentResult.code, NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure);
    EXPECT_NE(presentResult.message.find("does not implement"), std::string::npos);
    EXPECT_EQ(queue.presentCalls, 0u);
}

TEST(RenderFrameworkContractTests, QueueCheckedSubmissionAndPresentExposeStatus)
{
    ContractQueue queue;

    const auto submitResult = queue.SubmitChecked({});
    EXPECT_TRUE(submitResult.Succeeded());
    EXPECT_EQ(queue.submitCalls, 1u);

    const auto presentResult = queue.PresentChecked({});
    EXPECT_TRUE(presentResult.Succeeded());
    EXPECT_EQ(queue.presentCalls, 1u);
}

TEST(RenderFrameworkContractTests, DX12UploadAndDescriptorWaitsUseBoundedFencePolicy)
{
    const std::filesystem::path dx12ResourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp";
    const std::filesystem::path dx12DescriptorPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string resourceSource = readFile(dx12ResourcePath);
    const std::string descriptorSource = readFile(dx12DescriptorPath);

    ASSERT_FALSE(resourceSource.empty());
    ASSERT_FALSE(descriptorSource.empty());

    EXPECT_EQ(resourceSource.find("WaitForSingleObject(fenceEvent, INFINITE)"), std::string::npos);
    EXPECT_EQ(descriptorSource.find("WaitForSingleObject(fenceEvent, INFINITE)"), std::string::npos);
    EXPECT_NE(resourceSource.find("WaitForDX12FenceValue"), std::string::npos);
    EXPECT_NE(descriptorSource.find("WaitForDX12FenceValue"), std::string::npos);
}

TEST(RenderFrameworkContractTests, ThreadedWorkersUseWakeConditionInsteadOfIdleSleepPolling)
{
    const std::filesystem::path driverSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Context/Driver.cpp";
    const std::filesystem::path driverInternalPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Context/DriverInternal.h";
    const std::filesystem::path renderCoordinatorPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Context/RenderThreadCoordinator.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string driverSource = readFile(driverSourcePath);
    const std::string driverInternalSource = readFile(driverInternalPath);
    const std::string renderCoordinatorSource = readFile(renderCoordinatorPath);

    ASSERT_FALSE(driverSource.empty());
    ASSERT_FALSE(driverInternalSource.empty());
    ASSERT_FALSE(renderCoordinatorSource.empty());

    EXPECT_NE(driverInternalSource.find("std::condition_variable threadedWorkerWake"), std::string::npos);
    EXPECT_NE(driverSource.find("WaitForThreadedWorkerWake"), std::string::npos);
    EXPECT_NE(driverSource.find("NotifyThreadedWorkers"), std::string::npos);
    EXPECT_NE(renderCoordinatorSource.find("NotifyThreadedWorkers"), std::string::npos);
    const auto drainStart = driverSource.find("bool DrainThreadedLifecycleSynchronously");
    const auto notifyStart = driverSource.find("void NotifyThreadedWorkers", drainStart);
    ASSERT_NE(drainStart, std::string::npos);
    ASSERT_NE(notifyStart, std::string::npos);
    const auto drainBody = driverSource.substr(drainStart, notifyStart - drainStart);
    EXPECT_NE(drainBody.find("WaitForThreadedWorkerWake"), std::string::npos);
    EXPECT_NE(driverSource.find("kThreadedDrainWakeTimeout"), std::string::npos);
    EXPECT_NE(drainBody.find("kThreadedDrainWakeTimeout"), std::string::npos);
    EXPECT_EQ(drainBody.find("kThreadedWorkerWakeTimeout"), std::string::npos);
    EXPECT_EQ(drainBody.find("std::this_thread::sleep_for(std::chrono::milliseconds(1));"), std::string::npos);
    EXPECT_EQ(driverSource.find("std::this_thread::sleep_for(std::chrono::milliseconds(1));", driverSource.find("Render Thread Tick")),
        std::string::npos);
    EXPECT_EQ(driverSource.find("std::this_thread::sleep_for(std::chrono::milliseconds(1));", driverSource.find("RHI Thread Tick")),
        std::string::npos);
}

TEST(RenderFrameworkContractTests, DriverCreatesUploadContextThroughBackendFactoryWithoutDX12Casts)
{
    const std::filesystem::path driverSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Context/Driver.cpp";
    const std::filesystem::path backendFactoryHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/RHIDeviceFactory.h";
    const std::filesystem::path backendFactorySourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/RHIDeviceFactory.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string driverSource = readFile(driverSourcePath);
    const std::string backendFactoryHeader = readFile(backendFactoryHeaderPath);
    const std::string backendFactorySource = readFile(backendFactorySourcePath);

    ASSERT_FALSE(driverSource.empty());
    ASSERT_FALSE(backendFactoryHeader.empty());
    ASSERT_FALSE(backendFactorySource.empty());

    EXPECT_EQ(driverSource.find("DX12Resource.h"), std::string::npos);
    EXPECT_EQ(driverSource.find("ID3D12Device"), std::string::npos);
    EXPECT_EQ(driverSource.find("CreateDX12UploadBackend"), std::string::npos);
    EXPECT_NE(driverSource.find("CreateUploadContextForRhiDevice"), std::string::npos);
    EXPECT_NE(backendFactoryHeader.find("CreateUploadContextForRhiDevice"), std::string::npos);
    EXPECT_NE(backendFactorySource.find("CreateDX12UploadBackend"), std::string::npos);
}

TEST(RenderFrameworkContractTests, QueueOperationFailuresAreExposedThroughDriverTelemetry)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto swapchain = std::make_shared<ContractSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto frameFence = std::make_shared<ContractFence>();
    auto imageAcquiredSemaphore = std::make_shared<ContractSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<ContractSemaphore>();

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = commandBuffer;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    const auto queue = explicitDevice->GetContractQueue();
    queue->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "submit failed for telemetry"
    };
    queue->nextPresentResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "present failed for telemetry"
    };

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, true);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(telemetry.queueOperationFailureCount, 2u);
    EXPECT_NE(telemetry.lastQueueOperationFailure.find("present failed for telemetry"), std::string::npos);
}

TEST(RenderFrameworkContractTests, StandaloneFrameFenceWaitUsesBoundedDriverPolicy)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto swapchain = std::make_shared<ContractSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto frameFence = std::make_shared<ContractFence>();
    auto imageAcquiredSemaphore = std::make_shared<ContractSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<ContractSemaphore>();

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = commandBuffer;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);

    EXPECT_EQ(frameFence->waitCalls, 1u);
    EXPECT_GT(frameFence->lastWaitTimeoutNanoseconds, 0u);
}

TEST(RenderFrameworkContractTests, QueueOperationTelemetrySeparatesCurrentFrameFromCumulativeHistory)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto swapchain = std::make_shared<ContractSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto frameFence = std::make_shared<ContractFence>();
    auto imageAcquiredSemaphore = std::make_shared<ContractSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<ContractSemaphore>();

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = commandBuffer;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    const auto queue = explicitDevice->GetContractQueue();
    queue->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
        "first frame submit failed"
    };
    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    auto failedTelemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(failedTelemetry.queueOperationFailureCount, 1u);
    EXPECT_EQ(failedTelemetry.currentFrameQueueOperationFailureCount, 1u);
    EXPECT_NE(failedTelemetry.currentFrameLastQueueOperationFailure.find("first frame submit failed"), std::string::npos);

    queue->nextSubmitResult = {};
    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    const auto recoveredTelemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_EQ(recoveredTelemetry.queueOperationFailureCount, 1u);
    EXPECT_EQ(recoveredTelemetry.currentFrameQueueOperationFailureCount, 0u);
    EXPECT_TRUE(recoveredTelemetry.currentFrameLastQueueOperationFailure.empty());
}

TEST(RenderFrameworkContractTests, DeviceLostQueueOperationIsExposedThroughTelemetry)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto swapchain = std::make_shared<ContractSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto frameFence = std::make_shared<ContractFence>();
    auto imageAcquiredSemaphore = std::make_shared<ContractSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<ContractSemaphore>();

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = commandBuffer;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    const auto queue = explicitDevice->GetContractQueue();
    queue->nextSubmitResult = {
        NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost,
        "device removed during submit"
    };

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_TRUE(telemetry.deviceLostDetected);
    EXPECT_NE(telemetry.deviceLostReason.find("device removed during submit"), std::string::npos);
}

TEST(RenderFrameworkContractTests, TimelineProfilerFenceCompletionFailureIsChecked)
{
    const std::filesystem::path profilerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp";

    std::ifstream stream(profilerSourcePath, std::ios::binary);
    const std::string profilerSource{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    };

    ASSERT_FALSE(profilerSource.empty());
    EXPECT_NE(
        profilerSource.find("const HRESULT hr = m_pResolveFence->SetEventOnCompletion(wait_frame, nullptr)"),
        std::string::npos);
    EXPECT_NE(profilerSource.find("FAILED("), std::string::npos);
}

TEST(RenderFrameworkContractTests, TimelineProfilerGpuResourcesUseRaiiAndUnmapReadback)
{
    const std::filesystem::path profilerHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.h";
    const std::filesystem::path profilerSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(profilerHeaderPath);
    const std::string profilerSource = readFile(profilerSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(profilerSource.empty());

    EXPECT_NE(headerSource.find("Microsoft::WRL::ComPtr<ID3D12Resource>"), std::string::npos);
    EXPECT_NE(headerSource.find("HashMap<ID3D12CommandList*, std::unique_ptr<CommandListState>>"), std::string::npos);
    EXPECT_NE(profilerSource.find("m_pReadbackResource->Unmap(0, nullptr)"), std::string::npos);
    EXPECT_EQ(profilerSource.find("delete commandListState.second"), std::string::npos);
    EXPECT_EQ(profilerSource.find("pAllocator->Release()"), std::string::npos);
    EXPECT_EQ(profilerSource.find("m_pReadbackResource->Release()"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DX12TextureDoesNotExposeRawVoidNativeTextureHandle)
{
    static_assert(!HasRawNativeTextureHandle<NLS::Render::Backend::NativeDX12Texture>);
    static_assert(HasTaggedNativeImageHandle<NLS::Render::Backend::NativeDX12Texture>);
    SUCCEED();
}

TEST(RenderFrameworkContractTests, CompletionTokenExposesExplicitPollContract)
{
    class PollCountingToken final : public NLS::Render::RHI::RHICompletionToken
    {
    public:
        std::string_view GetDebugName() const override { return "PollCountingToken"; }

        NLS::Render::RHI::RHICompletionStatus Poll() override
        {
            ++pollCalls;
            return {
                pollCalls > 1u
                    ? NLS::Render::RHI::RHICompletionStatusCode::Success
                    : NLS::Render::RHI::RHICompletionStatusCode::Pending,
                {}
            };
        }

        NLS::Render::RHI::RHICompletionStatus Wait(uint64_t = 0) override
        {
            return Poll();
        }

        size_t pollCalls = 0u;
    };

    PollCountingToken token;

    const auto firstStatus = token.GetStatus();
    EXPECT_EQ(firstStatus.code, NLS::Render::RHI::RHICompletionStatusCode::Pending);
    EXPECT_EQ(token.pollCalls, 1u);

    EXPECT_TRUE(token.IsComplete());
    EXPECT_EQ(token.pollCalls, 2u);
}

TEST(RenderFrameworkContractTests, ShaderGenerationParticipatesInExplicitModuleCacheKey)
{
    const std::filesystem::path shaderHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Resources/Shader.h";
    const std::filesystem::path shaderSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Resources/Shader.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(shaderHeaderPath);
    const std::string source = readFile(shaderSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    EXPECT_NE(headerSource.find("uint64_t GetGeneration() const"), std::string::npos);
    EXPECT_NE(headerSource.find("m_generation"), std::string::npos);
    EXPECT_NE(source.find("std::make_tuple(backend, stage, m_generation)"), std::string::npos);
    EXPECT_NE(source.find("++m_generation"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DefaultSceneRendererFactoryCreatesDeferredRenderer)
{
    EXPECT_EQ(
        NLS::Engine::Rendering::GetDefaultSceneRendererKind(),
        NLS::Engine::Rendering::SceneRendererKind::Deferred);

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);

    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    const auto renderer = NLS::Engine::Rendering::CreateSceneRenderer(
        driver,
        NLS::Engine::Rendering::GetDefaultSceneRendererKind(),
        options);
    ASSERT_NE(renderer, nullptr);
    EXPECT_NE(dynamic_cast<NLS::Engine::Rendering::DeferredSceneRenderer*>(renderer.get()), nullptr);
}

TEST(RenderFrameworkContractTests, LightGridPrepassReusesFrameScratchStorage)
{
    const std::filesystem::path lightGridHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.h";
    const std::filesystem::path lightGridSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(lightGridHeaderPath);
    const std::string source = readFile(lightGridSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    EXPECT_NE(headerSource.find("mutable PackedFrameData m_frameScratch"), std::string::npos);
    EXPECT_NE(source.find("auto& frameData = m_frameScratch"), std::string::npos);
    EXPECT_NE(source.find("outFrameData.forwardLocalLightData.clear()"), std::string::npos);
}

TEST(RenderFrameworkContractTests, LightGridPrepassCachesPreparedResourcesForStableFrameInputs)
{
    const std::filesystem::path lightGridHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.h";
    const std::filesystem::path lightGridSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(lightGridHeaderPath);
    const std::string source = readFile(lightGridSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    EXPECT_NE(headerSource.find("PreparedResourceCacheKey"), std::string::npos);
    EXPECT_NE(headerSource.find("PreparedResourceCache"), std::string::npos);
    EXPECT_NE(headerSource.find("m_preparedResourceCache"), std::string::npos);
    EXPECT_NE(source.find("BuildPreparedResourceCacheKey"), std::string::npos);
    EXPECT_NE(source.find("TryReusePreparedResources"), std::string::npos);
    EXPECT_NE(source.find("StorePreparedResourceCache"), std::string::npos);
    EXPECT_NE(source.find("AreSameForwardLightData(m_preparedResourceCache.forwardLightData, forwardLightData)"), std::string::npos);
    EXPECT_NE(source.find("m_computeDispatchInputs.clear();"), std::string::npos);
    EXPECT_NE(source.find("DescriptorAllocationLifetime::Persistent"), std::string::npos);
    EXPECT_EQ(source.find("m_graphicsPassBindingSet.reset();"), std::string::npos);
}

TEST(RenderFrameworkContractTests, LightGridPrepassDoesNotRecreatePreparedResourcesForCameraMotion)
{
    const std::filesystem::path lightGridHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.h";
    const std::filesystem::path lightGridSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(lightGridHeaderPath);
    const std::string source = readFile(lightGridSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    EXPECT_NE(headerSource.find("forwardLightDataBuffer"), std::string::npos);
    EXPECT_EQ(headerSource.find("cameraPosition{}"), std::string::npos);
    EXPECT_EQ(headerSource.find("viewMatrix{}"), std::string::npos);
    EXPECT_EQ(headerSource.find("projectionMatrix{}"), std::string::npos);
    EXPECT_EQ(source.find("key.cameraPosition"), std::string::npos);
    EXPECT_EQ(source.find("key.viewMatrix"), std::string::npos);
    EXPECT_EQ(source.find("key.projectionMatrix"), std::string::npos);
    EXPECT_EQ(source.find("areSameMatrices(lhs.viewMatrix"), std::string::npos);
    EXPECT_EQ(source.find("areSameMatrices(lhs.projectionMatrix"), std::string::npos);
    EXPECT_NE(source.find("TryReusePreparedResources(preparedCacheKey.value(), frameData.forwardLightData)"), std::string::npos);
    EXPECT_NE(source.find("forwardLightDataBuffer->UpdateData"), std::string::npos);
    EXPECT_NE(source.find("m_computeDispatchInputs = m_preparedResourceCache.computeDispatchInputs"), std::string::npos);
}

TEST(RenderFrameworkContractTests, LightGridPrepassUsesGpuResetAndBufferSizeCacheForDynamicFrames)
{
    const std::filesystem::path lightGridHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.h";
    const std::filesystem::path lightGridSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.cpp";
    const std::filesystem::path resetShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridReset.hlsl";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(lightGridHeaderPath);
    const std::string source = readFile(lightGridSourcePath);
    const std::string resetShaderSource = readFile(resetShaderPath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(resetShaderSource.empty());
    EXPECT_NE(headerSource.find("PreparedBufferCache"), std::string::npos);
    EXPECT_NE(headerSource.find("m_preparedBufferCache"), std::string::npos);
    EXPECT_NE(headerSource.find("m_resetPipeline"), std::string::npos);
    EXPECT_NE(headerSource.find("startOffsetGrid"), std::string::npos);
    EXPECT_NE(headerSource.find("culledLightLinks"), std::string::npos);
    EXPECT_NE(source.find("CreateOrReusePreparedBuffer"), std::string::npos);
    EXPECT_NE(source.find("\"LightGridReset\""), std::string::npos);
    EXPECT_NE(source.find("\":Shaders/LightGridReset.hlsl\""), std::string::npos);
    EXPECT_NE(source.find("\"LightGridStartOffsetGrid\""), std::string::npos);
    EXPECT_NE(source.find("\"LightGridCulledLightLinks\""), std::string::npos);
    EXPECT_NE(source.find("\"u_LightGridStartOffsetGrid\""), std::string::npos);
    EXPECT_NE(source.find("\"u_LightGridCulledLightLinks\""), std::string::npos);
    EXPECT_EQ(source.find("\"u_LightGridClusterLightCounts\""), std::string::npos);
    EXPECT_EQ(source.find("outFrameData.startOffsetGrid.assign"), std::string::npos);
    EXPECT_EQ(source.find("outFrameData.culledLightLinks.assign"), std::string::npos);
    EXPECT_EQ(source.find("outFrameData.clusterRecords.assign"), std::string::npos);
    EXPECT_EQ(source.find("outFrameData.compactLightIndices.assign"), std::string::npos);
    EXPECT_NE(resetShaderSource.find("0xFFFFFFFFu"), std::string::npos);
    EXPECT_NE(resetShaderSource.find("u_LightGridLinkCounter[0] = 0u"), std::string::npos);
    EXPECT_NE(resetShaderSource.find("u_LightGridCompactCounter[0] = 0u"), std::string::npos);
}

TEST(RenderFrameworkContractTests, LightGridGraphicsContractUsesUE427ForwardLightDataNames)
{
    const std::filesystem::path lightGridHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.h";
    const std::filesystem::path lightGridSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.cpp";
    const std::filesystem::path commonShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridCommon.hlsli";
    const std::filesystem::path standardShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/Standard.hlsl";
    const std::filesystem::path deferredLightingShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/DeferredLighting.hlsl";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(lightGridHeaderPath);
    const std::string source = readFile(lightGridSourcePath);
    const std::string commonShaderSource = readFile(commonShaderPath);
    const std::string standardShaderSource = readFile(standardShaderPath);
    const std::string deferredLightingShaderSource = readFile(deferredLightingShaderPath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(commonShaderSource.empty());
    ASSERT_FALSE(standardShaderSource.empty());
    ASSERT_FALSE(deferredLightingShaderSource.empty());

    EXPECT_NE(headerSource.find("ForwardLightData"), std::string::npos);
    EXPECT_NE(headerSource.find("forwardLocalLightData"), std::string::npos);
    EXPECT_NE(headerSource.find("numCulledLightsGrid"), std::string::npos);
    EXPECT_NE(headerSource.find("culledLightDataGrid"), std::string::npos);
    EXPECT_NE(source.find("\"Forward\""), std::string::npos);
    EXPECT_NE(source.find("\"ForwardLocalLightBuffer\""), std::string::npos);
    EXPECT_NE(source.find("\"NumCulledLightsGrid\""), std::string::npos);
    EXPECT_NE(source.find("\"CulledLightDataGrid\""), std::string::npos);
    EXPECT_NE(source.find("\"ForwardLightDataUniformBuffer\""), std::string::npos);
    EXPECT_NE(source.find("ShaderParameterStructBuilder(\"LightGridGraphicsParameters\")"), std::string::npos);
    EXPECT_NE(source.find(".AddUniformBuffer(\"ForwardLightData\", 0u"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("cbuffer ForwardLightData : register(b0, space1)"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("NLSGetNumLocalLights"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("NLSGetCulledGridSize"), std::string::npos);
    EXPECT_NE(standardShaderSource.find("u_ForwardLocalLightBuffer"), std::string::npos);
    EXPECT_NE(standardShaderSource.find("u_NumCulledLightsGrid"), std::string::npos);
    EXPECT_NE(standardShaderSource.find("u_CulledLightDataGrid"), std::string::npos);
    EXPECT_NE(deferredLightingShaderSource.find("u_ForwardLocalLightBuffer"), std::string::npos);
    EXPECT_NE(deferredLightingShaderSource.find("u_NumCulledLightsGrid"), std::string::npos);
    EXPECT_NE(deferredLightingShaderSource.find("u_CulledLightDataGrid"), std::string::npos);
    EXPECT_EQ(source.find("packedLightsBuffer"), std::string::npos);
    EXPECT_EQ(commonShaderSource.find("StructuredBuffer<uint> packedLights"), std::string::npos);
    EXPECT_EQ(commonShaderSource.find("StructuredBuffer<uint> clusterRecords"), std::string::npos);
    EXPECT_EQ(commonShaderSource.find("StructuredBuffer<uint> compactIndices"), std::string::npos);
    EXPECT_EQ(commonShaderSource.find("LightGridPassConstants"), std::string::npos);
    EXPECT_EQ(standardShaderSource.find("u_LightGridLights"), std::string::npos);
    EXPECT_EQ(deferredLightingShaderSource.find("u_LightGridClusterRecords"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DeferredThreadedGBufferCaptureUsesPassBindingPlaceholder)
{
    const std::filesystem::path deferredRendererPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";

    std::ifstream deferredStream(deferredRendererPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(deferredStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto threadedBranch = source.find("if (usesThreadedRendering)");
    const auto gbufferLoop = source.find("for (const auto& entry : drawables.opaques)", threadedBranch);
    const auto lightingPass = source.find("m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>(\"u_GBufferAlbedo\"", gbufferLoop);
    const auto directLightingGBufferSet =
        source.find("m_lightingMaterial->GetParameterBlock().Set(\"u_GBuffer", gbufferLoop);
    const auto directLightingSkyboxSet =
        source.find("m_lightingMaterial->GetParameterBlock().Set(\"u_SkyboxCube", gbufferLoop);
    const auto placeholderBeforeGBuffer =
        source.find("SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder())", threadedBranch);
    const auto clearAfterLighting = source.find("SetActivePreparedPassBindingSet(nullptr)", lightingPass);

    ASSERT_NE(threadedBranch, std::string::npos);
    ASSERT_NE(gbufferLoop, std::string::npos);
    ASSERT_NE(lightingPass, std::string::npos);
    EXPECT_EQ(directLightingGBufferSet, std::string::npos);
    EXPECT_EQ(directLightingSkyboxSet, std::string::npos);
    EXPECT_LT(placeholderBeforeGBuffer, gbufferLoop);
    EXPECT_GT(clearAfterLighting, lightingPass);
}

TEST(RenderFrameworkContractTests, BaseSceneRendererCachesLightGridCompileContextPerFrame)
{
    const std::filesystem::path rendererHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.h";
    const std::filesystem::path rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(rendererHeaderPath);
    const std::string source = readFile(rendererSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    EXPECT_NE(headerSource.find("LightGridCompileContextCache"), std::string::npos);
    EXPECT_NE(headerSource.find("m_lightGridCompileContextCache"), std::string::npos);
    EXPECT_NE(source.find("InvalidateLightGridCompileContextCache();"), std::string::npos);
    EXPECT_NE(source.find("IsLightGridCompileContextCacheHit(frameSnapshot, hasSkyboxTexture)"), std::string::npos);
    EXPECT_NE(source.find("m_lightGridCompileContextCache.context = context"), std::string::npos);
}

TEST(RenderFrameworkContractTests, BaseSceneRendererLightGridContextCacheTracksCameraMotion)
{
    const std::filesystem::path rendererHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.h";
    const std::filesystem::path rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(rendererHeaderPath);
    const std::string source = readFile(rendererSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(source.empty());
    EXPECT_NE(headerSource.find("cameraPosition"), std::string::npos);
    EXPECT_NE(headerSource.find("cameraRotation"), std::string::npos);
    EXPECT_NE(source.find("m_lightGridCompileContextCache.cameraPosition = frameSnapshot.camera->GetPosition()"), std::string::npos);
    EXPECT_NE(source.find("m_lightGridCompileContextCache.cameraRotation = frameSnapshot.camera->GetRotation()"), std::string::npos);
    EXPECT_NE(source.find("Maths::Vector3::Distance(cached.cameraPosition, current.camera->GetPosition())"), std::string::npos);
    EXPECT_NE(source.find("cached.cameraRotation.w - current.camera->GetRotation().w"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DriverLightGridSettingDefaultsEnabledAndCanBeDisabled)
{
    NLS::Render::Settings::DriverSettings defaultSettings;
    defaultSettings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    NLS::Render::Context::Driver defaultDriver(defaultSettings);

    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::IsLightGridEnabled(defaultDriver));

    NLS::Render::Settings::DriverSettings disabledSettings;
    disabledSettings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    disabledSettings.enableLightGrid = false;
    NLS::Render::Context::Driver disabledDriver(disabledSettings);

    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::IsLightGridEnabled(disabledDriver));
}

TEST(RenderFrameworkContractTests, DisabledLightGridContextReturnsBeforePreparingDispatches)
{
    const std::filesystem::path rendererSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/BaseSceneRenderer.cpp";

    std::ifstream stream(rendererSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto disabledBranch = source.find("if (!NLS::Render::Context::DriverRendererAccess::IsLightGridEnabled(m_driver))");
    const auto preparedRequest = source.find("LightGridPrepass::BuildPreparedComputeRequest(");
    const auto preparedSource = source.find("LightGridPrepass::BuildPreparedComputeDispatchSource(preparedComputeRequest)");

    ASSERT_NE(disabledBranch, std::string::npos);
    ASSERT_NE(preparedRequest, std::string::npos);
    ASSERT_NE(preparedSource, std::string::npos);
    EXPECT_LT(disabledBranch, preparedRequest);
    EXPECT_LT(disabledBranch, preparedSource);
    EXPECT_EQ(source.find("return NLS::Render::FrameGraph::BuildLightGridCompileContext(\n\t\t\tframeSnapshot,\n\t\t\t{},\n\t\t\t{});", disabledBranch), std::string::npos);
    EXPECT_NE(source.find("EnsureFallbackGraphicsPassBindingSet(frameSnapshot, hasSkyboxTexture)", disabledBranch), std::string::npos);
    EXPECT_NE(source.find("GetLightGridGraphicsPassBindingSet()", disabledBranch), std::string::npos);
    EXPECT_NE(source.find("EnsureFallbackGraphicsPassBindingSet(frameSnapshot, hasSkyboxTexture)", preparedSource), std::string::npos);
}

TEST(RenderFrameworkContractTests, ThreadedDrainResultIsActionableForCallers)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings(true));
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    EXPECT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));
    EXPECT_TRUE(NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(driver));
}

TEST(RenderFrameworkContractTests, LightGridPipelineMemberCacheTracksPipelineCacheKey)
{
    const std::filesystem::path lightGridHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.h";
    const std::filesystem::path lightGridSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(lightGridHeaderPath);
    const std::string lightGridSource = readFile(lightGridSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(lightGridSource.empty());

    EXPECT_NE(headerSource.find("m_injectionPipelineKey"), std::string::npos);
    EXPECT_NE(headerSource.find("m_compactPipelineKey"), std::string::npos);
    EXPECT_NE(lightGridSource.find("MatchesPipelineCacheKey"), std::string::npos);
    EXPECT_EQ(lightGridSource.find("if (m_injectionPipeline != nullptr && m_compactPipeline != nullptr)"), std::string::npos);
}

TEST(RenderFrameworkContractTests, LightGridShaderUsesCellOwnedUEInjectionShape)
{
    const std::filesystem::path shaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridInjection.hlsl";
    const std::filesystem::path commonShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridCommon.hlsli";

    std::ifstream stream(shaderPath, std::ios::binary);
    const std::string shaderSource{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    std::ifstream commonStream(commonShaderPath, std::ios::binary);
    const std::string commonShaderSource{
        std::istreambuf_iterator<char>(commonStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shaderSource.empty());
    ASSERT_FALSE(commonShaderSource.empty());
    EXPECT_NE(shaderSource.find("[numthreads(4, 4, 4)]"), std::string::npos);
    EXPECT_NE(shaderSource.find("NLS_LIGHT_LINK_STRIDE"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("NLS_NUM_CULLED_LIGHTS_GRID_STRIDE"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("NLS_LIGHT_LINK_STRIDE"), std::string::npos);
    EXPECT_NE(shaderSource.find("NLSComputeCellViewAABB"), std::string::npos);
    EXPECT_NE(shaderSource.find("for (uint lightIndex = 0u; lightIndex < NLSGetSceneLightCount(); ++lightIndex)"), std::string::npos);
    EXPECT_NE(shaderSource.find("u_LightGridStartOffsetGrid"), std::string::npos);
    EXPECT_NE(shaderSource.find("u_LightGridCulledLightLinks"), std::string::npos);
    EXPECT_NE(shaderSource.find("InterlockedExchange(u_LightGridStartOffsetGrid[clusterIndex]"), std::string::npos);
    EXPECT_EQ(shaderSource.find("const uint lightIndex = dispatchThreadId.x;"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("u_LightGridClipToView"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("mul(float4(tileMin.x, tileMin.y, minDeviceZ, 1.0f), u_LightGridClipToView)"), std::string::npos);
    EXPECT_EQ(commonShaderSource.find("mul(u_LightGridInverseViewProjection, float4(tileMin.x, tileMin.y, minDeviceZ, 1.0f))"), std::string::npos);
}

TEST(RenderFrameworkContractTests, LightGridCommonAvoidsDxcReservedPointParameterName)
{
    const std::filesystem::path commonShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridCommon.hlsli";

    std::ifstream commonStream(commonShaderPath, std::ios::binary);
    const std::string commonShaderSource{
        std::istreambuf_iterator<char>(commonStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(commonShaderSource.empty());
    EXPECT_EQ(commonShaderSource.find("float3 point)"), std::string::npos);
    EXPECT_EQ(commonShaderSource.find("abs(point -"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("float3 queryPoint)"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DeferredLightingUsesFullSceneLightListForPhaseOneLighting)
{
    const std::filesystem::path commonShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridCommon.hlsli";
    const std::filesystem::path deferredLightingShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/DeferredLighting.hlsl";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string commonShaderSource = readFile(commonShaderPath);
    const std::string deferredLightingShaderSource = readFile(deferredLightingShaderPath);

    ASSERT_FALSE(commonShaderSource.empty());
    ASSERT_FALSE(deferredLightingShaderSource.empty());
    EXPECT_NE(commonShaderSource.find("NLSAccumulateSceneLightingPBR"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("for (uint lightIndex = 0u; lightIndex < NLSGetSceneLightCount(); ++lightIndex)"), std::string::npos);
    EXPECT_NE(deferredLightingShaderSource.find("NLSAccumulateSceneLightingPBR("), std::string::npos);
    EXPECT_EQ(deferredLightingShaderSource.find("NLSAccumulateClusteredLightingPBR("), std::string::npos);
}

TEST(RenderFrameworkContractTests, DeferredSceneLightingTreatsAmbientSphereAsGlobalAmbient)
{
    const std::filesystem::path commonShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridCommon.hlsli";

    std::ifstream commonStream(commonShaderPath, std::ios::binary);
    const std::string commonShaderSource{
        std::istreambuf_iterator<char>(commonStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(commonShaderSource.empty());
    EXPECT_NE(commonShaderSource.find("NLSIsGlobalDeferredLight"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("light.type == NLS_LIGHT_TYPE_AMBIENT_SPHERE"), std::string::npos);
    EXPECT_NE(commonShaderSource.find("NLSIsAmbientLight(light)"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DX12BindingSetAllowsSampledDepthTextureDescriptors)
{
    const std::filesystem::path descriptorSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Descriptor.cpp";

    std::ifstream stream(descriptorSourcePath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto writeDescriptorSwitch = source.find("void NativeDX12BindingSet::WriteResourceDescriptor");
    const auto textureBindingCase = source.find("case NLS::Render::RHI::BindingType::Texture:", writeDescriptorSwitch);
    const auto samplerCase = source.find("case NLS::Render::RHI::BindingType::Sampler:", textureBindingCase);
    ASSERT_NE(writeDescriptorSwitch, std::string::npos);
    ASSERT_NE(textureBindingCase, std::string::npos);
    ASSERT_NE(samplerCase, std::string::npos);

    const std::string textureBindingBlock =
        source.substr(textureBindingCase, samplerCase - textureBindingCase);
    EXPECT_EQ(textureBindingBlock.find("!NLS::Render::RHI::DX12::IsDepthStencilFormat(viewDesc->format)"), std::string::npos);
    EXPECT_EQ(textureBindingBlock.find("!NLS::Render::RHI::DX12::IsDepthStencilFormat(texture->GetDesc().format)"), std::string::npos);
    EXPECT_NE(textureBindingBlock.find("BuildDX12TextureViewDescriptorSet(texture->GetDesc(), *viewDesc)"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DeferredLightingReconstructsWorldPositionFromStoredDeviceDepth)
{
    const std::filesystem::path deferredLightingShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/DeferredLighting.hlsl";

    std::ifstream stream(deferredLightingShaderPath, std::ios::binary);
    const std::string shaderSource{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shaderSource.empty());
    const auto reconstructFunction = shaderSource.find("float3 ReconstructWorldPosition");
    const auto psMain = shaderSource.find("float4 PSMain", reconstructFunction);
    ASSERT_NE(reconstructFunction, std::string::npos);
    ASSERT_NE(psMain, std::string::npos);

    const std::string reconstructBlock = shaderSource.substr(reconstructFunction, psMain - reconstructFunction);
    EXPECT_EQ(reconstructBlock.find("depth01 * 2.0f - 1.0f"), std::string::npos);
    EXPECT_NE(reconstructBlock.find("const float clipZ = depth01;"), std::string::npos);
    EXPECT_NE(reconstructBlock.find("float4(clipXY, clipZ, 1.0f)"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DeferredLightingUsesProceduralSkyWhenNoSkyboxTextureIsBound)
{
    const std::filesystem::path deferredLightingShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/DeferredLighting.hlsl";

    std::ifstream stream(deferredLightingShaderPath, std::ios::binary);
    const std::string shaderSource{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shaderSource.empty());
    EXPECT_NE(shaderSource.find("float3 EvalProceduralSky"), std::string::npos);
    EXPECT_NE(shaderSource.find("return float4(EvalProceduralSky(skyDirection), 1.0f);"), std::string::npos);
    EXPECT_EQ(shaderSource.find("return float4(0.55f, 0.70f, 0.92f, 1.0f);"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DeferredLightingPresentsSceneAsOpaqueTexture)
{
    const std::filesystem::path deferredLightingShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/DeferredLighting.hlsl";

    std::ifstream stream(deferredLightingShaderPath, std::ios::binary);
    const std::string shaderSource{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shaderSource.empty());
    EXPECT_NE(shaderSource.find("return float4(litColor, 1.0f);"), std::string::npos);
    EXPECT_EQ(shaderSource.find("return float4(litColor, albedo.a);"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DeferredLightingHasVisibleAmbientFloorForUnlitImportedModels)
{
    const std::filesystem::path commonShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridCommon.hlsli";

    std::ifstream stream(commonShaderPath, std::ios::binary);
    const std::string shaderSource{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shaderSource.empty());
    EXPECT_NE(shaderSource.find("NLSGetVisibleAmbientFloor"), std::string::npos);
    EXPECT_NE(shaderSource.find("max(NLSGetAmbientFloor(), 0.18f)"), std::string::npos);
    EXPECT_NE(shaderSource.find("NLSGetVisibleAmbientFloor() * ao"), std::string::npos);
}

TEST(RenderFrameworkContractTests, DeferredLightingFlipsScreenYWhenReconstructingWorldSpace)
{
    const std::filesystem::path deferredLightingShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/DeferredLighting.hlsl";

    std::ifstream stream(deferredLightingShaderPath, std::ios::binary);
    const std::string shaderSource{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(shaderSource.empty());
    const auto helperFunction = shaderSource.find("float2 ToDeferredClipXY");
    const auto farReconstruct = shaderSource.find("float3 ReconstructFarWorldDirection", helperFunction);
    const auto positionReconstruct = shaderSource.find("float3 ReconstructWorldPosition", farReconstruct);
    const auto psMain = shaderSource.find("float4 PSMain", positionReconstruct);
    ASSERT_NE(helperFunction, std::string::npos);
    ASSERT_NE(farReconstruct, std::string::npos);
    ASSERT_NE(positionReconstruct, std::string::npos);
    ASSERT_NE(psMain, std::string::npos);

    const std::string helperBlock = shaderSource.substr(helperFunction, farReconstruct - helperFunction);
    EXPECT_NE(helperBlock.find("texCoord.x * 2.0f - 1.0f"), std::string::npos);
    EXPECT_NE(helperBlock.find("1.0f - texCoord.y * 2.0f"), std::string::npos);

    const std::string reconstructBlock = shaderSource.substr(farReconstruct, psMain - farReconstruct);
    EXPECT_EQ(reconstructBlock.find("texCoord * 2.0f - 1.0f"), std::string::npos);
    EXPECT_NE(reconstructBlock.find("const float2 clipXY = ToDeferredClipXY(texCoord);"), std::string::npos);
}

TEST(RenderFrameworkContractTests, LightGridLinkedListPathCapsGlobalLinksLikeUESource)
{
    const std::filesystem::path injectionShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridInjection.hlsl";
    const std::filesystem::path compactShaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/LightGridCompact.hlsl";

    std::ifstream injectionStream(injectionShaderPath, std::ios::binary);
    std::ifstream compactStream(compactShaderPath, std::ios::binary);
    const std::string injectionShaderSource{
        std::istreambuf_iterator<char>(injectionStream),
        std::istreambuf_iterator<char>()};
    const std::string compactShaderSource{
        std::istreambuf_iterator<char>(compactStream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(injectionShaderSource.empty());
    ASSERT_FALSE(compactShaderSource.empty());
    EXPECT_NE(injectionShaderSource.find("NLSGetGridSizeX() * NLSGetGridSizeY() * NLSGetGridSizeZ() * NLSGetMaxLightsPerCluster()"), std::string::npos);
    EXPECT_NE(injectionShaderSource.find("if (nextLink < maxAvailableLinks)"), std::string::npos);
    EXPECT_NE(compactShaderSource.find("while (linkOffset != 0xFFFFFFFFu && count < NLSGetSceneLightCount())"), std::string::npos);
    EXPECT_NE(compactShaderSource.find("InterlockedAdd(u_LightGridCompactCounter[0], count, offset)"), std::string::npos);
}

TEST(RenderFrameworkContractTests, LightGridPrepassUsesShaderParameterStructsForComputeShaders)
{
    const std::filesystem::path lightGridHeaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.h";
    const std::filesystem::path lightGridSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/LightGridPrepass.cpp";

    const auto readFile = [](const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };

    const std::string headerSource = readFile(lightGridHeaderPath);
    const std::string lightGridSource = readFile(lightGridSourcePath);

    ASSERT_FALSE(headerSource.empty());
    ASSERT_FALSE(lightGridSource.empty());

    EXPECT_NE(headerSource.find("LightGridResetParameters"), std::string::npos);
    EXPECT_NE(headerSource.find("LightGridInjectionParameters"), std::string::npos);
    EXPECT_NE(headerSource.find("LightGridCompactParameters"), std::string::npos);
    EXPECT_NE(headerSource.find("LightGridGraphicsParameters"), std::string::npos);
    EXPECT_NE(headerSource.find("GlobalShader"), std::string::npos);
    EXPECT_NE(headerSource.find("Forward"), std::string::npos);
    EXPECT_NE(headerSource.find("NumCulledLightsGrid"), std::string::npos);
    EXPECT_NE(headerSource.find("CulledLightDataGrid"), std::string::npos);
    EXPECT_NE(headerSource.find("RWNumCulledLightsGrid"), std::string::npos);
    EXPECT_NE(headerSource.find("RWCulledLightDataGrid"), std::string::npos);
    EXPECT_NE(headerSource.find("RWStartOffsetGrid"), std::string::npos);
    EXPECT_NE(headerSource.find("RWCulledLightLinks"), std::string::npos);

    const auto& shaderRegistry = NLS::Render::Resources::GetShaderTypeRegistry();
    ASSERT_NE(shaderRegistry.FindByName("LightGridResetCS"), nullptr);
    ASSERT_NE(shaderRegistry.FindByName("LightGridInjectionCS"), nullptr);
    ASSERT_NE(shaderRegistry.FindByName("LightGridCompactCS"), nullptr);
    EXPECT_FALSE(shaderRegistry.FindByName("LightGridResetCS")->GetRootParameterStructs().empty());
    EXPECT_FALSE(shaderRegistry.FindByName("LightGridInjectionCS")->GetRootParameterStructs().empty());
    EXPECT_FALSE(shaderRegistry.FindByName("LightGridCompactCS")->GetRootParameterStructs().empty());
    EXPECT_NE(lightGridSource.find("ShaderParameterStructBuilder(\"LightGridGraphicsParameters\")"), std::string::npos);
    EXPECT_NE(lightGridSource.find("ComputeShaderUtils::CreatePassBindingLayout"), std::string::npos);
    EXPECT_NE(lightGridSource.find("ComputeShaderUtils::CreateComputePipeline"), std::string::npos);
    EXPECT_NE(lightGridSource.find("BuildBindingSetDescFromShaderParameters"), std::string::npos);
    EXPECT_NE(lightGridSource.find("ComputeShaderUtils::BuildRecordedDispatch"), std::string::npos);

    EXPECT_EQ(lightGridSource.find("makePassSetLayoutDesc"), std::string::npos);
    EXPECT_EQ(lightGridSource.find("auto createComputePipeline ="), std::string::npos);
    EXPECT_EQ(lightGridSource.find("graphicsSetDesc.entries ="), std::string::npos);
}

TEST(RenderFrameworkContractTests, UiCompositionContractPropagatesSignalToPresentAndClearsAfterSubmit)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto swapchain = std::make_shared<ContractSwapchain>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);

    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    auto commandPool = std::make_shared<ContractCommandPool>();
    auto frameFence = std::make_shared<ContractFence>();
    auto imageAcquiredSemaphore = std::make_shared<ContractSemaphore>();
    auto renderFinishedSemaphore = std::make_shared<ContractSemaphore>();
    commandPool->commandBuffer = commandBuffer;

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = commandBuffer;
    frameContext.commandPool = commandPool;
    frameContext.frameFence = frameFence;
    frameContext.imageAcquiredSemaphore = imageAcquiredSemaphore;
    frameContext.renderFinishedSemaphore = renderFinishedSemaphore;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::CanBeginStandaloneExplicitFrame(driver));
    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, true);

    auto boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    ASSERT_TRUE(boundary.sceneToUiWaitSemaphore.IsValid());
    EXPECT_EQ(boundary.sceneToUiWaitSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(boundary.sceneToUiWaitSemaphore.handle, renderFinishedSemaphore.get());
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());

    const NLS::Render::RHI::NativeHandle uiSignalSemaphore{
        NLS::Render::RHI::BackendType::DX12,
        reinterpret_cast<void*>(0x9876)
    };
    constexpr uint64_t uiSignalValue = 77u;
    NLS::Render::Context::DriverUIAccess::SetUICompositionSignal(
        driver,
        uiSignalSemaphore,
        uiSignalValue);

    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, true);

    const auto queue = explicitDevice->GetContractQueue();
    ASSERT_EQ(queue->submitCalls, 1u);
    ASSERT_EQ(queue->presentCalls, 1u);
    ASSERT_EQ(queue->lastPresentDesc.waitSemaphores.size(), 1u);
    EXPECT_EQ(queue->lastPresentDesc.waitSemaphores.front(), renderFinishedSemaphore);
    EXPECT_EQ(queue->lastPresentDesc.uiSignalSemaphore.backend, NLS::Render::RHI::BackendType::DX12);
    EXPECT_EQ(queue->lastPresentDesc.uiSignalSemaphore.handle, uiSignalSemaphore.handle);
    EXPECT_EQ(queue->lastPresentDesc.uiSignalValue, uiSignalValue);

    boundary = NLS::Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(driver);
    EXPECT_FALSE(boundary.uiToPresentSignalSemaphore.IsValid());
    EXPECT_EQ(boundary.uiToPresentSignalValue, 0u);
}

TEST(RenderFrameworkContractTests, StandaloneRenderDocCaptureBeginsBeforeCommandRecording)
{
    const std::filesystem::path coordinatorPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Context/RhiThreadCoordinator.cpp";

    std::ifstream stream(coordinatorPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto beginStandaloneFrame =
        source.find("void RhiThreadCoordinator::BeginStandaloneExplicitFrame");
    const auto endStandaloneFrame =
        source.find("void RhiThreadCoordinator::EndStandaloneExplicitFrame", beginStandaloneFrame);
    ASSERT_NE(beginStandaloneFrame, std::string::npos);
    ASSERT_NE(endStandaloneFrame, std::string::npos);

    const auto commandRecordingBegin =
        source.find("frameContext.commandBuffer->Begin()", beginStandaloneFrame);
    const auto renderDocPreFrame =
        source.find("renderDocCaptureController->OnPreFrame(acquireSwapchainImage)", beginStandaloneFrame);

    ASSERT_NE(commandRecordingBegin, std::string::npos);
    ASSERT_LT(commandRecordingBegin, endStandaloneFrame);
    ASSERT_NE(renderDocPreFrame, std::string::npos);
    ASSERT_LT(renderDocPreFrame, endStandaloneFrame);
    EXPECT_LT(renderDocPreFrame, commandRecordingBegin);
}

TEST(RenderFrameworkContractTests, StandaloneUiRenderDocCaptureBeginsBeforeCommandRecording)
{
    const std::filesystem::path coordinatorPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Context/RhiThreadCoordinator.cpp";

    std::ifstream stream(coordinatorPath, std::ios::binary);
    const std::string source{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};

    ASSERT_FALSE(source.empty());
    const auto beginStandaloneFrame = source.find("bool BeginStandaloneUiExplicitFrame");
    const auto endStandaloneFrame = source.find("bool PresentStandaloneUiFrame", beginStandaloneFrame);
    ASSERT_NE(beginStandaloneFrame, std::string::npos);
    ASSERT_NE(endStandaloneFrame, std::string::npos);

    const auto commandRecordingBegin =
        source.find("frameContext.commandBuffer->Begin()", beginStandaloneFrame);
    const auto renderDocPreFrame =
        source.find("renderDocCaptureController->OnPreFrame(true)", beginStandaloneFrame);

    ASSERT_NE(commandRecordingBegin, std::string::npos);
    ASSERT_LT(commandRecordingBegin, endStandaloneFrame);
    ASSERT_NE(renderDocPreFrame, std::string::npos);
    ASSERT_LT(renderDocPreFrame, endStandaloneFrame);
    EXPECT_LT(renderDocPreFrame, commandRecordingBegin);
}

TEST(RenderFrameworkContractTests, FrameGraphResourceLifetimeContractKeepsTransientResourcesUntilRetiredFrame)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 9u;
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        commandBuffer.get(),
        &frameContext
    };

    NLS::Render::FrameGraph::FrameGraphTexture texture;
    NLS::Render::FrameGraph::FrameGraphTexture::Desc textureDesc;
    textureDesc.extent = { 64u, 64u, 1u };
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    textureDesc.debugName = "ContractTransientColor";

    NLS::Render::FrameGraph::FrameGraphBuffer buffer;
    NLS::Render::FrameGraph::FrameGraphBuffer::Desc bufferDesc;
    bufferDesc.size = 256u;
    bufferDesc.type = NLS::Render::RHI::BufferType::ShaderStorage;
    bufferDesc.usage = NLS::Render::RHI::BufferUsage::DynamicDraw;
    bufferDesc.debugName = "ContractTransientLightList";

    texture.create(textureDesc, &executionContext);
    buffer.create(bufferDesc, &executionContext);

    std::weak_ptr<NLS::Render::RHI::RHITexture> weakTexture = texture.explicitTexture;
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakTextureView = texture.explicitView;
    std::weak_ptr<NLS::Render::RHI::RHIBuffer> weakBuffer = buffer.explicitBuffer;

    auto stats = frameContext.resourceStateTracker->GetStats();
    EXPECT_EQ(stats.transientTextureRegistrations, 1u);
    EXPECT_EQ(stats.transientTextureViewRegistrations, 1u);
    EXPECT_EQ(stats.transientBufferRegistrations, 1u);

    texture.destroy(textureDesc, &executionContext);
    buffer.destroy(bufferDesc, &executionContext);
    EXPECT_EQ(texture.explicitTexture, nullptr);
    EXPECT_EQ(texture.explicitView, nullptr);
    EXPECT_EQ(buffer.explicitBuffer, nullptr);
    EXPECT_FALSE(weakTexture.expired());
    EXPECT_FALSE(weakTextureView.expired());
    EXPECT_FALSE(weakBuffer.expired());

    frameContext.resourceStateTracker->RetireTransientResources(frameContext.frameIndex - 1u);
    EXPECT_FALSE(weakTexture.expired());
    EXPECT_FALSE(weakTextureView.expired());
    EXPECT_FALSE(weakBuffer.expired());

    frameContext.resourceStateTracker->RetireTransientResources(frameContext.frameIndex);
    stats = frameContext.resourceStateTracker->GetStats();
    EXPECT_EQ(stats.retiredTransientTextures, 1u);
    EXPECT_EQ(stats.retiredTransientTextureViews, 1u);
    EXPECT_EQ(stats.retiredTransientBuffers, 1u);
    EXPECT_TRUE(weakTexture.expired());
    EXPECT_TRUE(weakTextureView.expired());
    EXPECT_TRUE(weakBuffer.expired());
}

TEST(RenderFrameworkContractTests, FrameGraphUniformBufferUsesGenericReadStateForExplicitTracking)
{
    NLS::Render::Context::Driver driver(MakeContractDriverSettings());
    auto explicitDevice = std::make_shared<ContractDevice>();
    auto commandBuffer = std::make_shared<ContractCommandBuffer>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 11u;
    frameContext.commandBuffer = commandBuffer;
    frameContext.resourceStateTracker = NLS::Render::RHI::CreateDefaultResourceStateTracker();
    frameContext.resourceStateTracker->BeginFrame(frameContext.frameIndex);

    NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
        driver,
        explicitDevice.get(),
        commandBuffer.get(),
        &frameContext
    };

    NLS::Render::FrameGraph::FrameGraphBuffer buffer;
    NLS::Render::FrameGraph::FrameGraphBuffer::Desc desc;
    desc.size = 256u;
    desc.type = NLS::Render::RHI::BufferType::Uniform;
    desc.usage = NLS::Render::RHI::BufferUsage::DynamicDraw;
    desc.debugName = "ContractTransientUniform";
    buffer.create(desc, &executionContext);

    ASSERT_NE(buffer.explicitBuffer, nullptr);
    EXPECT_EQ(buffer.explicitBuffer->GetDesc().memoryUsage, NLS::Render::RHI::MemoryUsage::CPUToGPU);

    buffer.preRead(desc, 0u, &executionContext);
    EXPECT_EQ(commandBuffer->barrierCalls, 0u);

    commandBuffer->lastBarrierDesc = {};
    buffer.preWrite(desc, 0u, &executionContext);
    EXPECT_EQ(commandBuffer->barrierCalls, 0u);
}

TEST(RenderFrameworkContractTests, UE427RhiCommandListSubmissionMetadataTracksLifecycleAndChildOrder)
{
    NLS::Render::RHI::RHICommandListSubmissionMetadata emptyList;
    emptyList.debugName = "EmptyImmediate";
    emptyList.queueType = NLS::Render::RHI::QueueType::Graphics;
    emptyList.lifecycleState = NLS::Render::RHI::RHICommandListLifecycleState::Closed;

    EXPECT_TRUE(emptyList.IsSubmittable());
    EXPECT_FALSE(emptyList.HasVisibleWork());
    EXPECT_EQ(emptyList.commandCount, 0u);
    EXPECT_EQ(emptyList.visibleDrawCount, 0u);

    NLS::Render::RHI::RHICommandListSubmissionMetadata basePass;
    basePass.debugName = "BasePass";
    basePass.queueType = NLS::Render::RHI::QueueType::Graphics;
    basePass.lifecycleState = NLS::Render::RHI::RHICommandListLifecycleState::Recording;
    basePass.RecordCommand(NLS::Render::RHI::RHICommandListCommandKind::DrawIndexed, 2u);

    EXPECT_FALSE(basePass.IsSubmittable());
    EXPECT_TRUE(basePass.HasVisibleWork());
    EXPECT_EQ(basePass.commandCount, 1u);
    EXPECT_EQ(basePass.visibleDrawCount, 2u);

    basePass.Close();
    EXPECT_EQ(basePass.lifecycleState, NLS::Render::RHI::RHICommandListLifecycleState::Closed);
    EXPECT_TRUE(basePass.IsSubmittable());

    NLS::Render::RHI::RHICommandListSubmissionMetadata lightGrid;
    lightGrid.debugName = "LightGrid";
    lightGrid.queueType = NLS::Render::RHI::QueueType::Compute;
    lightGrid.lifecycleState = NLS::Render::RHI::RHICommandListLifecycleState::Closed;
    lightGrid.RecordCommand(NLS::Render::RHI::RHICommandListCommandKind::Dispatch, 0u);

    NLS::Render::RHI::RHICommandListSubmissionMetadata immediate;
    immediate.debugName = "Immediate";
    immediate.queueType = NLS::Render::RHI::QueueType::Graphics;
    immediate.lifecycleState = NLS::Render::RHI::RHICommandListLifecycleState::Recording;
    immediate.QueueChildSubmission(lightGrid);
    immediate.QueueChildSubmission(basePass);
    immediate.Close();

    ASSERT_EQ(immediate.childSubmissions.size(), 2u);
    EXPECT_EQ(immediate.childSubmissions[0].debugName, "LightGrid");
    EXPECT_EQ(immediate.childSubmissions[0].queueType, NLS::Render::RHI::QueueType::Compute);
    EXPECT_EQ(immediate.childSubmissions[0].submitOrder, 0u);
    EXPECT_EQ(immediate.childSubmissions[1].debugName, "BasePass");
    EXPECT_EQ(immediate.childSubmissions[1].queueType, NLS::Render::RHI::QueueType::Graphics);
    EXPECT_EQ(immediate.childSubmissions[1].submitOrder, 1u);
    EXPECT_EQ(immediate.commandCount, 2u);
    EXPECT_EQ(immediate.visibleDrawCount, 2u);
    EXPECT_TRUE(immediate.HasVisibleWork());
}
