#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "Guid.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Core/FrameObjectBindingProvider.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"
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
    class ScopedDriverService final
    {
    public:
        explicit ScopedDriverService(NLS::Render::Context::Driver& driver)
        {
            NLS::Core::ServiceLocator::Provide(driver);
        }

        ~ScopedDriverService()
        {
            NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
        }

        ScopedDriverService(const ScopedDriverService&) = delete;
        ScopedDriverService& operator=(const ScopedDriverService&) = delete;
    };

    class ScopedShaderManagerService final
    {
    public:
        explicit ScopedShaderManagerService(NLS::Core::ResourceManagement::ShaderManager& shaderManager)
        {
            NLS::Core::ServiceLocator::Provide(shaderManager);
        }

        ~ScopedShaderManagerService()
        {
            NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
        }

        ScopedShaderManagerService(const ScopedShaderManagerService&) = delete;
        ScopedShaderManagerService& operator=(const ScopedShaderManagerService&) = delete;
    };

    NLS::Render::Resources::Shader* CreateTestComputeShader(const std::string& sourcePath)
    {
        NLS::Render::Assets::ShaderArtifact artifact;
        artifact.sourcePath = sourcePath;
        artifact.subAssetKey = "shader:test-compute";
        artifact.stages.push_back({
            NLS::Render::ShaderCompiler::ShaderStage::Compute,
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
            "CSMain",
            "cs_6_0",
            {
                NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                {1u, 2u, 3u, 4u},
                {},
                {},
                "test-compute",
                "test-compute.nshader"
            }
        });

        const auto root = std::filesystem::temp_directory_path() /
            ("nullus_compute_shader_" + NLS::Guid::New().ToString());
        const auto path = root / "shader.nshader";
        std::filesystem::create_directories(path.parent_path());
        const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(path.string());
        std::filesystem::remove_all(root);
        return shader;
    }

    void RegisterLightGridTestShaders(NLS::Core::ResourceManagement::ShaderManager& shaderManager)
    {
        shaderManager.RegisterResource(
            ":Shaders/LightGridReset.hlsl",
            CreateTestComputeShader("App/Assets/Engine/Shaders/LightGridReset.hlsl"));
        shaderManager.RegisterResource(
            ":Shaders/LightGridInjection.hlsl",
            CreateTestComputeShader("App/Assets/Engine/Shaders/LightGridInjection.hlsl"));
        shaderManager.RegisterResource(
            ":Shaders/LightGridCompact.hlsl",
            CreateTestComputeShader("App/Assets/Engine/Shaders/LightGridCompact.hlsl"));
    }

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
        void BindGraphicsPipeline(const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override
        {
            ++bindGraphicsPipelineCalls;
        }
        void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}
        void BindBindingSet(uint32_t, const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override {}
        void PushConstants(NLS::Render::RHI::ShaderStageMask, uint32_t, uint32_t size, const void* data) override
        {
            if (size == sizeof(uint32_t) && data != nullptr)
                std::memcpy(&lastObjectIndexPushConstant, data, sizeof(lastObjectIndexPushConstant));
            ++pushConstantCalls;
        }
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

        uint32_t pushConstantCalls = 0u;
        uint32_t bindGraphicsPipelineCalls = 0u;
        uint32_t lastObjectIndexPushConstant = 0u;
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

        bool OnPrepareDraw(PipelineState&, const NLS::Render::Entities::Drawable&) override
        {
            m_events.push_back("before");
            return true;
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

    class CameraMatrixProbeBindingProvider final : public NLS::Render::Core::FrameObjectBindingProvider
    {
    public:
        CameraMatrixProbeBindingProvider(
            NLS::Render::Core::CompositeRenderer& renderer,
            const NLS::Render::Entities::Camera& camera)
            : FrameObjectBindingProvider(renderer)
            , m_camera(camera)
        {
        }

        NLS::Maths::Matrix4 observedViewMatrix = NLS::Maths::Matrix4::Identity;

    protected:
        void OnBeginFrame(const NLS::Render::Data::FrameDescriptor&) override
        {
            observedViewMatrix = m_camera.GetViewMatrix();
        }

    private:
        const NLS::Render::Entities::Camera& m_camera;
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

    class TestBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        TestBuffer(
            NLS::Render::RHI::RHIBufferDesc desc,
            const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc = {})
            : m_desc(std::move(desc))
        {
            if (uploadDesc.HasData())
            {
                uploadData.resize(uploadDesc.dataSize);
                std::memcpy(uploadData.data(), uploadDesc.data, uploadDesc.dataSize);
            }
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }
        NLS::Render::RHI::RHIUpdateResult UpdateData(const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
        {
            if (!uploadDesc.HasData())
            {
                return {
                    NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument,
                    "missing upload data"
                };
            }
            if (uploadDesc.destinationOffset + uploadDesc.dataSize > m_desc.size)
            {
                return {
                    NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument,
                    "upload exceeds buffer size"
                };
            }

            if (uploadData.size() < m_desc.size)
                uploadData.resize(m_desc.size, 0u);
            std::memcpy(
                uploadData.data() + static_cast<size_t>(uploadDesc.destinationOffset),
                uploadDesc.data,
                uploadDesc.dataSize);
            ++updateCalls;
            lastUpdate = uploadDesc;
            return { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };
        }
        std::vector<uint8_t> uploadData;
        uint32_t updateCalls = 0u;
        NLS::Render::RHI::RHIBufferUploadDesc lastUpdate {};

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
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

    class TestMesh final : public NLS::Render::RHI::RHIMesh
    {
    public:
        explicit TestMesh(std::shared_ptr<NLS::Render::RHI::RHIBuffer> vertexBuffer)
            : m_vertexBuffer(std::move(vertexBuffer))
        {
        }

        std::shared_ptr<NLS::Render::RHI::RHIBuffer> GetVertexBuffer() const override { return m_vertexBuffer; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> GetIndexBuffer() const override { return nullptr; }
        uint32_t GetVertexCount() const override { return 3u; }
        uint32_t GetIndexCount() const override { return 0u; }
        NLS::Render::Settings::EPrimitiveMode GetPrimitiveMode() const override { return NLS::Render::Settings::EPrimitiveMode::TRIANGLES; }
        uint32_t GetVertexStride() const override { return sizeof(float) * 3u; }
        NLS::Render::RHI::IndexType GetIndexType() const override { return NLS::Render::RHI::IndexType::UInt32; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> m_vertexBuffer;
    };

    class ObjectIndexSubmitRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit ObjectIndexSubmitRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
            NLS::Render::RHI::RHIBufferDesc vertexDesc;
            vertexDesc.size = sizeof(float) * 9u;
            vertexDesc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
            vertexDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
            vertexDesc.debugName = "ObjectIndexSubmitVertexBuffer";
            auto vertexBuffer = std::make_shared<TestBuffer>(vertexDesc);
            mesh = std::make_shared<TestMesh>(std::move(vertexBuffer));
            commandBuffer = std::make_shared<TestCommandBuffer>();
        }

        void SubmitWithObjectIndex(const uint32_t objectIndex) const
        {
            PreparedRecordedDraw draw;
            draw.commandBuffer = commandBuffer;
            draw.mesh = mesh;
            draw.instanceCount = 1u;
            draw.objectIndex = objectIndex;
            draw.usesObjectIndex = true;
            SubmitPreparedDraw(draw);
        }

        std::shared_ptr<TestCommandBuffer> commandBuffer;
        std::shared_ptr<TestMesh> mesh;
    };

    class ImmediateObjectIndexRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit ImmediateObjectIndexRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
            , m_commandBuffer(std::make_shared<TestCommandBuffer>())
        {
            NLS::Render::RHI::RHIBufferDesc vertexDesc;
            vertexDesc.size = sizeof(float) * 9u;
            vertexDesc.usage = NLS::Render::RHI::BufferUsageFlags::Vertex;
            vertexDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
            vertexDesc.debugName = "ImmediateObjectIndexVertexBuffer";
            auto vertexBuffer = std::make_shared<TestBuffer>(vertexDesc);
            mesh = std::make_shared<TestMesh>(std::move(vertexBuffer));
        }

        std::shared_ptr<TestCommandBuffer> commandBuffer() const { return m_commandBuffer; }
        std::shared_ptr<TestMesh> mesh;

    protected:
        bool PrepareRecordedDraw(
            PipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            PreparedRecordedDraw& outDraw) const override
        {
            outDraw.commandBuffer = m_commandBuffer;
            outDraw.materialBindingSet = std::make_shared<TestBindingSet>(NLS::Render::RHI::RHIBindingSetDesc{});
            outDraw.mesh = mesh;
            outDraw.instanceCount = drawable.instanceCount != 0u ? drawable.instanceCount : 1u;
            return true;
        }

        void BindPreparedGraphicsPipeline(const PreparedRecordedDraw&) const override
        {
            ++m_commandBuffer->bindGraphicsPipelineCalls;
        }

        void BindPreparedMaterialBindingSet(const PreparedRecordedDraw&) const override {}

    private:
        std::shared_ptr<TestCommandBuffer> m_commandBuffer;
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

    class TestComputePipeline final : public NLS::Render::RHI::RHIComputePipeline
    {
    public:
        explicit TestComputePipeline(NLS::Render::RHI::RHIComputePipelineDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIComputePipelineDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIComputePipelineDesc m_desc {};
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

        void SetNativeBackendType(const NLS::Render::RHI::NativeBackendType backend)
        {
            m_nativeDeviceInfo.backend = backend;
        }

        std::string_view GetDebugName() const override { return "RendererFrameObjectBindingTestsDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return m_queue; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
        {
            auto buffer = std::make_shared<TestBuffer>(desc, uploadDesc);
            buffers.push_back(buffer);
            return buffer;
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
            bindingSetDescs.push_back(desc);
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
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc) override
        {
            ++computePipelineCreateCalls;
            return std::make_shared<TestComputePipeline>(desc);
        }
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
        uint32_t computePipelineCreateCalls = 0u;
        bool failSamplerCreation = false;
        NLS::Render::RHI::RHIBindingSetDesc lastBindingSetDesc {};
        std::vector<NLS::Render::RHI::RHIBindingSetDesc> bindingSetDescs;
        std::vector<std::shared_ptr<TestBuffer>> buffers;
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

    NLS::Render::Resources::Shader* CreateReflectionOnlyObjectDataShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        const_cast<std::vector<NLS::Render::Resources::ShaderParameterStruct>&>(shader->GetParameterStructs()).clear();
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.properties.push_back({
            "ObjectData",
            NLS::Render::Resources::UniformType::UNIFORM_FLOAT_MAT4,
            NLS::Render::Resources::ShaderResourceKind::StructuredBuffer,
            NLS::Render::ShaderCompiler::ShaderStage::Vertex,
            NLS::Render::RHI::BindingPointMap::kObjectBindingSpace,
            0u,
            -1,
            1,
            0u,
            sizeof(NLS::Maths::Matrix4),
            {}
        });
        const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) = std::move(reflection);
        return shader;
    }

    class ScopedShaderManagerAssetPaths final
    {
    public:
        ScopedShaderManagerAssetPaths(
            const std::string& projectAssetsPath,
            const std::string& engineAssetsPath)
        {
            NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(
                projectAssetsPath,
                engineAssetsPath);
        }

        ~ScopedShaderManagerAssetPaths()
        {
            NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
            NLS::Render::Resources::Loaders::ShaderLoader::SetDefaultProjectAssetsPath({});
        }

        ScopedShaderManagerAssetPaths(const ScopedShaderManagerAssetPaths&) = delete;
        ScopedShaderManagerAssetPaths& operator=(const ScopedShaderManagerAssetPaths&) = delete;
    };
}

TEST(RendererFrameObjectBindingTests, ProviderTracksFrameLifecycle)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

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
    const ScopedDriverService driverService(*driver);

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
    const ScopedDriverService driverService(driver);
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
    const ScopedDriverService driverService(*driver);

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

TEST(RendererFrameObjectBindingTests, EngineProviderCapturesObjectConstantsFromCurrentDrawable)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 31u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(16u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    const auto modelMatrix =
        NLS::Maths::Matrix4::Translation({ 2.0f, 3.0f, 5.0f }) *
        NLS::Maths::Matrix4::Scaling({ 1.5f, 1.5f, 1.5f });
    NLS::Render::Entities::Drawable drawable;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity
    });

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, drawable);

    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindingSets;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindingSets));
    ASSERT_NE(bindingSets.objectBindingSet, nullptr);
    ASSERT_EQ(bindingSets.objectBindingSet->GetDesc().entries.size(), 1u);
    const auto& objectEntry = bindingSets.objectBindingSet->GetDesc().entries[0];
    ASSERT_NE(objectEntry.buffer, nullptr);
    EXPECT_EQ(objectEntry.buffer->GetDebugName(), "EngineObjectConstantsSnapshot");

    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(objectEntry.buffer);
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_GE(objectBuffer->uploadData.size(), sizeof(NLS::Maths::Matrix4));

    NLS::Maths::Matrix4 capturedMatrix;
    std::memcpy(&capturedMatrix, objectBuffer->uploadData.data(), sizeof(capturedMatrix));
    const auto expectedShaderMatrix = NLS::Maths::Matrix4::Transpose(modelMatrix);
    for (size_t index = 0u; index < std::size(capturedMatrix.data); ++index)
        EXPECT_FLOAT_EQ(capturedMatrix.data[index], expectedShaderMatrix.data[index]);

    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderCapturesPerFrameObjectBufferOnceForIndexedDraws)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 32u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    const auto secondMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });

    NLS::Render::Entities::Drawable firstDrawable;
    firstDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        firstMatrix,
        NLS::Maths::Matrix4::Identity,
        0u
    });
    NLS::Render::Entities::Drawable secondDrawable;
    secondDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        secondMatrix,
        NLS::Maths::Matrix4::Identity,
        1u
    });

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, firstDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets firstBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, firstDrawable, firstBindings));
    ASSERT_NE(firstBindings.objectBindingSet, nullptr);

    provider.PrepareDraw(pso, secondDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets secondBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, secondDrawable, secondBindings));

    EXPECT_EQ(secondBindings.objectBindingSet, firstBindings.objectBindingSet);

    const auto objectSetCount = static_cast<size_t>(std::count_if(
        explicitDevice->bindingSetDescs.begin(),
        explicitDevice->bindingSetDescs.end(),
        [](const NLS::Render::RHI::RHIBindingSetDesc& desc)
        {
            return desc.debugName == "EngineObjectBindingSet";
        }));
    EXPECT_EQ(objectSetCount, 1u);

    ASSERT_NE(secondBindings.objectBindingSet, nullptr);
    ASSERT_EQ(secondBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    const auto& objectEntry = secondBindings.objectBindingSet->GetDesc().entries[0];
    EXPECT_EQ(objectEntry.type, NLS::Render::RHI::BindingType::StructuredBuffer);
    ASSERT_NE(objectEntry.buffer, nullptr);
    EXPECT_EQ(objectEntry.buffer->GetDebugName(), "EngineObjectDataBuffer");
    EXPECT_EQ(objectEntry.elementStride, sizeof(NLS::Maths::Matrix4));

    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(objectEntry.buffer);
    ASSERT_NE(objectBuffer, nullptr);
    EXPECT_EQ(objectBuffer->updateCalls, 2u);
    ASSERT_GE(objectBuffer->uploadData.size(), sizeof(NLS::Maths::Matrix4) * 2u);

    NLS::Maths::Matrix4 capturedFirst;
    NLS::Maths::Matrix4 capturedSecond;
    std::memcpy(&capturedFirst, objectBuffer->uploadData.data(), sizeof(capturedFirst));
    std::memcpy(&capturedSecond, objectBuffer->uploadData.data() + sizeof(capturedFirst), sizeof(capturedSecond));

    const auto expectedFirst = NLS::Maths::Matrix4::Transpose(firstMatrix);
    const auto expectedSecond = NLS::Maths::Matrix4::Transpose(secondMatrix);
    for (size_t index = 0u; index < std::size(capturedFirst.data); ++index)
    {
        EXPECT_FLOAT_EQ(capturedFirst.data[index], expectedFirst.data[index]);
        EXPECT_FLOAT_EQ(capturedSecond.data[index], expectedSecond.data[index]);
    }

    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderPreservesEarlierObjectDataWhenFrameBufferGrows)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 36u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    const auto growthMatrix = NLS::Maths::Matrix4::Translation({ 9.0f, 8.0f, 7.0f });

    NLS::Render::Entities::Drawable firstDrawable;
    firstDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        firstMatrix,
        NLS::Maths::Matrix4::Identity,
        0u
    });
    NLS::Render::Entities::Drawable growthDrawable;
    growthDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        growthMatrix,
        NLS::Maths::Matrix4::Identity,
        300u
    });

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, firstDrawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets firstBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, firstDrawable, firstBindings));

    ASSERT_TRUE(provider.PrepareDraw(pso, growthDrawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets growthBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, growthDrawable, growthBindings));

    ASSERT_NE(growthBindings.objectBindingSet, nullptr);
    ASSERT_EQ(growthBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(
        growthBindings.objectBindingSet->GetDesc().entries[0].buffer);
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_GE(objectBuffer->uploadData.size(), 301u * sizeof(NLS::Maths::Matrix4));

    NLS::Maths::Matrix4 capturedFirst;
    NLS::Maths::Matrix4 capturedGrowth;
    std::memcpy(&capturedFirst, objectBuffer->uploadData.data(), sizeof(capturedFirst));
    std::memcpy(
        &capturedGrowth,
        objectBuffer->uploadData.data() + 300u * sizeof(NLS::Maths::Matrix4),
        sizeof(capturedGrowth));

    const auto expectedFirst = NLS::Maths::Matrix4::Transpose(firstMatrix);
    const auto expectedGrowth = NLS::Maths::Matrix4::Transpose(growthMatrix);
    for (size_t index = 0u; index < std::size(capturedFirst.data); ++index)
    {
        EXPECT_FLOAT_EQ(capturedFirst.data[index], expectedFirst.data[index]);
        EXPECT_FLOAT_EQ(capturedGrowth.data[index], expectedGrowth.data[index]);
    }

    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderShrinksIdleHighWaterObjectBuffer)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 46u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto drawIndexedObject = [&](uint32_t objectIndex) -> std::shared_ptr<TestBuffer>
    {
        NLS::Render::Entities::Drawable drawable;
        drawable.material = &material;
        drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
            NLS::Maths::Matrix4::Identity,
            NLS::Maths::Matrix4::Identity,
            objectIndex
        });

        NLS::Render::Data::PipelineState pso;
        EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
        EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
        EXPECT_NE(bindings.objectBindingSet, nullptr);
        return std::dynamic_pointer_cast<TestBuffer>(bindings.objectBindingSet->GetDesc().entries[0].buffer);
    };

    provider.BeginFrame(frameDescriptor);
    auto highWaterBuffer = drawIndexedObject(300u);
    ASSERT_NE(highWaterBuffer, nullptr);
    EXPECT_GT(highWaterBuffer->GetDesc().size, 256u * sizeof(NLS::Maths::Matrix4));
    provider.EndFrame();

    for (uint32_t idleFrame = 0u; idleFrame < 3u; ++idleFrame)
    {
        provider.BeginFrame(frameDescriptor);
        provider.EndFrame();
    }

    provider.BeginFrame(frameDescriptor);
    auto resetBuffer = drawIndexedObject(0u);
    ASSERT_NE(resetBuffer, nullptr);
    EXPECT_NE(resetBuffer, highWaterBuffer);
    EXPECT_EQ(resetBuffer->GetDesc().size, 256u * sizeof(NLS::Maths::Matrix4));
    provider.EndFrame();

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsSparseExternalObjectIndexForIndexedDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 42u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        (1u << 20u) + 1u
    });

    NLS::Render::Data::PipelineState pso;
    EXPECT_FALSE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    EXPECT_FALSE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_TRUE(std::none_of(
        explicitDevice->buffers.begin(),
        explicitDevice->buffers.end(),
        [](const std::shared_ptr<TestBuffer>& buffer)
        {
            return buffer != nullptr && buffer->GetDebugName() == "EngineObjectDataBuffer";
        }));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderAssignsObjectIndexForManualIndexedShaderDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 49u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 3.0f, 4.0f, 5.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity
    });

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));

    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectIndex, 0u);
    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    const auto& objectEntry = bindings.objectBindingSet->GetDesc().entries[0];
    EXPECT_EQ(objectEntry.type, NLS::Render::RHI::BindingType::StructuredBuffer);
    ASSERT_NE(objectEntry.buffer, nullptr);
    EXPECT_EQ(objectEntry.buffer->GetDebugName(), "EngineObjectDataBuffer");

    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(objectEntry.buffer);
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_GE(objectBuffer->uploadData.size(), sizeof(NLS::Maths::Matrix4));

    NLS::Maths::Matrix4 capturedMatrix;
    std::memcpy(&capturedMatrix, objectBuffer->uploadData.data(), sizeof(capturedMatrix));
    const auto expectedMatrix = NLS::Maths::Matrix4::Transpose(modelMatrix);
    for (size_t index = 0u; index < std::size(capturedMatrix.data); ++index)
        EXPECT_FLOAT_EQ(capturedMatrix.data[index], expectedMatrix.data[index]);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderRestoresIndexedObjectBindingAfterLegacyObjectDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 34u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    const auto indexedMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f });
    const auto legacyMatrix = NLS::Maths::Matrix4::Translation({ 2.0f, 0.0f, 0.0f });

    NLS::Render::Entities::Drawable indexedDrawable;
    indexedDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        indexedMatrix,
        NLS::Maths::Matrix4::Identity,
        5u
    });

    NLS::Render::Entities::Drawable legacyDrawable;
    legacyDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        legacyMatrix,
        NLS::Maths::Matrix4::Identity
    });

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, indexedDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets firstIndexedBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, indexedDrawable, firstIndexedBindings));
    ASSERT_NE(firstIndexedBindings.objectBindingSet, nullptr);
    ASSERT_EQ(firstIndexedBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(firstIndexedBindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::StructuredBuffer);

    provider.PrepareDraw(pso, legacyDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets legacyBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, legacyDrawable, legacyBindings));
    ASSERT_NE(legacyBindings.objectBindingSet, nullptr);
    ASSERT_EQ(legacyBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(legacyBindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::UniformBuffer);

    provider.PrepareDraw(pso, indexedDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets secondIndexedBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, indexedDrawable, secondIndexedBindings));
    EXPECT_TRUE(secondIndexedBindings.usesObjectIndex);
    EXPECT_EQ(secondIndexedBindings.objectIndex, 5u);
    EXPECT_EQ(secondIndexedBindings.objectBindingSet, firstIndexedBindings.objectBindingSet);
    ASSERT_NE(secondIndexedBindings.objectBindingSet, nullptr);
    ASSERT_EQ(secondIndexedBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(secondIndexedBindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::StructuredBuffer);

    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderUploadsObjectIndexRangeForInstancedIndexedDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 33u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f });
    const auto secondMatrix = NLS::Maths::Matrix4::Translation({ 2.0f, 0.0f, 0.0f });
    const auto thirdMatrix = NLS::Maths::Matrix4::Translation({ 3.0f, 0.0f, 0.0f });

    NLS::Render::Entities::Drawable drawable;
    drawable.instanceCount = 3u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        firstMatrix,
        NLS::Maths::Matrix4::Identity,
        7u,
        3u,
        { firstMatrix, secondMatrix, thirdMatrix }
    });

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, drawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectIndex, 7u);

    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    const auto& objectEntry = bindings.objectBindingSet->GetDesc().entries[0];
    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(objectEntry.buffer);
    ASSERT_NE(objectBuffer, nullptr);
    EXPECT_EQ(objectBuffer->updateCalls, 1u);
    EXPECT_EQ(objectBuffer->lastUpdate.destinationOffset, 7u * sizeof(NLS::Maths::Matrix4));
    EXPECT_EQ(objectBuffer->lastUpdate.dataSize, 3u * sizeof(NLS::Maths::Matrix4));
    ASSERT_GE(objectBuffer->uploadData.size(), 10u * sizeof(NLS::Maths::Matrix4));

    NLS::Maths::Matrix4 capturedFirst;
    NLS::Maths::Matrix4 capturedSecond;
    NLS::Maths::Matrix4 capturedThird;
    const auto baseOffset = 7u * sizeof(NLS::Maths::Matrix4);
    std::memcpy(&capturedFirst, objectBuffer->uploadData.data() + baseOffset, sizeof(capturedFirst));
    std::memcpy(&capturedSecond, objectBuffer->uploadData.data() + baseOffset + sizeof(capturedFirst), sizeof(capturedSecond));
    std::memcpy(&capturedThird, objectBuffer->uploadData.data() + baseOffset + sizeof(capturedFirst) + sizeof(capturedSecond), sizeof(capturedThird));

    const auto expectedFirst = NLS::Maths::Matrix4::Transpose(firstMatrix);
    const auto expectedSecond = NLS::Maths::Matrix4::Transpose(secondMatrix);
    const auto expectedThird = NLS::Maths::Matrix4::Transpose(thirdMatrix);
    for (size_t index = 0u; index < std::size(capturedFirst.data); ++index)
    {
        EXPECT_FLOAT_EQ(capturedFirst.data[index], expectedFirst.data[index]);
        EXPECT_FLOAT_EQ(capturedSecond.data[index], expectedSecond.data[index]);
        EXPECT_FLOAT_EQ(capturedThird.data[index], expectedThird.data[index]);
    }

    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderUploadsMaterialOwnedGpuInstanceRangeForIndexedDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 35u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    material.SetGPUInstances(3);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        11u,
        3u,
        { modelMatrix, modelMatrix, modelMatrix }
    });

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, drawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectIndex, 11u);

    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    const auto& objectEntry = bindings.objectBindingSet->GetDesc().entries[0];
    EXPECT_EQ(objectEntry.type, NLS::Render::RHI::BindingType::StructuredBuffer);
    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(objectEntry.buffer);
    ASSERT_NE(objectBuffer, nullptr);
    EXPECT_EQ(objectBuffer->updateCalls, 1u);
    EXPECT_EQ(objectBuffer->lastUpdate.destinationOffset, 11u * sizeof(NLS::Maths::Matrix4));
    EXPECT_EQ(objectBuffer->lastUpdate.dataSize, 3u * sizeof(NLS::Maths::Matrix4));

    NLS::Maths::Matrix4 capturedThird;
    const auto baseOffset = (11u + 2u) * sizeof(NLS::Maths::Matrix4);
    ASSERT_GE(objectBuffer->uploadData.size(), baseOffset + sizeof(capturedThird));
    std::memcpy(&capturedThird, objectBuffer->uploadData.data() + baseOffset, sizeof(capturedThird));
    const auto expectedShaderMatrix = NLS::Maths::Matrix4::Transpose(modelMatrix);
    for (size_t index = 0u; index < std::size(capturedThird.data); ++index)
        EXPECT_FLOAT_EQ(capturedThird.data[index], expectedShaderMatrix.data[index]);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderUsesLegacyObjectConstantsWhenShaderLacksObjectData)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 36u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    auto& parameterStructs =
        const_cast<std::vector<NLS::Render::Resources::ShaderParameterStruct>&>(shader->GetParameterStructs());
    parameterStructs.clear();
    parameterStructs.push_back(
        NLS::Render::Resources::ShaderParameterStructBuilder("LegacyObjectParameters")
            .SetGroup(NLS::Render::Resources::ShaderParameterGroupKind::Object)
            .AddUniformBuffer(
                "ObjectConstants",
                0u,
                sizeof(NLS::Maths::Matrix4),
                NLS::Render::RHI::ShaderStageMask::Vertex)
            .Build());

    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 7.0f, 8.0f, 9.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        13u
    });

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, drawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_FALSE(bindings.usesObjectIndex);

    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    const auto& objectEntry = bindings.objectBindingSet->GetDesc().entries[0];
    EXPECT_EQ(objectEntry.type, NLS::Render::RHI::BindingType::UniformBuffer);
    ASSERT_NE(objectEntry.buffer, nullptr);
    EXPECT_EQ(objectEntry.buffer->GetDebugName(), "EngineObjectConstantsSnapshot");

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsIndexedShaderWhenObjectDataRangeCannotBePrepared)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 38u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 3.0f, 4.0f, 5.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = 2u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        17u,
        2u,
        {}
    });

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, drawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    EXPECT_FALSE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_FALSE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectBindingSet, nullptr);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsIndexedShaderWhenObjectDataRangeExceedsSharedLimit)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 47u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 3.0f, 4.0f, 5.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = (1u << 20u) + 1u;
    std::vector<NLS::Maths::Matrix4> instanceMatrices(2u, modelMatrix);
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        0u,
        (1u << 20u) + 1u,
        std::move(instanceMatrices)
    });

    NLS::Render::Data::PipelineState pso;
    EXPECT_FALSE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    EXPECT_FALSE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_EQ(bindings.objectBindingSet, nullptr);
    EXPECT_TRUE(std::none_of(
        explicitDevice->buffers.begin(),
        explicitDevice->buffers.end(),
        [](const std::shared_ptr<TestBuffer>& buffer)
        {
            return buffer != nullptr && buffer->GetDebugName() == "EngineObjectDataBuffer";
        }));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsIndexedShaderBeforeAllocatingWhenInstanceMatricesAreIncomplete)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 44u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 3.0f, 4.0f, 5.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = 2u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        17u,
        2u,
        { modelMatrix }
    });

    NLS::Render::Data::PipelineState pso;
    EXPECT_FALSE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    EXPECT_FALSE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_EQ(bindings.objectBindingSet, nullptr);
    EXPECT_TRUE(std::none_of(
        explicitDevice->buffers.begin(),
        explicitDevice->buffers.end(),
        [](const std::shared_ptr<TestBuffer>& buffer)
        {
            return buffer != nullptr && buffer->GetDebugName() == "EngineObjectDataBuffer";
        }));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsIndexedShaderWhenObjectDescriptorIsMissing)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 40u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;

    NLS::Render::Data::PipelineState pso;
    EXPECT_FALSE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    EXPECT_FALSE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_FALSE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectBindingSet, nullptr);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderUsesReflectionObjectDataShaderForIndexedDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 48u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = CreateReflectionOnlyObjectDataShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 6.0f, 7.0f, 8.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        33u
    });

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectIndex, 33u);
    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(
        bindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::StructuredBuffer);
    ASSERT_NE(bindings.objectBindingSet->GetDesc().entries[0].buffer, nullptr);
    EXPECT_EQ(bindings.objectBindingSet->GetDesc().entries[0].buffer->GetDebugName(), "EngineObjectDataBuffer");

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, ReflectionObjectDataRequiresMatrixStrideForIndexedDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 49u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    auto* shader = CreateReflectionOnlyObjectDataShader();
    ASSERT_NE(shader, nullptr);
    auto& reflection = const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection());
    ASSERT_EQ(reflection.properties.size(), 1u);
    reflection.properties[0].byteSize = sizeof(uint32_t);

    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity,
        34u
    });

    NLS::Render::Data::PipelineState pso;
    EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_FALSE(bindings.usesObjectIndex);
    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(
        bindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::UniformBuffer);
    EXPECT_NE(bindings.objectBindingSet->GetDesc().entries[0].buffer->GetDebugName(), "EngineObjectDataBuffer");

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, ImmediateIndexedShaderDrawIsSkippedWhenObjectDataRangeCannotBePrepared)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 39u;
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    ImmediateObjectIndexRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(
        std::make_unique<NLS::Engine::Rendering::EngineFrameObjectBindingProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    renderer.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 6.0f, 7.0f, 8.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = 2u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        23u,
        2u,
        {}
    });

    NLS::Render::Data::PipelineState pso;
    renderer.DrawEntity(pso, drawable);

    EXPECT_EQ(renderer.commandBuffer()->bindGraphicsPipelineCalls, 0u);
    EXPECT_EQ(renderer.commandBuffer()->pushConstantCalls, 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderKeepsIndexedObjectBuffersIsolatedAcrossFrameSlots)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    for (size_t slotIndex = 0u; slotIndex < 2u; ++slotIndex)
    {
        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, slotIndex);
        frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
        ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    }

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::CanBeginStandaloneExplicitFrame(driver));
    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false);
    provider.BeginFrame(frameDescriptor);

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    NLS::Render::Entities::Drawable firstDrawable;
    firstDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        firstMatrix,
        NLS::Maths::Matrix4::Identity,
        0u
    });

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, firstDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets firstBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, firstDrawable, firstBindings));
    ASSERT_TRUE(firstBindings.usesObjectIndex);
    ASSERT_NE(firstBindings.objectBindingSet, nullptr);
    ASSERT_EQ(firstBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    auto firstBuffer = std::dynamic_pointer_cast<TestBuffer>(firstBindings.objectBindingSet->GetDesc().entries[0].buffer);
    ASSERT_NE(firstBuffer, nullptr);
    ASSERT_GE(firstBuffer->uploadData.size(), sizeof(NLS::Maths::Matrix4));

    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);

    NLS::Render::Context::DriverTestAccess::BeginStandaloneExplicitFrame(driver, false);
    provider.BeginFrame(frameDescriptor);

    const auto secondMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });
    NLS::Render::Entities::Drawable secondDrawable;
    secondDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        secondMatrix,
        NLS::Maths::Matrix4::Identity,
        0u
    });

    provider.PrepareDraw(pso, secondDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets secondBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, secondDrawable, secondBindings));
    ASSERT_TRUE(secondBindings.usesObjectIndex);
    ASSERT_NE(secondBindings.objectBindingSet, nullptr);
    ASSERT_EQ(secondBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    auto secondBuffer = std::dynamic_pointer_cast<TestBuffer>(secondBindings.objectBindingSet->GetDesc().entries[0].buffer);
    ASSERT_NE(secondBuffer, nullptr);

    EXPECT_NE(secondBuffer, firstBuffer);

    NLS::Maths::Matrix4 capturedFirst;
    std::memcpy(&capturedFirst, firstBuffer->uploadData.data(), sizeof(capturedFirst));
    const auto expectedFirst = NLS::Maths::Matrix4::Transpose(firstMatrix);
    for (size_t index = 0u; index < std::size(capturedFirst.data); ++index)
        EXPECT_FLOAT_EQ(capturedFirst.data[index], expectedFirst.data[index]);

    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::EndStandaloneExplicitFrame(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderFailsClosedWhenPreparedThreadedSlotsAreBackPressured)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 40u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 1u);
    secondFrameContext.frameIndex = 41u;
    secondFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(secondFrameContext.descriptorAllocator, nullptr);
    secondFrameContext.descriptorAllocator->BeginFrame(secondFrameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    NLS::Render::Data::PipelineState pso;

    const auto prepareFrame = [&](const NLS::Maths::Matrix4& matrix) -> std::shared_ptr<TestBuffer>
    {
        provider.BeginFrame(frameDescriptor);
        NLS::Render::Entities::Drawable drawable;
        drawable.material = &material;
        drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
            matrix,
            NLS::Maths::Matrix4::Identity,
            0u
        });

        if (!provider.PrepareDraw(pso, drawable))
        {
            ADD_FAILURE() << "Expected indexed object data preparation to succeed";
            provider.EndFrame();
            return nullptr;
        }

        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
        if (!provider.CapturePreparedBindingSets(pso, drawable, bindings) ||
            !bindings.usesObjectIndex ||
            bindings.objectBindingSet == nullptr ||
            bindings.objectBindingSet->GetDesc().entries.empty())
        {
            ADD_FAILURE() << "Expected prepared object data binding set";
            provider.EndFrame();
            return nullptr;
        }

        auto buffer = std::dynamic_pointer_cast<TestBuffer>(bindings.objectBindingSet->GetDesc().entries[0].buffer);
        EXPECT_NE(buffer, nullptr);
        provider.EndFrame();
        return buffer;
    };

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    const auto secondMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });
    auto firstBuffer = prepareFrame(firstMatrix);
    ASSERT_NE(firstBuffer, nullptr);

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 101u;
    NLS::Render::Context::RenderScenePackage firstPackage;
    firstPackage.frameId = firstSnapshot.frameId;
    size_t firstPublishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        firstSnapshot,
        firstPackage,
        &firstPublishedSlot));
    EXPECT_EQ(firstPublishedSlot, 0u);

    auto secondBuffer = prepareFrame(secondMatrix);
    ASSERT_NE(secondBuffer, nullptr);
    EXPECT_NE(secondBuffer, firstBuffer);

    NLS::Render::Context::FrameSnapshot secondSnapshot;
    secondSnapshot.frameId = 102u;
    NLS::Render::Context::RenderScenePackage secondPackage;
    secondPackage.frameId = secondSnapshot.frameId;
    size_t secondPublishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        secondSnapshot,
        secondPackage,
        &secondPublishedSlot));
    EXPECT_EQ(secondPublishedSlot, 1u);

    provider.BeginFrame(frameDescriptor);
    const auto thirdMatrix = NLS::Maths::Matrix4::Translation({ 7.0f, 8.0f, 9.0f });
    NLS::Render::Entities::Drawable thirdDrawable;
    thirdDrawable.material = &material;
    thirdDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        thirdMatrix,
        NLS::Maths::Matrix4::Identity,
        0u
    });

    EXPECT_FALSE(provider.PrepareDraw(pso, thirdDrawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets thirdBindings;
    EXPECT_FALSE(provider.CapturePreparedBindingSets(pso, thirdDrawable, thirdBindings));
    provider.EndFrame();

    NLS::Maths::Matrix4 capturedFirst;
    std::memcpy(&capturedFirst, firstBuffer->uploadData.data(), sizeof(capturedFirst));
    const auto expectedFirst = NLS::Maths::Matrix4::Transpose(firstMatrix);
    for (size_t index = 0u; index < std::size(capturedFirst.data); ++index)
        EXPECT_FLOAT_EQ(capturedFirst.data[index], expectedFirst.data[index]);

    const auto objectSetCount = static_cast<size_t>(std::count_if(
        explicitDevice->bindingSetDescs.begin(),
        explicitDevice->bindingSetDescs.end(),
        [](const NLS::Render::RHI::RHIBindingSetDesc& desc)
        {
            return desc.debugName == "EngineObjectBindingSet";
        }));
    EXPECT_LE(objectSetCount, 2u);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, EngineProviderReusesPreparedObjectBufferAfterLifecycleSlotRetires)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 2u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 41u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 1u);
    secondFrameContext.frameIndex = 42u;
    secondFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(secondFrameContext.descriptorAllocator, nullptr);
    secondFrameContext.descriptorAllocator->BeginFrame(secondFrameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    NLS::Render::Data::PipelineState pso;

    const auto prepareFrame = [&](const NLS::Maths::Matrix4& matrix) -> std::shared_ptr<TestBuffer>
    {
        provider.BeginFrame(frameDescriptor);
        NLS::Render::Entities::Drawable drawable;
        drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
            matrix,
            NLS::Maths::Matrix4::Identity,
            0u
        });
        EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
        EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
        provider.EndFrame();
        return std::dynamic_pointer_cast<TestBuffer>(bindings.objectBindingSet->GetDesc().entries[0].buffer);
    };

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    const auto secondMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });
    auto firstBuffer = prepareFrame(firstMatrix);
    ASSERT_NE(firstBuffer, nullptr);

    NLS::Render::Context::FrameSnapshot firstSnapshot;
    firstSnapshot.frameId = 201u;
    NLS::Render::Context::RenderScenePackage firstPackage;
    firstPackage.frameId = firstSnapshot.frameId;
    size_t firstPublishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        firstSnapshot,
        firstPackage,
        &firstPublishedSlot));
    EXPECT_EQ(firstPublishedSlot, 0u);

    auto secondBuffer = prepareFrame(secondMatrix);
    ASSERT_NE(secondBuffer, nullptr);
    EXPECT_NE(firstBuffer, secondBuffer);

    auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);

    NLS::Render::Context::FrameSnapshot secondSnapshot;
    secondSnapshot.frameId = 202u;
    NLS::Render::Context::RenderScenePackage secondPackage;
    secondPackage.frameId = secondSnapshot.frameId;
    size_t secondPublishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        secondSnapshot,
        secondPackage,
        &secondPublishedSlot));
    EXPECT_EQ(secondPublishedSlot, 1u);

    ASSERT_TRUE(lifecycle->TryBeginRenderScene(0u));
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::ResolveAndCompleteThreadedRenderScene(driver, 0u));
    ASSERT_TRUE(lifecycle->TryBeginRhiSubmission(0u));
    NLS::Render::Context::RhiSubmissionFrame submissionFrame;
    submissionFrame.frameId = firstSnapshot.frameId;
    ASSERT_TRUE(lifecycle->CompleteRhiSubmission(0u, submissionFrame));
    ASSERT_TRUE(lifecycle->RetireFrame(0u));

    const auto thirdMatrix = NLS::Maths::Matrix4::Translation({ 7.0f, 8.0f, 9.0f });
    auto thirdBuffer = prepareFrame(thirdMatrix);
    ASSERT_NE(thirdBuffer, nullptr);
    EXPECT_EQ(thirdBuffer, firstBuffer);
}

