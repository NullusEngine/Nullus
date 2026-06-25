#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "Guid.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/UI/RHIImGuiFontAtlas.h"
#include "Rendering/UI/RHIImGuiOverlayRenderer.h"
#include "Rendering/UI/UiDrawDataSnapshot.h"
#include "ImGui/imgui.h"

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#endif

namespace
{
    constexpr auto kUiTexturePreviousFrameOrStatic =
        NLS::Render::UI::UiTextureSynchronizationScope::PreviousFrameOrStatic;

    class TestUiOverlayCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "TestUiOverlayCommandBuffer"; }
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        bool IsRecording() const override { return false; }
        NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
        void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetViewport(const NLS::Render::RHI::RHIViewport& viewport) override
        {
            ++setViewportCalls;
            lastViewport = viewport;
        }
        void SetScissor(const NLS::Render::RHI::RHIRect2D& scissor) override
        {
            ++setScissorCalls;
            scissors.push_back(scissor);
        }
        void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline) override
        {
            ++bindGraphicsPipelineCalls;
            lastGraphicsPipeline = pipeline;
            events.push_back("BindGraphicsPipeline");
        }
        void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
        void BindBindingSet(
            const uint32_t setIndex,
            const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override
        {
            ++bindBindingSetCalls;
            lastBindingSetIndex = setIndex;
            lastBindingSet = bindingSet;
            boundBindingSets.push_back(bindingSet);
            events.push_back("BindBindingSet");
        }
        void PushConstants(
            NLS::Render::RHI::ShaderStageMask stageMask,
            uint32_t offset,
            uint32_t size,
            const void* data) override
        {
            ++pushConstantsCalls;
            lastPushConstantsStageMask = stageMask;
            lastPushConstantsOffset = offset;
            lastPushConstantsSize = size;
            lastPushConstantsBytes.resize(size);
            if (data != nullptr && size != 0u)
                std::memcpy(lastPushConstantsBytes.data(), data, size);
            events.push_back("PushConstants");
        }
        void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override
        {
            ++bindVertexBufferCalls;
        }
        void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override
        {
            ++bindIndexBufferCalls;
        }
        void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override { ++drawCalls; }
        void DrawIndexed(
            const uint32_t indexCount,
            uint32_t,
            const uint32_t firstIndex,
            const int32_t vertexOffset,
            uint32_t) override
        {
            ++drawIndexedCalls;
            events.push_back("DrawIndexed");
            drawIndexedIndexCounts.push_back(indexCount);
            drawIndexedFirstIndices.push_back(firstIndex);
            drawIndexedVertexOffsets.push_back(vertexOffset);
        }
        NLS::Render::RHI::RHICommandRecordingResult DrawIndexedChecked(
            const uint32_t indexCount,
            const uint32_t instanceCount,
            const uint32_t firstIndex,
            const int32_t vertexOffset,
            const uint32_t firstInstance) override
        {
            DrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
            return {};
        }
        void Dispatch(uint32_t, uint32_t, uint32_t) override {}
        void CopyBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const NLS::Render::RHI::RHIBufferCopyRegion&) override {}
        void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override
        {
            ++copyBufferToTextureCalls;
            lastBufferToTextureCopyDesc = desc;
            events.push_back("CopyBufferToTexture");
        }
        void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrierDesc) override
        {
            ++barrierCalls;
            lastBarrierDesc = barrierDesc;
            barrierDescs.push_back(barrierDesc);
            events.push_back("Barrier");
        }

        uint32_t drawCalls = 0u;
        uint32_t drawIndexedCalls = 0u;
        uint32_t setViewportCalls = 0u;
        uint32_t setScissorCalls = 0u;
        uint32_t bindGraphicsPipelineCalls = 0u;
        uint32_t bindBindingSetCalls = 0u;
        uint32_t bindVertexBufferCalls = 0u;
        uint32_t bindIndexBufferCalls = 0u;
        uint32_t pushConstantsCalls = 0u;
        uint32_t barrierCalls = 0u;
        uint32_t copyBufferToTextureCalls = 0u;
        NLS::Render::RHI::ShaderStageMask lastPushConstantsStageMask = NLS::Render::RHI::ShaderStageMask::None;
        uint32_t lastPushConstantsOffset = 0u;
        uint32_t lastPushConstantsSize = 0u;
        NLS::Render::RHI::RHIBarrierDesc lastBarrierDesc {};
        std::vector<NLS::Render::RHI::RHIBarrierDesc> barrierDescs;
        NLS::Render::RHI::RHIBufferToTextureCopyDesc lastBufferToTextureCopyDesc {};
        NLS::Render::RHI::RHIViewport lastViewport {};
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> lastGraphicsPipeline;
        uint32_t lastBindingSetIndex = 0u;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> lastBindingSet;
        std::vector<NLS::Render::RHI::RHIRect2D> scissors;
        std::vector<uint32_t> drawIndexedIndexCounts;
        std::vector<uint32_t> drawIndexedFirstIndices;
        std::vector<int32_t> drawIndexedVertexOffsets;
        std::vector<uint8_t> lastPushConstantsBytes;
        std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> boundBindingSets;
        std::vector<std::string> events;
    };

    struct ImGuiContextGuard
    {
        ImGuiContextGuard()
        {
            IMGUI_CHECKVERSION();
            context = ImGui::CreateContext();
            ImGui::GetIO().DisplaySize = ImVec2(320.0f, 200.0f);
            ImGui::GetIO().Fonts->AddFontDefault();
        }

        ~ImGuiContextGuard()
        {
            ImGui::DestroyContext(context);
        }

        ImGuiContext* context = nullptr;
    };

    class TestUiOverlayBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        explicit TestUiOverlayBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::GenericRead; }
        uint64_t GetGPUAddress() const override { return 0u; }
        NLS::Render::RHI::RHIUpdateResult UpdateData(
            const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
        {
            ++updateCalls;
            lastUpload = uploadDesc;
            return updateResult;
        }

        uint32_t updateCalls = 0u;
        NLS::Render::RHI::RHIBufferUploadDesc lastUpload {};
        NLS::Render::RHI::RHIUpdateResult updateResult { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
    };

    class TestUiOverlayAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "TestUiOverlayAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override
        {
            return NLS::Render::RHI::NativeBackendType::DX12;
        }
        std::string_view GetVendor() const override { return "Test"; }
        std::string_view GetHardware() const override { return "Test"; }
    };

    class TestUiOverlayGraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
    {
    public:
        explicit TestUiOverlayGraphicsPipeline(NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc {};
    };

    class TestUiOverlayPipelineLayout final : public NLS::Render::RHI::RHIPipelineLayout
    {
    public:
        explicit TestUiOverlayPipelineLayout(NLS::Render::RHI::RHIPipelineLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIPipelineLayoutDesc m_desc {};
    };

    class TestUiOverlayShaderModule final : public NLS::Render::RHI::RHIShaderModule
    {
    public:
        explicit TestUiOverlayShaderModule(NLS::Render::RHI::RHIShaderModuleDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIShaderModuleDesc m_desc {};
    };

    class TestUiBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
    {
    public:
        explicit TestUiBindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingLayoutDesc m_desc {};
    };

    class TestUiTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit TestUiTexture(NLS::Render::RHI::RHITextureDesc desc = {})
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return "RHIUiOverlayPassTestsTexture"; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override
        {
            return NLS::Render::RHI::ResourceState::ShaderRead;
        }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class TestUiTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        explicit TestUiTextureView(
            std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
            NLS::Render::RHI::RHITextureViewDesc desc = {})
            : m_texture(std::move(texture))
            , m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return "RHIUiOverlayPassTestsTextureView"; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    };

    class TestUiSampler final : public NLS::Render::RHI::RHISampler
    {
    public:
        explicit TestUiSampler(NLS::Render::RHI::SamplerDesc desc = {})
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return "RHIUiOverlayPassTestsSampler"; }
        const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::SamplerDesc m_desc {};
    };

    class TestUiBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        explicit TestUiBindingSet(NLS::Render::RHI::RHIBindingSetDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc {};
    };

    class TestUiOverlayDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        using NLS::Render::RHI::RHIDevice::CreateBuffer;
        using NLS::Render::RHI::RHIDevice::CreateTexture;

        TestUiOverlayDevice()
            : m_adapter(std::make_shared<TestUiOverlayAdapter>())
        {
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Graphics, true);
        }

        std::string_view GetDebugName() const override { return "TestUiOverlayDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override
        {
            NLS::Render::RHI::NativeRenderDeviceInfo info;
            info.backend = NLS::Render::RHI::NativeBackendType::DX12;
            return info;
        }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(
            const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            auto buffer = std::make_shared<TestUiOverlayBuffer>(desc);
            createdBuffers.push_back(buffer);
            return buffer;
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override
        {
            ++createTextureCalls;
            lastTextureDesc = desc;
            lastTextureUploadDesc = uploadDesc;
            if (!allowTextureCreation)
                return nullptr;

            auto texture = std::make_shared<TestUiTexture>(desc);
            createdTextures.push_back(texture);
            return texture;
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHITextureViewDesc& desc) override
        {
            ++createTextureViewCalls;
            lastTextureViewDesc = desc;
            lastTextureViewTexture = texture;
            if (!allowTextureViewCreation)
                return nullptr;

            createdTextureView = std::make_shared<TestUiTextureView>(texture, desc);
            return createdTextureView;
        }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
            const NLS::Render::RHI::SamplerDesc& desc,
            std::string = {}) override
        {
            ++createSamplerCalls;
            lastSamplerDesc = desc;
            if (!allowSamplerCreation)
                return nullptr;

            createdSampler = std::make_shared<TestUiSampler>(desc);
            return createdSampler;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
            const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override
        {
            ++createBindingLayoutCalls;
            lastBindingLayoutDesc = desc;
            if (!allowBindingLayoutCreation)
                return nullptr;

            createdBindingLayout = std::make_shared<TestUiBindingLayout>(desc);
            return createdBindingLayout;
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
            const NLS::Render::RHI::RHIBindingSetDesc& desc) override
        {
            ++createBindingSetCalls;
            lastBindingSetDesc = desc;
            bindingSetDescs.push_back(desc);
            if (!allowBindingSetCreation)
                return nullptr;

            createdBindingSet = std::make_shared<TestUiBindingSet>(desc);
            createdBindingSets.push_back(createdBindingSet);
            return createdBindingSet;
        }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
            const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override
        {
            ++createPipelineLayoutCalls;
            lastPipelineLayoutDesc = desc;
            if (!allowPipelineLayoutCreation)
                return nullptr;

            createdPipelineLayout = std::make_shared<TestUiOverlayPipelineLayout>(desc);
            return createdPipelineLayout;
        }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
            const NLS::Render::RHI::RHIShaderModuleDesc& desc) override
        {
            ++createShaderModuleCalls;
            shaderModuleDescs.push_back(desc);
            if ((desc.stage == NLS::Render::RHI::ShaderStage::Vertex && !allowVertexShaderModuleCreation) ||
                (desc.stage == NLS::Render::RHI::ShaderStage::Fragment && !allowFragmentShaderModuleCreation))
            {
                return nullptr;
            }

            auto shaderModule = std::make_shared<TestUiOverlayShaderModule>(desc);
            createdShaderModules.push_back(shaderModule);
            return shaderModule;
        }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
            const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override
        {
            ++createGraphicsPipelineCalls;
            lastGraphicsPipelineDesc = desc;
            if (!allowGraphicsPipelineCreation)
                return nullptr;

            createdGraphicsPipeline = std::make_shared<TestUiOverlayGraphicsPipeline>(desc);
            return createdGraphicsPipeline;
        }
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
            void*) override {}

        std::vector<std::shared_ptr<TestUiOverlayBuffer>> createdBuffers;
        std::vector<std::shared_ptr<TestUiTexture>> createdTextures;
        uint32_t createPipelineLayoutCalls = 0u;
        uint32_t createShaderModuleCalls = 0u;
        uint32_t createGraphicsPipelineCalls = 0u;
        uint32_t createBindingLayoutCalls = 0u;
        uint32_t createTextureCalls = 0u;
        uint32_t createTextureViewCalls = 0u;
        uint32_t createSamplerCalls = 0u;
        uint32_t createBindingSetCalls = 0u;
        bool allowPipelineLayoutCreation = true;
        bool allowVertexShaderModuleCreation = true;
        bool allowFragmentShaderModuleCreation = true;
        bool allowGraphicsPipelineCreation = true;
        bool allowBindingLayoutCreation = true;
        bool allowTextureCreation = true;
        bool allowTextureViewCreation = true;
        bool allowSamplerCreation = true;
        bool allowBindingSetCreation = true;
        NLS::Render::RHI::RHIPipelineLayoutDesc lastPipelineLayoutDesc {};
        NLS::Render::RHI::RHIBindingLayoutDesc lastBindingLayoutDesc {};
        std::vector<NLS::Render::RHI::RHIShaderModuleDesc> shaderModuleDescs;
        NLS::Render::RHI::RHIGraphicsPipelineDesc lastGraphicsPipelineDesc {};
        NLS::Render::RHI::RHITextureDesc lastTextureDesc {};
        NLS::Render::RHI::RHITextureUploadDesc lastTextureUploadDesc {};
        NLS::Render::RHI::RHITextureViewDesc lastTextureViewDesc {};
        std::shared_ptr<NLS::Render::RHI::RHITexture> lastTextureViewTexture;
        NLS::Render::RHI::SamplerDesc lastSamplerDesc {};
        NLS::Render::RHI::RHIBindingSetDesc lastBindingSetDesc {};
        std::vector<NLS::Render::RHI::RHIBindingSetDesc> bindingSetDescs;
        std::shared_ptr<TestUiOverlayPipelineLayout> createdPipelineLayout;
        std::shared_ptr<TestUiBindingLayout> createdBindingLayout;
        std::vector<std::shared_ptr<TestUiOverlayShaderModule>> createdShaderModules;
        std::shared_ptr<TestUiOverlayGraphicsPipeline> createdGraphicsPipeline;
        std::shared_ptr<NLS::Render::RHI::RHITextureView> createdTextureView;
        std::shared_ptr<NLS::Render::RHI::RHISampler> createdSampler;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> createdBindingSet;
        std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> createdBindingSets;

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    const TestUiOverlayBuffer* FindBufferWithUsage(
        const std::vector<std::shared_ptr<TestUiOverlayBuffer>>& buffers,
        const NLS::Render::RHI::BufferUsageFlags usageFlag)
    {
        for (const auto& buffer : buffers)
        {
            if (buffer != nullptr &&
                NLS::Render::RHI::HasBufferUsage(buffer->GetDesc().usage, usageFlag))
            {
                return buffer.get();
            }
        }
        return nullptr;
    }

    template <typename T>
    concept SupportsNoArgInvalidate = requires(T& atlas)
    {
        atlas.Invalidate();
    };

    class ScopedOverlayShaderManager final
    {
    public:
        ScopedOverlayShaderManager()
        {
            m_tempRoot = std::filesystem::temp_directory_path() /
                ("nullus_ui_overlay_shader_" + NLS::Guid::New().ToString());
            std::filesystem::create_directories(m_tempRoot);

            NLS::Render::Assets::ShaderArtifact artifact;
            artifact.sourcePath = "App/Assets/Engine/Shaders/RHIImGuiOverlay.hlsl";
            artifact.subAssetKey = "shader:rhi-imgui-overlay";
            artifact.stages.push_back({
                NLS::Render::ShaderCompiler::ShaderStage::Vertex,
                NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
                "VSMain",
                "vs_6_0",
                {
                    NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                    { 0x44u, 0x58u, 0x49u, 0x4cu, 0x56u, 0x53u },
                    {},
                    {},
                    "ui-overlay-vs-cache",
                    (m_tempRoot / "RHIImGuiOverlay.vs.dxil").string()
                }
            });
            artifact.stages.push_back({
                NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
                "PSMain",
                "ps_6_0",
                {
                    NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                    { 0x44u, 0x58u, 0x49u, 0x4cu, 0x50u, 0x53u },
                    {},
                    {},
                    "ui-overlay-ps-cache",
                    (m_tempRoot / "RHIImGuiOverlay.ps.dxil").string()
                }
            });

            const auto shaderArtifactPath = m_tempRoot / "e1ac067b4307ee69c2cae08bf3e7715d98799884c16e12156b66224707eecae9";
            const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
            std::ofstream output(shaderArtifactPath, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            output.close();

            auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderArtifactPath.string());
            m_shaderManager.RegisterResource(":Shaders/RHIImGuiOverlay.hlsl", shader);
            NLS::Core::ServiceLocator::Provide(m_shaderManager);
        }

        ~ScopedOverlayShaderManager()
        {
            NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
            std::filesystem::remove_all(m_tempRoot);
        }

        ScopedOverlayShaderManager(const ScopedOverlayShaderManager&) = delete;
        ScopedOverlayShaderManager& operator=(const ScopedOverlayShaderManager&) = delete;

    private:
        std::filesystem::path m_tempRoot;
        NLS::Core::ResourceManagement::ShaderManager m_shaderManager;
    };

    std::filesystem::path RepoPath(const char* relativePath)
    {
        return std::filesystem::path(NLS_ROOT_DIR) / relativePath;
    }

    std::string ReadSourceText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input.is_open()) << "Failed to open source file: " << path.string();
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    NLS::Render::UI::UiDrawDataSnapshot MakeTwoDrawListUiSnapshot()
    {
        NLS::Render::UI::UiDrawDataSnapshot snapshot;
        snapshot.frameId = 56u;
        snapshot.hasVisibleDraws = true;
        snapshot.displaySize[0] = 640.0f;
        snapshot.displaySize[1] = 360.0f;
        snapshot.framebufferScale[0] = 1.0f;
        snapshot.framebufferScale[1] = 1.0f;
        snapshot.totalVertexCount = 6u;
        snapshot.totalIndexCount = 6u;

        NLS::Render::UI::UiDrawListSnapshot firstDrawList;
        firstDrawList.vertices.resize(3u);
        firstDrawList.indices = { 0u, 1u, 2u };
        firstDrawList.commands.push_back({
            3u,
            0u,
            0u,
            { 0.0f, 0.0f, 32.0f, 32.0f },
            {},
            NLS::Render::UI::UiDrawCallbackKind::None,
            false
        });
        snapshot.drawLists.push_back(std::move(firstDrawList));

        NLS::Render::UI::UiDrawListSnapshot secondDrawList;
        secondDrawList.vertices.resize(3u);
        secondDrawList.indices = { 0u, 1u, 2u };
        secondDrawList.commands.push_back({
            3u,
            0u,
            0u,
            { 32.0f, 32.0f, 64.0f, 64.0f },
            {},
            NLS::Render::UI::UiDrawCallbackKind::None,
            false
        });
        snapshot.drawLists.push_back(std::move(secondDrawList));

        return snapshot;
    }
}

