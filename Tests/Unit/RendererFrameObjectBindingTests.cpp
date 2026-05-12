#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Core/FrameObjectBindingProvider.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/ForwardSceneRenderer.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"

namespace
{
    class TestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "TestCommandBuffer"; }
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        bool IsRecording() const override { return true; }
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
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc&) override {}
    };

    class RecordingBindingProvider final : public NLS::Render::Core::FrameObjectBindingProvider
    {
    public:
        RecordingBindingProvider(
            NLS::Render::Core::CompositeRenderer& renderer,
            std::vector<std::string>& events)
            : FrameObjectBindingProvider(renderer)
            , m_events(events)
        {
        }

    protected:
        void OnBeginFrame(const NLS::Render::Data::FrameDescriptor&) override
        {
            m_events.push_back("begin");
        }

        void OnEndFrame() override
        {
            m_events.push_back("end");
        }

        void OnPrepareDraw(PipelineState&, const NLS::Render::Entities::Drawable&) override
        {
            m_events.push_back("before");
        }

        void OnPrepareExplicitDraw(
            NLS::Render::RHI::RHICommandBuffer&,
            PipelineState&,
            const NLS::Render::Entities::Drawable&) override
        {
            m_events.push_back("prepare");
        }

    private:
        std::vector<std::string>& m_events;
    };

    class ProviderAwareRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        ProviderAwareRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
            , m_commandBuffer(std::make_shared<TestCommandBuffer>())
        {
        }

    protected:
        bool PrepareRecordedDraw(
            PipelineState,
            const NLS::Render::Entities::Drawable&,
            PreparedRecordedDraw& outDraw) const override
        {
            outDraw.commandBuffer = m_commandBuffer;
            outDraw.instanceCount = 1u;
            return true;
        }

        void BindPreparedGraphicsPipeline(const PreparedRecordedDraw&) const override {}
        void BindPreparedMaterialBindingSet(const PreparedRecordedDraw&) const override {}
        void SubmitPreparedDraw(const PreparedRecordedDraw&) const override {}

    private:
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> m_commandBuffer;
    };

    class PackageProbeSceneRenderer final : public NLS::Engine::Rendering::BaseSceneRenderer
    {
    public:
        explicit PackageProbeSceneRenderer(NLS::Render::Context::Driver& driver)
            : BaseSceneRenderer(driver)
        {
        }

        NLS::Render::Context::RenderScenePackage CaptureRenderScenePackage(
            const NLS::Render::Context::FrameSnapshot& snapshot) const
        {
            return BuildRenderScenePackage(snapshot);
        }
    };

    class TestAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "RendererFrameObjectBindingTestsAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
        std::string_view GetVendor() const override { return "TestVendor"; }
        std::string_view GetHardware() const override { return "TestHardware"; }
    };

    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit TestTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        TestTextureView(
            std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
            NLS::Render::RHI::RHITextureViewDesc desc)
            : m_texture(std::move(texture))
            , m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
    };

    class TestBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        explicit TestBindingSet(NLS::Render::RHI::RHIBindingSetDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc {};
    };

    class TestBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
    {
    public:
        explicit TestBindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingLayoutDesc m_desc {};
    };

    class TestPipelineLayout final : public NLS::Render::RHI::RHIPipelineLayout
    {
    public:
        explicit TestPipelineLayout(NLS::Render::RHI::RHIPipelineLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIPipelineLayoutDesc m_desc {};
    };

    class TestShaderModule final : public NLS::Render::RHI::RHIShaderModule
    {
    public:
        explicit TestShaderModule(NLS::Render::RHI::RHIShaderModuleDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIShaderModuleDesc m_desc {};
    };

    class TestGraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
    {
    public:
        explicit TestGraphicsPipeline(NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc {};
    };

    class TestSampler final : public NLS::Render::RHI::RHISampler
    {
    public:
        explicit TestSampler(NLS::Render::RHI::SamplerDesc desc)
            : m_desc(desc)
        {
        }

        std::string_view GetDebugName() const override { return "RendererFrameObjectBindingTestsSampler"; }
        const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::SamplerDesc m_desc {};
    };

    class TestQueue final : public NLS::Render::RHI::RHIQueue
    {
    public:
        std::string_view GetDebugName() const override { return "RendererFrameObjectBindingTestsQueue"; }
        NLS::Render::RHI::QueueType GetType() const override { return NLS::Render::RHI::QueueType::Graphics; }
        void Submit(const NLS::Render::RHI::RHISubmitDesc&) override {}
        NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(
            const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
        {
            Submit(submitDesc);
            return {};
        }
        void Present(const NLS::Render::RHI::RHIPresentDesc&) override {}
        NLS::Render::RHI::RHIQueueOperationResult PresentChecked(
            const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            Present(presentDesc);
            return {};
        }
    };

    class TestExplicitDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        using NLS::Render::RHI::RHIDevice::CreateBuffer;
        using NLS::Render::RHI::RHIDevice::CreateTexture;

        TestExplicitDevice()
            : m_adapter(std::make_shared<TestAdapter>())
            , m_queue(std::make_shared<TestQueue>())
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.backendReady = true;
            m_capabilities.supportsGraphics = true;
            m_capabilities.supportsCompute = true;
            m_capabilities.supportsSwapchain = true;
            m_capabilities.supportsCurrentSceneRenderer = true;
            m_capabilities.supportsOffscreenFramebuffers = true;
            m_capabilities.supportsMultiRenderTargets = true;
            m_capabilities.supportsExplicitBarriers = true;
            m_capabilities.supportsCentralizedDescriptorManagement = true;
            m_capabilities.supportsPipelineStateCache = true;
        }

        std::string_view GetDebugName() const override { return "RendererFrameObjectBindingTestsDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return m_queue; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc&,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::RHITextureUploadDesc&) override
        {
            return std::make_shared<TestTexture>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHITextureViewDesc& desc) override
        {
            return std::make_shared<TestTextureView>(texture, desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc& desc, std::string = {}) override
        {
            if (failSamplerCreation)
                return nullptr;
            return std::make_shared<TestSampler>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override
        {
            ++bindingLayoutCreateCalls;
            return std::make_shared<TestBindingLayout>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc& desc) override
        {
            ++bindingSetCreateCalls;
            lastBindingSetDesc = desc;
            return std::make_shared<TestBindingSet>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override
        {
            ++pipelineLayoutCreateCalls;
            lastPipelineLayoutDesc = desc;
            return std::make_shared<TestPipelineLayout>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc& desc) override
        {
            ++shaderModuleCreateCalls;
            return std::make_shared<TestShaderModule>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override
        {
            ++graphicsPipelineCreateCalls;
            lastGraphicsPipelineDesc = desc;
            return std::make_shared<TestGraphicsPipeline>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType, std::string = {}) override { return nullptr; }
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

        uint32_t bindingLayoutCreateCalls = 0u;
        uint32_t bindingSetCreateCalls = 0u;
        uint32_t pipelineLayoutCreateCalls = 0u;
        uint32_t shaderModuleCreateCalls = 0u;
        uint32_t graphicsPipelineCreateCalls = 0u;
        bool failSamplerCreation = false;
        NLS::Render::RHI::RHIBindingSetDesc lastBindingSetDesc {};
        NLS::Render::RHI::RHIPipelineLayoutDesc lastPipelineLayoutDesc {};
        NLS::Render::RHI::RHIGraphicsPipelineDesc lastGraphicsPipelineDesc {};

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        std::shared_ptr<NLS::Render::RHI::RHIQueue> m_queue;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    NLS::Render::Resources::Shader* CreateMaterialSamplerShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        const_cast<std::vector<NLS::Render::Resources::ShaderParameterStruct>&>(shader->GetParameterStructs()).clear();
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.properties.push_back({
            "u_MaterialSampler",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::Sampler,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            3u,
            -1,
            1,
            0u,
            0u,
            {}
        });
        const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) = std::move(reflection);
        return shader;
    }

    NLS::Render::Resources::Shader* CreateMaterialStructuredBufferShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        const_cast<std::vector<NLS::Render::Resources::ShaderParameterStruct>&>(shader->GetParameterStructs()).clear();
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.properties.push_back({
            "u_MissingStructuredBuffer",
            NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
            NLS::Render::Resources::ShaderResourceKind::StructuredBuffer,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            4u,
            -1,
            1,
            0u,
            0u,
            {}
        });
        const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) = std::move(reflection);
        return shader;
    }

    NLS::Render::Resources::Shader* CreateConflictingMaterialBindingShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        const_cast<std::vector<NLS::Render::Resources::ShaderParameterStruct>&>(shader->GetParameterStructs()).clear();
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.properties = {
            {
                "u_Buffer",
                NLS::Render::Resources::UniformType::UNIFORM_FLOAT,
                NLS::Render::Resources::ShaderResourceKind::StructuredBuffer,
                NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
                0u,
                -1,
                1,
                0u,
                0u,
                {}
            },
            {
                "u_OtherBuffer",
                NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
                NLS::Render::Resources::ShaderResourceKind::StructuredBuffer,
                NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
                0u,
                -1,
                1,
                0u,
                0u,
                {}
            }
        };
        const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) = std::move(reflection);
        return shader;
    }
}

TEST(RendererFrameObjectBindingTests, ProviderTracksFrameLifecycle)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    std::vector<std::string> events;
    ProviderAwareRenderer renderer(*driver);
    auto provider = std::make_unique<RecordingBindingProvider>(renderer, events);
    auto* providerPtr = provider.get();
    renderer.SetFrameObjectBindingProvider(std::move(provider));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    providerPtr->BeginFrame(frameDescriptor);
    EXPECT_TRUE(providerPtr->IsFramePrepared());
    EXPECT_FALSE(providerPtr->IsObjectPrepared());

    providerPtr->EndFrame();
    EXPECT_FALSE(providerPtr->IsFramePrepared());
    EXPECT_FALSE(providerPtr->IsObjectPrepared());
    EXPECT_EQ(events, std::vector<std::string>({ "begin", "end" }));
}

