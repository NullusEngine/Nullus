#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

#include "Rendering/DeferredSceneRenderer.h"

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"
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
        explicit DeferredTestBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }
        uint64_t GetGPUAddress() const override { return 0u; }
        NLS::Render::RHI::RHIUpdateResult UpdateData(const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
        {
            ++updateCalls;
            lastUpdateSize = uploadDesc.dataSize;
            return { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };
        }

        uint32_t updateCalls = 0u;
        size_t lastUpdateSize = 0u;

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc {};
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
            m_capabilities.supportsCompute = true;
            m_capabilities.supportsExplicitBarriers = true;
            m_capabilities.supportsOffscreenFramebuffers = true;
            m_capabilities.supportsMultiRenderTargets = true;
            m_capabilities.supportsHierarchicalZBuffer = true;
            m_capabilities.supportsConservativeOcclusion = true;
            m_capabilities.supportsAsyncReadback = true;
            NLS::Render::RHI::TextureFormatCapability depthCapability;
            depthCapability.format = NLS::Render::FrameGraph::kDeferredGBufferDepthFormat;
            depthCapability.sampled = true;
            m_capabilities.SetTextureFormatCapability(depthCapability.format, depthCapability);
            NLS::Render::RHI::TextureFormatCapability hzbCapability;
            hzbCapability.format = NLS::Render::RHI::TextureFormat::R32F;
            hzbCapability.sampled = true;
            hzbCapability.storage = true;
            m_capabilities.SetTextureFormatCapability(hzbCapability.format, hzbCapability);
        }

        std::string_view GetDebugName() const override { return "DeferredSceneRendererTestsDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc& desc,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            ++bufferCreateCalls;
            bufferDescs.push_back(desc);
            auto buffer = std::make_shared<DeferredTestBuffer>(desc);
            buffers.push_back(buffer);
            return buffer;
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
            textureViewDescs.push_back(desc);
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
        size_t bufferCreateCalls = 0u;
        std::vector<NLS::Render::RHI::RHIBufferDesc> bufferDescs;
        std::vector<std::shared_ptr<DeferredTestBuffer>> buffers;
        std::vector<NLS::Render::RHI::RHITextureViewDesc> textureViewDescs;

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
            96u,
            {
                {"u_Diffuse", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u},
                {"u_DiffuseMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, 16u, 0u, 1u},
                {"u_Albedo", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 32u, 16u, 1u},
                {"u_AlbedoMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, 48u, 0u, 1u},
                {"u_MetallicMapChannel", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 64u, 16u, 1u},
                {"u_RoughnessMapChannel", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 80u, 16u, 1u}
            }
        });
        for (const auto& [name, type, kind, offset, size] : {
            std::tuple{"u_Diffuse", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 0u, 16u},
            std::tuple{"u_DiffuseMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 16u, 0u},
            std::tuple{"u_Albedo", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 32u, 16u},
            std::tuple{"u_AlbedoMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 48u, 0u},
            std::tuple{"u_MetallicMapChannel", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 64u, 16u},
            std::tuple{"u_RoughnessMapChannel", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 80u, 16u}
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

    NLS::Render::Resources::ShaderReflection MakeShaderLabStandardPbrShaderReflection()
    {
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.constantBuffers.push_back({
            "MaterialProperties",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            64u,
            {
                {"_BaseColor", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u},
                {"_Metallic", NLS::Render::Resources::UniformType::UNIFORM_FLOAT, 16u, 4u, 1u},
                {"_Roughness", NLS::Render::Resources::UniformType::UNIFORM_FLOAT, 20u, 4u, 1u},
                {"_AmbientOcclusion", NLS::Render::Resources::UniformType::UNIFORM_FLOAT, 24u, 4u, 1u},
                {"_MetallicMapChannel", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 32u, 16u, 1u},
                {"_RoughnessMapChannel", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 48u, 16u, 1u}
            }
        });
        for (const auto& [name, type, kind, offset, size, cbuffer] : {
            std::tuple{"_BaseColor", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 0u, 16u, "MaterialProperties"},
            std::tuple{"_Metallic", NLS::Render::Resources::UniformType::UNIFORM_FLOAT, NLS::Render::Resources::ShaderResourceKind::Value, 16u, 4u, "MaterialProperties"},
            std::tuple{"_Roughness", NLS::Render::Resources::UniformType::UNIFORM_FLOAT, NLS::Render::Resources::ShaderResourceKind::Value, 20u, 4u, "MaterialProperties"},
            std::tuple{"_AmbientOcclusion", NLS::Render::Resources::UniformType::UNIFORM_FLOAT, NLS::Render::Resources::ShaderResourceKind::Value, 24u, 4u, "MaterialProperties"},
            std::tuple{"_MetallicMapChannel", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 32u, 16u, "MaterialProperties"},
            std::tuple{"_RoughnessMapChannel", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, NLS::Render::Resources::ShaderResourceKind::Value, 48u, 16u, "MaterialProperties"},
            std::tuple{"_BaseMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 0u, 0u, ""},
            std::tuple{"_NormalMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 1u, 0u, ""},
            std::tuple{"_MetallicMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 2u, 0u, ""},
            std::tuple{"_RoughnessMap", NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D, NLS::Render::Resources::ShaderResourceKind::SampledTexture, 3u, 0u, ""}
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
                cbuffer
            });
        }
        return reflection;
    }

    NLS::Render::Resources::Shader* CreateTestShader(
        const std::string& sourcePath,
        NLS::Render::Resources::ShaderReflection reflection = MakeDeferredMaterialShaderReflection())
    {
        NLS::Render::Assets::ShaderArtifact artifact;
        artifact.sourcePath = sourcePath;
        artifact.subAssetKey = "shader:test";
        artifact.reflection = std::move(reflection);
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
                "test.shader"
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
                "test.shader"
            }
        });

        const auto root = std::filesystem::temp_directory_path() /
            ("nullus_deferred_shader_" + NLS::Guid::New().ToString());
        const auto path = root / "2d3bb8c6242a8a5cb5d8892ddf9bb18b5a7344f3f8a3791f946d7c217337b5e1";
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

    NLS::Engine::Rendering::SceneOcclusionFrameInput MakeDeferredHZBFrameInput()
    {
        NLS::Engine::Rendering::SceneOcclusionFrameInput input;
        input.enabled = true;
        input.backendSupported = true;
        input.historyTextureValid = true;
        input.frameSerial = 32u;
        input.maxHistoryAge = 2u;
        input.viewKey = 7u;
        input.viewCompatibilityHash = 0x100u;
        input.projectionHash = 0x200u;
        input.jitterHash = 0u;
        input.depthFormatKey = 24u;
        input.viewportWidth = 1280u;
        input.viewportHeight = 720u;
        return input;
    }

    NLS::Engine::Rendering::SceneOcclusionPrimitiveInput MakeDeferredHZBPrimitiveInput(
        const uint32_t index)
    {
        NLS::Engine::Rendering::SceneOcclusionPrimitiveInput input;
        input.handle = { 1u, index, 1u };
        input.boundsGeneration = 10u + index;
        input.transformGeneration = 20u + index;
        input.representationId = 30u + index;
        input.depthWriteEligibilityGeneration = 40u + index;
        input.depthWriteEligible = true;
        return input;
    }

    NLS::Engine::Rendering::SceneOcclusionPrimitivePacket MakeDeferredHZBPrimitivePacket(
        const float offset)
    {
        NLS::Engine::Rendering::SceneOcclusionPrimitivePacket packet;
        packet.screenMinX = 10.0f + offset;
        packet.screenMinY = 20.0f + offset;
        packet.screenMaxX = 30.0f + offset;
        packet.screenMaxY = 40.0f + offset;
        packet.nearestDepth = 0.5f;
        packet.flags = 1u;
        return packet;
    }

    size_t CountDeferredHZBPrimitiveBuffersCreated(
        const DeferredTestExplicitDevice& device)
    {
        return static_cast<size_t>(std::count_if(
            device.bufferDescs.begin(),
            device.bufferDescs.end(),
            [](const NLS::Render::RHI::RHIBufferDesc& desc)
            {
                return desc.debugName == "SceneHZBOcclusionPrimitiveInputs" ||
                    desc.debugName == "SceneHZBOcclusionPrimitiveResults";
            }));
    }

    std::shared_ptr<DeferredTestBuffer> FindLastDeferredTestBufferByDebugName(
        const DeferredTestExplicitDevice& device,
        const std::string_view debugName)
    {
        for (auto it = device.buffers.rbegin(); it != device.buffers.rend(); ++it)
        {
            if ((*it)->GetDesc().debugName == debugName)
                return *it;
        }
        return nullptr;
    }
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBOcclusionObservationCanBeDiscardedAndReusedAfterReadbackFailure)
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

    const auto frame = MakeDeferredHZBFrameInput();
    const std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitiveInput> primitiveInputs {
        MakeDeferredHZBPrimitiveInput(0u),
        MakeDeferredHZBPrimitiveInput(1u)
    };

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BeginHZBOcclusionObservationFrame(
        renderer,
        frame,
        primitiveInputs);
    EXPECT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::HasPendingHZBOcclusionObservationFrame(renderer));

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::DiscardPendingHZBOcclusionObservationFrame(renderer);
    EXPECT_FALSE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::HasPendingHZBOcclusionObservationFrame(renderer));

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BeginHZBOcclusionObservationFrame(
        renderer,
        frame,
        primitiveInputs);
    const std::vector<uint32_t> flags { 0u, 1u };
    const auto stats = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::CompleteHZBOcclusionObservationFrame(
        renderer,
        flags);

    EXPECT_EQ(stats.observedPrimitiveCount, 2u);
    EXPECT_EQ(stats.occludedPrimitiveCount, 1u);
    EXPECT_FALSE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::HasPendingHZBOcclusionObservationFrame(renderer));

    const auto result = NLS::Engine::Rendering::SceneOcclusionSystem::Evaluate(
        frame,
        primitiveInputs,
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetHZBOcclusionHistory(renderer));
    ASSERT_EQ(result.primitiveResults.size(), 2u);
    EXPECT_FALSE(result.primitiveResults[0].culledByOcclusion);
    EXPECT_TRUE(result.primitiveResults[1].culledByOcclusion);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBOcclusionPrimitiveBuffersReuseSameSizeAcrossCameraMotion)
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

    const std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> firstPackets {
        MakeDeferredHZBPrimitivePacket(0.0f),
        MakeDeferredHZBPrimitivePacket(1.0f)
    };
    const std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> movedCameraPackets {
        MakeDeferredHZBPrimitivePacket(100.0f),
        MakeDeferredHZBPrimitivePacket(101.0f)
    };

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        firstPackets));
    const auto hzbBufferCreateCallsAfterFirstPrepare =
        CountDeferredHZBPrimitiveBuffersCreated(*explicitDevice);
    ASSERT_EQ(hzbBufferCreateCallsAfterFirstPrepare, 2u);
    auto firstInputBuffer = FindLastDeferredTestBufferByDebugName(
        *explicitDevice,
        "SceneHZBOcclusionPrimitiveInputs");
    auto firstResultBuffer = FindLastDeferredTestBufferByDebugName(
        *explicitDevice,
        "SceneHZBOcclusionPrimitiveResults");
    ASSERT_NE(firstInputBuffer, nullptr);
    ASSERT_NE(firstResultBuffer, nullptr);

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        movedCameraPackets));

    EXPECT_EQ(
        CountDeferredHZBPrimitiveBuffersCreated(*explicitDevice),
        hzbBufferCreateCallsAfterFirstPrepare);
    EXPECT_EQ(firstInputBuffer->GetDesc().memoryUsage, NLS::Render::RHI::MemoryUsage::CPUToGPU);
    EXPECT_EQ(firstInputBuffer->updateCalls, 1u);
    EXPECT_EQ(firstInputBuffer->lastUpdateSize, firstPackets.size() * sizeof(NLS::Engine::Rendering::SceneOcclusionPrimitivePacket));
    EXPECT_EQ(firstResultBuffer->updateCalls, 0u);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBOcclusionPrimitiveBuffersReuseCapacityAcrossVisibleCountChanges)
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

    const std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> firstPackets {
        MakeDeferredHZBPrimitivePacket(0.0f),
        MakeDeferredHZBPrimitivePacket(1.0f)
    };
    const std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitivePacket> fewerPackets {
        MakeDeferredHZBPrimitivePacket(100.0f)
    };

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        firstPackets));
    const auto hzbBufferCreateCallsAfterFirstPrepare =
        CountDeferredHZBPrimitiveBuffersCreated(*explicitDevice);
    ASSERT_EQ(hzbBufferCreateCallsAfterFirstPrepare, 2u);
    auto firstInputBuffer = FindLastDeferredTestBufferByDebugName(
        *explicitDevice,
        "SceneHZBOcclusionPrimitiveInputs");
    auto firstResultBuffer = FindLastDeferredTestBufferByDebugName(
        *explicitDevice,
        "SceneHZBOcclusionPrimitiveResults");
    ASSERT_NE(firstInputBuffer, nullptr);
    ASSERT_NE(firstResultBuffer, nullptr);

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
        renderer,
        fewerPackets));

    EXPECT_EQ(
        CountDeferredHZBPrimitiveBuffersCreated(*explicitDevice),
        hzbBufferCreateCallsAfterFirstPrepare);
    EXPECT_EQ(firstInputBuffer->updateCalls, 1u);
    EXPECT_EQ(firstInputBuffer->lastUpdateSize, fewerPackets.size() * sizeof(NLS::Engine::Rendering::SceneOcclusionPrimitivePacket));
    EXPECT_EQ(firstResultBuffer->updateCalls, 0u);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBPostSubmitReadbackRequestConstructionDoesNotAdoptPendingState)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    ASSERT_FALSE(source.empty());
    const auto buildRequest = source.find("DeferredSceneRenderer::BuildHZBPostSubmitReadbackRequest");
    const auto adoptRequest = source.find("DeferredSceneRenderer::AdoptHZBPostSubmitReadbackRequest");
    const auto clearRequest = source.find("DeferredSceneRenderer::ClearHZBPendingResultReadback");
    ASSERT_NE(buildRequest, std::string::npos);
    ASSERT_NE(adoptRequest, std::string::npos);
    ASSERT_NE(clearRequest, std::string::npos);

    const auto buildBody = source.substr(buildRequest, adoptRequest - buildRequest);
    EXPECT_NE(buildBody.find("auto readbackFlags = std::make_shared<std::vector<uint32_t>>"), std::string::npos);
    EXPECT_NE(buildBody.find("auto readbackState = std::make_shared<NLS::Render::Context::PostSubmitBufferReadbackState>"), std::string::npos);
    EXPECT_NE(buildBody.find("m_hzbOcclusionResultReadbackState != nullptr"), std::string::npos);
    EXPECT_EQ(buildBody.find("m_hzbOcclusionResultReadbackState = request.state"), std::string::npos);
    EXPECT_EQ(buildBody.find("m_hzbOcclusionResultReadbackFlags ="), std::string::npos);

    const auto adoptBody = source.substr(adoptRequest, clearRequest - adoptRequest);
    EXPECT_NE(adoptBody.find("m_hzbOcclusionResultReadbackState = request.state"), std::string::npos);
    EXPECT_NE(adoptBody.find("m_hzbOcclusionResultReadbackFlags ="), std::string::npos);

    const auto clearBody = source.substr(clearRequest);
    EXPECT_NE(clearBody.find("m_hzbOcclusionResultReadbackState.reset()"), std::string::npos);
    EXPECT_NE(clearBody.find("m_hzbOcclusionResultReadbackFlags.reset()"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBThreadedPreparedBuilderDoesNotMutateRendererReadbackStateOnRenderThread)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const auto builderStart = source.find("DeferredSceneRenderer::BuildDeferredPreparedRenderSceneBuilder");
    const auto nextFunction = source.find("DeferredSceneRenderer::BuildPreparedRenderSceneBuilder", builderStart);
    ASSERT_NE(builderStart, std::string::npos);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto builderBody = source.substr(builderStart, nextFunction - builderStart);

    EXPECT_EQ(builderBody.find("return [this"), std::string::npos);
    EXPECT_EQ(builderBody.find("AdoptHZBPostSubmitReadbackRequest"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, DebugSceneRendererForwardsThreadedHZBReadbackIntoDeferredPreparedBuilder)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Rendering/DebugSceneRenderer.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const auto builderStart = source.find("DebugSceneRenderer::BuildPreparedRenderSceneBuilder");
    const auto builderCall = source.find("BuildDeferredPreparedRenderSceneBuilder", builderStart);
    ASSERT_NE(builderStart, std::string::npos);
    ASSERT_NE(builderCall, std::string::npos);
    const auto builderBody = source.substr(builderStart, builderCall + 512u - builderStart);

    EXPECT_NE(builderBody.find("GetThreadedHZBPostSubmitReadbackForPreparedBuilder()"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBPollWaitsForExplicitReadbackBeginAckOrNack)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const auto pollStart = source.find("DeferredSceneRenderer::PollHZBOcclusionResultReadback");
    const auto beginStart = source.find("DeferredSceneRenderer::BeginHZBOcclusionResultReadback", pollStart);
    ASSERT_NE(pollStart, std::string::npos);
    ASSERT_NE(beginStart, std::string::npos);
    const auto pollBody = source.substr(pollStart, beginStart - pollStart);

    EXPECT_NE(pollBody.find("if (!readbackState->beginAttempted)"), std::string::npos);
    EXPECT_EQ(pollBody.find("m_hzbOcclusionResultReadbackState.use_count() == 1u"), std::string::npos);
    EXPECT_EQ(pollBody.find("m_hzbOcclusionUnstartedReadbackPollCount"), std::string::npos);
    EXPECT_NE(pollBody.find("DiscardPendingHZBOcclusionObservationFrame()"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBTransientBusyReadbackKeepsObservationForRetry)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("IsTransientHZBReadbackBusyFailure"), std::string::npos);
    EXPECT_NE(source.find("previous async readback has not been completed"), std::string::npos);

    const auto pollStart = source.find("DeferredSceneRenderer::PollHZBOcclusionResultReadback");
    const auto beginStart = source.find("DeferredSceneRenderer::BeginHZBOcclusionResultReadback", pollStart);
    const auto buildStart = source.find("DeferredSceneRenderer::BuildHZBPostSubmitReadbackRequest", beginStart);
    ASSERT_NE(pollStart, std::string::npos);
    ASSERT_NE(beginStart, std::string::npos);
    ASSERT_NE(buildStart, std::string::npos);

    const auto pollBody = source.substr(pollStart, beginStart - pollStart);
    EXPECT_NE(pollBody.find("retryReadback"), std::string::npos);
    EXPECT_NE(pollBody.find("IsTransientHZBReadbackBusyFailure(readbackState->resultMessage)"), std::string::npos);
    EXPECT_NE(pollBody.find("ClearHZBPendingResultReadback(false)"), std::string::npos);

    const auto beginBody = source.substr(beginStart, buildStart - beginStart);
    EXPECT_NE(beginBody.find("IsTransientHZBReadbackBusyFailure(result.message)"), std::string::npos);
    EXPECT_NE(beginBody.find("ClearHZBPendingResultReadback(false)"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBPublishFailureClearsUnpublishedObservationWithoutReadbackState)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const auto publishStart = source.find("DeferredSceneRenderer::TryPublishThreadedFrame");
    const auto failedStart = source.find("DeferredSceneRenderer::OnThreadedFramePublishFailed", publishStart);
    ASSERT_NE(publishStart, std::string::npos);
    ASSERT_NE(failedStart, std::string::npos);
    const auto publishBody = source.substr(publishStart, failedStart - publishStart);

    EXPECT_NE(publishBody.find("if (!published && m_threadedHZBPostSubmitReadback.has_value())"), std::string::npos);
    EXPECT_NE(publishBody.find("DiscardPendingHZBOcclusionObservationFrame()"), std::string::npos);
    EXPECT_NE(publishBody.find("m_threadedHZBPostSubmitReadback.reset()"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBPublishFailureKeepsAdoptedReadbackUntilRhiBeginAck)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    ASSERT_FALSE(source.empty());
    const auto failedStart = source.find("DeferredSceneRenderer::OnThreadedFramePublishFailed");
    const auto nextFunction = source.find("DeferredSceneRenderer::IsThreadedFramePublishSkippedForCurrentFrame", failedStart);
    ASSERT_NE(failedStart, std::string::npos);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto failedBody = source.substr(failedStart, nextFunction - failedStart);

    EXPECT_NE(failedBody.find("if (m_hzbOcclusionResultReadbackState != nullptr)"), std::string::npos);
    EXPECT_NE(failedBody.find("return;"), std::string::npos);
    EXPECT_EQ(failedBody.find("discardReadback = !readbackState->beginAttempted"), std::string::npos);
    EXPECT_EQ(failedBody.find("ClearHZBPendingResultReadback()"), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBSkippedThreadedPreparationClearsObservationWhenNoReadbackWasPublished)
{
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Rendering/DeferredSceneRenderer.cpp";
    std::ifstream input(sourcePath);
    const std::string source{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };

    const auto beginFrameStart = source.find("DeferredSceneRenderer::BeginFrame");
    const auto drawFrameStart = source.find("DeferredSceneRenderer::DrawFrame", beginFrameStart);
    ASSERT_NE(beginFrameStart, std::string::npos);
    ASSERT_NE(drawFrameStart, std::string::npos);
    const auto beginFrameBody = source.substr(beginFrameStart, drawFrameStart - beginFrameStart);

    const auto preparedUnavailable = beginFrameBody.find("preparedFrameResourcesAvailable");
    ASSERT_NE(preparedUnavailable, std::string::npos);
    const auto pipelineUnavailable = beginFrameBody.find("!HasDeferredThreadedPipelineResources()");
    ASSERT_NE(pipelineUnavailable, std::string::npos);
    EXPECT_NE(beginFrameBody.find("DiscardHZBObservationIfNoReadbackWasPublished()", preparedUnavailable), std::string::npos);
    EXPECT_NE(beginFrameBody.find("DiscardHZBObservationIfNoReadbackWasPublished()", pipelineUnavailable), std::string::npos);
}

TEST(DeferredSceneRendererMaterialCacheTests, HZBTargetsAllocateMipChainForHierarchicalOcclusion)
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

    ASSERT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureHZBTargets(renderer, 640u, 360u));
    const auto request = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildHZBFrameResourceRequest(renderer);

    ASSERT_NE(request.hzbTexture, nullptr);
    EXPECT_EQ(request.hzbTexture->GetDesc().mipLevels, 11u);
    EXPECT_EQ(request.hzbMipCount, request.hzbTexture->GetDesc().mipLevels);

    const auto hasMip0UAV = std::any_of(
        explicitDevice->textureViewDescs.begin(),
        explicitDevice->textureViewDescs.end(),
        [](const NLS::Render::RHI::RHITextureViewDesc& desc)
        {
            return desc.debugName == "SceneHZBMip0UAV" &&
                desc.subresourceRange.baseMipLevel == 0u &&
                desc.subresourceRange.mipLevelCount == 1u;
        });
    const auto hasFullSRV = std::any_of(
        explicitDevice->textureViewDescs.begin(),
        explicitDevice->textureViewDescs.end(),
        [](const NLS::Render::RHI::RHITextureViewDesc& desc)
        {
            return desc.debugName == "SceneHZBAllMipsSRV" &&
                desc.subresourceRange.baseMipLevel == 0u &&
                desc.subresourceRange.mipLevelCount == 11u;
        });

    EXPECT_TRUE(hasMip0UAV);
    EXPECT_TRUE(hasFullSRV);
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
    firstMaterial.path = "App/Assets/Test/SharedDeferredMaterial.mat";
    secondMaterial.path = "App/Assets/Test/SharedDeferredMaterial.mat";

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
    firstMaterial.path = "App/Assets/Test/SharedDeferredMaterial.mat";
    secondMaterial.path = "App/Assets/Test/SharedDeferredMaterial.mat";
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
    EXPECT_EQ(gbuffer.ResolveShaderForLightMode("GBuffer"), gbufferShader)
        << "Deferred fallback materials use a built-in HLSL GBuffer shader, but still must resolve "
           "for the GBuffer LightMode draw path.";

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

    const auto* enableNormalMappingValue = gbuffer.GetParameterBlock().TryGet("u_EnableNormalMapping");
    ASSERT_NE(enableNormalMappingValue, nullptr);
    ASSERT_EQ(enableNormalMappingValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*enableNormalMappingValue), 0.0f);

    for (const char* channelName : {"u_MetallicMapChannel", "u_RoughnessMapChannel"})
    {
        const auto* channelValue = gbuffer.GetParameterBlock().TryGet(channelName);
        ASSERT_NE(channelValue, nullptr) << channelName;
        ASSERT_EQ(channelValue->type(), typeid(NLS::Maths::Vector4)) << channelName;
        const auto& channel = std::any_cast<const NLS::Maths::Vector4&>(*channelValue);
        EXPECT_FLOAT_EQ(channel.x, 1.0f) << channelName;
        EXPECT_FLOAT_EQ(channel.y, 0.0f) << channelName;
        EXPECT_FLOAT_EQ(channel.z, 0.0f) << channelName;
        EXPECT_FLOAT_EQ(channel.w, 0.0f) << channelName;
    }

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(diffuseTexture));
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}

TEST(DeferredSceneRendererMaterialCacheTests, MapsShaderLabStandardPBRParametersIntoDeferredGBufferFallback)
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

    auto* standardShader = CreateTestShader(
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        MakeShaderLabStandardPbrShaderReflection());
    ASSERT_NE(standardShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, gbufferShader);

    auto* baseMap = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(96, 128, 160, 255);
    auto* normalMap = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(128, 128, 255, 255);
    auto* metallicMap = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(32, 32, 32, 255);
    auto* roughnessMap = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(192, 192, 192, 255);
    ASSERT_NE(baseMap, nullptr);
    ASSERT_NE(normalMap, nullptr);
    ASSERT_NE(metallicMap, nullptr);
    ASSERT_NE(roughnessMap, nullptr);

    NLS::Render::Resources::Material source(standardShader);
    source.Set<NLS::Maths::Vector4>("_BaseColor", { 0.2f, 0.4f, 0.6f, 0.8f });
    source.Set<NLS::Render::Resources::Texture2D*>("_BaseMap", baseMap);
    source.SetTextureResourcePath("_BaseMap", "Library/Artifacts/ab/abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    source.Set<float>("_Metallic", 0.35f);
    source.Set<float>("_Roughness", 0.65f);
    source.Set<float>("_AmbientOcclusion", 0.75f);
    source.Set<NLS::Maths::Vector4>("_MetallicMapChannel", { 0.0f, 0.0f, 1.0f, 0.0f });
    source.Set<NLS::Maths::Vector4>("_RoughnessMapChannel", { 0.0f, 1.0f, 0.0f, 0.0f });
    source.Set<NLS::Render::Resources::Texture2D*>("_NormalMap", normalMap);
    source.Set<NLS::Render::Resources::Texture2D*>("_MetallicMap", metallicMap);
    source.Set<NLS::Render::Resources::Texture2D*>("_RoughnessMap", roughnessMap);

    SyncOneDeferredCacheMaterial(renderer, source);

    const auto& gbufferCache = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer);
    ASSERT_EQ(gbufferCache.size(), 1u);
    const auto& gbuffer = *gbufferCache.begin()->second.material;

    const auto* albedoValue = gbuffer.GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedoValue, nullptr);
    ASSERT_EQ(albedoValue->type(), typeid(NLS::Maths::Vector4));
    const auto& albedo = std::any_cast<const NLS::Maths::Vector4&>(*albedoValue);
    EXPECT_FLOAT_EQ(albedo.x, 0.2f);
    EXPECT_FLOAT_EQ(albedo.y, 0.4f);
    EXPECT_FLOAT_EQ(albedo.z, 0.6f);
    EXPECT_FLOAT_EQ(albedo.w, 0.8f);

    const auto* albedoMapValue = gbuffer.GetParameterBlock().TryGet("u_AlbedoMap");
    ASSERT_NE(albedoMapValue, nullptr);
    ASSERT_EQ(albedoMapValue->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*albedoMapValue), baseMap);
    EXPECT_EQ(
        gbuffer.GetTextureResourcePath("u_AlbedoMap"),
        "Library/Artifacts/ab/abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");

    const auto* metallicValue = gbuffer.GetParameterBlock().TryGet("u_Metallic");
    ASSERT_NE(metallicValue, nullptr);
    ASSERT_EQ(metallicValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*metallicValue), 0.35f);

    const auto* roughnessValue = gbuffer.GetParameterBlock().TryGet("u_Roughness");
    ASSERT_NE(roughnessValue, nullptr);
    ASSERT_EQ(roughnessValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*roughnessValue), 0.65f);

    const auto* ambientOcclusionValue = gbuffer.GetParameterBlock().TryGet("u_AmbientOcclusion");
    ASSERT_NE(ambientOcclusionValue, nullptr);
    ASSERT_EQ(ambientOcclusionValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*ambientOcclusionValue), 0.75f);

    const auto* normalMapValue = gbuffer.GetParameterBlock().TryGet("u_NormalMap");
    ASSERT_NE(normalMapValue, nullptr);
    ASSERT_EQ(normalMapValue->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*normalMapValue), normalMap);

    const auto* enableNormalMappingValue = gbuffer.GetParameterBlock().TryGet("u_EnableNormalMapping");
    ASSERT_NE(enableNormalMappingValue, nullptr);
    ASSERT_EQ(enableNormalMappingValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*enableNormalMappingValue), 1.0f);

    const auto* metallicMapValue = gbuffer.GetParameterBlock().TryGet("u_MetallicMap");
    ASSERT_NE(metallicMapValue, nullptr);
    ASSERT_EQ(metallicMapValue->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*metallicMapValue), metallicMap);

    const auto* roughnessMapValue = gbuffer.GetParameterBlock().TryGet("u_RoughnessMap");
    ASSERT_NE(roughnessMapValue, nullptr);
    ASSERT_EQ(roughnessMapValue->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*roughnessMapValue), roughnessMap);

    const auto* metallicMapChannelValue = gbuffer.GetParameterBlock().TryGet("u_MetallicMapChannel");
    ASSERT_NE(metallicMapChannelValue, nullptr);
    ASSERT_EQ(metallicMapChannelValue->type(), typeid(NLS::Maths::Vector4));
    const auto& metallicMapChannel = std::any_cast<const NLS::Maths::Vector4&>(*metallicMapChannelValue);
    EXPECT_FLOAT_EQ(metallicMapChannel.x, 0.0f);
    EXPECT_FLOAT_EQ(metallicMapChannel.y, 0.0f);
    EXPECT_FLOAT_EQ(metallicMapChannel.z, 1.0f);
    EXPECT_FLOAT_EQ(metallicMapChannel.w, 0.0f);

    const auto* roughnessMapChannelValue = gbuffer.GetParameterBlock().TryGet("u_RoughnessMapChannel");
    ASSERT_NE(roughnessMapChannelValue, nullptr);
    ASSERT_EQ(roughnessMapChannelValue->type(), typeid(NLS::Maths::Vector4));
    const auto& roughnessMapChannel = std::any_cast<const NLS::Maths::Vector4&>(*roughnessMapChannelValue);
    EXPECT_FLOAT_EQ(roughnessMapChannel.x, 0.0f);
    EXPECT_FLOAT_EQ(roughnessMapChannel.y, 1.0f);
    EXPECT_FLOAT_EQ(roughnessMapChannel.z, 0.0f);
    EXPECT_FLOAT_EQ(roughnessMapChannel.w, 0.0f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(baseMap));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(normalMap));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(metallicMap));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(roughnessMap));
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(standardShader));
}