TEST(RendererFrameObjectBindingTests, ImmediateIndexedDrawPushesObjectIndexAfterProviderBindsObjectData)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 37u;
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    ImmediateObjectIndexRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(
        std::make_unique<NLS::Engine::Rendering::EngineFrameObjectBindingProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    renderer.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 2.0f, 4.0f, 6.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = 1u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        21u
    });

    NLS::Render::Data::PipelineState pso;
    renderer.DrawEntity(pso, drawable);

    EXPECT_EQ(renderer.commandBuffer()->bindGraphicsPipelineCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer()->pushConstantCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer()->lastObjectIndexPushConstant, 21u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, ImmediateIndexedShaderDrawAssignsObjectIndexWhenMissing)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 41u;
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    ImmediateObjectIndexRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(
        std::make_unique<NLS::Engine::Rendering::EngineFrameObjectBindingProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    renderer.BeginFrame(frameDescriptor);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 2.0f, 4.0f, 6.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = 1u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex
    });

    NLS::Render::Data::PipelineState pso;
    renderer.DrawEntity(pso, drawable);

    EXPECT_EQ(renderer.commandBuffer()->bindGraphicsPipelineCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer()->pushConstantCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer()->lastObjectIndexPushConstant, 0u);

    const auto indexedObjectSetCount = static_cast<size_t>(std::count_if(
        explicitDevice->bindingSetDescs.begin(),
        explicitDevice->bindingSetDescs.end(),
        [](const NLS::Render::RHI::RHIBindingSetDesc& desc)
        {
            return desc.debugName == "EngineObjectBindingSet" &&
                !desc.entries.empty() &&
                desc.entries[0].type == NLS::Render::RHI::BindingType::StructuredBuffer;
        }));
    EXPECT_EQ(indexedObjectSetCount, 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, ObjectIndexedRecordedDrawPushesObjectIndexBeforeSubmission)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    ObjectIndexSubmitRenderer renderer(*driver);
    renderer.SubmitWithObjectIndex(9u);

    EXPECT_EQ(renderer.commandBuffer->pushConstantCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer->lastObjectIndexPushConstant, 9u);
}