TEST(RHIUiOverlayPassTests, OverlayPassIdentityIsDeclaredInThreadedLifecycle)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/ThreadedRenderingLifecycle.h"));

    EXPECT_NE(source.find("UIOverlay"), std::string::npos)
        << "RenderPassCommandKind must expose UIOverlay for the migrated UI pass.";
}

TEST(RHIUiOverlayPassTests, OverlayRendererContractHeaderExists)
{
    EXPECT_TRUE(std::filesystem::exists(RepoPath("Runtime/Rendering/UI/RHIImGuiOverlayRenderer.h")))
        << "RHIImGuiOverlayRenderer must own migrated ImGui draw recording through RHI commands.";
}

TEST(RHIUiOverlayPassTests, DefaultDeviceCapabilitiesReportExplicitUiOverlayUnsupportedReason)
{
    NLS::Render::RHI::RHIDeviceCapabilities capabilities;

    const auto feature = capabilities.GetFeature(NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph);

    EXPECT_FALSE(feature.supported);
    EXPECT_NE(feature.reason.find("UI overlay FrameGraph"), std::string::npos)
        << "Default/non-migrated RHI devices must explain the UI overlay capability gap explicitly.";
}

TEST(RHIUiOverlayPassTests, DX12CapabilityPlumbingMarksUiOverlayRuntimeSelectableAfterUS3Validation)
{
#if defined(_WIN32)
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device creation is unavailable on this machine.";

    const auto feature =
        resources.capabilities.GetFeature(NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph);

    EXPECT_TRUE(feature.supported);
    EXPECT_NE(feature.reason.find("US1/US2/US3"), std::string::npos)
        << "DX12 UIOverlayFrameGraph support must record the validation gates that make the "
           "migrated product path selectable.";
#else
    GTEST_SKIP() << "DX12 capability plumbing is only available on Windows.";
#endif
}

TEST(RHIUiOverlayPassTests, UIOverlayCapabilityHelperExplainsMissingDevice)
{
    const auto feature = NLS::Render::RHI::GetUIOverlayFrameGraphFeature(nullptr);

    EXPECT_FALSE(feature.supported);
    EXPECT_NE(feature.reason.find("no active RHI device"), std::string::npos);
}

TEST(RHIUiOverlayPassTests, RenderScenePackageAndPassInputCarryUiOverlaySnapshotPayload)
{
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 42u;
    snapshot->hasVisibleDraws = true;

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::UIOverlay;
    passInput.uiDrawDataSnapshot = snapshot;

    NLS::Render::Context::RenderScenePackage package;
    package.uiDrawDataSnapshot = snapshot;
    package.hasUIOverlayPass = true;
    package.uiOverlayDrawCount = 3u;

    EXPECT_EQ(passInput.uiDrawDataSnapshot, snapshot);
    EXPECT_EQ(package.uiDrawDataSnapshot, snapshot);
    EXPECT_TRUE(package.hasUIOverlayPass);
    EXPECT_EQ(package.uiOverlayDrawCount, 3u);
}

TEST(RHIUiOverlayPassTests, UIOverlayFrameGraphMetadataUsesRecordedFinalOverlayContract)
{
    const auto metadata = NLS::Render::FrameGraph::MakeUIOverlayFrameGraphPassMetadata();

    EXPECT_EQ(metadata.commandKind, NLS::Render::Context::RenderPassCommandKind::UIOverlay);
    EXPECT_EQ(metadata.role, NLS::Render::FrameGraph::ThreadedRenderScenePassRole::UIOverlay);
    EXPECT_EQ(metadata.executionMode, NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded);
    EXPECT_EQ(metadata.queueType, NLS::Render::RHI::QueueType::Graphics);
    EXPECT_EQ(std::string_view(metadata.graphPassName), std::string_view("RHIFrameGraph::UIOverlay"));
    EXPECT_FALSE(metadata.propagatesColorOutput);
    EXPECT_FALSE(metadata.propagatesDepthOutput);
}

TEST(RHIUiOverlayPassTests, ApplyingExecutionPlanTracksUiOverlayDrawsSeparately)
{
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 77u;
    snapshot->hasVisibleDraws = true;

    NLS::Render::Context::RenderPassCommandInput uiPass;
    uiPass.kind = NLS::Render::Context::RenderPassCommandKind::UIOverlay;
    uiPass.debugName = "RHIFrameGraph::UIOverlay";
    uiPass.drawCount = 5u;
    uiPass.targetsSwapchain = true;
    uiPass.uiDrawDataSnapshot = snapshot;

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;
    plan.passes.push_back({
        uiPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::UIOverlay,
        NLS::Render::RHI::QueueType::Graphics,
        NLS::Render::Context::QueueDependencyPolicy::Previous,
        std::nullopt,
        false,
        "RHIFrameGraph::UIOverlay",
        uiPass.drawCount
    });

    NLS::Render::Context::RenderScenePackage package;
    NLS::Render::FrameGraph::ApplyThreadedRenderSceneExecutionPlan(package, plan);

    EXPECT_TRUE(package.hasVisibleDraws);
    EXPECT_TRUE(package.hasUIOverlayPass);
    EXPECT_EQ(package.visibleDrawCount, 5u);
    EXPECT_EQ(package.uiOverlayDrawCount, 5u);
    EXPECT_EQ(package.opaqueDrawCount, 0u);
    EXPECT_EQ(package.drawCommandCount, 5u);
    ASSERT_EQ(package.passCommandInputs.size(), 1u);
    EXPECT_EQ(package.passCommandInputs[0].uiDrawDataSnapshot, snapshot);
}

TEST(RHIUiOverlayPassTests, ApplyingExecutionPlanIncludesUiOverlayInDrawCommandCount)
{
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->hasVisibleDraws = true;

    NLS::Render::Context::RenderPassCommandInput scenePass;
    scenePass.kind = NLS::Render::Context::RenderPassCommandKind::Opaque;
    scenePass.drawCount = 2u;
    scenePass.recordedDrawCommands.resize(2u);

    NLS::Render::Context::RenderPassCommandInput uiPass;
    uiPass.kind = NLS::Render::Context::RenderPassCommandKind::UIOverlay;
    uiPass.drawCount = 3u;
    uiPass.uiDrawDataSnapshot = snapshot;

    NLS::Render::FrameGraph::ThreadedRenderSceneExecutionPlan plan;
    plan.passes.push_back({
        scenePass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
        NLS::Render::RHI::QueueType::Graphics,
        NLS::Render::Context::QueueDependencyPolicy::Previous,
        std::nullopt,
        false,
        "ThreadedOpaquePass",
        scenePass.drawCount
    });
    plan.passes.push_back({
        uiPass,
        NLS::Render::FrameGraph::ThreadedRenderScenePassRole::UIOverlay,
        NLS::Render::RHI::QueueType::Graphics,
        NLS::Render::Context::QueueDependencyPolicy::Previous,
        std::nullopt,
        false,
        NLS::Render::Context::kUIOverlayRenderPassDebugName,
        uiPass.drawCount
    });

    NLS::Render::Context::RenderScenePackage package;
    package.recordedDrawCommands.resize(2u);
    NLS::Render::FrameGraph::ApplyThreadedRenderSceneExecutionPlan(package, plan);

    EXPECT_EQ(package.visibleDrawCount, 5u);
    EXPECT_EQ(package.opaqueDrawCount, 2u);
    EXPECT_EQ(package.uiOverlayDrawCount, 3u);
    EXPECT_EQ(package.drawCommandCount, 5u);
}

