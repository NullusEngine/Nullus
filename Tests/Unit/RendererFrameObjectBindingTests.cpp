#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
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
#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Data/ObjectDataLimits.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"
#include "Rendering/ForwardSceneRenderer.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/LargeSceneSettings.h"
#include "Rendering/RenderScene.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/IndexedObjectDataShaderSupport.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"

namespace
{
    struct CopyTrackedDrawableDescriptor
    {
        CopyTrackedDrawableDescriptor() = default;
        explicit CopyTrackedDrawableDescriptor(int descriptorValue)
            : value(descriptorValue)
        {
        }
        CopyTrackedDrawableDescriptor(const CopyTrackedDrawableDescriptor& other)
            : value(other.value)
        {
            ++copyCount;
        }

        CopyTrackedDrawableDescriptor& operator=(const CopyTrackedDrawableDescriptor& other)
        {
            value = other.value;
            ++copyCount;
            return *this;
        }

        int value = 0;
        static inline uint32_t copyCount = 0u;
    };

    static_assert(sizeof(NLS::Render::Data::ObjectDrawConstants) == 16u);
    static_assert(offsetof(NLS::Render::Data::ObjectDrawConstants, objectIndex) == 0u);
    static_assert(offsetof(NLS::Render::Data::ObjectDrawConstants, objectFlags) == 4u);
    static_assert(offsetof(NLS::Render::Data::ObjectDrawConstants, padding0) == 8u);
    static_assert(offsetof(NLS::Render::Data::ObjectDrawConstants, padding1) == 12u);

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

#if defined(NLS_ENABLE_TEST_HOOKS)
    class ScopedObjectDataCountLimitOverride final
    {
    public:
        explicit ScopedObjectDataCountLimitOverride(const uint32_t limit)
            : m_previousLimit(NLS::Render::Data::GetObjectDataCountLimitForTesting())
        {
            NLS::Render::Data::SetObjectDataCountLimitForTesting(limit);
        }

        ~ScopedObjectDataCountLimitOverride()
        {
            NLS::Render::Data::SetObjectDataCountLimitForTesting(m_previousLimit);
        }

        ScopedObjectDataCountLimitOverride(const ScopedObjectDataCountLimitOverride&) = delete;
        ScopedObjectDataCountLimitOverride& operator=(const ScopedObjectDataCountLimitOverride&) = delete;

    private:
        uint32_t m_previousLimit = 0u;
    };
#endif

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
                "test-compute.shader"
            }
        });

        const auto root = std::filesystem::temp_directory_path() /
            ("nullus_compute_shader_" + NLS::Guid::New().ToString());
        const auto path = root / "15debd90c0c3b528ccbf6b82e43e45d4e0e3328e401b9e5c721f4085138711b3";
        std::filesystem::create_directories(path.parent_path());
        const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(path.string());
        std::filesystem::remove_all(root);
        return shader;
    }

    NLS::Render::Resources::ShaderConstantBufferDesc MakeObjectDrawConstantsReflection(
        uint32_t byteSize = sizeof(NLS::Render::Data::ObjectDrawConstants),
        NLS::Render::RHI::ShaderStageMask stageMask = NLS::Render::RHI::ShaderStageMask::Vertex);

    NLS::Render::Resources::Shader* CreateTestGraphicsShader(const std::string& sourcePath)
    {
        NLS::Render::Assets::ShaderArtifact artifact;
        artifact.sourcePath = sourcePath;
        artifact.subAssetKey = "shader:test-graphics";
        artifact.reflection.constantBuffers.push_back({
            "MaterialConstants",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            16u,
            {
                {"u_TestColor", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u}
            }
        });
        artifact.reflection.constantBuffers.push_back(MakeObjectDrawConstantsReflection());
        artifact.reflection.properties.push_back({
            "u_TestColor",
            NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
            NLS::Render::Resources::ShaderResourceKind::Value,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            16u,
            "MaterialConstants"
        });
        artifact.reflection.properties.push_back({
            "u_MaterialSampler",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::Sampler,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            1u,
            -1,
            1,
            0u,
            0u,
            {}
        });
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
                "test-graphics.shader"
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
                "test-graphics.shader"
            }
        });

        const auto root = std::filesystem::temp_directory_path() /
            ("nullus_graphics_shader_" + NLS::Guid::New().ToString());
        const auto path = root / "15debd90c0c3b528ccbf6b82e43e45d4e0e3328e401b9e5c721f4085138711b3";
        std::filesystem::create_directories(path.parent_path());
        const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(path.string());
        std::filesystem::remove_all(root);
        return shader;
    }

    NLS::Render::Resources::Shader* CreateTestShaderLabGraphicsPass(
        const std::string& sourcePath,
        const std::string& subAssetKey,
        const std::string& lightMode)
    {
        auto* shader = CreateTestGraphicsShader(sourcePath);
        if (shader == nullptr)
            return nullptr;

        shader->SetImportedShaderLabPassForTesting(
            sourcePath,
            subAssetKey,
            lightMode,
            NLS::Render::ShaderLab::ShaderLabPassState{});
        return shader;
    }

    NLS::Render::Resources::Shader* CreateReflectionOnlyImportedShader(
        const std::string& sourcePath,
        const NLS::Render::Resources::ShaderReflection& reflection,
        const std::string& subAssetKey)
    {
        NLS::Render::Assets::ShaderArtifact artifact;
        artifact.sourcePath = sourcePath;
        artifact.subAssetKey = subAssetKey;
        artifact.reflection = reflection;

        const auto root = std::filesystem::temp_directory_path() /
            ("nullus_reflection_shader_" + NLS::Guid::New().ToString());
        const auto path = root / "15debd90c0c3b528ccbf6b82e43e45d4e0e3328e401b9e5c721f4085138711b3";
        std::filesystem::create_directories(path.parent_path());
        const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(path.string());
        std::filesystem::remove_all(root);
        return shader;
    }

    NLS::Render::Resources::ShaderConstantBufferDesc MakeObjectDrawConstantsReflection(
        const uint32_t byteSize,
        const NLS::Render::RHI::ShaderStageMask stageMask)
    {
        NLS::Render::Resources::ShaderConstantBufferDesc constants;
        constants.name = "ObjectIndexConstants";
        constants.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
        constants.bindingSpace = NLS::Render::RHI::BindingPointMap::kObjectBindingSpace;
        constants.bindingIndex = 1u;
        constants.byteSize = byteSize;
        constants.stageMask = stageMask;
        if (byteSize == sizeof(NLS::Render::Data::ObjectDrawConstants))
        {
            constants.members = {
                { "u_ObjectIndex", NLS::Render::Resources::UniformType::UNIFORM_INT, 0u, 4u, 1u },
                { "u_ObjectFlags", NLS::Render::Resources::UniformType::UNIFORM_INT, 4u, 4u, 1u },
                { "u_ObjectPadding0", NLS::Render::Resources::UniformType::UNIFORM_INT, 8u, 4u, 1u },
                { "u_ObjectPadding1", NLS::Render::Resources::UniformType::UNIFORM_INT, 12u, 4u, 1u }
            };
        }
        return constants;
    }

    NLS::Render::Resources::ShaderReflection MakeIndexedObjectDataReflection(
        const bool includeObjectConstants = true,
        const uint32_t objectConstantsByteSize = sizeof(NLS::Render::Data::ObjectDrawConstants),
        const NLS::Render::RHI::ShaderStageMask objectConstantsStageMask =
            NLS::Render::RHI::ShaderStageMask::Vertex)
    {
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
        if (includeObjectConstants)
        {
            reflection.constantBuffers.push_back(MakeObjectDrawConstantsReflection(
                objectConstantsByteSize,
                objectConstantsStageMask));
        }
        return reflection;
    }

    NLS::Render::Resources::ShaderReflection MakeFrameConstantOnlyReflection(const std::string& constantBufferName)
    {
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.constantBuffers.push_back({
            constantBufferName,
            NLS::Render::ShaderCompiler::ShaderStage::Vertex,
            NLS::Render::RHI::BindingPointMap::kFrameBindingSpace,
            0u,
            sizeof(NLS::Maths::Matrix4),
            {}
        });
        return reflection;
    }

    NLS::Render::Resources::ShaderReflection MakeMaterialColorReflection(const std::string& propertyName)
    {
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.constantBuffers.push_back({
            propertyName + "Constants",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            sizeof(NLS::Maths::Vector4),
            {
                {propertyName, NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u}
            }
        });
        reflection.properties.push_back({
            propertyName,
            NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
            NLS::Render::Resources::ShaderResourceKind::Value,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            16u,
            propertyName + "Constants"
        });
        return reflection;
    }

    NLS::Render::Resources::ShaderReflection MakeIndexedObjectDataMaterialReflection(
        const std::string& propertyName,
        const uint32_t objectConstantsByteSize)
    {
        auto reflection = MakeIndexedObjectDataReflection(true, objectConstantsByteSize);
        reflection.properties.push_back({
            propertyName,
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::Sampler,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            0u,
            {}
        });
        return reflection;
    }

    NLS::Render::Resources::ShaderReflection MakePassConstantOnlyReflection(const std::string& constantBufferName)
    {
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.constantBuffers.push_back({
            constantBufferName,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kPassBindingSpace,
            0u,
            sizeof(NLS::Maths::Vector4),
            {}
        });
        return reflection;
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
        void PushConstants(
            NLS::Render::RHI::ShaderStageMask stageMask,
            uint32_t,
            uint32_t size,
            const void* data) override
        {
            lastPushConstantStageMask = stageMask;
            lastPushConstantSize = size;
            lastPushConstantBytes.resize(size);
            if (data != nullptr && size != 0u)
            {
                std::memcpy(lastPushConstantBytes.data(), data, size);
                if (size >= sizeof(uint32_t))
                    std::memcpy(&lastObjectIndexPushConstant, data, sizeof(lastObjectIndexPushConstant));
            }
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
        NLS::Render::RHI::ShaderStageMask lastPushConstantStageMask =
            NLS::Render::RHI::ShaderStageMask::None;
        uint32_t lastPushConstantSize = 0u;
        std::vector<uint8_t> lastPushConstantBytes;
    };

    class TestCommandPool final : public NLS::Render::RHI::RHICommandPool
    {
    public:
        explicit TestCommandPool(
            const NLS::Render::RHI::QueueType queueType,
            std::string debugName)
            : m_queueType(queueType)
            , m_debugName(std::move(debugName))
        {
        }

        std::string_view GetDebugName() const override { return m_debugName; }
        NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string = {}) override
        {
            return std::make_shared<TestCommandBuffer>();
        }
        void Reset() override {}

    private:
        NLS::Render::RHI::QueueType m_queueType = NLS::Render::RHI::QueueType::Graphics;
        std::string m_debugName;
    };

    class TestFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        explicit TestFence(std::string debugName)
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

    class TestSemaphore final : public NLS::Render::RHI::RHISemaphore
    {
    public:
        explicit TestSemaphore(std::string debugName)
            : m_debugName(std::move(debugName))
        {
        }

        std::string_view GetDebugName() const override { return m_debugName; }
        bool IsSignaled() const override { return false; }
        void Reset() override {}

    private:
        std::string m_debugName;
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

    class PreparedProbeBindingSet final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        explicit PreparedProbeBindingSet(std::string debugName)
        {
            m_desc.debugName = std::move(debugName);
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc {};
    };

    class PreparedBindingProbeProvider final : public NLS::Render::Core::FrameObjectBindingProvider
    {
    public:
        explicit PreparedBindingProbeProvider(
            NLS::Render::Core::CompositeRenderer& renderer,
            const bool captureFrameSeparately = true)
            : FrameObjectBindingProvider(renderer)
            , m_captureFrameSeparately(captureFrameSeparately)
        {
        }

        uint32_t prepareDrawCount = 0u;
        uint32_t captureFrameBindingSetCount = 0u;
        uint32_t captureBindingSetCount = 0u;
        const NLS::Render::Resources::Material* lastPreparedMaterial = nullptr;

    protected:
        bool OnCaptureFrameBindingSet(
            std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& outBindingSet) override
        {
            if (!m_captureFrameSeparately)
                return false;

            ++captureFrameBindingSetCount;
            outBindingSet = std::make_shared<PreparedProbeBindingSet>(
                "PreparedFrameBindingSet" + std::to_string(captureFrameBindingSetCount));
            return true;
        }

        bool OnPrepareDraw(PipelineState&, const NLS::Render::Entities::Drawable&) override
        {
            ++prepareDrawCount;
            lastPreparedMaterial = GetPreparedMaterial();
            return true;
        }

        bool OnCapturePreparedBindingSets(
            PipelineState&,
            const NLS::Render::Entities::Drawable&,
            PreparedBindingSets& outBindings) override
        {
            ++captureBindingSetCount;
            if (!m_captureFrameSeparately)
            {
                outBindings.frameBindingSet = std::make_shared<PreparedProbeBindingSet>(
                    "LegacyPreparedFrameBindingSet" + std::to_string(captureBindingSetCount));
            }
            outBindings.objectBindingSet = std::make_shared<PreparedProbeBindingSet>(
                "PreparedObjectBindingSet" + std::to_string(captureBindingSetCount));
            return true;
        }

    private:
        bool m_captureFrameSeparately = true;
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

        bool PrepareRecordedDraw(
            PipelineState pipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            std::string_view lightMode,
            PreparedRecordedDraw& outDraw) const override
        {
            (void) lightMode;
            return PrepareRecordedDraw(pipelineState, drawable, outDraw);
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

    class RecordedDrawCacheProbeSceneRenderer final : public NLS::Engine::Rendering::BaseSceneRenderer
    {
    public:
        explicit RecordedDrawCacheProbeSceneRenderer(NLS::Render::Context::Driver& driver)
            : BaseSceneRenderer(driver)
        {
        }

        void BeginFrame(const NLS::Render::Data::FrameDescriptor& frameDescriptor) override
        {
            NLS::Render::Core::CompositeRenderer::BeginFrame(frameDescriptor);
        }

        bool CaptureDrawForTesting(
            const Drawable& drawable,
            NLS::Render::Resources::MaterialPipelineStateOverrides overrides,
            const NLS::Render::Settings::EComparaisonAlgorithm depthCompareOverride)
        {
            PreparedRecordedDraw preparedDraw;
            return CaptureThreadedPreparedDraw(
                drawable,
                std::move(overrides),
                depthCompareOverride,
                "Forward",
                preparedDraw) &&
                QueueThreadedRecordedDraw(preparedDraw);
        }

        bool CaptureDrawForTesting(
            const Drawable& drawable,
            Material& effectiveMaterial,
            NLS::Render::Resources::MaterialPipelineStateOverrides overrides,
            const NLS::Render::Settings::EComparaisonAlgorithm depthCompareOverride)
        {
            PreparedRecordedDraw preparedDraw;
            return CaptureThreadedPreparedDraw(
                drawable,
                effectiveMaterial,
                std::move(overrides),
                depthCompareOverride,
                "Forward",
                preparedDraw) &&
                QueueThreadedRecordedDraw(preparedDraw);
        }

        std::optional<NLS::Render::Context::FrameSnapshot> CaptureSnapshotForTesting() const
        {
            return BuildFrameSnapshot(GetFrameDescriptor());
        }

#if defined(NLS_ENABLE_TEST_HOOKS)
        size_t GetPreparedRecordedDrawStaticBaseCacheSizeForTesting() const
        {
            return NLS::Render::Core::ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheSizeForTesting();
        }

        size_t GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting() const
        {
            return NLS::Render::Core::ABaseRenderer::GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting();
        }

        void ClearPreparedRecordedDrawStaticBaseCacheForTesting() const
        {
            NLS::Render::Core::ABaseRenderer::ClearPreparedRecordedDrawStaticBaseCache();
        }

        static size_t GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting()
        {
            return NLS::Render::Core::ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting();
        }

        static uint64_t GetPreparedRecordedDrawStaticBaseCacheMaxFrameAgeForTesting()
        {
            return NLS::Render::Core::ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxFrameAgeForTesting();
        }

        static size_t GetPreparedRecordedDrawStaticBaseCacheAgeSweepBudgetForTesting()
        {
            return NLS::Render::Core::ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheAgeSweepBudgetForTesting();
        }

        size_t AdvancePreparedRecordedDrawStaticBaseCacheForTesting(const uint64_t frameCount) const
        {
            return NLS::Render::Core::ABaseRenderer::AdvancePreparedRecordedDrawStaticBaseCacheForTesting(frameCount);
        }
#endif
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

    std::vector<NLS::Render::Geometry::Vertex> MakeTriangleVertices()
    {
        std::vector<NLS::Render::Geometry::Vertex> vertices(3u);
        vertices[0].position[0] = 0.0f;
        vertices[1].position[0] = 1.0f;
        vertices[2].position[1] = 1.0f;
        return vertices;
    }

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

        void SubmitWithObjectConstants(
            const NLS::Render::Data::ObjectDrawConstants& objectConstants,
            const bool usesObjectIndex = true) const
        {
            PreparedRecordedDraw draw;
            draw.commandBuffer = commandBuffer;
            draw.mesh = mesh;
            draw.instanceCount = 1u;
            draw.objectConstants = objectConstants;
            draw.usesObjectIndex = usesObjectIndex;
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

        bool PrepareRecordedDraw(
            PipelineState pipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            std::string_view lightMode,
            PreparedRecordedDraw& outDraw) const override
        {
            (void) lightMode;
            return PrepareRecordedDraw(pipelineState, drawable, outDraw);
        }

        void BindPreparedGraphicsPipeline(const PreparedRecordedDraw&) const override
        {
            ++m_commandBuffer->bindGraphicsPipelineCalls;
        }

        void BindPreparedMaterialBindingSet(const PreparedRecordedDraw&) const override {}

    private:
        std::shared_ptr<TestCommandBuffer> m_commandBuffer;
    };

    class PipelineAttachmentOverrideProbeRenderer final : public NLS::Render::Core::CompositeRenderer
    {
    public:
        explicit PipelineAttachmentOverrideProbeRenderer(NLS::Render::Context::Driver& driver)
            : CompositeRenderer(driver)
        {
        }

        bool Prepare(
            NLS::Render::Data::PipelineState pipelineState,
            const NLS::Render::Entities::Drawable& drawable)
        {
            PreparedRecordedDraw draw;
            return PrepareRecordedDraw(pipelineState, drawable, draw);
        }

        bool Prepare(
            NLS::Render::Data::PipelineState pipelineState,
            const NLS::Render::Entities::Drawable& drawable,
            std::string_view lightMode)
        {
            PreparedRecordedDraw draw;
            return PrepareRecordedDraw(pipelineState, drawable, lightMode, draw);
        }
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
            graphicsPipelineDescs.push_back(desc);
            return std::make_shared<TestGraphicsPipeline>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc& desc) override
        {
            ++computePipelineCreateCalls;
            return std::make_shared<TestComputePipeline>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
            NLS::Render::RHI::QueueType queueType,
            std::string debugName = {}) override
        {
            return std::make_shared<TestCommandPool>(queueType, std::move(debugName));
        }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName = {}) override
        {
            return std::make_shared<TestFence>(std::move(debugName));
        }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName = {}) override
        {
            return std::make_shared<TestSemaphore>(std::move(debugName));
        }
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
        std::vector<NLS::Render::RHI::RHIGraphicsPipelineDesc> graphicsPipelineDescs;
        std::vector<std::shared_ptr<TestBuffer>> buffers;
        NLS::Render::RHI::RHIPipelineLayoutDesc lastPipelineLayoutDesc {};
        NLS::Render::RHI::RHIGraphicsPipelineDesc lastGraphicsPipelineDesc {};

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        std::shared_ptr<NLS::Render::RHI::RHIQueue> m_queue;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    void ExpectDeferredDecalPipelineState(const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc)
    {
        constexpr auto rgbMask = NLS::Render::RHI::RHIColorWriteMask::Red |
            NLS::Render::RHI::RHIColorWriteMask::Green |
            NLS::Render::RHI::RHIColorWriteMask::Blue;
        EXPECT_TRUE(desc.blendState.enabled);
        EXPECT_TRUE(desc.blendState.independentBlendEnable);
        ASSERT_EQ(desc.blendState.renderTargets.size(), 3u);
        EXPECT_TRUE(desc.blendState.renderTargets[0].blendEnable);
        EXPECT_EQ(desc.blendState.renderTargets[0].colorWriteMask, rgbMask);
        EXPECT_FALSE(desc.blendState.renderTargets[1].blendEnable);
        EXPECT_EQ(desc.blendState.renderTargets[1].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
        EXPECT_FALSE(desc.blendState.renderTargets[2].blendEnable);
        EXPECT_EQ(desc.blendState.renderTargets[2].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
        EXPECT_FALSE(desc.depthStencilState.depthWrite);
        EXPECT_EQ(desc.depthStencilState.stencilWriteMask, 0u);
    }

    void AddDecalMeshToScene(
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Render::Resources::Mesh& mesh,
        NLS::Render::Resources::Material& material)
    {
        auto& meshActor = scene.CreateGameObject("DecalMeshActor");
        auto* meshFilter = meshActor.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = meshActor.AddComponent<NLS::Engine::Components::MeshRenderer>();
        ASSERT_NE(meshFilter, nullptr);
        ASSERT_NE(meshRenderer, nullptr);
        meshFilter->SetMesh(&mesh);
        meshRenderer->FillWithMaterial(material);
        meshActor.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
    }

    void AddOpaqueMeshToScene(
        NLS::Engine::SceneSystem::Scene& scene,
        NLS::Render::Resources::Mesh& mesh,
        NLS::Render::Resources::Material& material)
    {
        auto& meshActor = scene.CreateGameObject("OpaqueMeshActor");
        auto* meshFilter = meshActor.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = meshActor.AddComponent<NLS::Engine::Components::MeshRenderer>();
        ASSERT_NE(meshFilter, nullptr);
        ASSERT_NE(meshRenderer, nullptr);
        meshFilter->SetMesh(&mesh);
        meshRenderer->FillWithMaterial(material);
        meshActor.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
    }

    NLS::Render::Resources::Shader* CreateMaterialSamplerShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        shader->SetParameterStructsForTesting({});
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
        shader->SetReflectionForTesting(std::move(reflection));
        return shader;
    }

    NLS::Render::Resources::Shader* CreateMaterialTextureShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        shader->SetParameterStructsForTesting({});
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.properties.push_back({
            "u_MaterialTexture",
            NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
            NLS::Render::Resources::ShaderResourceKind::SampledTexture,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            0u,
            {}
        });
        shader->SetReflectionForTesting(std::move(reflection));
        return shader;
    }

    NLS::Render::Resources::Shader* CreateMaterialStructuredBufferShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        shader->SetParameterStructsForTesting({});
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
        shader->SetReflectionForTesting(std::move(reflection));
        return shader;
    }

    NLS::Render::Resources::Shader* CreateMaterialConstantBufferShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        shader->SetParameterStructsForTesting({});
        NLS::Render::Resources::ShaderReflection reflection;
        reflection.constantBuffers.push_back({
            "MaterialConstants",
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            16u,
            {
                {"u_TestColor", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u}
            }
        });
        reflection.properties.push_back({
            "u_TestColor",
            NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
            NLS::Render::Resources::ShaderResourceKind::Value,
            NLS::Render::ShaderCompiler::ShaderStage::Pixel,
            NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace,
            0u,
            -1,
            1,
            0u,
            16u,
            "MaterialConstants"
        });
        shader->SetReflectionForTesting(std::move(reflection));
        return shader;
    }

    NLS::Render::Resources::Shader* CreateConflictingMaterialBindingShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        shader->SetParameterStructsForTesting({});
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
        shader->SetReflectionForTesting(std::move(reflection));
        return shader;
    }

    NLS::Render::Resources::Shader* CreateReflectionOnlyObjectDataShader()
    {
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
        if (shader == nullptr)
            return nullptr;

        shader->SetParameterStructsForTesting({});
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
        reflection.constantBuffers.push_back(MakeObjectDrawConstantsReflection());
        shader->SetReflectionForTesting(std::move(reflection));
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

TEST(RendererFrameObjectBindingTests, ObjectDrawConstantsAndDrawableDescriptorDefaultsMatchShadowAbi)
{
    constexpr uint32_t expectedDefaultFlags =
        NLS::Render::Data::kDrawableObjectFlagReceiveShadows |
        NLS::Render::Data::kDrawableObjectFlagCastShadows;

    const NLS::Render::Data::ObjectDrawConstants constants;
    EXPECT_EQ(constants.objectIndex, NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
    EXPECT_EQ(constants.objectFlags, expectedDefaultFlags);
    EXPECT_EQ(constants.padding0, 0u);
    EXPECT_EQ(constants.padding1, 0u);

    const NLS::Render::Data::DrawableObjectDescriptor descriptor;
    EXPECT_EQ(descriptor.objectIndex, NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
    EXPECT_EQ(descriptor.objectFlags, expectedDefaultFlags);
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

TEST(RendererFrameObjectBindingTests, ProviderCapturesFrameBindingWithoutPreparingDrawable)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    ProviderAwareRenderer renderer(*driver);
    PreparedBindingProbeProvider provider(renderer);
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> frameBindingSet;
    EXPECT_FALSE(provider.CaptureFrameBindingSet(frameBindingSet));
    EXPECT_EQ(frameBindingSet, nullptr);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    ASSERT_TRUE(provider.CaptureFrameBindingSet(frameBindingSet));
    ASSERT_NE(frameBindingSet, nullptr);
    EXPECT_EQ(frameBindingSet->GetDebugName(), "PreparedFrameBindingSet1");
    EXPECT_EQ(provider.captureFrameBindingSetCount, 1u);
    EXPECT_EQ(provider.prepareDrawCount, 0u);
    provider.EndFrame();

    EXPECT_FALSE(provider.CaptureFrameBindingSet(frameBindingSet));
    EXPECT_EQ(frameBindingSet, nullptr);
}

TEST(RendererFrameObjectBindingTests, ObjectCapturePreservesLegacyProviderFrameBinding)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    ProviderAwareRenderer renderer(*driver);
    PreparedBindingProbeProvider provider(renderer, false);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    NLS::Render::Data::PipelineState pso;
    NLS::Render::Entities::Drawable drawable;
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedObjectBindingSet(pso, drawable, bindings));
    ASSERT_NE(bindings.frameBindingSet, nullptr);
    ASSERT_NE(bindings.objectBindingSet, nullptr);
    EXPECT_EQ(bindings.frameBindingSet->GetDebugName(), "LegacyPreparedFrameBindingSet1");
    EXPECT_EQ(bindings.objectBindingSet->GetDebugName(), "PreparedObjectBindingSet1");
    EXPECT_EQ(provider.captureFrameBindingSetCount, 0u);
    EXPECT_EQ(provider.captureBindingSetCount, 1u);

    provider.EndFrame();
}

TEST(RendererFrameObjectBindingTests, ProviderPreparesObjectStateDuringDrawsWithoutFeatures)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(*driver, explicitDevice);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(*driver, 0u);
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();

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

    renderer.BeginFrame(frameDescriptor);

    NLS::Render::Data::PipelineState pipelineState;
    NLS::Render::Entities::Drawable drawable;
    renderer.DrawEntity(pipelineState, drawable);

    EXPECT_TRUE(providerPtr->IsFramePrepared());
    EXPECT_TRUE(providerPtr->IsObjectPrepared());
    EXPECT_EQ(providerPtr->GetPreparedDrawCount(), 1u);
    EXPECT_EQ(events, std::vector<std::string>({ "begin", "before", "prepare" }));

    renderer.EndFrame();
    EXPECT_FALSE(providerPtr->IsFramePrepared());
    EXPECT_FALSE(providerPtr->IsObjectPrepared());
    EXPECT_EQ(events, std::vector<std::string>({ "begin", "before", "prepare", "end" }));
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
    EXPECT_EQ(package.passCommandInputs[1].kind, NLS::Render::Context::RenderPassCommandKind::Skybox);
    EXPECT_EQ(package.passCommandInputs[1].drawCount, 1u);
    EXPECT_EQ(package.passCommandInputs[2].kind, NLS::Render::Context::RenderPassCommandKind::Transparent);
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

TEST(RendererFrameObjectBindingTests, EngineProviderCapturesCurrentFrameBindingWithoutDrawable)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 29u;
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

    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> frameBindingSet;
    ASSERT_TRUE(provider.CaptureFrameBindingSet(frameBindingSet));
    ASSERT_NE(frameBindingSet, nullptr);
    EXPECT_EQ(frameBindingSet->GetDebugName(), "EngineFrameBindingSet");
    EXPECT_EQ(provider.GetPreparedDrawCount(), 0u);

    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
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
    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor {
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex
    };
    descriptor.objectFlags = NLS::Render::Data::kDrawableObjectFlagReceiveShadows;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(descriptor));

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, drawable);

    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindingSets;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindingSets));
    ASSERT_NE(bindingSets.objectBindingSet, nullptr);
    EXPECT_FALSE(bindingSets.usesObjectIndex);
    EXPECT_EQ(bindingSets.objectConstants.objectIndex,
        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
    EXPECT_EQ(bindingSets.objectConstants.objectFlags,
        NLS::Render::Data::kDrawableObjectFlagReceiveShadows);
    EXPECT_EQ(bindingSets.objectConstants.padding0, 0u);
    EXPECT_EQ(bindingSets.objectConstants.padding1, 0u);
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

TEST(RendererFrameObjectBindingTests, EngineProviderCapturesPreparedReceiveShadowConstantsForIndexedDraws)
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
    NLS::Engine::Rendering::EngineDrawableDescriptor firstDescriptor {
        firstMatrix,
        NLS::Maths::Matrix4::Identity,
        0u
    };
    firstDescriptor.objectFlags = NLS::Render::Data::kDrawableObjectFlagReceiveShadows;
    firstDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(firstDescriptor));
    NLS::Render::Entities::Drawable secondDrawable;
    NLS::Engine::Rendering::EngineDrawableDescriptor secondDescriptor {
        secondMatrix,
        NLS::Maths::Matrix4::Identity,
        1u
    };
    secondDescriptor.objectFlags = NLS::Render::Data::kDrawableObjectFlagCastShadows;
    secondDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(secondDescriptor));

    NLS::Render::Data::PipelineState pso;
    provider.PrepareDraw(pso, firstDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets firstBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, firstDrawable, firstBindings));
    ASSERT_NE(firstBindings.objectBindingSet, nullptr);
    EXPECT_EQ(firstBindings.objectConstants.objectIndex, 0u);
    EXPECT_EQ(firstBindings.objectConstants.objectFlags,
        NLS::Render::Data::kDrawableObjectFlagReceiveShadows);
    EXPECT_EQ(firstBindings.objectConstants.padding0, 0u);
    EXPECT_EQ(firstBindings.objectConstants.padding1, 0u);

    provider.PrepareDraw(pso, secondDrawable);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets secondBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, secondDrawable, secondBindings));
    EXPECT_EQ(secondBindings.objectConstants.objectIndex, 1u);
    EXPECT_EQ(secondBindings.objectConstants.objectFlags,
        NLS::Render::Data::kDrawableObjectFlagCastShadows);

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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
    EXPECT_EQ(provider.GetLegacyObjectBufferWriteCountForTesting(), 0u);
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));

    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectConstants.objectIndex, 0u);
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

