#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <tuple>
#include <vector>

#include "Rendering/DeferredSceneRenderer.h"

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Resources/Texture2D.h"
#include "Core/ServiceLocator.h"
#include "Guid.h"

namespace
{
    std::string ReadRepoFile(const std::filesystem::path& relativePath)
    {
        std::ifstream input(std::filesystem::path(NLS_ROOT_DIR) / relativePath);
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
    }

    class DeferredTestAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "DeferredSceneRendererTestsAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
        std::string_view GetVendor() const override { return "TestVendor"; }
        std::string_view GetHardware() const override { return "TestHardware"; }
    };

    class DeferredTestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit DeferredTestTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class DeferredTestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        DeferredTestTextureView(
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

    class DeferredTestCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "DeferredSceneRendererTestsCommandBuffer"; }
        void Begin() override { m_recording = true; m_closed = false; }
        void End() override { m_recording = false; m_closed = true; }
        void Reset() override { m_recording = false; m_closed = false; }
        bool IsRecording() const override { return m_recording; }
        bool IsClosedForSubmission() const override { return m_closed; }
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

    private:
        bool m_recording = false;
        bool m_closed = false;
    };

    class DeferredTestCommandPool final : public NLS::Render::RHI::RHICommandPool
    {
    public:
        DeferredTestCommandPool(NLS::Render::RHI::QueueType queueType, std::string debugName)
            : m_queueType(queueType)
            , m_debugName(std::move(debugName))
        {
        }

        std::string_view GetDebugName() const override { return m_debugName; }
        NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string = {}) override
        {
            return std::make_shared<DeferredTestCommandBuffer>();
        }
        void Reset() override {}

    private:
        NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
        std::string m_debugName;
    };

    class DeferredTestFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        explicit DeferredTestFence(std::string debugName)
            : m_debugName(std::move(debugName))
        {
        }

        std::string_view GetDebugName() const override { return m_debugName; }
        bool IsSignaled() const override { return m_signaled; }
        void Reset() override { m_signaled = false; }
        bool Wait(uint64_t = 0u) override
        {
            m_signaled = true;
            return true;
        }

    private:
        std::string m_debugName;
        bool m_signaled = true;
    };

    class DeferredTestSemaphore final : public NLS::Render::RHI::RHISemaphore
    {
    public:
        explicit DeferredTestSemaphore(std::string debugName)
            : m_debugName(std::move(debugName))
        {
        }

        std::string_view GetDebugName() const override { return m_debugName; }
        bool IsSignaled() const override { return false; }
        void Reset() override {}

    private:
        std::string m_debugName;
    };

    class DeferredTestExplicitDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        DeferredTestExplicitDevice()
            : m_adapter(std::make_shared<DeferredTestAdapter>())
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.backendReady = true;
            m_capabilities.supportsGraphics = true;
            m_capabilities.supportsOffscreenFramebuffers = true;
            m_capabilities.supportsMultiRenderTargets = true;
        }

        std::string_view GetDebugName() const override { return "DeferredSceneRendererTestsDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
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
            ++textureCreateCalls;
            if (failTextureCreateCall != 0u && textureCreateCalls == failTextureCreateCall)
                return nullptr;
            return std::make_shared<DeferredTestTexture>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHITextureViewDesc& desc) override
        {
            return std::make_shared<DeferredTestTextureView>(texture, desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc&, std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
            NLS::Render::RHI::QueueType queueType,
            std::string debugName = {}) override
        {
            return std::make_shared<DeferredTestCommandPool>(queueType, std::move(debugName));
        }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override
        {
            return std::make_shared<DeferredTestFence>(std::move(debugName));
        }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName = {}) override
        {
            return std::make_shared<DeferredTestSemaphore>(std::move(debugName));
        }
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

        size_t textureCreateCalls = 0u;
        size_t failTextureCreateCall = 0u;

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    NLS::Render::Resources::ShaderReflection MakeDeferredMaterialShaderReflection()
    {
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.constantBuffers.push_back({
            "MaterialConstants",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            64u,
            {
                {"u_Diffuse", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u},
                {"u_DiffuseMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, 16u, 0u, 1u},
                {"u_Albedo", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 32u, 16u, 1u},
                {"u_AlbedoMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, 48u, 0u, 1u}
            }
        });
        for (const auto& [name, type, kind, offset, size] : {
            std::tuple{"u_Diffuse", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 0u, 16u},
            std::tuple{"u_DiffuseMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 16u, 0u},
            std::tuple{"u_Albedo", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 32u, 16u},
            std::tuple{"u_AlbedoMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 48u, 0u}
        })
        {
            reflection.properties.push_back({
                name,
                type,
                kind,
                NLS::Render::ShaderCompiler::ShaderStage::Pixel,
                NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
                0u,
                -1,
                1,
                offset,
                size,
                kind == NLS::Render::Resources::ShaderResourceKind::Value ? "MaterialConstants" : ""
            });
        }
        return reflection;
    }

    NLS::Render::Resources::Shader* CreateTestShader(const std::string& sourcePath)
    {
        NLS::Render::Assets::ShaderArtifact artifact;
        artifact.sourcePath = sourcePath;
        artifact.subAssetKey = "shader:test";
        artifact.reflection = MakeDeferredMaterialShaderReflection();
        artifact.stages.push_back({
            NLS::Render::ShaderCompiler::ShaderStage::Vertex,
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
            "VSMain",
            "vs_6_0",
            {
                NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                {1u, 2u, 3u, 4u},
                {},
                {},
                "test-vertex",
                "test.nshader"
            }
        });
        artifact.stages.push_back({
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
            "PSMain",
            "ps_6_0",
            {
                NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded,
                {5u, 6u, 7u, 8u},
                {},
                {},
                "test-pixel",
                "test.nshader"
            }
        });

        const auto root = std::filesystem::temp_directory_path() /
            ("nullus_deferred_shader_" + NLS::Guid::New().ToString());
        const auto path = root / "shader.nshader";
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(path.string());
        std::filesystem::remove_all(root);
        return shader;
    }

    NLS::Render::Resources::Material& SyncOneDeferredCacheMaterial(
        NLS::Engine::Rendering::DeferredSceneRenderer& renderer,
        NLS::Render::Resources::Material& sourceMaterial)
    {
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResetFrameGBufferMaterialSyncCount(renderer);
        return NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetOrCreateGBufferMaterial(
            renderer,
            sourceMaterial);
    }
}

TEST(DeferredSceneRendererMaterialCacheTests, ReusesStaticGBufferColorFormatOverrides)
{
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    EXPECT_NE(source.find("std::span<const NLS::Render::RHI::TextureFormat> GetDeferredGBufferColorFormats()"), std::string::npos);
    EXPECT_NE(source.find("overrides.SetColorFormats(GetDeferredGBufferColorFormats())"), std::string::npos);
    EXPECT_EQ(source.find("colorFormatsView"), std::string::npos);
    EXPECT_EQ(source.find("gBufferOverrides.colorFormats = GetDeferredGBufferColorFormats()"), std::string::npos);
    EXPECT_EQ(source.find("static const std::vector<NLS::Render::RHI::TextureFormat> kFormats"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, ReusesWrappedGBufferTargetsUntilSizeChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<DeferredTestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureGBufferTargets(renderer, 320u, 180u);

    const auto* firstAlbedo = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(renderer);
    const auto* firstNormal = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferNormalTexture(renderer);
    const auto* firstMaterial = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialTexture(renderer);
    const auto* firstDepth = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferDepthTexture(renderer);
    ASSERT_NE(firstAlbedo, nullptr);
    ASSERT_NE(firstNormal, nullptr);
    ASSERT_NE(firstMaterial, nullptr);
    ASSERT_NE(firstDepth, nullptr);
    const auto firstAlbedoId = firstAlbedo->GetInstanceID();
    const auto firstNormalId = firstNormal->GetInstanceID();
    const auto firstMaterialId = firstMaterial->GetInstanceID();
    const auto firstDepthId = firstDepth->GetInstanceID();
    const auto firstTextureCreateCalls = explicitDevice->textureCreateCalls;
    EXPECT_GE(firstTextureCreateCalls, 4u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureGBufferTargets(renderer, 320u, 180u);

    EXPECT_EQ(explicitDevice->textureCreateCalls, firstTextureCreateCalls);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(renderer)->GetInstanceID(), firstAlbedoId);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferNormalTexture(renderer)->GetInstanceID(), firstNormalId);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialTexture(renderer)->GetInstanceID(), firstMaterialId);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferDepthTexture(renderer)->GetInstanceID(), firstDepthId);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureGBufferTargets(renderer, 640u, 360u);

    EXPECT_GT(explicitDevice->textureCreateCalls, firstTextureCreateCalls);
    EXPECT_NE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(renderer)->GetInstanceID(), firstAlbedoId);
    EXPECT_NE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferNormalTexture(renderer)->GetInstanceID(), firstNormalId);
    EXPECT_NE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialTexture(renderer)->GetInstanceID(), firstMaterialId);
    EXPECT_NE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferDepthTexture(renderer)->GetInstanceID(), firstDepthId);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureGBufferTargets(renderer, 0u, 0u);

    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(renderer), nullptr);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferNormalTexture(renderer), nullptr);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialTexture(renderer), nullptr);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferDepthTexture(renderer), nullptr);
}

