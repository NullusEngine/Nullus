#include <gtest/gtest.h>
#include "Tests/RenderingThreadTestHelpers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <limits>
#include <span>
#include <stdexcept>
#include <thread>
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
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/FrameLatencyController.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/Context/RenderCommandFence.h"
#include "Rendering/Buffers/UniformBuffer.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Data/ObjectDataLimits.h"
#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/Entities/Camera.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
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
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"

namespace
{
    bool NativeDxcUnavailableForFrameObjectBindingTests()
    {
        static const bool unavailable =
            NLS::Render::ShaderCompiler::GetCurrentShaderCompilerToolchainIdentity().compilerPath ==
            "unavailable";
        return unavailable;
    }

#define NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE()      \
    do                                                                \
    {                                                                 \
        if (NativeDxcUnavailableForFrameObjectBindingTests())         \
            GTEST_SKIP() << "Native dxc is unavailable in this environment."; \
    } while (false)

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
        void BindBindingSet(
            const uint32_t setIndex,
            const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override
        {
            if (setIndex == NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet)
                lastObjectBindingSet = bindingSet;
        }
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
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> lastObjectBindingSet;
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
        void Signal() { m_signaled = true; }
        bool Wait(const uint64_t timeoutNanoseconds = 0u) override
        {
            m_lastWaitTimeoutNanoseconds = timeoutNanoseconds;
            ++m_waitCount;
            if (m_waitSucceeds)
                m_signaled = true;
            return m_waitSucceeds;
        }
        void SetWaitSucceeds(const bool waitSucceeds) { m_waitSucceeds = waitSucceeds; }
        uint64_t GetLastWaitTimeoutNanoseconds() const { return m_lastWaitTimeoutNanoseconds; }
        uint64_t GetWaitCount() const { return m_waitCount; }

    private:
        std::string m_debugName;
        bool m_signaled = true;
        bool m_waitSucceeds = true;
        uint64_t m_lastWaitTimeoutNanoseconds = 0u;
        uint64_t m_waitCount = 0u;
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
            const NLS::Render::Context::RenderFramePacket& packet,
            const NLS::Render::Context::RenderSceneBuildData& scene) const
        {
            return NLS::Render::Context::BuildRenderScenePackage(packet, scene);
        }

        std::shared_ptr<const NLS::Engine::Rendering::SceneRenderStore> CaptureAndDropSceneView(
            const NLS::Render::FrameGraph::FrozenFrameDescriptor& frame)
        {
            auto input = BeginSceneParse(frame);
            auto store = input.scenes.front().syncRequest.proxyStore;
            ReleaseUnresolvedSceneParseCapture(input);
            return store;
        }
    };

    class RecordedDrawCacheProbeSceneRenderer final : public NLS::Engine::Rendering::BaseSceneRenderer
    {
    public:
        using PreparedDrawForTesting = PreparedRecordedDraw;
        using PreparedFrameScopeForTesting = PreparedFrameScope;

        explicit RecordedDrawCacheProbeSceneRenderer(NLS::Render::Context::Driver& driver)
            : BaseSceneRenderer(driver)
        {
        }

        void BeginFrame(const NLS::Render::Data::FrameDescriptor& frameDescriptor) override
        {
            NLS::Render::Core::CompositeRenderer::BeginFrame(frameDescriptor);
        }

        NLS::Render::Context::RenderSceneBuildCommand BuildRenderSceneCommand(
            const NLS::Render::Context::RenderFramePacket&) override
        {
            // This probe validates caches without native submission buffers.
            return {};
        }

        bool PrepareDrawResourcesForTesting(const Drawable& drawable, PreparedDrawForTesting& outDraw) const
        {
            NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
            overrides.hasDepthAttachment = false;
            return NLS::Render::Core::ABaseRenderer::PrepareRecordedDraw(
                drawable, overrides, NLS::Render::Settings::EComparaisonAlgorithm::LESS, "Forward", outDraw);
        }

        bool PrepareDrawResourcesForTesting(
            PipelineState pipelineState, const Drawable& drawable, PreparedDrawForTesting& outDraw) const
        {
            return NLS::Render::Core::ABaseRenderer::PrepareRecordedDraw(pipelineState, drawable, outDraw);
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
            NLS::Render::Resources::MaterialPipelineStateOverrides overrides,
            const NLS::Render::Settings::EComparaisonAlgorithm depthCompareOverride,
            const std::string_view lightMode)
        {
            PreparedRecordedDraw preparedDraw;
            return CaptureThreadedPreparedDraw(
                drawable,
                std::move(overrides),
                depthCompareOverride,
                lightMode,
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

        std::optional<NLS::Render::Context::RenderSceneBuildData> CaptureSceneBuildDataForTesting() const
        {
            const auto packet = BuildRenderFramePacket(GetFrameDescriptor());
            if (!packet.has_value())
                return std::nullopt;
            NLS::Render::Context::RenderSceneBuildData snapshot;
            snapshot.recordedDrawCommands = m_threadedRecordedDrawCommands;
            return snapshot;
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

        size_t GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting() const
        {
            return NLS::Render::Core::ABaseRenderer::
                GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting();
        }

        uint64_t GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting() const
        {
            return NLS::Render::Core::ABaseRenderer::
                GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
        }

        uint64_t GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting() const
        {
            return NLS::Render::Core::ABaseRenderer::
                GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
        }

        uint64_t GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting() const
        {
            return NLS::Render::Core::ABaseRenderer::
                GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
        }

        void SetActivePreparedPassBindingSetForTesting(
            const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
        {
            SetActivePreparedPassBindingSet(bindingSet);
        }

        void SetPreparedRecordedDrawStaticBaseRevisionDigestMaskForTesting(const uint64_t mask)
        {
            NLS::Render::Core::ABaseRenderer::
                SetPreparedRecordedDrawStaticBaseRevisionDigestMaskForTesting(mask);
        }

        void ClearPreparedRecordedDrawStaticBaseCacheForTesting() const
        {
            NLS::Render::Core::ABaseRenderer::ClearPreparedRecordedDrawStaticBaseCache();
        }

        static size_t GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting()
        {
            return NLS::Render::Core::ABaseRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting();
        }

        static size_t GetPreparedRecordedDrawStaticBaseRevisionIndexMaxEntriesForTesting()
        {
            return NLS::Render::Core::ABaseRenderer::
                GetPreparedRecordedDrawStaticBaseRevisionIndexMaxEntriesForTesting();
        }

        static bool IsPreparedRecordedDrawStaticBaseRevisionLookupKeyCompactForTesting()
        {
            return NLS::Render::Core::ABaseRenderer::
                IsPreparedRecordedDrawStaticBaseRevisionLookupKeyCompactForTesting();
        }

        static size_t GetPreparedRecordedDrawStaticBaseRevisionLookupKeySizeForTesting()
        {
            return NLS::Render::Core::ABaseRenderer::
                GetPreparedRecordedDrawStaticBaseRevisionLookupKeySizeForTesting();
        }

        static size_t GetPreparedRecordedDrawStaticBaseRevisionLookupKeyMaxSizeForTesting()
        {
            return NLS::Render::Core::ABaseRenderer::
                GetPreparedRecordedDrawStaticBaseRevisionLookupKeyMaxSizeForTesting();
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
            if (failUpdates)
                return { NLS::Render::RHI::RHIUpdateStatusCode::Unsupported, "updates disabled by test" };
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
        bool failUpdates = false;
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

        bool Prepare(
            const NLS::Render::Entities::Drawable& drawable,
            NLS::Render::Resources::MaterialPipelineStateOverrides overrides)
        {
            PreparedRecordedDraw draw;
            return PrepareRecordedDraw(
                drawable,
                std::move(overrides),
                NLS::Render::Settings::EComparaisonAlgorithm::LESS,
                draw);
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
            NLS::Render::RHI::RHIQueueOperationResult result;
            result.mayHaveQueuedGpuWork = !submitDesc.commandBuffers.empty();
            if (auto fence = std::dynamic_pointer_cast<TestFence>(submitDesc.signalFence); fence != nullptr)
            {
                fence->Signal();
                result.frameFenceSignalQueued = true;
            }
            return result;
        }
        void Present(const NLS::Render::RHI::RHIPresentDesc&) override {}
        NLS::Render::RHI::RHIQueueOperationResult PresentChecked(
            const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
        {
            Present(presentDesc);
            NLS::Render::RHI::RHIQueueOperationResult result;
            if (auto fence = std::dynamic_pointer_cast<TestFence>(presentDesc.signalFence); fence != nullptr)
            {
                fence->Signal();
                result.frameFenceSignalQueued = true;
            }
            return result;
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
            if ((failLegacyObjectRingCreation && desc.debugName == "EngineLegacyObjectConstantsRing") ||
                (failObjectSnapshotCreation && desc.debugName == "EngineObjectConstantsSnapshot") ||
                (failSnapshotPageCreation && desc.debugName == "ImmutableUniformSnapshotPage"))
                return nullptr;
            auto buffer = std::make_shared<TestBuffer>(desc, uploadDesc);
            buffer->failUpdates = failSnapshotPageUpdates && desc.debugName == "ImmutableUniformSnapshotPage";
            buffers.push_back(buffer);
            return buffer;
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::RHITextureUploadDesc&) override
        {
            textureCreationThreads.push_back(NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread());
            return std::make_shared<TestTexture>(desc);
        }

        std::vector<bool> textureCreationThreads;
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
            std::lock_guard lock(bindingSetMutex);
            ++bindingSetCreateCalls;
            lastBindingSetDesc = desc;
            bindingSetDescs.push_back(desc);
            return std::make_shared<TestBindingSet>(desc);
        }
        std::mutex bindingSetMutex;
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
            if (onGraphicsPipelineCreate)
                onGraphicsPipelineCreate();
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
        bool failLegacyObjectRingCreation = false;
        bool failObjectSnapshotCreation = false;
        bool failSnapshotPageCreation = false;
        bool failSnapshotPageUpdates = false;
        std::function<void()> onGraphicsPipelineCreate;
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

    struct LegacyObjectBindingFixture
    {
        static NLS::Render::Settings::DriverSettings MakeSettings()
        {
            NLS::Render::Settings::DriverSettings settings;
            settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            settings.framesInFlight = 2u;
            return settings;
        }

        LegacyObjectBindingFixture()
            : driver(MakeSettings()), driverService(driver), device(std::make_shared<TestExplicitDevice>()),
              renderer(driver), provider(renderer)
        {
            NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
            NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, device);
            frame.renderWidth = 256u;
            frame.renderHeight = 144u;
            frame.camera = &camera;
            for (size_t slot = 0u; slot < 2u; ++slot)
            {
                auto& context = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, slot);
                context.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(2048u);
            }
        }

        void BeginFrame(const size_t slot)
        {
            auto& context = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, slot);
            if (context.descriptorAllocator == nullptr)
                context.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(2048u);
            context.descriptorAllocator->BeginFrame(++context.frameIndex);
            resources.emplace(driver, slot);
            provider.BeginFrame(frame);
        }

        void EndFrame()
        {
            provider.EndFrame();
            resources.reset();
        }

        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> Capture(
            const NLS::Maths::Matrix4& model, bool* immutableBinding = nullptr,
            const bool requiresImmutableBinding = false)
        {
            NLS::Render::Entities::Drawable drawable;
            NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
            descriptor.modelMatrix = model;
            descriptor.requiresImmutableObjectBinding = requiresImmutableBinding;
            drawable.AddDescriptor(std::move(descriptor));
            NLS::Render::Data::PipelineState pso;
            EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
            NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
            EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
            EXPECT_FALSE(bindings.usesObjectIndex);
            if (immutableBinding != nullptr)
                *immutableBinding = bindings.objectBindingImmutable;
            return bindings.objectBindingSet;
        }

        size_t ObjectBindingCreationCount() const
        {
            return std::count_if(device->bindingSetDescs.begin(), device->bindingSetDescs.end(),
                [](const auto& desc) { return desc.debugName == "EngineObjectBindingSet"; });
        }

        NLS::Render::Context::Driver driver;
        ScopedDriverService driverService;
        std::shared_ptr<TestExplicitDevice> device;
        ProviderAwareRenderer renderer;
        NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider;
        NLS::Render::Entities::Camera camera;
        NLS::Render::Data::FrameDescriptor frame;
        std::optional<NLS::Render::Context::ScopedFrameResourceContext> resources;
    };

    void ExpectLegacyObjectMatrix(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& binding,
        const NLS::Maths::Matrix4& model)
    {
        ASSERT_NE(binding, nullptr);
        ASSERT_EQ(binding->GetDesc().entries.size(), 1u);
        const auto& entry = binding->GetDesc().entries.front();
        const auto buffer = std::dynamic_pointer_cast<TestBuffer>(entry.buffer);
        ASSERT_NE(buffer, nullptr);
        ASSERT_GE(buffer->uploadData.size(), entry.bufferOffset + sizeof(NLS::Maths::Matrix4));
        NLS::Maths::Matrix4 captured;
        std::memcpy(&captured, buffer->uploadData.data() + entry.bufferOffset, sizeof(captured));
        const auto expected = NLS::Maths::Matrix4::Transpose(model);
        for (size_t index = 0u; index < std::size(captured.data); ++index)
            EXPECT_FLOAT_EQ(captured.data[index], expected.data[index]);
    }

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

TEST(RendererFrameObjectBindingTests, DroppedSceneViewStillAppliesProxyUpdatesOnTheRenderPipe)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("DroppedViewLight").AddComponent<NLS::Engine::Components::LightComponent>();
    PackageProbeSceneRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene, std::nullopt, nullptr});
    Entities::Camera camera;
    Data::FrameDescriptor frame;
    frame.camera = &camera;
    frame.renderWidth = 64u;
    frame.renderHeight = 64u;
    Context::RenderCommandEvent release;
    Context::EnqueueRenderCommand<struct HoldProxyUpdates>(
        [release] { (void)release.WaitFor(std::chrono::seconds(3)); });
    const auto store = renderer.CaptureAndDropSceneView(NLS::Render::FrameGraph::FreezeFrameDescriptor(frame));
    const auto countBeforeRender = store != nullptr ? store->GetProxyCount() : 0u;
    Context::RenderCommandFence fence;
    fence.BeginFence(Context::RenderCommandSyncDepth::Render);
    release.Signal();
    const bool completed = fence.WaitFor(std::chrono::seconds(3));
    const bool drained = Context::DriverRendererAccess::TryDrainThreadedRendering(driver, false);
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(countBeforeRender, 0u);
    EXPECT_TRUE(completed);
    EXPECT_TRUE(drained);
    EXPECT_EQ(store->GetProxyCount(), 1u);
}

TEST(RendererFrameObjectBindingTests, QueuedProxyUpdatesOutliveTheViewRenderer)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("RetainedProxyLight").AddComponent<NLS::Engine::Components::LightComponent>();
    auto renderer = std::make_unique<PackageProbeSceneRenderer>(driver);
    renderer->AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene, std::nullopt, nullptr});
    Entities::Camera camera;
    Data::FrameDescriptor frame;
    frame.camera = &camera;
    frame.renderWidth = 64u;
    frame.renderHeight = 64u;
    Context::RenderCommandEvent release;
    Context::EnqueueRenderCommand<struct HoldViewDestruction>(
        [release] { (void)release.WaitFor(std::chrono::seconds(3)); });
    auto* view = renderer.release();
    Context::EnqueueRenderCommand<struct DestroyViewBeforeItsProxyUpdate>([view] { delete view; });
    const auto store = view->CaptureAndDropSceneView(NLS::Render::FrameGraph::FreezeFrameDescriptor(frame));
    Context::RenderCommandFence fence;
    fence.BeginFence(Context::RenderCommandSyncDepth::Render);
    release.Signal();
    ASSERT_TRUE(fence.WaitFor(std::chrono::seconds(3)));
    ASSERT_TRUE(fence.Succeeded()) << fence.GetDiagnostic();
    ASSERT_NE(store, nullptr);
    EXPECT_EQ(store->GetProxyCount(), 1u);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RendererFrameObjectBindingTests, RenderScenePackageMarksFrameAndObjectDataReadyForResolvedDraws)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    PackageProbeSceneRenderer renderer(driver);

    NLS::Render::Context::RenderFramePacket packet;
    NLS::Render::Context::RenderSceneBuildData snapshot;
    snapshot.visibleOpaqueDrawCount = 2u;
    snapshot.visibleTransparentDrawCount = 1u;
    snapshot.visibleSkyboxDrawCount = 1u;

    const auto package = renderer.CaptureRenderScenePackage(packet, snapshot);

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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    ProviderAwareRenderer renderer(*driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Context::RenderFramePacket packet;

    NLS::Render::Context::RenderScenePackage package;
    package.visibleDrawCount = 3u;
    EXPECT_FALSE(package.frameDataReady);
    EXPECT_FALSE(package.objectDataReady);

    provider.PrepareRenderScenePackage(packet, package);

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderDelayedCaptureRetainsProducingFrameConstants)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, device);
    auto& context = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    context.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    context.descriptorAllocator->BeginFrame(0u);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);
    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);
    NLS::Maths::Transform cameraTransform;
    NLS::Render::Entities::Camera camera(&cameraTransform);
    camera.SetPosition({ 3.0f, 4.0f, 5.0f });
    camera.CacheMatrices(256u, 144u);
    NLS::Render::Data::FrameDescriptor frame;
    frame.camera = &camera;
    provider.BeginFrame(frame);
    auto frozenFrame = provider.FreezeCaptureFrame();
    ASSERT_TRUE(provider.TryReservePreparedFrameResources());

    std::atomic_bool bound = false;
    std::atomic_bool nextFrameFinished = false;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> bindings;
    bool captured = false;
    bool objectPrepared = false;
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets drawBindings;
    std::thread capture([&]
    {
        provider.BindCaptureFrameSlot(0u);
        NLS::Render::Data::PipelineState pso;
        NLS::Render::Entities::Drawable drawable;
        drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
            NLS::Maths::Matrix4::Translation({ 7.0f, 8.0f, 9.0f }),
            NLS::Maths::Matrix4::Identity
        });
        const bool prepared = provider.PrepareDraw(pso, drawable);
        bound.store(true, std::memory_order_release);
        while (!nextFrameFinished.load(std::memory_order_acquire))
            std::this_thread::yield();
        captured = provider.CaptureFrameBindingSet(bindings);
        objectPrepared = prepared && provider.IsObjectPrepared() &&
            provider.CapturePreparedBindingSets(pso, drawable, drawBindings);
        provider.UnbindCaptureFrameSlot();
    });
    while (!bound.load(std::memory_order_acquire))
        std::this_thread::yield();
    provider.EndFrame();
    explicitFrameResources.reset();
    camera.SetPosition({ 30.0f, 40.0f, 50.0f });
    camera.CacheMatrices(256u, 144u);
    provider.BeginFrame(frame);
    provider.EndFrame();
    nextFrameFinished.store(true, std::memory_order_release);
    capture.join();
    ASSERT_TRUE(captured);
    ASSERT_TRUE(objectPrepared);
    ASSERT_NE(drawBindings.objectBindingSet, nullptr);
    ASSERT_NE(bindings, nullptr);
    ASSERT_FALSE(bindings->GetDesc().entries.empty());
    const auto buffer = std::dynamic_pointer_cast<TestBuffer>(bindings->GetDesc().entries.front().buffer);
    ASSERT_NE(buffer, nullptr);
    ASSERT_GE(buffer->uploadData.size(), sizeof(NLS::Maths::Matrix4) + sizeof(NLS::Maths::Vector3));
    NLS::Maths::Vector3 position;
    std::memcpy(&position, buffer->uploadData.data() + sizeof(NLS::Maths::Matrix4), sizeof(position));
    EXPECT_FLOAT_EQ(position.x, 3.0f);
    EXPECT_FLOAT_EQ(position.y, 4.0f);
    EXPECT_FLOAT_EQ(position.z, 5.0f);

    // A builder may receive its slot only after later frames have finished.
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> lateBindings;
    std::thread lateCapture([&]
    {
        frozenFrame(1u);
        captured = provider.CaptureFrameBindingSet(lateBindings);
        provider.UnbindCaptureFrameSlot();
    });
    lateCapture.join();
    ASSERT_TRUE(captured);
    ASSERT_NE(lateBindings, nullptr);
    const auto lateBuffer = std::dynamic_pointer_cast<TestBuffer>(lateBindings->GetDesc().entries.front().buffer);
    ASSERT_NE(lateBuffer, nullptr);
    ASSERT_GE(lateBuffer->uploadData.size(), sizeof(NLS::Maths::Matrix4) + sizeof(position));
    std::memcpy(&position, lateBuffer->uploadData.data() + sizeof(NLS::Maths::Matrix4), sizeof(position));
    EXPECT_FLOAT_EQ(position.x, 3.0f);
    EXPECT_FLOAT_EQ(position.y, 4.0f);
    EXPECT_FLOAT_EQ(position.z, 5.0f);
}