TEST(RHIUiOverlayPassTests, VisibleUiSnapshotAppendsFinalOverlayPassAfterScenePass)
{
    NLS::Render::Context::FrameSnapshot frameSnapshot;
    frameSnapshot.frameId = 34u;
    frameSnapshot.targetsSwapchain = true;
    frameSnapshot.renderWidth = 1280u;
    frameSnapshot.renderHeight = 720u;
    frameSnapshot.visibleOpaqueDrawCount = 2u;
    frameSnapshot.recordedDrawCommands.resize(2u);

    auto package = NLS::Render::Context::BuildSnapshotOwnedRenderScenePackage(frameSnapshot);
    ASSERT_EQ(package.passCommandInputs.size(), 1u);
    EXPECT_EQ(package.passCommandInputs.front().kind, NLS::Render::Context::RenderPassCommandKind::Opaque);

    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 34u;
    snapshot->hasVisibleDraws = true;
    snapshot->displaySize[0] = 1280.0f;
    snapshot->displaySize[1] = 720.0f;
    snapshot->totalVertexCount = 3u;
    snapshot->totalIndexCount = 3u;
    NLS::Render::UI::UiDrawListSnapshot drawList;
    drawList.commands.push_back({ 3u, 0u, 0u, { 0.0f, 0.0f, 16.0f, 16.0f }, {}, NLS::Render::UI::UiDrawCallbackKind::None, false });
    snapshot->drawLists.push_back(std::move(drawList));

    ASSERT_TRUE(NLS::Render::Context::AttachUiOverlaySnapshotToRenderScenePackage(package, snapshot));

    ASSERT_EQ(package.passCommandInputs.size(), 2u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    const auto& uiPass = package.passCommandInputs[1];
    EXPECT_EQ(uiPass.kind, NLS::Render::Context::RenderPassCommandKind::UIOverlay);
    EXPECT_EQ(uiPass.debugName, NLS::Render::Context::kUIOverlayRenderPassDebugName);
    EXPECT_EQ(uiPass.uiDrawDataSnapshot, snapshot);
    EXPECT_EQ(uiPass.queueType, NLS::Render::RHI::QueueType::Graphics);
    EXPECT_EQ(uiPass.queueDependencyPolicy, NLS::Render::Context::QueueDependencyPolicy::Previous);
    EXPECT_TRUE(uiPass.targetsSwapchain);
    EXPECT_TRUE(uiPass.usesColorAttachment);
    EXPECT_FALSE(uiPass.usesDepthStencilAttachment);
    EXPECT_FALSE(uiPass.clearColor);
    EXPECT_FALSE(uiPass.clearDepth);
    EXPECT_FALSE(uiPass.clearStencil);
    EXPECT_EQ(uiPass.renderWidth, 1280u);
    EXPECT_EQ(uiPass.renderHeight, 720u);
    EXPECT_TRUE(package.hasUIOverlayPass);
    EXPECT_EQ(package.uiDrawDataSnapshot, snapshot);
    EXPECT_EQ(package.uiOverlayDrawCount, 1u);
    EXPECT_EQ(package.passPlanCount, 2u);
    EXPECT_TRUE(package.containsCommandInputs);
}

TEST(RHIUiOverlayPassTests, EmptyUiSnapshotDoesNotAppendOverlayPass)
{
    NLS::Render::Context::FrameSnapshot frameSnapshot;
    frameSnapshot.frameId = 35u;
    frameSnapshot.targetsSwapchain = true;
    frameSnapshot.renderWidth = 1280u;
    frameSnapshot.renderHeight = 720u;
    frameSnapshot.visibleOpaqueDrawCount = 1u;
    frameSnapshot.recordedDrawCommands.resize(1u);

    auto package = NLS::Render::Context::BuildSnapshotOwnedRenderScenePackage(frameSnapshot);
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 35u;
    snapshot->hasVisibleDraws = false;

    EXPECT_FALSE(NLS::Render::Context::AttachUiOverlaySnapshotToRenderScenePackage(package, snapshot));
    EXPECT_FALSE(package.hasUIOverlayPass);
    EXPECT_EQ(package.passCommandInputs.size(), 1u);
    EXPECT_EQ(package.uiDrawDataSnapshot, nullptr);
}

TEST(RHIUiOverlayPassTests, OffscreenPackageDoesNotConsumeUiOverlaySnapshot)
{
    NLS::Render::Context::FrameSnapshot frameSnapshot;
    frameSnapshot.frameId = 116u;
    frameSnapshot.targetsSwapchain = false;
    frameSnapshot.renderWidth = 128u;
    frameSnapshot.renderHeight = 72u;
    frameSnapshot.visibleOpaqueDrawCount = 1u;

    auto package = NLS::Render::Context::BuildSnapshotOwnedRenderScenePackage(frameSnapshot);
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 117u;
    snapshot->hasVisibleDraws = true;
    snapshot->displaySize[0] = 128.0f;
    snapshot->displaySize[1] = 72.0f;
    snapshot->totalVertexCount = 3u;
    snapshot->totalIndexCount = 3u;
    NLS::Render::UI::UiDrawListSnapshot drawList;
    drawList.commands.push_back({ 3u, 0u, 0u, { 0.0f, 0.0f, 16.0f, 16.0f }, {}, NLS::Render::UI::UiDrawCallbackKind::None, false });
    snapshot->drawLists.push_back(std::move(drawList));

    EXPECT_FALSE(NLS::Render::Context::AttachUiOverlaySnapshotToRenderScenePackage(package, snapshot));
    EXPECT_FALSE(package.targetsSwapchain);
    EXPECT_FALSE(package.hasUIOverlayPass);
    EXPECT_EQ(package.uiDrawDataSnapshot, nullptr);
    EXPECT_TRUE(package.passCommandInputs.empty() ||
        std::none_of(
            package.passCommandInputs.begin(),
            package.passCommandInputs.end(),
            [](const NLS::Render::Context::RenderPassCommandInput& input)
            {
                return input.kind == NLS::Render::Context::RenderPassCommandKind::UIOverlay;
            }));
}

TEST(RHIUiOverlayPassTests, UiOverlayPassDeclaresSwapchainAccessPresentTransitionAndDependencyEdge)
{
    NLS::Render::Context::FrameSnapshot frameSnapshot;
    frameSnapshot.frameId = 41u;
    frameSnapshot.targetsSwapchain = true;
    frameSnapshot.renderWidth = 640u;
    frameSnapshot.renderHeight = 360u;
    frameSnapshot.visibleOpaqueDrawCount = 1u;
    frameSnapshot.recordedDrawCommands.resize(1u);

    auto package = NLS::Render::Context::BuildSnapshotOwnedRenderScenePackage(frameSnapshot);
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 41u;
    snapshot->hasVisibleDraws = true;
    snapshot->totalVertexCount = 3u;
    snapshot->totalIndexCount = 3u;
    NLS::Render::UI::UiDrawListSnapshot drawList;
    drawList.commands.push_back({ 3u, 0u, 0u, { 0.0f, 0.0f, 8.0f, 8.0f }, {}, NLS::Render::UI::UiDrawCallbackKind::None, false });
    snapshot->drawLists.push_back(std::move(drawList));

    ASSERT_TRUE(NLS::Render::Context::AttachUiOverlaySnapshotToRenderScenePackage(package, snapshot));

    ASSERT_EQ(package.passCommandInputs.size(), 2u);
    const auto& scenePass = package.passCommandInputs[0];
    const auto& uiPass = package.passCommandInputs[1];

    const auto uiSwapchainAccess = std::find_if(
        uiPass.textureResourceAccesses.begin(),
        uiPass.textureResourceAccesses.end(),
        [](const NLS::Render::Context::TextureResourceAccess& access)
        {
            return access.mode == NLS::Render::Context::ResourceAccessMode::Write &&
                access.state == NLS::Render::RHI::ResourceState::RenderTarget &&
                access.stages == NLS::Render::RHI::PipelineStageMask::RenderTarget &&
                access.access == NLS::Render::RHI::AccessMask::ColorAttachmentWrite &&
                access.subresourceRange.mipLevelCount != 0u &&
                access.subresourceRange.arrayLayerCount != 0u;
        });
    EXPECT_NE(uiSwapchainAccess, uiPass.textureResourceAccesses.end());

    ASSERT_FALSE(uiPass.exportedTextureVisibilityTransitions.empty());
    const auto& exportedTransition = uiPass.exportedTextureVisibilityTransitions.front();
    EXPECT_EQ(exportedTransition.before, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(exportedTransition.after, NLS::Render::RHI::ResourceState::Present);
    EXPECT_EQ(exportedTransition.sourceStages, NLS::Render::RHI::PipelineStageMask::RenderTarget);
    EXPECT_EQ(exportedTransition.destinationStages, NLS::Render::RHI::PipelineStageMask::Present);
    EXPECT_EQ(exportedTransition.sourceAccess, NLS::Render::RHI::AccessMask::ColorAttachmentWrite);
    EXPECT_EQ(exportedTransition.destinationAccess, NLS::Render::RHI::AccessMask::Present);
    EXPECT_NE(exportedTransition.subresourceRange.mipLevelCount, 0u);
    EXPECT_NE(exportedTransition.subresourceRange.arrayLayerCount, 0u);

    ASSERT_EQ(package.workUnitDependencyEdges.size(), 1u);
    const auto& edge = package.workUnitDependencyEdges.front();
    EXPECT_EQ(edge.sourceWorkUnitIndex, 0u);
    EXPECT_EQ(edge.targetWorkUnitIndex, 1u);
    EXPECT_EQ(edge.kind, NLS::Render::Context::ThreadedDependencyKind::ResourceVisibility);
    EXPECT_EQ(edge.resourceKind, NLS::Render::Context::ThreadedDependencyResourceKind::Texture);
    ASSERT_TRUE(edge.sourceTextureAccess.has_value());
    ASSERT_TRUE(edge.targetTextureAccess.has_value());
    EXPECT_EQ(edge.sourceTextureAccess->mode, NLS::Render::Context::ResourceAccessMode::Write);
    EXPECT_EQ(edge.targetTextureAccess->mode, NLS::Render::Context::ResourceAccessMode::Write);
    EXPECT_EQ(edge.targetTextureAccess->state, NLS::Render::RHI::ResourceState::RenderTarget);

    EXPECT_TRUE(scenePass.targetsSwapchain);
}

TEST(RHIUiOverlayPassTests, UiOverlayDependencyEdgeUsesSceneSwapchainWriterWhenScenePassHasMultipleTextureAccesses)
{
    NLS::Render::Context::FrameSnapshot frameSnapshot;
    frameSnapshot.frameId = 42u;
    frameSnapshot.targetsSwapchain = true;
    frameSnapshot.renderWidth = 640u;
    frameSnapshot.renderHeight = 360u;
    frameSnapshot.visibleOpaqueDrawCount = 1u;
    frameSnapshot.recordedDrawCommands.resize(1u);

    auto package = NLS::Render::Context::BuildSnapshotOwnedRenderScenePackage(frameSnapshot);
    ASSERT_EQ(package.passCommandInputs.size(), 1u);
    auto& scenePass = package.passCommandInputs[0];
    NLS::Render::Context::TextureResourceAccess sampledAccess;
    sampledAccess.mode = NLS::Render::Context::ResourceAccessMode::Read;
    sampledAccess.state = NLS::Render::RHI::ResourceState::ShaderRead;
    sampledAccess.stages = NLS::Render::RHI::PipelineStageMask::FragmentShader;
    sampledAccess.access = NLS::Render::RHI::AccessMask::ShaderRead;
    scenePass.textureResourceAccesses.insert(scenePass.textureResourceAccesses.begin(), sampledAccess);

    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 42u;
    snapshot->hasVisibleDraws = true;
    snapshot->totalVertexCount = 3u;
    snapshot->totalIndexCount = 3u;
    NLS::Render::UI::UiDrawListSnapshot drawList;
    drawList.commands.push_back({ 3u, 0u, 0u, { 0.0f, 0.0f, 8.0f, 8.0f }, {}, NLS::Render::UI::UiDrawCallbackKind::None, false });
    snapshot->drawLists.push_back(std::move(drawList));

    ASSERT_TRUE(NLS::Render::Context::AttachUiOverlaySnapshotToRenderScenePackage(package, snapshot));

    ASSERT_EQ(package.workUnitDependencyEdges.size(), 1u);
    const auto& edge = package.workUnitDependencyEdges.front();
    ASSERT_TRUE(edge.sourceTextureAccess.has_value());
    EXPECT_EQ(edge.sourceTextureAccess->mode, NLS::Render::Context::ResourceAccessMode::Write);
    EXPECT_EQ(edge.sourceTextureAccess->state, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(edge.sourceTextureAccess->stages, NLS::Render::RHI::PipelineStageMask::RenderTarget);
    EXPECT_EQ(edge.sourceTextureAccess->access, NLS::Render::RHI::AccessMask::ColorAttachmentWrite);
}

TEST(RHIUiOverlayPassTests, UiOverlayPassDoesNotDeclareNullDynamicVertexIndexBufferResources)
{
    NLS::Render::Context::FrameSnapshot frameSnapshot;
    frameSnapshot.frameId = 43u;
    frameSnapshot.targetsSwapchain = true;
    frameSnapshot.renderWidth = 640u;
    frameSnapshot.renderHeight = 360u;
    frameSnapshot.visibleOpaqueDrawCount = 1u;
    frameSnapshot.recordedDrawCommands.resize(1u);

    auto package = NLS::Render::Context::BuildSnapshotOwnedRenderScenePackage(frameSnapshot);
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 43u;
    snapshot->hasVisibleDraws = true;
    snapshot->totalVertexCount = 3u;
    snapshot->totalIndexCount = 3u;
    NLS::Render::UI::UiDrawListSnapshot drawList;
    drawList.vertices.resize(3u);
    drawList.indices = { 0u, 1u, 2u };
    drawList.commands.push_back({ 3u, 0u, 0u, { 0.0f, 0.0f, 8.0f, 8.0f }, {}, NLS::Render::UI::UiDrawCallbackKind::None, false });
    snapshot->drawLists.push_back(std::move(drawList));

    ASSERT_TRUE(NLS::Render::Context::AttachUiOverlaySnapshotToRenderScenePackage(package, snapshot));

    ASSERT_EQ(package.passCommandInputs.size(), 2u);
    const auto& uiPass = package.passCommandInputs[1];

    EXPECT_TRUE(uiPass.bufferResourceAccesses.empty())
        << "Per-frame UI dynamic buffers are allocated during overlay recording; the pass package must not "
           "declare null buffer resources that FrameGraph validation cannot reason about.";
}

TEST(RHIUiOverlayPassTests, DriverImplStoresPendingUiOverlaySnapshotBehindMutex)
{
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 91u;

    NLS::Render::Context::DriverImpl impl;
    {
        std::lock_guard lock(impl.pendingUiOverlaySnapshotMutex);
        impl.pendingUiOverlaySnapshot = snapshot;
        impl.pendingUiOverlaySnapshotFrameId = snapshot->frameId;
        ++impl.pendingUiOverlaySnapshotGeneration;
    }

    std::lock_guard lock(impl.pendingUiOverlaySnapshotMutex);
    EXPECT_EQ(impl.pendingUiOverlaySnapshot, snapshot);
    EXPECT_EQ(impl.pendingUiOverlaySnapshotFrameId, 91u);
    EXPECT_EQ(impl.pendingUiOverlaySnapshotGeneration, 1u);
}

TEST(RHIUiOverlayPassTests, DriverUIAccessDeclaresUiOverlaySnapshotAndTextureApis)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/DriverAccess.h"));

    EXPECT_NE(source.find("PublishUiDrawDataSnapshot"), std::string::npos);
    EXPECT_NE(source.find("ConsumePendingUiDrawDataSnapshot"), std::string::npos);
    EXPECT_NE(source.find("RegisterUiTextureView"), std::string::npos);
    EXPECT_NE(source.find("ReleaseUiTextureView"), std::string::npos);
}

TEST(RHIUiOverlayPassTests, DriverUIAccessPublishesAndConsumesUiOverlaySnapshot)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1u;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);

    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 123u;
    snapshot->hasVisibleDraws = true;

    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);

    const auto consumed = NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver);
    ASSERT_EQ(consumed, snapshot);
    EXPECT_EQ(consumed->frameId, 123u);
    EXPECT_TRUE(consumed->hasVisibleDraws);
    EXPECT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), nullptr);

    const auto invalidTextureId = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        nullptr,
        kUiTexturePreviousFrameOrStatic);
    EXPECT_FALSE(invalidTextureId.IsValid());
    NLS::Render::Context::DriverUIAccess::ReleaseUiTextureView(driver, nullptr);
}