TEST(DeferredSceneRendererMaterialCacheTests, ClearsWrappedGBufferTargetsWhenResizeAllocationFails)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<DeferredTestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureGBufferTargets(renderer, 320u, 180u);

    ASSERT_NE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(renderer), nullptr);
    ASSERT_NE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferNormalTexture(renderer), nullptr);
    ASSERT_NE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialTexture(renderer), nullptr);
    ASSERT_NE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferDepthTexture(renderer), nullptr);

    explicitDevice->failTextureCreateCall = explicitDevice->textureCreateCalls + 1u;

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureGBufferTargets(renderer, 640u, 360u);

    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(renderer), nullptr);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferNormalTexture(renderer), nullptr);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialTexture(renderer), nullptr);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferDepthTexture(renderer), nullptr);

    explicitDevice->failTextureCreateCall = 0u;

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureGBufferTargets(renderer, 640u, 360u);

    const auto* retriedAlbedo = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(renderer);
    const auto* retriedNormal = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferNormalTexture(renderer);
    const auto* retriedMaterial = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialTexture(renderer);
    const auto* retriedDepth = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferDepthTexture(renderer);
    ASSERT_NE(retriedAlbedo, nullptr);
    ASSERT_NE(retriedNormal, nullptr);
    ASSERT_NE(retriedMaterial, nullptr);
    ASSERT_NE(retriedDepth, nullptr);
    EXPECT_EQ(retriedAlbedo->width, 640u);
    EXPECT_EQ(retriedAlbedo->height, 360u);
    EXPECT_EQ(retriedNormal->width, 640u);
    EXPECT_EQ(retriedNormal->height, 360u);
    EXPECT_EQ(retriedMaterial->width, 640u);
    EXPECT_EQ(retriedMaterial->height, 360u);
    EXPECT_EQ(retriedDepth->width, 640u);
    EXPECT_EQ(retriedDepth->height, 360u);
}