TEST(RendererFrameObjectBindingTests, ProviderPreparesObjectStateDuringDrawsWithoutFeatures)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    std::vector<std::string> events;
    ProviderAwareRenderer renderer(*driver);
    auto provider = std::make_unique<RecordingBindingProvider>(renderer, events);
    auto* providerPtr = provider.get();
    renderer.SetFrameObjectBindingProvider(std::move(provider));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    providerPtr->BeginFrame(frameDescriptor);

    NLS::Render::Data::PipelineState pipelineState;
    NLS::Render::Entities::Drawable drawable;
    renderer.DrawEntity(pipelineState, drawable);

    EXPECT_TRUE(providerPtr->IsFramePrepared());
    EXPECT_TRUE(providerPtr->IsObjectPrepared());
    EXPECT_EQ(providerPtr->GetPreparedDrawCount(), 1u);
    EXPECT_EQ(events, std::vector<std::string>({ "begin", "before", "prepare" }));

    providerPtr->EndFrame();
}

TEST(RendererFrameObjectBindingTests, RenderScenePackageMarksFrameAndObjectDataReadyForSnapshotDraws)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    PackageProbeSceneRenderer renderer(driver);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 2u;
    snapshot.visibleTransparentDrawCount = 1u;
    snapshot.visibleSkyboxDrawCount = 1u;

    const auto package = renderer.CaptureRenderScenePackage(snapshot);

    EXPECT_TRUE(package.hasVisibleDraws);
    EXPECT_EQ(package.visibleDrawCount, 4u);
    EXPECT_EQ(package.opaqueDrawCount, 2u);
    EXPECT_EQ(package.transparentDrawCount, 1u);
    EXPECT_EQ(package.skyboxDrawCount, 1u);
    EXPECT_TRUE(package.hasOpaquePass);
    EXPECT_TRUE(package.hasTransparentPass);
    EXPECT_TRUE(package.hasSkyboxPass);
    EXPECT_EQ(package.passPlanCount, 3u);
    EXPECT_TRUE(package.frameDataReady);
    EXPECT_TRUE(package.objectDataReady);
    EXPECT_EQ(package.drawCommandCount, 4u);
    EXPECT_EQ(package.materialBatchCount, 4u);
    EXPECT_EQ(package.renderTargetUseCount, 1u);
    EXPECT_TRUE(package.containsCommandInputs);
    ASSERT_EQ(package.passCommandInputs.size(), 3u);
    EXPECT_EQ(package.passCommandInputs[0].kind, NLS::Render::Context::RenderPassCommandKind::Opaque);
    EXPECT_EQ(package.passCommandInputs[0].drawCount, 2u);
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
    EXPECT_EQ(package.passCommandInputs[1].drawCount, 1u);
    EXPECT_EQ(package.passCommandInputs[2].kind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(package.passCommandInputs[2].drawCount, 1u);
}