TEST(RendererFrameObjectBindingTests, ReservedGameHelpersSurviveRenderCaptureInTheSameSlot)
{
    using namespace NLS::Render;
    for (const bool indexed : {false, true})
    {
        SCOPED_TRACE(indexed);
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
        settings.enableExplicitRHI = false;
        Context::Driver driver(settings);
        const ScopedDriverService driverService(driver);
        Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
        const Context::ScopedGameThread gameThread;
        Context::DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
        const auto slot = Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(driver);
        ASSERT_TRUE(slot.has_value());
        ProviderAwareRenderer renderer(driver);
        NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);
        Entities::Camera camera;
        Data::FrameDescriptor frame;
        frame.camera = &camera;
        provider.BeginFrame(frame);
        auto* shader = CreateReflectionOnlyImportedShader("Tests/Synthetic/ReservedHelpers.hlsl",
            indexed ? MakeIndexedObjectDataReflection() : MakeFrameConstantOnlyReflection("FrameConstants"),
            "shader:reserved-helpers");
        ASSERT_NE(shader, nullptr);
        Resources::Material material(shader);
        Entities::Drawable helper;
        helper.material = &material;
        const auto helperMatrix = NLS::Maths::Matrix4::Translation({2.0f, 3.0f, 5.0f});
        helper.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({helperMatrix, NLS::Maths::Matrix4::Identity});
        Data::PipelineState pso;
        ASSERT_TRUE(provider.PrepareDraw(pso, helper));
        Core::FrameObjectBindingProvider::PreparedBindingSets helperBindings;
        ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, helper, helperBindings));
        ASSERT_NE(helperBindings.objectBindingSet, nullptr);
        EXPECT_FALSE(helperBindings.objectBindingImmutable);
        const auto helperEntry = helperBindings.objectBindingSet->GetDesc().entries.front();
        const auto helperBuffer = std::dynamic_pointer_cast<TestBuffer>(helperEntry.buffer);
        ASSERT_NE(helperBuffer, nullptr);
        const size_t helperOffset = indexed
            ? helperBindings.objectConstants.objectIndex * sizeof(NLS::Maths::Matrix4)
            : helperEntry.bufferOffset;
        auto frozen = provider.FreezeCaptureFrame();
        provider.EndFrame();
        std::thread rendering([&]
        {
            const Context::ScopedFrameResourceContext frameResources(driver, *slot);
            frozen(*slot);
            Entities::Drawable sceneDraw;
            sceneDraw.material = &material;
            sceneDraw.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
                NLS::Maths::Matrix4::Translation({20.0f, 30.0f, 50.0f}), NLS::Maths::Matrix4::Identity,
                indexed ? 0u : Data::DrawableObjectDescriptor::kInvalidObjectIndex});
            Core::FrameObjectBindingProvider::PreparedBindingSets sceneBindings;
            EXPECT_TRUE(provider.PrepareDraw(pso, sceneDraw));
            EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, sceneDraw, sceneBindings));
            if (indexed)
                EXPECT_NE(sceneBindings.objectConstants.objectIndex, helperBindings.objectConstants.objectIndex);
            provider.UnbindCaptureFrameSlot();
        });
        rendering.join();
        ASSERT_GE(helperBuffer->uploadData.size(), helperOffset + sizeof(NLS::Maths::Matrix4));
        NLS::Maths::Matrix4 retained;
        std::memcpy(&retained, helperBuffer->uploadData.data() + helperOffset, sizeof(retained));
        const auto expected = NLS::Maths::Matrix4::Transpose(helperMatrix);
        for (size_t index = 0u; index < 16u; ++index)
            EXPECT_FLOAT_EQ(retained.data[index], expected.data[index]);
        EXPECT_TRUE(Resources::Loaders::ShaderLoader::Destroy(shader));
        Context::DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(driver, *slot);
    }
}

TEST(RendererFrameObjectBindingTests, OnlyDedicatedObjectSnapshotsAreImmutableAcrossSlotReuse)
{
    LegacyObjectBindingFixture fixture;
    const auto first = NLS::Maths::Matrix4::Translation({2.0f, 3.0f, 5.0f});
    const auto second = NLS::Maths::Matrix4::Translation({20.0f, 30.0f, 50.0f});
    bool immutable = true;
    fixture.BeginFrame(0u);
    ASSERT_NE(fixture.Capture(first, &immutable), nullptr);
    EXPECT_FALSE(immutable);
    fixture.EndFrame();

    // A failed ring allocation uses a dedicated snapshot that retains its original matrix.
    fixture.device->failLegacyObjectRingCreation = true;
    fixture.BeginFrame(1u);
    const auto snapshot = fixture.Capture(first, &immutable);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(immutable);
    fixture.EndFrame();
    fixture.BeginFrame(1u);
    ASSERT_NE(fixture.Capture(second, &immutable), nullptr);
    ExpectLegacyObjectMatrix(snapshot, first);
    fixture.EndFrame();

    // If the snapshot allocation also fails, the mutable uniform-buffer fallback is unsafe to cache.
    fixture.device->failObjectSnapshotCreation = true;
    fixture.BeginFrame(1u);
    ASSERT_NE(fixture.Capture(first, &immutable), nullptr);
    EXPECT_FALSE(immutable);
    fixture.EndFrame();
}

TEST(RendererFrameObjectBindingTests, RequestedImmutableObjectBindingSurvivesFrameSlotReuse)
{
    LegacyObjectBindingFixture fixture;
    const auto& diagnostics = static_cast<const NLS::Render::Core::FrameObjectBindingProvider&>(fixture.provider);
    const auto original = NLS::Maths::Matrix4::Translation({2.0f, 3.0f, 5.0f});
    bool immutable = false;
    fixture.BeginFrame(0u);
    const auto snapshot = fixture.Capture(original, &immutable, true);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_TRUE(immutable);
    EXPECT_EQ(diagnostics.GetLegacyObjectConstantsRingHitCount(), 0u);
    EXPECT_EQ(diagnostics.GetLegacyObjectConstantsRingFallbackCount(), 0u);
    fixture.EndFrame();

    for (size_t frame = 0u; frame < 12u; ++frame)
    {
        fixture.BeginFrame(frame % 2u);
        const auto moved = NLS::Maths::Matrix4::Translation({20.0f + static_cast<float>(frame), 30.0f, 50.0f});
        const auto ringBinding = fixture.Capture(moved, &immutable);
        ASSERT_NE(ringBinding, nullptr);
        EXPECT_FALSE(immutable);
        ExpectLegacyObjectMatrix(ringBinding, moved);
        ExpectLegacyObjectMatrix(snapshot, original);
        fixture.EndFrame();
    }
    EXPECT_EQ(diagnostics.GetLegacyObjectConstantsRingHitCount(), 12u);
}

TEST(RendererFrameObjectBindingTests, RequestedImmutableObjectBindingRejectsMutableAllocationFallback)
{
    LegacyObjectBindingFixture fixture;
    const auto& diagnostics = static_cast<const NLS::Render::Core::FrameObjectBindingProvider&>(fixture.provider);
    fixture.device->failObjectSnapshotCreation = true;
    fixture.BeginFrame(0u);
    NLS::Render::Entities::Drawable drawable;
    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    descriptor.requiresImmutableObjectBinding = true;
    drawable.AddDescriptor(std::move(descriptor));
    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(fixture.provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    EXPECT_FALSE(fixture.provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_EQ(bindings.objectBindingSet, nullptr);
    EXPECT_FALSE(bindings.objectBindingImmutable);
    EXPECT_EQ(diagnostics.GetLegacyObjectConstantsRingHitCount(), 0u);
    fixture.EndFrame();
}

TEST(RendererFrameObjectBindingTests, ImmutableSnapshotBatchKeepsObjectsAcrossPagesAndFrameSlotReuse)
{
    LegacyObjectBindingFixture fixture;
    std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> captured;
    std::vector<NLS::Maths::Matrix4> models;
    {
        NLS::Render::Buffers::ScopedUniformBufferSnapshotBatch batch;
        fixture.BeginFrame(0u);
        for (size_t index = 0u; index < 300u; ++index)
        {
            models.push_back(NLS::Maths::Matrix4::Translation({static_cast<float>(index), 3.0f, 5.0f}));
            bool immutable = false;
            captured.push_back(fixture.Capture(models.back(), &immutable, true));
            ASSERT_NE(captured.back(), nullptr);
            ASSERT_TRUE(immutable);
            const auto& entry = captured.back()->GetDesc().entries.front();
            EXPECT_EQ(entry.bufferOffset % 256u, 0u);
            EXPECT_LE(entry.bufferOffset + entry.bufferRange, entry.buffer->GetDesc().size);
        }
        fixture.EndFrame();
    }
    EXPECT_EQ(captured[0]->GetDesc().entries.front().buffer, captured[1]->GetDesc().entries.front().buffer);
    EXPECT_NE(captured[0]->GetDesc().entries.front().bufferOffset, captured[1]->GetDesc().entries.front().bufferOffset);
    EXPECT_EQ(std::count_if(fixture.device->buffers.begin(), fixture.device->buffers.end(),
        [](const auto& buffer) { return buffer->GetDebugName() == "ImmutableUniformSnapshotPage"; }), 2u);

    for (size_t frame = 0u; frame < 12u; ++frame)
    {
        fixture.BeginFrame(frame % 2u);
        const auto moved = NLS::Maths::Matrix4::Translation({1000.0f + static_cast<float>(frame), 7.0f, 11.0f});
        ExpectLegacyObjectMatrix(fixture.Capture(moved), moved);
        {
            NLS::Render::Buffers::ScopedUniformBufferSnapshotBatch batch;
            const auto nextSnapshot = fixture.Capture(moved, nullptr, true);
            ASSERT_NE(nextSnapshot, nullptr);
            EXPECT_NE(nextSnapshot->GetDesc().entries.front().buffer, captured.front()->GetDesc().entries.front().buffer);
        }
        fixture.EndFrame();
    }
    for (size_t index = 0u; index < captured.size(); ++index)
        ExpectLegacyObjectMatrix(captured[index], models[index]);
}

TEST(RendererFrameObjectBindingTests, ImmutableSnapshotBatchesNestAndReleaseUnreferencedPages)
{
    LegacyObjectBindingFixture fixture;
    NLS::Render::Buffers::UniformBuffer buffer(sizeof(uint32_t));
    NLS::Render::Buffers::ExplicitUniformBufferSnapshot first;
    NLS::Render::Buffers::ExplicitUniformBufferSnapshot second;
    std::weak_ptr<NLS::Render::RHI::RHIBuffer> nestedPage;
    {
        NLS::Render::Buffers::ScopedUniformBufferSnapshotBatch outer;
        buffer.SetSubData(uint32_t{17u}, 0u);
        first = buffer.CreateExplicitSnapshot();
        ASSERT_NE(first.buffer, nullptr);
        {
            NLS::Render::Buffers::ScopedUniformBufferSnapshotBatch nested;
            buffer.SetSubData(uint32_t{29u}, 0u);
            const auto nestedSnapshot = buffer.CreateExplicitSnapshot();
            ASSERT_NE(nestedSnapshot.buffer, first.buffer);
            nestedPage = nestedSnapshot.buffer;
            fixture.device->buffers.clear();
        }
        EXPECT_TRUE(nestedPage.expired());
        buffer.SetSubData(uint32_t{41u}, 0u);
        second = buffer.CreateExplicitSnapshot();
        EXPECT_EQ(second.buffer, first.buffer);
        EXPECT_NE(second.offset, first.offset);
    }
    const auto page = std::dynamic_pointer_cast<TestBuffer>(first.buffer);
    ASSERT_NE(page, nullptr);
    uint32_t value = 0u;
    std::memcpy(&value, page->uploadData.data() + first.offset, sizeof(value));
    EXPECT_EQ(value, 17u);
    std::memcpy(&value, page->uploadData.data() + second.offset, sizeof(value));
    EXPECT_EQ(value, 41u);
    const auto standalone = buffer.CreateExplicitSnapshot();
    EXPECT_NE(standalone.buffer, first.buffer);
    EXPECT_EQ(standalone.offset, 0u);
}

TEST(RendererFrameObjectBindingTests, ImmutableSnapshotBatchFallsBackWhenPageCreationOrUpdateFails)
{
    LegacyObjectBindingFixture fixture;
    NLS::Render::Buffers::UniformBuffer buffer(sizeof(uint32_t));
    for (const bool failCreation : {true, false})
    {
        fixture.device->failSnapshotPageCreation = failCreation;
        fixture.device->failSnapshotPageUpdates = !failCreation;
        NLS::Render::Buffers::ScopedUniformBufferSnapshotBatch batch;
        for (uint32_t value = 1u; value <= 3u; ++value)
        {
            buffer.SetSubData(value, 0u);
            const auto snapshot = buffer.CreateExplicitSnapshot("FallbackSnapshot");
            ASSERT_NE(snapshot.buffer, nullptr);
            EXPECT_EQ(snapshot.buffer->GetDebugName(), "FallbackSnapshot");
            EXPECT_EQ(snapshot.offset, 0u);
            const auto capturedBuffer = std::dynamic_pointer_cast<TestBuffer>(snapshot.buffer);
            ASSERT_NE(capturedBuffer, nullptr);
            uint32_t capturedValue = 0u;
            std::memcpy(&capturedValue, capturedBuffer->uploadData.data(), sizeof(capturedValue));
            EXPECT_EQ(capturedValue, value);
        }
    }
}

TEST(RendererFrameObjectBindingTests, ImmutableSnapshotBatchKeepsMaterialColorsAtDistinctOffsets)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();
    LegacyObjectBindingFixture fixture;
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Unlit.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> bindings;
    {
        NLS::Render::Buffers::ScopedUniformBufferSnapshotBatch batch;
        for (uint32_t index = 1u; index <= 4u; ++index)
        {
            material.Set("u_Diffuse", NLS::Maths::Vector4(static_cast<float>(index) / 255.0f, 0.0f, 0.0f, 1.0f));
            bindings.push_back(material.GetExplicitBindingSet(fixture.device));
            ASSERT_NE(bindings.back(), nullptr);
        }
    }
    for (size_t index = 0u; index < bindings.size(); ++index)
    {
        const auto& entries = bindings[index]->GetDesc().entries;
        const auto constant = std::find_if(entries.begin(), entries.end(),
            [](const auto& entry) { return entry.type == NLS::Render::RHI::BindingType::UniformBuffer; });
        ASSERT_NE(constant, entries.end());
        EXPECT_EQ(constant->bufferOffset, index * 256u);
        const auto page = std::dynamic_pointer_cast<TestBuffer>(constant->buffer);
        ASSERT_NE(page, nullptr);
        NLS::Maths::Vector4 color;
        std::memcpy(&color, page->uploadData.data() + constant->bufferOffset, sizeof(color));
        EXPECT_FLOAT_EQ(color.x, static_cast<float>(index + 1u) / 255.0f);
        EXPECT_FLOAT_EQ(color.w, 1.0f);
    }
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, EngineProviderCapturesObjectConstantsFromCurrentDrawable)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    // The object constants come from the frame slot's ring, not from a dedicated
    // snapshot buffer: the binding names a block inside the ring and the block is
    // addressed by an aligned offset.
    EXPECT_EQ(objectEntry.buffer->GetDebugName(), "EngineLegacyObjectConstantsRing");
    EXPECT_EQ(objectEntry.bufferOffset % 256u, 0u);
    EXPECT_EQ(objectEntry.bufferRange, sizeof(NLS::Maths::Matrix4));

    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(objectEntry.buffer);
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_GE(objectBuffer->uploadData.size(),
        static_cast<size_t>(objectEntry.bufferOffset) + sizeof(NLS::Maths::Matrix4));

    NLS::Maths::Matrix4 capturedMatrix;
    std::memcpy(
        &capturedMatrix,
        objectBuffer->uploadData.data() + static_cast<size_t>(objectEntry.bufferOffset),
        sizeof(capturedMatrix));
    const auto expectedShaderMatrix = NLS::Maths::Matrix4::Transpose(modelMatrix);
    for (size_t index = 0u; index < std::size(capturedMatrix.data); ++index)
        EXPECT_FLOAT_EQ(capturedMatrix.data[index], expectedShaderMatrix.data[index]);

    provider.EndFrame();
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderCapturesPreparedBindingsAfterFrameEndWhenSlotIsBound)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 32u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(16u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);
    provider.EndFrame();

    provider.BindCaptureFrameSlot(0u);
    NLS::Render::Data::PipelineState pso;
    NLS::Render::Entities::Drawable drawable;
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));

    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_NE(bindings.frameBindingSet, nullptr);
    EXPECT_NE(bindings.objectBindingSet, nullptr);
    EXPECT_EQ(bindings.frameBindingSet->GetDebugName(), "EngineFrameBindingSet");
    EXPECT_EQ(bindings.objectBindingSet->GetDebugName(), "EngineObjectBindingSet");

    provider.UnbindCaptureFrameSlot();
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderCapturesPreparedReceiveShadowConstantsForIndexedDraws)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderPreservesEarlierObjectDataWhenFrameBufferGrows)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderShrinksIdleHighWaterObjectBuffer)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsSparseExternalObjectIndexForIndexedDraw)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderAssignsObjectIndexForManualIndexedShaderDraw)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 2u);
    const auto& objectEntry = bindings.objectBindingSet->GetDesc().entries[0];
    EXPECT_EQ(objectEntry.type, NLS::Render::RHI::BindingType::StructuredBuffer);
    ASSERT_NE(objectEntry.buffer, nullptr);
    EXPECT_EQ(objectEntry.buffer->GetDebugName(), "EngineObjectDataBuffer");

    const auto& indirectionEntry = bindings.objectBindingSet->GetDesc().entries[1];
    EXPECT_EQ(indirectionEntry.type, NLS::Render::RHI::BindingType::StructuredBuffer);
    ASSERT_NE(indirectionEntry.buffer, nullptr);
    EXPECT_EQ(indirectionEntry.buffer->GetDebugName(), "EngineInstanceObjectIndexBuffer");
    auto indirectionBuffer = std::dynamic_pointer_cast<TestBuffer>(indirectionEntry.buffer);
    ASSERT_NE(indirectionBuffer, nullptr);
    ASSERT_GE(indirectionBuffer->uploadData.size(), sizeof(uint32_t));
    uint32_t resolvedObjectSlot = NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
    std::memcpy(&resolvedObjectSlot, indirectionBuffer->uploadData.data(), sizeof(resolvedObjectSlot));
    EXPECT_EQ(resolvedObjectSlot, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderUsesEffectivePassShaderForIndexedObjectData)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRestoresIndexedObjectBindingAfterLegacyObjectDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}