TEST(RendererFrameObjectBindingTests, EngineProviderUsesEffectivePassShaderForIndexedObjectData)
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
    frameContext.frameIndex = 50u;
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

    auto* rootShader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/ShaderLabRoot.hlsl",
        MakeFrameConstantOnlyReflection("FrameConstants"),
        "shader:shaderlab-root");
    auto* effectivePassShader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/ShaderLabGBuffer.hlsl",
        MakeIndexedObjectDataReflection(),
        "shader:shaderlab-gbuffer");
    ASSERT_NE(rootShader, nullptr);
    ASSERT_NE(effectivePassShader, nullptr);
    ASSERT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*rootShader));
    ASSERT_TRUE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*effectivePassShader));

    NLS::Render::Resources::Material material(rootShader);
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Translation({ 6.0f, 7.0f, 8.0f }),
        NLS::Maths::Matrix4::Identity
    });

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable, material, *effectivePassShader));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));

    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectConstants.objectIndex, 0u);
    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(
        bindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::StructuredBuffer);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(effectivePassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(rootShader));
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
    EXPECT_EQ(secondIndexedBindings.objectConstants.objectIndex, 5u);
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
    EXPECT_EQ(bindings.objectConstants.objectIndex, 7u);

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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
    EXPECT_EQ(bindings.objectConstants.objectIndex, 11u);

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    std::vector<NLS::Render::Resources::ShaderParameterStruct> parameterStructs;
    parameterStructs.push_back(
        NLS::Render::Resources::ShaderParameterStructBuilder("LegacyObjectParameters")
            .SetGroup(NLS::Render::Resources::ShaderParameterGroupKind::Object)
            .AddUniformBuffer(
                "ObjectConstants",
                0u,
                sizeof(NLS::Maths::Matrix4),
                NLS::Render::RHI::ShaderStageMask::Vertex)
            .Build());
    shader->SetParameterStructsForTesting(std::move(parameterStructs));

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsInvalidObjectIndexAfterObjectDataSlotIsFull)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    const ScopedObjectDataCountLimitOverride scopedLimit(3u);
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
    frameContext.frameIndex = 58u;
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 3.0f, 4.0f, 5.0f });
    NLS::Render::Entities::Drawable fullDrawable;
    fullDrawable.material = &material;
    fullDrawable.instanceCount = 3u;
    fullDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        0u,
        3u,
        { modelMatrix, modelMatrix, modelMatrix }
    });

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, fullDrawable));

    NLS::Render::Entities::Drawable overflowDrawable;
    overflowDrawable.material = &material;
    overflowDrawable.instanceCount = 1u;
    overflowDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex,
        1u,
        { modelMatrix }
    });

    EXPECT_FALSE(provider.PrepareDraw(pso, overflowDrawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    EXPECT_FALSE(provider.CapturePreparedBindingSets(pso, overflowDrawable, bindings));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to override object-data limits.";
#endif
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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
    EXPECT_EQ(bindings.objectConstants.objectIndex, 33u);
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

TEST(RendererFrameObjectBindingTests, EngineProviderKeepsIndexedObjectDataShaderSupportCacheAcrossFrames)
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
    frameContext.frameIndex = 50u;
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

    auto* shader = CreateReflectionOnlyObjectDataShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity,
        0u
    });

    NLS::Render::Data::PipelineState pso;
    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    EXPECT_EQ(provider.GetIndexedObjectDataShaderSupportQueryCountForTesting(), 1u);
    provider.EndFrame();

    frameContext.frameIndex = 51u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    EXPECT_EQ(provider.GetIndexedObjectDataShaderSupportQueryCountForTesting(), 0u);
    provider.EndFrame();

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderSkipsUnchangedObjectDataUploadWhenFrameSlotIsReused)
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
    frameContext.frameIndex = 58u;
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
    NLS::Render::Data::PipelineState pso;

    const auto prepareObject = [&](const NLS::Maths::Matrix4& modelMatrix)
        -> std::shared_ptr<TestBuffer>
    {
        provider.BeginFrame(frameDescriptor);
        NLS::Render::Entities::Drawable drawable;
        drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
            modelMatrix,
            NLS::Maths::Matrix4::Identity,
            0u
        });
        EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
        EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
        provider.EndFrame();
        return std::dynamic_pointer_cast<TestBuffer>(
            bindings.objectBindingSet->GetDesc().entries[0].buffer);
    };

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    auto objectBuffer = prepareObject(firstMatrix);
    ASSERT_NE(objectBuffer, nullptr);
    EXPECT_EQ(objectBuffer->updateCalls, 1u);

    auto reusedBuffer = prepareObject(firstMatrix);
    ASSERT_EQ(reusedBuffer, objectBuffer);
    EXPECT_EQ(objectBuffer->updateCalls, 1u);

    const auto changedMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });
    auto changedBuffer = prepareObject(changedMatrix);
    ASSERT_EQ(changedBuffer, objectBuffer);
    EXPECT_EQ(objectBuffer->updateCalls, 2u);

    NLS::Maths::Matrix4 capturedMatrix;
    std::memcpy(&capturedMatrix, objectBuffer->uploadData.data(), sizeof(capturedMatrix));
    const auto expectedMatrix = NLS::Maths::Matrix4::Transpose(changedMatrix);
    for (size_t index = 0u; index < std::size(capturedMatrix.data); ++index)
        EXPECT_FLOAT_EQ(capturedMatrix.data[index], expectedMatrix.data[index]);

    const auto prepareInstances = [&](std::vector<NLS::Maths::Matrix4> modelMatrices)
        -> std::shared_ptr<TestBuffer>
    {
        provider.BeginFrame(frameDescriptor);
        NLS::Render::Entities::Drawable drawable;
        drawable.instanceCount = static_cast<uint32_t>(modelMatrices.size());
        NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
        descriptor.modelMatrix = modelMatrices.front();
        descriptor.objectIndex = 0u;
        descriptor.objectCount = drawable.instanceCount;
        descriptor.instanceModelMatrices = std::move(modelMatrices);
        drawable.AddDescriptor(std::move(descriptor));
        EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
        EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
        provider.EndFrame();
        return std::dynamic_pointer_cast<TestBuffer>(
            bindings.objectBindingSet->GetDesc().entries[0].buffer);
    };

    std::vector<NLS::Maths::Matrix4> instanceMatrices {
        NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f }),
        NLS::Maths::Matrix4::Translation({ 2.0f, 0.0f, 0.0f }),
        NLS::Maths::Matrix4::Translation({ 3.0f, 0.0f, 0.0f })
    };
    auto instanceBuffer = prepareInstances(instanceMatrices);
    ASSERT_EQ(instanceBuffer, objectBuffer);
    EXPECT_EQ(objectBuffer->updateCalls, 3u);

    auto reusedInstanceBuffer = prepareInstances(instanceMatrices);
    ASSERT_EQ(reusedInstanceBuffer, objectBuffer);
    EXPECT_EQ(objectBuffer->updateCalls, 3u);

    instanceMatrices[1] = NLS::Maths::Matrix4::Translation({ 8.0f, 0.0f, 0.0f });
    auto changedInstanceBuffer = prepareInstances(instanceMatrices);
    ASSERT_EQ(changedInstanceBuffer, objectBuffer);
    EXPECT_EQ(objectBuffer->updateCalls, 4u);

    NLS::Maths::Matrix4 capturedInstanceMatrix;
    std::memcpy(
        &capturedInstanceMatrix,
        objectBuffer->uploadData.data() + sizeof(NLS::Maths::Matrix4),
        sizeof(capturedInstanceMatrix));
    const auto expectedInstanceMatrix = NLS::Maths::Matrix4::Transpose(instanceMatrices[1]);
    for (size_t index = 0u; index < std::size(capturedInstanceMatrix.data); ++index)
        EXPECT_FLOAT_EQ(capturedInstanceMatrix.data[index], expectedInstanceMatrix.data[index]);

    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, SpatialVisibilityPipelineDrawsKeepRendererAssignedObjectIndices)
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

    auto* shader = CreateReflectionOnlyObjectDataShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{0.0f, 0.0f, 0.0f}, 1.0f});

    NLS::Engine::SceneSystem::Scene scene;
    auto& object = scene.CreateGameObject("SpatialObjectIndexVisible");
    auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(&mesh);
    meshRenderer->FillWithMaterial(material);
    meshRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
    object.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &material;
    ASSERT_EQ(renderScene.Synchronize(scene, syncOptions).rebuiltCachedCommandCount, 1u);

    NLS::Render::Data::Frustum frustum;
    const auto view = NLS::Maths::Matrix4::CreateView(
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f,
        0.0f, 1.0f, 0.0f);
    const auto projection = NLS::Maths::Matrix4::CreatePerspective(90.0f, 1.0f, 0.1f, 100.0f);
    frustum.CalculateFrustum(projection * view);

    auto largeSceneSettings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    largeSceneSettings.enableSpatialIndex = true;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &largeSceneSettings;

    const auto visibleQueues = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    ASSERT_EQ(visibleQueues.opaques.size(), 1u);

    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    ASSERT_TRUE(visibleQueues.opaques.front().second.TryGetDescriptor(descriptor));
    EXPECT_EQ(descriptor.objectIndex, 0u);
    EXPECT_EQ(descriptor.objectCount, 1u);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, visibleQueues.opaques.front().second));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, visibleQueues.opaques.front().second, bindings));
    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectConstants.objectIndex, 0u);
    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(
        bindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::StructuredBuffer);
    ASSERT_NE(bindings.objectBindingSet->GetDesc().entries[0].buffer, nullptr);
    EXPECT_EQ(bindings.objectBindingSet->GetDesc().entries[0].buffer->GetDebugName(), "EngineObjectDataBuffer");

    provider.EndFrame();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderRebuildsIndexedObjectDataResourcesWhenExplicitDeviceChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, firstDevice);

    auto& firstFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    firstFrameContext.frameIndex = 49u;
    firstFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(firstFrameContext.descriptorAllocator, nullptr);
    firstFrameContext.descriptorAllocator->BeginFrame(firstFrameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateReflectionOnlyObjectDataShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity,
        7u
    });

    NLS::Render::Data::PipelineState pso;
    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets firstBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, firstBindings));
    ASSERT_NE(firstBindings.objectBindingSet, nullptr);
    EXPECT_GE(firstDevice->bindingLayoutCreateCalls, 1u);
    EXPECT_GE(firstDevice->bindingSetCreateCalls, 1u);
    const auto firstBindingSet = firstBindings.objectBindingSet;
    const auto firstBuffer = firstBindings.objectBindingSet->GetDesc().entries[0].buffer;
    ASSERT_NE(firstBuffer, nullptr);
    provider.EndFrame();

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    secondFrameContext.frameIndex = 50u;
    secondFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(secondFrameContext.descriptorAllocator, nullptr);
    secondFrameContext.descriptorAllocator->BeginFrame(secondFrameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets secondBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, secondBindings));
    provider.EndFrame();

    ASSERT_NE(secondBindings.objectBindingSet, nullptr);
    EXPECT_GE(secondDevice->bindingLayoutCreateCalls, 1u);
    EXPECT_GE(secondDevice->bindingSetCreateCalls, 1u);
    ASSERT_FALSE(secondDevice->buffers.empty());
    EXPECT_NE(secondBindings.objectBindingSet, firstBindingSet);
    EXPECT_NE(secondBindings.objectBindingSet->GetDesc().entries[0].buffer, firstBuffer);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, EngineProviderRebuildsExplicitFrameObjectBindingSetsWhenExplicitDeviceChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, firstDevice);

    auto& firstFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    firstFrameContext.frameIndex = 53u;
    firstFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(firstFrameContext.descriptorAllocator, nullptr);
    firstFrameContext.descriptorAllocator->BeginFrame(firstFrameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    NLS::Render::Entities::Drawable drawable;
    drawable.AddDescriptor<NLS::Render::Data::DrawableObjectDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity,
        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex
    });

    NLS::Render::Data::PipelineState pso;
    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets firstBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, firstBindings));
    ASSERT_NE(firstBindings.frameBindingSet, nullptr);
    ASSERT_NE(firstBindings.objectBindingSet, nullptr);
    const auto firstFrameBindingSet = firstBindings.frameBindingSet;
    const auto firstObjectBindingSet = firstBindings.objectBindingSet;
    provider.EndFrame();

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    secondFrameContext.frameIndex = 54u;
    secondFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(secondFrameContext.descriptorAllocator, nullptr);
    secondFrameContext.descriptorAllocator->BeginFrame(secondFrameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets secondBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, secondBindings));
    provider.EndFrame();

    ASSERT_NE(secondBindings.frameBindingSet, nullptr);
    ASSERT_NE(secondBindings.objectBindingSet, nullptr);
    EXPECT_NE(secondBindings.frameBindingSet, firstFrameBindingSet);
    EXPECT_NE(secondBindings.objectBindingSet, firstObjectBindingSet);
    EXPECT_GE(secondDevice->bindingSetCreateCalls, 2u);

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
    auto reflection = shader->GetReflection();
    ASSERT_EQ(reflection.properties.size(), 1u);
    reflection.properties[0].byteSize = sizeof(uint32_t);
    shader->SetReflectionForTesting(std::move(reflection));

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