TEST(DeferredSceneRendererMaterialCacheTests, ReusesGBufferMaterialForStableMaterialAssetPath)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* shader = CreateTestShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material firstMaterial(shader);
    NLS::Render::Resources::Material secondMaterial(shader);
    firstMaterial.path = "App/Assets/Test/SharedDeferredMaterial.nmat";
    secondMaterial.path = "App/Assets/Test/SharedDeferredMaterial.nmat";

    SyncOneDeferredCacheMaterial(renderer, firstMaterial);
    SyncOneDeferredCacheMaterial(renderer, secondMaterial);

    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).size(), 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(DeferredSceneRendererMaterialCacheTests, StableMaterialAssetPathDoesNotShareRuntimeGBufferMaterialAcrossDistinctSourceInstances)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* shader = CreateTestShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    auto* firstTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(255, 0, 0, 255);
    auto* secondTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(0, 255, 0, 255);
    ASSERT_NE(firstTexture, nullptr);
    ASSERT_NE(secondTexture, nullptr);

    NLS::Render::Resources::Material firstMaterial(shader);
    NLS::Render::Resources::Material secondMaterial(shader);
    firstMaterial.path = "App/Assets/Test/SharedDeferredMaterial.nmat";
    secondMaterial.path = "App/Assets/Test/SharedDeferredMaterial.nmat";
    firstMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", firstTexture);
    secondMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", secondTexture);

    auto& firstGBufferMaterial = SyncOneDeferredCacheMaterial(renderer, firstMaterial);
    auto& secondGBufferMaterial = SyncOneDeferredCacheMaterial(renderer, secondMaterial);
    const auto& cache = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer);

    const auto* firstAlbedoMap = firstGBufferMaterial.GetParameterBlock().TryGet("u_AlbedoMap");
    const auto* secondAlbedoMap = secondGBufferMaterial.GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(firstAlbedoMap, nullptr);
    ASSERT_NE(secondAlbedoMap, nullptr);
    ASSERT_EQ(firstAlbedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    ASSERT_EQ(secondAlbedoMap->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*firstAlbedoMap), firstTexture);
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*secondAlbedoMap), secondTexture);
    EXPECT_NE(&firstGBufferMaterial, &secondGBufferMaterial);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(firstTexture));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(secondTexture));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(DeferredSceneRendererMaterialCacheTests, ProvidesVisibleDeferredGBufferFallbackInputsForLambertMaterials)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* lambertShader = CreateTestShader("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, gbufferShader);

    auto* diffuseTexture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(128, 64, 32, 255);
    ASSERT_NE(diffuseTexture, nullptr);

    NLS::Render::Resources::Material source(lambertShader);
    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.25f, 0.5f, 0.75f, 1.0f });
    source.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", diffuseTexture);

    SyncOneDeferredCacheMaterial(renderer, source);

    const auto& gbufferCache = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer);
    ASSERT_EQ(gbufferCache.size(), 1u);
    const auto& gbuffer = *gbufferCache.begin()->second.material;

    const auto* albedoValue = gbuffer.GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.25f);
    EXPECT_FLOAT_EQ(albedo.y, 0.5f);
    EXPECT_FLOAT_EQ(albedo.z, 0.75f);
    EXPECT_FLOAT_EQ(albedo.w, 1.0f);

    const auto* albedoMapValue = gbuffer.GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(albedoMapValue, nullptr);
    ASSERT_EQ(albedoMapValue->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*albedoMapValue), diffuseTexture);

    for (const char* textureName : {
        "u_MetallicMap",
        "u_RoughnessMap",
        "u_AmbientOcclusionMap",
        "u_NormalMap",
        "u_OpacityMap",
        "u_EmissiveMap",
        "u_SpecularMap"
    })
    {
        const auto* textureValue = gbuffer.GetParameterBlock().TryGet(textureName);
        ASSERT_NE(textureValue, nullptr) << textureName;
        ASSERT_EQ(textureValue->type(), typeid(NLS::Render::Resources::Texture2D*)) << textureName;
    }

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(diffuseTexture));
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}