TEST(RendererFrameObjectBindingTests, EngineProviderResolvesSparsePersistentInstanceSlotsThroughIndirection)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 71u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    material.SetGPUInstances(3);

    const NLS::Maths::Matrix4 matrices[3] = {
        NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f }),
        NLS::Maths::Matrix4::Translation({ 2.0f, 0.0f, 0.0f }),
        NLS::Maths::Matrix4::Translation({ 3.0f, 0.0f, 0.0f })
    };
    const std::array<uint32_t, 3u> persistentSlots { 7u, 2u, 11u };

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = 3u;
    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    descriptor.modelMatrix = matrices[0];
    descriptor.objectCount = 3u;
    descriptor.instanceModelMatrices = { matrices[0], matrices[1], matrices[2] };
    descriptor.instanceObjectIndices.assign(persistentSlots.begin(), persistentSlots.end());
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(descriptor));

    provider.BeginFrame(frameDescriptor);

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectConstants.objectIndex, 0u);

    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 2u);
    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(
        bindings.objectBindingSet->GetDesc().entries[0].buffer);
    ASSERT_NE(objectBuffer, nullptr);
    EXPECT_EQ(objectBuffer->GetDebugName(), "EngineObjectDataBuffer");
    auto indirectionBuffer = std::dynamic_pointer_cast<TestBuffer>(
        bindings.objectBindingSet->GetDesc().entries[1].buffer);
    ASSERT_NE(indirectionBuffer, nullptr);
    EXPECT_EQ(indirectionBuffer->GetDebugName(), "EngineInstanceObjectIndexBuffer");

    std::array<uint32_t, 3u> uploadedSlots {};
    ASSERT_GE(indirectionBuffer->uploadData.size(), sizeof(uploadedSlots));
    std::memcpy(uploadedSlots.data(), indirectionBuffer->uploadData.data(), sizeof(uploadedSlots));
    EXPECT_EQ(uploadedSlots, persistentSlots);
    EXPECT_EQ(indirectionBuffer->updateCalls, 1u);

    // Sparse persistent slots force one dirty-range upload per instance.
    ASSERT_EQ(objectBuffer->updateCalls, 3u);
    for (size_t instanceIndex = 0u; instanceIndex < persistentSlots.size(); ++instanceIndex)
    {
        const auto slotOffset = static_cast<size_t>(persistentSlots[instanceIndex]) * sizeof(NLS::Maths::Matrix4);
        ASSERT_GE(objectBuffer->uploadData.size(), slotOffset + sizeof(NLS::Maths::Matrix4));
        NLS::Maths::Matrix4 capturedMatrix;
        std::memcpy(&capturedMatrix, objectBuffer->uploadData.data() + slotOffset, sizeof(capturedMatrix));
        const auto expectedMatrix = NLS::Maths::Matrix4::Transpose(matrices[instanceIndex]);
        for (size_t element = 0u; element < std::size(capturedMatrix.data); ++element)
            EXPECT_FLOAT_EQ(capturedMatrix.data[element], expectedMatrix.data[element]);
    }

    provider.EndFrame();

    // The persistent slots stay stable across frames and unchanged data is not re-uploaded.
    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets secondBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, secondBindings));
    EXPECT_EQ(secondBindings.objectConstants.objectIndex, 0u);
    auto secondIndirectionBuffer = std::dynamic_pointer_cast<TestBuffer>(
        secondBindings.objectBindingSet->GetDesc().entries[1].buffer);
    ASSERT_NE(secondIndirectionBuffer, nullptr);
    EXPECT_EQ(secondIndirectionBuffer, indirectionBuffer);
    EXPECT_EQ(secondIndirectionBuffer->updateCalls, 1u);
    EXPECT_EQ(objectBuffer->updateCalls, 3u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderKeepsTransientAndExplicitObjectSlotsDisjointInEitherOrder)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 74u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
        "App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);

    const auto capture = [&provider](
        NLS::Render::Entities::Drawable& drawable,
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets& bindings)
    {
        NLS::Render::Data::PipelineState pso;
        EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
        EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
        EXPECT_TRUE(bindings.usesObjectIndex);
        ASSERT_NE(bindings.objectBindingSet, nullptr);
        ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 2u);
    };

    const auto verifyOrder = [&](const bool transientFirst, const float matrixOffset)
    {
        provider.BeginFrame(frameDescriptor);

        const auto transientMatrix = NLS::Maths::Matrix4::Translation({ matrixOffset + 1.0f, 2.0f, 3.0f });
        const auto explicitMatrix = NLS::Maths::Matrix4::Translation({ matrixOffset + 4.0f, 5.0f, 6.0f });

        NLS::Render::Entities::Drawable transientDrawable;
        transientDrawable.material = &material;
        transientDrawable.instanceCount = 1u;
        NLS::Engine::Rendering::EngineDrawableDescriptor transientDescriptor;
        transientDescriptor.modelMatrix = transientMatrix;
        transientDescriptor.objectCount = 1u;
        transientDescriptor.instanceModelMatrices = { transientMatrix };
        transientDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(
            std::move(transientDescriptor));

        NLS::Render::Entities::Drawable explicitDrawable;
        explicitDrawable.material = &material;
        explicitDrawable.instanceCount = 1u;
        NLS::Engine::Rendering::EngineDrawableDescriptor explicitDescriptor;
        explicitDescriptor.modelMatrix = explicitMatrix;
        explicitDescriptor.objectIndex = 0u;
        explicitDescriptor.objectCount = 1u;
        explicitDescriptor.instanceModelMatrices = { explicitMatrix };
        explicitDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(
            std::move(explicitDescriptor));

        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets transientBindings;
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets explicitBindings;
        if (transientFirst)
        {
            capture(transientDrawable, transientBindings);
            capture(explicitDrawable, explicitBindings);
        }
        else
        {
            capture(explicitDrawable, explicitBindings);
            capture(transientDrawable, transientBindings);
        }

        auto indirectionBuffer = std::dynamic_pointer_cast<TestBuffer>(
            transientBindings.objectBindingSet->GetDesc().entries[1].buffer);
        auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(
            transientBindings.objectBindingSet->GetDesc().entries[0].buffer);
        ASSERT_NE(indirectionBuffer, nullptr);
        ASSERT_NE(objectBuffer, nullptr);

        const auto readObjectSlot = [&](const auto& bindings)
        {
            const auto byteOffset =
                static_cast<size_t>(bindings.objectConstants.objectIndex) * sizeof(uint32_t);
            EXPECT_GE(indirectionBuffer->uploadData.size(), byteOffset + sizeof(uint32_t));
            uint32_t objectSlot = NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex;
            if (indirectionBuffer->uploadData.size() >= byteOffset + sizeof(uint32_t))
            {
                std::memcpy(
                    &objectSlot,
                    indirectionBuffer->uploadData.data() + byteOffset,
                    sizeof(objectSlot));
            }
            return objectSlot;
        };
        const auto transientSlot = readObjectSlot(transientBindings);
        const auto explicitSlot = readObjectSlot(explicitBindings);
        EXPECT_NE(transientSlot, explicitSlot);

        const auto expectMatrixAtSlot = [&](const uint32_t slot, const NLS::Maths::Matrix4& expected)
        {
            const auto byteOffset = static_cast<size_t>(slot) * sizeof(NLS::Maths::Matrix4);
            ASSERT_GE(objectBuffer->uploadData.size(), byteOffset + sizeof(NLS::Maths::Matrix4));
            NLS::Maths::Matrix4 captured;
            std::memcpy(
                &captured,
                objectBuffer->uploadData.data() + byteOffset,
                sizeof(captured));
            const auto transposedExpected = NLS::Maths::Matrix4::Transpose(expected);
            for (size_t index = 0u; index < std::size(captured.data); ++index)
                EXPECT_FLOAT_EQ(captured.data[index], transposedExpected.data[index]);
        };
        expectMatrixAtSlot(transientSlot, transientMatrix);
        expectMatrixAtSlot(explicitSlot, explicitMatrix);

        provider.EndFrame();
    };

    verifyOrder(true, 0.0f);
    ++frameContext.frameIndex;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    verifyOrder(false, 10.0f);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderGrowsIndirectionBufferForLargeSparseInstanceGroups)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 73u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    // kInitialObjectDataBufferCapacity and the indirection ring both start at 256
    // entries, so 300 sparse instances must force both buffers to grow.
    constexpr uint32_t kInstanceCount = 300u;
    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    descriptor.modelMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f });
    descriptor.objectCount = kInstanceCount;
    descriptor.instanceModelMatrices.reserve(kInstanceCount);
    descriptor.instanceObjectIndices.reserve(kInstanceCount);
    for (uint32_t index = 0u; index < kInstanceCount; ++index)
    {
        descriptor.instanceModelMatrices.push_back(
            NLS::Maths::Matrix4::Translation({ static_cast<float>(index) * 0.5f, 0.0f, 0.0f }));
        descriptor.instanceObjectIndices.push_back(index * 2u);
    }

    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.instanceCount = kInstanceCount;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(descriptor));

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectConstants.objectIndex, 0u);

    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 2u);
    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(
        bindings.objectBindingSet->GetDesc().entries[0].buffer);
    auto indirectionBuffer = std::dynamic_pointer_cast<TestBuffer>(
        bindings.objectBindingSet->GetDesc().entries[1].buffer);
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_NE(indirectionBuffer, nullptr);

    // Doubling from 256 covers 300 entries at 512; persistent slots reach 598 at
    // 1024 object-data entries.
    EXPECT_EQ(indirectionBuffer->GetDesc().size, 512u * sizeof(uint32_t));
    EXPECT_EQ(objectBuffer->GetDesc().size, 1024u * sizeof(NLS::Maths::Matrix4));

    std::vector<uint32_t> uploadedSlots(kInstanceCount, 0u);
    ASSERT_GE(indirectionBuffer->uploadData.size(), kInstanceCount * sizeof(uint32_t));
    std::memcpy(uploadedSlots.data(), indirectionBuffer->uploadData.data(), kInstanceCount * sizeof(uint32_t));
    for (uint32_t index = 0u; index < kInstanceCount; ++index)
        EXPECT_EQ(uploadedSlots[index], index * 2u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    explicitFrameResources.reset();
}
TEST(RendererFrameObjectBindingTests, EngineProviderKeepsDirectSlotPathForShadersWithoutInstanceIndirection)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 72u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    const NLS::Maths::Matrix4 firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    const NLS::Maths::Matrix4 secondMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });

    NLS::Render::Entities::Drawable contiguousDrawable;
    contiguousDrawable.material = &material;
    contiguousDrawable.instanceCount = 2u;
    NLS::Engine::Rendering::EngineDrawableDescriptor contiguousDescriptor;
    contiguousDescriptor.modelMatrix = firstMatrix;
    contiguousDescriptor.objectCount = 2u;
    contiguousDescriptor.instanceModelMatrices = { firstMatrix, secondMatrix };
    contiguousDescriptor.instanceObjectIndices = { 5u, 6u };
    contiguousDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(contiguousDescriptor));

    NLS::Render::Data::PipelineState pso;
    ASSERT_TRUE(provider.PrepareDraw(pso, contiguousDrawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, contiguousDrawable, bindings));
    EXPECT_TRUE(bindings.usesObjectIndex);
    EXPECT_EQ(bindings.objectConstants.objectIndex, 5u);
    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(bindings.objectBindingSet->GetDesc().entries[0].buffer->GetDebugName(), "EngineObjectDataBuffer");

    NLS::Render::Entities::Drawable sparseDrawable;
    sparseDrawable.material = &material;
    sparseDrawable.instanceCount = 2u;
    NLS::Engine::Rendering::EngineDrawableDescriptor sparseDescriptor;
    sparseDescriptor.modelMatrix = firstMatrix;
    sparseDescriptor.objectCount = 2u;
    sparseDescriptor.instanceModelMatrices = { firstMatrix, secondMatrix };
    sparseDescriptor.instanceObjectIndices = { 5u, 9u };
    sparseDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(sparseDescriptor));

    EXPECT_FALSE(provider.PrepareDraw(pso, sparseDrawable));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderKeepsLegacyObjectBindingAfterIndexedObjectDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 51u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    NLS::Render::Entities::Drawable legacyDrawable;
    legacyDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Identity,
        NLS::Maths::Matrix4::Identity
    });

    NLS::Render::Entities::Drawable indexedDrawable;
    indexedDrawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
        NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f }),
        NLS::Maths::Matrix4::Identity,
        5u
    });

    NLS::Render::Entities::Drawable descriptorlessLegacyDrawable;
    NLS::Render::Data::PipelineState pso;

    ASSERT_TRUE(provider.PrepareDraw(pso, legacyDrawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets initialLegacyBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, legacyDrawable, initialLegacyBindings));
    ASSERT_NE(initialLegacyBindings.objectBindingSet, nullptr);
    ASSERT_EQ(initialLegacyBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(initialLegacyBindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::UniformBuffer);

    ASSERT_TRUE(provider.PrepareDraw(pso, indexedDrawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets indexedBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, indexedDrawable, indexedBindings));
    ASSERT_NE(indexedBindings.objectBindingSet, nullptr);
    ASSERT_EQ(indexedBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(indexedBindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::StructuredBuffer);

    ASSERT_TRUE(provider.PrepareDraw(pso, descriptorlessLegacyDrawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets descriptorlessLegacyBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(
        pso,
        descriptorlessLegacyDrawable,
        descriptorlessLegacyBindings));
    EXPECT_EQ(descriptorlessLegacyBindings.objectBindingSet, initialLegacyBindings.objectBindingSet);
    ASSERT_NE(descriptorlessLegacyBindings.objectBindingSet, nullptr);
    ASSERT_EQ(descriptorlessLegacyBindings.objectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(descriptorlessLegacyBindings.objectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::UniformBuffer);

    TestCommandBuffer commandBuffer;
    provider.PrepareExplicitDraw(commandBuffer, pso, descriptorlessLegacyDrawable);
    EXPECT_EQ(commandBuffer.lastObjectBindingSet, initialLegacyBindings.objectBindingSet);
    ASSERT_NE(commandBuffer.lastObjectBindingSet, nullptr);
    ASSERT_EQ(commandBuffer.lastObjectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(commandBuffer.lastObjectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::UniformBuffer);

    ASSERT_TRUE(provider.PrepareDraw(pso, indexedDrawable));
    provider.PrepareExplicitDraw(commandBuffer, pso, indexedDrawable);
    EXPECT_EQ(commandBuffer.lastObjectBindingSet, indexedBindings.objectBindingSet);
    ASSERT_NE(commandBuffer.lastObjectBindingSet, nullptr);
    ASSERT_EQ(commandBuffer.lastObjectBindingSet->GetDesc().entries.size(), 1u);
    EXPECT_EQ(commandBuffer.lastObjectBindingSet->GetDesc().entries[0].type,
        NLS::Render::RHI::BindingType::StructuredBuffer);

    provider.EndFrame();
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderUploadsObjectIndexRangeForInstancedIndexedDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderUploadsMaterialOwnedGpuInstanceRangeForIndexedDraw)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    EXPECT_EQ(bindings.objectConstants.objectIndex, 0u);

    ASSERT_NE(bindings.objectBindingSet, nullptr);
    ASSERT_EQ(bindings.objectBindingSet->GetDesc().entries.size(), 2u);
    const auto& objectEntry = bindings.objectBindingSet->GetDesc().entries[0];
    EXPECT_EQ(objectEntry.type, NLS::Render::RHI::BindingType::StructuredBuffer);
    auto objectBuffer = std::dynamic_pointer_cast<TestBuffer>(objectEntry.buffer);
    ASSERT_NE(objectBuffer, nullptr);
    EXPECT_EQ(objectBuffer->updateCalls, 1u);
    EXPECT_EQ(objectBuffer->lastUpdate.destinationOffset, 11u * sizeof(NLS::Maths::Matrix4));
    EXPECT_EQ(objectBuffer->lastUpdate.dataSize, 3u * sizeof(NLS::Maths::Matrix4));

    const auto& indirectionEntry = bindings.objectBindingSet->GetDesc().entries[1];
    EXPECT_EQ(indirectionEntry.type, NLS::Render::RHI::BindingType::StructuredBuffer);
    EXPECT_EQ(indirectionEntry.buffer->GetDebugName(), "EngineInstanceObjectIndexBuffer");
    auto indirectionBuffer = std::dynamic_pointer_cast<TestBuffer>(indirectionEntry.buffer);
    ASSERT_NE(indirectionBuffer, nullptr);
    ASSERT_GE(indirectionBuffer->uploadData.size(), 3u * sizeof(uint32_t));
    std::array<uint32_t, 3u> resolvedObjectSlots {};
    std::memcpy(resolvedObjectSlots.data(), indirectionBuffer->uploadData.data(), sizeof(resolvedObjectSlots));
    EXPECT_EQ(resolvedObjectSlots[0], 11u);
    EXPECT_EQ(resolvedObjectSlots[1], 12u);
    EXPECT_EQ(resolvedObjectSlots[2], 13u);

    NLS::Maths::Matrix4 capturedThird;
    const auto baseOffset = (11u + 2u) * sizeof(NLS::Maths::Matrix4);
    ASSERT_GE(objectBuffer->uploadData.size(), baseOffset + sizeof(capturedThird));
    std::memcpy(&capturedThird, objectBuffer->uploadData.data() + baseOffset, sizeof(capturedThird));
    const auto expectedShaderMatrix = NLS::Maths::Matrix4::Transpose(modelMatrix);
    for (size_t index = 0u; index < std::size(capturedThird.data); ++index)
        EXPECT_FLOAT_EQ(capturedThird.data[index], expectedShaderMatrix.data[index]);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderUsesLegacyObjectConstantsWhenShaderLacksObjectData)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    // A shader without the indexed object-data ABI still gets its constants from
    // the per-frame ring, addressed by the block offset the binding names.
    EXPECT_EQ(objectEntry.buffer->GetDebugName(), "EngineLegacyObjectConstantsRing");
    EXPECT_EQ(objectEntry.bufferOffset % 256u, 0u);
    EXPECT_EQ(objectEntry.bufferRange, sizeof(NLS::Maths::Matrix4));

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.EndFrame();
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderReusesLegacyObjectBindingsForReusedFrameSlot)
{
    LegacyObjectBindingFixture fixture;
    std::array<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>, 2u> firstBindings;
    for (size_t frame = 0u; frame < 8u; ++frame)
    {
        SCOPED_TRACE(frame);
        fixture.BeginFrame(0u);
        for (size_t draw = 0u; draw < firstBindings.size(); ++draw)
        {
            const auto model = NLS::Maths::Matrix4::Translation({
                static_cast<float>(frame), static_cast<float>(draw), 3.0f
            });
            const auto binding = fixture.Capture(model);
            ASSERT_NE(binding, nullptr);
            ExpectLegacyObjectMatrix(binding, model);
            if (frame == 0u)
                firstBindings[draw] = binding;
            else
                EXPECT_EQ(binding, firstBindings[draw]);
        }
        EXPECT_NE(firstBindings[0], firstBindings[1]);
        EXPECT_NE(firstBindings[0]->GetDesc().entries.front().bufferOffset,
            firstBindings[1]->GetDesc().entries.front().bufferOffset);
        EXPECT_EQ(fixture.ObjectBindingCreationCount(), 2u)
            << "Stable ring blocks must not allocate another descriptor set every frame.";
        const auto descriptorStats = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(
            fixture.driver, 0u).descriptorAllocator->GetStats();
        EXPECT_GE(descriptorStats.persistentUsed, 2u);
        EXPECT_EQ(descriptorStats.transientUsed, 0u);
        fixture.EndFrame();
    }
}

TEST(RendererFrameObjectBindingTests, EngineProviderKeepsLegacyObjectBindingsIsolatedAcrossFrameSlots)
{
    LegacyObjectBindingFixture fixture;
    const auto firstModel = NLS::Maths::Matrix4::Translation({ 2.0f, 3.0f, 5.0f });
    const auto secondModel = NLS::Maths::Matrix4::Translation({ 7.0f, 11.0f, 13.0f });
    fixture.BeginFrame(0u);
    const auto first = fixture.Capture(firstModel);
    ASSERT_NE(first, nullptr);
    fixture.EndFrame();
    fixture.BeginFrame(1u);
    const auto second = fixture.Capture(secondModel);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_NE(first->GetDesc().entries.front().buffer, second->GetDesc().entries.front().buffer);
    ExpectLegacyObjectMatrix(first, firstModel);
    ExpectLegacyObjectMatrix(second, secondModel);
    fixture.EndFrame();

    fixture.BeginFrame(0u);
    const auto updated = fixture.Capture(secondModel);
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated, first);
    ExpectLegacyObjectMatrix(updated, secondModel);
    ExpectLegacyObjectMatrix(second, secondModel);
    EXPECT_EQ(fixture.ObjectBindingCreationCount(), 2u);
    fixture.EndFrame();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRebuildsLegacyObjectBindingsWhenRingGrows)
{
    LegacyObjectBindingFixture fixture;
    const auto originalModel = NLS::Maths::Matrix4::Translation({ 2.0f, 3.0f, 5.0f });
    const auto grownModel = NLS::Maths::Matrix4::Translation({ 7.0f, 11.0f, 13.0f });
    fixture.BeginFrame(0u);
    const auto original = fixture.Capture(originalModel);
    ASSERT_NE(original, nullptr);
    const auto originalBuffer = original->GetDesc().entries.front().buffer;
    const auto blockCapacity = originalBuffer->GetDesc().size / 256u;
    ASSERT_GT(blockCapacity, 0u);
    ASSERT_LT(blockCapacity, 2048u);
    for (size_t block = 1u; block < blockCapacity; ++block)
        ASSERT_NE(fixture.Capture(originalModel), nullptr);
    const auto grown = fixture.Capture(grownModel);
    ASSERT_NE(grown, nullptr);
    EXPECT_NE(grown->GetDesc().entries.front().buffer, originalBuffer);
    ExpectLegacyObjectMatrix(original, originalModel);
    ExpectLegacyObjectMatrix(grown, grownModel);
    const auto creationsAfterGrowth = fixture.ObjectBindingCreationCount();
    fixture.EndFrame();

    fixture.BeginFrame(0u);
    const auto reused = fixture.Capture(originalModel);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused, grown);
    EXPECT_NE(reused->GetDesc().entries.front().buffer, originalBuffer);
    EXPECT_EQ(fixture.ObjectBindingCreationCount(), creationsAfterGrowth);
    ExpectLegacyObjectMatrix(original, originalModel);
    ExpectLegacyObjectMatrix(reused, originalModel);
    fixture.EndFrame();
}

TEST(RendererFrameObjectBindingTests, EngineProviderInvalidatesLegacyObjectBindingsWhenDeviceChanges)
{
    LegacyObjectBindingFixture fixture;
    const auto originalModel = NLS::Maths::Matrix4::Translation({ 2.0f, 3.0f, 5.0f });
    const auto updatedModel = NLS::Maths::Matrix4::Translation({ 7.0f, 11.0f, 13.0f });
    fixture.BeginFrame(0u);
    const auto original = fixture.Capture(originalModel);
    ASSERT_NE(original, nullptr);
    fixture.EndFrame();
    fixture.device = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(fixture.driver, fixture.device);
    fixture.BeginFrame(0u);
    const auto updated = fixture.Capture(updatedModel);
    ASSERT_NE(updated, nullptr);
    EXPECT_NE(updated, original);
    EXPECT_NE(updated->GetDesc().layout, original->GetDesc().layout);
    EXPECT_NE(updated->GetDesc().entries.front().buffer, original->GetDesc().entries.front().buffer);
    ExpectLegacyObjectMatrix(original, originalModel);
    ExpectLegacyObjectMatrix(updated, updatedModel);
    EXPECT_EQ(fixture.ObjectBindingCreationCount(), 1u);
    fixture.EndFrame();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsIndexedShaderWhenObjectDataRangeCannotBePrepared)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsIndexedShaderWhenObjectDataRangeExceedsSharedLimit)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsInvalidObjectIndexAfterObjectDataSlotIsFull)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