TEST(RendererFrameObjectBindingTests, EngineProviderPreparesFrameObjectDataIntoRenderScenePackage)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    ProviderAwareRenderer renderer(*driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 2u;
    snapshot.visibleTransparentDrawCount = 1u;

    NLS::Render::Context::RenderScenePackage package;
    package.visibleDrawCount = 3u;
    EXPECT_FALSE(package.frameDataReady);
    EXPECT_FALSE(package.objectDataReady);

    provider.PrepareRenderScenePackage(snapshot, package);

    EXPECT_TRUE(package.frameDataReady);
    EXPECT_TRUE(package.objectDataReady);
}

TEST(RendererFrameObjectBindingTests, ExplicitBindingSetCreationRequiresCentralDescriptorAllocator)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 23u;
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::RHI::RHIBindingSetDesc desc;
    desc.debugName = "MainlineBindings";
    desc.entries.resize(2u);
    desc.entries[0].binding = 0u;
    desc.entries[0].type = NLS::Render::RHI::BindingType::UniformBuffer;
    desc.entries[1].binding = 1u;
    desc.entries[1].type = NLS::Render::RHI::BindingType::Texture;

    auto bindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
        driver,
        desc,
        NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame);
    EXPECT_EQ(bindingSet, nullptr);

    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(16u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    bindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
        driver,
        desc,
        NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame);
    ASSERT_NE(bindingSet, nullptr);

    const auto descriptorStats = frameContext.descriptorAllocator->GetStats();
    EXPECT_EQ(descriptorStats.transientUsed, 2u);

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingSetUsesProvidedDeviceInsteadOfLocatedDriver)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    auto* shader = CreateMaterialSamplerShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();

    const auto& bindingSet = material.GetExplicitBindingSet(explicitDevice);

    ASSERT_NE(bindingSet, nullptr);
    EXPECT_EQ(explicitDevice->bindingLayoutCreateCalls, 1u);
    EXPECT_EQ(explicitDevice->bindingSetCreateCalls, 1u);
    ASSERT_NE(explicitDevice->lastBindingSetDesc.layout, nullptr);
    const auto samplerEntry = std::find_if(
        explicitDevice->lastBindingSetDesc.entries.begin(),
        explicitDevice->lastBindingSetDesc.entries.end(),
        [](const NLS::Render::RHI::RHIBindingSetEntry& entry)
        {
            return entry.type == NLS::Render::RHI::BindingType::Sampler;
        });
    ASSERT_NE(samplerEntry, explicitDevice->lastBindingSetDesc.entries.end());
    EXPECT_NE(samplerEntry->sampler, nullptr);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingSetReportsMissingRequiredBindings)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    auto* shader = CreateMaterialStructuredBufferShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();

    const auto& bindingSet = material.GetExplicitBindingSet(explicitDevice);

    ASSERT_NE(bindingSet, nullptr);
    EXPECT_TRUE(explicitDevice->lastBindingSetDesc.entries.empty());
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    const auto& diagnostic = material.GetLastExplicitBindingDiagnostics().front();
    EXPECT_EQ(diagnostic.severity, NLS::Render::Resources::MaterialBindingDiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.bindingName, "u_MissingStructuredBuffer");
    EXPECT_NE(diagnostic.message.find("missing"), std::string::npos);
    EXPECT_TRUE(material.HasExplicitBindingErrors());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingSetReportsNullSamplerDescriptor)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    auto* shader = CreateMaterialSamplerShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->failSamplerCreation = true;

    const auto& bindingSet = material.GetExplicitBindingSet(explicitDevice);

    ASSERT_NE(bindingSet, nullptr);
    EXPECT_TRUE(explicitDevice->lastBindingSetDesc.entries.empty());
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    const auto& diagnostic = material.GetLastExplicitBindingDiagnostics().front();
    EXPECT_EQ(diagnostic.severity, NLS::Render::Resources::MaterialBindingDiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.bindingName, "u_MaterialSampler");
    EXPECT_NE(diagnostic.message.find("sampler"), std::string::npos);
    EXPECT_TRUE(material.HasExplicitBindingErrors());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingSetReportsShaderReflectionValidationErrors)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    auto* shader = CreateConflictingMaterialBindingShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();

    const auto& bindingSet = material.GetExplicitBindingSet(explicitDevice);

    EXPECT_EQ(bindingSet, nullptr);
    EXPECT_EQ(explicitDevice->bindingLayoutCreateCalls, 0u);
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    const auto& diagnostic = material.GetLastExplicitBindingDiagnostics().front();
    EXPECT_EQ(diagnostic.severity, NLS::Render::Resources::MaterialBindingDiagnosticSeverity::Error);
    EXPECT_NE(diagnostic.message.find("conflict"), std::string::npos);
    EXPECT_TRUE(material.HasExplicitBindingErrors());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, EngineGraphicsShadersExposeRendererOwnedParameterStructs)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    const std::vector<std::string> shaderPaths = {
        "App/Assets/Engine/Shaders/Standard.hlsl",
        "App/Assets/Engine/Shaders/Lambert.hlsl",
        "App/Assets/Engine/Shaders/StandardPBR.hlsl",
        "App/Assets/Engine/Shaders/DeferredLighting.hlsl"
    };

    for (const auto& shaderPath : shaderPaths)
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(shaderPath);
        ASSERT_NE(shader, nullptr) << shaderPath;
        ASSERT_TRUE(shader->HasParameterStructs()) << shaderPath;

        const auto& parameterStructs = shader->GetParameterStructs();
        ASSERT_EQ(parameterStructs.size(), 4u) << shaderPath;
        EXPECT_EQ(parameterStructs[0].groupKind, NLS::Render::Resources::ShaderParameterGroupKind::Frame);
        EXPECT_EQ(parameterStructs[1].groupKind, NLS::Render::Resources::ShaderParameterGroupKind::Material);
        EXPECT_EQ(parameterStructs[2].groupKind, NLS::Render::Resources::ShaderParameterGroupKind::Object);
        EXPECT_EQ(parameterStructs[3].groupKind, NLS::Render::Resources::ShaderParameterGroupKind::Pass);
        EXPECT_FALSE(parameterStructs[1].members.empty()) << shaderPath;
        EXPECT_FALSE(parameterStructs[3].members.empty()) << shaderPath;

        EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    }
}

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutUsesRendererOwnedShaderParameterStructs)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    ASSERT_TRUE(shader->HasParameterStructs());

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();

    const auto& materialLayout = material.GetExplicitBindingLayout(explicitDevice);
    ASSERT_NE(materialLayout, nullptr);
    ASSERT_EQ(materialLayout->GetDesc().entries.size(), 7u);
    EXPECT_EQ(materialLayout->GetDesc().entries[0].name, "MaterialConstants");
    EXPECT_EQ(materialLayout->GetDesc().entries[1].name, "u_DiffuseMap");
    EXPECT_EQ(materialLayout->GetDesc().entries[6].name, "u_LinearWrapSampler");

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);
    ASSERT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts.size(), 4u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[0]->GetDesc().entries[0].name, "FrameConstants");
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[1]->GetDesc().entries[0].name, "MaterialConstants");
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[2]->GetDesc().entries[0].name, "ObjectConstants");
    ASSERT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries.size(), 4u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[0].name, "ForwardLightData");
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[0].type, NLS::Render::RHI::BindingType::UniformBuffer);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[0].binding, 0u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[0].registerSpace, NLS::Render::RHI::BindingPointMap::kPassBindingSpace);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[1].name, "u_ForwardLocalLightBuffer");

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, DeferredGBufferPipelineOverridesUseThreeRenderTargets)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 recorded material pipeline override test requires the phase-1 Windows DX12 runtime.";
#endif

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    NLS::Core::ServiceLocator::Provide(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto pipelineCache = NLS::Render::RHI::CreateDefaultPipelineCache();

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.colorWrite = true;
    overrides.colorFormats = {
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA8
    };

    const NLS::Render::Data::PipelineState pipelineState;
    const auto pipeline = material.BuildRecordedGraphicsPipeline(
        explicitDevice,
        pipelineCache,
        NLS::Render::Settings::EPrimitiveMode::TRIANGLES,
        pipelineState,
        overrides);

    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(explicitDevice->graphicsPipelineCreateCalls, 1u);
    ASSERT_EQ(explicitDevice->lastGraphicsPipelineDesc.renderTargetLayout.colorFormats.size(), 3u);
    ASSERT_EQ(explicitDevice->lastGraphicsPipelineDesc.blendState.renderTargets.size(), 3u);
    for (const auto colorFormat : explicitDevice->lastGraphicsPipelineDesc.renderTargetLayout.colorFormats)
        EXPECT_EQ(colorFormat, NLS::Render::RHI::TextureFormat::RGBA8);
    for (const auto& renderTargetBlend : explicitDevice->lastGraphicsPipelineDesc.blendState.renderTargets)
        EXPECT_EQ(renderTargetBlend.colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::All);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, ThreadedPreparedBindingSetCreationUsesCurrentFrameDescriptorAllocator)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 7u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(16u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    NLS::Render::RHI::RHIBindingSetDesc desc;
    desc.debugName = "ThreadedPreparedBindings";
    desc.entries.resize(2u);
    desc.entries[0].binding = 0u;
    desc.entries[0].type = NLS::Render::RHI::BindingType::UniformBuffer;
    desc.entries[1].binding = 1u;
    desc.entries[1].type = NLS::Render::RHI::BindingType::Texture;

    const auto bindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
        driver,
        desc,
        NLS::Render::RHI::DescriptorAllocationLifetime::Persistent);
    ASSERT_NE(bindingSet, nullptr);

    const auto descriptorStats = frameContext.descriptorAllocator->GetStats();
    EXPECT_EQ(descriptorStats.persistentUsed, 2u);
}