TEST(RHIUiOverlayPassTests, PreparedSceneFrameBuilderConsumesPendingUiSnapshotWhenBuildingPackage)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/Driver.cpp"));

    const auto functionBody = source.substr(
        source.find("bool DriverRendererAccess::TryPublishPreparedFrameBuilder"),
        source.find("bool DriverRendererAccess::QueueStandalonePostSubmitBufferReadback") -
            source.find("bool DriverRendererAccess::TryPublishPreparedFrameBuilder"));

    const auto builderWrap = functionBody.find("originalRenderSceneBuilder = std::move(renderSceneBuilder)");
    const auto generation = functionBody.find("consumedUiSnapshotGeneration", builderWrap);
    const auto consume = functionBody.find("DriverUIAccess::ConsumePendingUiDrawDataSnapshot(", generation);
    const auto attach = functionBody.find("AttachUiOverlaySnapshotToRenderScenePackage(package, uiSnapshot)", consume);
    const auto publish = functionBody.find("RenderThreadCoordinator::TryPublishPreparedFrameBuilder", attach);

    ASSERT_NE(builderWrap, std::string::npos);
    ASSERT_NE(generation, std::string::npos);
    ASSERT_NE(consume, std::string::npos);
    ASSERT_NE(attach, std::string::npos);
    ASSERT_NE(publish, std::string::npos);
    EXPECT_LT(builderWrap, generation);
    EXPECT_LT(generation, consume);
    EXPECT_LT(consume, attach);
    EXPECT_LT(attach, publish);
}

TEST(RHIUiOverlayPassTests, PreparedSceneFrameBuilderRestoresUiSnapshotWhenPackageCannotAttachIfUnchanged)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/Driver.cpp"));

    const auto functionBody = source.substr(
        source.find("bool DriverRendererAccess::TryPublishPreparedFrameBuilder"),
        source.find("bool DriverRendererAccess::QueueStandalonePostSubmitBufferReadback") -
            source.find("bool DriverRendererAccess::TryPublishPreparedFrameBuilder"));

    const auto consume = functionBody.find("DriverUIAccess::ConsumePendingUiDrawDataSnapshot(");
    const auto attach = functionBody.find("AttachUiOverlaySnapshotToRenderScenePackage(package, uiSnapshot)", consume);
    const auto restore = functionBody.find("DriverUIAccess::RestoreConsumedUiDrawDataSnapshotIfUnchanged(", attach);
    const auto generation = functionBody.find("consumedUiSnapshotGeneration", restore);
    const auto publish = functionBody.find("RenderThreadCoordinator::TryPublishPreparedFrameBuilder", attach);

    ASSERT_NE(consume, std::string::npos);
    ASSERT_NE(attach, std::string::npos);
    ASSERT_NE(restore, std::string::npos);
    ASSERT_NE(generation, std::string::npos);
    ASSERT_NE(publish, std::string::npos);
    EXPECT_LT(attach, restore);
    EXPECT_LT(restore, generation);
    EXPECT_LT(generation, publish)
        << "If an offscreen package cannot own the swapchain UI overlay pass, the snapshot must remain "
           "pending for UI-only overlay only when no newer pending UI snapshot has replaced it.";
}

TEST(RHIUiOverlayPassTests, DriverUIAccessRegistersStableUiTextureIdentities)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1u;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    auto texture = std::make_shared<TestUiTexture>();
    auto textureView = std::make_shared<TestUiTextureView>(texture);

    const auto firstId = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        textureView,
        kUiTexturePreviousFrameOrStatic);
    const auto secondId = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        textureView,
        kUiTexturePreviousFrameOrStatic);

    EXPECT_TRUE(firstId.IsValid());
    EXPECT_EQ(secondId.value, firstId.value);
    EXPECT_EQ(secondId.generation, firstId.generation);

    NLS::Render::Context::DriverUIAccess::ReleaseUiTextureView(driver, textureView);
    const auto thirdId = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        textureView,
        kUiTexturePreviousFrameOrStatic);

    EXPECT_TRUE(thirdId.IsValid());
    EXPECT_NE(thirdId.value, firstId.value);
}

TEST(RHIUiOverlayPassTests, DriverUIAccessDoesNotResetUiTextureRetireFrameOnEmptySnapshotPublish)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1u;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    auto texture = std::make_shared<TestUiTexture>();
    auto textureView = std::make_shared<TestUiTextureView>(texture);
    const auto id = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        textureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(id.IsValid());

    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 77u;
    snapshot->hasVisibleDraws = true;
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);
    EXPECT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), snapshot);

    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, nullptr);
    NLS::Render::Context::DriverUIAccess::ReleaseUiTextureView(driver, textureView);

    EXPECT_FALSE(impl->uiTextureRegistry.Resolve(id).has_value());
    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u);

    impl->uiTextureRegistry.ReleaseRetiredTextureViewsUpTo(76u);
    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u);

    impl->uiTextureRegistry.ReleaseRetiredTextureViewsUpTo(77u);
    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 0u);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}

TEST(RHIUiOverlayPassTests, DeferredFrameScopedCleanupReleasesRetiredUiTextureViews)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1u;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    auto texture = std::make_shared<TestUiTexture>();
    auto textureView = std::make_shared<TestUiTextureView>(texture);
    const auto id = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        textureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(id.IsValid());
    impl->uiTextureRegistry.ReleaseTextureView(textureView, 11u);
    ASSERT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 11u;
    impl->deferredThreadedFrameScopedRetirementFrameContexts.insert(0u);
    impl->deferredUiTextureRetirementFrameIdsByFrameContext[0u] = 11u;

    EXPECT_TRUE(NLS::Render::Context::Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
        *impl,
        frameContext,
        0u));
    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 0u);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}

TEST(RHIUiOverlayPassTests, ThreadedCompletionRetiresUiTexturesOnlyForSubmittedUiSnapshotFrame)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1u;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    auto texture = std::make_shared<TestUiTexture>();
    auto textureView = std::make_shared<TestUiTextureView>(texture);
    const auto id = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        textureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(id.IsValid());

    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 19u;
    snapshot->hasVisibleDraws = true;
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);
    ASSERT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), snapshot);
    NLS::Render::Context::DriverUIAccess::ReleaseUiTextureView(driver, textureView);
    ASSERT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = 20u;
    submissionFrame.frameContextIndex = 0u;
    submissionFrame.submittedSuccessfully = true;
    submissionFrame.uiOverlaySnapshotFrameId = 0u;

    NLS::Render::Context::Detail::LogCompletedThreadedRhiSubmission(
        *impl,
        frameContext,
        submissionFrame);
    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u);

    submissionFrame.uiOverlaySnapshotFrameId = 19u;
    NLS::Render::Context::Detail::LogCompletedThreadedRhiSubmission(
        *impl,
        frameContext,
        submissionFrame);
    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 0u);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}

TEST(RHIUiOverlayPassTests, UncompletedPublishedUiSnapshotDoesNotRetireOnUnrelatedFrameCompletion)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1u;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    auto texture = std::make_shared<TestUiTexture>();
    auto textureView = std::make_shared<TestUiTextureView>(texture);
    const auto id = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        textureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(id.IsValid());

    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 19u;
    snapshot->hasVisibleDraws = true;
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);
    ASSERT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), snapshot);

    NLS::Render::Context::DriverUIAccess::ReleaseUiTextureView(driver, textureView);
    ASSERT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u);

    NLS::Render::Context::Detail::ReleaseRetiredUiTextureViewsForCompletedUiFrame(*impl, 18u);

    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u);

    NLS::Render::Context::Detail::ReleaseRetiredUiTextureViewsForCompletedUiFrame(*impl, 19u);

    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 0u);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}

TEST(RHIUiOverlayPassTests, StandaloneFrameCompletionPathsDoNotAdvanceUiTextureRetirementWatermark)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/RhiThreadCoordinator.cpp"));

    const auto finalizeStart = source.find("bool FinalizeStandaloneUiFrame");
    ASSERT_NE(finalizeStart, std::string::npos);
    const auto finalizeEnd = source.find("void FinalizeThreadedRhiFrameContext", finalizeStart);
    ASSERT_NE(finalizeEnd, std::string::npos);
    const auto finalizeBody = source.substr(finalizeStart, finalizeEnd - finalizeStart);
    EXPECT_EQ(
        finalizeBody.find("ReleaseRetiredUiTextureViewsForCompletedUiFrame"),
        std::string::npos);
    EXPECT_EQ(
        finalizeBody.find("ReleaseRetiredUiTextureViewsForCompletedFrame"),
        std::string::npos);
    EXPECT_EQ(
        finalizeBody.find("ReleaseRetiredTextureViewsUpTo"),
        std::string::npos);

    const auto explicitStart = source.find("void RhiThreadCoordinator::EndStandaloneExplicitFrame");
    ASSERT_NE(explicitStart, std::string::npos);
    const auto explicitEnd = source.find("bool RhiThreadCoordinator::TryExecuteNextThreadedSubmission", explicitStart);
    ASSERT_NE(explicitEnd, std::string::npos);
    const auto explicitBody = source.substr(explicitStart, explicitEnd - explicitStart);
    EXPECT_EQ(
        explicitBody.find("ReleaseRetiredUiTextureViewsForCompletedUiFrame"),
        std::string::npos);
    EXPECT_EQ(
        explicitBody.find("ReleaseRetiredUiTextureViewsForCompletedFrame"),
        std::string::npos);
    EXPECT_EQ(
        explicitBody.find("ReleaseRetiredTextureViewsUpTo"),
        std::string::npos);
}

TEST(RHIUiOverlayPassTests, UIOverlayFallbackPassNameUsesFrameGraphIdentity)
{
    NLS::Render::Context::RenderPassCommandInput input;
    input.kind = NLS::Render::Context::RenderPassCommandKind::UIOverlay;

    EXPECT_EQ(
        std::string_view(NLS::Render::Context::Detail::ToPassDebugName(input.kind)),
        std::string_view(NLS::Render::Context::kUIOverlayRenderPassDebugName));
    EXPECT_EQ(
        std::string_view(NLS::Render::Context::Detail::ResolvePassProfileScopeName(input)),
        std::string_view(NLS::Render::Context::kUIOverlayRenderPassDebugName));
}