#if defined(NLS_ENABLE_TEST_HOOKS)
    const ScopedObjectDataCountLimitOverride scopedLimit(3u);
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to override object-data limits.";
#endif
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsIndexedShaderBeforeAllocatingWhenInstanceMatricesAreIncomplete)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRejectsIndexedShaderWhenObjectDescriptorIsMissing)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderUsesReflectionObjectDataShaderForIndexedDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderKeepsCaptureStateIsolatedPerFrameSlot)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 61u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    NLS::Render::Data::PipelineState pso;
    auto captureInSlot = [&](const size_t slotIndex, const uint32_t objectIndex)
    {
        provider.BindCaptureFrameSlot(slotIndex);

        NLS::Render::Entities::Drawable drawable;
        drawable.material = &material;
        drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>({
            NLS::Maths::Matrix4::Translation({ static_cast<float>(objectIndex), 0.0f, 0.0f }),
            NLS::Maths::Matrix4::Identity,
            objectIndex
        });

        EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
        EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
        return bindings;
    };

    const auto slotZeroFirst = captureInSlot(0u, 5u);
    EXPECT_EQ(provider.GetBoundCaptureFrameSlotForTesting(), std::optional<size_t>(0u));
    ASSERT_NE(slotZeroFirst.frameBindingSet, nullptr);
    ASSERT_NE(slotZeroFirst.objectBindingSet, nullptr);
    EXPECT_EQ(slotZeroFirst.objectConstants.objectIndex, 5u);

    // A second frame slot captures its own draws without disturbing slot zero.
    const auto slotOne = captureInSlot(1u, 9u);
    EXPECT_EQ(provider.GetBoundCaptureFrameSlotForTesting(), std::optional<size_t>(1u));
    ASSERT_NE(slotOne.frameBindingSet, nullptr);
    EXPECT_EQ(slotOne.objectConstants.objectIndex, 9u);
    EXPECT_NE(slotOne.frameBindingSet, slotZeroFirst.frameBindingSet);

    // Re-capturing slot zero reuses slot zero's own frame binding set: the slot-one
    // capture neither rebuilt nor replaced it, which is the property render-thread
    // capture needs once the two frames overlap.
    const auto slotZeroAgain = captureInSlot(0u, 5u);
    EXPECT_EQ(slotZeroAgain.frameBindingSet, slotZeroFirst.frameBindingSet);
    EXPECT_EQ(slotZeroAgain.objectConstants.objectIndex, 5u);

    // Slot one kept its own captured state as well.
    const auto slotOneAgain = captureInSlot(1u, 9u);
    EXPECT_EQ(slotOneAgain.frameBindingSet, slotOne.frameBindingSet);
    EXPECT_EQ(slotOneAgain.objectConstants.objectIndex, 9u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    provider.UnbindCaptureFrameSlot();
    EXPECT_FALSE(provider.GetBoundCaptureFrameSlotForTesting().has_value());
    provider.EndFrame();
    explicitFrameResources.reset();
}
TEST(RendererFrameObjectBindingTests, EngineProviderKeepsIndexedObjectDataShaderSupportCacheAcrossFrames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderSkipsUnchangedObjectDataUploadWhenFrameSlotIsReused)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRevisionReuseSkipsChangedMatrixUploadInSameFrameSlot)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 59u;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    NLS::Render::Data::PipelineState pso;

    renderer.ResetFrameStatistics();
    const auto prepareObject = [&](
        NLS::Engine::Rendering::EngineDrawableDescriptor descriptor,
        std::vector<NLS::Maths::Matrix4> instanceMatrices = {})
        -> std::shared_ptr<TestBuffer>
    {
        provider.BeginFrame(frameDescriptor);
        NLS::Render::Entities::Drawable drawable;
        if (!instanceMatrices.empty())
        {
            drawable.instanceCount = static_cast<uint32_t>(instanceMatrices.size());
            descriptor.instanceModelMatrices = std::move(instanceMatrices);
        }
        drawable.AddDescriptor(std::move(descriptor));
        EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
        EXPECT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, bindings));
        provider.EndFrame();
        return std::dynamic_pointer_cast<TestBuffer>(
            bindings.objectBindingSet->GetDesc().entries[0].buffer);
    };

    NLS::Engine::Rendering::EngineDrawableDescriptor trustedDescriptor;
    trustedDescriptor.modelMatrix = NLS::Maths::Matrix4::Identity;
    trustedDescriptor.objectIndex = 12u;
    trustedDescriptor.objectCount = 1u;
    trustedDescriptor.hasTrustedStaticDrawRevision = true;
    trustedDescriptor.stableSceneIdentity = { 42u, 1u, 1u };
    trustedDescriptor.transformRevision = 7u;
    trustedDescriptor.allowsSingleObjectDataReuse = true;

    auto objectBuffer = prepareObject(trustedDescriptor);
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_EQ(objectBuffer->updateCalls, 1u);
    const auto primeWork = provider.GetObjectDataWorkCountsForTesting();
    EXPECT_EQ(primeWork.validityScanCount, 0u);
    EXPECT_EQ(primeWork.memcmpCount, 0u);
    EXPECT_EQ(primeWork.transposeCount, 1u);
    EXPECT_EQ(primeWork.uploadCount, 1u);

    auto changedBytesSameRevision = trustedDescriptor;
    changedBytesSameRevision.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 9.0f, 8.0f, 7.0f });
    auto reusedBuffer = prepareObject(changedBytesSameRevision);
    ASSERT_EQ(reusedBuffer, objectBuffer);
    EXPECT_EQ(objectBuffer->updateCalls, 1u);
    const auto hitWork = provider.GetObjectDataWorkCountsForTesting();
    EXPECT_EQ(hitWork.validityScanCount, primeWork.validityScanCount);
    EXPECT_EQ(hitWork.memcmpCount, primeWork.memcmpCount);
    EXPECT_EQ(hitWork.transposeCount, primeWork.transposeCount);
    EXPECT_EQ(hitWork.uploadCount, primeWork.uploadCount);

    auto sparseStableDescriptor = trustedDescriptor;
    sparseStableDescriptor.objectIndex = 3u;
    auto sparseBuffer = prepareObject(sparseStableDescriptor);
    ASSERT_EQ(sparseBuffer, objectBuffer);
    EXPECT_EQ(objectBuffer->updateCalls, 2u);

    const auto expectFallbackUpload = [&](
        NLS::Engine::Rendering::EngineDrawableDescriptor descriptor,
        std::vector<NLS::Maths::Matrix4> instanceMatrices = {})
    {
        const auto before = provider.GetObjectDataWorkCountsForTesting();
        auto buffer = prepareObject(std::move(descriptor), std::move(instanceMatrices));
        ASSERT_NE(buffer, nullptr);
        const auto after = provider.GetObjectDataWorkCountsForTesting();
        EXPECT_EQ(after.transposeCount, before.transposeCount + 1u);
        EXPECT_EQ(after.uploadCount, before.uploadCount + 1u);
    };

    auto changedTransformRevision = trustedDescriptor;
    changedTransformRevision.transformRevision = 8u;
    changedTransformRevision.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f });
    expectFallbackUpload(changedTransformRevision);

    auto changedObjectIndex = trustedDescriptor;
    changedObjectIndex.objectIndex = 13u;
    changedObjectIndex.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 2.0f, 0.0f, 0.0f });
    expectFallbackUpload(changedObjectIndex);

    auto changedObjectCount = trustedDescriptor;
    changedObjectCount.objectCount = 2u;
    expectFallbackUpload(
        changedObjectCount,
        {
            NLS::Maths::Matrix4::Translation({ 3.0f, 0.0f, 0.0f }),
            NLS::Maths::Matrix4::Translation({ 4.0f, 0.0f, 0.0f })
        });

    auto mergedGroup = trustedDescriptor;
    mergedGroup.groupIdentity = 71u;
    mergedGroup.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 5.0f, 0.0f, 0.0f });
    expectFallbackUpload(mergedGroup);

    auto reuseDisallowed = trustedDescriptor;
    reuseDisallowed.allowsSingleObjectDataReuse = false;
    reuseDisallowed.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 6.0f, 0.0f, 0.0f });
    expectFallbackUpload(reuseDisallowed);

    auto untrusted = trustedDescriptor;
    untrusted.hasTrustedStaticDrawRevision = false;
    untrusted.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 7.0f, 0.0f, 0.0f });
    expectFallbackUpload(untrusted);

    auto untrustedSparse = sparseStableDescriptor;
    untrustedSparse.hasTrustedStaticDrawRevision = false;
    untrustedSparse.allowsSingleObjectDataReuse = false;
    untrustedSparse.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 8.0f, 0.0f, 0.0f });
    expectFallbackUpload(untrustedSparse);
    auto repairedSparseBuffer = prepareObject(sparseStableDescriptor);
    ASSERT_EQ(repairedSparseBuffer, objectBuffer);

    renderer.FinalizeFrameStatistics();
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionReuseHitCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionReuseFallbackCount, 10u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionDescriptorFallbackCount, 5u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionMetadataUnavailableCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionMetadataUninitializedCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionMetadataMismatchCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionMetadataInvalidCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionStableIdentityMismatchCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionTransformMismatchCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionObjectIndexMismatchCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataRevisionObjectCountMismatchCount, 0u);

    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRevisionMetadataIsSlotLocalAndInvalidatedByStorageChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto firstDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, firstDevice);
    for (size_t slotIndex = 0u; slotIndex < 2u; ++slotIndex)
    {
        auto& frameContext =
            NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, slotIndex);
        frameContext.frameIndex = 70u + slotIndex;
        frameContext.descriptorAllocator =
            NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
        ASSERT_NE(frameContext.descriptorAllocator, nullptr);
        frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    }
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    NLS::Render::Data::PipelineState pso;

    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    descriptor.modelMatrix = NLS::Maths::Matrix4::Identity;
    descriptor.objectIndex = 12u;
    descriptor.objectCount = 1u;
    descriptor.hasTrustedStaticDrawRevision = true;
    descriptor.stableSceneIdentity = { 42u, 1u, 1u };
    descriptor.transformRevision = 7u;
    descriptor.allowsSingleObjectDataReuse = true;

    const auto prepareInSlot = [&](
        const size_t slotIndex,
        NLS::Engine::Rendering::EngineDrawableDescriptor drawDescriptor)
        -> std::shared_ptr<TestBuffer>
    {
        provider.SetActiveObjectDataSlotIndexForTesting(slotIndex);
        provider.BeginFrame(frameDescriptor);
        NLS::Render::Entities::Drawable drawable;
        drawable.AddDescriptor(std::move(drawDescriptor));
        EXPECT_TRUE(provider.PrepareDraw(pso, drawable));
        NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindings;
        if (!provider.CapturePreparedBindingSets(pso, drawable, bindings))
        {
            ADD_FAILURE() << "Failed to capture prepared bindings for slot " << slotIndex;
            provider.EndFrame();
            return nullptr;
        }
        provider.EndFrame();
        return std::dynamic_pointer_cast<TestBuffer>(
            bindings.objectBindingSet->GetDesc().entries[0].buffer);
    };

    auto slot0Buffer = prepareInSlot(0u, descriptor);
    ASSERT_NE(slot0Buffer, nullptr);
    const auto afterSlot0Prime = provider.GetObjectDataWorkCountsForTesting();

    auto slot1Descriptor = descriptor;
    slot1Descriptor.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f });
    auto slot1Buffer = prepareInSlot(1u, slot1Descriptor);
    ASSERT_NE(slot1Buffer, nullptr);
    EXPECT_NE(slot1Buffer, slot0Buffer);
    const auto afterSlot1Fallback = provider.GetObjectDataWorkCountsForTesting();
    EXPECT_EQ(afterSlot1Fallback.transposeCount, afterSlot0Prime.transposeCount + 1u);
    EXPECT_EQ(afterSlot1Fallback.uploadCount, afterSlot0Prime.uploadCount + 1u);

    auto slot0ReusedBuffer = prepareInSlot(0u, slot1Descriptor);
    EXPECT_EQ(slot0ReusedBuffer, slot0Buffer);
    const auto afterSlot0Hit = provider.GetObjectDataWorkCountsForTesting();
    EXPECT_EQ(afterSlot0Hit.validityScanCount, afterSlot1Fallback.validityScanCount);
    EXPECT_EQ(afterSlot0Hit.memcmpCount, afterSlot1Fallback.memcmpCount);
    EXPECT_EQ(afterSlot0Hit.transposeCount, afterSlot1Fallback.transposeCount);
    EXPECT_EQ(afterSlot0Hit.uploadCount, afterSlot1Fallback.uploadCount);

    provider.ResetObjectDataSlotForTesting(0u);
    auto resetBuffer = prepareInSlot(0u, descriptor);
    ASSERT_NE(resetBuffer, nullptr);
    EXPECT_NE(resetBuffer, slot0Buffer);
    const auto afterResetFallback = provider.GetObjectDataWorkCountsForTesting();
    EXPECT_EQ(afterResetFallback.transposeCount, afterSlot0Hit.transposeCount + 1u);
    EXPECT_EQ(afterResetFallback.uploadCount, afterSlot0Hit.uploadCount + 1u);

    auto growthDescriptor = descriptor;
    growthDescriptor.stableSceneIdentity = { 43u, 2u, 1u };
    growthDescriptor.transformRevision = 1u;
    growthDescriptor.objectIndex = 300u;
    growthDescriptor.modelMatrix =
        NLS::Maths::Matrix4::Translation({ 2.0f, 0.0f, 0.0f });
    ASSERT_NE(prepareInSlot(0u, growthDescriptor), nullptr);
    const auto afterGrowth = provider.GetObjectDataWorkCountsForTesting();
    const auto hitCountBeforePreservedDraw =
        renderer.GetFrameInfo().objectDataRevisionReuseHitCount;
    const auto fallbackCountBeforePreservedDraw =
        renderer.GetFrameInfo().objectDataRevisionReuseFallbackCount;
    auto preservedBuffer = prepareInSlot(0u, descriptor);
    ASSERT_NE(preservedBuffer, nullptr);
    const auto afterGrowthReuse = provider.GetObjectDataWorkCountsForTesting();
    EXPECT_EQ(afterGrowthReuse.validityScanCount, afterGrowth.validityScanCount);
    EXPECT_EQ(afterGrowthReuse.memcmpCount, afterGrowth.memcmpCount);
    EXPECT_EQ(afterGrowthReuse.transposeCount, afterGrowth.transposeCount);
    EXPECT_EQ(afterGrowthReuse.uploadCount, afterGrowth.uploadCount);
    EXPECT_EQ(
        renderer.GetFrameInfo().objectDataRevisionReuseHitCount,
        hitCountBeforePreservedDraw + 1u);
    EXPECT_EQ(
        renderer.GetFrameInfo().objectDataRevisionReuseFallbackCount,
        fallbackCountBeforePreservedDraw);

    const auto hitCountBeforeRearmedDraw =
        renderer.GetFrameInfo().objectDataRevisionReuseHitCount;
    const auto fallbackCountAfterGrowthComparison =
        renderer.GetFrameInfo().objectDataRevisionReuseFallbackCount;
    auto rearmedBuffer = prepareInSlot(0u, descriptor);
    ASSERT_EQ(rearmedBuffer, preservedBuffer);
    const auto afterRearmedHit = provider.GetObjectDataWorkCountsForTesting();
    EXPECT_EQ(afterRearmedHit.validityScanCount, afterGrowthReuse.validityScanCount);
    EXPECT_EQ(afterRearmedHit.memcmpCount, afterGrowthReuse.memcmpCount);
    EXPECT_EQ(afterRearmedHit.transposeCount, afterGrowthReuse.transposeCount);
    EXPECT_EQ(afterRearmedHit.uploadCount, afterGrowthReuse.uploadCount);
    EXPECT_EQ(
        renderer.GetFrameInfo().objectDataRevisionReuseHitCount,
        hitCountBeforeRearmedDraw + 1u);
    EXPECT_EQ(
        renderer.GetFrameInfo().objectDataRevisionReuseFallbackCount,
        fallbackCountAfterGrowthComparison);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
    auto& secondDeviceFrameContext =
        NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    secondDeviceFrameContext.frameIndex = 80u;
    secondDeviceFrameContext.descriptorAllocator =
        NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
    ASSERT_NE(secondDeviceFrameContext.descriptorAllocator, nullptr);
    secondDeviceFrameContext.descriptorAllocator->BeginFrame(secondDeviceFrameContext.frameIndex);
    auto deviceChangedBuffer = prepareInSlot(0u, descriptor);
    ASSERT_NE(deviceChangedBuffer, nullptr);
    EXPECT_NE(deviceChangedBuffer, preservedBuffer);
    const auto afterDeviceFallback = provider.GetObjectDataWorkCountsForTesting();
    EXPECT_EQ(afterDeviceFallback.transposeCount, afterGrowthReuse.transposeCount + 1u);
    EXPECT_EQ(afterDeviceFallback.uploadCount, afterGrowthReuse.uploadCount + 1u);

    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, SpatialVisibilityPipelineDrawsKeepRendererAssignedObjectIndices)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(renderScene, scene, syncOptions).rebuiltCachedCommandCount, 1u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, EngineProviderRebuildsIndexedObjectDataResourcesWhenExplicitDeviceChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.emplace(driver, 0u);

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
    explicitFrameResources.reset();
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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.emplace(driver, 0u);

    ASSERT_TRUE(provider.PrepareDraw(pso, drawable));
    NLS::Render::Core::FrameObjectBindingProvider::PreparedBindingSets secondBindings;
    ASSERT_TRUE(provider.CapturePreparedBindingSets(pso, drawable, secondBindings));
    provider.EndFrame();

    ASSERT_NE(secondBindings.frameBindingSet, nullptr);
    ASSERT_NE(secondBindings.objectBindingSet, nullptr);
    EXPECT_NE(secondBindings.frameBindingSet, firstFrameBindingSet);
    EXPECT_NE(secondBindings.objectBindingSet, firstObjectBindingSet);
    EXPECT_GE(secondDevice->bindingSetCreateCalls, 2u);

    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, ReflectionObjectDataRequiresMatrixStrideForIndexedDraw)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, ImmediateIndexedShaderDrawIsSkippedWhenObjectDataRangeCannotBePrepared)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
}

TEST(RendererFrameObjectBindingTests, EngineProviderKeepsIndexedObjectBuffersIsolatedAcrossFrameSlots)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    for (size_t slotIndex = 0u; slotIndex < 2u; ++slotIndex)
    {
        auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, slotIndex);
        frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(32u);
        ASSERT_NE(frameContext.descriptorAllocator, nullptr);
        frameContext.descriptorAllocator->BeginFrame(slotIndex + 1u);
    }

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    provider.BeginFrame(frameDescriptor);
    provider.BindCaptureFrameSlot(0u);

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

    provider.UnbindCaptureFrameSlot();
    provider.EndFrame();

    provider.BeginFrame(frameDescriptor);
    provider.BindCaptureFrameSlot(1u);

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

    provider.UnbindCaptureFrameSlot();
    provider.EndFrame();
}

TEST(RendererFrameObjectBindingTests, EngineProviderFailsClosedWhenPreparedThreadedSlotsAreBackPressured)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

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
        return buffer;
    };

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    const auto secondMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });
    auto firstBuffer = prepareFrame(firstMatrix);
    ASSERT_NE(firstBuffer, nullptr);

    NLS::Render::Context::RenderFramePacket firstSnapshot;
    firstSnapshot.frameId = 101u;
    NLS::Render::Context::RenderScenePackage firstPackage;
    firstPackage.frameId = firstSnapshot.frameId;
    size_t firstPublishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        firstSnapshot,
        [firstPackage](size_t) { return firstPackage; },
        &firstPublishedSlot));
    provider.EndFrame();
    EXPECT_EQ(firstPublishedSlot, 0u);

    auto secondBuffer = prepareFrame(secondMatrix);
    ASSERT_NE(secondBuffer, nullptr);
    EXPECT_NE(secondBuffer, firstBuffer);

    NLS::Render::Context::RenderFramePacket secondSnapshot;
    secondSnapshot.frameId = 102u;
    NLS::Render::Context::RenderScenePackage secondPackage;
    secondPackage.frameId = secondSnapshot.frameId;
    size_t secondPublishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        secondSnapshot,
        [secondPackage](size_t) { return secondPackage; },
        &secondPublishedSlot));
    provider.EndFrame();
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
    settings.framesInFlight = 2u;

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

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    const auto publishPreparedFrame = [&](const uint64_t frameId, const size_t expectedSlot)
    {
        NLS::Render::Context::RenderFramePacket snapshot;
        snapshot.frameId = frameId;
        NLS::Render::Context::RenderScenePackage package;
        package.frameId = snapshot.frameId;
        size_t publishedSlot = 99u;
        ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
            driver,
            snapshot,
            [package](size_t) { return package; },
            &publishedSlot));
        EXPECT_EQ(publishedSlot, expectedSlot);
    };

    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.TryReservePreparedFrameResources());
    publishPreparedFrame(301u, 0u);
    provider.EndFrame();

    provider.BeginFrame(frameDescriptor);
    ASSERT_TRUE(provider.TryReservePreparedFrameResources());
    publishPreparedFrame(302u, 1u);
    provider.EndFrame();

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