TEST(DeferredSceneRendererMaterialCacheTests, EnablesDeferredNormalMappingAfterNormalTextureResourceLoads)
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

    auto* standardShader = CreateTestShader(
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        MakeShaderLabStandardPbrShaderReflection());
    ASSERT_NE(standardShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, gbufferShader);

    constexpr const char* normalTexturePath =
        "Library/Artifacts/12/123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    NLS::Render::Resources::Material source(standardShader);
    source.SetTextureResourcePath("_NormalMap", normalTexturePath);

    auto& unloadedGBuffer = SyncOneDeferredCacheMaterial(renderer, source);
    EXPECT_EQ(unloadedGBuffer.GetTextureResourcePath("u_NormalMap"), normalTexturePath);
    const auto* unloadedNormalMapValue = unloadedGBuffer.GetParameterBlock().TryGet("u_NormalMap");
    ASSERT_NE(unloadedNormalMapValue, nullptr);
    ASSERT_EQ(unloadedNormalMapValue->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*unloadedNormalMapValue), nullptr);

    const auto* unloadedEnableValue = unloadedGBuffer.GetParameterBlock().TryGet("u_EnableNormalMapping");
    ASSERT_NE(unloadedEnableValue, nullptr);
    ASSERT_EQ(unloadedEnableValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*unloadedEnableValue), 0.0f);

    auto* normalMap = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(128, 128, 255, 255);
    ASSERT_NE(normalMap, nullptr);
    source.SetRawParameter("_NormalMap", normalMap);

    auto& loadedGBuffer = SyncOneDeferredCacheMaterial(renderer, source);
    const auto* loadedNormalMapValue = loadedGBuffer.GetParameterBlock().TryGet("u_NormalMap");
    ASSERT_NE(loadedNormalMapValue, nullptr);
    ASSERT_EQ(loadedNormalMapValue->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*loadedNormalMapValue), normalMap);

    const auto* loadedEnableValue = loadedGBuffer.GetParameterBlock().TryGet("u_EnableNormalMapping");
    ASSERT_NE(loadedEnableValue, nullptr);
    ASSERT_EQ(loadedEnableValue->type(), typeid(float));
    EXPECT_FLOAT_EQ(std::any_cast<float>(*loadedEnableValue), 1.0f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(normalMap));
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(standardShader));
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

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResetFrameGBufferMaterialResolveStats(renderer);
    auto& nextFrameResolve = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
        renderer,
        source);
    EXPECT_EQ(&nextFrameResolve, &firstResolve);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveCacheSize(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 0u);

    NLS::Render::RHI::SamplerDesc samplerOverride;
    samplerOverride.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
    samplerOverride.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
    source.SetSamplerOverride("u_MaterialSampler", samplerOverride);
    auto& bindingChangedResolve = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
        renderer,
        source);

    EXPECT_EQ(&bindingChangedResolve, &firstResolve);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 2u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 2u);