TEST(RHIUiOverlayPassTests, UIOverlayCannotBeRecordedByGenericDrawPassPath)
{
    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->hasVisibleDraws = true;

    NLS::Render::Context::RenderScenePackage package;
    NLS::Render::Context::RenderPassCommandInput input;
    input.kind = NLS::Render::Context::RenderPassCommandKind::UIOverlay;
    input.debugName = NLS::Render::Context::kUIOverlayRenderPassDebugName;
    input.drawCount = 4u;
    input.usesColorAttachment = true;
    input.uiDrawDataSnapshot = snapshot;
    input.recordedDrawCommands.resize(4u);

    EXPECT_FALSE(NLS::Render::Context::Detail::IsPassRecordable(package, input));
    EXPECT_EQ(NLS::Render::Context::Detail::RecordPreparedDrawCommandsForPass(nullptr, input), 0u);
    EXPECT_EQ(
        NLS::Render::Context::Detail::RecordPreparedDrawCommandsForPassRange(
            nullptr,
            input,
            input.recordedDrawCommands,
            0u,
            4u),
        0u);
}

TEST(RHIUiOverlayPassTests, OverlayRendererRecordsVisibleDrawCommandsThroughRhiCommandBuffer)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    NLS::Render::UI::UiDrawDataSnapshot snapshot;
    snapshot.frameId = 55u;
    snapshot.hasVisibleDraws = true;
    snapshot.displayPos[0] = 0.0f;
    snapshot.displayPos[1] = 0.0f;
    snapshot.displaySize[0] = 640.0f;
    snapshot.displaySize[1] = 360.0f;
    snapshot.framebufferScale[0] = 1.0f;
    snapshot.framebufferScale[1] = 1.0f;
    snapshot.totalVertexCount = 3u;
    snapshot.totalIndexCount = 3u;
    NLS::Render::UI::UiDrawListSnapshot drawList;
    drawList.vertices.resize(3u);
    drawList.indices = { 0u, 1u, 2u };
    drawList.commands.push_back({
        3u,
        0u,
        0u,
        { 10.0f, 20.0f, 110.0f, 120.0f },
        {},
        NLS::Render::UI::UiDrawCallbackKind::None,
        false
    });
    snapshot.drawLists.push_back(std::move(drawList));

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto result = renderer.RecordPrepared(commandBuffer, snapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.recordedDraws);
    EXPECT_EQ(result.message, "recorded 1 UI draw command(s)");
    EXPECT_EQ(commandBuffer.drawCalls, 0u);
    EXPECT_EQ(commandBuffer.drawIndexedCalls, 1u);
    EXPECT_EQ(commandBuffer.setViewportCalls, 1u);
    EXPECT_EQ(commandBuffer.pushConstantsCalls, 1u);
    EXPECT_EQ(commandBuffer.lastPushConstantsStageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
    EXPECT_EQ(commandBuffer.lastPushConstantsOffset, 0u);
    EXPECT_EQ(commandBuffer.lastPushConstantsSize, 16u);
    EXPECT_FLOAT_EQ(commandBuffer.lastViewport.width, 640.0f);
    EXPECT_FLOAT_EQ(commandBuffer.lastViewport.height, 360.0f);
    ASSERT_EQ(commandBuffer.scissors.size(), 1u);
    EXPECT_EQ(commandBuffer.scissors[0].x, 10);
    EXPECT_EQ(commandBuffer.scissors[0].y, 20);
    EXPECT_EQ(commandBuffer.scissors[0].width, 100u);
    EXPECT_EQ(commandBuffer.scissors[0].height, 100u);
    ASSERT_EQ(commandBuffer.drawIndexedIndexCounts.size(), 1u);
    EXPECT_EQ(commandBuffer.drawIndexedIndexCounts[0], 3u);
    EXPECT_EQ(commandBuffer.drawIndexedFirstIndices[0], 0u);
    EXPECT_EQ(commandBuffer.drawIndexedVertexOffsets[0], 0);
}

TEST(RHIUiOverlayPassTests, OverlayRendererRejectsPreparedDrawsWithoutGraphicsPipeline)
{
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    device.allowGraphicsPipelineCreation = false;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto result = renderer.RecordPrepared(commandBuffer, snapshot, 0u);

    EXPECT_FALSE(prepareResult.success);
    EXPECT_NE(prepareResult.message.find("UI overlay graphics pipeline"), std::string::npos);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.recordedDraws);
    EXPECT_EQ(commandBuffer.bindGraphicsPipelineCalls, 0u);
    EXPECT_EQ(commandBuffer.drawIndexedCalls, 0u);
}

TEST(RHIUiOverlayPassTests, OverlayRendererRejectsPreparedDrawsWhenPipelineLayoutCreationFails)
{
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    device.allowPipelineLayoutCreation = false;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);

    EXPECT_FALSE(prepareResult.success);
    EXPECT_NE(prepareResult.message.find("UI overlay pipeline layout"), std::string::npos);
    EXPECT_EQ(device.createPipelineLayoutCalls, 1u);
    EXPECT_EQ(device.createShaderModuleCalls, 0u);
    EXPECT_EQ(device.createGraphicsPipelineCalls, 0u);
    EXPECT_EQ(commandBuffer.drawIndexedCalls, 0u);
}

TEST(RHIUiOverlayPassTests, OverlayRendererRejectsPreparedDrawsWhenVertexShaderModuleCreationFails)
{
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    device.allowVertexShaderModuleCreation = false;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);

    EXPECT_FALSE(prepareResult.success);
    EXPECT_NE(prepareResult.message.find("UI overlay vertex shader module"), std::string::npos);
    EXPECT_EQ(device.createPipelineLayoutCalls, 1u);
    EXPECT_EQ(device.createShaderModuleCalls, 1u);
    EXPECT_EQ(device.createGraphicsPipelineCalls, 0u);
    ASSERT_EQ(device.shaderModuleDescs.size(), 1u);
    EXPECT_EQ(device.shaderModuleDescs[0].stage, NLS::Render::RHI::ShaderStage::Vertex);
}

TEST(RHIUiOverlayPassTests, OverlayRendererRejectsPreparedDrawsWhenFragmentShaderModuleCreationFails)
{
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    device.allowFragmentShaderModuleCreation = false;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);

    EXPECT_FALSE(prepareResult.success);
    EXPECT_NE(prepareResult.message.find("UI overlay fragment shader module"), std::string::npos);
    EXPECT_EQ(device.createPipelineLayoutCalls, 1u);
    EXPECT_EQ(device.createShaderModuleCalls, 2u);
    EXPECT_EQ(device.createGraphicsPipelineCalls, 0u);
    ASSERT_EQ(device.shaderModuleDescs.size(), 2u);
    EXPECT_EQ(device.shaderModuleDescs[0].stage, NLS::Render::RHI::ShaderStage::Vertex);
    EXPECT_EQ(device.shaderModuleDescs[1].stage, NLS::Render::RHI::ShaderStage::Fragment);
}

TEST(RHIUiOverlayPassTests, OverlayRendererBindsGraphicsPipelineBeforePreparedDraws)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto result = renderer.RecordPrepared(commandBuffer, snapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    EXPECT_TRUE(result.success) << result.message;
    EXPECT_EQ(device.createPipelineLayoutCalls, 1u);
    EXPECT_EQ(device.lastPipelineLayoutDesc.debugName, "RHIImGuiOverlayPipelineLayout");
    ASSERT_EQ(device.lastPipelineLayoutDesc.pushConstants.size(), 1u);
    EXPECT_EQ(device.lastPipelineLayoutDesc.pushConstants[0].stageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
    EXPECT_EQ(device.lastPipelineLayoutDesc.pushConstants[0].offset, 0u);
    EXPECT_EQ(device.lastPipelineLayoutDesc.pushConstants[0].size, 16u);
    EXPECT_EQ(device.lastPipelineLayoutDesc.pushConstants[0].shaderRegister, 0u);
    EXPECT_EQ(device.lastPipelineLayoutDesc.pushConstants[0].registerSpace, 0u);
    EXPECT_EQ(device.createShaderModuleCalls, 2u);
    ASSERT_EQ(device.shaderModuleDescs.size(), 2u);
    EXPECT_EQ(device.shaderModuleDescs[0].stage, NLS::Render::RHI::ShaderStage::Vertex);
    EXPECT_EQ(device.shaderModuleDescs[0].entryPoint, "VSMain");
    EXPECT_FALSE(device.shaderModuleDescs[0].debugName.empty());
    EXPECT_EQ(device.shaderModuleDescs[0].shaderToolchainFingerprint.find("DXIL|vs_6_0|VSMain|"), 0u);
    EXPECT_NE(device.shaderModuleDescs[0].shaderToolchainFingerprint.find("RHIImGuiOverlay.vs.dxil"), std::string::npos);
    EXPECT_NE(device.shaderModuleDescs[0].shaderToolchainFingerprint, "RHIImGuiOverlay:placeholder");
    EXPECT_FALSE(device.shaderModuleDescs[0].bytecode.empty());
    EXPECT_EQ(device.shaderModuleDescs[1].stage, NLS::Render::RHI::ShaderStage::Fragment);
    EXPECT_EQ(device.shaderModuleDescs[1].entryPoint, "PSMain");
    EXPECT_FALSE(device.shaderModuleDescs[1].debugName.empty());
    EXPECT_EQ(device.shaderModuleDescs[1].shaderToolchainFingerprint.find("DXIL|ps_6_0|PSMain|"), 0u);
    EXPECT_NE(device.shaderModuleDescs[1].shaderToolchainFingerprint.find("RHIImGuiOverlay.ps.dxil"), std::string::npos);
    EXPECT_NE(device.shaderModuleDescs[1].shaderToolchainFingerprint, "RHIImGuiOverlay:placeholder");
    EXPECT_FALSE(device.shaderModuleDescs[1].bytecode.empty());
    EXPECT_EQ(device.createGraphicsPipelineCalls, 1u);
    EXPECT_EQ(device.lastGraphicsPipelineDesc.debugName, "RHIImGuiOverlayPipeline");
    EXPECT_EQ(device.lastGraphicsPipelineDesc.pipelineLayout, device.createdPipelineLayout);
    ASSERT_EQ(device.createdShaderModules.size(), 2u);
    EXPECT_EQ(device.lastGraphicsPipelineDesc.vertexShader, device.createdShaderModules[0]);
    EXPECT_EQ(device.lastGraphicsPipelineDesc.fragmentShader, device.createdShaderModules[1]);
    EXPECT_EQ(commandBuffer.bindGraphicsPipelineCalls, 1u);
    EXPECT_EQ(commandBuffer.lastGraphicsPipeline, device.createdGraphicsPipeline);
    ASSERT_FALSE(commandBuffer.events.empty());
    const auto bindIt = std::find(
        commandBuffer.events.begin(),
        commandBuffer.events.end(),
        "BindGraphicsPipeline");
    const auto drawIt = std::find(
        commandBuffer.events.begin(),
        commandBuffer.events.end(),
        "DrawIndexed");
    ASSERT_NE(bindIt, commandBuffer.events.end());
    const auto pushConstantsIt = std::find(
        commandBuffer.events.begin(),
        commandBuffer.events.end(),
        "PushConstants");
    ASSERT_NE(pushConstantsIt, commandBuffer.events.end());
    ASSERT_NE(drawIt, commandBuffer.events.end());
    EXPECT_LT(bindIt, drawIt);
    EXPECT_LT(pushConstantsIt, drawIt);
}

TEST(RHIUiOverlayPassTests, OverlayRendererUploadsAndBindsFontAtlasOnFirstUse)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto recordResult = renderer.RecordPrepared(commandBuffer, snapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    EXPECT_TRUE(recordResult.success) << recordResult.message;
    EXPECT_TRUE(renderer.FontAtlas().IsUploaded());
    EXPECT_EQ(device.createBindingLayoutCalls, 1u);
    ASSERT_EQ(device.lastBindingLayoutDesc.entries.size(), 2u);
    EXPECT_EQ(device.lastBindingLayoutDesc.entries[0].type, NLS::Render::RHI::BindingType::Texture);
    EXPECT_EQ(device.lastBindingLayoutDesc.entries[0].binding, 0u);
    EXPECT_EQ(device.lastBindingLayoutDesc.entries[1].type, NLS::Render::RHI::BindingType::Sampler);
    EXPECT_EQ(device.lastBindingLayoutDesc.entries[1].binding, 1u);
    EXPECT_EQ(device.createTextureCalls, 1u);
    EXPECT_EQ(device.lastTextureDesc.format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_TRUE(NLS::Render::RHI::HasTextureUsage(
        device.lastTextureDesc.usage,
        NLS::Render::RHI::TextureUsageFlags::Sampled));
    EXPECT_FALSE(device.lastTextureUploadDesc.HasData())
        << "The migrated overlay must not use RHIDevice::CreateTexture(initialData), because DX12 implements "
           "that path with a private upload command list/fence wait outside the frame graph.";
    EXPECT_EQ(commandBuffer.copyBufferToTextureCalls, 1u);
    EXPECT_EQ(commandBuffer.lastBufferToTextureCopyDesc.destination, device.createdTextures.front());
    EXPECT_EQ(commandBuffer.lastBufferToTextureCopyDesc.extent.width, device.lastTextureDesc.extent.width);
    EXPECT_EQ(commandBuffer.lastBufferToTextureCopyDesc.extent.height, device.lastTextureDesc.extent.height);
    EXPECT_EQ(commandBuffer.lastBufferToTextureCopyDesc.rowPitch % 256u, 0u);
    EXPECT_GE(commandBuffer.lastBufferToTextureCopyDesc.rowPitch, device.lastTextureDesc.extent.width * 4u);
    EXPECT_EQ(device.createTextureViewCalls, 1u);
    ASSERT_FALSE(device.createdTextures.empty());
    EXPECT_EQ(device.lastTextureViewTexture, device.createdTextures.front());
    EXPECT_EQ(device.lastTextureViewDesc.viewType, NLS::Render::RHI::TextureViewType::Texture2D);
    EXPECT_EQ(device.createSamplerCalls, 1u);
    EXPECT_EQ(device.lastSamplerDesc.minFilter, NLS::Render::RHI::TextureFilter::Linear);
    EXPECT_EQ(device.lastSamplerDesc.magFilter, NLS::Render::RHI::TextureFilter::Linear);
    EXPECT_EQ(device.lastSamplerDesc.wrapU, NLS::Render::RHI::TextureWrap::ClampToEdge);
    EXPECT_EQ(device.lastSamplerDesc.wrapV, NLS::Render::RHI::TextureWrap::ClampToEdge);
    EXPECT_EQ(device.createBindingSetCalls, 1u);
    ASSERT_EQ(device.lastBindingSetDesc.entries.size(), 2u);
    EXPECT_EQ(device.lastBindingSetDesc.entries[0].binding, 0u);
    EXPECT_EQ(device.lastBindingSetDesc.entries[0].type, NLS::Render::RHI::BindingType::Texture);
    EXPECT_EQ(device.lastBindingSetDesc.entries[0].textureView, device.createdTextureView);
    EXPECT_EQ(device.lastBindingSetDesc.entries[1].binding, 1u);
    EXPECT_EQ(device.lastBindingSetDesc.entries[1].type, NLS::Render::RHI::BindingType::Sampler);
    EXPECT_EQ(device.lastBindingSetDesc.entries[1].sampler, device.createdSampler);
    EXPECT_EQ(commandBuffer.bindBindingSetCalls, 1u);
    EXPECT_EQ(commandBuffer.lastBindingSetIndex, 0u);
    EXPECT_EQ(commandBuffer.lastBindingSet, device.createdBindingSet);

    const auto bindSetIt = std::find(
        commandBuffer.events.begin(),
        commandBuffer.events.end(),
        "BindBindingSet");
    const auto drawIt = std::find(
        commandBuffer.events.begin(),
        commandBuffer.events.end(),
        "DrawIndexed");
    ASSERT_NE(bindSetIt, commandBuffer.events.end());
    ASSERT_NE(drawIt, commandBuffer.events.end());
    EXPECT_LT(bindSetIt, drawIt);
}

TEST(RHIUiOverlayPassTests, SwapchainResizeNotificationDoesNotReleaseUncompletedUiResources)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1u;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    auto texture = std::make_shared<TestUiTexture>();
    auto textureView = std::make_shared<TestUiTextureView>(texture);
    const auto id = NLS::Render::Context::DriverUIAccess::RegisterUiTextureView(
        driver,
        textureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(id.IsValid());

    auto snapshot = std::make_shared<NLS::Render::UI::UiDrawDataSnapshot>();
    snapshot->frameId = 23u;
    snapshot->hasVisibleDraws = true;
    NLS::Render::Context::DriverUIAccess::PublishUiDrawDataSnapshot(driver, snapshot);
    ASSERT_EQ(NLS::Render::Context::DriverUIAccess::ConsumePendingUiDrawDataSnapshot(driver), snapshot);

    NLS::Render::Context::DriverUIAccess::ReleaseUiTextureView(driver, textureView);
    NLS::Render::RHI::RHIBindingSetDesc retiredBindingSetDesc;
    retiredBindingSetDesc.debugName = "RetiredFontAtlasBindingSet";
    impl->uiOverlayRenderer.FontAtlas().SetUploadedResourcesForTesting(
        texture,
        textureView,
        std::make_shared<TestUiSampler>(),
        std::make_shared<TestUiBindingSet>(retiredBindingSetDesc));
    NLS::Render::Context::DriverUIAccess::NotifyUiOverlayFontAtlasChanged(driver);
    ASSERT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u);
    ASSERT_EQ(impl->uiOverlayRenderer.FontAtlas().GetRetiredResourceCountForTesting(), 1u);

    NLS::Render::Context::DriverUIAccess::NotifyUiOverlaySwapchainWillResize(driver);

    EXPECT_EQ(impl->uiTextureRegistry.GetEntryCountForTesting(), 1u)
        << "Resize notifications must not treat the latest published UI snapshot id as a completed GPU fence.";
    EXPECT_EQ(impl->uiOverlayRenderer.FontAtlas().GetRetiredResourceCountForTesting(), 1u);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}

TEST(RHIUiOverlayPassTests, RecordPreparedKeepsPreparedFontAtlasBindingAfterInvalidation)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    ASSERT_TRUE(prepareResult.success) << prepareResult.message;
    const auto preparedBindingSet = device.createdBindingSet;
    ASSERT_NE(preparedBindingSet, nullptr);

    renderer.InvalidateFontAtlas(snapshot.frameId);

    const auto recordResult = renderer.RecordPrepared(commandBuffer, snapshot, 0u);

    EXPECT_TRUE(recordResult.success) << recordResult.message;
    EXPECT_EQ(commandBuffer.lastBindingSet, preparedBindingSet)
        << "A font-atlas invalidation between prepare and record must not make the prepared frame bind "
           "a different or empty atlas resource.";
}