TEST(RendererFrameObjectBindingTests, EngineProviderWaitsForFrameBudgetWithoutDroppingSlowFrames)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    auto* controller = DriverTestAccess::GetImplForTesting(driver)->frameLatency.get();
    ASSERT_NE(controller, nullptr);
    ASSERT_TRUE(controller->BeginFrame());

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frame;
    frame.renderWidth = 256u;
    frame.renderHeight = 144u;
    frame.camera = &camera;
    provider.BeginFrame(frame);

    auto reservation = std::async(std::launch::async, [&provider]
        { return provider.TryReservePreparedFrameResources(); });
    const auto observationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (controller->GetStalledFrameCount() == 0u && std::chrono::steady_clock::now() < observationDeadline)
        std::this_thread::yield();
    EXPECT_EQ(controller->GetStalledFrameCount(), 1u);
    EXPECT_EQ(reservation.wait_for(std::chrono::milliseconds(32)), std::future_status::timeout);
    EXPECT_TRUE(controller->CompleteOldestFrame());
    if (reservation.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
        controller->Stop();
    EXPECT_TRUE(reservation.get());
    EXPECT_EQ(controller->GetStallTimeoutCount(), 0u);
    EXPECT_EQ(controller->GetInFlightFrameCount(), 1u);
    EXPECT_TRUE(provider.HasReservedPreparedFrameResources());
    provider.EndFrame();
    EXPECT_EQ(controller->GetInFlightFrameCount(), 0u);
}

TEST(RendererFrameObjectBindingTests, DriverReservationSharesRemainingDeadlineWithDeferredFence)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto fence = std::make_shared<TestFence>("RemainingDeadlineFence");
    fence->Reset();
    fence->SetWaitSucceeds(false);
    frameContext.frameFence = fence;

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl->frameResourceSlots, nullptr);
    size_t slotIndex = 99u;
    {
        std::lock_guard retirementLock(impl->frameResourceRetirementMutex);
        auto* slot = impl->frameResourceSlots->AcquireSlot();
        ASSERT_NE(slot, nullptr);
        slotIndex = slot->slotIndex;
    }

    std::atomic_bool retireThreadReady{ false };
    std::atomic_bool startRetirementDelay{ false };
    std::thread retireThread([impl, fence, slotIndex, &retireThreadReady, &startRetirementDelay]()
    {
        retireThreadReady.store(true, std::memory_order_release);
        while (!startRetirementDelay.load(std::memory_order_acquire))
            std::this_thread::yield();
        const auto retireAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        while (std::chrono::steady_clock::now() < retireAt)
            std::this_thread::yield();

        {
            std::lock_guard retirementLock(impl->frameResourceRetirementMutex);
            impl->frameResourceSlots->RetireSlot(slotIndex, fence);
        }
    });

    while (!retireThreadReady.load(std::memory_order_acquire))
        std::this_thread::yield();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
    startRetirementDelay.store(true, std::memory_order_release);
    const auto reservedSlot =
        NLS::Render::Context::DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
            driver,
            deadline);
    retireThread.join();

    EXPECT_FALSE(reservedSlot.has_value());
    EXPECT_GT(fence->GetWaitCount(), 0u);
    // The wait count is scheduling dependent: the deferred test fence returns
    // immediately, so a loaded host performs many bounded waits inside the same
    // deadline. The per-wait bound below is the contract this test guards.
    EXPECT_GT(fence->GetLastWaitTimeoutNanoseconds(), 0u);
    EXPECT_LE(fence->GetLastWaitTimeoutNanoseconds(), 4'000'000u);
    EXPECT_FALSE(
        NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());
    EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(slotIndex));
    EXPECT_EQ(impl->frameResourceSlots->PeekSlot(slotIndex)->gpuFence, fence);
    fence->SetWaitSucceeds(true);
    EXPECT_TRUE(fence->Wait());
}

TEST(RendererFrameObjectBindingTests, EngineProviderHonorsExpiredPreparedResourceDeadline)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl->frameResourceSlots, nullptr);
    size_t occupiedSlot = 99u;
    {
        std::lock_guard retirementLock(impl->frameResourceRetirementMutex);
        auto* slot = impl->frameResourceSlots->AcquireSlot();
        ASSERT_NE(slot, nullptr);
        occupiedSlot = slot->slotIndex;
    }

    ProviderAwareRenderer renderer(driver);
    NLS::Engine::Rendering::EngineFrameObjectBindingProvider provider(renderer);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    provider.BeginFrame(frameDescriptor);

    const auto callStart = std::chrono::steady_clock::now();
    EXPECT_FALSE(provider.TryReservePreparedFrameResourcesUntil(
        callStart - std::chrono::milliseconds(1)));
    EXPECT_LT(std::chrono::steady_clock::now() - callStart, std::chrono::milliseconds(5));
    EXPECT_FALSE(
        NLS::Render::Context::DriverRendererAccess::GetReservedFrameContextSlotIndex(driver).has_value());
    provider.EndFrame();

    EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(occupiedSlot));
    std::lock_guard retirementLock(impl->frameResourceRetirementMutex);
    impl->frameResourceSlots->RetireSlot(occupiedSlot, nullptr);
}

TEST(RendererFrameObjectBindingTests, EngineProviderCanReleasePreparedFrameReservationAfterPublishFailure)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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

    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 401u;
    NLS::Render::Context::RenderScenePackage package;
    package.frameId = snapshot.frameId;
    size_t publishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        snapshot,
        [package](size_t) { return package; },
        &publishedSlot));
    EXPECT_EQ(publishedSlot, 0u);

    provider.EndFrame();
}

TEST(RendererFrameObjectBindingTests, EngineProviderSelectionReservationGuardDoesNotReleaseExistingGBufferPreflight)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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

TEST(RendererFrameObjectBindingTests, EngineProviderReusesPreparedObjectBufferAfterFrameResourceRetires)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;

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
        return std::dynamic_pointer_cast<TestBuffer>(bindings.objectBindingSet->GetDesc().entries[0].buffer);
    };

    const auto firstMatrix = NLS::Maths::Matrix4::Translation({ 1.0f, 2.0f, 3.0f });
    const auto secondMatrix = NLS::Maths::Matrix4::Translation({ 4.0f, 5.0f, 6.0f });
    auto firstBuffer = prepareFrame(firstMatrix);
    ASSERT_NE(firstBuffer, nullptr);

    NLS::Render::Context::RenderFramePacket firstSnapshot;
    firstSnapshot.frameId = 201u;
    NLS::Render::Context::RenderScenePackage firstPackage;
    firstPackage.frameId = firstSnapshot.frameId;
    size_t firstPublishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        firstSnapshot,
        [firstPackage](size_t) { return firstPackage; },
        &firstPublishedSlot));
    provider.EndFrame();
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl->frameResourceSlots, nullptr);
    EXPECT_TRUE(impl->frameResourceSlots->IsSlotAvailable(firstPublishedSlot));

    auto secondBuffer = prepareFrame(secondMatrix);
    ASSERT_NE(secondBuffer, nullptr);
    EXPECT_NE(firstBuffer, secondBuffer);

    NLS::Render::Context::RenderFramePacket secondSnapshot;
    secondSnapshot.frameId = 202u;
    NLS::Render::Context::RenderScenePackage secondPackage;
    secondPackage.frameId = secondSnapshot.frameId;
    size_t secondPublishedSlot = 99u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(
        driver,
        secondSnapshot,
        [secondPackage](size_t) { return secondPackage; },
        &secondPublishedSlot));
    provider.EndFrame();
    EXPECT_NE(secondPublishedSlot, firstPublishedSlot);
    EXPECT_FALSE(impl->frameResourceSlots->IsSlotAvailable(secondPublishedSlot));

    const auto thirdMatrix = NLS::Maths::Matrix4::Translation({ 7.0f, 8.0f, 9.0f });
    auto thirdBuffer = prepareFrame(thirdMatrix);
    ASSERT_NE(thirdBuffer, nullptr);
    EXPECT_EQ(thirdBuffer, firstBuffer);
    provider.EndFrame();
}

TEST(RendererFrameObjectBindingTests, ImmediateIndexedDrawPushesReceiveShadowObjectConstantsAfterProviderBindsObjectData)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
    // Indexed object-data shaders address the per-draw instance indirection table.
    EXPECT_EQ(renderer.commandBuffer()->lastObjectIndexPushConstant, 0u);
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
    EXPECT_EQ(capturedConstants.objectIndex, 0u);
    EXPECT_EQ(capturedConstants.objectFlags,
        NLS::Render::Data::kDrawableObjectFlagReceiveShadows);
    EXPECT_EQ(capturedConstants.padding0, 0u);
    EXPECT_EQ(capturedConstants.padding1, 0u);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    renderer.EndFrame();
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
}

TEST(RendererFrameObjectBindingTests, ImmediateIndexedShaderDrawAssignsObjectIndexWhenMissing)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
}

TEST(RendererFrameObjectBindingTests, PreparedDrawPushesReceiveShadowObjectConstantsBeforeSubmission)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    const ScopedDriverService driverService(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 23u;
    frameContext.descriptorAllocator.reset();
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, MaterialExplicitBindingSetUsesProvidedDeviceInsteadOfLocatedDriver)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RendererFrameObjectBindingTests, MaterialResourceChangesPreserveLayoutsAndRefreshBindings)
{
    LegacyObjectBindingFixture fixture;
    auto* shader = CreateMaterialSamplerShader();
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    const auto firstLayout = material.GetExplicitPipelineLayout(fixture.device);
    const auto bindingLayout = material.GetExplicitBindingLayout(fixture.device);
    const auto firstBinding = material.GetExplicitBindingSet(fixture.device);
    ASSERT_NE(firstLayout, nullptr);
    ASSERT_NE(firstBinding, nullptr);

    NLS::Render::RHI::SamplerDesc sampler;
    sampler.minFilter = NLS::Render::RHI::TextureFilter::Nearest;
    material.SetSamplerOverride("u_MaterialSampler", sampler);
    EXPECT_EQ(material.GetExplicitPipelineLayout(fixture.device), firstLayout);
    EXPECT_EQ(material.GetExplicitBindingLayout(fixture.device), bindingLayout);
    EXPECT_NE(material.GetExplicitBindingSet(fixture.device), firstBinding);
    material.MarkParametersDirty();
    EXPECT_EQ(material.GetExplicitPipelineLayout(fixture.device), firstLayout);
    EXPECT_EQ(fixture.device->pipelineLayoutCreateCalls, 1u);

    auto otherDevice = std::make_shared<TestExplicitDevice>();
    EXPECT_NE(material.GetExplicitPipelineLayout(otherDevice), firstLayout);
    EXPECT_EQ(otherDevice->pipelineLayoutCreateCalls, 1u);
    EXPECT_EQ(material.GetExplicitPipelineLayout(fixture.device), firstLayout);
    material.RebuildBindingLayout();
    EXPECT_NE(material.GetExplicitPipelineLayout(fixture.device), firstLayout);
    EXPECT_EQ(fixture.device->pipelineLayoutCreateCalls, 2u);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, MaterialSetSamePointerValueDoesNotInvalidateExplicitBindings)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    material.MarkParametersDirty();
    EXPECT_EQ(material.GetExplicitPipelineLayout(firstDevice), nullptr);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutSupportsIndexedObjectDataOnVulkan)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = CreateReflectionOnlyImportedShader(
        "Tests/Synthetic/VulkanIndexedObjectData.hlsl",
        MakeIndexedObjectDataReflection(),
        "shader:vulkan-indexed-object-data");
    ASSERT_NE(shader, nullptr);
    ASSERT_TRUE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*shader));

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::Vulkan);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);
    EXPECT_EQ(explicitDevice->pipelineLayoutCreateCalls, 1u);
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

TEST(RendererFrameObjectBindingTests, MaterialPipelineLayoutUsesShadowObjectConstantsOnSupportedBackend)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
    const ScopedDriverService driverService(*driver);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/DeferredLighting.hlsl");
    ASSERT_NE(shader, nullptr);
    ASSERT_TRUE(shader->HasParameterStructs());

    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    explicitDevice->SetNativeBackendType(NLS::Render::RHI::NativeBackendType::Vulkan);

    const auto& pipelineLayout = material.GetExplicitPipelineLayout(explicitDevice);
    ASSERT_NE(pipelineLayout, nullptr);

    const auto& bindingLayouts = explicitDevice->lastPipelineLayoutDesc.bindingLayouts;
	// Vulkan requires empty descriptor-set placeholders so the vector index
	// remains the reflected register-space/set index.
	ASSERT_EQ(bindingLayouts.size(), 4u);
	ASSERT_NE(bindingLayouts[0], nullptr);
	ASSERT_NE(bindingLayouts[1], nullptr);
	ASSERT_NE(bindingLayouts[2], nullptr);
	ASSERT_NE(bindingLayouts[3], nullptr);
	bool foundMaterialConstants = false;
	bool foundForwardLightData = false;
	for (const auto& bindingLayout : bindingLayouts)
	{
		ASSERT_NE(bindingLayout, nullptr);
		for (const auto& entry : bindingLayout->GetDesc().entries)
		{
			if (entry.name == "MaterialConstants")
			{
				foundMaterialConstants = true;
				EXPECT_EQ(entry.set, NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet);
			}
			if (entry.name == "ForwardLightData")
			{
				foundForwardLightData = true;
				EXPECT_EQ(entry.set, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet);
			}
		}
	}
	EXPECT_TRUE(foundMaterialConstants);
	EXPECT_TRUE(foundForwardLightData);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RendererFrameObjectBindingTests, LightGridFallbackGraphicsBindingSetUsesShaderExpectedPassBufferNames)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, LightGridFallbackGraphicsBindingSetUsesCompactEmptyGridBuffers)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    explicitFrameResources.reset();
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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    explicitFrameResources.reset();
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    NLS::Render::Resources::MaterialPipelineStateOverrides colorOnlyOverrides;
    colorOnlyOverrides.depthTest = false;
    colorOnlyOverrides.depthWrite = false;
    colorOnlyOverrides.hasDepthAttachment = false;
    ASSERT_TRUE(renderer.Prepare(drawable, colorOnlyOverrides));
    const auto& colorOnlyDesc = explicitDevice->lastGraphicsPipelineDesc;
    EXPECT_FALSE(colorOnlyDesc.renderTargetLayout.hasDepth);
    EXPECT_FALSE(colorOnlyDesc.depthStencilState.depthTest);
    EXPECT_FALSE(colorOnlyDesc.depthStencilState.depthWrite);
    EXPECT_EQ(colorOnlyDesc.renderTargetLayout.sampleCount, 4u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    renderer.EndFrame();
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, PipelineStateRecordedDrawCanSelectShaderLabLightModePass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, PipelineStateRecordedDrawDefaultsShaderLabMaterialsToForwardPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, RecordedPipelineFailsClosedForMixedAttachmentSampleCounts)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, RecordedPipelineOverridesCanForceBlending)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 recorded material pipeline override test requires the phase-1 Windows DX12 runtime.";
#endif

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::DX12;
    settings.enableExplicitRHI = false;

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

TEST(RendererFrameObjectBindingTests, DisabledDepthStateKeepsExplicitRenderPassDepthAttachment)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/TestGraphics.shader");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material material(shader);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    auto pipelineCache = NLS::Render::RHI::CreateDefaultPipelineCache();

    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = false;
    overrides.depthWrite = false;
    overrides.stencilTest = false;
    overrides.hasDepthAttachment = true;
    overrides.depthFormat = NLS::Render::RHI::TextureFormat::Depth24Stencil8;

    NLS::Render::Data::PipelineState pipelineState;
    pipelineState.depthTest = false;
    pipelineState.depthWriting = false;
    pipelineState.stencilTest = false;

    const auto pipeline = material.BuildRecordedGraphicsPipeline(
        explicitDevice,
        pipelineCache,
        NLS::Render::Settings::EPrimitiveMode::TRIANGLES,
        pipelineState,
        overrides);

    ASSERT_NE(pipeline, nullptr);
    const auto& desc = explicitDevice->lastGraphicsPipelineDesc;
    EXPECT_TRUE(desc.renderTargetLayout.hasDepth);
    EXPECT_EQ(desc.renderTargetLayout.depthFormat, NLS::Render::RHI::TextureFormat::Depth24Stencil8);
    EXPECT_FALSE(desc.depthStencilState.depthTest);
    EXPECT_FALSE(desc.depthStencilState.depthWrite);
    EXPECT_FALSE(desc.depthStencilState.stencilTest);

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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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

    auto driver = std::make_unique<NLS::Render::Context::Driver>(settings);
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
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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
    class FrameBoundaryRenderer final : public NLS::Engine::Rendering::DeferredSceneRenderer
    {
    public:
        using DeferredSceneRenderer::DeferredSceneRenderer;
        void ResolveFrameBoundary(const NLS::Render::FrameGraph::FrozenFrameDescriptor& frame)
        {
            NLS::Render::Context::RenderSceneBuildData snapshot;
            DeferredThreadedCaptureInput capture;
            (void)ResolveThreadedSceneCapture(snapshot, capture, frame);
        }
    };
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    FrameBoundaryRenderer renderer(driver, options);

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
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveHitCount(renderer), 1u);
    const auto frozenFrame = NLS::Render::FrameGraph::FreezeFrameDescriptor(frameDescriptor);
    NLS::Tests::RunOnRenderThread([&]
    {
        renderer.ResolveFrameBoundary(frozenFrame);
    });

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
    settings.framesInFlight = 1u;

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
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    auto observedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));
    ASSERT_EQ(observedScene.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
    const auto package = observedScene.get();
    ASSERT_EQ(package.decalDrawCount, 1u);

    const auto expectedFragmentShader = decalPassShader->GetOrCreateExplicitShaderModule(
        explicitDevice,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel);
    ASSERT_NE(expectedFragmentShader, nullptr);
    const auto decalCommand = std::find_if(
        package.recordedDrawCommands.begin(),
        package.recordedDrawCommands.end(),
        [&](const auto& command)
        {
            return command.pipeline != nullptr &&
                command.pipeline->GetDesc().fragmentShader == expectedFragmentShader;
        });
    EXPECT_NE(decalCommand, package.recordedDrawCommands.end())
        << "Threaded decal capture must select the material's DeferredDecal LightMode pass.";
    if (decalCommand != package.recordedDrawCommands.end())
        ExpectDeferredDecalPipelineState(decalCommand->pipeline->GetDesc());

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);
    NLS::Render::Context::DriverTestAccess::DrainThreadedRendering(driver);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(decalPassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferPassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forwardPassShader));
}

TEST(RendererFrameObjectBindingTests, RenderCommandDeferredDecalDrawUsesDeferredDecalPassAndPipelineState)
{
    // Compiles the deferred decal pass through the real shader toolchain, so it
    // shares the native DXC requirement with the other deferred decal cases.
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
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
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));

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
        << "Render-command decal capture must select the material's DeferredDecal LightMode pass.";
    if (decalPipeline != explicitDevice->graphicsPipelineDescs.end())
        ExpectDeferredDecalPipelineState(*decalPipeline);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, fallbackShader);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(decalPassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(gbufferPassShader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(forwardPassShader));
}

TEST(RendererFrameObjectBindingTests, ThreadedDeferredDecalMissingFallbackSkipsOnlyUnresolvedDecal)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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

    auto preparedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));
    ASSERT_EQ(preparedScene.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    const auto package = preparedScene.get();
    EXPECT_EQ(package.decalDrawCount, 0u);
    EXPECT_EQ(package.recordedDrawCommands.size(), 1u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, decalShader);
}

TEST(RendererFrameObjectBindingTests, RenderCommandDeferredDecalMissingFallbackSkipsOnlyUnresolvedDecal)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
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

    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));
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
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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

    auto preparedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    ASSERT_NO_THROW(renderer.EndFrame());

    EXPECT_EQ(preparedScene.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));
    ASSERT_EQ(preparedScene.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    const auto package = preparedScene.get();
    EXPECT_EQ(package.opaqueDrawCount, 1u);
    EXPECT_EQ(package.decalDrawCount, 0u);

    NLS::Engine::Rendering::DeferredSceneRendererTestAccess::SetDeferredDecalShader(renderer, decalShader);
}