TEST(RendererFrameObjectBindingTests, ExplicitBindingSetCreationRequiresCentralDescriptorAllocator)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
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
    const ScopedDriverService driverService(*driver);

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

TEST(RendererFrameObjectBindingTests, MaterialRefreshesCachedBindingStateWhenShaderGenerationChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    const auto initialGeneration = shader->GetGeneration();
    EXPECT_EQ(material.GetCachedShaderGenerationForTesting(), initialGeneration);

    auto reflection = shader->GetReflection();
    reflection.properties.push_back({
        "u_ReloadTint",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
        31u,
        -1,
        1,
        64u,
        16u,
        "MaterialConstants"
    });
    shader->SetReflectionForTesting(std::move(reflection));
    ASSERT_NE(shader->GetGeneration(), initialGeneration);

    EXPECT_FALSE(material.GetParameterBlock().Contains("u_ReloadTint"));
    material.GetExplicitBindingLayout(nullptr);

    EXPECT_EQ(material.GetCachedShaderGenerationForTesting(), shader->GetGeneration());
    EXPECT_TRUE(material.GetParameterBlock().Contains("u_ReloadTint"));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingSetReportsMissingRequiredBindings)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

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
    const ScopedDriverService driverService(*driver);

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
    const ScopedDriverService driverService(*driver);

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
    const ScopedDriverService driverService(*driver);

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
    const ScopedDriverService driverService(*driver);

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
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[2]->GetDesc().entries[0].name, "ObjectData");
    EXPECT_EQ(
        explicitDevice->lastPipelineLayoutDesc.bindingLayouts[2]->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::StructuredBuffer);
    ASSERT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants.size(), 1u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants[0].shaderRegister, 1u);
    EXPECT_EQ(
        explicitDevice->lastPipelineLayoutDesc.pushConstants[0].registerSpace,
        NLS::Render::RHI::BindingPointMap::kObjectBindingSpace);
    ASSERT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries.size(), 4u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[0].name, "ForwardLightData");
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[0].type, NLS::Render::RHI::BindingType::UniformBuffer);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[0].binding, 0u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[0].registerSpace, NLS::Render::RHI::BindingPointMap::kPassBindingSpace);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.bindingLayouts[3]->GetDesc().entries[1].name, "u_ForwardLocalLightBuffer");

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialRequiresPassDescriptorSetWhenParameterStructDefinesPassBindings)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::ShaderReflection reflectionWithoutPassBindings;
    const_cast<NLS::Render::Resources::ShaderReflection&>(shader->GetReflection()) =
        std::move(reflectionWithoutPassBindings);
    auto& parameterStructs =
        const_cast<std::vector<NLS::Render::Resources::ShaderParameterStruct>&>(shader->GetParameterStructs());
    parameterStructs.clear();
    parameterStructs.push_back(
        NLS::Render::Resources::ShaderParameterStructBuilder("MaterialOnlyParameters")
            .SetGroup(NLS::Render::Resources::ShaderParameterGroupKind::Material)
            .AddUniformBuffer(
                "MaterialConstants",
                0u,
                sizeof(NLS::Maths::Vector4),
                NLS::Render::RHI::ShaderStageMask::Fragment)
            .Build());
    parameterStructs.push_back(
        NLS::Render::Resources::ShaderParameterStructBuilder("PassOnlyParameters")
            .SetGroup(NLS::Render::Resources::ShaderParameterGroupKind::Pass)
            .AddStructuredBuffer(
                "ForwardLightData",
                0u,
                NLS::Render::RHI::ShaderStageMask::Fragment)
            .Build());

    NLS::Render::Resources::Material material(shader);

    EXPECT_TRUE(material.RequiresPassDescriptorSet());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutRejectsIndexedObjectDataOnUnsupportedBackend)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    ASSERT_TRUE(shader->HasParameterStructs());

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::Vulkan);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    EXPECT_EQ(pipelineLayout, nullptr);
    EXPECT_EQ(explicitDevice->pipelineLayoutCreateCalls, 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, DeferredLightingPipelineLayoutSkipsEmptyFrameAndObjectDescriptorSets)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/DeferredLighting.hlsl");
    ASSERT_NE(shader, nullptr);
    ASSERT_TRUE(shader->HasParameterStructs());

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);

    const auto& bindingLayouts = explicitDevice->lastPipelineLayoutDesc.bindingLayouts;
    ASSERT_EQ(bindingLayouts.size(), 2u);
    ASSERT_NE(bindingLayouts[0], nullptr);
    ASSERT_NE(bindingLayouts[1], nullptr);
    EXPECT_EQ(bindingLayouts[0]->GetDesc().entries[0].name, "MaterialConstants");
    EXPECT_EQ(bindingLayouts[0]->GetDesc().entries[0].set, NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet);
    EXPECT_EQ(bindingLayouts[1]->GetDesc().entries[0].name, "ForwardLightData");
    EXPECT_EQ(bindingLayouts[1]->GetDesc().entries[0].set, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, LightGridFallbackGraphicsBindingSetUsesShaderExpectedPassBufferNames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 11u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(16u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    NLS::Engine::Rendering::LightGridPrepass lightGridPrepass(driver);
    ASSERT_TRUE(lightGridPrepass.EnsureFallbackGraphicsPassBindingSet(frameDescriptor, false));

    const auto& bindingSet = lightGridPrepass.GetGraphicsPassBindingSet();
    ASSERT_NE(bindingSet, nullptr);
    ASSERT_NE(bindingSet->GetDesc().layout, nullptr);

    const auto& layoutEntries = bindingSet->GetDesc().layout->GetDesc().entries;
    const auto hasStructuredBuffer = [&layoutEntries](std::string_view name, uint32_t binding)
    {
        return std::any_of(
            layoutEntries.begin(),
            layoutEntries.end(),
            [name, binding](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
            {
                return entry.name == name &&
                    entry.type == NLS::Render::RHI::BindingType::StructuredBuffer &&
                    entry.binding == binding;
            });
    };

    EXPECT_TRUE(hasStructuredBuffer("u_ForwardLocalLightBuffer", 0u));
    EXPECT_TRUE(hasStructuredBuffer("u_NumCulledLightsGrid", 1u));
    EXPECT_TRUE(hasStructuredBuffer("u_CulledLightDataGrid", 2u));

    const auto& bindingEntries = bindingSet->GetDesc().entries;
    EXPECT_EQ(bindingEntries.size(), 4u);

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, LightGridComputeBindingSetsUseShaderExpectedResourceNames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    const ScopedShaderManagerAssetPaths shaderAssetPaths("", "App/Assets/Engine/");
    static auto shaderManager = std::make_unique<NLS::Core::ResourceManagement::ShaderManager>();
    const ScopedShaderManagerService shaderManagerService(*shaderManager);
    RegisterLightGridTestShaders(*shaderManager);

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 12u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Maths::Transform lightTransform;
    lightTransform.SetWorldPosition({ 0.0f, 0.0f, 5.0f });
    NLS::Render::Entities::Light pointLight(&lightTransform);
    pointLight.type = NLS::Render::Settings::ELightType::POINT;
    pointLight.constant = 1.0f;
    pointLight.linear = 0.0f;
    pointLight.quadratic = 0.04f;
    pointLight.intensity = 8.0f;

    NLS::Render::Data::LightingDescriptor lightingDescriptor;
    lightingDescriptor.lights.emplace_back(std::cref(pointLight));

    NLS::Render::Entities::Camera camera;
    camera.CacheMatrices(320u, 180u);
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 320u;
    frameDescriptor.renderHeight = 180u;
    frameDescriptor.camera = &camera;

    NLS::Engine::Rendering::LightGridPrepass lightGridPrepass(driver);
    ASSERT_TRUE(lightGridPrepass.Prepare(frameDescriptor, lightingDescriptor, false));
    ASSERT_GE(explicitDevice->bindingSetDescs.size(), 4u);

    const auto findSet = [&](std::string_view debugName) -> const NLS::Render::RHI::RHIBindingSetDesc*
    {
        const auto found = std::find_if(
            explicitDevice->bindingSetDescs.begin(),
            explicitDevice->bindingSetDescs.end(),
            [debugName](const NLS::Render::RHI::RHIBindingSetDesc& desc)
            {
                return desc.debugName == debugName;
            });
        return found != explicitDevice->bindingSetDescs.end() ? &(*found) : nullptr;
    };
    const auto hasEntry = [](const NLS::Render::RHI::RHIBindingSetDesc& desc, std::string_view name, NLS::Render::RHI::BindingType type, uint32_t binding)
    {
        return std::any_of(
            desc.layout->GetDesc().entries.begin(),
            desc.layout->GetDesc().entries.end(),
            [name, type, binding](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
            {
                return entry.name == name && entry.type == type && entry.binding == binding;
            });
    };

    const auto* resetSet = findSet("LightGridResetBindingSet");
    const auto* injectionSet = findSet("LightGridInjectionBindingSet");
    const auto* compactSet = findSet("LightGridCompactBindingSet");
    ASSERT_NE(resetSet, nullptr);
    ASSERT_NE(injectionSet, nullptr);
    ASSERT_NE(compactSet, nullptr);

    EXPECT_TRUE(hasEntry(*injectionSet, "u_ForwardLocalLightBuffer", NLS::Render::RHI::BindingType::StructuredBuffer, 0u));
    EXPECT_TRUE(hasEntry(*injectionSet, "u_LightGridStartOffsetGrid", NLS::Render::RHI::BindingType::StorageBuffer, 1u));
    EXPECT_TRUE(hasEntry(*injectionSet, "u_LightGridCulledLightLinks", NLS::Render::RHI::BindingType::StorageBuffer, 2u));
    EXPECT_TRUE(hasEntry(*injectionSet, "u_LightGridLinkCounter", NLS::Render::RHI::BindingType::StorageBuffer, 3u));
    EXPECT_TRUE(hasEntry(*resetSet, "u_NumCulledLightsGrid", NLS::Render::RHI::BindingType::StorageBuffer, 5u));
    EXPECT_TRUE(hasEntry(*resetSet, "u_CulledLightDataGrid", NLS::Render::RHI::BindingType::StorageBuffer, 6u));
    EXPECT_TRUE(hasEntry(*compactSet, "u_LightGridStartOffsetGrid", NLS::Render::RHI::BindingType::StructuredBuffer, 1u));
    EXPECT_TRUE(hasEntry(*compactSet, "u_LightGridCulledLightLinks", NLS::Render::RHI::BindingType::StructuredBuffer, 2u));

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
    shaderManager->UnloadResources();
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
    const ScopedDriverService driverService(*driver);

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
    const ScopedDriverService driverService(driver);
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
    const ScopedDriverService driverService(driver);
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

TEST(RendererFrameObjectBindingTests, RendererCachesCameraMatricesBeforeFrameObjectBindingBeginFrame)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);

    NLS::Render::Core::CompositeRenderer renderer(driver);
    const NLS::Maths::Vector3 cameraPosition{ -10.0f, 3.0f, 10.0f };
    const NLS::Maths::Quaternion cameraRotation{ NLS::Maths::Vector3(0.0f, 135.0f, 0.0f) };
    NLS::Maths::Transform cameraTransform;
    cameraTransform.SetWorldPosition(cameraPosition);
    cameraTransform.SetWorldRotation(cameraRotation);
    NLS::Render::Entities::Camera camera(&cameraTransform);
    NLS::Maths::Transform expectedCameraTransform;
    expectedCameraTransform.SetWorldPosition(cameraPosition);
    expectedCameraTransform.SetWorldRotation(cameraRotation);
    NLS::Render::Entities::Camera expectedCamera(&expectedCameraTransform);
    expectedCamera.CacheMatrices(256u, 144u);
    const auto expectedViewMatrix = expectedCamera.GetViewMatrix();

    auto provider = std::make_unique<CameraMatrixProbeBindingProvider>(renderer, camera);
    auto* providerPtr = provider.get();
    renderer.SetFrameObjectBindingProvider(std::move(provider));

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    for (size_t index = 0u; index < std::size(providerPtr->observedViewMatrix.data); ++index)
        EXPECT_FLOAT_EQ(providerPtr->observedViewMatrix.data[index], expectedViewMatrix.data[index]);
    ASSERT_NO_THROW(renderer.EndFrame());
}

TEST(RendererFrameObjectBindingTests, ForwardThreadedOffscreenPackageRegistersExternalOutputExtraction)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
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