TEST(RendererFrameObjectBindingTests, EngineProviderPreparedFrameResourcePreflightFailsWhenThreadedSlotsAreBackPressured)
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

    for (size_t slotIndex = 0u; slotIndex < 2u; ++slotIndex)
    {
        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, slotIndex);
        frameContext.frameIndex = 50u + slotIndex;
        frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
        ASSERT_NE(frameContext.descriptorAllocator, nullptr);
        frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    }
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    const auto publishPreparedFrame = [&](const uint64_t frameId, const size_t expectedSlot)
    {
        NLS::Render::Context::FrameSnapshot snapshot;
        snapshot.frameId = frameId;
        NLS::Render::Context::RenderScenePackage package;
        package.frameId = snapshot.frameId;
        size_t publishedSlot = 99u;
        ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
            driver,
            snapshot,
            package,
            &publishedSlot));
        EXPECT_EQ(publishedSlot, expectedSlot);
    };

    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.TryReservePreparedFrameResources());
    provider.EndFrame();
    publishPreparedFrame(301u, 0u);

    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.TryReservePreparedFrameResources());
    provider.EndFrame();
    publishPreparedFrame(302u, 1u);

    provider.BeginFrame(frameDescriptor);
    EXPECT_FALSE(provider.TryReservePreparedFrameResources());

    auto* shader = CreateReflectionOnlyObjectDataShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity,
        0u
    });

    NLS::Render::Data::PipelineState pso;
    EXPECT_FALSE(provider.PrepareDraw(pso, drawable));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
}

TEST(RendererFrameObjectBindingTests, EngineProviderCanReleasePreparedFrameReservationAfterPublishFailure)
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
    frameContext.frameIndex = 70u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.TryReservePreparedFrameResources());
    EXPECT_EQ(
        NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver),
        std::optional<size_t>(0u));

    provider.ReleaseReservedPreparedFrameResources();
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());

    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.frameId = 401u;
    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    size_t publishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        package,
        &publishedSlot));
    EXPECT_EQ(publishedSlot, 0u);

    provider.EndFrame();
}

TEST(RendererFrameObjectBindingTests, EngineProviderSelectionReservationGuardDoesNotReleaseExistingGBufferPreflight)
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
    frameContext.frameIndex = 71u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.TryReservePreparedFrameResources());
    ASSERT_TRUE(provider.HasReservedPreparedFrameResources());
    EXPECT_EQ(
        NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver),
        std::optional<size_t>(0u));

    const bool hadPreparedFrameReservationBeforeSelection = provider.HasReservedPreparedFrameResources();
    const auto releaseSelectionOwnedReservation = [&provider, hadPreparedFrameReservationBeforeSelection]()
    {
        if (!hadPreparedFrameReservationBeforeSelection && provider.HasReservedPreparedFrameResources())
            provider.ReleaseReservedPreparedFrameResources();
    };
    releaseSelectionOwnedReservation();

    EXPECT_TRUE(provider.HasReservedPreparedFrameResources())
        << "Selection outline fallback paths must not release the GBuffer reservation they did not create.";
    EXPECT_EQ(
        NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver),
        std::optional<size_t>(0u));

    provider.ReleaseReservedPreparedFrameResources();
    EXPECT_FALSE(provider.HasReservedPreparedFrameResources());
    provider.EndFrame();
}

TEST(RendererFrameObjectBindingTests, EngineProviderSelectionReservationGuardReleasesLateSelectionReservation)
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
    frameContext.frameIndex = 72u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    provider.BeginFrame(frameDescriptor);
    ASSERT_FALSE(provider.HasReservedPreparedFrameResources());
    const bool hadPreparedFrameReservationBeforeSelection = provider.HasReservedPreparedFrameResources();

    ASSERT_TRUE(provider.TryReservePreparedFrameResources());
    ASSERT_TRUE(provider.HasReservedPreparedFrameResources());

    const auto releaseSelectionOwnedReservation = [&provider, hadPreparedFrameReservationBeforeSelection]()
    {
        if (!hadPreparedFrameReservationBeforeSelection && provider.HasReservedPreparedFrameResources())
            provider.ReleaseReservedPreparedFrameResources();
    };
    releaseSelectionOwnedReservation();

    EXPECT_FALSE(provider.HasReservedPreparedFrameResources());
    EXPECT_FALSE(NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());
    provider.EndFrame();
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

TEST(RendererFrameObjectBindingTests, ImmediateIndexedDrawPushesReceiveShadowObjectConstantsAfterProviderBindsObjectData)
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto modelMatrix = NLS::Maths::Matrix4::Translation({ 2.0f, 4.0f, 6.0f });
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = 1u;
    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor {
        modelMatrix,
        NLS::Maths::Matrix4::Identity,
        21u
    };
    descriptor.objectFlags = NLS::Render::Data::kDrawableObjectFlagReceiveShadows;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(descriptor));

    NLS::Render::Data::PipelineState pso;
    renderer.DrawEntity(pso, drawable);

    EXPECT_EQ(renderer.commandBuffer()->bindGraphicsPipelineCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer()->pushConstantCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer()->lastObjectIndexPushConstant, 21u);
    EXPECT_EQ(renderer.commandBuffer()->lastPushConstantSize,
        sizeof(NLS::Render::Data::ObjectDrawConstants));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        renderer.commandBuffer()->lastPushConstantStageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        renderer.commandBuffer()->lastPushConstantStageMask,
        NLS::Render::RHI::ShaderStageMask::Fragment));
    ASSERT_EQ(renderer.commandBuffer()->lastPushConstantBytes.size(),
        sizeof(NLS::Render::Data::ObjectDrawConstants));
    NLS::Render::Data::ObjectDrawConstants capturedConstants;
    std::memcpy(
        &capturedConstants,
        renderer.commandBuffer()->lastPushConstantBytes.data(),
        sizeof(capturedConstants));
    EXPECT_EQ(capturedConstants.objectIndex, 21u);
    EXPECT_EQ(capturedConstants.objectFlags,
        NLS::Render::Data::kDrawableObjectFlagReceiveShadows);
    EXPECT_EQ(capturedConstants.padding0, 0u);
    EXPECT_EQ(capturedConstants.padding1, 0u);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, ImmediateNonIndexedDrawPushesCompleteObjectConstants)
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

    auto reflection = MakeFrameConstantOnlyReflection("FrameConstants");
    reflection.constantBuffers.push_back(MakeObjectDrawConstantsReflection(
        sizeof(NLS::Render::Data::ObjectDrawConstants),
        NLS::Render::RHI::ShaderStageMask::Fragment));
    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/NonIndexedObjectConstants.hlsl",
        reflection,
        "shader:non-indexed-object-constants");
    ASSERT_NE(shader, nullptr);
    ASSERT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));

    NLS::Render::Resources::Material material(shader);
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = 1u;
    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor {
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity,
        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex
    };
    descriptor.objectFlags = NLS::Render::Data::kDrawableObjectFlagReceiveShadows;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(descriptor));

    NLS::Render::Data::PipelineState pso;
    renderer.DrawEntity(pso, drawable);

    EXPECT_EQ(renderer.commandBuffer()->bindGraphicsPipelineCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer()->pushConstantCalls, 1u);
    ASSERT_EQ(renderer.commandBuffer()->lastPushConstantBytes.size(),
        sizeof(NLS::Render::Data::ObjectDrawConstants));
    NLS::Render::Data::ObjectDrawConstants capturedConstants;
    std::memcpy(
        &capturedConstants,
        renderer.commandBuffer()->lastPushConstantBytes.data(),
        sizeof(capturedConstants));
    EXPECT_EQ(capturedConstants.objectIndex,
        NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
    EXPECT_EQ(capturedConstants.objectFlags,
        NLS::Render::Data::kDrawableObjectFlagReceiveShadows);
    EXPECT_EQ(capturedConstants.padding0, 0u);
    EXPECT_EQ(capturedConstants.padding1, 0u);

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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

TEST(RendererFrameObjectBindingTests, PreparedDrawPushesReceiveShadowObjectConstantsBeforeSubmission)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    ObjectIndexSubmitRenderer renderer(*driver);
    NLS::Render::Data::ObjectDrawConstants constants;
    constants.objectIndex = 9u;
    constants.objectFlags = NLS::Render::Data::kDrawableObjectFlagReceiveShadows;
    renderer.SubmitWithObjectConstants(constants);

    EXPECT_EQ(renderer.commandBuffer->pushConstantCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer->lastObjectIndexPushConstant, 9u);
    EXPECT_EQ(renderer.commandBuffer->lastPushConstantSize, sizeof(constants));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        renderer.commandBuffer->lastPushConstantStageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        renderer.commandBuffer->lastPushConstantStageMask,
        NLS::Render::RHI::ShaderStageMask::Fragment));
    ASSERT_EQ(renderer.commandBuffer->lastPushConstantBytes.size(), sizeof(constants));
    NLS::Render::Data::ObjectDrawConstants capturedConstants;
    std::memcpy(&capturedConstants, renderer.commandBuffer->lastPushConstantBytes.data(), sizeof(capturedConstants));
    EXPECT_EQ(capturedConstants.objectIndex, constants.objectIndex);
    EXPECT_EQ(capturedConstants.objectFlags, constants.objectFlags);
    EXPECT_EQ(capturedConstants.padding0, 0u);
    EXPECT_EQ(capturedConstants.padding1, 0u);
}

TEST(RendererFrameObjectBindingTests, PreparedNonIndexedDrawPushesCompleteObjectConstantsBeforeSubmission)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    ObjectIndexSubmitRenderer renderer(*driver);
    NLS::Render::Data::ObjectDrawConstants constants;
    constants.objectIndex = NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
    constants.objectFlags = NLS::Render::Data::kDrawableObjectFlagReceiveShadows;
    renderer.SubmitWithObjectConstants(constants, false);

    EXPECT_EQ(renderer.commandBuffer->pushConstantCalls, 1u);
    EXPECT_EQ(renderer.commandBuffer->lastPushConstantSize, sizeof(constants));
    ASSERT_EQ(renderer.commandBuffer->lastPushConstantBytes.size(), sizeof(constants));
    NLS::Render::Data::ObjectDrawConstants capturedConstants;
    std::memcpy(&capturedConstants, renderer.commandBuffer->lastPushConstantBytes.data(), sizeof(capturedConstants));
    EXPECT_EQ(capturedConstants.objectIndex, constants.objectIndex);
    EXPECT_EQ(capturedConstants.objectFlags, constants.objectFlags);
    EXPECT_EQ(capturedConstants.padding0, 0u);
    EXPECT_EQ(capturedConstants.padding1, 0u);
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

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingSetIsReusedUntilMaterialBindingChanges)
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

    const auto firstBindingSet = material.GetExplicitBindingSet(explicitDevice);
    const auto firstCreationCount = material.GetExplicitBindingSetCreationCount();
    const auto secondBindingSet = material.GetExplicitBindingSet(explicitDevice);

    ASSERT_NE(firstBindingSet, nullptr);
    EXPECT_EQ(secondBindingSet, firstBindingSet);
    EXPECT_EQ(material.GetExplicitBindingSetCreationCount(), firstCreationCount);
    EXPECT_EQ(explicitDevice->bindingSetCreateCalls, 1u);

    NLS::Render::RHI::SamplerDesc samplerOverride;
    samplerOverride.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
    material.SetSamplerOverride("u_MaterialSampler", samplerOverride);
    const auto changedBindingSet = material.GetExplicitBindingSet(explicitDevice);

    ASSERT_NE(changedBindingSet, nullptr);
    EXPECT_NE(changedBindingSet, firstBindingSet);
    EXPECT_EQ(material.GetExplicitBindingSetCreationCount(), firstCreationCount + 1u);
    EXPECT_EQ(explicitDevice->bindingSetCreateCalls, 2u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialSetSamePointerValueDoesNotInvalidateExplicitBindings)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = CreateMaterialTextureShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    int firstTexture = 1;
    int secondTexture = 2;

    material.Set<int*>("u_MaterialTexture", &firstTexture);
    const auto firstBindingRevision = material.GetBindingRevision();

    material.Set<int*>("u_MaterialTexture", &firstTexture);
    EXPECT_EQ(material.GetBindingRevision(), firstBindingRevision);

    material.Set<int*>("u_MaterialTexture", &secondTexture);
    EXPECT_GT(material.GetBindingRevision(), firstBindingRevision);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MeshAndShaderInstanceIdsDistinguishResourceInstances)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);

    auto* firstShader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    auto* secondShader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(firstShader, nullptr);
    ASSERT_NE(secondShader, nullptr);
    EXPECT_NE(firstShader->GetInstanceId(), 0u);
    EXPECT_NE(secondShader->GetInstanceId(), 0u);
    EXPECT_NE(firstShader->GetInstanceId(), secondShader->GetInstanceId());

    NLS::Render::Resources::Mesh firstMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);
    NLS::Render::Resources::Mesh secondMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);
    EXPECT_NE(firstMesh.GetInstanceId(), 0u);
    EXPECT_NE(secondMesh.GetInstanceId(), 0u);
    EXPECT_NE(firstMesh.GetInstanceId(), secondMesh.GetInstanceId());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(firstShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(secondShader));
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

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingSetReportsConstantBufferValueCopyFailures)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = CreateMaterialConstantBufferShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    material.SetRawParameter("u_TestColor", 1.0f);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();

    const auto& bindingSet = material.GetExplicitBindingSet(explicitDevice);

    ASSERT_NE(bindingSet, nullptr);
    EXPECT_TRUE(explicitDevice->lastBindingSetDesc.entries.empty());
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    const auto& diagnostic = material.GetLastExplicitBindingDiagnostics().front();
    EXPECT_EQ(diagnostic.severity, NLS::Render::Resources::MaterialBindingDiagnosticSeverity::Error);
    EXPECT_EQ(diagnostic.bindingName, "MaterialConstants");
    EXPECT_NE(diagnostic.message.find("u_TestColor"), std::string::npos);
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

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingLayoutValidationDiagnosticsDoNotAccumulate)
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

    EXPECT_EQ(material.GetExplicitBindingLayout(explicitDevice), nullptr);
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    EXPECT_EQ(material.GetExplicitBindingLayout(explicitDevice), nullptr);
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);

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
        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(shaderPath);
        ASSERT_NE(shader, nullptr) << shaderPath;
        ASSERT_TRUE(shader->HasParameterStructs()) << shaderPath;

        const auto parameterStructs = shader->GetParameterStructs();
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