TEST(RendererFrameObjectBindingTests, ThreadedDeferredFramePublishFailsClosedWhenLightingDrawIsMissing)
{
    NLS::Render::Context::RenderSceneBuildData snapshot;
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
    NLS::Render::Context::RenderSceneBuildData snapshot;
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
    NLS::Render::Context::RenderSceneBuildData snapshot;
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

TEST(RendererFrameObjectBindingTests, ThreadedPreparedBindingSetCreationUsesBoundFrameDescriptorAllocator)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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

    const NLS::Render::Context::ScopedFrameResourceContext frameResources(driver, 0u);
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

TEST(RendererFrameObjectBindingTests, FrameResourceContextRestoresNestedScopeAfterException)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    DriverTestAccess::EnsureFrameContext(driver, 1u);
    const auto first = DriverTestAccess::EnsureFrameContext(driver, 0u).descriptorAllocator;
    const auto second = DriverTestAccess::EnsureFrameContext(driver, 1u).descriptorAllocator;
    ASSERT_NE(first, second);
    EXPECT_EQ(DriverRendererAccess::GetActiveDescriptorAllocator(driver), nullptr);
    {
        const ScopedFrameResourceContext outer(driver, 0u);
        EXPECT_EQ(Detail::GetActiveFrameContext(*DriverTestAccess::GetImplForTesting(driver)),
            &DriverTestAccess::EnsureFrameContext(driver, 0u));
        EXPECT_EQ(DriverRendererAccess::GetActiveDescriptorAllocator(driver), first);
        EXPECT_THROW((ScopedFrameResourceContext(driver, 2u)), std::out_of_range);
        EXPECT_EQ(DriverRendererAccess::GetActiveFrameContextSlotIndex(driver), 0u);
        try
        {
            const ScopedFrameResourceContext inner(driver, 1u);
            EXPECT_EQ(Detail::GetActiveFrameContext(*DriverTestAccess::GetImplForTesting(driver)),
                &DriverTestAccess::EnsureFrameContext(driver, 1u));
            EXPECT_EQ(DriverRendererAccess::GetActiveDescriptorAllocator(driver), second);
            throw std::runtime_error("capture failed");
        }
        catch (const std::runtime_error&) {}
        EXPECT_EQ(DriverRendererAccess::GetActiveFrameContextSlotIndex(driver), 0u);
        EXPECT_EQ(DriverRendererAccess::GetActiveDescriptorAllocator(driver), first);
    }
    EXPECT_FALSE(DriverRendererAccess::GetActiveFrameContextSlotIndex(driver).has_value());
    EXPECT_EQ(DriverRendererAccess::GetActiveDescriptorAllocator(driver), nullptr);
    EXPECT_EQ(Detail::GetActiveFrameContext(*DriverTestAccess::GetImplForTesting(driver)), nullptr);
}

TEST(RendererFrameObjectBindingTests, FrameResourceContextIsolatesConcurrentBindingAllocations)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    auto& first = DriverTestAccess::EnsureFrameContext(driver, 0u);
    auto& second = DriverTestAccess::EnsureFrameContext(driver, 1u);
    first.frameIndex = 7u;
    second.frameIndex = 9u;
    first.descriptorAllocator->BeginFrame(7u);
    second.descriptorAllocator->BeginFrame(9u);
    std::promise<void> release;
    const auto start = release.get_future().share();
    const auto capture = [&](const size_t slot)
    {
        const ScopedFrameResourceContext resources(driver, slot);
        start.wait();
        EXPECT_EQ(DriverRendererAccess::GetActiveFrameContextSlotIndex(driver), slot);
        NLS::Render::RHI::RHIBindingSetDesc desc;
        desc.entries.resize(slot + 1u);
        return DriverRendererAccess::CreateExplicitBindingSet(
            driver, desc, NLS::Render::RHI::DescriptorAllocationLifetime::TransientFrame);
    };
    auto firstCapture = std::async(std::launch::async, capture, 0u);
    auto secondCapture = std::async(std::launch::async, capture, 1u);
    release.set_value();
    ASSERT_NE(firstCapture.get(), nullptr);
    ASSERT_NE(secondCapture.get(), nullptr);
    EXPECT_EQ(first.descriptorAllocator->GetStats().transientUsed, 1u);
    EXPECT_EQ(second.descriptorAllocator->GetStats().transientUsed, 2u);
    first.descriptorAllocator->BeginFrame(8u);
    EXPECT_EQ(first.descriptorAllocator->GetStats().transientUsed, 0u);
    EXPECT_EQ(second.descriptorAllocator->GetStats().transientUsed, 2u);
    second.descriptorAllocator->BeginFrame(10u);
    EXPECT_EQ(second.descriptorAllocator->GetStats().transientUsed, 0u);
}

TEST(RendererFrameObjectBindingTests, UnboundWorkerDoesNotBorrowGameFrameReservation)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    const ScopedGameThread gameThread;
    DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    const auto reserved = DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(driver);
    ASSERT_TRUE(reserved.has_value());
    EXPECT_NE(DriverRendererAccess::GetActiveDescriptorAllocator(driver), nullptr);
    auto allocator = std::async(std::launch::async, [&]
    {
        return DriverRendererAccess::GetActiveDescriptorAllocator(driver);
    });
    EXPECT_EQ(allocator.get(), nullptr);
    EXPECT_TRUE(DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(driver, *reserved));
    EXPECT_EQ(DriverRendererAccess::GetActiveDescriptorAllocator(driver), nullptr);
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
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}
#endif

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheReusesPipelineMaterialAndMeshOnly)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    const auto snapshot = renderer.CaptureSceneBuildDataForTesting();
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
    // Preserve the provider's binding for each capture instead of substituting
    // the first draw's binding from renderer-wide mutable state.
    EXPECT_NE(snapshot->recordedDrawCommands[0].frameBindingSet, snapshot->recordedDrawCommands[1].frameBindingSet);
    EXPECT_NE(snapshot->recordedDrawCommands[0].frameBindingSet, snapshot->recordedDrawCommands[2].frameBindingSet);
    EXPECT_EQ(snapshot->recordedDrawCommands[2].instanceCount, 2u);
    EXPECT_EQ(snapshot->recordedDrawCommands[2].mesh, snapshot->recordedDrawCommands[0].mesh);
    EXPECT_NE(snapshot->recordedDrawCommands[2].materialBindingSet, snapshot->recordedDrawCommands[0].materialBindingSet);
    EXPECT_EQ(providerPtr->prepareDrawCount, 3u);
    EXPECT_EQ(providerPtr->captureFrameBindingSetCount, 3u);
    EXPECT_EQ(providerPtr->captureBindingSetCount, 3u);

    renderer.EndFrame();
    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.preparedRecordedDrawStaticBaseCacheMissCount, 2u);
    EXPECT_EQ(frameInfo.preparedRecordedDrawStaticBaseCacheHitCount, 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(RendererFrameObjectBindingTests, PreparedDrawCacheMaintenanceWaitsForRecordingAndKeepsDrawResourcesAlive)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, device);
    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(1u);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frame;
    frame.renderWidth = 256u;
    frame.renderHeight = 144u;
    frame.camera = &camera;
    renderer.BeginFrame(frame);

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(), {}, 0u, NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu);
    NLS::Render::Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.instanceCount = 1u;
    drawable.vertexCount = 3u;

    std::promise<void> pipelineEntered;
    auto pipelineEnteredEvent = pipelineEntered.get_future();
    std::promise<void> releasePipeline;
    auto releasePipelineEvent = releasePipeline.get_future().share();
    device->onGraphicsPipelineCreate = [&]()
    {
        pipelineEntered.set_value();
        releasePipelineEvent.wait();
    };

    RecordedDrawCacheProbeSceneRenderer::PreparedDrawForTesting preparedDraw;
    auto prepare = std::async(std::launch::async, [&]()
    {
        return renderer.PrepareDrawResourcesForTesting(drawable, preparedDraw);
    });
    const auto entered = pipelineEnteredEvent.wait_for(std::chrono::seconds(2));
    if (entered != std::future_status::ready)
    {
        releasePipeline.set_value();
        prepare.get();
    }
    ASSERT_EQ(entered, std::future_status::ready);

    std::promise<void> maintenanceStarted;
    auto maintenanceStartedEvent = maintenanceStarted.get_future();
    auto maintenance = std::async(std::launch::async, [&]()
    {
        maintenanceStarted.set_value();
        renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(
            renderer.GetPreparedRecordedDrawStaticBaseCacheMaxFrameAgeForTesting() + 1u);
        renderer.ClearPreparedRecordedDrawStaticBaseCacheForTesting();
    });
    maintenanceStartedEvent.wait();
    const auto maintenanceWhileRecording = maintenance.wait_for(std::chrono::milliseconds(100));
    releasePipeline.set_value();
    const bool prepared = prepare.get();
    maintenance.get();
    device->onGraphicsPipelineCreate = {};

    EXPECT_EQ(maintenanceWhileRecording, std::future_status::timeout);
    ASSERT_TRUE(prepared);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 0u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 0u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(), 0u);
    ASSERT_NE(preparedDraw.pipeline, nullptr);
    EXPECT_EQ(preparedDraw.pipeline->GetDesc().debugName, device->lastGraphicsPipelineDesc.debugName);
    ASSERT_NE(preparedDraw.materialBindingSet, nullptr);
    ASSERT_NE(preparedDraw.mesh, nullptr);

    RecordedDrawCacheProbeSceneRenderer::PreparedDrawForTesting nextDraw;
    ASSERT_TRUE(renderer.PrepareDrawResourcesForTesting(drawable, nextDraw));
    ASSERT_TRUE(renderer.PrepareDrawResourcesForTesting(drawable, nextDraw));
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 1u);
    renderer.ClearPreparedRecordedDrawStaticBaseCacheForTesting();
    EXPECT_NE(nextDraw.pipeline, nullptr);
    renderer.AbortFrameForBackgroundPreview();
    NLS::Render::Core::ABaseRenderer::EndGlobalFrameForBackgroundPreview();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}
#endif