TEST(RendererFrameObjectBindingTests, DeferredThreadedOffscreenPackageCarriesExternalOutputAttachmentViewForLightingPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(256u, 144u);
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("LightActor").AddComponent<NLS::Engine::Components::LightComponent>();
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    EXPECT_FALSE(renderer.HasPendingLightGridFrameInputs());
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* publishedSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(publishedSlot, nullptr);
    ASSERT_TRUE(publishedSlot->snapshot.has_value());
    EXPECT_TRUE(publishedSlot->preparedRenderSceneBuilder.has_value() || publishedSlot->renderScenePackage.has_value());

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    ASSERT_TRUE(retiredSlot->renderScenePackage.has_value());
    ASSERT_EQ(retiredSlot->renderScenePackage->passCommandInputs.size(), 2u);
    const auto& lightingPass = retiredSlot->renderScenePackage->passCommandInputs[1];
    EXPECT_EQ(lightingPass.kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
    EXPECT_FALSE(lightingPass.targetsSwapchain);
    EXPECT_TRUE(lightingPass.usesColorAttachment);
    ASSERT_FALSE(lightingPass.colorAttachmentViews.empty());
    EXPECT_NE(lightingPass.colorAttachmentViews.front(), nullptr);
    ASSERT_FALSE(retiredSlot->renderScenePackage->extractedTextures.empty());
    EXPECT_NE(retiredSlot->renderScenePackage->extractedTextures.front(), nullptr);
}

TEST(RendererFrameObjectBindingTests, ForwardThreadedOffscreenPackageRegistersExternalOutputExtraction)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Buffers::Framebuffer outputBuffer(256u, 144u);
    NLS::Engine::Rendering::ForwardSceneRenderer renderer(driver);

    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("MeshActor").AddComponent<NLS::Engine::Components::MeshRenderer>();
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    EXPECT_FALSE(renderer.HasPendingLightGridFrameInputs());
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* publishedSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(publishedSlot, nullptr);
    ASSERT_TRUE(publishedSlot->snapshot.has_value());
    EXPECT_TRUE(publishedSlot->preparedRenderSceneBuilder.has_value() || publishedSlot->renderScenePackage.has_value());

    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);

    const auto* retiredSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(retiredSlot, nullptr);
    ASSERT_TRUE(retiredSlot->renderScenePackage.has_value());
    ASSERT_FALSE(retiredSlot->renderScenePackage->passCommandInputs.empty());
    ASSERT_FALSE(retiredSlot->renderScenePackage->extractedTextures.empty());
    EXPECT_NE(retiredSlot->renderScenePackage->extractedTextures.front(), nullptr);
}
