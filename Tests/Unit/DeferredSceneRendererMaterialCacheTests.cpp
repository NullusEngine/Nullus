#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <tuple>
#include <vector>

#include "Rendering/DeferredSceneRenderer.h"

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Data/SceneOcclusionPacketLayout.h"
#include "Rendering/SceneOcclusion.h"
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

    class DeferredTestBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        DeferredTestBuffer(
            NLS::Render::RHI::RHIBufferDesc desc,
            NLS::Render::RHI::RHIBufferUploadDesc uploadDesc)
            : m_desc(std::move(desc))
            , m_uploadDesc(std::move(uploadDesc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }
        const NLS::Render::RHI::RHIBufferUploadDesc& GetInitialUploadDesc() const { return m_uploadDesc; }

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
        NLS::Render::RHI::RHIBufferUploadDesc m_uploadDesc {};
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

    class DeferredTestCompletionToken final : public NLS::Render::RHI::RHICompletionToken
    {
    public:
        explicit DeferredTestCompletionToken(NLS::Render::RHI::RHIBufferReadbackDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return "DeferredSceneRendererTestsCompletionToken"; }

        NLS::Render::RHI::RHICompletionStatus Poll() override
        {
            ++pollCount;
            if (status.code == NLS::Render::RHI::RHICompletionStatusCode::Pending)
                return status;
            CompleteDestination();
            return status;
        }

        NLS::Render::RHI::RHICompletionStatus Wait(uint64_t = 0u) override
        {
            ++waitCount;
            CompleteDestination();
            return status;
        }

        void CompleteWith(std::vector<uint32_t> flags)
        {
            payload = std::move(flags);
            status = { NLS::Render::RHI::RHICompletionStatusCode::Success, {} };
        }

        int pollCount = 0;
        int waitCount = 0;

    private:
        void CompleteDestination()
        {
            if (copied ||
                status.code != NLS::Render::RHI::RHICompletionStatusCode::Success ||
                m_desc.data == nullptr ||
                payload.empty())
            {
                return;
            }
            const auto byteCount = (std::min<uint64_t>)(
                m_desc.size,
                static_cast<uint64_t>(payload.size()) * sizeof(uint32_t));
            std::memcpy(m_desc.data, payload.data(), static_cast<size_t>(byteCount));
            copied = true;
        }

        NLS::Render::RHI::RHIBufferReadbackDesc m_desc;
        NLS::Render::RHI::RHICompletionStatus status{};
        std::vector<uint32_t> payload;
        bool copied = false;
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
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Compute, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ExplicitBarriers, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::HierarchicalZBuffer, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ConservativeOcclusion, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::AsyncReadback, true);
            NLS::Render::RHI::TextureFormatCapability depthCapability;
            depthCapability.sampled = true;
            m_capabilities.SetTextureFormatCapability(
                NLS::Render::RHI::TextureFormat::Depth32F,
                depthCapability);
            m_capabilities.SetTextureFormatCapability(
                NLS::Render::RHI::TextureFormat::Depth24Stencil8,
                depthCapability);
            NLS::Render::RHI::TextureFormatCapability hzbCapability;
            hzbCapability.sampled = true;
            hzbCapability.storage = true;
            m_capabilities.SetTextureFormatCapability(
                NLS::Render::RHI::TextureFormat::R32F,
                hzbCapability);
            m_capabilities.SynchronizeLegacyFields();
        }

        std::string_view GetDebugName() const override { return "DeferredSceneRendererTestsDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        void SetHZBOcclusionSupportedForTesting(const bool supported)
        {
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::HierarchicalZBuffer, supported);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ConservativeOcclusion, supported);
            m_capabilities.SynchronizeLegacyFields();
        }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
        {
            bufferDescs.push_back(desc);
            bufferUploadDebugNames.push_back(uploadDesc.debugName);
            return std::make_shared<DeferredTestBuffer>(desc, uploadDesc);
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
        NLS::Render::RHI::RHIReadbackResult BeginReadBuffer(
            const NLS::Render::RHI::RHIBufferReadbackDesc& desc) override
        {
            ++beginBufferReadbackCalls;
            lastBufferReadbackDebugName = desc.debugName;
            lastBufferReadbackSize = desc.size;
            lastBufferReadbackSource = desc.source;
            lastBufferReadbackSourceState = desc.sourceState;
            if (nextBufferReadbackCode != NLS::Render::RHI::RHIReadbackStatusCode::Success)
            {
                return {
                    nextBufferReadbackCode,
                    nextBufferReadbackMessage,
                    nullptr
                };
            }
            lastReadbackToken = std::make_shared<DeferredTestCompletionToken>(desc);
            return {
                NLS::Render::RHI::RHIReadbackStatusCode::Success,
                {},
                lastReadbackToken
            };
        }

        size_t textureCreateCalls = 0u;
        size_t failTextureCreateCall = 0u;
        size_t beginBufferReadbackCalls = 0u;
        uint64_t lastBufferReadbackSize = 0u;
        std::string lastBufferReadbackDebugName;
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> lastBufferReadbackSource;
        NLS::Render::RHI::ResourceState lastBufferReadbackSourceState =
            NLS::Render::RHI::ResourceState::Unknown;
        std::shared_ptr<DeferredTestCompletionToken> lastReadbackToken;
        NLS::Render::RHI::RHIReadbackStatusCode nextBufferReadbackCode =
            NLS::Render::RHI::RHIReadbackStatusCode::Success;
        std::string nextBufferReadbackMessage;
        std::vector<NLS::Render::RHI::RHIBufferDesc> bufferDescs;
        std::vector<std::string> bufferUploadDebugNames;

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

TEST(DeferredSceneRendererMaterialCacheTests, ReusesHZBFramePersistentDescriptorsUntilSourceTexturesChange)
{
    const auto header = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.h");
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    EXPECT_NE(header.find("m_hzbPreparedDepthTexture"), std::string::npos);
    EXPECT_NE(header.find("m_hzbPreparedHZBTexture"), std::string::npos);
    EXPECT_EQ(header.find("m_hzbPreparedOcclusionOutputTexture"), std::string::npos);
    EXPECT_NE(header.find("m_hzbPreparedOcclusionPrimitiveInputBuffer"), std::string::npos);
    EXPECT_NE(header.find("m_hzbPreparedOcclusionPrimitiveResultBuffer"), std::string::npos);

    const auto prepareStart = source.find("bool DeferredSceneRenderer::PrepareHZBFrameResources");
    const auto buildRequestStart = source.find("NLS::Render::FrameGraph::HZBFrameResourceRequest", prepareStart);
    ASSERT_NE(prepareStart, std::string::npos);
    ASSERT_NE(buildRequestStart, std::string::npos);
    const auto prepareBody = source.substr(prepareStart, buildRequestStart - prepareStart);

    const auto cacheCheck = prepareBody.find("const bool hzbBindingCacheValid");
    const auto textureViewCreate = prepareBody.find("m_hzbDepthReadView = device->CreateTextureView");
    ASSERT_NE(cacheCheck, std::string::npos);
    ASSERT_NE(textureViewCreate, std::string::npos);
    EXPECT_LT(cacheCheck, textureViewCreate);
    EXPECT_NE(prepareBody.find("m_hzbPreparedDepthTexture == depthTexture", cacheCheck), std::string::npos);
    EXPECT_NE(prepareBody.find("m_hzbPreparedHZBTexture == m_hzbTexture", cacheCheck), std::string::npos);
    EXPECT_EQ(prepareBody.find("m_hzbPreparedOcclusionOutputTexture"), std::string::npos);
    EXPECT_EQ(prepareBody.find("m_hzbOcclusionOutputTexture"), std::string::npos);
    EXPECT_NE(prepareBody.find("m_hzbPreparedOcclusionPrimitiveInputBuffer == m_hzbOcclusionPrimitiveInputBuffer", cacheCheck), std::string::npos);
    EXPECT_NE(prepareBody.find("m_hzbPreparedOcclusionPrimitiveResultBuffer == m_hzbOcclusionPrimitiveResultBuffer", cacheCheck), std::string::npos);
    EXPECT_NE(prepareBody.find("if (hzbBindingCacheValid)"), std::string::npos);

    EXPECT_NE(source.find("m_hzbPreparedDepthTexture.reset();"), std::string::npos);
    EXPECT_NE(source.find("m_hzbPreparedHZBTexture.reset();"), std::string::npos);
    EXPECT_EQ(source.find("m_hzbPreparedOcclusionOutputTexture.reset();"), std::string::npos);
    EXPECT_NE(source.find("m_hzbPreparedOcclusionPrimitiveInputBuffer.reset();"), std::string::npos);
    EXPECT_NE(source.find("m_hzbPreparedOcclusionPrimitiveResultBuffer.reset();"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBOcclusionBindsPrimitiveInputAndResultBuffers)
{
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto ensureStart = source.find("bool DeferredSceneRenderer::EnsureHZBTargets");
    const auto prepareStart = source.find("bool DeferredSceneRenderer::PrepareHZBFrameResources");
    const auto ensureEnd = source.find("bool DeferredSceneRenderer::EnsureHZBPipelines", ensureStart);
    const auto buildRequestStart = source.find("NLS::Render::FrameGraph::HZBFrameResourceRequest", prepareStart);
    ASSERT_NE(ensureStart, std::string::npos);
    ASSERT_NE(ensureEnd, std::string::npos);
    ASSERT_NE(prepareStart, std::string::npos);
    ASSERT_NE(buildRequestStart, std::string::npos);
    const auto ensureBody = source.substr(ensureStart, ensureEnd - ensureStart);
    const auto prepareBody = source.substr(prepareStart, buildRequestStart - prepareStart);

    EXPECT_NE(ensureBody.find("SceneHZBOcclusionPrimitiveInputs"), std::string::npos);
    EXPECT_NE(ensureBody.find("SceneHZBOcclusionPrimitiveResults"), std::string::npos);
    EXPECT_NE(ensureBody.find("SceneHZBOcclusionPrimitiveInputsInitialUpload"), std::string::npos);
    EXPECT_NE(ensureBody.find("SceneHZBOcclusionPrimitiveResultsInitialUpload"), std::string::npos);
    EXPECT_NE(ensureBody.find("NLS::Render::RHI::BufferUsageFlags::ShaderRead"), std::string::npos);
    EXPECT_NE(ensureBody.find("NLS::Render::RHI::BufferUsageFlags::Storage"), std::string::npos);
    EXPECT_NE(prepareBody.find("ShaderParameterBindingValue::StructuredBuffer"), std::string::npos);
    EXPECT_NE(prepareBody.find("\"u_OcclusionPrimitiveInputs\""), std::string::npos);
    EXPECT_NE(prepareBody.find("ShaderParameterBindingValue::StorageBuffer"), std::string::npos);
    EXPECT_NE(prepareBody.find("\"u_OcclusionPrimitiveResults\""), std::string::npos);
    EXPECT_EQ(prepareBody.find("\"u_OcclusionOutput\""), std::string::npos);

    const auto requestStart = source.find("NLS::Render::FrameGraph::HZBFrameResourceRequest DeferredSceneRenderer::BuildHZBFrameResourceRequest");
    ASSERT_NE(requestStart, std::string::npos);
    const auto requestBody = source.substr(requestStart);
    EXPECT_NE(requestBody.find("request.occlusionPrimitiveInputBuffer = m_hzbOcclusionPrimitiveInputBuffer;"), std::string::npos);
    EXPECT_NE(requestBody.find("request.occlusionPrimitiveResultBuffer = m_hzbOcclusionPrimitiveResultBuffer;"), std::string::npos);
    EXPECT_EQ(requestBody.find("request.occlusionOutputTexture"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBFramePreparationLogsDiagnosticEarlyExitReasons)
{
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto prepareStart = source.find("bool DeferredSceneRenderer::PrepareHZBFrameResources");
    const auto buildRequestStart = source.find("NLS::Render::FrameGraph::HZBFrameResourceRequest", prepareStart);
    ASSERT_NE(prepareStart, std::string::npos);
    ASSERT_NE(buildRequestStart, std::string::npos);
    const auto prepareBody = source.substr(prepareStart, buildRequestStart - prepareStart);

    EXPECT_NE(source.find("[DeferredSceneRenderer][HZB] Prepare skipped: "), std::string::npos);
    EXPECT_NE(source.find("ToHZBFallbackReasonName"), std::string::npos);
    EXPECT_NE(prepareBody.find("support.fallbackReason"), std::string::npos);
    EXPECT_NE(prepareBody.find("support.diagnosticReason"), std::string::npos);
    EXPECT_NE(prepareBody.find("capability gate rejected HZB occlusion"), std::string::npos);
    EXPECT_NE(prepareBody.find("deferred prepared resources missing GBuffer depth"), std::string::npos);
    EXPECT_NE(prepareBody.find("GBuffer depth has no explicit RHI texture"), std::string::npos);
    EXPECT_NE(prepareBody.find("GBuffer depth texture is not sampleable"), std::string::npos);
    EXPECT_NE(prepareBody.find("target, pipeline, or shader setup failed"), std::string::npos);
    EXPECT_NE(prepareBody.find("depth SRV creation failed"), std::string::npos);
    EXPECT_NE(prepareBody.find("binding set creation failed"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBCapabilityGateUsesActualDeferredDepthTextureFormat)
{
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto prepareStart = source.find("bool DeferredSceneRenderer::PrepareHZBFrameResources");
    const auto buildRequestStart = source.find("NLS::Render::FrameGraph::HZBFrameResourceRequest", prepareStart);
    ASSERT_NE(prepareStart, std::string::npos);
    ASSERT_NE(buildRequestStart, std::string::npos);
    const auto prepareBody = source.substr(prepareStart, buildRequestStart - prepareStart);

    const auto depthDesc = prepareBody.find("const auto& depthDesc = depthTexture->GetDesc();");
    const auto capabilityRequest = prepareBody.find("SceneOcclusionCapabilityRequest capabilityRequest");
    const auto assignActualDepthFormat = prepareBody.find("capabilityRequest.opaqueDepthFormat = depthDesc.format");
    const auto resolveCapabilities = prepareBody.find("SceneOcclusionSystem::ResolveCapabilities(*device, capabilityRequest)");
    ASSERT_NE(depthDesc, std::string::npos);
    ASSERT_NE(capabilityRequest, std::string::npos);
    ASSERT_NE(assignActualDepthFormat, std::string::npos);
    ASSERT_NE(resolveCapabilities, std::string::npos);
    EXPECT_LT(depthDesc, capabilityRequest);
    EXPECT_LT(capabilityRequest, assignActualDepthFormat);
    EXPECT_LT(assignActualDepthFormat, resolveCapabilities);
    EXPECT_EQ(prepareBody.find("const SceneOcclusionCapabilityRequest capabilityRequest"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBFrameRequestCarriesRuntimePrimitiveOcclusionBuffers)
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

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureHZBTargets(renderer, 320u, 180u));
    const auto request = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildHZBFrameResourceRequest(renderer);

    ASSERT_NE(request.occlusionPrimitiveInputBuffer, nullptr);
    ASSERT_NE(request.occlusionPrimitiveResultBuffer, nullptr);
    EXPECT_EQ(request.occlusionPrimitiveInputBuffer->GetDesc().debugName, "SceneHZBOcclusionPrimitiveInputs");
    EXPECT_EQ(
        request.occlusionPrimitiveInputBuffer->GetDesc().size,
        NLS::Render::Data::kSceneOcclusionPrimitivePacketStride);
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(
        request.occlusionPrimitiveInputBuffer->GetDesc().usage,
        NLS::Render::RHI::BufferUsageFlags::ShaderRead));
    EXPECT_EQ(request.occlusionPrimitiveResultBuffer->GetDesc().debugName, "SceneHZBOcclusionPrimitiveResults");
    EXPECT_EQ(request.occlusionPrimitiveResultBuffer->GetDesc().size, sizeof(uint32_t));
    EXPECT_TRUE(NLS::Render::RHI::HasBufferUsage(
        request.occlusionPrimitiveResultBuffer->GetDesc().usage,
        NLS::Render::RHI::BufferUsageFlags::Storage));

    EXPECT_NE(std::find(
        explicitDevice->bufferUploadDebugNames.begin(),
        explicitDevice->bufferUploadDebugNames.end(),
        "SceneHZBOcclusionPrimitiveInputsInitialUpload"), explicitDevice->bufferUploadDebugNames.end());
    EXPECT_NE(std::find(
        explicitDevice->bufferUploadDebugNames.begin(),
        explicitDevice->bufferUploadDebugNames.end(),
        "SceneHZBOcclusionPrimitiveResultsInitialUpload"), explicitDevice->bufferUploadDebugNames.end());
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBFrameRequestStaysDisabledWhenCapabilitiesAreGated)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<DeferredTestExplicitDevice>();
    explicitDevice->SetHZBOcclusionSupportedForTesting(false);
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureHZBTargets(renderer, 320u, 180u));
    const auto request = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildHZBFrameResourceRequest(renderer);

    EXPECT_FALSE(request.opaqueDepthEligible);
    EXPECT_EQ(request.hzbBuildPipeline, nullptr);
    EXPECT_EQ(request.occlusionPipeline, nullptr);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBPrimitiveOcclusionBuffersResizeToUploadedPacketCount)
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

    std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> packets(3u);
    packets[0].screenMinX = 0.10f;
    packets[1].screenMinX = 0.20f;
    packets[2].screenMinX = 0.30f;

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureHZBTargets(renderer, 320u, 180u));
    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        packets));

    const auto request = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildHZBFrameResourceRequest(renderer);
    ASSERT_NE(request.occlusionPrimitiveInputBuffer, nullptr);
    ASSERT_NE(request.occlusionPrimitiveResultBuffer, nullptr);
    EXPECT_EQ(
        request.occlusionPrimitiveInputBuffer->GetDesc().size,
        packets.size() * sizeof(NLS::Engine::Rendering::SceneOcclusionPrimitivePacket));
    EXPECT_EQ(request.occlusionPrimitiveResultBuffer->GetDesc().size, packets.size() * sizeof(uint32_t));

    auto inputBuffer = std::dynamic_pointer_cast<DeferredTestBuffer>(request.occlusionPrimitiveInputBuffer);
    auto resultBuffer = std::dynamic_pointer_cast<DeferredTestBuffer>(request.occlusionPrimitiveResultBuffer);
    ASSERT_NE(inputBuffer, nullptr);
    ASSERT_NE(resultBuffer, nullptr);
    EXPECT_EQ(inputBuffer->GetInitialUploadDesc().debugName, "SceneHZBOcclusionPrimitiveInputsFrameUpload");
    EXPECT_EQ(
        inputBuffer->GetInitialUploadDesc().dataSize,
        packets.size() * sizeof(NLS::Engine::Rendering::SceneOcclusionPrimitivePacket));
    EXPECT_EQ(inputBuffer->GetInitialUploadDesc().data, packets.data());
    EXPECT_EQ(resultBuffer->GetInitialUploadDesc().debugName, "SceneHZBOcclusionPrimitiveResultsFrameClear");
    EXPECT_EQ(resultBuffer->GetInitialUploadDesc().dataSize, packets.size() * sizeof(uint32_t));
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBPrimitiveOcclusionBuffersRefreshWhenSameCountPacketsChange)
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

    std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> firstPackets(2u);
    firstPackets[0].nearestDepth = 0.75f;
    firstPackets[1].nearestDepth = 0.50f;
    std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> secondPackets(2u);
    secondPackets[0].nearestDepth = 0.25f;
    secondPackets[1].nearestDepth = 0.10f;

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureHZBTargets(renderer, 320u, 180u));
    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        firstPackets));
    const auto firstRequest =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildHZBFrameResourceRequest(renderer);
    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        secondPackets));
    const auto secondRequest =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildHZBFrameResourceRequest(renderer);

    EXPECT_NE(firstRequest.occlusionPrimitiveInputBuffer, secondRequest.occlusionPrimitiveInputBuffer);
    auto inputBuffer = std::dynamic_pointer_cast<DeferredTestBuffer>(secondRequest.occlusionPrimitiveInputBuffer);
    ASSERT_NE(inputBuffer, nullptr);
    EXPECT_EQ(inputBuffer->GetInitialUploadDesc().data, secondPackets.data());
    EXPECT_EQ(
        inputBuffer->GetInitialUploadDesc().dataSize,
        secondPackets.size() * sizeof(NLS::Engine::Rendering::SceneOcclusionPrimitivePacket));
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBDispatchGroupsSeparateTextureBuildFromPrimitiveOcclusionWork)
{
    const auto header = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.h");
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    EXPECT_NE(header.find("m_hzbBuildDispatchGroups"), std::string::npos);
    EXPECT_NE(header.find("m_hzbOcclusionDispatchGroups"), std::string::npos);
    EXPECT_EQ(header.find("m_hzbDispatchGroups"), std::string::npos);

    const auto prepareStart = source.find("bool DeferredSceneRenderer::PrepareHZBFrameResources");
    const auto requestStart = source.find("NLS::Render::FrameGraph::HZBFrameResourceRequest", prepareStart);
    ASSERT_NE(prepareStart, std::string::npos);
    ASSERT_NE(requestStart, std::string::npos);
    const auto prepareBody = source.substr(prepareStart, requestStart - prepareStart);

    EXPECT_NE(prepareBody.find("m_hzbBuildDispatchGroups"), std::string::npos);
    EXPECT_NE(prepareBody.find("m_hzbOcclusionDispatchGroups"), std::string::npos);
    EXPECT_NE(prepareBody.find("(std::max<uint32_t>(depthDesc.extent.width, 1u) + 7u) / 8u"), std::string::npos);
    EXPECT_NE(prepareBody.find("(std::max<uint32_t>(m_hzbOcclusionPrimitiveCount, 1u) + 7u) / 8u"), std::string::npos);
    EXPECT_EQ(prepareBody.find("m_hzbBuildDispatchGroups[0] = (std::max)"), std::string::npos);

    const auto buildRequestStart = source.find(
        "NLS::Render::FrameGraph::HZBFrameResourceRequest DeferredSceneRenderer::BuildHZBFrameResourceRequest",
        requestStart);
    const auto createMaterialStart = source.find(
        "std::unique_ptr<NLS::Render::Resources::Material> DeferredSceneRenderer::CreateGBufferMaterial",
        buildRequestStart);
    ASSERT_NE(buildRequestStart, std::string::npos);
    ASSERT_NE(createMaterialStart, std::string::npos);
    const auto requestBody = source.substr(buildRequestStart, createMaterialStart - buildRequestStart);

    EXPECT_NE(requestBody.find("request.hzbBuildGroupCounts = m_hzbBuildDispatchGroups"), std::string::npos);
    EXPECT_NE(requestBody.find("request.occlusionGroupCounts = m_hzbOcclusionDispatchGroups"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, BeginFrameUploadsParsedHZBPrimitivePacketsBeforePreparingResources)
{
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto parseScene = source.find("auto drawables = ParseScene();");
    const auto threadedPrepare = source.find("PrepareHZBFrameResources(BuildDeferredPreparedSceneResourceRequest())", parseScene);
    ASSERT_NE(parseScene, std::string::npos);
    ASSERT_NE(threadedPrepare, std::string::npos);

    const auto beginFrameBody = source.substr(parseScene, threadedPrepare - parseScene);
    EXPECT_NE(beginFrameBody.find("PrepareHZBOcclusionPrimitiveBuffers("), std::string::npos);
    EXPECT_NE(beginFrameBody.find("const auto& hzbPacketBuild = GetLastHZBOcclusionPrimitivePacketBuildResult()"), std::string::npos);
    EXPECT_NE(beginFrameBody.find("hzbPacketBuild.primitivePackets"), std::string::npos);
    EXPECT_NE(beginFrameBody.find("hzbOcclusionFrameInput.enabled"), std::string::npos);
    EXPECT_NE(beginFrameBody.find("hzbOcclusionFrameInput.backendSupported"), std::string::npos);
    EXPECT_NE(beginFrameBody.find("hzbOcclusionFrameInput.historyTextureValid"), std::string::npos);
    EXPECT_NE(beginFrameBody.find("BeginHZBOcclusionObservationFrame("), std::string::npos);
    EXPECT_NE(beginFrameBody.find("hzbPacketBuild.primitiveInputs"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, ValidationDisableHZBOcclusionSkipsPrepareAndPacketUploads)
{
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto beginFrameStart = source.find("void DeferredSceneRenderer::BeginFrame");
    const auto drawFrameStart = source.find("void DeferredSceneRenderer::DrawFrame", beginFrameStart);
    ASSERT_NE(beginFrameStart, std::string::npos);
    ASSERT_NE(drawFrameStart, std::string::npos);
    const auto beginFrameBody = source.substr(beginFrameStart, drawFrameStart - beginFrameStart);

    const auto prepareStart = source.find("bool DeferredSceneRenderer::PrepareHZBFrameResources");
    const auto requestStart = source.find("NLS::Render::FrameGraph::HZBFrameResourceRequest", prepareStart);
    ASSERT_NE(prepareStart, std::string::npos);
    ASSERT_NE(requestStart, std::string::npos);
    const auto prepareBody = source.substr(prepareStart, requestStart - prepareStart);

    EXPECT_NE(beginFrameBody.find("hzbOcclusionFrameInput.enabled &&"), std::string::npos);
    EXPECT_LT(
        beginFrameBody.find("hzbOcclusionFrameInput.enabled &&"),
        beginFrameBody.find("PrepareHZBOcclusionPrimitiveBuffers("));
    EXPECT_NE(prepareBody.find("editorValidationDisableHZBOcclusion"), std::string::npos);
    EXPECT_NE(prepareBody.find("disabled by editor validation override"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBOcclusionDescriptorRangesCoverPreparedPrimitiveBuffers)
{
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto prepareStart = source.find("bool DeferredSceneRenderer::PrepareHZBFrameResources");
    const auto requestStart = source.find("NLS::Render::FrameGraph::HZBFrameResourceRequest", prepareStart);
    ASSERT_NE(prepareStart, std::string::npos);
    ASSERT_NE(requestStart, std::string::npos);
    const auto prepareBody = source.substr(prepareStart, requestStart - prepareStart);

    EXPECT_NE(
        prepareBody.find("m_hzbOcclusionPrimitiveInputBuffer->GetDesc().size"),
        std::string::npos);
    EXPECT_NE(
        prepareBody.find("m_hzbOcclusionPrimitiveResultBuffer->GetDesc().size"),
        std::string::npos);
    EXPECT_EQ(
        prepareBody.find("m_hzbOcclusionPrimitiveInputBuffer,\n\t\t\t\t\tNLS::Render::Data::kSceneOcclusionPrimitivePacketStride"),
        std::string::npos);
    EXPECT_EQ(
        prepareBody.find("m_hzbOcclusionPrimitiveResultBuffer,\n\t\t\t\t\tsizeof(uint32_t)"),
        std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBPrimitiveResultClearUploadAllocatesOneUintPerPacket)
{
    const auto source = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    const auto prepareStart = source.find("bool DeferredSceneRenderer::PrepareHZBOcclusionPrimitiveBuffers");
    const auto pipelineStart = source.find("bool DeferredSceneRenderer::EnsureHZBPipelines", prepareStart);
    ASSERT_NE(prepareStart, std::string::npos);
    ASSERT_NE(pipelineStart, std::string::npos);
    const auto prepareBody = source.substr(prepareStart, pipelineStart - prepareStart);

    EXPECT_NE(prepareBody.find("std::vector<uint32_t> primitiveResultClear(packetCount"), std::string::npos);
    EXPECT_NE(prepareBody.find("primitiveResultClear.data()"), std::string::npos);
    EXPECT_EQ(prepareBody.find("const uint32_t emptyResult = 0u"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, ReadyHZBOcclusionObservationBatchPublishesRendererHistory)
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

    NLS::Engine::Rendering::SceneOcclusionFrameInput frame;
    frame.enabled = true;
    frame.backendSupported = true;
    frame.historyTextureValid = true;
    frame.frameSerial = 42u;
    frame.viewKey = 7u;
    frame.viewCompatibilityHash = 8u;
    frame.projectionHash = 9u;
    frame.jitterHash = 10u;
    frame.depthFormatKey = 11u;
    frame.viewportWidth = 320u;
    frame.viewportHeight = 180u;

    std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitiveInput> primitives(2u);
    primitives[0].handle = { 1u, 3u, 5u };
    primitives[0].boundsGeneration = 101u;
    primitives[0].transformGeneration = 102u;
    primitives[0].representationId = 103u;
    primitives[0].depthWriteEligibilityGeneration = 104u;
    primitives[0].depthWriteEligible = true;
    primitives[1].handle = { 1u, 4u, 5u };
    primitives[1].boundsGeneration = 201u;
    primitives[1].transformGeneration = 202u;
    primitives[1].representationId = 203u;
    primitives[1].depthWriteEligibilityGeneration = 204u;
    primitives[1].depthWriteEligible = true;

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BeginHZBOcclusionObservationFrame(
        renderer,
        frame,
        primitives);
    const std::array<uint32_t, 2u> resultFlags{ 0u, 1u };
    const auto stats = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::CompleteHZBOcclusionObservationFrame(
        renderer,
        resultFlags);

    EXPECT_EQ(stats.observedPrimitiveCount, 2u);
    EXPECT_EQ(stats.visiblePrimitiveCount, 1u);
    EXPECT_EQ(stats.occludedPrimitiveCount, 1u);
    EXPECT_FALSE(stats.usedSynchronousReadback);
    EXPECT_FALSE(stats.waitedForGpuFence);
    EXPECT_FALSE(stats.blockedOnReadbackMap);
    EXPECT_FALSE(stats.requestedCurrentFrameReadback);

    const auto result = NLS::Engine::Rendering::SceneOcclusionSystem::Evaluate(
        frame,
        primitives,
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetHZBOcclusionHistory(renderer));
    ASSERT_EQ(result.primitiveResults.size(), 2u);
    EXPECT_FALSE(result.primitiveResults[0].culledByOcclusion);
    EXPECT_TRUE(result.primitiveResults[1].culledByOcclusion);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBResultReadbackPollPublishesRendererHistoryWithoutWait)
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

    NLS::Engine::Rendering::SceneOcclusionFrameInput frame;
    frame.enabled = true;
    frame.backendSupported = true;
    frame.historyTextureValid = true;
    frame.frameSerial = 77u;
    frame.viewKey = 12u;
    frame.viewCompatibilityHash = 13u;
    frame.projectionHash = 14u;
    frame.jitterHash = 15u;
    frame.depthFormatKey = 16u;
    frame.viewportWidth = 640u;
    frame.viewportHeight = 360u;

    std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitiveInput> primitives(2u);
    primitives[0].handle = { 1u, 10u, 2u };
    primitives[0].boundsGeneration = 1u;
    primitives[0].transformGeneration = 2u;
    primitives[0].representationId = 3u;
    primitives[0].depthWriteEligibilityGeneration = 4u;
    primitives[0].depthWriteEligible = true;
    primitives[1].handle = { 1u, 11u, 2u };
    primitives[1].boundsGeneration = 5u;
    primitives[1].transformGeneration = 6u;
    primitives[1].representationId = 7u;
    primitives[1].depthWriteEligibilityGeneration = 8u;
    primitives[1].depthWriteEligible = true;

    const std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> packets(2u);
    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        packets));
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BeginHZBOcclusionObservationFrame(
        renderer,
        frame,
        primitives);

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BeginHZBOcclusionResultReadback(renderer));
    EXPECT_EQ(explicitDevice->beginBufferReadbackCalls, 1u);
    EXPECT_EQ(explicitDevice->lastBufferReadbackDebugName, "SceneHZBOcclusionPrimitiveResultsReadback");
    EXPECT_EQ(explicitDevice->lastBufferReadbackSize, sizeof(uint32_t) * 2u);
    EXPECT_EQ(explicitDevice->lastBufferReadbackSourceState, NLS::Render::RHI::ResourceState::ShaderWrite);
    ASSERT_NE(explicitDevice->lastReadbackToken, nullptr);

    EXPECT_FALSE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PollHZBOcclusionResultReadback(renderer));
    EXPECT_EQ(explicitDevice->lastReadbackToken->pollCount, 1);
    EXPECT_EQ(explicitDevice->lastReadbackToken->waitCount, 0);

    auto pendingResult = NLS::Engine::Rendering::SceneOcclusionSystem::Evaluate(
        frame,
        primitives,
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetHZBOcclusionHistory(renderer));
    ASSERT_EQ(pendingResult.primitiveResults.size(), 2u);
    EXPECT_FALSE(pendingResult.primitiveResults[0].culledByOcclusion);
    EXPECT_FALSE(pendingResult.primitiveResults[1].culledByOcclusion);

    explicitDevice->lastReadbackToken->CompleteWith({ 0u, 1u });
    EXPECT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PollHZBOcclusionResultReadback(renderer));
    EXPECT_EQ(explicitDevice->lastReadbackToken->waitCount, 0);

    const auto readyResult = NLS::Engine::Rendering::SceneOcclusionSystem::Evaluate(
        frame,
        primitives,
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetHZBOcclusionHistory(renderer));
    ASSERT_EQ(readyResult.primitiveResults.size(), 2u);
    EXPECT_FALSE(readyResult.primitiveResults[0].culledByOcclusion);
    EXPECT_TRUE(readyResult.primitiveResults[1].culledByOcclusion);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBDirectReadbackDeviceLostMarksDriverTelemetry)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<DeferredTestExplicitDevice>();
    explicitDevice->nextBufferReadbackCode = NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost;
    explicitDevice->nextBufferReadbackMessage = "device removed during direct HZB readback";
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    NLS::Engine::Rendering::SceneOcclusionFrameInput frame;
    frame.enabled = true;
    frame.backendSupported = true;
    frame.historyTextureValid = true;
    frame.frameSerial = 88u;
    frame.viewKey = 12u;
    frame.viewCompatibilityHash = 13u;
    frame.projectionHash = 14u;
    frame.depthFormatKey = 16u;
    frame.viewportWidth = 640u;
    frame.viewportHeight = 360u;

    std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitiveInput> primitives(1u);
    primitives[0].handle = { 1u, 10u, 2u };
    primitives[0].depthWriteEligible = true;

    const std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> packets(1u);
    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        packets));
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BeginHZBOcclusionObservationFrame(
        renderer,
        frame,
        primitives);

    EXPECT_FALSE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BeginHZBOcclusionResultReadback(renderer));
    EXPECT_EQ(explicitDevice->beginBufferReadbackCalls, 1u);

    const auto telemetry = NLS::Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(driver);
    EXPECT_TRUE(telemetry.deviceLostDetected);
    EXPECT_NE(telemetry.deviceLostReason.find("device removed during direct HZB readback"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, ThreadedPreparedHZBRequestsPostSubmitResultReadback)
{
    const auto lifecycleHeader = ReadRepoFile("Runtime/Rendering/Context/ThreadedRenderingLifecycle.h");
    const auto coordinatorSource = ReadRepoFile("Runtime/Rendering/Context/RhiThreadCoordinator.cpp");
    const auto driverAccessHeader = ReadRepoFile("Runtime/Rendering/Context/DriverAccess.h");
    const auto deferredSource = ReadRepoFile("Runtime/Engine/Rendering/DeferredSceneRenderer.cpp");

    EXPECT_NE(lifecycleHeader.find("PostSubmitBufferReadbackRequest"), std::string::npos);
    EXPECT_NE(lifecycleHeader.find("postSubmitBufferReadbacks"), std::string::npos);
    EXPECT_NE(coordinatorSource.find("BeginPostSubmitBufferReadbacks("), std::string::npos);
    EXPECT_NE(coordinatorSource.find("impl.explicitDevice->BeginReadBuffer(readbackDesc)"), std::string::npos);
    EXPECT_NE(coordinatorSource.find("MarkReadbackDeviceLostIfNeeded("), std::string::npos);
    EXPECT_NE(coordinatorSource.find("waitForLastComputeQueueCompletion"), std::string::npos);
    EXPECT_NE(coordinatorSource.find("readbackDesc.waitSemaphores.push_back"), std::string::npos);
    EXPECT_NE(driverAccessHeader.find("QueueStandalonePostSubmitBufferReadback"), std::string::npos);
    EXPECT_NE(deferredSource.find("BuildHZBPostSubmitReadbackRequest("), std::string::npos);
    EXPECT_NE(deferredSource.find("BuildHZBPostSubmitReadbackRequest(true)"), std::string::npos);
    EXPECT_NE(deferredSource.find("BuildHZBPostSubmitReadbackRequest(false)"), std::string::npos);
    EXPECT_NE(deferredSource.find("request.waitForLastComputeQueueCompletion = waitForLastComputeQueueCompletion"), std::string::npos);
    EXPECT_NE(deferredSource.find("package.postSubmitBufferReadbacks.push_back"), std::string::npos);

    const auto drawFrameStart = deferredSource.find("void DeferredSceneRenderer::DrawFrame()");
    const auto loadResourcesStart = deferredSource.find("void DeferredSceneRenderer::LoadPipelineResources()", drawFrameStart);
    ASSERT_NE(drawFrameStart, std::string::npos);
    ASSERT_NE(loadResourcesStart, std::string::npos);
    const auto drawFrameBody = deferredSource.substr(drawFrameStart, loadResourcesStart - drawFrameStart);
    EXPECT_NE(drawFrameBody.find("QueueStandalonePostSubmitBufferReadback"), std::string::npos);
    EXPECT_EQ(drawFrameBody.find("BeginHZBOcclusionResultReadback()"), std::string::npos);
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