TEST(RendererFrameObjectBindingTests, SelectionOutlineMaskPipelineLayoutSkipsRendererOwnedObjectIndexConstants)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/SelectionOutlineMask.hlsl",
        MakeIndexedObjectDataReflection(),
        "shader:selection-outline-mask");
    ASSERT_NE(shader, nullptr);
    ASSERT_TRUE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));
    const auto validation = NLS::Render::Resources::ValidateIndexedObjectDataShader(*shader);
    EXPECT_EQ(validation.status, NLS::Render::Resources::IndexedObjectDataShaderStatus::Compatible);
    EXPECT_TRUE(validation.diagnostic.empty());

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);
    ASSERT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants.size(), 1u);
    EXPECT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants[0].shaderRegister, 1u);
    EXPECT_EQ(
        explicitDevice->lastPipelineLayoutDesc.pushConstants[0].registerSpace,
        NLS::Render::RHI::BindingPointMap::kObjectBindingSpace);

    const auto& bindingLayouts = explicitDevice->lastPipelineLayoutDesc.bindingLayouts;
    auto objectLayout = std::find_if(
        bindingLayouts.begin(),
        bindingLayouts.end(),
        [](const std::shared_ptr<NLS::Render::RHI::RHIBindingLayout>& layout)
        {
            return layout != nullptr &&
                std::any_of(
                    layout->GetDesc().entries.begin(),
                    layout->GetDesc().entries.end(),
                    [](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
                    {
                        return entry.registerSpace == NLS::Render::RHI::BindingPointMap::kObjectBindingSpace;
                    });
        });
    ASSERT_NE(objectLayout, bindingLayouts.end());

    const auto& entries = (*objectLayout)->GetDesc().entries;
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "ObjectData");
    EXPECT_EQ(entries[0].type, NLS::Render::RHI::BindingType::StructuredBuffer);
    EXPECT_EQ(entries[0].binding, 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, LegacyFourByteObjectConstantsAreNotIndexedCompatible)
{
    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/LegacyIndexed.hlsl",
        MakeIndexedObjectDataReflection(true, sizeof(uint32_t)),
        "shader:legacy-indexed");
    ASSERT_NE(shader, nullptr);

    const auto validation = NLS::Render::Resources::ValidateIndexedObjectDataShader(*shader);
    EXPECT_EQ(validation.status, NLS::Render::Resources::IndexedObjectDataShaderStatus::Incompatible);
    EXPECT_FALSE(validation.diagnostic.empty());
    EXPECT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MissingObjectConstantsAreNotIndexedCompatible)
{
    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/MissingIndexedConstants.hlsl",
        MakeIndexedObjectDataReflection(false),
        "shader:missing-indexed-constants");
    ASSERT_NE(shader, nullptr);

    const auto validation = NLS::Render::Resources::ValidateIndexedObjectDataShader(*shader);
    EXPECT_EQ(validation.status, NLS::Render::Resources::IndexedObjectDataShaderStatus::Incompatible);
    EXPECT_FALSE(validation.diagnostic.empty());
    EXPECT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, WrongObjectConstantStageAndBindingAreNotIndexedCompatible)
{
    auto wrongStageReflection = MakeIndexedObjectDataReflection(
        true,
        sizeof(NLS::Render::Data::ObjectDrawConstants),
        NLS::Render::RHI::ShaderStageMask::Fragment);
    auto* wrongStageShader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/WrongIndexedStage.hlsl",
        wrongStageReflection,
        "shader:wrong-indexed-stage");
    ASSERT_NE(wrongStageShader, nullptr);
    EXPECT_EQ(
        NLS::Render::Resources::ValidateIndexedObjectDataShader(*wrongStageShader).status,
        NLS::Render::Resources::IndexedObjectDataShaderStatus::Incompatible);
    EXPECT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*wrongStageShader));

    auto wrongBindingReflection = MakeIndexedObjectDataReflection();
    wrongBindingReflection.constantBuffers.front().bindingIndex = 2u;
    auto* wrongBindingShader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/WrongIndexedBinding.hlsl",
        wrongBindingReflection,
        "shader:wrong-indexed-binding");
    ASSERT_NE(wrongBindingShader, nullptr);
    EXPECT_EQ(
        NLS::Render::Resources::ValidateIndexedObjectDataShader(*wrongBindingShader).status,
        NLS::Render::Resources::IndexedObjectDataShaderStatus::Incompatible);
    EXPECT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*wrongBindingShader));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(wrongStageShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(wrongBindingShader));
}

TEST(RendererFrameObjectBindingTests, ObjectConstantMembersMustMatchTheExactIndexedAbi)
{
    const auto expectIncompatible = [](NLS::Render::Resources::ShaderReflection reflection, const std::string& caseName)
    {
        SCOPED_TRACE(caseName);
        auto* shader = CreateReflectionOnlyImportedShader(
            "Tests/Synthetic/InvalidIndexedMembers.hlsl",
            reflection,
            "shader:invalid-indexed-members-" + caseName);
        ASSERT_NE(shader, nullptr);
        EXPECT_EQ(
            NLS::Render::Resources::ValidateIndexedObjectDataShader(*shader).status,
            NLS::Render::Resources::IndexedObjectDataShaderStatus::Incompatible);
        EXPECT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));
        EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    };

    auto wrongSpace = MakeIndexedObjectDataReflection();
    wrongSpace.constantBuffers.front().bindingSpace = NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace;
    expectIncompatible(std::move(wrongSpace), "wrong-space");

    auto emptyMembers = MakeIndexedObjectDataReflection();
    emptyMembers.constantBuffers.front().members.clear();
    expectIncompatible(std::move(emptyMembers), "empty-members");

    auto wrongName = MakeIndexedObjectDataReflection();
    wrongName.constantBuffers.front().members[1].name = "u_UnexpectedFlags";
    expectIncompatible(std::move(wrongName), "wrong-name");

    auto wrongOrder = MakeIndexedObjectDataReflection();
    std::swap(wrongOrder.constantBuffers.front().members[0], wrongOrder.constantBuffers.front().members[1]);
    expectIncompatible(std::move(wrongOrder), "wrong-order");

    auto wrongType = MakeIndexedObjectDataReflection();
    wrongType.constantBuffers.front().members[0].type = NLS::Render::Resources::UniformType::UNIFORM_FLOAT;
    expectIncompatible(std::move(wrongType), "wrong-type");

    auto wrongOffset = MakeIndexedObjectDataReflection();
    wrongOffset.constantBuffers.front().members[2].byteOffset = 12u;
    expectIncompatible(std::move(wrongOffset), "wrong-offset");
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(RendererFrameObjectBindingTests, MaterialNegativeCachesIncompatibleIndexedShaderPerDeviceAndGeneration)
{
    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/NegativeCachedIndexedMaterial.hlsl",
        MakeIndexedObjectDataReflection(true, sizeof(uint32_t)),
        "shader:negative-cached-indexed-material");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    auto secondDevice = std::make_shared<TestExplicitDevice>();
    auto* otherShader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/OtherNegativeCachedIndexedMaterial.hlsl",
        MakeIndexedObjectDataReflection(true, sizeof(uint32_t)),
        "shader:other-negative-cached-indexed-material");
    ASSERT_NE(otherShader, nullptr);
    ASSERT_NE(shader->GetInstanceId(), otherShader->GetInstanceId());

    EXPECT_EQ(material.GetExplicitPipelineLayout(firstDevice), nullptr);
    EXPECT_EQ(material.GetExplicitPipelineLayout(firstDevice), nullptr);
    EXPECT_EQ(firstDevice->pipelineLayoutCreateCalls, 0u);
    EXPECT_EQ(material.GetIndexedObjectDataShaderValidationCountForTesting(), 1u);
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    EXPECT_EQ(material.GetLastExplicitBindingDiagnostics().front().bindingName, "ObjectIndexConstants");

    EXPECT_EQ(material.GetExplicitBindingSet(firstDevice), nullptr);
    EXPECT_EQ(material.GetExplicitPipelineLayout(firstDevice), nullptr);
    EXPECT_EQ(material.GetIndexedObjectDataShaderValidationCountForTesting(), 1u);
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    EXPECT_EQ(material.GetLastExplicitBindingDiagnostics().front().bindingName, "ObjectIndexConstants");

    EXPECT_EQ(material.GetExplicitPipelineLayout(firstDevice, otherShader), nullptr);
    EXPECT_EQ(firstDevice->pipelineLayoutCreateCalls, 0u);
    EXPECT_EQ(material.GetIndexedObjectDataShaderValidationCountForTesting(), 2u);
    EXPECT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 2u);
    EXPECT_EQ(
        material.GetExplicitBindingDiagnosticCountForTesting(shader->GetInstanceId(), shader->GetGeneration()),
        1u);
    EXPECT_EQ(
        material.GetExplicitBindingDiagnosticCountForTesting(otherShader->GetInstanceId(), otherShader->GetGeneration()),
        1u);

    EXPECT_EQ(material.GetExplicitPipelineLayout(secondDevice), nullptr);
    EXPECT_EQ(secondDevice->pipelineLayoutCreateCalls, 0u);
    EXPECT_EQ(material.GetIndexedObjectDataShaderValidationCountForTesting(), 3u);
    EXPECT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 2u);

    const auto malformedGeneration = shader->GetGeneration();
    shader->SetReflectionForTesting(MakeIndexedObjectDataReflection());
    EXPECT_GT(shader->GetGeneration(), malformedGeneration);
    EXPECT_NE(material.GetExplicitPipelineLayout(firstDevice), nullptr);
    EXPECT_EQ(firstDevice->pipelineLayoutCreateCalls, 1u);
    EXPECT_EQ(material.GetIndexedObjectDataShaderValidationCountForTesting(), 4u);
    EXPECT_TRUE(material.GetLastExplicitBindingDiagnostics().empty());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(otherShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, EffectivePassReloadPrunesItsOldGenerationCachesAndDiagnostic)
{
    auto* forward = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/EffectiveReload.shader",
        MakeMaterialColorReflection("_Color"),
        "shader:EffectiveReload/Forward#0");
    auto* depth = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/EffectiveReload.shader",
        MakeIndexedObjectDataMaterialReflection("u_MaterialSampler", sizeof(uint32_t)),
        "shader:EffectiveReload/DepthOnly#1");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    NLS::Render::Resources::Material material(forward);
    auto device = std::make_shared<TestExplicitDevice>();
    const auto oldGeneration = depth->GetGeneration();

    EXPECT_NE(material.GetExplicitBindingLayout(device, depth), nullptr);
    EXPECT_NE(material.GetExplicitBindingSet(device, depth), nullptr);
    EXPECT_EQ(material.GetExplicitPipelineLayout(device, depth), nullptr);
    EXPECT_EQ(device->pipelineLayoutCreateCalls, 0u);
    EXPECT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);

    const auto oldCounts = material.GetExplicitShaderCacheEntryCountsForTesting(
        depth->GetInstanceId(),
        oldGeneration);
    EXPECT_EQ(oldCounts.bindingLayouts, 1u);
    EXPECT_EQ(oldCounts.bindingSets, 1u);
    EXPECT_EQ(oldCounts.pipelineLayouts, 1u);
    EXPECT_EQ(material.GetExplicitBindingDiagnosticCountForTesting(depth->GetInstanceId(), oldGeneration), 1u);

    depth->SetReflectionForTesting(MakeIndexedObjectDataMaterialReflection(
        "u_MaterialSampler",
        sizeof(NLS::Render::Data::ObjectDrawConstants)));
    ASSERT_GT(depth->GetGeneration(), oldGeneration);
    const auto currentGeneration = depth->GetGeneration();

    EXPECT_NE(material.GetExplicitPipelineLayout(device, depth), nullptr);
    EXPECT_EQ(device->pipelineLayoutCreateCalls, 1u);
    EXPECT_FALSE(material.HasExplicitBindingErrors());
    EXPECT_TRUE(material.GetLastExplicitBindingDiagnostics().empty());

    const auto prunedCounts = material.GetExplicitShaderCacheEntryCountsForTesting(
        depth->GetInstanceId(),
        oldGeneration);
    EXPECT_EQ(prunedCounts.bindingLayouts, 0u);
    EXPECT_EQ(prunedCounts.bindingSets, 0u);
    EXPECT_EQ(prunedCounts.pipelineLayouts, 0u);
    EXPECT_EQ(material.GetExplicitBindingDiagnosticCountForTesting(depth->GetInstanceId(), oldGeneration), 0u);

    const auto currentCounts = material.GetExplicitShaderCacheEntryCountsForTesting(
        depth->GetInstanceId(),
        currentGeneration);
    EXPECT_EQ(currentCounts.bindingLayouts, 0u);
    EXPECT_EQ(currentCounts.bindingSets, 0u);
    EXPECT_EQ(currentCounts.pipelineLayouts, 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(depth));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forward));
}

TEST(RendererFrameObjectBindingTests, EffectivePassReloadPreservesAnotherMalformedPassDiagnostic)
{
    auto* forward = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/MultiMalformed.shader",
        MakeMaterialColorReflection("_Color"),
        "shader:MultiMalformed/Forward#0");
    auto* depth = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/MultiMalformed.shader",
        MakeIndexedObjectDataMaterialReflection("u_MaterialSampler", sizeof(uint32_t)),
        "shader:MultiMalformed/DepthOnly#1");
    auto* shadow = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/MultiMalformed.shader",
        MakeIndexedObjectDataMaterialReflection("u_MaterialSampler", sizeof(uint32_t)),
        "shader:MultiMalformed/ShadowCaster#2");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);
    ASSERT_NE(shadow, nullptr);

    NLS::Render::Resources::Material material(forward);
    auto device = std::make_shared<TestExplicitDevice>();
    const auto depthOldGeneration = depth->GetGeneration();
    const auto shadowGeneration = shadow->GetGeneration();

    EXPECT_NE(material.GetExplicitBindingLayout(device, depth), nullptr);
    EXPECT_NE(material.GetExplicitBindingSet(device, depth), nullptr);
    EXPECT_EQ(material.GetExplicitPipelineLayout(device, depth), nullptr);
    EXPECT_NE(material.GetExplicitBindingLayout(device, shadow), nullptr);
    EXPECT_NE(material.GetExplicitBindingSet(device, shadow), nullptr);
    EXPECT_EQ(material.GetExplicitPipelineLayout(device, shadow), nullptr);
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 2u);
    EXPECT_EQ(material.GetExplicitBindingDiagnosticCountForTesting(depth->GetInstanceId(), depthOldGeneration), 1u);
    EXPECT_EQ(material.GetExplicitBindingDiagnosticCountForTesting(shadow->GetInstanceId(), shadowGeneration), 1u);
    const auto shadowCountsBeforeReload = material.GetExplicitShaderCacheEntryCountsForTesting(
        shadow->GetInstanceId(),
        shadowGeneration);
    EXPECT_EQ(shadowCountsBeforeReload.bindingLayouts, 1u);
    EXPECT_EQ(shadowCountsBeforeReload.bindingSets, 1u);
    EXPECT_EQ(shadowCountsBeforeReload.pipelineLayouts, 1u);

    depth->SetReflectionForTesting(MakeIndexedObjectDataMaterialReflection(
        "u_MaterialSampler",
        sizeof(NLS::Render::Data::ObjectDrawConstants)));
    EXPECT_NE(material.GetExplicitPipelineLayout(device, depth), nullptr);

    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    EXPECT_EQ(material.GetExplicitBindingDiagnosticCountForTesting(depth->GetInstanceId(), depthOldGeneration), 0u);
    EXPECT_EQ(material.GetExplicitBindingDiagnosticCountForTesting(shadow->GetInstanceId(), shadowGeneration), 1u);
    EXPECT_TRUE(material.HasExplicitBindingErrors());
    const auto shadowCountsAfterReload = material.GetExplicitShaderCacheEntryCountsForTesting(
        shadow->GetInstanceId(),
        shadowGeneration);
    EXPECT_EQ(shadowCountsAfterReload.bindingLayouts, 1u);
    EXPECT_EQ(shadowCountsAfterReload.bindingSets, 1u);
    EXPECT_EQ(shadowCountsAfterReload.pipelineLayouts, 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shadow));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(depth));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forward));
}