TEST(RHIUiOverlayPassTests, OverlayRendererRebuildsFontAtlasAndRegisteredTextureBindingsWhenDeviceChanges)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer(&registry);
    TestUiOverlayDevice firstDevice;
    TestUiOverlayDevice secondDevice;
    TestUiOverlayCommandBuffer firstCommandBuffer;
    TestUiOverlayCommandBuffer secondCommandBuffer;
    auto snapshot = MakeTwoDrawListUiSnapshot();
    auto registeredTexture = std::make_shared<TestUiTexture>();
    auto registeredTextureView = std::make_shared<TestUiTextureView>(registeredTexture);
    const auto registeredId = registry.RegisterTextureView(
        registeredTextureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(registeredId.IsValid());
    snapshot.drawLists[1].commands[0].textureId = registeredId;

    const auto firstPrepareResult = renderer.PrepareFrameResources(firstDevice, firstCommandBuffer, snapshot, 0u);
    ASSERT_TRUE(firstPrepareResult.success) << firstPrepareResult.message;
    ASSERT_GE(firstDevice.createdBindingSets.size(), 2u);
    const auto firstFontBindingSet = firstDevice.createdBindingSets[0];
    const auto firstRegisteredTextureBindingSet = firstDevice.createdBindingSets[1];
    const auto firstFontTextureView = firstDevice.createdTextureView;

    const auto secondPrepareResult = renderer.PrepareFrameResources(secondDevice, secondCommandBuffer, snapshot, 0u);

    EXPECT_TRUE(secondPrepareResult.success) << secondPrepareResult.message;
    ASSERT_GE(secondDevice.createdBindingSets.size(), 2u);
    EXPECT_NE(secondDevice.createdTextureView, firstFontTextureView);
    EXPECT_NE(secondDevice.createdBindingSets[0], firstFontBindingSet);
    EXPECT_NE(secondDevice.createdBindingSets[1], firstRegisteredTextureBindingSet);

    const auto secondRecordResult = renderer.RecordPrepared(secondCommandBuffer, snapshot, 0u);
    EXPECT_TRUE(secondRecordResult.success) << secondRecordResult.message;
    EXPECT_EQ(secondCommandBuffer.boundBindingSets[0], secondDevice.createdBindingSets[0]);
    EXPECT_EQ(secondCommandBuffer.boundBindingSets[1], secondDevice.createdBindingSets[1]);
}