TEST(RendererFrameObjectBindingTests, PreparedDrawUsesCapturedFrameAfterTheGameThreadChangesOutputAttachments)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    auto& frameContext = Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.commandBuffer = std::make_shared<TestCommandBuffer>();
    frameContext.descriptorAllocator = RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(1u);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    RHI::RHITextureDesc color;
    color.format = RHI::TextureFormat::RGBA16F;
    color.sampleCount = 4u;
    RHI::RHITextureDesc depth;
    depth.format = RHI::TextureFormat::Depth32F;
    depth.sampleCount = 4u;
    Entities::Camera camera;
    Data::FrameDescriptor frame;
    frame.renderWidth = 64u;
    frame.renderHeight = 64u;
    frame.camera = &camera;
    frame.outputColorTexture = std::make_shared<TestTexture>(color);
    frame.outputDepthStencilTexture = std::make_shared<TestTexture>(depth);
    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    renderer.BeginFrame(frame);
    const auto frozen = NLS::Render::FrameGraph::FreezeFrameDescriptor(frame);
    auto pipeline = renderer.CreatePipelineState();
    pipeline.depthWriting = false;
    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    Resources::Material material(shader);
    Resources::Mesh mesh(MakeTriangleVertices(), {}, 0u, Resources::MeshBufferUploadMode::CpuToGpu);
    Entities::Drawable drawable;
    drawable.material = &material;
    drawable.mesh = &mesh;
    drawable.instanceCount = 1u;
    drawable.vertexCount = 3u;

    std::promise<void> scopeEntered;
    auto scopeEnteredEvent = scopeEntered.get_future();
    std::promise<void> frameChanged;
    auto frameChangedEvent = frameChanged.get_future().share();
    auto record = std::async(std::launch::async, [&]()
    {
        RecordedDrawCacheProbeSceneRenderer::PreparedFrameScopeForTesting scope(renderer, frozen.descriptor, pipeline);
        scopeEntered.set_value();
        frameChangedEvent.wait();
        EXPECT_EQ(renderer.GetFrameDescriptor().renderWidth, 64u);
        EXPECT_EQ(renderer.GetFrameDescriptor().camera, frozen.camera.get());
        EXPECT_FALSE(renderer.CreatePipelineState().depthWriting);
        EXPECT_TRUE(renderer.IsFrameActive());
        RecordedDrawCacheProbeSceneRenderer::PreparedDrawForTesting psoDraw;
        const bool psoPrepared = renderer.PrepareDrawResourcesForTesting(renderer.CreatePipelineState(), drawable, psoDraw);
        EXPECT_EQ(psoDraw.commandBuffer, nullptr);
        if (psoPrepared)
        {
            const auto& layout = psoDraw.pipeline->GetDesc().renderTargetLayout;
            EXPECT_EQ(layout.colorFormats, std::vector<RHI::TextureFormat> { RHI::TextureFormat::RGBA16F });
            EXPECT_TRUE(layout.hasDepth);
            EXPECT_EQ(layout.depthFormat, RHI::TextureFormat::Depth32F);
            EXPECT_EQ(layout.sampleCount, 4u);
        }
        RecordedDrawCacheProbeSceneRenderer::PreparedDrawForTesting overrideDraw;
        const bool overridesPrepared = renderer.PrepareDrawResourcesForTesting(drawable, overrideDraw);
        if (overridesPrepared)
            EXPECT_EQ(overrideDraw.pipeline->GetDesc().renderTargetLayout.sampleCount, 4u);
        return psoPrepared && overridesPrepared;
    });
    scopeEnteredEvent.wait();
    renderer.AbortFrameForBackgroundPreview();
    Core::ABaseRenderer::EndGlobalFrameForBackgroundPreview();
    frame.renderWidth = 128u;
    frame.renderHeight = 128u;
    color.format = RHI::TextureFormat::RGBA8;
    color.sampleCount = 1u;
    frame.outputColorTexture = std::make_shared<TestTexture>(color);
    frame.outputDepthStencilTexture.reset();
    renderer.BeginFrame(frame);
    EXPECT_EQ(renderer.GetFrameDescriptor().renderWidth, 128u);
    frameChanged.set_value();
    EXPECT_TRUE(record.get());
    renderer.AbortFrameForBackgroundPreview();
    Core::ABaseRenderer::EndGlobalFrameForBackgroundPreview();
    EXPECT_TRUE(Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, PreparedFrameScopesRestoreOuterFrameAndPipelineAfterAnException)
{
    using namespace NLS::Render;
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    Context::DriverTestAccess::SetExplicitDevice(driver, device);
    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    Entities::Camera camera;
    Data::FrameDescriptor outerFrame;
    outerFrame.camera = &camera;
    outerFrame.renderWidth = 64u;
    outerFrame.renderHeight = 64u;
    renderer.BeginFrame(outerFrame);
    const auto livePipeline = renderer.CreatePipelineState();
    auto innerFrame = outerFrame;
    innerFrame.renderWidth = 128u;
    Data::PipelineState outerPipeline;
    outerPipeline.depthWriting = false;
    Data::PipelineState innerPipeline;
    innerPipeline.depthWriting = true;
    {
        RecordedDrawCacheProbeSceneRenderer::PreparedFrameScopeForTesting outer(renderer, outerFrame, outerPipeline);
        try
        {
            RecordedDrawCacheProbeSceneRenderer::PreparedFrameScopeForTesting inner(renderer, innerFrame, innerPipeline);
            EXPECT_EQ(renderer.GetFrameDescriptor().renderWidth, 128u);
            EXPECT_TRUE(renderer.CreatePipelineState().depthWriting);
            throw std::runtime_error("prepared frame failed");
        }
        catch (const std::runtime_error&)
        {
        }
        EXPECT_EQ(renderer.GetFrameDescriptor().renderWidth, 64u);
        EXPECT_FALSE(renderer.CreatePipelineState().depthWriting);
    }
    EXPECT_EQ(renderer.GetFrameDescriptor().renderWidth, 64u);
    EXPECT_EQ(renderer.CreatePipelineState().depthWriting, livePipeline.depthWriting);
    renderer.AbortFrameForBackgroundPreview();
    Core::ABaseRenderer::EndGlobalFrameForBackgroundPreview();
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseTrustedRevisionUsesPreKeyFastPath)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    const ScopedDriverService driverService(driver);
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    frameContext.frameIndex = 42u;
    frameContext.commandBuffer = nullptr;
    frameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(frameContext.descriptorAllocator, nullptr);
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(std::make_unique<PreparedBindingProbeProvider>(renderer));

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    auto* depthShader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    ASSERT_NE(depthShader, nullptr);
    shader->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/TrustedRevision.shader",
        "shader:TrustedRevision/Forward#0",
        "Forward",
        NLS::Render::ShaderLab::ShaderLabPassState{});
    depthShader->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/TrustedRevision.shader",
        "shader:TrustedRevision/DepthOnly#1",
        "DepthOnly",
        NLS::Render::ShaderLab::ShaderLabPassState{});
    NLS::Render::Resources::Material material(shader);
    material.SetShaderLabSourcePath("Assets/Shaders/TrustedRevision.shader");
    material.RegisterShaderLabPassShader(shader);
    material.RegisterShaderLabPassShader(depthShader);
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
    drawable.vertexCount = 3u;
    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    descriptor.hasTrustedStaticDrawRevision = true;
    descriptor.stableSceneIdentity = { 11u, 7u, 3u };
    descriptor.cachedCommandBuildSerial = 19u;
    descriptor.meshInstanceId = mesh.GetInstanceId();
    descriptor.meshContentRevision = mesh.GetContentRevision();
    descriptor.materialInstanceId = material.GetInstanceId();
    descriptor.materialParameterRevision = material.GetParameterRevision();
    descriptor.materialRenderStateRevision = material.GetRenderStateRevision();
    descriptor.materialBindingRevision = material.GetBindingRevision();
    descriptor.shaderInstanceId = shader->GetInstanceId();
    descriptor.shaderGeneration = shader->GetGeneration();
    descriptor.transformRevision = 5u;
    descriptor.allowsSingleObjectDataReuse = true;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(descriptor));

    EXPECT_TRUE(RecordedDrawCacheProbeSceneRenderer::
        IsPreparedRecordedDrawStaticBaseRevisionLookupKeyCompactForTesting());
    EXPECT_LE(
        RecordedDrawCacheProbeSceneRenderer::
            GetPreparedRecordedDrawStaticBaseRevisionLookupKeySizeForTesting(),
        RecordedDrawCacheProbeSceneRenderer::
            GetPreparedRecordedDrawStaticBaseRevisionLookupKeyMaxSizeForTesting());
    EXPECT_GT(
        RecordedDrawCacheProbeSceneRenderer::
            GetPreparedRecordedDrawStaticBaseRevisionIndexMaxEntriesForTesting(),
        RecordedDrawCacheProbeSceneRenderer::
            GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting());

    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    const auto fullKeyBuildCountAfterGeneralMiss =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto lruSpliceCountAfterGeneralMiss =
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
    const auto generalCacheLookupCountAfterGeneralMiss =
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();

    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountAfterGeneralMiss);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting(),
        lruSpliceCountAfterGeneralMiss);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
        generalCacheLookupCountAfterGeneralMiss);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);

    frameContext.frameIndex = 43u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);

    using StaticDescriptor = NLS::Engine::Rendering::EngineDrawableDescriptor;
    struct RevisionCase
    {
        const char* name = nullptr;
        uint64_t StaticDescriptor::* member = nullptr;
    };
    const std::array<RevisionCase, 11u> revisionCases = {{
        { "command build serial", &StaticDescriptor::cachedCommandBuildSerial },
        { "mesh identity", &StaticDescriptor::meshInstanceId },
        { "mesh content revision", &StaticDescriptor::meshContentRevision },
        { "material identity", &StaticDescriptor::materialInstanceId },
        { "material parameter revision", &StaticDescriptor::materialParameterRevision },
        { "material render revision", &StaticDescriptor::materialRenderStateRevision },
        { "material binding revision", &StaticDescriptor::materialBindingRevision },
        { "shader identity", &StaticDescriptor::shaderInstanceId },
        { "shader generation", &StaticDescriptor::shaderGeneration },
        { "transform revision", &StaticDescriptor::transformRevision },
        { "group identity", &StaticDescriptor::groupIdentity }
    }};

    const auto expectMissThenRefresh =
        [&](const char* caseName, NLS::Render::Entities::Drawable changedDrawable)
        {
            SCOPED_TRACE(caseName);
            const auto fullKeyBuildCountBeforeMiss =
                renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
            const auto lruSpliceCountBeforeMiss =
                renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
            ASSERT_TRUE(renderer.CaptureDrawForTesting(
                changedDrawable,
                overrides,
                NLS::Render::Settings::EComparaisonAlgorithm::LESS));
            EXPECT_EQ(
                renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
                fullKeyBuildCountBeforeMiss + 1u);
            EXPECT_EQ(
                renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting(),
                lruSpliceCountBeforeMiss + 1u);

            const auto fullKeyBuildCountAfterRefresh =
                renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
            const auto lruSpliceCountAfterRefresh =
                renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
            ASSERT_TRUE(renderer.CaptureDrawForTesting(
                changedDrawable,
                overrides,
                NLS::Render::Settings::EComparaisonAlgorithm::LESS));
            EXPECT_EQ(
                renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
                fullKeyBuildCountAfterRefresh);
            EXPECT_EQ(
                renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting(),
                lruSpliceCountAfterRefresh);
        };

    for (const auto& revisionCase : revisionCases)
    {
        auto changedDrawable = drawable;
        auto* changedDescriptor = changedDrawable.TryGetDescriptor<StaticDescriptor>();
        ASSERT_NE(changedDescriptor, nullptr);
        ++(changedDescriptor->*(revisionCase.member));
        expectMissThenRefresh(revisionCase.name, std::move(changedDrawable));
    }

    auto changedStableIdentityDrawable = drawable;
    auto* changedStableIdentityDescriptor =
        changedStableIdentityDrawable.TryGetDescriptor<StaticDescriptor>();
    ASSERT_NE(changedStableIdentityDescriptor, nullptr);
    ++changedStableIdentityDescriptor->stableSceneIdentity.primitiveGeneration;
    expectMissThenRefresh("stable scene identity", std::move(changedStableIdentityDrawable));

    auto changedReusePermissionDrawable = drawable;
    auto* changedReusePermissionDescriptor =
        changedReusePermissionDrawable.TryGetDescriptor<StaticDescriptor>();
    ASSERT_NE(changedReusePermissionDescriptor, nullptr);
    changedReusePermissionDescriptor->allowsSingleObjectDataReuse = false;
    expectMissThenRefresh("single object data reuse permission", std::move(changedReusePermissionDrawable));

    auto passBindingSet = std::make_shared<TestBindingSet>(NLS::Render::RHI::RHIBindingSetDesc{});
    renderer.SetActivePreparedPassBindingSetForTesting(passBindingSet);
    const auto fullKeyBuildCountBeforePassBindingChange =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto generalCacheLookupCountBeforePassBindingChange =
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforePassBindingChange);
    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
        generalCacheLookupCountBeforePassBindingChange);
    const auto fullKeyBuildCountAfterPassBindingRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto lruSpliceCountAfterPassBindingRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
    const auto generalCacheLookupCountAfterPassBindingRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountAfterPassBindingRefresh);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting(),
        lruSpliceCountAfterPassBindingRefresh);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
        generalCacheLookupCountAfterPassBindingRefresh);
    renderer.SetActivePreparedPassBindingSetForTesting(nullptr);

    const auto fullKeyBuildCountBeforeOverrideChange =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto lruSpliceCountBeforeOverrideChange =
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
    auto changedOverrides = overrides;
    changedOverrides.blending = true;
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        changedOverrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeOverrideChange);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting(),
        lruSpliceCountBeforeOverrideChange);
    const auto fullKeyBuildCountAfterOverrideRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto lruSpliceCountAfterOverrideRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
    const auto generalCacheLookupCountAfterOverrideRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        changedOverrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountAfterOverrideRefresh);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting(),
        lruSpliceCountAfterOverrideRefresh);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
        generalCacheLookupCountAfterOverrideRefresh);
    renderer.EndFrame();

    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 15u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 15u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 13u);

    renderer.ClearPreparedRecordedDrawStaticBaseCacheForTesting();
    renderer.SetPreparedRecordedDrawStaticBaseRevisionDigestMaskForTesting(0u);
    frameContext.frameIndex = 431u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));

    auto collidingOverrides = overrides;
    collidingOverrides.blending = true;
    const auto fullKeyBuildCountBeforeCollidingOverride =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        collidingOverrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeCollidingOverride);

    const auto fullKeyBuildCountBeforeCollidingOriginal =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeCollidingOriginal);
    renderer.EndFrame();
    renderer.SetPreparedRecordedDrawStaticBaseRevisionDigestMaskForTesting((std::numeric_limits<uint64_t>::max)());
    renderer.ClearPreparedRecordedDrawStaticBaseCacheForTesting();
    frameContext.frameIndex = 432u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    frameContext.frameIndex = 44u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS,
        "DepthOnly"));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS,
        "DepthOnly"));
    const auto fullKeyBuildCountBeforeDepthShaderReload =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    NLS::Render::ShaderLab::ShaderLabPassState reloadedDepthState;
    reloadedDepthState.cullMode = NLS::Render::ShaderLab::ShaderLabCullMode::Off;
    depthShader->SetShaderLabPassStateForTesting(reloadedDepthState);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS,
        "DepthOnly"));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS,
        "DepthOnly"));
    renderer.EndFrame();

    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeDepthShaderReload);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);

    NLS::Render::Resources::Material effectiveMaterial(shader);
    frameContext.frameIndex = 45u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        effectiveMaterial,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        effectiveMaterial,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    const auto fullKeyBuildCountBeforeEffectiveMaterialMutation =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    effectiveMaterial.SetBackfaceCulling(false);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        effectiveMaterial,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        effectiveMaterial,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeEffectiveMaterialMutation);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);

    auto untrustedDrawable = drawable;
    auto* untrustedDescriptor = untrustedDrawable.TryGetDescriptor<StaticDescriptor>();
    ASSERT_NE(untrustedDescriptor, nullptr);
    untrustedDescriptor->hasTrustedStaticDrawRevision = false;
    frameContext.frameIndex = 46u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    const auto fullKeyBuildCountBeforeUntrustedDraws =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto lruSpliceCountBeforeUntrustedDraws =
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        untrustedDrawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        untrustedDrawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeUntrustedDraws + 2u);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting(),
        lruSpliceCountBeforeUntrustedDraws + 2u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 2u);
    EXPECT_GT(renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(), 0u);
    renderer.ClearPreparedRecordedDrawStaticBaseCacheForTesting();
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 0u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 0u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(), 0u);

    frameContext.frameIndex = 47u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    const auto generalCacheLimit =
        RecordedDrawCacheProbeSceneRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxEntriesForTesting();
    const auto revisionIndexLimit =
        RecordedDrawCacheProbeSceneRenderer::GetPreparedRecordedDrawStaticBaseRevisionIndexMaxEntriesForTesting();
    const auto sceneIdentityCount = generalCacheLimit + 32u;
    const auto captureSceneIdentity = [&](const size_t index)
    {
        auto uniqueIdentityDrawable = drawable;
        auto* uniqueIdentityDescriptor =
            uniqueIdentityDrawable.TryGetDescriptor<StaticDescriptor>();
        if (uniqueIdentityDescriptor == nullptr)
            return false;
        uniqueIdentityDescriptor->stableSceneIdentity.primitiveIndex = static_cast<uint32_t>(index);
        return renderer.CaptureDrawForTesting(
            uniqueIdentityDrawable,
            overrides,
            NLS::Render::Settings::EComparaisonAlgorithm::LESS);
    };
    for (size_t index = 0u; index < sceneIdentityCount; ++index)
        ASSERT_TRUE(captureSceneIdentity(index));

    const auto fullKeyBuildCountBeforeSecondSceneScan =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    for (size_t index = 0u; index < sceneIdentityCount; ++index)
        ASSERT_TRUE(captureSceneIdentity(index));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeSecondSceneScan);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(),
        sceneIdentityCount);

    renderer.ClearPreparedRecordedDrawStaticBaseCacheForTesting();
    for (size_t index = 0u; index < revisionIndexLimit; ++index)
        ASSERT_TRUE(captureSceneIdentity(index));
    EXPECT_EQ(renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(1u), 0u);
    for (size_t index = 0u; index < revisionIndexLimit; ++index)
        ASSERT_TRUE(captureSceneIdentity(index));
    ASSERT_TRUE(captureSceneIdentity(revisionIndexLimit));
    const auto fullKeyBuildCountBeforeAllHotRevisionOverflowProbe =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    ASSERT_TRUE(captureSceneIdentity(revisionIndexLimit));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeAllHotRevisionOverflowProbe);

    renderer.ClearPreparedRecordedDrawStaticBaseCacheForTesting();
    for (size_t index = 0u; index < revisionIndexLimit; ++index)
        ASSERT_TRUE(captureSceneIdentity(index));
    constexpr size_t hotRevisionIdentity = 33u;
    constexpr size_t coldRevisionIdentity = 34u;
    EXPECT_EQ(renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(1u), 0u);
    ASSERT_TRUE(captureSceneIdentity(hotRevisionIdentity));
    for (size_t index = revisionIndexLimit; index < revisionIndexLimit + 32u; ++index)
    {
        ASSERT_TRUE(captureSceneIdentity(index));
        if (index == revisionIndexLimit)
        {
            EXPECT_EQ(renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(1u), 0u);
            ASSERT_TRUE(captureSceneIdentity(hotRevisionIdentity));
        }
    }

    const auto fullKeyBuildCountBeforeHotRevisionProbe =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    ASSERT_TRUE(captureSceneIdentity(hotRevisionIdentity));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeHotRevisionProbe);

    const auto fullKeyBuildCountBeforeColdRevisionProbe =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    ASSERT_TRUE(captureSceneIdentity(coldRevisionIdentity));
    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeColdRevisionProbe);
    renderer.EndFrame();

    EXPECT_LE(
        renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(),
        revisionIndexLimit);
    renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(
        RecordedDrawCacheProbeSceneRenderer::GetPreparedRecordedDrawStaticBaseCacheMaxFrameAgeForTesting() + 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(), 0u);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    secondFrameContext.frameIndex = 48u;
    secondFrameContext.commandBuffer = nullptr;
    secondFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(secondFrameContext.descriptorAllocator, nullptr);
    secondFrameContext.descriptorAllocator->BeginFrame(secondFrameContext.frameIndex);
    explicitFrameResources.emplace(driver, 0u);
    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    const auto fullKeyBuildCountAfterDeviceRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto lruSpliceCountAfterDeviceRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting();
    const auto generalCacheLookupCountAfterDeviceRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountAfterDeviceRefresh);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseLruSpliceCountForTesting(),
        lruSpliceCountAfterDeviceRefresh);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
        generalCacheLookupCountAfterDeviceRefresh);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(depthShader));
    explicitFrameResources.reset();
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseRejectsSyntheticFullscreenAndIncompleteTrustedDescriptors)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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

    using StaticDescriptor = NLS::Engine::Rendering::EngineDrawableDescriptor;
    NLS::Render::Entities::Drawable trustedDrawable;
    trustedDrawable.material = &material;
    trustedDrawable.mesh = &mesh;
    trustedDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
    trustedDrawable.instanceCount = 1u;
    trustedDrawable.vertexCount = 3u;
    StaticDescriptor trustedDescriptor;
    trustedDescriptor.hasTrustedStaticDrawRevision = true;
    trustedDescriptor.stableSceneIdentity = { 31u, 2u, 1u };
    trustedDescriptor.cachedCommandBuildSerial = 7u;
    trustedDescriptor.meshInstanceId = mesh.GetInstanceId();
    trustedDescriptor.meshContentRevision = mesh.GetContentRevision();
    trustedDescriptor.materialInstanceId = material.GetInstanceId();
    trustedDescriptor.materialParameterRevision = material.GetParameterRevision();
    trustedDescriptor.materialRenderStateRevision = material.GetRenderStateRevision();
    trustedDescriptor.materialBindingRevision = material.GetBindingRevision();
    trustedDescriptor.shaderInstanceId = shader->GetInstanceId();
    trustedDescriptor.shaderGeneration = shader->GetGeneration();
    trustedDescriptor.transformRevision = 3u;
    trustedDescriptor.allowsSingleObjectDataReuse = true;
    trustedDrawable.AddDescriptor<StaticDescriptor>(std::move(trustedDescriptor));

    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        trustedDrawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    renderer.EndFrame();

    frameContext.frameIndex = 52u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);

    auto syntheticDrawable = trustedDrawable;
    auto* syntheticDescriptor = syntheticDrawable.TryGetDescriptor<StaticDescriptor>();
    ASSERT_NE(syntheticDescriptor, nullptr);
    *syntheticDescriptor = StaticDescriptor{};
    syntheticDescriptor->hasTrustedStaticDrawRevision = true;

    auto zeroRevisionDrawable = trustedDrawable;
    auto* zeroRevisionDescriptor = zeroRevisionDrawable.TryGetDescriptor<StaticDescriptor>();
    ASSERT_NE(zeroRevisionDescriptor, nullptr);
    zeroRevisionDescriptor->meshContentRevision = 0u;

    auto missingRevisionDrawable = trustedDrawable;
    auto* missingRevisionDescriptor = missingRevisionDrawable.TryGetDescriptor<StaticDescriptor>();
    ASSERT_NE(missingRevisionDescriptor, nullptr);
    missingRevisionDescriptor->transformRevision = 0u;

    auto fullscreenDrawable = trustedDrawable;
    fullscreenDrawable.RemoveDescriptor<StaticDescriptor>();

    const auto fullKeyBuildCountBeforeRejectedDraws =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto generalCacheLookupCountBeforeRejectedDraws =
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
    for (const auto& rejectedDrawable : {
             std::cref(syntheticDrawable),
             std::cref(fullscreenDrawable),
             std::cref(zeroRevisionDrawable),
             std::cref(missingRevisionDrawable) })
    {
        ASSERT_TRUE(renderer.CaptureDrawForTesting(
            rejectedDrawable.get(),
            overrides,
            NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    }
    renderer.EndFrame();

    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 0u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 4u);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeRejectedDraws + 4u);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
        generalCacheLookupCountBeforeRejectedDraws + 4u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}
#endif