TEST(RendererFrameObjectBindingTests, EffectivePassReloadPrunesOldGenerationAcrossDevices)
{
    auto* forward = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/MultiDeviceReload.shader",
        MakeMaterialColorReflection("_Color"),
        "shader:MultiDeviceReload/Forward#0");
    auto* depth = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/MultiDeviceReload.shader",
        MakeIndexedObjectDataMaterialReflection("u_MaterialSampler", sizeof(uint32_t)),
        "shader:MultiDeviceReload/DepthOnly#1");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    NLS::Render::Resources::Material material(forward);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    auto secondDevice = std::make_shared<TestExplicitDevice>();
    const auto oldGeneration = depth->GetGeneration();

    for (const auto& device : { firstDevice, secondDevice })
    {
        EXPECT_NE(material.GetExplicitBindingLayout(device, depth), nullptr);
        EXPECT_NE(material.GetExplicitBindingSet(device, depth), nullptr);
        EXPECT_EQ(material.GetExplicitPipelineLayout(device, depth), nullptr);
    }
    const auto oldCounts = material.GetExplicitShaderCacheEntryCountsForTesting(
        depth->GetInstanceId(),
        oldGeneration);
    EXPECT_EQ(oldCounts.bindingLayouts, 2u);
    EXPECT_EQ(oldCounts.bindingSets, 2u);
    EXPECT_EQ(oldCounts.pipelineLayouts, 2u);

    depth->SetReflectionForTesting(MakeIndexedObjectDataMaterialReflection(
        "u_MaterialSampler",
        sizeof(NLS::Render::Data::ObjectDrawConstants)));
    const auto currentGeneration = depth->GetGeneration();

    const auto firstPipeline = material.GetExplicitPipelineLayout(firstDevice, depth);
    ASSERT_NE(firstPipeline, nullptr);
    const auto prunedCounts = material.GetExplicitShaderCacheEntryCountsForTesting(
        depth->GetInstanceId(),
        oldGeneration);
    EXPECT_EQ(prunedCounts.bindingLayouts, 0u);
    EXPECT_EQ(prunedCounts.bindingSets, 0u);
    EXPECT_EQ(prunedCounts.pipelineLayouts, 0u);

    EXPECT_NE(material.GetExplicitBindingLayout(secondDevice, depth), nullptr);
    EXPECT_NE(material.GetExplicitBindingSet(secondDevice, depth), nullptr);
    const auto secondPipeline = material.GetExplicitPipelineLayout(secondDevice, depth);
    ASSERT_NE(secondPipeline, nullptr);
    EXPECT_NE(firstPipeline, secondPipeline);
    EXPECT_EQ(firstDevice->pipelineLayoutCreateCalls, 1u);
    EXPECT_EQ(secondDevice->pipelineLayoutCreateCalls, 1u);

    const auto currentCounts = material.GetExplicitShaderCacheEntryCountsForTesting(
        depth->GetInstanceId(),
        currentGeneration);
    EXPECT_EQ(currentCounts.bindingLayouts, 1u);
    EXPECT_EQ(currentCounts.bindingSets, 1u);
    EXPECT_EQ(currentCounts.pipelineLayouts, 2u);
    EXPECT_TRUE(material.GetLastExplicitBindingDiagnostics().empty());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(depth));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forward));
}
#endif

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutRejectsLegacyIndexedObjectConstantsWithDiagnostic)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/LegacyIndexedMaterial.hlsl",
        MakeIndexedObjectDataReflection(true, sizeof(uint32_t)),
        "shader:legacy-indexed-material");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    EXPECT_EQ(pipelineLayout, nullptr);
    EXPECT_EQ(explicitDevice->pipelineLayoutCreateCalls, 0u);
    EXPECT_TRUE(material.HasExplicitBindingErrors());
    const auto& diagnostics = material.GetLastExplicitBindingDiagnostics();
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics.back().message.find("ObjectIndexConstants"), std::string::npos);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, SelectionOutlineCompositeShaderDoesNotRequireIndexedObjectData)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/SelectionOutlineComposite.hlsl",
        MakeFrameConstantOnlyReflection("FrameConstants"),
        "shader:selection-outline-composite");
    ASSERT_NE(shader, nullptr);
    const auto validation = NLS::Render::Resources::ValidateIndexedObjectDataShader(*shader);
    EXPECT_EQ(validation.status, NLS::Render::Resources::IndexedObjectDataShaderStatus::NotIndexed);
    EXPECT_TRUE(validation.diagnostic.empty());
    EXPECT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);

    const auto& bindingLayouts = explicitDevice->lastPipelineLayoutDesc.bindingLayouts;
    const auto objectLayout = std::find_if(
        bindingLayouts.begin(),
        bindingLayouts.end(),
        [](const std::shared_ptr<NLS::Render::RHI::RHIBindingLayout>& layout)
        {
            return layout != nullptr &&
                std::any_of(
                    layout->GetDesc().entries.begin(),
                    layout->GetDesc().entries.end(),
                    [](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
                    {
                        return entry.registerSpace == NLS::Render::RHI::BindingPointMap::kObjectBindingSpace &&
                            entry.name == "ObjectData";
                    });
        });
    EXPECT_EQ(objectLayout, bindingLayouts.end());
    EXPECT_TRUE(explicitDevice->lastPipelineLayoutDesc.pushConstants.empty());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialRequiresPassDescriptorSetWhenParameterStructDefinesPassBindings)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::ShaderReflection reflectionWithoutPassBindings;
    shader->SetReflectionForTesting(std::move(reflectionWithoutPassBindings));
    std::vector<NLS::Render::Resources::ShaderParameterStruct> parameterStructs;
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
    shader->SetParameterStructsForTesting(std::move(parameterStructs));

    NLS::Render::Resources::Material material(shader);

    EXPECT_TRUE(material.RequiresPassDescriptorSet());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingLayoutUsesEffectiveShaderPassReflection)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* forward = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/Multi.shader",
        MakeMaterialColorReflection("_ForwardColor"),
        "shader:Multi/Forward#0");
    auto* depth = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/Multi.shader",
        MakeMaterialColorReflection("_DepthColor"),
        "shader:Multi/DepthOnly#1");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);
    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        {});
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        {});

    NLS::Render::Resources::Material material(forward);
    material.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    material.RegisterShaderLabPassShader(forward);
    material.RegisterShaderLabPassShader(depth);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();

    const auto& forwardLayout = material.GetExplicitBindingLayout(explicitDevice, forward);
    ASSERT_NE(forwardLayout, nullptr);
    ASSERT_FALSE(forwardLayout->GetDesc().entries.empty());
    EXPECT_EQ(forwardLayout->GetDesc().entries.front().name, "_ForwardColorConstants");

    const auto& depthLayout = material.GetExplicitBindingLayout(explicitDevice, depth);
    ASSERT_NE(depthLayout, nullptr);
    ASSERT_FALSE(depthLayout->GetDesc().entries.empty());
    EXPECT_EQ(depthLayout->GetDesc().entries.front().name, "_DepthColorConstants")
        << "Material binding layout must be rebuilt from the LightMode-selected pass shader.";

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forward));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(depth));
}

TEST(RendererFrameObjectBindingTests, MaterialPassDescriptorSetQueryUsesEffectiveShaderPassReflection)
{
    auto* forward = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/Multi.shader",
        MakeMaterialColorReflection("_ForwardColor"),
        "shader:Multi/Forward#0");
    auto* depth = CreateReflectionOnlyImportedShader(
        "Assets/Shaders/Multi.shader",
        MakePassConstantOnlyReflection("DepthPassConstants"),
        "shader:Multi/DepthOnly#1");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    NLS::Render::Resources::Material material(forward);

    EXPECT_FALSE(material.RequiresPassDescriptorSet(forward));
    EXPECT_TRUE(material.RequiresPassDescriptorSet(depth));
    EXPECT_TRUE(material.RequiresPassDescriptorSet(depth));

    depth->SetReflectionForTesting(MakeMaterialColorReflection("_DepthColor"));
    EXPECT_FALSE(material.RequiresPassDescriptorSet(depth));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forward));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(depth));
}

TEST(RendererFrameObjectBindingTests, MaterialClearsShaderLabPassReferencesWhenShaderIsUnregistered)
{
    auto* forward = NLS::Render::Resources::Shader::CreateForTesting("Library/Artifacts/12/forwardhash");
    auto* depth = NLS::Render::Resources::Shader::CreateForTesting("Library/Artifacts/34/depthhash");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        {});
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        {});

    NLS::Render::Resources::Material material(forward);
    material.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    material.RegisterShaderLabPassShader(forward);
    material.RegisterShaderLabPassShader(depth);
    ASSERT_EQ(material.ResolveShaderForLightMode("DepthOnly"), depth);

    material.ClearShaderReferences(depth);

    EXPECT_EQ(material.ResolveShaderForLightMode("DepthOnly"), nullptr);
    EXPECT_EQ(material.ResolveShaderForLightMode("Forward"), forward);

    NLS::Render::Resources::Shader::DestroyForTesting(forward);
    NLS::Render::Resources::Shader::DestroyForTesting(depth);
}

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutRejectsIndexedObjectDataOnUnsupportedBackend)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutUsesShadowObjectConstantsOnSupportedBackend)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
        "App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    ASSERT_TRUE(shader->HasParameterStructs());

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);
    ASSERT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants.size(), 1u);
    const auto& objectConstantRange = explicitDevice->lastPipelineLayoutDesc.pushConstants.front();
    EXPECT_EQ(objectConstantRange.size, sizeof(NLS::Render::Data::ObjectDrawConstants));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        objectConstantRange.stageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        objectConstantRange.stageMask,
        NLS::Render::RHI::ShaderStageMask::Fragment));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutUsesObjectConstantsWithoutIndexedObjectDataOnDx12)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto reflection = MakeFrameConstantOnlyReflection("FrameConstants");
    reflection.constantBuffers.push_back(MakeObjectDrawConstantsReflection(
        sizeof(NLS::Render::Data::ObjectDrawConstants),
        NLS::Render::RHI::ShaderStageMask::Fragment));
    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/NonIndexedObjectConstants.hlsl",
        reflection,
        "shader:non-indexed-object-constants-layout");
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(
        NLS::Render::Resources::ValidateObjectDrawConstants(*shader).status,
        NLS::Render::Resources::ObjectDrawConstantsStatus::Compatible);
    EXPECT_EQ(
        NLS::Render::Resources::ValidateIndexedObjectDataShader(*shader).status,
        NLS::Render::Resources::IndexedObjectDataShaderStatus::NotIndexed);
    EXPECT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);
    ASSERT_EQ(explicitDevice->lastPipelineLayoutDesc.pushConstants.size(), 1u);
    const auto& objectConstantRange = explicitDevice->lastPipelineLayoutDesc.pushConstants.front();
    EXPECT_EQ(objectConstantRange.shaderRegister, 1u);
    EXPECT_EQ(objectConstantRange.registerSpace,
        NLS::Render::RHI::BindingPointMap::kObjectBindingSpace);
    EXPECT_EQ(objectConstantRange.size, sizeof(NLS::Render::Data::ObjectDrawConstants));
    EXPECT_EQ(objectConstantRange.stageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex |
            NLS::Render::RHI::ShaderStageMask::Fragment);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutPreservesLegacyObjectIndexConstantsAsDescriptorOnDx12)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto reflection = MakeFrameConstantOnlyReflection("FrameConstants");
    reflection.constantBuffers.push_back(MakeObjectDrawConstantsReflection(
        sizeof(uint32_t),
        NLS::Render::RHI::ShaderStageMask::Vertex));
    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/LegacyObjectIndexConstants.hlsl",
        reflection,
        "shader:legacy-object-index-constants-layout");
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(
        NLS::Render::Resources::ValidateObjectDrawConstants(*shader).status,
        NLS::Render::Resources::ObjectDrawConstantsStatus::Absent);
    EXPECT_EQ(
        NLS::Render::Resources::ValidateIndexedObjectDataShader(*shader).status,
        NLS::Render::Resources::IndexedObjectDataShaderStatus::NotIndexed);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);
    EXPECT_TRUE(explicitDevice->lastPipelineLayoutDesc.pushConstants.empty());

    const auto& bindingLayouts = explicitDevice->lastPipelineLayoutDesc.bindingLayouts;
    const auto objectLayout = std::find_if(
        bindingLayouts.begin(),
        bindingLayouts.end(),
        [](const std::shared_ptr<NLS::Render::RHI::RHIBindingLayout>& layout)
        {
            return layout != nullptr &&
                std::any_of(
                    layout->GetDesc().entries.begin(),
                    layout->GetDesc().entries.end(),
                    [](const NLS::Render::RHI::RHIBindingLayoutEntry& entry)
                    {
                        return entry.name == "ObjectIndexConstants";
                    });
        });
    ASSERT_NE(objectLayout, bindingLayouts.end());
    const auto& objectEntries = (*objectLayout)->GetDesc().entries;
    ASSERT_EQ(objectEntries.size(), 1u);
    EXPECT_EQ(objectEntries[0].name, "ObjectIndexConstants");
    EXPECT_EQ(objectEntries[0].type, NLS::Render::RHI::BindingType::UniformBuffer);
    EXPECT_EQ(objectEntries[0].binding, 1u);
    EXPECT_EQ(objectEntries[0].registerSpace,
        NLS::Render::RHI::BindingPointMap::kObjectBindingSpace);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutRejectsWrongBindingObjectIndexConstantsOnDx12)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto reflection = MakeFrameConstantOnlyReflection("FrameConstants");
    auto objectConstants = MakeObjectDrawConstantsReflection();
    objectConstants.bindingIndex = 2u;
    reflection.constantBuffers.push_back(std::move(objectConstants));
    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/WrongBindingObjectIndexConstants.hlsl",
        reflection,
        "shader:wrong-binding-object-index-constants-layout");
    ASSERT_NE(shader, nullptr);

    const auto validation = NLS::Render::Resources::ValidateObjectDrawConstants(*shader);
    EXPECT_EQ(validation.status, NLS::Render::Resources::ObjectDrawConstantsStatus::Incompatible);
    EXPECT_NE(validation.diagnostic.find("ObjectIndexConstants"), std::string::npos);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);

    EXPECT_EQ(material.GetExplicitPipelineLayout(explicitDevice), nullptr);
    EXPECT_EQ(explicitDevice->pipelineLayoutCreateCalls, 0u);
    EXPECT_TRUE(material.HasExplicitBindingErrors());
    const auto& diagnostics = material.GetLastExplicitBindingDiagnostics();
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics.back().message.find("ObjectIndexConstants"), std::string::npos);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutRejectsMixedLegacyAndRendererObjectIndexConstantsOnDx12)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto reflection = MakeFrameConstantOnlyReflection("FrameConstants");
    reflection.constantBuffers.push_back(MakeObjectDrawConstantsReflection());
    reflection.constantBuffers.push_back(MakeObjectDrawConstantsReflection(sizeof(uint32_t)));
    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/DuplicateObjectIndexConstants.hlsl",
        reflection,
        "shader:duplicate-object-index-constants-layout");
    ASSERT_NE(shader, nullptr);

    const auto validation = NLS::Render::Resources::ValidateObjectDrawConstants(*shader);
    EXPECT_EQ(validation.status, NLS::Render::Resources::ObjectDrawConstantsStatus::Incompatible);
    EXPECT_NE(validation.diagnostic.find("more than once"), std::string::npos);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::DX12);

    EXPECT_EQ(material.GetExplicitPipelineLayout(explicitDevice), nullptr);
    EXPECT_EQ(explicitDevice->pipelineLayoutCreateCalls, 0u);
    EXPECT_TRUE(material.HasExplicitBindingErrors());
    const auto& diagnostics = material.GetLastExplicitBindingDiagnostics();
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_NE(diagnostics.back().message.find("more than once"), std::string::npos);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, SharedEditorUnlitShaderKeepsLegacyObjectConstantsForBackendCompatibility)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/Unlit.hlsl",
        MakeFrameConstantOnlyReflection("FrameConstants"),
        "shader:shared-editor-unlit");
    ASSERT_NE(shader, nullptr);
    EXPECT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::Vulkan);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    EXPECT_NE(pipelineLayout, nullptr);
    EXPECT_TRUE(explicitDevice->lastPipelineLayoutDesc.pushConstants.empty());

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, DeferredLightingPipelineLayoutSkipsEmptyFrameAndObjectDescriptorSets)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/DeferredLighting.hlsl");
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

TEST(RendererFrameObjectBindingTests, LightGridFallbackGraphicsBindingSetUsesCompactEmptyGridBuffers)
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
    frameDescriptor.renderWidth = 1920u;
    frameDescriptor.renderHeight = 1080u;
    frameDescriptor.camera = &camera;

    NLS::Engine::Rendering::LightGridPrepass lightGridPrepass(driver);
    ASSERT_TRUE(lightGridPrepass.EnsureFallbackGraphicsPassBindingSet(frameDescriptor, false));

    const auto findBuffer = [&](std::string_view debugName) -> const TestBuffer*
    {
        const auto found = std::find_if(
            explicitDevice->buffers.begin(),
            explicitDevice->buffers.end(),
            [debugName](const std::shared_ptr<TestBuffer>& buffer)
            {
                return buffer != nullptr && buffer->GetDebugName() == debugName;
            });
        return found != explicitDevice->buffers.end() ? found->get() : nullptr;
    };

    const auto* forwardLocalLightBuffer = findBuffer("ForwardLocalLightBuffer");
    const auto* numCulledLightsGridBuffer = findBuffer("NumCulledLightsGrid");
    const auto* culledLightDataGridBuffer = findBuffer("CulledLightDataGrid");
    ASSERT_NE(forwardLocalLightBuffer, nullptr);
    ASSERT_NE(numCulledLightsGridBuffer, nullptr);
    ASSERT_NE(culledLightDataGridBuffer, nullptr);
    EXPECT_EQ(forwardLocalLightBuffer->GetDesc().size, sizeof(uint32_t));
    EXPECT_EQ(numCulledLightsGridBuffer->GetDesc().size, 2u * sizeof(uint32_t));
    EXPECT_EQ(culledLightDataGridBuffer->GetDesc().size, sizeof(uint32_t));

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
    pointLight.range = 25.0f;
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

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto pipelineCache = NLS::Render::RHI::CreateDefaultPipelineCache();

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.colorWrite = true;
    static constexpr NLS::Render::RHI::TextureFormat kGBufferFormats[] = {
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA8
    };
    overrides.SetColorFormats(kGBufferFormats);

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

TEST(RendererFrameObjectBindingTests, DeferredGBufferPipelineOverridesOwnDepthAndSampleState)
{
    NLS::Render::Resources::Material source;
    const auto overrides =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildGBufferMaterialOverridesForTesting(source);

    ASSERT_TRUE(overrides.hasDepthAttachment.has_value());
    EXPECT_TRUE(*overrides.hasDepthAttachment);
    ASSERT_TRUE(overrides.depthFormat.has_value());
    EXPECT_EQ(*overrides.depthFormat, NLS::Render::FrameGraph::kDeferredGBufferDepthFormat);
    ASSERT_TRUE(overrides.sampleCount.has_value());
    EXPECT_EQ(*overrides.sampleCount, 1u);
}