TEST(DeferredSceneRendererMaterialCacheTests, SkipsGBufferMaterialSyncUntilSourceMaterialRevisionChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* lambertShader = CreateTestShader("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, gbufferShader);

    NLS::Render::Resources::Material source(lambertShader);
    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.15f, 0.25f, 0.35f, 1.0f });

    SyncOneDeferredCacheMaterial(renderer, source);
    ASSERT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).size(), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 1u);

    SyncOneDeferredCacheMaterial(renderer, source);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 0u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 1u);

    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.8f, 0.7f, 0.6f, 1.0f });
    SyncOneDeferredCacheMaterial(renderer, source);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 2u);

    const auto& gbuffer = *NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.material;
    const auto* albedoValue = gbuffer.GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.8f);
    EXPECT_FLOAT_EQ(albedo.y, 0.7f);
    EXPECT_FLOAT_EQ(albedo.z, 0.6f);
    EXPECT_FLOAT_EQ(albedo.w, 1.0f);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}

TEST(DeferredSceneRendererMaterialCacheTests, ReusesFrameLocalGBufferMaterialResolveForRepeatedSourceMaterial)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* lambertShader = CreateTestShader("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, gbufferShader);

    NLS::Render::Resources::Material source(lambertShader);
    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.15f, 0.25f, 0.35f, 1.0f });

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ClearFrameGBufferMaterialResolveCache(renderer);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResetFrameGBufferMaterialSyncCount(renderer);

    auto& firstResolve = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
        renderer,
        source);
    auto& secondResolve = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
        renderer,
        source);

    EXPECT_EQ(&secondResolve, &firstResolve);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).size(), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveCacheSize(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 1u);

    NLS::Render::RHI::SamplerDesc samplerOverride;
    samplerOverride.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
    samplerOverride.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
    source.SetSamplerOverride("u_MaterialSampler", samplerOverride);
    auto& bindingChangedResolve = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
        renderer,
        source);

    EXPECT_EQ(&bindingChangedResolve, &firstResolve);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 2u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 2u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 2u);