TEST(RendererFrameObjectBindingTests, RenderSceneOpaqueCommandsEmitResolvedTrustedStaticDrawMetadata)
{
    NLS_FRAME_OBJECT_BINDING_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

    auto* shader = CreateTestGraphicsShader("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material(shader);
    NLS::Render::Resources::Mesh mesh(
        MakeTriangleVertices(),
        {},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        {{ 0.0f, 0.0f, 0.0f }, 1.0f});
    std::vector<std::unique_ptr<NLS::Render::Resources::Mesh>> lodResources;
    lodResources.push_back(std::make_unique<NLS::Render::Resources::Mesh>(
        MakeTriangleVertices(),
        std::vector<uint32_t>{},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
        NLS::Render::Geometry::BoundingSphere{{ 0.0f, 0.0f, 0.0f }, 1.0f}));
    mesh.SetLODResources(std::move(lodResources), { 1.0f, 0.15f });
    auto* resolvedLodMesh = mesh.GetLODMesh(1u);
    ASSERT_NE(resolvedLodMesh, nullptr);
    ASSERT_NE(resolvedLodMesh, &mesh);

    NLS::Engine::SceneSystem::Scene scene;
    const auto addObject = [&](const char* name, const float x)
    {
        auto& object = scene.CreateGameObject(name);
        object.GetTransform()->SetWorldPosition({ x, 0.0f, -10.0f });
        auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        EXPECT_NE(meshFilter, nullptr);
        EXPECT_NE(meshRenderer, nullptr);
        if (meshFilter != nullptr)
            meshFilter->SetMesh(&mesh);
        if (meshRenderer != nullptr)
            meshRenderer->FillWithMaterial(material);
        return &object;
    };

    auto* firstObject = addObject("TrustedStaticDrawFirst", -1.0f);
    ASSERT_NE(firstObject, nullptr);
    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &material;
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(renderScene, scene, syncOptions).rebuiltCachedCommandCount, 1u);

    NLS::Engine::Rendering::LargeSceneSettings largeSceneSettings;
    largeSceneSettings.enableLOD = true;
    largeSceneSettings.enableSpatialIndex = false;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.largeSceneSettings = &largeSceneSettings;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.verticalFovRadians = 1.57079632679f;
    auto visible = renderScene.GatherVisibleCommands(visibilityOptions);
    ASSERT_EQ(visible.opaques.size(), 1u);
    const auto& singleDrawable = visible.opaques.front().second;
    ASSERT_EQ(singleDrawable.mesh, resolvedLodMesh);
    const auto* singleDescriptor =
        singleDrawable.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>();
    ASSERT_NE(singleDescriptor, nullptr);
    ASSERT_TRUE(singleDescriptor->hasTrustedStaticDrawRevision);
    ASSERT_TRUE(singleDescriptor->stableSceneIdentity.IsValid());
    const auto liveHandles = renderScene.GetLivePrimitiveHandles();
    ASSERT_EQ(liveHandles.size(), 1u);
    EXPECT_EQ(singleDescriptor->stableSceneIdentity.sceneId, liveHandles.front().sceneId);
    EXPECT_EQ(singleDescriptor->stableSceneIdentity.primitiveIndex, liveHandles.front().index);
    EXPECT_EQ(singleDescriptor->stableSceneIdentity.primitiveGeneration, liveHandles.front().generation);
    EXPECT_NE(singleDescriptor->cachedCommandBuildSerial, 0u);
    EXPECT_EQ(singleDescriptor->meshInstanceId, resolvedLodMesh->GetInstanceId());
    EXPECT_EQ(singleDescriptor->meshContentRevision, resolvedLodMesh->GetContentRevision());
    ASSERT_NE(singleDrawable.material, nullptr);
    EXPECT_EQ(singleDescriptor->materialInstanceId, singleDrawable.material->GetInstanceId());
    EXPECT_EQ(singleDescriptor->materialParameterRevision, singleDrawable.material->GetParameterRevision());
    EXPECT_EQ(singleDescriptor->materialRenderStateRevision, singleDrawable.material->GetRenderStateRevision());
    EXPECT_EQ(singleDescriptor->materialBindingRevision, singleDrawable.material->GetBindingRevision());
    ASSERT_NE(singleDrawable.material->GetShader(), nullptr);
    EXPECT_EQ(singleDescriptor->shaderInstanceId, singleDrawable.material->GetShader()->GetInstanceId());
    EXPECT_EQ(singleDescriptor->shaderGeneration, singleDrawable.material->GetShader()->GetGeneration());
    EXPECT_EQ(singleDescriptor->transformRevision, firstObject->GetTransform()->GetRenderRevision());
    EXPECT_EQ(
        singleDescriptor->groupIdentity,
        NLS::Engine::Rendering::EngineDrawableDescriptor::kInvalidStaticDrawGroupIdentity);
    EXPECT_TRUE(singleDescriptor->allowsSingleObjectDataReuse);

    ASSERT_NE(addObject("TrustedStaticDrawSecond", 1.0f), nullptr);
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(renderScene, scene, syncOptions).rebuiltCachedCommandCount, 1u);
    visible = renderScene.GatherVisibleCommands(visibilityOptions);
    ASSERT_EQ(visible.opaques.size(), 1u);
    const auto* groupDescriptor = visible.opaques.front().second.TryGetDescriptor<
        NLS::Engine::Rendering::EngineDrawableDescriptor>();
    ASSERT_NE(groupDescriptor, nullptr);
    ASSERT_TRUE(groupDescriptor->hasTrustedStaticDrawRevision);
    EXPECT_NE(
        groupDescriptor->groupIdentity,
        NLS::Engine::Rendering::EngineDrawableDescriptor::kInvalidStaticDrawGroupIdentity);
    EXPECT_FALSE(groupDescriptor->allowsSingleObjectDataReuse);
    const auto groupIdentity = groupDescriptor->groupIdentity;
    const auto groupCommandBuildSerial = groupDescriptor->cachedCommandBuildSerial;
    const auto groupTransformRevision = groupDescriptor->transformRevision;

    const auto repeatedVisible = renderScene.GatherVisibleCommands(visibilityOptions);
    ASSERT_EQ(repeatedVisible.opaques.size(), 1u);
    const auto* repeatedGroupDescriptor = repeatedVisible.opaques.front().second.TryGetDescriptor<
        NLS::Engine::Rendering::EngineDrawableDescriptor>();
    ASSERT_NE(repeatedGroupDescriptor, nullptr);
    EXPECT_EQ(repeatedGroupDescriptor->groupIdentity, groupIdentity);
    EXPECT_EQ(repeatedGroupDescriptor->cachedCommandBuildSerial, groupCommandBuildSerial);
    EXPECT_EQ(repeatedGroupDescriptor->transformRevision, groupTransformRevision);

    NLS::Render::Resources::Material transparentMaterial(shader);
    transparentMaterial.SetBlendable(true);
    syncOptions.overrideMaterial = &transparentMaterial;
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(renderScene, scene, syncOptions).rebuiltCachedCommandCount, 2u);
    const auto transparentVisible = renderScene.GatherVisibleCommands(visibilityOptions);
    ASSERT_EQ(transparentVisible.transparents.size(), 2u);
    for (const auto& entry : transparentVisible.transparents)
    {
        const auto* transparentDescriptor = entry.second.TryGetDescriptor<
            NLS::Engine::Rendering::EngineDrawableDescriptor>();
        ASSERT_NE(transparentDescriptor, nullptr);
        EXPECT_FALSE(transparentDescriptor->hasTrustedStaticDrawRevision);
    }

    NLS::Render::Resources::Material decalMaterial(shader);
    decalMaterial.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Decal);
    syncOptions.overrideMaterial = &decalMaterial;
    ASSERT_EQ(NLS::Tests::SynchronizeSceneForTest(renderScene, scene, syncOptions).rebuiltCachedCommandCount, 2u);
    const auto decalVisible = renderScene.GatherVisibleCommands(visibilityOptions);
    ASSERT_EQ(decalVisible.decals.size(), 2u);
    for (const auto& entry : decalVisible.decals)
    {
        const auto* decalDescriptor = entry.second.TryGetDescriptor<
            NLS::Engine::Rendering::EngineDrawableDescriptor>();
        ASSERT_NE(decalDescriptor, nullptr);
        EXPECT_FALSE(decalDescriptor->hasTrustedStaticDrawRevision);
    }

    RecordedDrawCacheProbeSceneRenderer renderer(driver);
    renderer.SetFrameObjectBindingProvider(std::make_unique<PreparedBindingProbeProvider>(renderer));
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;

    const auto captureUntrustedVisibleDraws =
        [&](const auto& visibleDraws, const uint64_t frameIndex, const char* category)
        {
            SCOPED_TRACE(category);
            frameContext.frameIndex = frameIndex;
            frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
            renderer.BeginFrame(frameDescriptor);
            const auto fullKeyBuildCountBefore =
                renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
            const auto generalCacheLookupCountBefore =
                renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
            for (const auto& entry : visibleDraws)
            {
                ASSERT_TRUE(renderer.CaptureDrawForTesting(
                    entry.second,
                    {},
                    NLS::Render::Settings::EComparaisonAlgorithm::LESS));
            }
            renderer.EndFrame();

            EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 0u);
            EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 0u);
            EXPECT_GT(
                renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
                fullKeyBuildCountBefore);
            EXPECT_GT(
                renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
                generalCacheLookupCountBefore);
            EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
            EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 1u);
        };

    captureUntrustedVisibleDraws(transparentVisible.transparents, 82u, "transparent");
    captureUntrustedVisibleDraws(decalVisible.decals, 83u, "decal");

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCachePersistsAcrossFramesUntilMaterialDirty)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    auto firstSnapshot = renderer.CaptureSceneBuildDataForTesting();
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
    auto secondSnapshot = renderer.CaptureSceneBuildDataForTesting();
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
    explicitFrameResources.reset();
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheInvalidatesWhenExplicitDeviceChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    descriptor.hasTrustedStaticDrawRevision = true;
    descriptor.stableSceneIdentity = { 41u, 0u, 1u };
    descriptor.cachedCommandBuildSerial = 1u;
    descriptor.meshInstanceId = mesh.GetInstanceId();
    descriptor.meshContentRevision = mesh.GetContentRevision();
    descriptor.materialInstanceId = material.GetInstanceId();
    descriptor.materialParameterRevision = material.GetParameterRevision();
    descriptor.materialRenderStateRevision = material.GetRenderStateRevision();
    descriptor.materialBindingRevision = material.GetBindingRevision();
    descriptor.shaderInstanceId = shader->GetInstanceId();
    descriptor.shaderGeneration = shader->GetGeneration();
    descriptor.transformRevision = 1u;
    descriptor.allowsSingleObjectDataReuse = true;
    drawable.AddDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(std::move(descriptor));

    renderer.BeginFrame(frameDescriptor);
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    const auto firstSnapshot = renderer.CaptureSceneBuildDataForTesting();
    ASSERT_TRUE(firstSnapshot.has_value());
    ASSERT_EQ(firstSnapshot->recordedDrawCommands.size(), 1u);
    const auto firstPipeline = firstSnapshot->recordedDrawCommands[0].pipeline;
    const auto firstMaterialBinding = firstSnapshot->recordedDrawCommands[0].materialBindingSet;
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(firstDevice->graphicsPipelineCreateCalls, 1u);
    EXPECT_EQ(firstDevice->bindingSetCreateCalls, 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(), 1u);

    auto secondDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, secondDevice);
    auto& secondFrameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0u);
    secondFrameContext.frameIndex = 62u;
    secondFrameContext.commandBuffer = nullptr;
    secondFrameContext.descriptorAllocator = NLS::Render::RHI::CreateDefaultDescriptorAllocator(64u);
    ASSERT_NE(secondFrameContext.descriptorAllocator, nullptr);
    secondFrameContext.descriptorAllocator->BeginFrame(secondFrameContext.frameIndex);
    explicitFrameResources.emplace(driver, 0u);
    renderer.BeginFrame(frameDescriptor);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 0u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 0u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(), 0u);
    const auto fullKeyBuildCountBeforeDeviceRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto generalCacheLookupCountBeforeDeviceRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeDeviceRefresh + 2u);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
        generalCacheLookupCountBeforeDeviceRefresh + 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(), 1u);
    const auto secondSnapshot = renderer.CaptureSceneBuildDataForTesting();
    ASSERT_TRUE(secondSnapshot.has_value());
    ASSERT_EQ(secondSnapshot->recordedDrawCommands.size(), 1u);
    EXPECT_NE(secondSnapshot->recordedDrawCommands[0].pipeline, firstPipeline);
    EXPECT_NE(secondSnapshot->recordedDrawCommands[0].materialBindingSet, firstMaterialBinding);
    const auto fullKeyBuildCountAfterDeviceRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    const auto generalCacheLookupCountAfterDeviceRefresh =
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting();
    ASSERT_TRUE(renderer.CaptureDrawForTesting(
        drawable,
        overrides,
        NLS::Render::Settings::EComparaisonAlgorithm::LESS));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountAfterDeviceRefresh);
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseGeneralCacheLookupCountForTesting(),
        generalCacheLookupCountAfterDeviceRefresh);
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseFastPathHitCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);
    EXPECT_EQ(secondDevice->graphicsPipelineCreateCalls, 1u);
    EXPECT_EQ(secondDevice->bindingSetCreateCalls, 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheIsBoundedUnderUniqueMaterialChurn)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    auto coldMaterial = std::make_unique<NLS::Render::Resources::Material>(shader);
    std::vector<std::unique_ptr<NLS::Render::Resources::Material>> materials;
    materials.reserve(cacheLimit + 32u);
    std::vector<NLS::Render::Resources::Material*> materialsByIdentity(cacheLimit + 32u, nullptr);

    renderer.BeginFrame(frameDescriptor);
    NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
    overrides.depthTest = true;
    overrides.depthWrite = true;
    overrides.colorWrite = true;
    overrides.hasDepthAttachment = false;

    auto* descriptor = drawable.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>();
    ASSERT_NE(descriptor, nullptr);
    descriptor->hasTrustedStaticDrawRevision = true;
    descriptor->cachedCommandBuildSerial = 1u;
    descriptor->meshInstanceId = mesh.GetInstanceId();
    descriptor->meshContentRevision = mesh.GetContentRevision();
    descriptor->shaderInstanceId = shader->GetInstanceId();
    descriptor->shaderGeneration = shader->GetGeneration();
    descriptor->transformRevision = 1u;
    descriptor->allowsSingleObjectDataReuse = true;

    const auto captureMaterial = [&](NLS::Render::Resources::Material& target, const size_t identity)
    {
        drawable.material = &target;
        descriptor->stableSceneIdentity = { 37u, static_cast<uint32_t>(identity), 1u };
        descriptor->materialInstanceId = target.GetInstanceId();
        descriptor->materialParameterRevision = target.GetParameterRevision();
        descriptor->materialRenderStateRevision = target.GetRenderStateRevision();
        descriptor->materialBindingRevision = target.GetBindingRevision();
        return renderer.CaptureDrawForTesting(
            drawable,
            overrides,
            NLS::Render::Settings::EComparaisonAlgorithm::LESS);
    };

    constexpr size_t hotMaterialIdentity = 33u;
    constexpr size_t coldMaterialIdentity = 34u;
    for (size_t index = 0u; index < cacheLimit; ++index)
    {
        if (index == hotMaterialIdentity)
        {
            materialsByIdentity[index] = hotMaterial.get();
            ASSERT_TRUE(captureMaterial(*hotMaterial, index));
        }
        else if (index == coldMaterialIdentity)
        {
            materialsByIdentity[index] = coldMaterial.get();
            ASSERT_TRUE(captureMaterial(*coldMaterial, index));
        }
        else
        {
            materials.push_back(std::make_unique<NLS::Render::Resources::Material>(shader));
            materialsByIdentity[index] = materials.back().get();
            ASSERT_TRUE(captureMaterial(*materials.back(), index));
        }
    }

    EXPECT_EQ(renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(1u), 0u);
    ASSERT_TRUE(captureMaterial(*hotMaterial, hotMaterialIdentity));

    for (size_t index = 0u; index < 32u; ++index)
    {
        materials.push_back(std::make_unique<NLS::Render::Resources::Material>(shader));
        materialsByIdentity[cacheLimit + index] = materials.back().get();
        ASSERT_TRUE(captureMaterial(*materials.back(), cacheLimit + index));
        if (index == 0u)
        {
            EXPECT_EQ(renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(1u), 0u);
            ASSERT_TRUE(captureMaterial(*hotMaterial, hotMaterialIdentity));
        }
    }

    const auto fullKeyBuildCountBeforeHotProbe =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    ASSERT_TRUE(captureMaterial(*hotMaterial, hotMaterialIdentity));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeHotProbe);

    const auto fullKeyBuildCountBeforeColdProbe =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    ASSERT_TRUE(captureMaterial(*coldMaterial, coldMaterialIdentity));
    EXPECT_GT(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeColdProbe);
    renderer.EndFrame();

    EXPECT_LE(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), cacheLimit);
    EXPECT_LE(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), cacheLimit);
    EXPECT_LE(
        renderer.GetPreparedRecordedDrawStaticBaseRevisionIndexSizeForTesting(),
        RecordedDrawCacheProbeSceneRenderer::
            GetPreparedRecordedDrawStaticBaseRevisionIndexMaxEntriesForTesting());

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

    frameContext.frameIndex = 72u;
    frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
    renderer.BeginFrame(frameDescriptor);
    for (size_t index = 0u; index < cacheLimit; ++index)
    {
        ASSERT_NE(materialsByIdentity[index], nullptr);
        ASSERT_TRUE(captureMaterial(*materialsByIdentity[index], index));
    }
    EXPECT_EQ(renderer.AdvancePreparedRecordedDrawStaticBaseCacheForTesting(1u), 0u);
    for (size_t index = 0u; index < cacheLimit; ++index)
        ASSERT_TRUE(captureMaterial(*materialsByIdentity[index], index));
    ASSERT_NE(materialsByIdentity[cacheLimit], nullptr);
    ASSERT_TRUE(captureMaterial(*materialsByIdentity[cacheLimit], cacheLimit));
    const auto fullKeyBuildCountBeforeAllHotGeneralOverflowProbe =
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting();
    ASSERT_TRUE(captureMaterial(*materialsByIdentity[cacheLimit], cacheLimit));
    EXPECT_EQ(
        renderer.GetPreparedRecordedDrawStaticBaseFullKeyBuildCountForTesting(),
        fullKeyBuildCountBeforeAllHotGeneralOverflowProbe);
    renderer.EndFrame();

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheHitDoesNotGrowStableIndex)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheReplacesEntryWhenMeshContentChanges)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    auto firstSnapshot = renderer.CaptureSceneBuildDataForTesting();
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
    auto secondSnapshot = renderer.CaptureSceneBuildDataForTesting();
    ASSERT_TRUE(secondSnapshot.has_value());
    ASSERT_EQ(secondSnapshot->recordedDrawCommands.size(), 1u);
    EXPECT_NE(secondSnapshot->recordedDrawCommands[0].mesh, firstMesh);
    renderer.EndFrame();

    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheMissCount, 1u);
    EXPECT_EQ(renderer.GetFrameInfo().preparedRecordedDrawStaticBaseCacheHitCount, 0u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseCacheSizeForTesting(), 1u);
    EXPECT_EQ(renderer.GetPreparedRecordedDrawStaticBaseStableIndexSizeForTesting(), 1u);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    explicitFrameResources.reset();
}

TEST(RendererFrameObjectBindingTests, PreparedRecordedDrawStaticBaseCacheSeparatesPipelineDimensions)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.reset();
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
    std::optional<NLS::Render::Context::ScopedFrameResourceContext> explicitFrameResources(std::in_place, driver, 0u);

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
    explicitFrameResources.emplace(driver, 0u);
    ASSERT_NE(renderer.CreateExplicitUniformBufferBindingSet(uniformBuffer, desc), nullptr);

    EXPECT_EQ(secondDevice->bindingLayoutCreateCalls, 1u);
    explicitFrameResources.reset();
}

#endif

TEST(RendererFrameObjectBindingTests, DeferredThreadedOffscreenPackageCarriesExternalOutputAttachmentViewForLightingPass)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 256u;
    frameDescriptor.renderHeight = 144u;
    frameDescriptor.camera = &camera;
    frameDescriptor.outputBuffer = &outputBuffer;

    using NLS::Render::Settings::ERasterizationMode;
    for (const auto mode : { ERasterizationMode::FILL, ERasterizationMode::LINE, ERasterizationMode::FILL })
    {
        SCOPED_TRACE(static_cast<int>(mode));
        NLS::Render::Context::DriverUIAccess::SetPolygonMode(driver, mode);
        renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
            scene, std::nullopt, nullptr
        });
        auto observedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);
        ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
        EXPECT_FALSE(renderer.HasPendingLightGridFrameInputs());
        ASSERT_NO_THROW(renderer.EndFrame());

        EXPECT_EQ(observedScene.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
        ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));
        ASSERT_EQ(observedScene.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
        const auto package = observedScene.get();
        ASSERT_EQ(package.passCommandInputs.size(), 2u);
        const auto& geometryPass = package.passCommandInputs[0];
        ASSERT_EQ(geometryPass.recordedDrawCommands.size(), 1u);
        ASSERT_NE(geometryPass.recordedDrawCommands.front().pipeline, nullptr);
        EXPECT_EQ(geometryPass.recordedDrawCommands.front().pipeline->GetDesc().rasterState.wireframe,
            mode == ERasterizationMode::LINE);

        const auto& lightingPass = package.passCommandInputs[1];
        EXPECT_EQ(lightingPass.kind, NLS::Render::Context::RenderPassCommandKind::Lighting);
        EXPECT_FALSE(lightingPass.targetsSwapchain);
        EXPECT_TRUE(lightingPass.usesColorAttachment);
        ASSERT_FALSE(lightingPass.colorAttachmentViews.empty());
        EXPECT_NE(lightingPass.colorAttachmentViews.front(), nullptr);
        ASSERT_EQ(lightingPass.recordedDrawCommands.size(), 1u);
        ASSERT_NE(lightingPass.recordedDrawCommands.front().pipeline, nullptr);
        EXPECT_FALSE(lightingPass.recordedDrawCommands.front().pipeline->GetDesc().rasterState.wireframe);
        ASSERT_FALSE(package.extractedTextures.empty());
        EXPECT_NE(package.extractedTextures.front(), nullptr);
    }
}

TEST(RendererFrameObjectBindingTests, DeferredRenderCommandHandlesMissingDescriptor)
{
    using namespace NLS::Render::Context;
    class DescriptorProbeRenderer final : public NLS::Engine::Rendering::DeferredSceneRenderer
    {
    public:
        using DeferredSceneRenderer::DeferredSceneRenderer;
        using DeferredSceneRenderer::DeferredSceneDescriptor;
    };

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.deferPipelineResourcesUntilFirstFrame = true;
    DescriptorProbeRenderer renderer(driver, options);
    NLS::Engine::SceneSystem::Scene scene;
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frame;
    frame.renderWidth = 64u;
    frame.renderHeight = 64u;
    frame.camera = &camera;
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(
        {scene, std::nullopt, nullptr});

    auto observedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frame));
    ASSERT_TRUE(renderer.IsFrameActive());
    ASSERT_TRUE(renderer.HasDescriptor<DescriptorProbeRenderer::DeferredSceneDescriptor>());
    renderer.RemoveDescriptor<DescriptorProbeRenderer::DeferredSceneDescriptor>();
    ASSERT_NO_THROW(renderer.DrawFrame());
    ASSERT_NO_THROW(renderer.EndFrame());
    ASSERT_TRUE(renderer.WasLastThreadedFramePublished());
    ASSERT_TRUE(DriverRendererAccess::TryDrainThreadedRendering(driver, true));
    ASSERT_EQ(observedScene.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
    const auto package = observedScene.get();
    EXPECT_TRUE(package.recordedDrawCommands.empty());
    EXPECT_TRUE(package.passCommandInputs.empty());
}

TEST(RendererFrameObjectBindingTests, DeferredEmptySceneRenderCommandDoesNotCreateLightingWork)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    DriverTestAccess::SetExplicitDevice(driver, std::make_shared<TestExplicitDevice>());
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.deferPipelineResourcesUntilFirstFrame = true;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);
    NLS::Engine::SceneSystem::Scene scene;
    scene.CreateGameObject("LightOnly").AddComponent<NLS::Engine::Components::LightComponent>();
    NLS::Render::Buffers::Framebuffer output(64u, 64u);
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frame;
    frame.renderWidth = 64u;
    frame.renderHeight = 64u;
    frame.camera = &camera;
    frame.outputBuffer = &output;
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(
        {scene, std::nullopt, nullptr});

    auto observedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    auto observedRhiFrame = NLS::Tests::ObserveNextRhiFrameForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frame));
    ASSERT_TRUE(renderer.IsFrameActive());
    ASSERT_NO_THROW(renderer.DrawFrame());
    ASSERT_NO_THROW(renderer.EndFrame());
    ASSERT_TRUE(renderer.WasLastThreadedFramePublished());
    ASSERT_TRUE(DriverRendererAccess::TryDrainThreadedRendering(driver, true));
    ASSERT_EQ(observedScene.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
    const auto package = observedScene.get();
    EXPECT_TRUE(package.recordedDrawCommands.empty());
    EXPECT_TRUE(package.passCommandInputs.empty());
    EXPECT_FALSE(package.targetsSwapchain);
    ASSERT_EQ(observedRhiFrame.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
    const auto submission = observedRhiFrame.get();
    EXPECT_TRUE(submission.submittedSuccessfully);
    EXPECT_EQ(submission.commandRecordingFailureCount, 0u);
    ASSERT_FALSE(package.extractedTextures.empty());
    EXPECT_EQ(package.extractedTextures.front(), output.GetExplicitTextureHandle());
    EXPECT_FALSE(renderer.HasPendingLightGridFrameInputs());
    EXPECT_FALSE(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::
        HasDeferredThreadedPipelineResourcesForTesting(renderer));
    EXPECT_EQ(NLS::Engine::Rendering::DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(renderer), nullptr);
}

TEST(RendererFrameObjectBindingTests, DeferredHelperDepthFirstUseAndResizeExecuteOnRenderThread)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Driver driver(settings);
    const ScopedDriverService driverService(driver);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto device = std::make_shared<TestExplicitDevice>();
    DriverTestAccess::SetExplicitDevice(driver, device);
    NLS::Engine::Rendering::DeferredSceneRenderer::ConstructionOptions options;
    options.loadPipelineResources = false;
    NLS::Engine::Rendering::DeferredSceneRenderer renderer(driver, options);
    NLS::Engine::SceneSystem::Scene scene;
    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frame;
    frame.renderWidth = 128u;
    frame.renderHeight = 96u;
    frame.camera = &camera;
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(
        {scene, std::nullopt, nullptr});
    renderer.BeginFrame(frame);
    device->textureCreationThreads.clear();
    const auto firstDepth = renderer.GetDeferredPreparedSceneDepthViewForEditorHelpers();
    ASSERT_NE(firstDepth, nullptr);
    ASSERT_NE(firstDepth->GetTexture(), nullptr);
    EXPECT_EQ(firstDepth->GetTexture()->GetDesc().extent.width, 128u);
    const auto creationCount = device->textureCreationThreads.size();
    EXPECT_EQ(renderer.GetDeferredPreparedSceneDepthViewForEditorHelpers(), firstDepth);
    EXPECT_EQ(device->textureCreationThreads.size(), creationCount);
    ASSERT_GT(creationCount, 0u);
    for (const auto onRenderThread : device->textureCreationThreads)
        EXPECT_TRUE(onRenderThread);
    renderer.EndFrame();
    frame.renderWidth = 256u;
    frame.renderHeight = 144u;
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(
        {scene, std::nullopt, nullptr});
    renderer.BeginFrame(frame);
    device->textureCreationThreads.clear();
    const auto resizedDepth = renderer.GetDeferredPreparedSceneDepthViewForEditorHelpers();
    ASSERT_NE(resizedDepth, nullptr);
    EXPECT_NE(resizedDepth->GetTexture(), firstDepth->GetTexture());
    EXPECT_EQ(resizedDepth->GetTexture()->GetDesc().extent.width, 256u);
    EXPECT_EQ(resizedDepth->GetTexture()->GetDesc().extent.height, 144u);
    EXPECT_EQ(firstDepth->GetTexture()->GetDesc().extent.width, 128u);
    ASSERT_GT(device->textureCreationThreads.size(), 0u);
    for (const auto onRenderThread : device->textureCreationThreads)
        EXPECT_TRUE(onRenderThread);
    renderer.EndFrame();
}

TEST(RendererFrameObjectBindingTests, RendererCachesCameraMatricesBeforeFrameObjectBindingBeginFrame)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

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
    settings.framesInFlight = 1u;

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

    auto observedScene = NLS::Tests::ObserveNextRenderSceneForTest(driver);
    ASSERT_NO_THROW(renderer.BeginFrame(frameDescriptor));
    EXPECT_FALSE(renderer.HasPendingLightGridFrameInputs());
    ASSERT_NO_THROW(renderer.EndFrame());

    EXPECT_EQ(observedScene.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
    ASSERT_TRUE(NLS::Render::Context::DriverTestAccess::TryDrainThreadedRendering(driver));
    ASSERT_EQ(observedScene.wait_for(std::chrono::seconds::zero()), std::future_status::ready);
    const auto package = observedScene.get();
    ASSERT_FALSE(package.passCommandInputs.empty());
    ASSERT_FALSE(package.extractedTextures.empty());
    EXPECT_EQ(
        package.extractedTextures.front(),
        outputBuffer.GetExplicitTextureHandle());
}