TEST(RendererFrameObjectBindingTests, RecordedPipelineOverridesUseOutputViewFormats)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.frameIndex = 71u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::RHI::RHITextureDesc colorTextureDesc;
    colorTextureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    colorTextureDesc.sampleCount = 4u;
    auto colorTexture = std::make_shared<TestTexture>(colorTextureDesc);
    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.format = NLS::Render::RHI::TextureFormat::RGBA16F;
    auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::RHI::RHITextureDesc depthTextureDesc;
    depthTextureDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    depthTextureDesc.sampleCount = 4u;
    auto depthTexture = std::make_shared<TestTexture>(depthTextureDesc);
    NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
    depthViewDesc.format = NLS::Render::RHI::TextureFormat::Depth32F;
    auto depthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputColorView = colorView;
    frameDescriptor.outputDepthStencilView = depthView;

    PipelineAttachmentOverrideProbeRenderer renderer(driver);
    renderer.BeginFrame(frameDescriptor);

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/TestGraphics.shader");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh resourceMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &resourceMesh;
    drawable.instanceCount = 1u;

    ASSERT_TRUE(renderer.Prepare({}, drawable));
    const auto& desc = explicitDevice->lastGraphicsPipelineDesc;
    ASSERT_EQ(desc.renderTargetLayout.colorFormats.size(), 1u);
    EXPECT_EQ(desc.renderTargetLayout.colorFormats[0], NLS::Render::RHI::TextureFormat::RGBA16F);
    EXPECT_TRUE(desc.renderTargetLayout.hasDepth);
    EXPECT_EQ(desc.renderTargetLayout.depthFormat, NLS::Render::RHI::TextureFormat::Depth32F);
    EXPECT_EQ(desc.renderTargetLayout.sampleCount, 4u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, PipelineStateRecordedDrawCanSelectShaderLabLightModePass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.frameIndex = 73u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    PipelineAttachmentOverrideProbeRenderer renderer(driver);
    renderer.BeginFrame(frameDescriptor);

    auto* forward = CreateTestGraphicsShader("Assets/Shaders/Multi.shader");
    auto* depth = CreateTestGraphicsShader("Assets/Shaders/Multi.shader");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    NLS::Render::ShaderLab::ShaderLabPassState forwardState;
    forwardState.cullMode = NLS::Render::ShaderLab::ShaderLabCullMode::Back;
    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        forwardState);

    NLS::Render::ShaderLab::ShaderLabPassState depthState;
    depthState.cullMode = NLS::Render::ShaderLab::ShaderLabCullMode::Off;
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        depthState);

    NLS::Render::Resources::Material material(forward);
    material.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    material.RegisterShaderLabPassShader(forward);
    material.RegisterShaderLabPassShader(depth);
    NLS::Render::Resources::Mesh resourceMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &resourceMesh;
    drawable.instanceCount = 1u;

    ASSERT_TRUE(renderer.Prepare({}, drawable, "DepthOnly"));
    EXPECT_FALSE(explicitDevice->lastGraphicsPipelineDesc.rasterState.cullEnabled);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forward));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(depth));
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, PipelineStateRecordedDrawDefaultsShaderLabMaterialsToForwardPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.frameIndex = 74u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;

    PipelineAttachmentOverrideProbeRenderer renderer(driver);
    renderer.BeginFrame(frameDescriptor);

    auto* forward = CreateTestGraphicsShader("Assets/Shaders/Multi.shader");
    auto* depth = CreateTestGraphicsShader("Assets/Shaders/Multi.shader");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    NLS::Render::ShaderLab::ShaderLabPassState forwardState;
    forwardState.cullMode = NLS::Render::ShaderLab::ShaderLabCullMode::Back;
    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        forwardState);

    NLS::Render::ShaderLab::ShaderLabPassState depthState;
    depthState.cullMode = NLS::Render::ShaderLab::ShaderLabCullMode::Off;
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        depthState);

    NLS::Render::Resources::Material material(depth);
    material.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    material.RegisterShaderLabPassShader(depth);
    NLS::Render::Resources::Mesh resourceMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &resourceMesh;
    drawable.instanceCount = 1u;

    EXPECT_FALSE(renderer.Prepare({}, drawable))
        << "Forward rendering must skip ShaderLab materials that have no Forward LightMode pass.";
    EXPECT_EQ(explicitDevice->graphicsPipelineCreateCalls, 0u);

    material.RegisterShaderLabPassShader(forward);
    ASSERT_TRUE(renderer.Prepare({}, drawable));
    EXPECT_TRUE(explicitDevice->lastGraphicsPipelineDesc.rasterState.cullEnabled);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forward));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(depth));
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, RecordedPipelineFailsClosedForMixedAttachmentSampleCounts)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.frameIndex = 72u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    NLS::Render::RHI::RHITextureDesc colorTextureDesc;
    colorTextureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    colorTextureDesc.sampleCount = 1u;
    auto colorTexture = std::make_shared<TestTexture>(colorTextureDesc);
    NLS::Render::RHI::RHITextureViewDesc colorViewDesc;
    colorViewDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    auto colorView = std::make_shared<TestTextureView>(colorTexture, colorViewDesc);

    NLS::Render::RHI::RHITextureDesc depthTextureDesc;
    depthTextureDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    depthTextureDesc.sampleCount = 4u;
    auto depthTexture = std::make_shared<TestTexture>(depthTextureDesc);
    NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
    depthViewDesc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    auto depthView = std::make_shared<TestTextureView>(depthTexture, depthViewDesc);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputColorView = colorView;
    frameDescriptor.outputDepthStencilView = depthView;

    PipelineAttachmentOverrideProbeRenderer renderer(driver);
    renderer.BeginFrame(frameDescriptor);

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/TestGraphics.shader");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh resourceMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &resourceMesh;
    drawable.instanceCount = 1u;

    EXPECT_FALSE(renderer.Prepare({}, drawable));
    EXPECT_EQ(explicitDevice->graphicsPipelineCreateCalls, 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    renderer.EndFrame();
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, RecordedPipelineOverridesCanForceBlending)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 recorded material pipeline override test requires the phase-1 Windows DX12 runtime.";
#endif

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    material.SetBlendable(false);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto pipelineCache = NLS::Render::RHI::CreateDefaultPipelineCache();

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.blending = true;
    overrides.colorWrite = true;

    NLS::Render::Data::PipelineState pipelineState;
    pipelineState.blending = false;

    const auto pipeline = material.BuildRecordedGraphicsPipeline(
        explicitDevice,
        pipelineCache,
        NLS::Render::Settings::EPrimitiveMode::TRIANGLES,
        pipelineState,
        overrides);

    ASSERT_NE(pipeline, nullptr);
    EXPECT_TRUE(explicitDevice->lastGraphicsPipelineDesc.blendState.enabled);
    ASSERT_FALSE(explicitDevice->lastGraphicsPipelineDesc.blendState.renderTargets.empty());
    EXPECT_TRUE(explicitDevice->lastGraphicsPipelineDesc.blendState.renderTargets.front().blendEnable);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, RecordedPipelineOverridesCanUseIndependentDecalGBufferBlendState)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 recorded material pipeline override test requires the phase-1 Windows DX12 runtime.";
#endif

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto pipelineCache = NLS::Render::RHI::CreateDefaultPipelineCache();

    const auto overrides =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildDeferredDecalMaterialOverridesForTesting(
            material);

    NLS::Render::Data::PipelineState pipelineState;
    pipelineState.stencilTest = true;
    pipelineState.stencilWriteMask = 0xFFu;

    const auto pipeline = material.BuildRecordedGraphicsPipeline(
        explicitDevice,
        pipelineCache,
        NLS::Render::Settings::EPrimitiveMode::TRIANGLES,
        pipelineState,
        overrides);

    ASSERT_NE(pipeline, nullptr);
    const auto& desc = explicitDevice->lastGraphicsPipelineDesc;
    constexpr auto rgbMask = NLS::Render::RHI::RHIColorWriteMask::Red |
        NLS::Render::RHI::RHIColorWriteMask::Green |
        NLS::Render::RHI::RHIColorWriteMask::Blue;
    EXPECT_TRUE(desc.blendState.enabled);
    EXPECT_TRUE(desc.blendState.independentBlendEnable);
    ASSERT_EQ(desc.blendState.renderTargets.size(), 3u);
    EXPECT_TRUE(desc.blendState.renderTargets[0].blendEnable);
    EXPECT_EQ(desc.blendState.renderTargets[0].colorWriteMask, rgbMask);
    EXPECT_EQ(desc.blendState.renderTargets[0].srcColor, NLS::Render::RHI::RHIBlendFactor::SrcAlpha);
    EXPECT_EQ(desc.blendState.renderTargets[0].dstColor, NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha);
    EXPECT_FALSE(desc.blendState.renderTargets[1].blendEnable);
    EXPECT_EQ(desc.blendState.renderTargets[1].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
    EXPECT_FALSE(desc.blendState.renderTargets[2].blendEnable);
    EXPECT_EQ(desc.blendState.renderTargets[2].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
    EXPECT_FALSE(desc.depthStencilState.stencilTest);
    EXPECT_EQ(desc.depthStencilState.stencilWriteMask, 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, RecordedPipelinePadsPerTargetBlendOverridesWithNoWriteTargets)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 recorded material pipeline override test requires the phase-1 Windows DX12 runtime.";
#endif

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;

    static auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto pipelineCache = NLS::Render::RHI::CreateDefaultPipelineCache();

    static constexpr NLS::Render::RHI::TextureFormat kGBufferFormats[] = {
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA8,
        NLS::Render::RHI::TextureFormat::RGBA8
    };

    NLS::Render::RHI::RHIRenderTargetBlendStateDesc albedoTarget;
    albedoTarget.blendEnable = true;
    albedoTarget.colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::All;

    NLS::Render::RHI::RHIRenderTargetBlendStateDesc normalTarget;
    normalTarget.blendEnable = false;
    normalTarget.colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::None;

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.SetColorFormats(kGBufferFormats);
    overrides.blending = true;
    overrides.colorWrite = true;
    const std::array<NLS::Render::RHI::RHIRenderTargetBlendStateDesc, 2u> shortTargets = {
        albedoTarget,
        normalTarget
    };
    overrides.SetRenderTargetBlendStates(shortTargets);

    const NLS::Render::Data::PipelineState pipelineState;
    const auto pipeline = material.BuildRecordedGraphicsPipeline(
        explicitDevice,
        pipelineCache,
        NLS::Render::Settings::EPrimitiveMode::TRIANGLES,
        pipelineState,
        overrides);

    ASSERT_NE(pipeline, nullptr);
    const auto& desc = explicitDevice->lastGraphicsPipelineDesc;
    ASSERT_EQ(desc.renderTargetLayout.colorFormats.size(), 3u);
    ASSERT_EQ(desc.blendState.renderTargets.size(), 3u);
    EXPECT_TRUE(desc.blendState.renderTargets[0].blendEnable);
    EXPECT_EQ(desc.blendState.renderTargets[0].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::All);
    EXPECT_FALSE(desc.blendState.renderTargets[1].blendEnable);
    EXPECT_EQ(desc.blendState.renderTargets[1].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
    EXPECT_FALSE(desc.blendState.renderTargets[2].blendEnable);
    EXPECT_EQ(desc.blendState.renderTargets[2].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, DeferredDecalOverridesBlendAlbedoOnlyAndDisableStencilWrites)
{
    NLS::Render::Resources::Material decalMaterial;
    decalMaterial.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Decal);
    decalMaterial.SetDepthTest(false);

    const auto overrides =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::BuildDeferredDecalMaterialOverridesForTesting(
            decalMaterial);

    ASSERT_TRUE(overrides.blending.has_value());
    EXPECT_TRUE(*overrides.blending);
    ASSERT_TRUE(overrides.depthTest.has_value());
    EXPECT_TRUE(*overrides.depthTest);
    ASSERT_TRUE(overrides.depthWrite.has_value());
    EXPECT_FALSE(*overrides.depthWrite);
    ASSERT_TRUE(overrides.hasDepthAttachment.has_value());
    EXPECT_TRUE(*overrides.hasDepthAttachment);
    ASSERT_TRUE(overrides.HasRenderTargetBlendStatesOverride());
    const auto targets = overrides.GetRenderTargetBlendStates();
    ASSERT_EQ(targets.size(), NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount);
    constexpr auto rgbMask = NLS::Render::RHI::RHIColorWriteMask::Red |
        NLS::Render::RHI::RHIColorWriteMask::Green |
        NLS::Render::RHI::RHIColorWriteMask::Blue;
    EXPECT_TRUE(targets[0].blendEnable);
    EXPECT_EQ(targets[0].colorWriteMask, rgbMask);
    EXPECT_FALSE(NLS::Render::RHI::HasColorWriteMask(
        targets[0].colorWriteMask,
        NLS::Render::RHI::RHIColorWriteMask::Alpha));
    EXPECT_FALSE(targets[1].blendEnable);
    EXPECT_EQ(targets[1].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
    EXPECT_FALSE(targets[2].blendEnable);
    EXPECT_EQ(targets[2].colorWriteMask, NLS::Render::RHI::RHIColorWriteMask::None);
    ASSERT_TRUE(overrides.stencilTest.has_value());
    EXPECT_FALSE(*overrides.stencilTest);
    ASSERT_TRUE(overrides.stencilWriteMask.has_value());
    EXPECT_EQ(*overrides.stencilWriteMask, 0u);
    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalDepthCompareForTesting(),
        NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalResolutionUsesSourcePassAndKeepsFallbackIdentitySeparateFromGBuffer)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* gbufferFallbackShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredGBufferFallback.hlsl");
    auto* decalFallbackShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalFallback.hlsl");
    auto* forwardShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalSource.shader");
    auto* decalPassShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalSource.shader");
    ASSERT_NE(gbufferFallbackShader, nullptr);
    ASSERT_NE(decalFallbackShader, nullptr);
    ASSERT_NE(forwardShader, nullptr);
    ASSERT_NE(decalPassShader, nullptr);
    decalPassShader->SetImportedShaderLabPassForTesting(
        "Tests/Shaders/DeferredDecalSource.shader",
        "shader:DeferredDecalSource/DeferredDecal#1",
        "DeferredDecal",
        NLS::Render::ShaderLab::ShaderLabPassState{});

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, gbufferFallbackShader);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, decalFallbackShader);

    NLS::Render::Resources::Material sourceWithPass(forwardShader);
    sourceWithPass.SetShaderLabSourcePath("Tests/Shaders/DeferredDecalSource.shader");
    sourceWithPass.RegisterShaderLabPassShader(forwardShader);
    sourceWithPass.RegisterShaderLabPassShader(decalPassShader);

    auto* sourceResolve =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            sourceWithPass);
    EXPECT_EQ(sourceResolve, &sourceWithPass);
    EXPECT_EQ(sourceWithPass.ResolveShaderForLightMode("DeferredDecal"), decalPassShader);

    NLS::Render::Resources::Material sourceWithoutPass(forwardShader);
    auto* decalFallback =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            sourceWithoutPass);
    auto& gbufferFallback =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveGBufferDrawableMaterialForTesting(
            renderer,
            sourceWithoutPass);
    ASSERT_NE(decalFallback, nullptr);
    EXPECT_NE(decalFallback, &gbufferFallback);
    EXPECT_EQ(decalFallback->GetShader(), decalFallbackShader);
    EXPECT_EQ(gbufferFallback.GetShader(), gbufferFallbackShader);
    EXPECT_TRUE(decalFallback->GetParameterBlock().Contains("u_Albedo"));
    EXPECT_TRUE(decalFallback->GetParameterBlock().Contains("u_AlbedoMap"));
    EXPECT_TRUE(decalFallback->GetParameterBlock().Contains("u_OpacityMap"));
    EXPECT_FALSE(decalFallback->GetParameterBlock().Contains("u_Roughness"));
    EXPECT_FALSE(decalFallback->GetParameterBlock().Contains("u_MetallicMap"));
    EXPECT_FALSE(decalFallback->GetParameterBlock().Contains("u_NormalMap"));
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).size(), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferMaterialCache(renderer).size(), 1u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetGBufferShader(renderer, nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(decalPassShader);
    NLS::Render::Resources::Shader::DestroyForTesting(forwardShader);
    NLS::Render::Resources::Shader::DestroyForTesting(decalFallbackShader);
    NLS::Render::Resources::Shader::DestroyForTesting(gbufferFallbackShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalResolutionReusesAndSynchronizesOnePersistentFallbackEntry)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* fallbackShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalFallback.hlsl");
    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSource.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    ASSERT_NE(sourceShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);

    NLS::Render::Resources::Material source(sourceShader);
    source.SetRawParameter("u_TestColor", NLS::Maths::Vector4{ 0.1f, 0.2f, 0.3f, 1.0f });

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ClearFrameDeferredDecalMaterialResolveCache(renderer);
    auto* first =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);
    auto* second =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(second, first);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).size(), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveCacheSize(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveMissCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).begin()->second.syncCount, 1u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResetFrameDeferredDecalMaterialResolveStats(renderer);
    auto* nextFrame =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);
    EXPECT_EQ(nextFrame, first);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveCacheSize(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveMissCount(renderer), 0u);

    source.SetRawParameter("u_TestColor", NLS::Maths::Vector4{ 0.8f, 0.7f, 0.6f, 1.0f });
    auto* propertyChanged =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);
    source.SetTextureResourcePath("u_AlbedoMap", ":Textures/DeferredDecal.ntx");
    auto* bindingChanged =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);

    EXPECT_EQ(propertyChanged, first);
    EXPECT_EQ(bindingChanged, first);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).size(), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveCacheSize(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveHitCount(renderer), 1u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveMissCount(renderer), 2u);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).begin()->second.syncCount, 3u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
    NLS::Render::Resources::Shader::DestroyForTesting(fallbackShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalResolutionKeepsSamePathMaterialInstancesIsolatedAcrossABA)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* fallbackShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalFallback.hlsl");
    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSource.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    ASSERT_NE(sourceShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);

    NLS::Render::Resources::Material sourceA(sourceShader);
    NLS::Render::Resources::Material sourceB(sourceShader);
    sourceA.path = ":Materials/SharedDeferredDecal.nmat";
    sourceB.path = sourceA.path;
    const NLS::Maths::Vector4 colorA{0.1f, 0.2f, 0.3f, 0.4f};
    const NLS::Maths::Vector4 colorB{0.8f, 0.7f, 0.6f, 0.5f};
    sourceA.SetRawParameter("_BaseColor", colorA);
    sourceB.SetRawParameter("_BaseColor", colorB);

    auto* firstA =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            sourceA);
    auto* resolvedB =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            sourceB);
    auto* secondA =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            sourceA);

    ASSERT_NE(firstA, nullptr);
    ASSERT_NE(resolvedB, nullptr);
    ASSERT_NE(secondA, nullptr);
    EXPECT_NE(firstA, resolvedB);
    EXPECT_EQ(secondA, firstA);
    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).size(),
        2u);
    const auto* resolvedColor = secondA->GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(resolvedColor, nullptr);
    ASSERT_EQ(resolvedColor->type(), typeid(NLS::Maths::Vector4));
    const auto& actualColor = std::any_cast<const NLS::Maths::Vector4&>(*resolvedColor);
    EXPECT_FLOAT_EQ(actualColor.x, colorA.x);
    EXPECT_FLOAT_EQ(actualColor.y, colorA.y);
    EXPECT_FLOAT_EQ(actualColor.z, colorA.z);
    EXPECT_FLOAT_EQ(actualColor.w, colorA.w);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
    NLS::Render::Resources::Shader::DestroyForTesting(fallbackShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalFallbackSynchronizesDirectTexturePointersAndPaths)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* fallbackShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalFallback.hlsl");
    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSource.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    ASSERT_NE(sourceShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    auto firstTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<TestTexture>(textureDesc),
        1u,
        1u);
    auto secondTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<TestTexture>(textureDesc),
        1u,
        1u);
    ASSERT_NE(firstTexture, nullptr);
    ASSERT_NE(secondTexture, nullptr);

    NLS::Render::Resources::Material source(sourceShader);
    source.SetRawParameter("u_AlbedoMap", firstTexture.get());
    source.SetTextureResourcePath("u_AlbedoMap", ":Textures/DirectAlbedoA.ntx");
    source.SetRawParameter("u_OpacityMap", firstTexture.get());
    source.SetTextureResourcePath("u_OpacityMap", ":Textures/DirectOpacityA.ntx");

    auto* fallback =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);
    ASSERT_NE(fallback, nullptr);
    const auto expectTexture = [&](const char* name, NLS::Render::Resources::Texture2D* expected, const char* path)
    {
        const auto* value = fallback->GetParameterBlock().TryGet(name);
        ASSERT_NE(value, nullptr);
        ASSERT_EQ(value->type(), typeid(NLS::Render::Resources::Texture2D*));
        EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*value), expected);
        EXPECT_EQ(fallback->GetTextureResourcePath(name), path);
    };
    expectTexture("u_AlbedoMap", firstTexture.get(), ":Textures/DirectAlbedoA.ntx");
    expectTexture("u_OpacityMap", firstTexture.get(), ":Textures/DirectOpacityA.ntx");

    source.SetRawParameter("u_AlbedoMap", secondTexture.get());
    source.SetTextureResourcePath("u_AlbedoMap", ":Textures/DirectAlbedoB.ntx");
    source.SetRawParameter("u_OpacityMap", secondTexture.get());
    source.SetTextureResourcePath("u_OpacityMap", ":Textures/DirectOpacityB.ntx");
    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source),
        fallback);
    expectTexture("u_AlbedoMap", secondTexture.get(), ":Textures/DirectAlbedoB.ntx");
    expectTexture("u_OpacityMap", secondTexture.get(), ":Textures/DirectOpacityB.ntx");

    source.SetRawParameter("u_AlbedoMap", static_cast<NLS::Render::Resources::Texture2D*>(nullptr));
    source.ClearTextureResourcePath("u_AlbedoMap");
    source.SetRawParameter("u_OpacityMap", static_cast<NLS::Render::Resources::Texture2D*>(nullptr));
    source.ClearTextureResourcePath("u_OpacityMap");
    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source),
        fallback);
    expectTexture("u_AlbedoMap", nullptr, "");
    expectTexture("u_OpacityMap", nullptr, "");

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
    NLS::Render::Resources::Shader::DestroyForTesting(fallbackShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalFallbackAliasPrecedenceOwnsPointerAndEmptyPath)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* fallbackShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalFallback.hlsl");
    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSource.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    ASSERT_NE(sourceShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);

    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    auto directTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<TestTexture>(textureDesc),
        1u,
        1u);
    auto aliasTexture = NLS::Render::Resources::Texture2D::WrapExternal(
        std::make_shared<TestTexture>(textureDesc),
        1u,
        1u);
    ASSERT_NE(directTexture, nullptr);
    ASSERT_NE(aliasTexture, nullptr);

    NLS::Render::Resources::Material source(sourceShader);
    source.SetRawParameter("u_AlbedoMap", directTexture.get());
    source.SetTextureResourcePath("u_AlbedoMap", ":Textures/DirectAlbedo.ntx");
    source.SetRawParameter("u_OpacityMap", directTexture.get());
    source.SetTextureResourcePath("u_OpacityMap", ":Textures/DirectOpacity.ntx");
    source.SetRawParameter("_BaseMap", aliasTexture.get());
    source.SetTextureResourcePath("_BaseMap", ":Textures/AliasAlbedo.ntx");
    source.SetRawParameter("_OpacityMap", aliasTexture.get());
    source.SetTextureResourcePath("_OpacityMap", ":Textures/AliasOpacity.ntx");

    auto* fallback =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);
    ASSERT_NE(fallback, nullptr);
    const auto expectTexture = [&](const char* name, NLS::Render::Resources::Texture2D* expected, const char* path)
    {
        const auto* value = fallback->GetParameterBlock().TryGet(name);
        ASSERT_NE(value, nullptr);
        ASSERT_EQ(value->type(), typeid(NLS::Render::Resources::Texture2D*));
        EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*value), expected);
        EXPECT_EQ(fallback->GetTextureResourcePath(name), path);
    };
    expectTexture("u_AlbedoMap", aliasTexture.get(), ":Textures/AliasAlbedo.ntx");
    expectTexture("u_OpacityMap", aliasTexture.get(), ":Textures/AliasOpacity.ntx");

    source.ClearTextureResourcePath("_BaseMap");
    source.ClearTextureResourcePath("_OpacityMap");
    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source),
        fallback);
    expectTexture("u_AlbedoMap", aliasTexture.get(), "");
    expectTexture("u_OpacityMap", aliasTexture.get(), "");

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
    NLS::Render::Resources::Shader::DestroyForTesting(fallbackShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalFallbackPreservesTransparentBlackBaseColor)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* fallbackShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalFallback.hlsl");
    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSource.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    ASSERT_NE(sourceShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);

    NLS::Render::Resources::Material source(sourceShader);
    source.SetRawParameter("_BaseColor", NLS::Maths::Vector4{0.0f, 0.0f, 0.0f, 0.0f});
    auto* fallback =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);
    ASSERT_NE(fallback, nullptr);
    const auto* albedo = fallback->GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedo, nullptr);
    ASSERT_EQ(albedo->type(), typeid(NLS::Maths::Vector4));
    const auto& actual = std::any_cast<const NLS::Maths::Vector4&>(*albedo);
    EXPECT_FLOAT_EQ(actual.x, 0.0f);
    EXPECT_FLOAT_EQ(actual.y, 0.0f);
    EXPECT_FLOAT_EQ(actual.z, 0.0f);
    EXPECT_FLOAT_EQ(actual.w, 0.0f);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
    NLS::Render::Resources::Shader::DestroyForTesting(fallbackShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalBuiltInFallbackDefaultsMissingAlbedoToWhite)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);

    auto* fallbackShader =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer);
    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSourceWithoutAlbedo.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    ASSERT_NE(sourceShader, nullptr);

    NLS::Render::Resources::Material source(sourceShader);
    auto* fallback =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source);
    ASSERT_NE(fallback, nullptr);
    const auto* albedo = fallback->GetParameterBlock().TryGet("u_Albedo");
    ASSERT_NE(albedo, nullptr);
    ASSERT_EQ(albedo->type(), typeid(NLS::Maths::Vector4));
    const auto& actual = std::any_cast<const NLS::Maths::Vector4&>(*albedo);
    EXPECT_FLOAT_EQ(actual.x, 1.0f);
    EXPECT_FLOAT_EQ(actual.y, 1.0f);
    EXPECT_FLOAT_EQ(actual.z, 1.0f);
    EXPECT_FLOAT_EQ(actual.w, 1.0f);

    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalResolutionFailsClosedWithoutFallbackShader)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSource.hlsl");
    ASSERT_NE(sourceShader, nullptr);
    NLS::Render::Resources::Material source(sourceShader);

    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source),
        nullptr);
    EXPECT_TRUE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).empty());

    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
}