TEST(RHIUiOverlayPassTests, OverlayRendererBindsRegisteredTexturePerDrawWithoutNativeDescriptors)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer(&registry);
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    auto snapshot = MakeTwoDrawListUiSnapshot();
    auto registeredTexture = std::make_shared<TestUiTexture>();
    auto registeredTextureView = std::make_shared<TestUiTextureView>(registeredTexture);
    const auto registeredId = registry.RegisterTextureView(
        registeredTextureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(registeredId.IsValid());
    snapshot.drawLists[1].commands[0].textureId = registeredId;

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto recordResult = renderer.RecordPrepared(commandBuffer, snapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    EXPECT_TRUE(recordResult.success) << recordResult.message;
    ASSERT_EQ(device.createdBindingSets.size(), 2u)
        << "Expected one binding set for the font atlas and one for the registered UI texture.";
    ASSERT_EQ(device.bindingSetDescs.size(), 2u);
    const auto& textureBindingDesc = device.bindingSetDescs[1];
    EXPECT_EQ(textureBindingDesc.debugName, "RHIImGuiTextureBindingSet");
    ASSERT_EQ(textureBindingDesc.entries.size(), 2u);
    EXPECT_EQ(textureBindingDesc.entries[0].binding, 0u);
    EXPECT_EQ(textureBindingDesc.entries[0].type, NLS::Render::RHI::BindingType::Texture);
    EXPECT_EQ(textureBindingDesc.entries[0].textureView, registeredTextureView);
    EXPECT_EQ(textureBindingDesc.entries[1].binding, 1u);
    EXPECT_EQ(textureBindingDesc.entries[1].type, NLS::Render::RHI::BindingType::Sampler);
    EXPECT_EQ(textureBindingDesc.entries[1].sampler, device.createdSampler);

    ASSERT_EQ(commandBuffer.drawIndexedCalls, 2u);
    ASSERT_GE(commandBuffer.boundBindingSets.size(), 2u);
    EXPECT_EQ(commandBuffer.boundBindingSets[0], device.createdBindingSets[0]);
    EXPECT_EQ(commandBuffer.boundBindingSets[1], device.createdBindingSets[1]);
    EXPECT_EQ(commandBuffer.lastBindingSetIndex, 0u);
}

TEST(RHIUiOverlayPassTests, OverlayRendererExposesPreparedResourcesForFrameGraphVisibility)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer(&registry);
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    auto snapshot = MakeTwoDrawListUiSnapshot();
    auto registeredTexture = std::make_shared<TestUiTexture>();
    auto registeredTextureView = std::make_shared<TestUiTextureView>(registeredTexture);
    const auto registeredId = registry.RegisterTextureView(
        registeredTextureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(registeredId.IsValid());
    snapshot.drawLists[1].commands[0].textureId = registeredId;

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto resources = renderer.GetPreparedResourceSnapshot(snapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    ASSERT_GE(device.createdBuffers.size(), 3u);
    ASSERT_NE(resources.vertexBuffer, nullptr);
    ASSERT_NE(resources.indexBuffer, nullptr);
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(
        resources.vertexBuffer->GetDesc().usage,
        NLS::Render::RHI::BufferUsageFlags::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(
        resources.indexBuffer->GetDesc().usage,
        NLS::Render::RHI::BufferUsageFlags::Index));
    EXPECT_EQ(resources.fontAtlasTextureView, device.createdTextureView);
    EXPECT_TRUE(resources.fontAtlasUploadTransitionRequired);
    ASSERT_EQ(resources.registeredTextureViews.size(), 1u);
    EXPECT_EQ(resources.registeredTextureViews[0], registeredTextureView);
}

TEST(RHIUiOverlayPassTests, OverlayRendererRecordsRegisteredTextureShaderReadVisibility)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer(&registry);
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    auto snapshot = MakeTwoDrawListUiSnapshot();
    auto registeredTexture = std::make_shared<TestUiTexture>();
    auto registeredTextureView = std::make_shared<TestUiTextureView>(registeredTexture);
    const auto registeredId = registry.RegisterTextureView(
        registeredTextureView,
        kUiTexturePreviousFrameOrStatic);
    ASSERT_TRUE(registeredId.IsValid());
    snapshot.drawLists[1].commands[0].textureId = registeredId;

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto resources = renderer.GetPreparedResourceSnapshot(snapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    ASSERT_EQ(resources.registeredTextureViews.size(), 1u);
    EXPECT_EQ(resources.registeredTextureViews[0], registeredTextureView);

    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/RhiThreadCoordinator.cpp"));
    const auto appendAccesses = source.find("AppendUiOverlayPreparedResourceAccesses(");
    const auto readTransition = source.find("MakeUiOverlayTextureReadTransition(");
    ASSERT_NE(appendAccesses, std::string::npos);
    ASSERT_NE(readTransition, std::string::npos);
    EXPECT_NE(source.find("registeredTextureViews", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("MakeUiOverlaySampledTextureAccess(textureView)", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("MakeUiOverlayTextureReadTransition(textureView)", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("ShaderRead", readTransition), std::string::npos)
        << "Registered UI textures must be promoted to ShaderRead visibility before BeginPass.";
}

TEST(RHIUiOverlayPassTests, OverlayRendererRequiresFontAtlasUploadVisibilityOnlyOnFirstUpload)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer firstCommandBuffer;
    TestUiOverlayCommandBuffer secondCommandBuffer;
    auto firstSnapshot = MakeTwoDrawListUiSnapshot();
    auto secondSnapshot = MakeTwoDrawListUiSnapshot();
    secondSnapshot.frameId = firstSnapshot.frameId + 1u;

    const auto firstPrepareResult = renderer.PrepareFrameResources(device, firstCommandBuffer, firstSnapshot, 0u);
    const auto firstResources = renderer.GetPreparedResourceSnapshot(firstSnapshot, 0u);
    const auto secondPrepareResult = renderer.PrepareFrameResources(device, secondCommandBuffer, secondSnapshot, 0u);
    const auto secondResources = renderer.GetPreparedResourceSnapshot(secondSnapshot, 0u);

    EXPECT_TRUE(firstPrepareResult.success) << firstPrepareResult.message;
    EXPECT_TRUE(secondPrepareResult.success) << secondPrepareResult.message;
    EXPECT_TRUE(firstResources.fontAtlasUploadTransitionRequired);
    EXPECT_FALSE(secondResources.fontAtlasUploadTransitionRequired)
        << "Atlas upload-to-read visibility should only be recorded when the atlas upload/rebuild happened.";
    EXPECT_EQ(device.createTextureCalls, 1u);
}

TEST(RHIUiOverlayPassTests, RhiThreadUiOverlayPassInputAddsPreparedConcreteResourcesBeforeBeginPass)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/RhiThreadCoordinator.cpp"));
    const auto appendAccesses = source.find("AppendUiOverlayPreparedResourceAccesses(");
    const auto prepareInput = source.find("std::optional<RenderPassCommandInput> PrepareUiOverlayPassInput(");
    ASSERT_NE(appendAccesses, std::string::npos);
    ASSERT_NE(prepareInput, std::string::npos);
    EXPECT_LT(appendAccesses, prepareInput)
        << "The concrete prepared-resource injection helper should be defined before the pass-input "
           "builder that consumes it.";

    EXPECT_NE(source.find("resources.vertexBuffer", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("resources.indexBuffer", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("resources.fontAtlasTextureView", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("resources.fontAtlasUploadTransitionRequired", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("resources.registeredTextureViews", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("MakeUiOverlaySampledTextureAccess", appendAccesses), std::string::npos);
    EXPECT_NE(source.find("MakeUiOverlayFontAtlasUploadToReadTransition", appendAccesses), std::string::npos);

    const auto prepareResources = source.find("PrepareFrameResources(", prepareInput);
    const auto appendPreparedAccesses = source.find("AppendUiOverlayPreparedResourceAccesses(", prepareInput);
    const auto preparedSnapshot = source.find("GetPreparedResourceSnapshot(", prepareInput);
    ASSERT_NE(prepareResources, std::string::npos);
    ASSERT_NE(appendPreparedAccesses, std::string::npos);
    ASSERT_NE(preparedSnapshot, std::string::npos);
    EXPECT_LT(prepareResources, appendPreparedAccesses)
        << "Concrete UI resources must exist before the pass input declares FrameGraph-visible accesses.";
    EXPECT_LT(appendPreparedAccesses, preparedSnapshot);

    size_t searchFrom = 0u;
    size_t prepareCallCount = 0u;
    while ((searchFrom = source.find("PrepareUiOverlayPassInput(", searchFrom)) != std::string::npos)
    {
        ++prepareCallCount;
        searchFrom += std::string_view("PrepareUiOverlayPassInput(").size();
    }
    EXPECT_GE(prepareCallCount, 4u)
        << "Expected the helper definition plus threaded, serial, and parallel UI overlay recording call sites.";

    const auto firstRecordVisibility = source.find("RecordResourceVisibilityTransitions", prepareInput);
    const auto firstBeginPlan = source.find("BeginPassCommandPlan");
    const auto firstPrepareInputCall = source.find("PrepareUiOverlayPassInput(", prepareInput + 1u);
    ASSERT_NE(firstBeginPlan, std::string::npos);
    ASSERT_NE(firstPrepareInputCall, std::string::npos);
    ASSERT_NE(firstRecordVisibility, std::string::npos);
    EXPECT_LT(firstPrepareInputCall, firstBeginPlan);
    EXPECT_LT(firstPrepareInputCall, firstRecordVisibility)
        << "UIOverlay resource preparation must append sampled texture/font transitions before visibility barriers are recorded.";
    EXPECT_LT(firstRecordVisibility, firstBeginPlan)
        << "Prepared UIOverlay sampled-resource barriers must be recorded before opening the render pass.";
}

TEST(RHIUiOverlayPassTests, RhiThreadUiOverlayPassInputMaterializesSwapchainBackbufferBeforeVisibility)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/RhiThreadCoordinator.cpp"));
    const auto materializeFunction = source.find("MaterializeUiOverlaySwapchainResourceAccesses(");
    const auto materializeEdgesFunction = source.find("MaterializeUiOverlayDependencyEdges(");
    const auto prepareInput = source.find("std::optional<RenderPassCommandInput> PrepareUiOverlayPassInput(");
    ASSERT_NE(materializeFunction, std::string::npos);
    ASSERT_NE(materializeEdgesFunction, std::string::npos);
    ASSERT_NE(prepareInput, std::string::npos);

    const auto backbufferTexture = source.find("swapchainBackbufferView->GetTexture()", materializeFunction);
    ASSERT_NE(backbufferTexture, std::string::npos)
        << "UIOverlay must replace package-time swapchain intent with the acquired backbuffer texture.";
    EXPECT_NE(source.find("access.texture = backbufferTexture", materializeFunction), std::string::npos);
    EXPECT_NE(source.find("transition.texture = backbufferTexture", materializeFunction), std::string::npos);

    const auto materializeCall = source.find("MaterializeUiOverlaySwapchainResourceAccesses(", prepareInput + 1u);
    const auto appendPreparedAccesses = source.find("AppendUiOverlayPreparedResourceAccesses(", prepareInput);
    ASSERT_NE(materializeCall, std::string::npos);
    ASSERT_NE(appendPreparedAccesses, std::string::npos);
    EXPECT_LT(materializeCall, appendPreparedAccesses)
        << "The swapchain access must be concrete before prepared sampled resources and visibility barriers are derived.";

    const auto visibilityWorkUnit = source.find("auto visibilityWorkUnit = workUnit;");
    ASSERT_NE(visibilityWorkUnit, std::string::npos);
    EXPECT_NE(source.find("MaterializeUiOverlayDependencyEdges(", visibilityWorkUnit), std::string::npos)
        << "Incoming UIOverlay dependency edges must use the materialized backbuffer access before barrier derivation.";
}

TEST(RHIUiOverlayPassTests, OverlayRendererUsesFloatColorVertexAttributeLayout)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/UI/RHIImGuiOverlayRenderer.cpp"));
    const auto layoutBody = source.find("desc.vertexAttributes.push_back({ 0u, 0u, 0u, sizeof(float) * 2u });");
    ASSERT_NE(layoutBody, std::string::npos);
    EXPECT_NE(source.find("desc.vertexAttributes.push_back({ 1u, 0u, sizeof(float) * 2u, sizeof(float) * 2u });", layoutBody), std::string::npos);
    EXPECT_NE(source.find("desc.vertexAttributes.push_back({ 3u, 0u, sizeof(float) * 4u, sizeof(float) * 4u });", layoutBody), std::string::npos);

    const auto shaderSource = ReadSourceText(RepoPath("App/Assets/Engine/Shaders/RHIImGuiOverlay.hlsl"));
    EXPECT_NE(shaderSource.find("float4 Color : TEXCOORD1;"), std::string::npos);
    EXPECT_NE(shaderSource.find("return input.Color * FontAtlasTexture.Sample(FontAtlasSampler, input.UV);"), std::string::npos);
}

TEST(RHIUiOverlayPassTests, OverlayRendererRebuildsGraphicsPipelineWhenDeviceChanges)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice firstDevice;
    TestUiOverlayDevice secondDevice;
    TestUiOverlayCommandBuffer firstCommandBuffer;
    TestUiOverlayCommandBuffer secondCommandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto firstPrepareResult = renderer.PrepareFrameResources(firstDevice, firstCommandBuffer, snapshot, 0u);
    const auto secondPrepareResult = renderer.PrepareFrameResources(secondDevice, secondCommandBuffer, snapshot, 0u);
    const auto secondRecordResult = renderer.RecordPrepared(secondCommandBuffer, snapshot, 0u);

    EXPECT_TRUE(firstPrepareResult.success) << firstPrepareResult.message;
    EXPECT_TRUE(secondPrepareResult.success) << secondPrepareResult.message;
    EXPECT_TRUE(secondRecordResult.success) << secondRecordResult.message;
    EXPECT_EQ(firstDevice.createPipelineLayoutCalls, 1u);
    EXPECT_EQ(firstDevice.createShaderModuleCalls, 2u);
    EXPECT_EQ(firstDevice.createGraphicsPipelineCalls, 1u);
    const auto* firstVertexBuffer = FindBufferWithUsage(firstDevice.createdBuffers, NLS::Render::RHI::BufferUsageFlags::Vertex);
    const auto* firstIndexBuffer = FindBufferWithUsage(firstDevice.createdBuffers, NLS::Render::RHI::BufferUsageFlags::Index);
    ASSERT_NE(firstVertexBuffer, nullptr);
    ASSERT_NE(firstIndexBuffer, nullptr);
    EXPECT_EQ(secondDevice.createPipelineLayoutCalls, 1u);
    EXPECT_EQ(secondDevice.createShaderModuleCalls, 2u);
    EXPECT_EQ(secondDevice.createGraphicsPipelineCalls, 1u);
    const auto* secondVertexBuffer = FindBufferWithUsage(secondDevice.createdBuffers, NLS::Render::RHI::BufferUsageFlags::Vertex);
    const auto* secondIndexBuffer = FindBufferWithUsage(secondDevice.createdBuffers, NLS::Render::RHI::BufferUsageFlags::Index);
    ASSERT_NE(secondVertexBuffer, nullptr);
    ASSERT_NE(secondIndexBuffer, nullptr);
    EXPECT_EQ(secondDevice.lastGraphicsPipelineDesc.pipelineLayout, secondDevice.createdPipelineLayout);
    ASSERT_EQ(secondDevice.createdShaderModules.size(), 2u);
    EXPECT_EQ(secondDevice.lastGraphicsPipelineDesc.vertexShader, secondDevice.createdShaderModules[0]);
    EXPECT_EQ(secondDevice.lastGraphicsPipelineDesc.fragmentShader, secondDevice.createdShaderModules[1]);
    ASSERT_EQ(secondCommandBuffer.lastBarrierDesc.bufferBarriers.size(), 2u);
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(
        secondCommandBuffer.lastBarrierDesc.bufferBarriers[0].buffer->GetDesc().usage,
        NLS::Render::RHI::BufferUsageFlags::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(
        secondCommandBuffer.lastBarrierDesc.bufferBarriers[1].buffer->GetDesc().usage,
        NLS::Render::RHI::BufferUsageFlags::Index));
    EXPECT_EQ(secondCommandBuffer.lastGraphicsPipeline, secondDevice.createdGraphicsPipeline);
}

TEST(RHIUiOverlayPassTests, OverlayRendererCreatesBindsAndOffsetsDynamicVertexIndexBuffers)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto result = renderer.RecordPrepared(commandBuffer, snapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    EXPECT_TRUE(result.success) << result.message;
    EXPECT_TRUE(result.recordedDraws);
    const auto* vertexBuffer = FindBufferWithUsage(device.createdBuffers, NLS::Render::RHI::BufferUsageFlags::Vertex);
    const auto* indexBuffer = FindBufferWithUsage(device.createdBuffers, NLS::Render::RHI::BufferUsageFlags::Index);
    ASSERT_NE(vertexBuffer, nullptr);
    ASSERT_NE(indexBuffer, nullptr);
    ASSERT_GE(commandBuffer.barrierCalls, 2u);
    const auto dynamicBarrierIt = std::find_if(
        commandBuffer.barrierDescs.begin(),
        commandBuffer.barrierDescs.end(),
        [](const NLS::Render::RHI::RHIBarrierDesc& barrierDesc)
        {
            return barrierDesc.bufferBarriers.size() == 2u;
        });
    ASSERT_NE(dynamicBarrierIt, commandBuffer.barrierDescs.end());
    ASSERT_EQ(dynamicBarrierIt->bufferBarriers.size(), 2u);
    EXPECT_EQ(dynamicBarrierIt->bufferBarriers[0].buffer.get(), vertexBuffer);
    EXPECT_EQ(commandBuffer.lastBarrierDesc.bufferBarriers[0].before, NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(commandBuffer.lastBarrierDesc.bufferBarriers[0].after, NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(
        commandBuffer.lastBarrierDesc.bufferBarriers[0].sourceStageMask,
        NLS::Render::RHI::PipelineStageMask::Host);
    EXPECT_EQ(
        commandBuffer.lastBarrierDesc.bufferBarriers[0].destinationStageMask,
        NLS::Render::RHI::PipelineStageMask::VertexInput);
    EXPECT_EQ(
        commandBuffer.lastBarrierDesc.bufferBarriers[0].sourceAccessMask,
        NLS::Render::RHI::AccessMask::HostWrite);
    EXPECT_EQ(
        commandBuffer.lastBarrierDesc.bufferBarriers[0].destinationAccessMask,
        NLS::Render::RHI::AccessMask::VertexRead);
    EXPECT_EQ(dynamicBarrierIt->bufferBarriers[1].buffer.get(), indexBuffer);
    EXPECT_EQ(dynamicBarrierIt->bufferBarriers[1].before, NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(dynamicBarrierIt->bufferBarriers[1].after, NLS::Render::RHI::ResourceState::GenericRead);
    EXPECT_EQ(
        dynamicBarrierIt->bufferBarriers[1].sourceStageMask,
        NLS::Render::RHI::PipelineStageMask::Host);
    EXPECT_EQ(
        dynamicBarrierIt->bufferBarriers[1].destinationStageMask,
        NLS::Render::RHI::PipelineStageMask::VertexInput);
    EXPECT_EQ(
        dynamicBarrierIt->bufferBarriers[1].sourceAccessMask,
        NLS::Render::RHI::AccessMask::HostWrite);
    EXPECT_EQ(
        dynamicBarrierIt->bufferBarriers[1].destinationAccessMask,
        NLS::Render::RHI::AccessMask::IndexRead);
    ASSERT_EQ(commandBuffer.bindVertexBufferCalls, 1u);
    ASSERT_EQ(commandBuffer.bindIndexBufferCalls, 1u);
    ASSERT_EQ(commandBuffer.drawIndexedFirstIndices.size(), 2u);
    EXPECT_EQ(commandBuffer.drawIndexedFirstIndices[0], 0u);
    EXPECT_EQ(commandBuffer.drawIndexedFirstIndices[1], 3u);
    ASSERT_EQ(commandBuffer.drawIndexedVertexOffsets.size(), 2u);
    EXPECT_EQ(commandBuffer.drawIndexedVertexOffsets[0], 0);
    EXPECT_EQ(commandBuffer.drawIndexedVertexOffsets[1], 3);

    const auto& vertexDesc = vertexBuffer->GetDesc();
    EXPECT_EQ(vertexDesc.memoryUsage, NLS::Render::RHI::MemoryUsage::CPUToGPU);
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(vertexDesc.usage, NLS::Render::RHI::BufferUsageFlags::Vertex));
    EXPECT_FALSE(NLS::Render::RHI::HasBufferUsage(vertexDesc.usage, NLS::Render::RHI::BufferUsageFlags::CopyDst))
        << "DX12 upload-heap dynamic UI buffers are updated through UpdateData, not GPU CopyDst transitions.";
    EXPECT_EQ(vertexBuffer->updateCalls, 1u);
    EXPECT_EQ(vertexBuffer->lastUpload.dataSize, 6u * sizeof(NLS::Render::UI::UiDrawVertex));

    const auto& indexDesc = indexBuffer->GetDesc();
    EXPECT_EQ(indexDesc.memoryUsage, NLS::Render::RHI::MemoryUsage::CPUToGPU);
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(indexDesc.usage, NLS::Render::RHI::BufferUsageFlags::Index));
    EXPECT_FALSE(NLS::Render::RHI::HasBufferUsage(indexDesc.usage, NLS::Render::RHI::BufferUsageFlags::CopyDst))
        << "DX12 upload-heap dynamic UI buffers are updated through UpdateData, not GPU CopyDst transitions.";
    EXPECT_EQ(indexBuffer->updateCalls, 1u);
    EXPECT_EQ(indexBuffer->lastUpload.dataSize, 6u * sizeof(uint32_t));
}

TEST(RHIUiOverlayPassTests, OverlayRendererReportsDynamicBufferAllocationAndReallocationTelemetry)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer firstCommandBuffer;
    TestUiOverlayCommandBuffer secondCommandBuffer;
    TestUiOverlayCommandBuffer thirdCommandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto firstPrepareResult = renderer.PrepareFrameResources(device, firstCommandBuffer, snapshot, 0u);
    ASSERT_TRUE(firstPrepareResult.success) << firstPrepareResult.message;
    EXPECT_GT(firstPrepareResult.dynamicBufferTelemetry.allocationCount, 0u);
    EXPECT_EQ(firstPrepareResult.dynamicBufferTelemetry.reallocationCount, 0u);
    EXPECT_GT(firstPrepareResult.dynamicBufferTelemetry.totalCpuCopyTimeNanoseconds, 0u);

    const auto secondPrepareResult = renderer.PrepareFrameResources(device, secondCommandBuffer, snapshot, 0u);
    ASSERT_TRUE(secondPrepareResult.success) << secondPrepareResult.message;
    EXPECT_EQ(secondPrepareResult.dynamicBufferTelemetry.allocationCount, 0u);
    EXPECT_EQ(secondPrepareResult.dynamicBufferTelemetry.reallocationCount, 0u);
    EXPECT_GT(secondPrepareResult.dynamicBufferTelemetry.totalCpuCopyTimeNanoseconds, 0u);

    auto largerSnapshot = snapshot;
    largerSnapshot.totalVertexCount = 12u;
    largerSnapshot.totalIndexCount = 12u;
    largerSnapshot.drawLists.front().vertices.resize(6u);
    largerSnapshot.drawLists.front().indices = {0u, 1u, 2u, 3u, 4u, 5u};
    largerSnapshot.drawLists.back().vertices.resize(6u);
    largerSnapshot.drawLists.back().indices = {0u, 1u, 2u, 3u, 4u, 5u};

    const auto thirdPrepareResult = renderer.PrepareFrameResources(device, thirdCommandBuffer, largerSnapshot, 0u);
    ASSERT_TRUE(thirdPrepareResult.success) << thirdPrepareResult.message;
    EXPECT_EQ(thirdPrepareResult.dynamicBufferTelemetry.allocationCount, 0u);
    EXPECT_GT(thirdPrepareResult.dynamicBufferTelemetry.reallocationCount, 0u);
    EXPECT_GT(thirdPrepareResult.dynamicBufferTelemetry.totalCpuCopyTimeNanoseconds, 0u);
}

TEST(RHIUiOverlayPassTests, OverlayRendererKeepsDynamicBuffersIsolatedPerFrameResourceSlot)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer slot0CommandBuffer;
    TestUiOverlayCommandBuffer slot1CommandBuffer;
    TestUiOverlayCommandBuffer slot0ReuseCommandBuffer;
    const auto snapshot = MakeTwoDrawListUiSnapshot();

    const auto slot0PrepareResult = renderer.PrepareFrameResources(device, slot0CommandBuffer, snapshot, 0u);
    const auto slot0Result = renderer.RecordPrepared(slot0CommandBuffer, snapshot, 0u);
    const auto slot1PrepareResult = renderer.PrepareFrameResources(device, slot1CommandBuffer, snapshot, 1u);
    const auto slot1Result = renderer.RecordPrepared(slot1CommandBuffer, snapshot, 1u);
    const auto slot0ReusePrepareResult = renderer.PrepareFrameResources(device, slot0ReuseCommandBuffer, snapshot, 0u);
    const auto slot0ReuseResult = renderer.RecordPrepared(slot0ReuseCommandBuffer, snapshot, 0u);

    EXPECT_TRUE(slot0PrepareResult.success) << slot0PrepareResult.message;
    EXPECT_TRUE(slot0Result.success) << slot0Result.message;
    EXPECT_TRUE(slot1PrepareResult.success) << slot1PrepareResult.message;
    EXPECT_TRUE(slot1Result.success) << slot1Result.message;
    EXPECT_TRUE(slot0ReusePrepareResult.success) << slot0ReusePrepareResult.message;
    EXPECT_TRUE(slot0ReuseResult.success) << slot0ReuseResult.message;
    const auto* vertexBuffer = FindBufferWithUsage(device.createdBuffers, NLS::Render::RHI::BufferUsageFlags::Vertex);
    const auto* indexBuffer = FindBufferWithUsage(device.createdBuffers, NLS::Render::RHI::BufferUsageFlags::Index);
    ASSERT_NE(vertexBuffer, nullptr);
    ASSERT_NE(indexBuffer, nullptr);
    EXPECT_GE(device.createdBuffers.size(), 4u)
        << "Each in-flight frame resource slot must own separate UI upload buffers.";
    EXPECT_EQ(vertexBuffer->updateCalls, 2u);
    EXPECT_EQ(indexBuffer->updateCalls, 2u);
    EXPECT_EQ(slot0CommandBuffer.bindVertexBufferCalls, 1u);
    EXPECT_EQ(slot1CommandBuffer.bindVertexBufferCalls, 1u);
    EXPECT_EQ(slot0ReuseCommandBuffer.bindVertexBufferCalls, 1u);
}

TEST(RHIUiOverlayPassTests, OverlayRendererSkipsUnsupportedTextureIdCommands)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    NLS::Render::UI::UiDrawDataSnapshot snapshot;
    snapshot.frameId = 57u;
    snapshot.hasVisibleDraws = true;
    snapshot.displaySize[0] = 640.0f;
    snapshot.displaySize[1] = 360.0f;
    snapshot.framebufferScale[0] = 1.0f;
    snapshot.framebufferScale[1] = 1.0f;
    snapshot.totalVertexCount = 3u;
    snapshot.totalIndexCount = 6u;

    NLS::Render::UI::UiDrawListSnapshot drawList;
    drawList.vertices.resize(3u);
    drawList.indices = { 0u, 1u, 2u, 0u, 2u, 1u };
    drawList.commands.push_back({
        3u,
        0u,
        0u,
        { 0.0f, 0.0f, 32.0f, 32.0f },
        {},
        NLS::Render::UI::UiDrawCallbackKind::None,
        false
    });
    drawList.commands.push_back({
        3u,
        3u,
        0u,
        { 32.0f, 32.0f, 64.0f, 64.0f },
        {},
        NLS::Render::UI::UiDrawCallbackKind::None,
        true
    });
    snapshot.drawLists.push_back(std::move(drawList));

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, snapshot, 0u);
    const auto result = renderer.RecordPrepared(commandBuffer, snapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    EXPECT_TRUE(result.success) << result.message;
    EXPECT_TRUE(result.recordedDraws);
    EXPECT_EQ(commandBuffer.drawIndexedCalls, 1u);
    EXPECT_EQ(result.message, "recorded 1 UI draw command(s)");
}

TEST(RHIUiOverlayPassTests, OverlayRendererRejectsMismatchedPreparedSnapshot)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    auto preparedSnapshot = MakeTwoDrawListUiSnapshot();
    auto mismatchedSnapshot = MakeTwoDrawListUiSnapshot();
    mismatchedSnapshot.frameId = preparedSnapshot.frameId + 1u;

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, preparedSnapshot, 0u);
    const auto result = renderer.RecordPrepared(commandBuffer, mismatchedSnapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.recordedDraws);
    EXPECT_NE(result.message.find("prepared UI overlay dynamic buffers do not match"), std::string::npos);
}

TEST(RHIUiOverlayPassTests, OverlayRendererRejectsEquivalentButDifferentPreparedSnapshotObject)
{
    ImGuiContextGuard imguiContext;
    ScopedOverlayShaderManager shaderManagerScope;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer;
    TestUiOverlayDevice device;
    TestUiOverlayCommandBuffer commandBuffer;
    const auto preparedSnapshot = MakeTwoDrawListUiSnapshot();
    const auto copiedSnapshot = preparedSnapshot;

    const auto prepareResult = renderer.PrepareFrameResources(device, commandBuffer, preparedSnapshot, 0u);
    const auto result = renderer.RecordPrepared(commandBuffer, copiedSnapshot, 0u);

    EXPECT_TRUE(prepareResult.success) << prepareResult.message;
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.recordedDraws);
    EXPECT_NE(result.message.find("prepared UI overlay dynamic buffers do not match"), std::string::npos);
}