#if defined(NLS_ENABLE_TEST_HOOKS)
    lambertShader->SetReflectionForTesting(lambertShader->GetReflection());
    auto& shaderChangedResolve = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
        renderer,
        source);

    EXPECT_EQ(&shaderChangedResolve, &firstResolve);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 2u);
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
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 3u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 4u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 4u);
#else
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(renderer), 2u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(renderer), 3u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).begin()->second.syncCount, 3u);
#endif

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(lambertShader));
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(DeferredSceneRendererMaterialCacheTests, GBufferMaterialResolveCacheStaysBoundedAcrossSourceChurn)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* sourceShader = CreateTestShader("App/Assets/Engine/Shaders/Lambert.hlsl");
    ASSERT_NE(sourceShader, nullptr);
    auto* gbufferShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(gbufferShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, gbufferShader);

    const auto maxEntries =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetMaterialResolveCacheMaxEntriesForTesting();
    for (size_t index = 0u; index <= maxEntries; ++index)
    {
        NLS::Render::Resources::Material source(sourceShader);
        (void)NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
            renderer,
            source);
    }

    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveCacheSize(renderer),
        1u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(sourceShader));
}
#endif

TEST(DeferredSceneRendererMaterialCacheTests, GBufferDrawableUsesSourceMaterialWhenShaderLabGBufferPassExists)
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

    auto* fallbackShader = CreateTestShader("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, fallbackShader);

    auto* forwardShader = NLS::Render::Resources::Shader::CreateForTesting("Assets/Shaders/Multi.shader");
    auto* gbufferShader = NLS::Render::Resources::Shader::CreateForTesting("Assets/Shaders/Multi.shader");
    ASSERT_NE(forwardShader, nullptr);
    ASSERT_NE(gbufferShader, nullptr);
    forwardShader->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        NLS::Render::ShaderLab::ShaderLabPassState{});
    gbufferShader->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/GBuffer#1",
        "GBuffer",
        NLS::Render::ShaderLab::ShaderLabPassState{});

    NLS::Render::Resources::Material source(forwardShader);
    source.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    source.RegisterShaderLabPassShader(forwardShader);
    source.RegisterShaderLabPassShader(gbufferShader);

    auto& selected = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveGBufferDrawableMaterialForTesting(
        renderer,
        source);

    EXPECT_EQ(&selected, &source)
        << "Deferred GBuffer rendering must let source ShaderLab materials select their own GBuffer pass.";
    EXPECT_EQ(source.ResolveShaderForLightMode("GBuffer"), gbufferShader);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).size(), 0u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(gbufferShader);
    NLS::Render::Resources::Shader::DestroyForTesting(forwardShader);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(fallbackShader));
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