TEST(RendererFrameObjectBindingTests, DeferredPipelineReadinessDoesNotRequireOptionalDecalFallbackShader)
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
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::EnsureGBufferTargets(renderer, 64u, 64u);

    auto* decalShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer);
    ASSERT_NE(decalShader, nullptr);
    EXPECT_TRUE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::HasDeferredThreadedPipelineResourcesForTesting(
            renderer));

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    EXPECT_TRUE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::HasDeferredThreadedPipelineResourcesForTesting(
            renderer));
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, decalShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalFrameBoundaryRetainsResolveCacheAndResetsFrameStats)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* fallbackShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/DeferredDecalFallback.hlsl");
    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSource.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    ASSERT_NE(sourceShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);
    NLS::Render::Resources::Material source(sourceShader);
    ASSERT_NE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source),
        nullptr);
    ASSERT_NE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source),
        nullptr);
    ASSERT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveCacheSize(
            renderer),
        1u);
    ASSERT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveHitCount(
            renderer),
        1u);

    NLS::Engine::SceneSystem::Scene scene;
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    renderer.BeginFrame(frameDescriptor);
    renderer.EndFrame();

    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveCacheSize(
            renderer),
        1u);
    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveHitCount(
            renderer),
        0u);
    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveMissCount(
            renderer),
        0u);
    EXPECT_EQ(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).size(),
        1u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
    NLS::Render::Resources::Shader::DestroyForTesting(fallbackShader);
}

TEST(RendererFrameObjectBindingTests, DeferredDecalShaderDestroyReleasesFallbackMaterialsFirst)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);

    auto* fallbackShader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
        "App/Assets/Engine/Shaders/DeferredDecal.hlsl");
    auto* sourceShader = NLS::Render::Resources::Shader::CreateForTesting(
        "Tests/Shaders/LegacyDecalSource.hlsl");
    ASSERT_NE(fallbackShader, nullptr);
    ASSERT_NE(sourceShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);
    NLS::Render::Resources::Material source(sourceShader);
    ASSERT_NE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
            renderer,
            source),
        nullptr);
    ASSERT_FALSE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).empty());

    bool observedBeforeDestroy = false;
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShaderDestroyProbe(
        renderer,
        [&]()
        {
            observedBeforeDestroy = true;
            EXPECT_TRUE(
                NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(renderer).empty());
            EXPECT_EQ(
                NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveCacheSize(
                    renderer),
                0u);
            EXPECT_EQ(
                NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer),
                fallbackShader);
        });
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ReleaseDeferredDecalPipelineResourcesForTesting(renderer);

    EXPECT_TRUE(observedBeforeDestroy);
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer), nullptr);
    NLS::Render::Resources::Shader::DestroyForTesting(sourceShader);
}

TEST(RendererFrameObjectBindingTests, ThreadedDeferredDecalDrawUsesDeferredDecalPassAndPipelineState)
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

    NLS::Render::Buffers::Framebuffer outputBuffer(64u, 64u);
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);
    NLS::Render::Resources::Mesh sceneMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{0.0f, 0.0f, 0.0f}, 1.0f});

    static const std::string shaderPath = "Tests/Shaders/DeferredDecalRouting.shader";
    auto* forwardPassShader = CreateTestShaderLabGraphicsPass(
        shaderPath,
        "shader:DeferredDecalRouting/Forward#0",
        "Forward");
    auto* gbufferPassShader = CreateTestShaderLabGraphicsPass(
        shaderPath,
        "shader:DeferredDecalRouting/GBuffer#1",
        "GBuffer");
    auto* decalPassShader = CreateTestShaderLabGraphicsPass(
        shaderPath,
        "shader:DeferredDecalRouting/DeferredDecal#2",
        "DeferredDecal");
    ASSERT_NE(forwardPassShader, nullptr);
    ASSERT_NE(gbufferPassShader, nullptr);
    ASSERT_NE(decalPassShader, nullptr);

    NLS::Render::Resources::Material decalMaterial(forwardPassShader);
    decalMaterial.SetShaderLabSourcePath(shaderPath);
    decalMaterial.RegisterShaderLabPassShader(forwardPassShader);
    decalMaterial.RegisterShaderLabPassShader(gbufferPassShader);
    decalMaterial.RegisterShaderLabPassShader(decalPassShader);
    decalMaterial.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Decal);

    NLS::Engine::SceneSystem::Scene scene;
    AddDecalMeshToScene(scene, sceneMesh, decalMaterial);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    auto* fallbackShader =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer);
    ASSERT_NE(fallbackShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* publishedSlot = lifecycle->PeekSlot(0u);
    ASSERT_NE(publishedSlot, nullptr);
    ASSERT_TRUE(publishedSlot->snapshot.has_value());
    const auto& snapshot = publishedSlot->snapshot.value();
    ASSERT_EQ(snapshot.visibleDecalDrawCount, 1u);

    const auto expectedFragmentShader = decalPassShader->GetOrCreateExplicitShaderModule(
        explicitDevice,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel);
    ASSERT_NE(expectedFragmentShader, nullptr);
    const auto decalCommand = std::find_if(
        snapshot.recordedDrawCommands.begin(),
        snapshot.recordedDrawCommands.end(),
        [&](const auto& command)
        {
            return command.pipeline != nullptr &&
                command.pipeline->GetDesc().fragmentShader == expectedFragmentShader;
        });
    EXPECT_NE(decalCommand, snapshot.recordedDrawCommands.end())
        << "Threaded decal capture must select the material's DeferredDecal LightMode pass.";
    if (decalCommand != snapshot.recordedDrawCommands.end())
        ExpectDeferredDecalPipelineState(decalCommand->pipeline->GetDesc());

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(decalPassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferPassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forwardPassShader));
}

TEST(RendererFrameObjectBindingTests, ImmediateDeferredDecalDrawUsesDeferredDecalPassAndPipelineState)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 121u;
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    NLS::Render::Buffers::Framebuffer outputBuffer(64u, 64u);
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);
    NLS::Render::Resources::Mesh sceneMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{0.0f, 0.0f, 0.0f}, 1.0f});

    static const std::string shaderPath = "Tests/Shaders/DeferredDecalRouting.shader";
    auto* forwardPassShader = CreateTestShaderLabGraphicsPass(
        shaderPath,
        "shader:DeferredDecalRouting/Forward#0",
        "Forward");
    auto* gbufferPassShader = CreateTestShaderLabGraphicsPass(
        shaderPath,
        "shader:DeferredDecalRouting/GBuffer#1",
        "GBuffer");
    auto* decalPassShader = CreateTestShaderLabGraphicsPass(
        shaderPath,
        "shader:DeferredDecalRouting/DeferredDecal#2",
        "DeferredDecal");
    ASSERT_NE(forwardPassShader, nullptr);
    ASSERT_NE(gbufferPassShader, nullptr);
    ASSERT_NE(decalPassShader, nullptr);

    NLS::Render::Resources::Material decalMaterial(forwardPassShader);
    decalMaterial.SetShaderLabSourcePath(shaderPath);
    decalMaterial.RegisterShaderLabPassShader(forwardPassShader);
    decalMaterial.RegisterShaderLabPassShader(gbufferPassShader);
    decalMaterial.RegisterShaderLabPassShader(decalPassShader);
    decalMaterial.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Decal);

    NLS::Engine::SceneSystem::Scene scene;
    AddDecalMeshToScene(scene, sceneMesh, decalMaterial);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    auto* fallbackShader =
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer);
    ASSERT_NE(fallbackShader, nullptr);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.DrawFrame());
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto expectedFragmentShader = decalPassShader->GetOrCreateExplicitShaderModule(
        explicitDevice,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel);
    ASSERT_NE(expectedFragmentShader, nullptr);
    const auto decalPipeline = std::find_if(
        explicitDevice->graphicsPipelineDescs.begin(),
        explicitDevice->graphicsPipelineDescs.end(),
        [&](const auto& desc)
        {
            return desc.fragmentShader == expectedFragmentShader;
        });
    EXPECT_NE(decalPipeline, explicitDevice->graphicsPipelineDescs.end())
        << "Immediate decal callback must select the material's DeferredDecal LightMode pass.";
    if (decalPipeline != explicitDevice->graphicsPipelineDescs.end())
        ExpectDeferredDecalPipelineState(*decalPipeline);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(decalPassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferPassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forwardPassShader));
}

TEST(RendererFrameObjectBindingTests, ThreadedDeferredDecalMissingFallbackSkipsOnlyUnresolvedDecal)
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

    NLS::Render::Buffers::Framebuffer outputBuffer(64u, 64u);
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);
    auto* gbufferShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferShader(renderer);
    auto* decalShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer);
    ASSERT_NE(gbufferShader, nullptr);
    ASSERT_NE(decalShader, nullptr);

    NLS::Render::Resources::Mesh sceneMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{0.0f, 0.0f, 0.0f}, 1.0f});
    NLS::Render::Resources::Material decalMaterial(gbufferShader);
    decalMaterial.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Decal);
    NLS::Engine::SceneSystem::Scene scene;
    AddDecalMeshToScene(scene, sceneMesh, decalMaterial);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_EQ(slot->snapshot->visibleDecalDrawCount, 0u);
    EXPECT_EQ(slot->snapshot->recordedDrawCommands.size(), 1u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, decalShader);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
}

TEST(RendererFrameObjectBindingTests, ImmediateDeferredDecalMissingFallbackSkipsOnlyUnresolvedDecal)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 122u;
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);

    NLS::Render::Buffers::Framebuffer outputBuffer(64u, 64u);
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);
    auto* gbufferShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferShader(renderer);
    auto* decalShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer);
    ASSERT_NE(gbufferShader, nullptr);
    ASSERT_NE(decalShader, nullptr);

    NLS::Render::Resources::Mesh sceneMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{0.0f, 0.0f, 0.0f}, 1.0f});
    NLS::Render::Resources::Material decalMaterial(gbufferShader);
    decalMaterial.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Decal);
    NLS::Engine::SceneSystem::Scene scene;
    AddDecalMeshToScene(scene, sceneMesh, decalMaterial);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.DrawFrame());
    ASSERT_NO_THROW(renderer.EndFrame());

    EXPECT_FALSE(explicitDevice->graphicsPipelineDescs.empty());
    const auto unresolvedDecalFragment = gbufferShader->GetOrCreateExplicitShaderModule(
        explicitDevice,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel);
    ASSERT_NE(unresolvedDecalFragment, nullptr);
    EXPECT_EQ(
        std::count_if(
            explicitDevice->graphicsPipelineDescs.begin(),
            explicitDevice->graphicsPipelineDescs.end(),
            [&](const auto& desc)
            {
                return desc.fragmentShader == unresolvedDecalFragment;
            }),
        0u);
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, decalShader);
}

TEST(RendererFrameObjectBindingTests, ThreadedDeferredFrameWithoutDecalsPublishesWhenFallbackShaderMissing)
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

    NLS::Render::Buffers::Framebuffer outputBuffer(64u, 64u);
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver);
    auto* gbufferShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferShader(renderer);
    auto* decalShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetDeferredDecalShader(renderer);
    ASSERT_NE(gbufferShader, nullptr);
    ASSERT_NE(decalShader, nullptr);

    NLS::Render::Resources::Mesh sceneMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{0.0f, 0.0f, 0.0f}, 1.0f});
    NLS::Render::Resources::Material opaqueMaterial(gbufferShader);
    NLS::Engine::SceneSystem::Scene scene;
    AddOpaqueMeshToScene(scene, sceneMesh, opaqueMaterial);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });
    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, nullptr);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 64u;
    frameDescriptor.renderHeight = 64u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    const auto* lifecycle = NLS::Render::Context::DriverTestAccess::GetThreadedRenderingLifecycle(driver);
    ASSERT_NE(lifecycle, nullptr);
    const auto* slot = lifecycle->PeekSlot(0u);
    ASSERT_NE(slot, nullptr);
    ASSERT_TRUE(slot->snapshot.has_value());
    EXPECT_EQ(slot->snapshot->visibleOpaqueDrawCount, 1u);
    EXPECT_EQ(slot->snapshot->visibleDecalDrawCount, 0u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, decalShader);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
}

TEST(RendererFrameObjectBindingTests, ThreadedDeferredFramePublishFailsClosedWhenLightingDrawIsMissing)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 1u;
    snapshot.visibleDecalDrawCount = 1u;
    snapshot.visibleTransparentDrawCount = 1u;
    snapshot.visibleSkyboxDrawCount = 1u;

    EXPECT_TRUE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ShouldSkipThreadedDeferredFramePublishForTesting(
            snapshot,
            1u,
            1u,
            0u,
            1u));
    EXPECT_FALSE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ShouldSkipThreadedDeferredFramePublishForTesting(
            snapshot,
            1u,
            1u,
            1u,
            1u));
}

TEST(RendererFrameObjectBindingTests, ThreadedDeferredFramePublishFailsClosedWhenAnySceneQueueIsPartial)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 2u;
    snapshot.visibleDecalDrawCount = 2u;
    snapshot.visibleTransparentDrawCount = 2u;

    EXPECT_TRUE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ShouldSkipThreadedDeferredFramePublishForTesting(
            snapshot,
            1u,
            2u,
            1u,
            2u));
    EXPECT_TRUE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ShouldSkipThreadedDeferredFramePublishForTesting(
            snapshot,
            2u,
            1u,
            1u,
            2u));
    EXPECT_TRUE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ShouldSkipThreadedDeferredFramePublishForTesting(
            snapshot,
            2u,
            2u,
            1u,
            1u));
    EXPECT_FALSE(
        NLS::Engine::Rendering::DeferredSceneRendererTestAccess::ShouldSkipThreadedDeferredFramePublishForTesting(
            snapshot,
            2u,
            2u,
            1u,
            2u));
}