TEST(RHIUiOverlayPassTests, OverlayRendererUsesExternalTextureRegistryAuthority)
{
    NLS::Render::UI::RHIImGuiTextureRegistry registry;
    NLS::Render::UI::RHIImGuiOverlayRenderer renderer(&registry);

    EXPECT_EQ(renderer.TextureRegistry(), &registry);

    NLS::Render::UI::RHIImGuiTextureRegistry replacementRegistry;
    renderer.SetTextureRegistry(&replacementRegistry);

    EXPECT_EQ(renderer.TextureRegistry(), &replacementRegistry);
}

TEST(RHIUiOverlayPassTests, FontAtlasInvalidateRequiresExplicitRetireFrameId)
{
    EXPECT_FALSE(SupportsNoArgInvalidate<NLS::Render::UI::RHIImGuiFontAtlas>)
        << "Font atlas invalidation must name the frame that retires current GPU resources.";
}

TEST(RHIUiOverlayPassTests, FontAtlasInvalidateRetiresResourcesUntilFrameCompletion)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    auto owner = std::make_shared<int>(1);
    std::shared_ptr<NLS::Render::RHI::RHITexture> texture(
        owner,
        reinterpret_cast<NLS::Render::RHI::RHITexture*>(0x1));
    std::shared_ptr<NLS::Render::RHI::RHITextureView> textureView(
        owner,
        reinterpret_cast<NLS::Render::RHI::RHITextureView*>(0x2));
    std::shared_ptr<NLS::Render::RHI::RHISampler> sampler(
        owner,
        reinterpret_cast<NLS::Render::RHI::RHISampler*>(0x3));
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> bindingSet(
        owner,
        reinterpret_cast<NLS::Render::RHI::RHIBindingSet*>(0x4));

    NLS::Render::UI::RHIImGuiFontAtlas atlas;
    atlas.SetUploadedResourcesForTesting(texture, textureView, sampler, bindingSet);

    atlas.Invalidate(5u);

    EXPECT_FALSE(atlas.IsUploaded());
    EXPECT_EQ(atlas.GetRetiredResourceCountForTesting(), 1u);

    atlas.ReleaseRetiredResourcesUpTo(4u);
    EXPECT_EQ(atlas.GetRetiredResourceCountForTesting(), 1u);

    atlas.ReleaseRetiredResourcesUpTo(5u);
    EXPECT_EQ(atlas.GetRetiredResourceCountForTesting(), 0u);
#else
    GTEST_SKIP() << "Requires NLS_ENABLE_TEST_HOOKS.";
#endif
}