#if defined(NLS_ENABLE_TEST_HOOKS)
    lambertShader->SetReflectionForTesting(lambertShader->GetReflection());
    auto& shaderChangedResolve = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
        renderer,
        source);

    EXPECT_EQ(&shaderChangedResolve, &firstResolve);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 3u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 3u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 3u);
#endif

    source.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.8f, 0.7f, 0.6f, 1.0f });
    auto& changedResolve = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
        renderer,
        source);

    EXPECT_EQ(&changedResolve, &firstResolve);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveHitCount(renderer), 1u);
#if defined(NLS_ENABLE_TEST_HOOKS)
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 4u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 4u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 4u);
#else
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 3u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 3u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 3u);
#endif

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}

TEST(DeferredSceneRendererMaterialCacheTests, ResyncsSharedRuntimeVariantWhenSourceMaterialIdentityChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* lambertShader = CreateTestShader("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(lambertShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, gbufferShader);

    NLS::Render::Resources::Material firstSource(lambertShader);
    NLS::Render::Resources::Material secondSource(lambertShader);
    firstSource.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.1f, 0.2f, 0.3f, 1.0f });
    secondSource.Set<NLS::Maths::Vector4>("u_Diffuse", { 0.7f, 0.6f, 0.5f, 1.0f });

    SyncOneDeferredCacheMaterial(renderer, firstSource);

    ASSERT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).size(), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 1u);

    SyncOneDeferredCacheMaterial(renderer, secondSource);

    ASSERT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).size(), 2u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 1u);

    SyncOneDeferredCacheMaterial(renderer, secondSource);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 0u);
    uint64_t totalSyncCount = 0u;
    const NLS::Render::Resources::Material* secondGBufferMaterial = nullptr;
    for (const auto& [_, entry] : NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer))
    {
        totalSyncCount += entry.syncCount;
        if (entry.syncedStamp.sourceMaterialInstanceId == secondSource.GetInstanceId())
            secondGBufferMaterial = entry.material.get();
    }
    EXPECT_EQ(totalSyncCount, 2u);

    ASSERT_NE(secondGBufferMaterial, nullptr);
    const auto& gbuffer = *secondGBufferMaterial;
    const auto* albedoValue = gbuffer.GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.7f);
    EXPECT_FLOAT_EQ(albedo.y, 0.6f);
    EXPECT_FLOAT_EQ(albedo.z, 0.5f);
    EXPECT_FLOAT_EQ(albedo.w, 1.0f);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}