TEST(RendererFrameObjectBindingTests, ThreadedDeferredSnapshotSynchronizationClampsTransparentToRecordedRemainder)
{
    NLS::Render::Context::FrameSnapshot snapshot;
    snapshot.hasSceneInput = true;
    snapshot.visibleOpaqueDrawCount = 2u;
    snapshot.visibleDecalDrawCount = 2u;
    snapshot.visibleTransparentDrawCount = 2u;
    snapshot.recordedDrawCommands.resize(4u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SynchronizeThreadedDeferredSnapshotForTesting(
        snapshot,
        2u,
        1u,
        1u,
        2u);

    EXPECT_EQ(snapshot.visibleOpaqueDrawCount, 2u);
    EXPECT_EQ(snapshot.visibleDecalDrawCount, 1u);
    EXPECT_EQ(snapshot.visibleTransparentDrawCount, 0u);
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

TEST(RendererFrameObjectBindingTests, MaterialTextureResourcePathsAdvanceBindingRevision)
{
    NLS::Render::Resources::Material material;
    const auto initialRevision = material.GetBindingRevision();

    material.SetTextureResourcePath("u_AlbedoMap", ":Textures/First.ntx");
    const auto firstPathRevision = material.GetBindingRevision();
    EXPECT_GT(firstPathRevision, initialRevision);

    material.SetTextureResourcePath("u_AlbedoMap", ":Textures/First.ntx");
    EXPECT_EQ(material.GetBindingRevision(), firstPathRevision);

    material.SetTextureResourcePath("u_AlbedoMap", ":Textures/Second.ntx");
    const auto changedPathRevision = material.GetBindingRevision();
    EXPECT_GT(changedPathRevision, firstPathRevision);

    material.ClearTextureResourcePath("u_AlbedoMap");
    EXPECT_GT(material.GetBindingRevision(), changedPathRevision);
}

TEST(RendererFrameObjectBindingTests, MaterialPipelineStateOverridesEqualityDistinguishesExplicitEmptyColorFormats)
{
    NLS::Render::Resources::MaterialPipelineStateOverrides implicitFormats;
    NLS::Render::Resources::MaterialPipelineStateOverrides explicitEmptyFormats;
    explicitEmptyFormats.SetColorFormats(std::span<const NLS::Render::RHI::TextureFormat>{});

    EXPECT_NE(implicitFormats, explicitEmptyFormats);
    EXPECT_NE(implicitFormats.GetHash(), explicitEmptyFormats.GetHash());
}

TEST(RendererFrameObjectBindingTests, RhiDeviceCacheIdentityDoesNotReusePointerIdentity)
{
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    auto secondDevice = std::make_shared<TestExplicitDevice>();

    EXPECT_NE(firstDevice->GetCacheIdentity(), 0u);
    EXPECT_NE(secondDevice->GetCacheIdentity(), 0u);
    EXPECT_NE(firstDevice->GetCacheIdentity(), secondDevice->GetCacheIdentity());
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(RendererFrameObjectBindingTests, PreparedDrawReusesMalformedShaderNegativeCacheAcrossDrawablesAndFrames)
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
    frameContext.frameIndex = 39u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/PreparedMalformedIndexed.hlsl",
        MakeIndexedObjectDataReflection(true, sizeof(uint32_t)),
        "shader:prepared-malformed-indexed");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    material.SetGPUInstances(1);

    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = true;
    overrides.depthWrite = true;
    overrides.colorWrite = true;
    overrides.hasDepthAttachment = false;

    NLS::Render::Entities::Drawable firstDrawable;
    firstDrawable.material = &material;
    firstDrawable.mesh = &mesh;
    firstDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    firstDrawable.instanceCount = 1u;
    firstDrawable.vertexCount = 3u;

    auto secondDrawable = firstDrawable;
    secondDrawable.vertexStart = 1u;
    secondDrawable.vertexCount = 2u;

    renderer.BeginFrame(frameDescriptor);
    EXPECT_FALSE(renderer.CaptureDrawForTesting(
        firstDrawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_FALSE(renderer.CaptureDrawForTesting(
        secondDrawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    frameContext.frameIndex = 40u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    EXPECT_FALSE(renderer.CaptureDrawForTesting(
        firstDrawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    EXPECT_EQ(explicitDevice->pipelineLayoutCreateCalls, 0u);
    EXPECT_EQ(material.GetIndexedObjectDataShaderValidationCountForTesting(), 1u);
    ASSERT_EQ(material.GetLastExplicitBindingDiagnostics().size(), 1u);
    EXPECT_EQ(material.GetLastExplicitBindingDiagnostics().front().bindingName, "ObjectIndexConstants");
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}
#endif

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheReusesPipelineMaterialAndMeshOnly)
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
    frameContext.frameIndex = 41u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    auto provider = std::make_unique<PreparedBindingProbeProvider>(renderer);
    auto* providerPtr = provider.get();
    renderer.SetFrameObjectBindingProvider(std::move(provider));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    renderer.BeginFrame(frameDescriptor);

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    material.SetGPUInstances(7);
    NLS::Render::Resources::Material overrideMaterial(shader);
    overrideMaterial.SetGPUInstances(7);

    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = true;
    overrides.depthWrite = true;
    overrides.colorWrite = true;
    overrides.hasDepthAttachment = false;

    NLS::Render::Entities::Drawable firstDrawable;
    firstDrawable.material = &material;
    firstDrawable.mesh = &mesh;
    firstDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    firstDrawable.instanceCount = 2u;
    firstDrawable.vertexStart = 0u;
    firstDrawable.vertexCount = 2u;
    firstDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f }),
        NLS::Maths::Matrix4::Identity
    });
    firstDrawable.AddDescriptor<CopyTrackedDrawableDescriptor>(CopyTrackedDrawableDescriptor{ 17 });

    NLS::Render::Entities::Drawable secondDrawable;
    secondDrawable.material = &material;
    secondDrawable.mesh = &mesh;
    secondDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    secondDrawable.instanceCount = 5u;
    secondDrawable.vertexStart = 1u;
    secondDrawable.vertexCount = 1u;
    secondDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f }),
        NLS::Maths::Matrix4::Identity
    });

    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        firstDrawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        secondDrawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    CopyTrackedDrawableDescriptor::copyCount = 0u;
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        firstDrawable,
        overrideMaterial,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_EQ(CopyTrackedDrawableDescriptor::copyCount, 0u);
    EXPECT_EQ(providerPtr->lastPreparedMaterial, &overrideMaterial);

    const auto snapshot = renderer.CaptureSnapshotForTesting();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_EQ(snapshot->recordedDrawCommands.size(), 3u);
    EXPECT_EQ(snapshot->recordedDrawCommands[0].instanceCount, 2u);
    EXPECT_EQ(snapshot->recordedDrawCommands[0].vertexStart, 0u);
    EXPECT_EQ(snapshot->recordedDrawCommands[0].vertexCount, 2u);
    EXPECT_EQ(snapshot->recordedDrawCommands[1].instanceCount, 5u);
    EXPECT_EQ(snapshot->recordedDrawCommands[1].vertexStart, 1u);
    EXPECT_EQ(snapshot->recordedDrawCommands[1].vertexCount, 1u);
    EXPECT_NE(snapshot->recordedDrawCommands[0].objectBindingSet, nullptr);
    EXPECT_NE(snapshot->recordedDrawCommands[1].objectBindingSet, nullptr);
    EXPECT_NE(snapshot->recordedDrawCommands[0].objectBindingSet, snapshot->recordedDrawCommands[1].objectBindingSet);
    EXPECT_EQ(snapshot->recordedDrawCommands[0].pipeline, snapshot->recordedDrawCommands[1].pipeline);
    EXPECT_EQ(snapshot->recordedDrawCommands[0].materialBindingSet, snapshot->recordedDrawCommands[1].materialBindingSet);
    EXPECT_EQ(snapshot->recordedDrawCommands[0].mesh, snapshot->recordedDrawCommands[1].mesh);
    EXPECT_NE(snapshot->recordedDrawCommands[0].frameBindingSet, nullptr);
    EXPECT_EQ(snapshot->recordedDrawCommands[0].frameBindingSet, snapshot->recordedDrawCommands[1].frameBindingSet);
    EXPECT_EQ(snapshot->recordedDrawCommands[0].frameBindingSet, snapshot->recordedDrawCommands[2].frameBindingSet);
    EXPECT_EQ(snapshot->recordedDrawCommands[2].instanceCount, 2u);
    EXPECT_EQ(snapshot->recordedDrawCommands[2].mesh, snapshot->recordedDrawCommands[0].mesh);
    EXPECT_NE(snapshot->recordedDrawCommands[2].materialBindingSet, snapshot->recordedDrawCommands[0].materialBindingSet);
    EXPECT_EQ(providerPtr->prepareDrawCount, 3u);
    EXPECT_EQ(providerPtr->captureFrameBindingSetCount, 1u);
    EXPECT_EQ(providerPtr->captureBindingSetCount, 3u);

    renderer.EndFrame();
    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.preparedRecordedDrawStaticBaseCacheMissCount, 2u);
    EXPECT_EQ(frameInfo.preparedRecordedDrawStaticBaseCacheHitCount, 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCachePersistsAcrossFramesUntilMaterialDirty)
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
    frameContext.frameIndex = 51u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    auto provider = std::make_unique<PreparedBindingProbeProvider>(renderer);
    auto* providerPtr = provider.get();
    renderer.SetFrameObjectBindingProvider(std::move(provider));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    material.SetGPUInstances(1);

    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = true;
    overrides.depthWrite = true;
    overrides.colorWrite = true;
    overrides.hasDepthAttachment = false;

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    drawable.instanceCount = 1u;
    drawable.vertexStart = 0u;
    drawable.vertexCount = 3u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f }),
        NLS::Maths::Matrix4::Identity
    });

    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    auto firstSnapshot = renderer.CaptureSnapshotForTesting();
    ASSERT_TRUE(firstSnapshot.has_value());
    ASSERT_EQ(firstSnapshot->recordedDrawCommands.size(), 1u);
    const auto firstPipeline = firstSnapshot->recordedDrawCommands[0].pipeline;
    const auto firstMaterialBinding = firstSnapshot->recordedDrawCommands[0].materialBindingSet;
    const auto firstMesh = firstSnapshot->recordedDrawCommands[0].mesh;
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);

    frameContext.frameIndex = 52u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    auto secondSnapshot = renderer.CaptureSnapshotForTesting();
    ASSERT_TRUE(secondSnapshot.has_value());
    ASSERT_EQ(secondSnapshot->recordedDrawCommands.size(), 1u);
    EXPECT_EQ(secondSnapshot->recordedDrawCommands[0].pipeline, firstPipeline);
    EXPECT_EQ(secondSnapshot->recordedDrawCommands[0].materialBindingSet, firstMaterialBinding);
    EXPECT_EQ(secondSnapshot->recordedDrawCommands[0].mesh, firstMesh);
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 1u);

#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Render::RHI::SamplerDesc samplerOverride;
    samplerOverride.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
    samplerOverride.magFilter = NLS::Render::RHI::TextureFilter::Nearest;
    material.SetSamplerOverride("u_MaterialSampler", samplerOverride);

    frameContext.frameIndex = 53u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);
#endif

    size_t expectedFrameCount = 2u;
#if defined(NLS_ENABLE_TEST_HOOKS)
    expectedFrameCount += 1u;
#endif
    EXPECT_EQ(providerPtr->prepareDrawCount, expectedFrameCount);
    EXPECT_EQ(providerPtr->captureBindingSetCount, expectedFrameCount);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheInvalidatesWhenExplicitDeviceChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.enableThreadedRendering = true;
    settings.threadedFrameSlotCount = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, firstDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 61u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(std::make_unique<PreparedBindingProbeProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = true;
    overrides.depthWrite = true;
    overrides.colorWrite = true;
    overrides.hasDepthAttachment = false;

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    drawable.instanceCount = 1u;
    drawable.vertexStart = 0u;
    drawable.vertexCount = 3u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity
    });

    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    const auto firstSnapshot = renderer.CaptureSnapshotForTesting();
    ASSERT_TRUE(firstSnapshot.has_value());
    ASSERT_EQ(firstSnapshot->recordedDrawCommands.size(), 1u);
    const auto firstPipeline = firstSnapshot->recordedDrawCommands[0].pipeline;
    const auto firstMaterialBinding = firstSnapshot->recordedDrawCommands[0].materialBindingSet;
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(firstDevice->graphicsPipelineCreateCalls, 1u);
    EXPECT_EQ(firstDevice->bindingSetCreateCalls, 1u);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    secondFrameContext.frameIndex = 62u;
    secondFrameContext.commandBuffer = nullptr;
    secondFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(secondFrameContext.descriptorAllocator, nullptr);
    secondFrameContext.descriptorAllocator->BeginFrame(secondFrameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    const auto secondSnapshot = renderer.CaptureSnapshotForTesting();
    ASSERT_TRUE(secondSnapshot.has_value());
    ASSERT_EQ(secondSnapshot->recordedDrawCommands.size(), 1u);
    EXPECT_NE(secondSnapshot->recordedDrawCommands[0].pipeline, firstPipeline);
    EXPECT_NE(secondSnapshot->recordedDrawCommands[0].materialBindingSet, firstMaterialBinding);
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);
    EXPECT_EQ(secondDevice->graphicsPipelineCreateCalls, 1u);
    EXPECT_EQ(secondDevice->bindingSetCreateCalls, 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheIsBoundedUnderUniqueMaterialChurn)
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
    frameContext.frameIndex = 71u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(std::make_unique<PreparedBindingProbeProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    drawable.instanceCount = 1u;
    drawable.vertexStart = 0u;
    drawable.vertexCount = 3u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity
    });

    const auto cacheLimit = RecordedDrawCacheProbeSceneRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting();
    auto hotMaterial = std::make_unique<NLS::Render::Resources::Material>(shader);
    std::vector<std::unique_ptr<NLS::Render::Resources::Material>> materials;
    materials.reserve(cacheLimit + 32u);

    renderer.BeginFrame(frameDescriptor);
    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = true;
    overrides.depthWrite = true;
    overrides.colorWrite = true;
    overrides.hasDepthAttachment = false;

    drawable.material = hotMaterial.get();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.ClearPreparedRecordedDrawStaticBaseCacheForTesting();

    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));

    for (size_t index = 0u; index < cacheLimit - 1u; ++index)
    {
        materials.push_back(std::make_unique<NLS::Render::Resources::Material>(shader));
        drawable.material = materials.back().get();
        ASSERT_TRUE(renderer.CaptureDrawForTesting(
            drawable,
            overrides,
            NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    }

    drawable.material = hotMaterial.get();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));

    for (size_t index = 0u; index < 32u; ++index)
    {
        materials.push_back(std::make_unique<NLS::Render::Resources::Material>(shader));
        drawable.material = materials.back().get();
        ASSERT_TRUE(renderer.CaptureDrawForTesting(
            drawable,
            overrides,
            NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    }

    drawable.material = hotMaterial.get();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    EXPECT_LE(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), cacheLimit);
    EXPECT_LE(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), cacheLimit);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 2u);

    const auto ageSweepBudget =
        RecordedDrawCacheProbeSceneRenderer::GetPreparedRecordedDrawStaticBaseCacheAgeSweepBudgetForTesting();
    const auto maxFrameAge =
        RecordedDrawCacheProbeSceneRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxFrameAgeForTesting();
    const auto sizeBeforeAgeSweep = renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting();
    ASSERT_GT(sizeBeforeAgeSweep, ageSweepBudget);
    EXPECT_EQ(
        renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(maxFrameAge + 1u),
        ageSweepBudget);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(),
        sizeBeforeAgeSweep - ageSweepBudget);

    while (renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting() != 0u)
    {
        const auto sizeBeforeSweep = renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting();
        const auto evictedCount = renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(1u);
        EXPECT_EQ(evictedCount, std::min(ageSweepBudget, sizeBeforeSweep));
        EXPECT_EQ(
            renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(),
            sizeBeforeSweep - evictedCount);
    }
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheHitDoesNotGrowStableIndex)
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
    frameContext.frameIndex = 81u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(std::make_unique<PreparedBindingProbeProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = true;
    overrides.depthWrite = true;
    overrides.colorWrite = true;
    overrides.hasDepthAttachment = false;

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    drawable.instanceCount = 1u;
    drawable.vertexStart = 0u;
    drawable.vertexCount = 3u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity
    });

    renderer.BeginFrame(frameDescriptor);
    for (uint32_t index = 0u; index < 32u; ++index)
    {
        ASSERT_TRUE(renderer.CaptureDrawForTesting(
            drawable,
            overrides,
            NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    }
    renderer.EndFrame();

    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheReplacesEntryWhenMeshContentChanges)
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
    frameContext.frameIndex = 101u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(std::make_unique<PreparedBindingProbeProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = true;
    overrides.depthWrite = true;
    overrides.colorWrite = true;
    overrides.hasDepthAttachment = false;

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    drawable.instanceCount = 1u;
    drawable.vertexStart = 0u;
    drawable.vertexCount = 3u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity
    });

    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    auto firstSnapshot = renderer.CaptureSnapshotForTesting();
    ASSERT_TRUE(firstSnapshot.has_value());
    ASSERT_EQ(firstSnapshot->recordedDrawCommands.size(), 1u);
    const auto firstMesh = firstSnapshot->recordedDrawCommands[0].mesh;
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);

    NLS::Render::Geometry::BoundingSphere updatedBounds {};
    updatedBounds.radius = 1.0f;
    mesh.Reload(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        updatedBounds);

    frameContext.frameIndex = 102u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    auto secondSnapshot = renderer.CaptureSnapshotForTesting();
    ASSERT_TRUE(secondSnapshot.has_value());
    ASSERT_EQ(secondSnapshot->recordedDrawCommands.size(), 1u);
    EXPECT_NE(secondSnapshot->recordedDrawCommands[0].mesh, firstMesh);
    renderer.EndFrame();

    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheSeparatesPipelineDimensions)
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
    frameContext.frameIndex = 111u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(std::make_unique<PreparedBindingProbeProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);

    NLS::Render::Resources::MaterialPipelineStateOverrides baseOverrides;
    baseOverrides.depthTest = true;
    baseOverrides.depthWrite = true;
    baseOverrides.colorWrite = true;
    baseOverrides.hasDepthAttachment = false;

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    drawable.instanceCount = 1u;
    drawable.vertexStart = 0u;
    drawable.vertexCount = 3u;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity
    });

    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        baseOverrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        baseOverrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));

    drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::LINES;
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        baseOverrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));

    drawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        baseOverrides,
        NLS::Render::Settings::EComparaisonAlgorithm::GREATER));

    auto blendOverrides = baseOverrides;
    blendOverrides.blending = true;
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        blendOverrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 4u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 4u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 4u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}

TEST(RendererFrameObjectBindingTests, ExplicitUniformBindingLayoutCacheIsDeviceAware)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, firstDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 91u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(16u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    NLS::Render::Buffers::UniformBuffer uniformBuffer(sizeof(float) * 4u);

    NLS::Render::Core::ABaseRenderer::ExplicitUniformBufferBindingDesc desc;
    desc.set = 2u;
    desc.registerSpace = 3u;
    desc.binding = 4u;
    desc.range = sizeof(float) * 4u;
    desc.entryName = "TestUniform";
    desc.layoutDebugName = "TestUniformLayout";
    desc.setDebugName = "TestUniformSet";
    desc.snapshotDebugName = "TestUniformSnapshot";

    ASSERT_NE(renderer.CreateExplicitUniformBufferBindingSet(uniformBuffer, desc), nullptr);
    ASSERT_NE(renderer.CreateExplicitUniformBufferBindingSet(uniformBuffer, desc), nullptr);
    EXPECT_EQ(firstDevice->bindingLayoutCreateCalls, 1u);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    secondFrameContext.frameIndex = 92u;
    secondFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(16u);
    ASSERT_NE(secondFrameContext.descriptorAllocator, nullptr);
    secondFrameContext.descriptorAllocator->BeginFrame(secondFrameContext.frameIndex);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, true);
    ASSERT_NE(renderer.CreateExplicitUniformBufferBindingSet(uniformBuffer, desc), nullptr);

    EXPECT_EQ(secondDevice->bindingLayoutCreateCalls, 1u);
    NLS::Render::Context::DriverTestAccess::SetExplicitFrameActive(driver, false);
}
#endif

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
    NLS::Render::Resources::Mesh sceneMesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* gbufferShader = NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferShader(renderer);
    ASSERT_NE(gbufferShader, nullptr);
    if (gbufferShader->GetCompiledArtifacts().empty())
        GTEST_SKIP() << "Deferred threaded offscreen packaging requires compiled shader artifacts.";
    NLS::Render::Resources::Material sceneMaterial;
    sceneMaterial.SetShader(gbufferShader);

    NLS::Engine::SceneSystem::Scene scene;
    auto& meshActor = scene.CreateGameObject("MeshActor");
    auto* meshFilter = meshActor.AddComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(meshFilter, nullptr);
    meshFilter->SetMesh(&sceneMesh);
    auto* meshRenderer = meshActor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshRenderer, nullptr);
    meshRenderer->FillWithMaterial(sceneMaterial);
    meshActor.GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});
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
    EXPECT_EQ(
        retiredSlot->renderScenePackage->extractedTextures.front(),
        outputBuffer.GetExplicitTextureHandle());
}
