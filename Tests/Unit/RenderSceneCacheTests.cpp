#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Jobs/JobSystem.h"
#include "Math/Matrix4.h"
#include "Object/Object.h"
#include "LargeSceneOptimizationTestHelpers.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Data/Frustum.h"
#include "Rendering/Data/DrawableInstanceCount.h"
#include "Rendering/Data/ObjectDataLimits.h"
#include "Rendering/LargeSceneSettings.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RenderScene.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/IndexedObjectDataShaderSupport.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/SceneHLOD.h"
#include "Rendering/SceneLOD.h"
#include "Rendering/SceneOcclusion.h"
#include "Rendering/SceneVisibilityPipeline.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Serialize/ObjectReferenceResolver.h"
#include "Serialize/PPtr.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"

namespace
{
    class ScopedRenderSceneCacheJobSystem
    {
    public:
        explicit ScopedRenderSceneCacheJobSystem(const uint32_t workerCount)
        {
            if (NLS::Base::Jobs::IsJobSystemInitialized())
                return;

            NLS::Base::Jobs::JobSystemConfig config;
            config.workerCount = workerCount;
            m_ownsRuntime = NLS::Base::Jobs::TryInitializeJobSystem(config);
        }

        ~ScopedRenderSceneCacheJobSystem()
        {
            if (!m_ownsRuntime)
                return;

            NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
            NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
        }

        bool IsInitialized() const
        {
            return NLS::Base::Jobs::IsJobSystemInitialized();
        }

    private:
        bool m_ownsRuntime = false;
    };

    NLS::Render::Context::Driver& EnsureRenderSceneTestDriver()
    {
        static auto driver = std::make_unique<NLS::Render::Context::Driver>([]()
        {
            NLS::Render::Settings::DriverSettings settings;
            settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
            settings.enableExplicitRHI = false;
            return settings;
        }());
        NLS::Core::ServiceLocator::Provide(*driver);
        return *driver;
    }

    class HZBPacketTestAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "HZBPacketTestAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override
        {
            return NLS::Render::RHI::NativeBackendType::DX12;
        }
        std::string_view GetVendor() const override { return "NullusTests"; }
        std::string_view GetHardware() const override { return "HZBPacketTestDevice"; }
    };

    class HZBPacketTestDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        HZBPacketTestDevice()
            : m_adapter(std::make_shared<HZBPacketTestAdapter>())
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Compute, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ExplicitBarriers, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::HierarchicalZBuffer, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::ConservativeOcclusion, true);
            m_capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::AsyncReadback, true);

            NLS::Render::RHI::TextureFormatCapability depthCapability;
            depthCapability.sampled = true;
            m_capabilities.SetTextureFormatCapability(
                NLS::Render::RHI::TextureFormat::Depth24Stencil8,
                depthCapability);
            NLS::Render::RHI::TextureFormatCapability hzbCapability;
            hzbCapability.sampled = true;
            hzbCapability.storage = true;
            m_capabilities.SetTextureFormatCapability(
                NLS::Render::RHI::TextureFormat::R32F,
                hzbCapability);
        }

        std::string_view GetDebugName() const override { return "HZBPacketTestDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
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
            void*) override {}

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    class ScopedHZBPacketTestDevice final
    {
    public:
        explicit ScopedHZBPacketTestDevice(NLS::Render::Context::Driver& driver)
            : m_driver(driver)
            , m_previousDevice(NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(driver))
        {
            NLS::Render::Context::DriverTestAccess::SetExplicitDevice(
                m_driver,
                std::make_shared<HZBPacketTestDevice>());
        }

        ~ScopedHZBPacketTestDevice()
        {
            NLS::Render::Context::DriverTestAccess::SetExplicitDevice(m_driver, m_previousDevice);
        }

        ScopedHZBPacketTestDevice(const ScopedHZBPacketTestDevice&) = delete;
        ScopedHZBPacketTestDevice& operator=(const ScopedHZBPacketTestDevice&) = delete;

    private:
        NLS::Render::Context::Driver& m_driver;
        std::shared_ptr<NLS::Render::RHI::RHIDevice> m_previousDevice;
    };

    NLS::Render::Geometry::Vertex VertexAt(const float x, const float y, const float z)
    {
        NLS::Render::Geometry::Vertex vertex{};
        vertex.position[0] = x;
        vertex.position[1] = y;
        vertex.position[2] = z;
        vertex.normals[2] = 1.0f;
        return vertex;
    }

    NLS::Render::Resources::Mesh* CreateTriangleMesh(
        const uint32_t materialIndex,
        const NLS::Render::Geometry::BoundingSphere& boundingSphere)
    {
        return new NLS::Render::Resources::Mesh(
            std::vector<NLS::Render::Geometry::Vertex>{
                VertexAt(-0.5f, -0.5f, 0.0f),
                VertexAt(0.5f, -0.5f, 0.0f),
                VertexAt(0.0f, 0.5f, 0.0f)
            },
            std::vector<uint32_t>{0u, 1u, 2u},
            materialIndex,
            NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
            boundingSphere);
    }

    NLS::Render::Resources::Mesh* CreateSingleMesh(const uint32_t materialIndex = 0u)
    {
        return CreateTriangleMesh(materialIndex, {{0.0f, 0.0f, 0.0f}, 1.0f});
    }

    NLS::Render::Resources::Mesh* CreateFarMesh()
    {
        return CreateTriangleMesh(0u, {{250.0f, 0.0f, 0.0f}, 1.0f});
    }

    struct RenderableFixture
    {
        NLS::Engine::SceneSystem::Scene scene;
        NLS::Render::Resources::Shader* shader = nullptr;
        NLS::Render::Resources::Material material;
        NLS::Render::Resources::Mesh* mesh = nullptr;
        NLS::Engine::Components::MeshFilter* meshFilter = nullptr;
        NLS::Engine::Components::MeshRenderer* meshRenderer = nullptr;

        RenderableFixture()
        {
            EnsureRenderSceneTestDriver();
            shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
            material.SetShader(shader);
            mesh = CreateSingleMesh();
            auto& actor = scene.CreateGameObject("RetainedSceneActor");
            meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
            meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
            meshFilter->SetMesh(mesh);
            meshRenderer->FillWithMaterial(material);
        }

        ~RenderableFixture()
        {
            delete mesh;
            if (shader != nullptr)
                EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
        }
    };

    NLS::Render::Data::Frustum CreateForwardFrustum()
    {
        NLS::Render::Data::Frustum frustum;
        const auto view = NLS::Maths::Matrix4::CreateView(
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, -1.0f,
            0.0f, 1.0f, 0.0f);
        const auto projection = NLS::Maths::Matrix4::CreatePerspective(90.0f, 1.0f, 0.1f, 100.0f);
        frustum.CalculateFrustum(projection * view);
        return frustum;
    }

    NLS::Render::Data::Frustum CreateWideForwardFrustum()
    {
        NLS::Render::Data::Frustum frustum;
        const auto view = NLS::Maths::Matrix4::CreateView(
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, -1.0f,
            0.0f, 1.0f, 0.0f);
        const auto projection = NLS::Maths::Matrix4::CreatePerspective(150.0f, 1.0f, 0.1f, 100.0f);
        frustum.CalculateFrustum(projection * view);
        return frustum;
    }

    struct ManyPrimitiveFixture
    {
        NLS::Engine::SceneSystem::Scene scene;
        NLS::Render::Resources::Shader* shader = nullptr;
        NLS::Render::Resources::Material material;
        std::vector<NLS::Render::Resources::Mesh*> meshes;

        explicit ManyPrimitiveFixture(const size_t primitiveCount)
        {
            EnsureRenderSceneTestDriver();
            shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
            material.SetShader(shader);
            meshes.reserve(primitiveCount);

            for (size_t index = 0u; index < primitiveCount; ++index)
            {
                auto* mesh = CreateSingleMesh();
                meshes.push_back(mesh);

                auto& object = scene.CreateGameObject("VisibilityPrimitive" + std::to_string(index));
                auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
                auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
                meshFilter->SetMesh(mesh);
                meshRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
                meshRenderer->FillWithMaterial(material);

                const bool visible = (index % 3u) != 0u;
                object.GetTransform()->SetWorldPosition({
                    visible ? static_cast<float>(index % 7u) * 0.2f - 0.6f : 250.0f,
                    0.0f,
                    visible ? -6.0f - static_cast<float>(index % 5u) : -6.0f
                });
            }
        }

        ~ManyPrimitiveFixture()
        {
            for (auto* mesh : meshes)
                delete mesh;

            if (shader != nullptr)
                EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
        }
    };

    std::vector<const NLS::Render::Resources::Mesh*> ExtractMeshes(
        const NLS::Engine::Rendering::RenderSceneVisibleQueues& queues)
    {
        std::vector<const NLS::Render::Resources::Mesh*> result;
        result.reserve(queues.opaques.size() + queues.transparents.size());

        for (const auto& entry : queues.opaques)
            result.push_back(entry.second.mesh);
        for (const auto& entry : queues.transparents)
            result.push_back(entry.second.mesh);

        std::sort(result.begin(), result.end());
        return result;
    }

    NLS::Engine::Rendering::ScenePrimitiveHandle FindPrimitiveHandleByMesh(
        const NLS::Engine::Rendering::ScenePrimitiveSnapshot& snapshot,
        const NLS::Render::Resources::Mesh& mesh)
    {
        const auto found = std::find_if(
            snapshot.primitiveRecords.begin(),
            snapshot.primitiveRecords.end(),
            [&mesh](const auto& record)
            {
                return record.mesh == &mesh;
            });
        return found != snapshot.primitiveRecords.end()
            ? found->handle
            : NLS::Engine::Rendering::ScenePrimitiveHandle {};
    }

    struct QueueSortFixture
    {
        NLS::Engine::SceneSystem::Scene scene;
        NLS::Render::Resources::Shader* shader = nullptr;
        NLS::Render::Resources::Material opaqueMaterialA;
        NLS::Render::Resources::Material opaqueMaterialB;
        NLS::Render::Resources::Material transparentMaterial;
        NLS::Render::Resources::Mesh* sharedMesh = nullptr;
        NLS::Render::Resources::Mesh* otherMesh = nullptr;

        QueueSortFixture()
        {
            EnsureRenderSceneTestDriver();
            shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
            opaqueMaterialA.SetShader(shader);
            opaqueMaterialB.SetShader(shader);
            transparentMaterial.SetShader(shader);
            transparentMaterial.SetBlendable(true);
            sharedMesh = CreateSingleMesh();
            otherMesh = CreateSingleMesh();
        }

        ~QueueSortFixture()
        {
            delete sharedMesh;
            delete otherMesh;
            if (shader != nullptr)
                EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
        }

        void AddObject(
            const char* name,
            NLS::Render::Resources::Mesh& mesh,
            NLS::Render::Resources::Material& material,
            const float distance)
        {
            auto& object = scene.CreateGameObject(name);
            auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
            auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
            meshFilter->SetMesh(&mesh);
            meshRenderer->FillWithMaterial(material);
            object.GetTransform()->SetWorldPosition({ distance, 0.0f, 0.0f });
        }

        void AddObjectAt(
            const char* name,
            NLS::Render::Resources::Mesh& mesh,
            NLS::Render::Resources::Material& material,
            const NLS::Maths::Vector3& position)
        {
            auto& object = scene.CreateGameObject(name);
            auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
            auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
            meshFilter->SetMesh(&mesh);
            meshRenderer->FillWithMaterial(material);
            meshRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
            object.GetTransform()->SetWorldPosition(position);
        }
    };

    uint32_t ResolveVisibleInstanceCount(const NLS::Render::Entities::Drawable& drawable)
    {
        return NLS::Render::Data::ResolveDrawableInstanceCount(drawable).count;
    }

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

    class ScopedObjectDataCountPerDrawLimitOverride final
    {
    public:
        explicit ScopedObjectDataCountPerDrawLimitOverride(const uint32_t limit)
            : m_previousLimit(NLS::Render::Data::GetObjectDataCountPerDrawLimitForTesting())
        {
            NLS::Render::Data::SetObjectDataCountPerDrawLimitForTesting(limit);
        }

        ~ScopedObjectDataCountPerDrawLimitOverride()
        {
            NLS::Render::Data::SetObjectDataCountPerDrawLimitForTesting(m_previousLimit);
        }

        ScopedObjectDataCountPerDrawLimitOverride(const ScopedObjectDataCountPerDrawLimitOverride&) = delete;
        ScopedObjectDataCountPerDrawLimitOverride& operator=(const ScopedObjectDataCountPerDrawLimitOverride&) = delete;

    private:
        uint32_t m_previousLimit = 0u;
    };
#endif

    template <typename T>
    NLS::Engine::Serialize::PPtr<T> MakeRenderScenePPtr(
        const NLS::Engine::Serialize::ObjectIdentifier& identifier)
    {
        return NLS::Engine::Serialize::PPtr<T>(
            NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
    }

    class ScopedTempDirectory final
    {
    public:
        explicit ScopedTempDirectory(std::filesystem::path path)
            : m_path(std::move(path))
        {
        }

        ~ScopedTempDirectory()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }

        const std::filesystem::path& Path() const
        {
            return m_path;
        }

    private:
        std::filesystem::path m_path;
    };

    class ScopedRenderSceneResourceManagers final
    {
    public:
        ScopedRenderSceneResourceManagers(
            NLS::Core::ResourceManagement::MeshManager& meshManager,
            NLS::Core::ResourceManagement::MaterialManager& materialManager,
            NLS::Core::ResourceManagement::ShaderManager& shaderManager,
            const std::string& projectAssetsRoot,
            const std::string& engineAssetsRoot)
            : m_meshManager(meshManager)
            , m_materialManager(materialManager)
            , m_shaderManager(shaderManager)
        {
            NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
            NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
            NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
            NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(m_meshManager);
            NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(m_materialManager);
            NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(m_shaderManager);
        }

        ~ScopedRenderSceneResourceManagers()
        {
            m_materialManager.UnloadResources();
            m_meshManager.UnloadResources();
            m_shaderManager.UnloadResources();
            NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
            NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
            NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
            NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
            NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
            NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
        }

        ScopedRenderSceneResourceManagers(const ScopedRenderSceneResourceManagers&) = delete;
        ScopedRenderSceneResourceManagers& operator=(const ScopedRenderSceneResourceManagers&) = delete;

    private:
        NLS::Core::ResourceManagement::MeshManager& m_meshManager;
        NLS::Core::ResourceManagement::MaterialManager& m_materialManager;
        NLS::Core::ResourceManagement::ShaderManager& m_shaderManager;
    };

    class SceneDrawableProbeRenderer final : public NLS::Engine::Rendering::BaseSceneRenderer
    {
    public:
        explicit SceneDrawableProbeRenderer(NLS::Render::Context::Driver& driver)
            : BaseSceneRenderer(driver)
        {
        }

        AllDrawables CaptureSceneDrawables(const NLS::Render::Data::FrameDescriptor& frameDescriptor)
        {
            m_frameDescriptor = frameDescriptor;
            return ParseScene();
        }

        void SetSceneDescriptorForTesting(
            NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor descriptor)
        {
            if (HasDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>())
                RemoveDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>();
            AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(std::move(descriptor));
        }

        const NLS::Engine::Rendering::SceneOcclusionPrimitivePacketBuildResult& GetHZBPacketBuildResult() const
        {
            return GetLastHZBOcclusionPrimitivePacketBuildResult();
        }

        const NLS::Engine::Rendering::SceneOcclusionFrameInput& GetLastHZBFrameInputForTesting() const
        {
            return GetLastHZBOcclusionFrameInput();
        }

        void SeedHZBHistoryForTesting(
            const NLS::Engine::Rendering::SceneOcclusionFrameInput& frame,
            std::span<const NLS::Engine::Rendering::SceneOcclusionPrimitiveInput> primitiveInputs)
        {
            BeginHZBOcclusionObservationFrame(frame, primitiveInputs);
            std::vector<uint32_t> visibleFlags(primitiveInputs.size(), 0u);
            (void)CompleteHZBOcclusionObservationFrame(visibleFlags);
        }

        void SeedHZBHistoryForTesting(
            const NLS::Engine::Rendering::SceneOcclusionFrameInput& frame,
            std::span<const NLS::Engine::Rendering::SceneOcclusionPrimitiveInput> primitiveInputs,
            std::span<const uint32_t> primitiveResultFlags)
        {
            BeginHZBOcclusionObservationFrame(frame, primitiveInputs);
            (void)CompleteHZBOcclusionObservationFrame(primitiveResultFlags);
        }
    };

    void WriteCubeMeshArtifact(const std::filesystem::path& artifactPath)
    {
        std::filesystem::create_directories(artifactPath.parent_path());

        NLS::Render::Assets::MeshArtifactData artifact;
        artifact.vertices = {
            VertexAt(-0.5f, -0.5f, 0.0f),
            VertexAt(0.5f, -0.5f, 0.0f),
            VertexAt(0.0f, 0.5f, 0.0f)
        };
        artifact.indices = {0u, 1u, 2u};

        const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(artifact);
        std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void CopyTextFile(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
    }

    void CopyFallbackShaderAssets(const std::filesystem::path& engineAssetsRoot)
    {
        const auto sourceRoot = std::filesystem::path(NLS_ROOT_DIR) / "App" / "Assets" / "Engine" / "Shaders";
        const auto destinationRoot = engineAssetsRoot / "Shaders";

        CopyTextFile(sourceRoot / "Lambert.hlsl", destinationRoot / "Lambert.hlsl");
        CopyTextFile(sourceRoot / "Standard.hlsl", destinationRoot / "Standard.hlsl");
        CopyTextFile(sourceRoot / "CommonTypes.hlsli", destinationRoot / "CommonTypes.hlsli");
        CopyTextFile(sourceRoot / "LightGridCommon.hlsli", destinationRoot / "LightGridCommon.hlsli");
    }

    std::string ReadRepoTextFile(const std::filesystem::path& relativePath)
    {
        const auto path = std::filesystem::path(NLS_ROOT_DIR) / relativePath;
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return {};

        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }
}

TEST(RenderSceneCacheTests, StableSceneReusesPersistentPrimitivesAndCachedCommandsAcrossFrames)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;

    auto first = renderScene.Synchronize(fixture.scene, options);
    const auto firstVisible = renderScene.GatherVisibleCommands({});
    const auto firstOptimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();
    auto second = renderScene.Synchronize(fixture.scene, options);
    const auto secondVisible = renderScene.GatherVisibleCommands({});
    const auto secondOptimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    ASSERT_EQ(renderScene.GetPrimitiveCount(), 1u);
    ASSERT_EQ(firstVisible.opaques.size(), 1u);
    ASSERT_EQ(secondVisible.opaques.size(), 1u);
    EXPECT_EQ(first.addedPrimitiveCount, 1u);
    EXPECT_EQ(first.reusedPrimitiveCount, 0u);
    EXPECT_EQ(first.rebuiltCachedCommandCount, 1u);
    EXPECT_EQ(second.addedPrimitiveCount, 0u);
    EXPECT_EQ(second.reusedPrimitiveCount, 1u);
    EXPECT_EQ(second.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(renderScene.GetCachedCommandBuildCountForTesting(), 1u);
    EXPECT_EQ(firstOptimizationStats.rawVisibleObjectCount, 1u);
    EXPECT_EQ(firstOptimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(firstOptimizationStats.cachedCommandRebuildCount, 1u);
    EXPECT_EQ(secondOptimizationStats.rawVisibleObjectCount, 1u);
    EXPECT_EQ(secondOptimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(secondOptimizationStats.cachedCommandRebuildCount, 0u);
}

TEST(RenderSceneCacheTests, RenderSceneIdentityIsNotCopyable)
{
    static_assert(!std::is_copy_constructible_v<NLS::Engine::Rendering::RenderScene>);
    static_assert(!std::is_copy_assignable_v<NLS::Engine::Rendering::RenderScene>);
    static_assert(std::is_move_constructible_v<NLS::Engine::Rendering::RenderScene>);
    static_assert(std::is_move_assignable_v<NLS::Engine::Rendering::RenderScene>);
}

TEST(RenderSceneCacheTests, MovedFromRenderSceneReceivesFreshSceneIdentityBeforeReuse)
{
    ManyPrimitiveFixture fixture(1u);
    NLS::Engine::Rendering::RenderScene source;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(source.Synchronize(fixture.scene, options).addedPrimitiveCount, 1u);
    const auto sourceSnapshot = source.CreatePrimitiveSnapshot();
    ASSERT_EQ(sourceSnapshot.primitiveRecords.size(), 1u);
    const auto sourceHandle = sourceSnapshot.primitiveRecords[0].handle;

    NLS::Engine::Rendering::RenderScene moved(std::move(source));
    EXPECT_TRUE(moved.IsPrimitiveHandleLive(sourceHandle));
    EXPECT_FALSE(source.IsPrimitiveHandleLive(sourceHandle));

    const auto movedFromSync = source.Synchronize(fixture.scene, options);
    ASSERT_EQ(movedFromSync.addedPrimitiveCount, 1u);
    const auto movedFromSnapshot = source.CreatePrimitiveSnapshot();
    ASSERT_EQ(movedFromSnapshot.primitiveRecords.size(), 1u);
    EXPECT_NE(movedFromSnapshot.sceneId, moved.CreatePrimitiveSnapshot().sceneId);
    EXPECT_NE(movedFromSnapshot.primitiveRecords[0].handle.sceneId, sourceHandle.sceneId);
    EXPECT_FALSE(moved.IsPrimitiveHandleLive(movedFromSnapshot.primitiveRecords[0].handle));
}

TEST(RenderSceneCacheTests, MaterialStateChangeInvalidatesOnlyAffectedCachedCommand)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).rebuiltCachedCommandCount, 1u);

    fixture.material.SetBlendable(true);
    const auto changed = renderScene.Synchronize(fixture.scene, options);

    EXPECT_EQ(changed.addedPrimitiveCount, 0u);
    EXPECT_EQ(changed.reusedPrimitiveCount, 1u);
    EXPECT_EQ(changed.rebuiltCachedCommandCount, 1u);
    EXPECT_EQ(renderScene.GetCachedCommandBuildCountForTesting(), 2u);
}

TEST(RenderSceneCacheTests, GeneratedMaterialStateMaskClearsUnusedBits)
{
    constexpr uint8_t kUsedRenderStateBitsMask = 0x3Fu;
    constexpr uint8_t kUnusedRenderStateBitsMask = static_cast<uint8_t>(~kUsedRenderStateBitsMask);
    NLS::Render::Resources::Material material;

    material.SetDepthWriting(true);
    material.SetColorWriting(true);
    material.SetBlendable(true);
    material.SetDepthTest(true);
    material.SetBackfaceCulling(true);
    material.SetFrontfaceCulling(true);

    const auto stateMask = material.GenerateStateMask();

    EXPECT_EQ(stateMask.mask & kUnusedRenderStateBitsMask, 0u);
    EXPECT_EQ(stateMask.mask & kUsedRenderStateBitsMask, kUsedRenderStateBitsMask);
}

TEST(RenderSceneCacheTests, TransformAndUserMatrixUpdateVisibleObjectDescriptorWithoutRebuildingCommand)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).rebuiltCachedCommandCount, 1u);

    fixture.meshRenderer->gameobject()->GetTransform()->SetWorldPosition({3.0f, 4.0f, 5.0f});
    fixture.meshRenderer->SetUserMatrixElement(0u, 3u, 9.0f);
    const auto unchangedCommands = renderScene.Synchronize(fixture.scene, options);
    const auto visible = renderScene.GatherVisibleCommands({});
    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(unchangedCommands.rebuiltCachedCommandCount, 0u);

    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    ASSERT_TRUE(visible.opaques.front().second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(descriptor));
    const auto& expectedWorldMatrix = fixture.meshRenderer->gameobject()->GetTransform()->GetWorldMatrix();
    for (size_t index = 0u; index < 16u; ++index)
        EXPECT_FLOAT_EQ(descriptor.modelMatrix.data[index], expectedWorldMatrix.data[index]);
    EXPECT_FLOAT_EQ(descriptor.userMatrix.data[3], 9.0f);
    EXPECT_EQ(renderScene.GetCachedCommandBuildCountForTesting(), 1u);
    EXPECT_EQ(optimizationStats.rawVisibleObjectCount, 1u);
    EXPECT_EQ(optimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(optimizationStats.cachedCommandRebuildCount, 0u);
}

TEST(RenderSceneCacheTests, GatherVisibleCommandsAssignsStablePerFrameObjectIndices)
{
    ManyPrimitiveFixture fixture(6u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 6u);

    const auto firstVisible = renderScene.GatherVisibleCommands({});
    const auto secondVisible = renderScene.GatherVisibleCommands({});

    ASSERT_EQ(firstVisible.opaques.size(), 6u);
    ASSERT_EQ(secondVisible.opaques.size(), firstVisible.opaques.size());

    for (size_t drawIndex = 0u; drawIndex < firstVisible.opaques.size(); ++drawIndex)
    {
        NLS::Engine::Rendering::EngineDrawableDescriptor firstDescriptor;
        NLS::Engine::Rendering::EngineDrawableDescriptor secondDescriptor;
        ASSERT_TRUE(firstVisible.opaques[drawIndex].second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(
            firstDescriptor));
        ASSERT_TRUE(secondVisible.opaques[drawIndex].second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(
            secondDescriptor));

        EXPECT_EQ(firstDescriptor.objectIndex, static_cast<uint32_t>(drawIndex));
        EXPECT_EQ(secondDescriptor.objectIndex, static_cast<uint32_t>(drawIndex));
    }
}

TEST(RenderSceneCacheTests, RemovedMeshRendererRemovesPersistentPrimitive)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).addedPrimitiveCount, 1u);

    EXPECT_TRUE(fixture.scene.DestroyGameObject(*fixture.meshRenderer->gameobject()));
    fixture.meshRenderer = nullptr;

    const auto removed = renderScene.Synchronize(fixture.scene, options);

    EXPECT_EQ(removed.removedPrimitiveCount, 1u);
    EXPECT_EQ(removed.syncFullSweepCount, 1u);
    EXPECT_EQ(removed.syncSweepTouchedSlotCount, 1u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u);
    EXPECT_TRUE(renderScene.GatherVisibleCommands({}).opaques.empty());
}

TEST(RenderSceneCacheTests, MarkedDestroyedMeshRendererRemovesPersistentPrimitiveBeforeGarbageCollection)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).addedPrimitiveCount, 1u);

    fixture.meshRenderer->gameobject()->MarkAsDestroy();

    const auto removed = renderScene.Synchronize(fixture.scene, options);

    EXPECT_EQ(removed.removedPrimitiveCount, 1u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u);
    EXPECT_TRUE(renderScene.GatherVisibleCommands({}).opaques.empty());
}

TEST(RenderSceneCacheTests, PrimitiveHandlesRejectRemovedLowerIndexAliasAfterSlotReuse)
{
    ManyPrimitiveFixture fixture(3u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).addedPrimitiveCount, 3u);

    const auto initialSnapshot = renderScene.CreatePrimitiveSnapshot();
    ASSERT_EQ(initialSnapshot.primitiveRecords.size(), 3u);
    const auto removedLowerIndexHandle = initialSnapshot.primitiveRecords[0].handle;
    const auto survivingHigherIndexHandle = initialSnapshot.primitiveRecords[1].handle;
    EXPECT_TRUE(initialSnapshot.primitiveRecords[0].ownerAlive);
    EXPECT_TRUE(initialSnapshot.primitiveRecords[0].ownerActive);
    EXPECT_TRUE(renderScene.IsPrimitiveHandleLive(removedLowerIndexHandle));
    EXPECT_TRUE(renderScene.IsPrimitiveHandleLive(survivingHigherIndexHandle));

    auto* removedObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    ASSERT_NE(removedObject, nullptr);
    EXPECT_TRUE(fixture.scene.DestroyGameObject(*removedObject));
    const auto removed = renderScene.Synchronize(fixture.scene, options);

    EXPECT_EQ(removed.removedPrimitiveCount, 1u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 2u);
    EXPECT_FALSE(renderScene.IsPrimitiveHandleLive(removedLowerIndexHandle));
    EXPECT_TRUE(renderScene.IsPrimitiveHandleLive(survivingHigherIndexHandle));

    auto& newObject = fixture.scene.CreateGameObject("SlotReusePrimitive");
    auto* mesh = CreateSingleMesh();
    fixture.meshes.push_back(mesh);
    auto* meshFilter = newObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = newObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    meshFilter->SetMesh(mesh);
    meshRenderer->FillWithMaterial(fixture.material);

    const auto reused = renderScene.Synchronize(fixture.scene, options);
    const auto reuseSnapshot = renderScene.CreatePrimitiveSnapshot();

    ASSERT_EQ(reused.addedPrimitiveCount, 1u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 3u);
    EXPECT_FALSE(renderScene.IsPrimitiveHandleLive(removedLowerIndexHandle));
    ASSERT_EQ(reuseSnapshot.primitiveRecords.size(), 3u);

    const auto reusedRecord = std::find_if(
        reuseSnapshot.primitiveRecords.begin(),
        reuseSnapshot.primitiveRecords.end(),
        [removedLowerIndexHandle](const auto& record)
        {
            return record.handle.index == removedLowerIndexHandle.index;
        });
    ASSERT_NE(reusedRecord, reuseSnapshot.primitiveRecords.end());
    EXPECT_TRUE(reusedRecord->ownerAlive);
    EXPECT_TRUE(reusedRecord->ownerActive);
    EXPECT_EQ(reusedRecord->handle.index, removedLowerIndexHandle.index);
    EXPECT_NE(reusedRecord->handle.generation, removedLowerIndexHandle.generation);
}

TEST(RenderSceneCacheTests, PrimitiveHandlesRejectMeshRendererAddressReuseWithDifferentInstanceIdentity)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).addedPrimitiveCount, 1u);

    const auto initialSnapshot = renderScene.CreatePrimitiveSnapshot();
    ASSERT_EQ(initialSnapshot.primitiveRecords.size(), 1u);
    const auto staleHandle = initialSnapshot.primitiveRecords[0].handle;
    ASSERT_TRUE(renderScene.IsPrimitiveHandleLive(staleHandle));

    const auto replacementIdentity = NLS::Object::ReserveInstanceID();
    NLS::Object::AssignInstanceID(fixture.meshRenderer, replacementIdentity);

    const auto identityRefresh = renderScene.Synchronize(fixture.scene, options);
    const auto refreshedSnapshot = renderScene.CreatePrimitiveSnapshot();

    EXPECT_EQ(identityRefresh.addedPrimitiveCount, 1u);
    EXPECT_EQ(identityRefresh.removedPrimitiveCount, 1u);
    EXPECT_EQ(identityRefresh.reusedPrimitiveCount, 0u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 1u);
    EXPECT_FALSE(renderScene.IsPrimitiveHandleLive(staleHandle));
    ASSERT_EQ(refreshedSnapshot.primitiveRecords.size(), 1u);
    EXPECT_NE(refreshedSnapshot.primitiveRecords[0].handle, staleHandle);
    EXPECT_TRUE(renderScene.IsPrimitiveHandleLive(refreshedSnapshot.primitiveRecords[0].handle));
}

TEST(RenderSceneCacheTests, LargeSceneTelemetryReportsSyncTouchedBoundsDirtyAndSlotReuseCounts)
{
    ManyPrimitiveFixture fixture(3u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    const auto firstSync = renderScene.Synchronize(fixture.scene, options);
    ASSERT_EQ(firstSync.addedPrimitiveCount, 3u);

    NLS::Tests::LargeScene::ExpectTouchedTelemetry(
        renderScene.GetLastLargeSceneTelemetryForTesting(),
        {
            .registeredPrimitiveCount = 3u,
            .syncTouchedPrimitiveCount = 3u,
            .syncFullSweepCount = 1u,
            .boundsDirtyPrimitiveCount = 3u,
            .primitiveSlotReuseCount = 0u,
            .allocatedPrimitiveSlotCount = 3u,
            .tombstonedPrimitiveSlotCount = 0u,
            .syncSweepTouchedSlotCount = 3u
        });
    EXPECT_GT(renderScene.GetLastLargeSceneTelemetryForTesting().syncTimeNs, 0u);

    const auto stableSync = renderScene.Synchronize(fixture.scene, options);
    EXPECT_EQ(stableSync.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(stableSync.reusedPrimitiveCount, 3u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().syncTouchedPrimitiveCount, 3u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().syncFullSweepCount, 0u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().syncSweepTouchedSlotCount, 0u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().boundsDirtyPrimitiveCount, 0u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().commandOffsetRebuildCount, 0u);

    auto* removedObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    ASSERT_NE(removedObject, nullptr);
    ASSERT_TRUE(fixture.scene.DestroyGameObject(*removedObject));
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).removedPrimitiveCount, 1u);

    auto& newObject = fixture.scene.CreateGameObject("TelemetrySlotReusePrimitive");
    auto* mesh = CreateSingleMesh();
    fixture.meshes.push_back(mesh);
    auto* meshFilter = newObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = newObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    meshFilter->SetMesh(mesh);
    meshRenderer->FillWithMaterial(fixture.material);

    const auto reusedSlot = renderScene.Synchronize(fixture.scene, options);
    EXPECT_EQ(reusedSlot.addedPrimitiveCount, 1u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().primitiveSlotReuseCount, 1u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().registeredPrimitiveCount, 3u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().allocatedPrimitiveSlotCount, 3u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().tombstonedPrimitiveSlotCount, 0u);
}

TEST(RenderSceneCacheTests, LargeSceneTelemetryReportsVisibilityAndQueueFinalizationTouchedCounts)
{
    ManyPrimitiveFixture fixture(9u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 9u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = false;
    settings.parallelVisibilityPrimitivesPerTask = 32u;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    NLS::Tests::LargeScene::ExpectTouchedTelemetry(
        telemetry,
        {
            .registeredPrimitiveCount = 9u,
            .syncTouchedPrimitiveCount = 9u,
            .syncFullSweepCount = 1u,
            .boundsDirtyPrimitiveCount = 9u,
            .primitiveSlotReuseCount = 0u,
            .fullScanCandidateCount = 9u,
            .primitiveRecordsTouched = 18u,
            .allocatedPrimitiveSlotCount = 9u,
            .tombstonedPrimitiveSlotCount = 0u,
            .syncSweepTouchedSlotCount = 9u,
            .visibilityTestedPrimitiveCount = 9u,
            .finalizationTouchedPrimitiveCount = 9u,
            .finalizationTouchedCommandCount = 6u
        });
    NLS::Tests::LargeScene::ExpectDrawResidencyTelemetry(
        telemetry,
        {
            .visiblePrimitiveCount = 6u,
            .visibleMeshCount = 6u,
            .rawVisibleDrawCount = 6u,
            .submittedDrawCount = static_cast<uint64_t>(visible.opaques.size())
        });
    EXPECT_EQ(telemetry.commandOffsetRebuildCount, 1u);
    EXPECT_GT(telemetry.syncTimeNs, 0u);
    EXPECT_GT(telemetry.serialVisibilityTimeNs, 0u);
    EXPECT_GT(telemetry.queueFinalizationTimeNs, 0u);
}

TEST(RenderSceneCacheTests, SpatialIndexVisibilityMatchesFullScanAndReportsSpatialCandidates)
{
    ManyPrimitiveFixture fixture(192u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 192u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions fullScanOptions;
    fullScanOptions.frustum = &frustum;
    fullScanOptions.cameraPosition = {};

    NLS::Engine::Rendering::RenderSceneVisibilityOptions spatialOptions = fullScanOptions;
    spatialOptions.largeSceneSettings = &settings;

    const auto fullScan = renderScene.EvaluateVisibilityForTesting(
        fullScanOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto spatial = renderScene.EvaluateVisibilityForTesting(
        spatialOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto visible = renderScene.GatherVisibleCommands(
        spatialOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    EXPECT_EQ(spatial.primitiveBits, fullScan.primitiveBits);
    EXPECT_EQ(spatial.meshBits, fullScan.meshBits);
    EXPECT_EQ(spatial.visiblePrimitiveCount, fullScan.visiblePrimitiveCount);
    EXPECT_EQ(spatial.visibleMeshCount, fullScan.visibleMeshCount);
    EXPECT_GT(telemetry.spatialCandidateCount, 0u);
    EXPECT_LT(telemetry.spatialCandidateCount, telemetry.registeredPrimitiveCount);
    EXPECT_EQ(telemetry.fullScanCandidateCount, 0u);
    EXPECT_LT(telemetry.visibilityTestedPrimitiveCount, telemetry.registeredPrimitiveCount);
    EXPECT_LT(telemetry.finalizationTouchedPrimitiveCount, telemetry.registeredPrimitiveCount);
    EXPECT_EQ(telemetry.finalizationTouchedPrimitiveCount, telemetry.visiblePrimitiveCount);
    EXPECT_EQ(telemetry.staticPrimitiveCount, telemetry.registeredPrimitiveCount);
    EXPECT_EQ(telemetry.dynamicPrimitiveCount, 0u);
    EXPECT_EQ(telemetry.unclassifiedPrimitiveCount, 0u);
    EXPECT_EQ(telemetry.staticIndexRebuildCount, 1u);
    EXPECT_EQ(telemetry.dynamicIndexUpdateCount, 1u);
    EXPECT_EQ(visible.opaques.size(), spatial.visibleMeshCount);
}

TEST(RenderSceneCacheTests, GatherVisibleCommandsUsesSparseSpatialSnapshotWithoutFullSceneBitsets)
{
    ManyPrimitiveFixture fixture(192u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 192u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions spatialOptions;
    spatialOptions.frustum = &frustum;
    spatialOptions.cameraPosition = {};
    spatialOptions.largeSceneSettings = &settings;

    const auto visible = renderScene.GatherVisibleCommands(
        spatialOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    EXPECT_EQ(telemetry.visibilityBitsetWordCount, 0u);
    EXPECT_EQ(telemetry.finalizationTouchedPrimitiveCount, telemetry.visiblePrimitiveCount);
    EXPECT_EQ(visible.opaques.size(), telemetry.visibleMeshCount);

    const auto comparableSnapshot = renderScene.EvaluateVisibilityForTesting(
        spatialOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    EXPECT_FALSE(comparableSnapshot.primitiveBits.empty());
    EXPECT_FALSE(comparableSnapshot.meshBits.empty());
}

TEST(RenderSceneCacheTests, SpatialIndexSyncUsesDirtyHandlesAfterInitialRebuild)
{
    ManyPrimitiveFixture fixture(3u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().staticIndexRebuildCount, 1u);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().dynamicIndexUpdateCount, 1u);

    const auto stableSync = renderScene.Synchronize(fixture.scene, syncOptions);
    const auto& stableTelemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    EXPECT_EQ(stableSync.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(stableTelemetry.staticIndexRebuildCount, 0u);
    EXPECT_EQ(stableTelemetry.staticIndexRefitCount, 0u);
    EXPECT_EQ(stableTelemetry.dynamicIndexUpdateCount, 0u);
    EXPECT_EQ(stableTelemetry.syncFullSweepCount, 0u);
    EXPECT_EQ(stableTelemetry.syncSweepTouchedSlotCount, 0u);

    auto* removedObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    ASSERT_NE(removedObject, nullptr);
    ASSERT_TRUE(fixture.scene.DestroyGameObject(*removedObject));

    const auto removedSync = renderScene.Synchronize(fixture.scene, syncOptions);
    const auto& removedTelemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    EXPECT_EQ(removedSync.removedPrimitiveCount, 1u);
    EXPECT_EQ(removedTelemetry.staticIndexRebuildCount, 0u);
    EXPECT_EQ(removedTelemetry.staticIndexRefitCount, 1u);
    EXPECT_EQ(removedTelemetry.dynamicIndexUpdateCount, 0u);
    EXPECT_EQ(removedTelemetry.syncFullSweepCount, 1u);
    EXPECT_EQ(removedTelemetry.syncSweepTouchedSlotCount, 3u);
}

TEST(RenderSceneCacheTests, SpatialIndexDirtyTransformUpdatePreservesVisibilityEquivalence)
{
    ManyPrimitiveFixture fixture(3u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    auto* movedObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    ASSERT_NE(movedObject, nullptr);
    movedObject->GetTransform()->SetWorldPosition({ 0.0f, 0.0f, -6.0f });

    const auto movedSync = renderScene.Synchronize(fixture.scene, syncOptions);
    const auto& syncTelemetry = renderScene.GetLastLargeSceneTelemetryForTesting();
    EXPECT_EQ(movedSync.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(syncTelemetry.boundsDirtyPrimitiveCount, 1u);
    EXPECT_EQ(syncTelemetry.staticIndexRebuildCount, 0u);
    EXPECT_EQ(syncTelemetry.staticIndexRefitCount, 1u);
    EXPECT_EQ(syncTelemetry.dynamicIndexUpdateCount, 0u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions fullScanOptions;
    fullScanOptions.frustum = &frustum;
    fullScanOptions.cameraPosition = {};

    auto spatialOptions = fullScanOptions;
    spatialOptions.largeSceneSettings = &settings;

    const auto fullScan = renderScene.EvaluateVisibilityForTesting(
        fullScanOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto spatial = renderScene.EvaluateVisibilityForTesting(
        spatialOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(fullScan.visiblePrimitiveCount, 3u);
    EXPECT_EQ(spatial.primitiveBits, fullScan.primitiveBits);
    EXPECT_EQ(spatial.meshBits, fullScan.meshBits);
    EXPECT_EQ(spatial.visiblePrimitiveCount, fullScan.visiblePrimitiveCount);
    EXPECT_EQ(spatial.visibleMeshCount, fullScan.visibleMeshCount);
}

TEST(RenderSceneCacheTests, SpatialIndexDirtyActiveStateUpdatePreservesVisibilityEquivalence)
{
    ManyPrimitiveFixture fixture(2u);
    NLS::Engine::Rendering::RenderScene renderScene;

    auto* toggledObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    ASSERT_NE(toggledObject, nullptr);
    toggledObject->GetTransform()->SetWorldPosition({ 0.0f, 0.0f, -6.0f });
    toggledObject->SetActive(false);

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    toggledObject->SetActive(true);
    const auto activeSync = renderScene.Synchronize(fixture.scene, syncOptions);
    const auto& syncTelemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    EXPECT_EQ(activeSync.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(syncTelemetry.staticIndexRebuildCount, 0u);
    EXPECT_EQ(syncTelemetry.staticIndexRefitCount, 1u);
    EXPECT_EQ(syncTelemetry.dynamicIndexUpdateCount, 0u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions fullScanOptions;
    fullScanOptions.frustum = &frustum;
    fullScanOptions.cameraPosition = {};

    auto spatialOptions = fullScanOptions;
    spatialOptions.largeSceneSettings = &settings;

    const auto fullScan = renderScene.EvaluateVisibilityForTesting(
        fullScanOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto spatial = renderScene.EvaluateVisibilityForTesting(
        spatialOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(fullScan.visiblePrimitiveCount, 2u);
    EXPECT_EQ(spatial.primitiveBits, fullScan.primitiveBits);
    EXPECT_EQ(spatial.meshBits, fullScan.meshBits);
    EXPECT_EQ(spatial.visiblePrimitiveCount, fullScan.visiblePrimitiveCount);
    EXPECT_EQ(spatial.visibleMeshCount, fullScan.visibleMeshCount);
}

TEST(RenderSceneCacheTests, SpatialIndexUsesConservativeWideFrustumQueryRadius)
{
    ManyPrimitiveFixture fixture(2u);
    NLS::Engine::Rendering::RenderScene renderScene;

    auto* edgeObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    auto* centerObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive1");
    ASSERT_NE(edgeObject, nullptr);
    ASSERT_NE(centerObject, nullptr);
    edgeObject->GetTransform()->SetWorldPosition({ 300.0f, 0.0f, -90.0f });
    centerObject->GetTransform()->SetWorldPosition({ 0.0f, 0.0f, -6.0f });

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    auto frustum = CreateWideForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions fullScanOptions;
    fullScanOptions.frustum = &frustum;
    fullScanOptions.cameraPosition = {};

    auto spatialOptions = fullScanOptions;
    spatialOptions.largeSceneSettings = &settings;

    const auto fullScan = renderScene.EvaluateVisibilityForTesting(
        fullScanOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto spatial = renderScene.EvaluateVisibilityForTesting(
        spatialOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(fullScan.visiblePrimitiveCount, 2u);
    EXPECT_EQ(spatial.primitiveBits, fullScan.primitiveBits);
    EXPECT_EQ(spatial.meshBits, fullScan.meshBits);
    EXPECT_EQ(spatial.visiblePrimitiveCount, fullScan.visiblePrimitiveCount);
    EXPECT_EQ(spatial.visibleMeshCount, fullScan.visibleMeshCount);
}

TEST(RenderSceneCacheTests, SpatialIndexPreservesFrustumDisabledPrimitiveVisibility)
{
    ManyPrimitiveFixture fixture(2u);
    NLS::Engine::Rendering::RenderScene renderScene;

    auto* disabledObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    auto* culledObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive1");
    ASSERT_NE(disabledObject, nullptr);
    ASSERT_NE(culledObject, nullptr);
    auto* disabledRenderer = disabledObject->GetComponent<NLS::Engine::Components::MeshRenderer>();
    auto* culledRenderer = culledObject->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(disabledRenderer, nullptr);
    ASSERT_NE(culledRenderer, nullptr);

    disabledRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
    culledRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
    disabledObject->GetTransform()->SetWorldPosition({ 1000.0f, 0.0f, -6.0f });
    culledObject->GetTransform()->SetWorldPosition({ 0.0f, 0.0f, -6.0f });

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions fullScanOptions;
    fullScanOptions.frustum = &frustum;
    fullScanOptions.cameraPosition = {};

    auto spatialOptions = fullScanOptions;
    spatialOptions.largeSceneSettings = &settings;

    const auto fullScan = renderScene.EvaluateVisibilityForTesting(
        fullScanOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto spatial = renderScene.EvaluateVisibilityForTesting(
        spatialOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(fullScan.visiblePrimitiveCount, 2u);
    EXPECT_EQ(spatial.primitiveBits, fullScan.primitiveBits);
    EXPECT_EQ(spatial.meshBits, fullScan.meshBits);
    EXPECT_EQ(spatial.visiblePrimitiveCount, fullScan.visiblePrimitiveCount);
    EXPECT_EQ(spatial.visibleMeshCount, fullScan.visibleMeshCount);
}

TEST(RenderSceneCacheTests, LargeSceneTelemetrySeparatesTombstonedSlotsFromLiveVisibilityWork)
{
    ManyPrimitiveFixture fixture(3u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    auto* removedObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    ASSERT_NE(removedObject, nullptr);
    ASSERT_TRUE(fixture.scene.DestroyGameObject(*removedObject));
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).removedPrimitiveCount, 1u);

    const auto visible = renderScene.GatherVisibleCommands(
        {},
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    ASSERT_EQ(visible.opaques.size(), 2u);
    EXPECT_EQ(telemetry.registeredPrimitiveCount, 2u);
    EXPECT_EQ(telemetry.allocatedPrimitiveSlotCount, 3u);
    EXPECT_EQ(telemetry.tombstonedPrimitiveSlotCount, 1u);
    EXPECT_EQ(telemetry.syncSweepTouchedSlotCount, 3u);
    EXPECT_EQ(telemetry.spatialCandidateCount, 0u);
    EXPECT_EQ(telemetry.fullScanCandidateCount, 2u);
    EXPECT_EQ(telemetry.primitiveRecordsTouched, 4u);
    EXPECT_EQ(telemetry.visibilityTestedPrimitiveCount, 2u);
    EXPECT_EQ(telemetry.finalizationTouchedPrimitiveCount, 2u);
    EXPECT_EQ(telemetry.finalizationTouchedCommandCount, 2u);
}

TEST(RenderSceneCacheTests, LargeSceneTestHelpersBuildScenesAndFormatTelemetryRows)
{
    const auto partitioned = NLS::Tests::LargeScene::BuildPartitionedPrimitiveScene(
        4u,
        2u,
        10.0f,
        3u);

    ASSERT_EQ(partitioned.primitives.size(), 8u);
    EXPECT_EQ(partitioned.staticPrimitiveCount, 5u);
    EXPECT_EQ(partitioned.dynamicPrimitiveCount, 3u);
    EXPECT_EQ(partitioned.primitives[5].centerX, 10.0f);
    EXPECT_EQ(partitioned.primitives[5].centerZ, 10.0f);
    EXPECT_EQ(partitioned.primitives[5].layer, 2u);

    const auto linear = NLS::Tests::LargeScene::BuildLinearPrimitiveScene(4u, 2.0f);
    ASSERT_EQ(linear.primitives.size(), 4u);
    EXPECT_EQ(linear.staticPrimitiveCount, 4u);
    EXPECT_EQ(linear.dynamicPrimitiveCount, 0u);

    NLS::Tests::LargeScene::TouchedCountSample touched;
    touched.registeredPrimitiveCount = 100u;
    touched.fullScanCandidateCount = 24u;
    touched.visibilityTestedPrimitiveCount = 34u;
    EXPECT_TRUE(NLS::Tests::LargeScene::IsCandidateRatioWithinBudget(24u, 100u, 0.25));
    EXPECT_FALSE(NLS::Tests::LargeScene::IsCandidateRatioWithinBudget(26u, 100u, 0.25));
    EXPECT_TRUE(NLS::Tests::LargeScene::AreTouchedCountsBounded(touched, 0.25, 0.35));
    touched.visibilityTestedPrimitiveCount = 36u;
    EXPECT_FALSE(NLS::Tests::LargeScene::AreTouchedCountsBounded(touched, 0.25, 0.35));

    touched.syncTouchedPrimitiveCount = 7u;
    touched.syncFullSweepCount = 1u;
    touched.boundsDirtyPrimitiveCount = 2u;
    touched.primitiveSlotReuseCount = 3u;
    touched.primitiveRecordsTouched = 50u;
    touched.allocatedPrimitiveSlotCount = 110u;
    touched.tombstonedPrimitiveSlotCount = 10u;
    touched.syncSweepTouchedSlotCount = 111u;
    touched.finalizationTouchedPrimitiveCount = 24u;
    touched.finalizationTouchedCommandCount = 25u;
    EXPECT_EQ(
        NLS::Tests::LargeScene::FormatTouchedCountTableRow(9u, touched),
        "| 9 | 100 | 7 | 1 | 2 | 3 | 0 | 24 | 50 | 110 | 10 | 111 | 36 | 24 | 25 |");

    NLS::Tests::LargeScene::TimingSample timing;
    timing.syncTimeNs = 10u;
    timing.serialVisibilityTimeNs = 20u;
    timing.parallelVisibilityTimeNs = 30u;
    timing.queueFinalizationTimeNs = 40u;
    timing.hzbBuildTimeNs = 50u;
    timing.streamingCommitTimeNs = 60u;
    EXPECT_EQ(
        NLS::Tests::LargeScene::FormatTimingTableRow(2u, timing),
        "| 2 | 10 | 20 | 30 | 40 | 50 | 60 |");

    NLS::Tests::LargeScene::DrawResidencySample drawResidency;
    drawResidency.visiblePrimitiveCount = 11u;
    drawResidency.visibleMeshCount = 12u;
    drawResidency.rawVisibleDrawCount = 13u;
    drawResidency.submittedDrawCount = 14u;
    drawResidency.dynamicInstanceGroupCount = 15u;
    drawResidency.streamingDependencyCount = 16u;
    drawResidency.residencyTicketCount = 17u;
    drawResidency.residentCpuBytes = 18u;
    drawResidency.residentGpuBytes = 19u;
    drawResidency.requestedCpuBytes = 20u;
    drawResidency.requestedGpuBytes = 21u;
    EXPECT_EQ(
        NLS::Tests::LargeScene::FormatDrawResidencyTableRow(3u, drawResidency),
        "| 3 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 |");

    NLS::Render::Data::LargeSceneTelemetry telemetry;
    telemetry.visiblePrimitiveCount = drawResidency.visiblePrimitiveCount;
    telemetry.visibleMeshCount = drawResidency.visibleMeshCount;
    telemetry.rawVisibleDrawCount = drawResidency.rawVisibleDrawCount;
    telemetry.submittedDrawCount = drawResidency.submittedDrawCount;
    telemetry.dynamicInstanceGroupCount = drawResidency.dynamicInstanceGroupCount;
    telemetry.streamingDependencyCount = drawResidency.streamingDependencyCount;
    telemetry.residencyTicketCount = drawResidency.residencyTicketCount;
    telemetry.residentCpuBytes = drawResidency.residentCpuBytes;
    telemetry.residentGpuBytes = drawResidency.residentGpuBytes;
    telemetry.requestedCpuBytes = drawResidency.requestedCpuBytes;
    telemetry.requestedGpuBytes = drawResidency.requestedGpuBytes;
    NLS::Tests::LargeScene::ExpectDrawResidencyTelemetry(telemetry, drawResidency);
}

TEST(RenderSceneCacheTests, GatherVisibleCommandsAppliesCameraVisibleLayerMask)
{
    ManyPrimitiveFixture fixture(3u);
    ASSERT_GE(fixture.scene.GetGameObjects().size(), 3u);
    fixture.scene.GetGameObjects()[0]->SetLayer(0);
    fixture.scene.GetGameObjects()[1]->SetLayer(1);
    fixture.scene.GetGameObjects()[2]->SetLayer(0);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = false;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;
    visibilityOptions.visibleLayerMask = 1u << 1u;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.mesh, fixture.meshes[1]);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().visiblePrimitiveCount, 1u);
}

TEST(RenderSceneCacheTests, CullReasonDebugSnapshotFeedsLargeSceneTelemetryReasonCounts)
{
    ManyPrimitiveFixture fixture(3u);
    ASSERT_GE(fixture.scene.GetGameObjects().size(), 3u);
    fixture.scene.GetGameObjects()[0]->SetLayer(0);
    fixture.scene.GetGameObjects()[1]->SetLayer(1);
    fixture.scene.GetGameObjects()[2]->SetLayer(0);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = false;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.visibleLayerMask = 1u << 1u;
    visibilityOptions.largeSceneSettings = &settings;
    visibilityOptions.enableCullReasonDebugSnapshot = true;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(visible.opaques.size(), 1u);
    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();
    EXPECT_EQ(telemetry.culledByReason[0], 1u);
    EXPECT_EQ(telemetry.culledByReason[2], 2u);
    EXPECT_EQ(telemetry.visiblePrimitiveCount, 1u);
}

TEST(RenderSceneCacheTests, BaseSceneRendererParseScenePublishesLargeSceneTelemetryToFrameInfo)
{
    auto& driver = EnsureRenderSceneTestDriver();
    ManyPrimitiveFixture fixture(3u);

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        fixture.scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    renderer.ResetFrameStatistics();
    renderer.ResetFrameStatistics();
    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);
    renderer.FinalizeFrameStatistics();
    renderer.FinalizeFrameStatistics();

    ASSERT_EQ(drawables.opaques.size(), 3u);
    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.largeScene.registeredPrimitiveCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.staticPrimitiveCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.dynamicPrimitiveCount, 0u);
    EXPECT_EQ(frameInfo.largeScene.unclassifiedPrimitiveCount, 0u);
    EXPECT_EQ(frameInfo.largeScene.syncTouchedPrimitiveCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.allocatedPrimitiveSlotCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.syncSweepTouchedSlotCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.spatialCandidateCount, 0u);
    EXPECT_EQ(frameInfo.largeScene.fullScanCandidateCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.primitiveRecordsTouched, 6u);
    EXPECT_EQ(frameInfo.largeScene.visibilityTestedPrimitiveCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.visiblePrimitiveCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.finalizationTouchedPrimitiveCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.finalizationTouchedCommandCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.rawVisibleDrawCount, 3u);
    EXPECT_EQ(frameInfo.largeScene.submittedDrawCount, 3u);
}

TEST(RenderSceneCacheTests, BaseSceneRendererParseScenePlansStreamingResidencyFromVisiblePrimitives)
{
    auto& driver = EnsureRenderSceneTestDriver();
    ManyPrimitiveFixture fixture(3u);

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        fixture.scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    renderer.ResetFrameStatistics();
    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);
    renderer.FinalizeFrameStatistics();

    ASSERT_EQ(drawables.opaques.size(), 3u);
    const auto& largeScene = renderer.GetFrameInfo().largeScene;
    EXPECT_EQ(largeScene.streamingDependencyCount, 3u);
    EXPECT_EQ(largeScene.streamingRequestCount, 3u);
    EXPECT_EQ(largeScene.residencyTicketCount, 3u);
    EXPECT_GT(largeScene.requestedCpuBytes, 0u);
    EXPECT_GT(largeScene.requestedGpuBytes, 0u);
    EXPECT_GT(largeScene.streamingCommitTimeNs, 0u);
}

TEST(RenderSceneCacheTests, BaseSceneRendererParseSceneBuildsHZBOcclusionPacketsFromVisibleOpaquePrimitives)
{
    auto& driver = EnsureRenderSceneTestDriver();
    ScopedHZBPacketTestDevice hzbDevice(driver);
    QueueSortFixture fixture;
    fixture.AddObjectAt("HZBOpaqueSource", *fixture.sharedMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -5.0f });
    fixture.AddObjectAt("HZBTransparentRejected", *fixture.otherMesh, fixture.transparentMaterial, { 2.0f, 0.0f, -5.0f });

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        fixture.scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    camera.CacheMatrices(128u, 128u);
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);
    ASSERT_EQ(drawables.opaques.size(), 1u);
    ASSERT_EQ(drawables.transparents.size(), 1u);

    const auto& packets = renderer.GetHZBPacketBuildResult();
    ASSERT_EQ(packets.primitivePackets.size(), 1u);
    ASSERT_EQ(packets.primitiveInputs.size(), 1u);
    EXPECT_TRUE(packets.primitiveInputs.front().depthWriteEligible);
    EXPECT_EQ(packets.rejectedPrimitiveCount, 1u);
    EXPECT_EQ(packets.primitivePackets.front().flags, 1u);

    renderer.FinalizeFrameStatistics();
    EXPECT_GT(renderer.GetFrameInfo().largeScene.hzbBuildTimeNs, 0u);
}

TEST(RenderSceneCacheTests, BaseSceneRendererPrunesHZBHistoryOnlyForRemovedPrimitiveHandles)
{
    auto& driver = EnsureRenderSceneTestDriver();
    ScopedHZBPacketTestDevice hzbDevice(driver);
    QueueSortFixture fixture;
    fixture.AddObjectAt("StableHZBSourceA", *fixture.sharedMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -5.0f });
    fixture.AddObjectAt("StableHZBSourceB", *fixture.otherMesh, fixture.opaqueMaterialA, { 0.25f, 0.0f, -5.0f });

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        fixture.scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    camera.CacheMatrices(128u, 128u);
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    ASSERT_EQ(renderer.CaptureSceneDrawables(frameDescriptor).opaques.size(), 2u);
    renderer.FinalizeFrameStatistics();
    const auto firstFrameInput = renderer.GetLastHZBFrameInputForTesting();
    const auto firstPackets = renderer.GetHZBPacketBuildResult();
    ASSERT_EQ(firstPackets.primitiveInputs.size(), 2u);
    renderer.SeedHZBHistoryForTesting(firstFrameInput, firstPackets.primitiveInputs);

    renderer.ResetFrameStatistics();
    ASSERT_EQ(renderer.CaptureSceneDrawables(frameDescriptor).opaques.size(), 2u);
    renderer.FinalizeFrameStatistics();
    const auto& stableTelemetry = renderer.GetFrameInfo().largeScene;
    EXPECT_EQ(stableTelemetry.hzbHistoryPruneTouchedHandleCount, 0u);
    EXPECT_EQ(stableTelemetry.hzbHistoryPruneRemovedHandleCount, 0u);
    EXPECT_EQ(stableTelemetry.hzbHistoryPruneRemovedKeyCount, 0u);
    EXPECT_EQ(stableTelemetry.occlusionTestCount, 2u);

    auto* removedObject = fixture.scene.FindGameObjectByName("StableHZBSourceA");
    ASSERT_NE(removedObject, nullptr);
    ASSERT_TRUE(fixture.scene.DestroyGameObject(*removedObject));

    renderer.ResetFrameStatistics();
    ASSERT_EQ(renderer.CaptureSceneDrawables(frameDescriptor).opaques.size(), 1u);
    renderer.FinalizeFrameStatistics();
    const auto& removedTelemetry = renderer.GetFrameInfo().largeScene;
    EXPECT_EQ(removedTelemetry.hzbHistoryPruneTouchedHandleCount, 1u);
    EXPECT_EQ(removedTelemetry.hzbHistoryPruneRemovedHandleCount, 1u);
    EXPECT_EQ(removedTelemetry.hzbHistoryPruneRemovedKeyCount, 1u);
    EXPECT_GT(removedTelemetry.hzbHistoryPruneTimeNs, 0u);
}

TEST(RenderSceneCacheTests, BaseSceneRendererPrunesDroppedAdditiveHZBHistoryWithoutRemovingMainHistory)
{
    auto& driver = EnsureRenderSceneTestDriver();
    ScopedHZBPacketTestDevice hzbDevice(driver);
    QueueSortFixture mainFixture;
    QueueSortFixture additiveFixture;
    mainFixture.AddObjectAt("MainHZBSource", *mainFixture.sharedMesh, mainFixture.opaqueMaterialA, { 0.0f, 0.0f, -5.0f });
    additiveFixture.AddObjectAt("AdditiveHZBSource", *additiveFixture.sharedMesh, additiveFixture.opaqueMaterialA, { 0.25f, 0.0f, -5.0f });

    SceneDrawableProbeRenderer renderer(driver);
    renderer.SetSceneDescriptorForTesting({
        mainFixture.scene,
        std::nullopt,
        nullptr,
        { &additiveFixture.scene }
    });

    NLS::Render::Entities::Camera camera;
    camera.CacheMatrices(128u, 128u);
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    ASSERT_EQ(renderer.CaptureSceneDrawables(frameDescriptor).opaques.size(), 2u);
    renderer.FinalizeFrameStatistics();
    const auto firstFrameInput = renderer.GetLastHZBFrameInputForTesting();
    const auto firstPackets = renderer.GetHZBPacketBuildResult();
    ASSERT_EQ(firstPackets.primitiveInputs.size(), 2u);
    const auto mainInput = firstPackets.primitiveInputs.front();
    const std::array<uint32_t, 2u> firstFrameFlags = { 1u, 0u };
    renderer.SeedHZBHistoryForTesting(firstFrameInput, firstPackets.primitiveInputs, firstFrameFlags);

    renderer.SetSceneDescriptorForTesting({
        mainFixture.scene,
        std::nullopt,
        nullptr,
        {}
    });

    renderer.ResetFrameStatistics();
    ASSERT_EQ(renderer.CaptureSceneDrawables(frameDescriptor).opaques.size(), 0u);
    renderer.FinalizeFrameStatistics();

    const auto& telemetry = renderer.GetFrameInfo().largeScene;
    EXPECT_EQ(telemetry.hzbHistoryPruneTouchedHandleCount, 1u);
    EXPECT_EQ(telemetry.hzbHistoryPruneRemovedHandleCount, 1u);
    EXPECT_EQ(telemetry.hzbHistoryPruneRemovedKeyCount, 1u);
    EXPECT_GT(telemetry.hzbHistoryPruneTimeNs, 0u);

    const auto mainHistoryKey =
        NLS::Engine::Rendering::SceneOcclusionSystem::BuildHistoryKey(firstFrameInput, mainInput);
    EXPECT_TRUE(renderer.GetHZBOcclusionHistoryForTesting().FindOccludedFrame(mainHistoryKey).has_value());
}

TEST(RenderSceneCacheTests, BaseSceneRendererBuildsHZBPacketsFromSparseVisibleHandlesAfterVisibility)
{
    const auto source = ReadRepoTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.cpp");
    ASSERT_FALSE(source.empty());

    const auto appendStart = source.find("auto appendSceneDrawables = [&]");
    const auto streamingStart = source.find("const auto streamingCommitStart", appendStart);
    ASSERT_NE(appendStart, std::string::npos);
    ASSERT_NE(streamingStart, std::string::npos);
    const auto appendBody = source.substr(appendStart, streamingStart - appendStart);

    const auto gatherVisible = appendBody.find("renderScene.GatherVisibleCommands");
    const auto packetSources = appendBody.find("SceneOcclusionSystem::BuildHZBPrimitivePacketSources");
    ASSERT_NE(gatherVisible, std::string::npos);
    ASSERT_NE(packetSources, std::string::npos);
    EXPECT_LT(gatherVisible, packetSources);

    EXPECT_NE(appendBody.find("renderScene.CreatePrimitiveSnapshotForHandles("), std::string::npos);
    EXPECT_NE(appendBody.find("renderScene.GetLastVisiblePrimitiveHandles()"), std::string::npos);
    EXPECT_EQ(appendBody.find("renderScene.CreatePrimitiveSnapshot()"), std::string::npos);
    EXPECT_EQ(appendBody.find("hzbPrimitiveSnapshot->denseIndexToHandle"), std::string::npos);
    EXPECT_EQ(appendBody.find("occlusionState.primitiveInputs = &occlusionPrimitiveInputs"), std::string::npos);
}

TEST(RenderSceneCacheTests, BaseSceneRendererCommitsStreamingResidencyWithRetiredFramePins)
{
    const auto source = ReadRepoTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.cpp");
    ASSERT_FALSE(source.empty());

    const auto mergeStart = source.find("void MergeStreamingTelemetry(");
    const auto rendererStart = source.find("BaseSceneRenderer::BaseSceneRenderer", mergeStart);
    ASSERT_NE(mergeStart, std::string::npos);
    ASSERT_NE(rendererStart, std::string::npos);
    const auto mergeBody = source.substr(mergeStart, rendererStart - mergeStart);
    EXPECT_NE(mergeBody.find("commit.telemetry.residentCpuBytes"), std::string::npos);
    EXPECT_NE(mergeBody.find("commit.telemetry.residentGpuBytes"), std::string::npos);

    const auto parseScene = source.find("BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()");
    ASSERT_NE(parseScene, std::string::npos);
    const auto streamingStart = source.find("const auto streamingCommitStart", parseScene);
    const auto telemetryMerge = source.find("MergeStreamingTelemetry(streamingTelemetry, allSceneStreamingPlan, allSceneStreamingCommit)", streamingStart);
    ASSERT_NE(streamingStart, std::string::npos);
    ASSERT_NE(telemetryMerge, std::string::npos);
    const auto streamingBody = source.substr(streamingStart, telemetryMerge - streamingStart);
    EXPECT_NE(streamingBody.find("const auto allSceneStreamingPlan = m_streamingResidency.Plan(allSceneStreamingInput, occlusionSettings)"), std::string::npos);
    EXPECT_NE(streamingBody.find("m_streamingResidency.Commit(\n\t\tallSceneStreamingPlan"), std::string::npos);
    EXPECT_NE(streamingBody.find("DriverRendererAccess::CollectStreamingDependencyPins(m_driver)"), std::string::npos);
    EXPECT_NE(streamingBody.find("framePins"), std::string::npos);
    EXPECT_EQ(streamingBody.find("StreamingResidencyFramePins{}"), std::string::npos);
}

TEST(RenderSceneCacheTests, BaseSceneRendererPublishesStreamingPinsThroughFrameSnapshots)
{
    const auto rendererSource = ReadRepoTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.cpp");
    const auto packageBuilderSource = ReadRepoTextFile("Runtime/Rendering/Context/RenderScenePackageBuilder.cpp");
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(packageBuilderSource.empty());

    const auto buildSnapshot = rendererSource.find("BaseSceneRenderer::BuildFrameSnapshot(");
    ASSERT_NE(buildSnapshot, std::string::npos);
    const auto buildSnapshotBody = rendererSource.substr(buildSnapshot, 1200u);
    EXPECT_NE(buildSnapshotBody.find("snapshot->streamingDependencyPins = m_lastStreamingDependencyPins"), std::string::npos);

    const auto parseScene = rendererSource.find("BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()");
    ASSERT_NE(parseScene, std::string::npos);
    const auto parseSceneBody = rendererSource.substr(parseScene);
    EXPECT_NE(parseSceneBody.find("m_lastStreamingDependencyPins ="), std::string::npos);
    EXPECT_NE(parseSceneBody.find("BuildRuntimeStreamingDependencyPins("), std::string::npos);

    EXPECT_NE(packageBuilderSource.find("package.streamingDependencyPins = snapshot.streamingDependencyPins"), std::string::npos);
}

TEST(RenderSceneCacheTests, BaseSceneRendererFeedsModeledStreamingResidencyBackIntoVisibility)
{
    const auto rendererSource = ReadRepoTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.cpp");
    const auto rendererHeader = ReadRepoTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.h");
    const auto renderSceneHeader = ReadRepoTextFile("Runtime/Engine/Rendering/RenderScene.h");
    ASSERT_FALSE(rendererSource.empty());
    ASSERT_FALSE(rendererHeader.empty());
    ASSERT_FALSE(renderSceneHeader.empty());

    EXPECT_NE(renderSceneHeader.find("const RepresentationResidencySnapshot* representationResidency"), std::string::npos);
    EXPECT_NE(rendererHeader.find("RepresentationResidencySnapshot m_lastRepresentationResidency"), std::string::npos);
    EXPECT_NE(rendererSource.find("BuildRepresentationResidencySnapshotFromStreamingCommit"), std::string::npos);
    EXPECT_NE(rendererSource.find("currentFrameRepresentationResidency"), std::string::npos);

    const auto parseScene = rendererSource.find("BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()");
    ASSERT_NE(parseScene, std::string::npos);
    const auto gatherVisible = rendererSource.find("renderScene.GatherVisibleCommands", parseScene);
    const auto streamingCommit = rendererSource.find("const auto allSceneStreamingCommit = m_streamingResidency.Commit", gatherVisible);
    const auto publishResidency = rendererSource.find("m_lastRepresentationResidency = std::move(currentFrameRepresentationResidency)", streamingCommit);
    ASSERT_NE(gatherVisible, std::string::npos);
    ASSERT_NE(streamingCommit, std::string::npos);
    ASSERT_NE(publishResidency, std::string::npos);

    const auto gatherBody = rendererSource.substr(gatherVisible, streamingCommit - gatherVisible);
    EXPECT_NE(gatherBody.find("&m_lastRepresentationResidency"), std::string::npos);

    const auto streamingBody = rendererSource.substr(streamingCommit, publishResidency - streamingCommit);
    EXPECT_NE(streamingBody.find("BuildRepresentationResidencySnapshotFromStreamingCommit("), std::string::npos);
    EXPECT_NE(streamingBody.find("allSceneStreamingPlan"), std::string::npos);
    EXPECT_NE(streamingBody.find("allSceneStreamingCommit"), std::string::npos);
    EXPECT_NE(streamingBody.find("MergeRepresentationResidencySnapshot("), std::string::npos);
}

TEST(RenderSceneCacheTests, BaseSceneRendererCommitsStreamingResidencyOnceForAllScenesPerFrame)
{
    const auto rendererSource = ReadRepoTextFile("Runtime/Engine/Rendering/BaseSceneRenderer.cpp");
    ASSERT_FALSE(rendererSource.empty());

    const auto parseScene = rendererSource.find("BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()");
    ASSERT_NE(parseScene, std::string::npos);
    const auto appendScene = rendererSource.find("auto appendSceneDrawables = [&]", parseScene);
    const auto appendMainCall = rendererSource.find("appendSceneDrawables(sceneDescriptor.scene", appendScene);
    ASSERT_NE(appendScene, std::string::npos);
    ASSERT_NE(appendMainCall, std::string::npos);
    const auto appendBody = rendererSource.substr(appendScene, appendMainCall - appendScene);

    EXPECT_EQ(appendBody.find("m_streamingResidency.Commit("), std::string::npos);
    EXPECT_NE(rendererSource.find("allSceneStreamingInput"), std::string::npos);
    EXPECT_NE(rendererSource.find("m_streamingResidency.Commit(\n\t\tallSceneStreamingPlan"), std::string::npos);
}

TEST(RenderSceneCacheTests, BaseSceneRendererParseSceneAggregatesLargeSceneTelemetryAcrossAdditiveScenes)
{
    auto& driver = EnsureRenderSceneTestDriver();
    ManyPrimitiveFixture mainFixture(2u);
    ManyPrimitiveFixture additiveFixture(3u);

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        mainFixture.scene,
        std::nullopt,
        nullptr,
        { &additiveFixture.scene }
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    renderer.ResetFrameStatistics();
    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);
    renderer.FinalizeFrameStatistics();

    ASSERT_EQ(drawables.opaques.size(), 5u);
    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.largeScene.registeredPrimitiveCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.staticPrimitiveCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.dynamicPrimitiveCount, 0u);
    EXPECT_EQ(frameInfo.largeScene.unclassifiedPrimitiveCount, 0u);
    EXPECT_EQ(frameInfo.largeScene.syncTouchedPrimitiveCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.allocatedPrimitiveSlotCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.syncSweepTouchedSlotCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.spatialCandidateCount, 0u);
    EXPECT_EQ(frameInfo.largeScene.fullScanCandidateCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.primitiveRecordsTouched, 10u);
    EXPECT_EQ(frameInfo.largeScene.visibilityTestedPrimitiveCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.visiblePrimitiveCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.finalizationTouchedPrimitiveCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.finalizationTouchedCommandCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.rawVisibleDrawCount, 5u);
    EXPECT_EQ(frameInfo.largeScene.submittedDrawCount, 5u);
}

TEST(RenderSceneCacheTests, BaseSceneRendererAggregatesLegacyDrawOptimizationStatsAcrossAdditiveScenes)
{
    auto& driver = EnsureRenderSceneTestDriver();
    QueueSortFixture mainFixture;
    QueueSortFixture additiveFixture;
    for (size_t index = 0u; index < 3u; ++index)
    {
        mainFixture.AddObject(
            ("MainGrouped" + std::to_string(index)).c_str(),
            *mainFixture.sharedMesh,
            mainFixture.opaqueMaterialA,
            static_cast<float>(index));
        additiveFixture.AddObject(
            ("AdditiveGrouped" + std::to_string(index)).c_str(),
            *additiveFixture.sharedMesh,
            additiveFixture.opaqueMaterialA,
            static_cast<float>(index));
    }

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        mainFixture.scene,
        std::nullopt,
        nullptr,
        { &additiveFixture.scene }
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    renderer.ResetFrameStatistics();
    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);
    renderer.FinalizeFrameStatistics();

    ASSERT_EQ(drawables.opaques.size(), 2u);
    const auto& frameInfo = renderer.GetFrameInfo();
    EXPECT_EQ(frameInfo.rawVisibleObjectCount, 6u);
    EXPECT_EQ(frameInfo.submittedSceneDrawCount, 2u);
    EXPECT_EQ(frameInfo.dynamicInstanceGroupCount, 2u);
    EXPECT_EQ(frameInfo.largestInstanceGroupSize, 3u);
}

TEST(RenderSceneCacheTests, BaseSceneRendererParseSceneAppliesCameraVisibleLayerMask)
{
    auto& driver = EnsureRenderSceneTestDriver();
    ManyPrimitiveFixture fixture(3u);
    ASSERT_GE(fixture.scene.GetGameObjects().size(), 3u);
    fixture.scene.GetGameObjects()[0]->SetLayer(0);
    fixture.scene.GetGameObjects()[1]->SetLayer(1);
    fixture.scene.GetGameObjects()[2]->SetLayer(0);

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        fixture.scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    camera.SetVisibleLayerMask(1u << 1u);
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    renderer.ResetFrameStatistics();
    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);
    renderer.FinalizeFrameStatistics();

    ASSERT_EQ(drawables.opaques.size(), 1u);
    EXPECT_EQ(drawables.opaques.front().second.mesh, fixture.meshes[1]);
    EXPECT_EQ(renderer.GetFrameInfo().largeScene.visiblePrimitiveCount, 1u);
}

TEST(RenderSceneCacheTests, PrimitiveSnapshotKeepsImmutableHandleAndCommandOffsetState)
{
    ManyPrimitiveFixture fixture(2u);
    ASSERT_GE(fixture.scene.GetGameObjects().size(), 2u);
    fixture.scene.GetGameObjects()[0]->SetLayer(7);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).addedPrimitiveCount, 2u);

    const auto beforeRemoval = renderScene.CreatePrimitiveSnapshot();
    ASSERT_EQ(beforeRemoval.primitiveRecords.size(), 2u);
    ASSERT_EQ(beforeRemoval.denseIndexToHandle.size(), 2u);
    ASSERT_EQ(beforeRemoval.commandOffsetTable.size(), 2u);
    EXPECT_EQ(beforeRemoval.primitiveRecords[0].visibilitySettings.layer, 7u);
    EXPECT_FALSE(beforeRemoval.primitiveRecords[0].visibilitySettings.distanceCullingEnabled);
    EXPECT_EQ(beforeRemoval.primitiveRecords[0].commandOffsetBegin, 0u);
    EXPECT_EQ(beforeRemoval.primitiveRecords[0].commandOffsetEnd, 1u);
    EXPECT_EQ(beforeRemoval.primitiveRecords[1].commandOffsetBegin, 1u);
    EXPECT_EQ(beforeRemoval.primitiveRecords[1].commandOffsetEnd, 2u);
    EXPECT_EQ(beforeRemoval.denseIndexToHandle[0], beforeRemoval.primitiveRecords[0].handle);
    EXPECT_EQ(beforeRemoval.denseIndexToHandle[1], beforeRemoval.primitiveRecords[1].handle);
    EXPECT_EQ(beforeRemoval.dirtySyncHandles.size(), 2u);
    EXPECT_TRUE(beforeRemoval.removedHandles.empty());
    ASSERT_EQ(beforeRemoval.liveHandleBits.size(), 1u);
    EXPECT_EQ(beforeRemoval.liveHandleBits[0] & 0x3ull, 0x3ull);

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).reusedPrimitiveCount, 2u);
    const auto stableFrame = renderScene.CreatePrimitiveSnapshot();
    EXPECT_TRUE(stableFrame.dirtySyncHandles.empty());
    EXPECT_TRUE(stableFrame.removedHandles.empty());

    auto* removedObject = fixture.scene.FindGameObjectByName("VisibilityPrimitive0");
    ASSERT_NE(removedObject, nullptr);
    EXPECT_TRUE(fixture.scene.DestroyGameObject(*removedObject));
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).removedPrimitiveCount, 1u);

    const auto afterRemoval = renderScene.CreatePrimitiveSnapshot();
    ASSERT_EQ(afterRemoval.primitiveRecords.size(), 1u);
    ASSERT_EQ(afterRemoval.removedHandles.size(), 1u);
    EXPECT_EQ(afterRemoval.removedHandles[0], beforeRemoval.primitiveRecords[0].handle);
    EXPECT_EQ(beforeRemoval.primitiveRecords.size(), 2u);
    EXPECT_EQ(beforeRemoval.primitiveRecords[0].handle, beforeRemoval.denseIndexToHandle[0]);
    EXPECT_EQ(beforeRemoval.commandOffsetTable.size(), 2u);
}

TEST(RenderSceneCacheTests, HandleScopedPrimitiveSnapshotPreservesGlobalCommandOffsetRanges)
{
    ManyPrimitiveFixture fixture(2u);
    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).addedPrimitiveCount, 2u);

    const auto fullSnapshot = renderScene.CreatePrimitiveSnapshot();
    ASSERT_EQ(fullSnapshot.primitiveRecords.size(), 2u);
    const auto secondHandle = fullSnapshot.primitiveRecords[1].handle;

    const auto sparseSnapshot = renderScene.CreatePrimitiveSnapshotForHandles({ secondHandle }, {});
    ASSERT_EQ(sparseSnapshot.primitiveRecords.size(), 1u);
    ASSERT_EQ(sparseSnapshot.commandOffsetTable.size(), 1u);
    EXPECT_EQ(sparseSnapshot.primitiveRecords[0].handle, secondHandle);
    EXPECT_EQ(sparseSnapshot.primitiveRecords[0].commandOffsetBegin, 1u);
    EXPECT_EQ(sparseSnapshot.primitiveRecords[0].commandOffsetEnd, 2u);
    EXPECT_EQ(sparseSnapshot.commandOffsetTable[0].handle, secondHandle);
    EXPECT_EQ(sparseSnapshot.commandOffsetTable[0].commandOffsetBegin, 1u);
    EXPECT_EQ(sparseSnapshot.commandOffsetTable[0].commandOffsetEnd, 2u);
}

TEST(RenderSceneCacheTests, SynchronizeRetriesDeferredMeshAndMaterialReferencesAfterResourceRegistration)
{
    using namespace NLS::Engine::Serialize;

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    const auto meshGuid = NLS::Guid::Parse("11111111-2222-4333-8444-555555555555");
    const auto materialGuid = NLS::Guid::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto meshPath = std::string("Library/Artifacts/Hero/body.nmesh");
    const auto materialPath = std::string("Library/Artifacts/Hero/body.nmat");
    const auto meshReference = ObjectIdentifier::Asset(
        AssetId(meshGuid),
        MakeLocalIdentifierInFile(meshGuid, "mesh:body"),
        meshPath);
    const auto materialReference = ObjectIdentifier::Asset(
        AssetId(materialGuid),
        MakeLocalIdentifierInFile(materialGuid, "material:body"),
        materialPath);

    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("Deferred");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMeshReference(MakeRenderScenePPtr<NLS::Render::Resources::Mesh>(meshReference));
    meshRenderer->SetMaterialReferences({
        MakeRenderScenePPtr<NLS::Render::Resources::Material>(materialReference)
    });

    NLS::Render::Resources::Material defaultMaterial;
    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    EXPECT_EQ(renderScene.Synchronize(scene, syncOptions).rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(meshFilter->ResolveMesh(), nullptr);

    auto* mesh = CreateSingleMesh();
    ASSERT_NE(mesh, nullptr);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    auto* material = new NLS::Render::Resources::Material();
    material->SetShader(shader);
    const_cast<std::string&>(material->path) = materialPath;
    meshManager.RegisterResource(meshPath, mesh);
    materialManager.RegisterResource(materialPath, material);

    const auto retried = renderScene.Synchronize(scene, syncOptions);

    EXPECT_EQ(retried.rebuiltCachedCommandCount, 1u);
    EXPECT_EQ(meshFilter->ResolveMesh(), mesh);
    EXPECT_EQ(meshFilter->GetMeshReference().Get(), mesh);
    EXPECT_EQ(meshRenderer->GetMaterials()[0], material);
    EXPECT_EQ(meshRenderer->GetMaterialReferences()[0].Get(), material);

    materialManager.UnloadResources();
    meshManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
}

TEST(RenderSceneCacheTests, SynchronizeDoesNotReloadPreviouslyMissingDeferredReferencesUntilRegistered)
{
    using namespace NLS::Engine::Serialize;

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    const auto meshGuid = NLS::Guid::Parse("22222222-3333-4444-8555-666666666666");
    const auto materialGuid = NLS::Guid::Parse("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");
    const auto meshPath = std::string("Library/Artifacts/Missing/body.nmesh");
    const auto materialPath = std::string("Library/Artifacts/Missing/body.nmat");
    const auto meshReference = ObjectIdentifier::Asset(
        AssetId(meshGuid),
        MakeLocalIdentifierInFile(meshGuid, "mesh:body"),
        meshPath);
    const auto materialReference = ObjectIdentifier::Asset(
        AssetId(materialGuid),
        MakeLocalIdentifierInFile(materialGuid, "material:body"),
        materialPath);

    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("Deferred Missing");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMeshReference(MakeRenderScenePPtr<NLS::Render::Resources::Mesh>(meshReference));
    meshRenderer->SetMaterialReferences({
        MakeRenderScenePPtr<NLS::Render::Resources::Material>(materialReference)
    });

    NLS::Render::Resources::Material defaultMaterial;
    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    EXPECT_EQ(renderScene.Synchronize(scene, syncOptions).rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(meshFilter->ResolveMesh(), nullptr);
    EXPECT_EQ(meshFilter->GetMeshReference().Get(), nullptr);
    EXPECT_EQ(meshRenderer->GetMaterials()[0], nullptr);
    EXPECT_EQ(meshRenderer->GetMaterialReferences()[0].Get(), nullptr);

    EXPECT_EQ(renderScene.Synchronize(scene, syncOptions).rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(meshFilter->ResolveMesh(), nullptr);
    EXPECT_EQ(meshFilter->GetMeshReference().Get(), nullptr);
    EXPECT_EQ(meshRenderer->GetMaterials()[0], nullptr);
    EXPECT_EQ(meshRenderer->GetMaterialReferences()[0].Get(), nullptr);

    auto* mesh = CreateSingleMesh();
    ASSERT_NE(mesh, nullptr);
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    auto* material = new NLS::Render::Resources::Material();
    material->SetShader(shader);
    const_cast<std::string&>(material->path) = materialPath;
    meshManager.RegisterResource(meshPath, mesh);
    materialManager.RegisterResource(materialPath, material);

    const auto retried = renderScene.Synchronize(scene, syncOptions);
    EXPECT_EQ(retried.rebuiltCachedCommandCount, 1u);
    EXPECT_EQ(meshFilter->ResolveMesh(), mesh);
    EXPECT_EQ(meshRenderer->GetMaterials()[0], material);

    materialManager.UnloadResources();
    meshManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
}

TEST(RenderSceneCacheTests, SynchronizeDrawsDirectMeshReferenceWithoutModelResource)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    auto* mesh = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 1.0f});

    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("DirectMeshActor");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMeshReference(NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Mesh>(mesh));
    meshRenderer->SetMaterialAtIndex(0u, material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &material;

    const auto stats = renderScene.Synchronize(scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({nullptr, {}});

    EXPECT_EQ(stats.rebuiltCachedCommandCount, 1u);
    EXPECT_EQ(meshFilter->ResolveMesh(), mesh);
    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.mesh, mesh);

    delete mesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RenderSceneCacheTests, SceneRendererDrawsLoadedPrimitiveCubeWithColdDefaultMaterial)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    auto& driver = EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_loaded_primitive_cube_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    WriteCubeMeshArtifact(
        root.Path() / "EngineAssets" / "Library" / "BuiltinArtifacts" / "Models" / "Cube.nmesh");
    CopyFallbackShaderAssets(root.Path() / "EngineAssets");

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedRenderSceneResourceManagers resourceManagers(
        meshManager,
        materialManager,
        shaderManager,
        projectAssetsRoot,
        engineAssetsRoot);

    NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager);
    EXPECT_TRUE(
        shaderManager.IsResourceRegistered(":Shaders\\Lambert.hlsl") ||
        shaderManager.IsResourceRegistered(":Shaders/Lambert.hlsl") ||
        shaderManager.IsResourceRegistered(":Shaders\\Standard.hlsl") ||
        shaderManager.IsResourceRegistered(":Shaders/Standard.hlsl"));

    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("Loaded Primitive Cube");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);

    const auto meshGuid = NLS::Guid::Parse("33333333-4444-4555-8666-777777777777");
    meshFilter->SetMeshReference(MakeRenderScenePPtr<NLS::Render::Resources::Mesh>(
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            NLS::Engine::Serialize::AssetId(meshGuid),
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshGuid, "mesh:Cube"),
            "builtin:Primitive/Cube")));
    meshRenderer->SetMaterialReferences({});

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);

    EXPECT_FALSE(materialManager.IsResourceRegistered(":Materials\\Default.mat"));
    ASSERT_EQ(drawables.opaques.size(), 1u);
    EXPECT_EQ(drawables.opaques.front().second.mesh, meshFilter->ResolveMesh());
    ASSERT_NE(drawables.opaques.front().second.material, nullptr);
    EXPECT_TRUE(drawables.opaques.front().second.material->IsValid());
}

TEST(RenderSceneCacheTests, SynchronizeResolvesOnlyMaterialSlotUsedByMesh)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    auto* usedMaterial = new NLS::Render::Resources::Material();
    auto* unusedMaterial = new NLS::Render::Resources::Material();
    usedMaterial->SetShader(shader);
    unusedMaterial->SetShader(shader);

    const auto usedPath = std::string("Library/Artifacts/HotPath/used.nmat");
    const auto unusedPath = std::string("Library/Artifacts/HotPath/unused-high-slot.nmat");
    const_cast<std::string&>(usedMaterial->path) = usedPath;
    const_cast<std::string&>(unusedMaterial->path) = unusedPath;
    materialManager.RegisterResource(usedPath, usedMaterial);
    materialManager.RegisterResource(unusedPath, unusedMaterial);

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("HotPathMaterialSlots");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);

    NLS::Array<std::string> materialPaths;
    materialPaths.resize(201u);
    materialPaths[0] = usedPath;
    materialPaths[200] = unusedPath;
    meshRenderer->SetMaterialPathHints(materialPaths);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = usedMaterial;

    const auto stats = renderScene.Synchronize(scene, syncOptions);

    EXPECT_EQ(stats.rebuiltCachedCommandCount, 1u);
    EXPECT_EQ(meshRenderer->GetMaterials()[0], usedMaterial);
    EXPECT_EQ(meshRenderer->GetMaterials()[200], nullptr);

    delete mesh;
    materialManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(RenderSceneCacheTests, ExplicitMaterialPathSuppressesDefaultMaterialUntilResolved)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/body.nmat");
    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("MaterialPathPending");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshRenderer->SetMaterialPathHints({ materialPath });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    const auto unresolved = renderScene.Synchronize(scene, syncOptions);
    EXPECT_EQ(unresolved.rebuiltCachedCommandCount, 0u);
    EXPECT_TRUE(renderScene.GatherVisibleCommands({}).opaques.empty())
        << "A generated model with an explicit but unresolved material path must not render with the white fallback.";

    auto* material = new NLS::Render::Resources::Material();
    material->SetShader(shader);
    const_cast<std::string&>(material->path) = materialPath;
    materialManager.RegisterResource(materialPath, material);

    const auto resolved = renderScene.Synchronize(scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({});

    EXPECT_EQ(resolved.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.material, material);

    delete mesh;
    materialManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(RenderSceneCacheTests, ExplicitMaterialPathSuppressesDrawUntilDeclaredTexturesResolved)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/textured-body.nmat");
    const auto texturePath = std::string("Library/Artifacts/Preview/textures/body-diffuse.ntex");
    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    const_cast<std::string&>(material.path) = materialPath;
    material.SetTextureResourcePath("u_DiffuseMap", texturePath);

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("MaterialTexturePending");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshRenderer->SetMaterialPathHints({ materialPath });
    meshRenderer->SetResolvedMaterialFromReference(0u, material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;
    syncOptions.requireExplicitMaterialTextures = true;

    const auto texturePending = renderScene.Synchronize(scene, syncOptions);
    EXPECT_EQ(texturePending.rebuiltCachedCommandCount, 0u);
    EXPECT_TRUE(renderScene.GatherVisibleCommands({}).opaques.empty())
        << "A generated model material with declared but unresolved textures must not render as a white/default-textured model.";

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(32u, 48u, 64u, 255u);
    ASSERT_NE(texture, nullptr);
    texture->path = texturePath;
    material.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", texture);

    const auto textureResolved = renderScene.Synchronize(scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({});

    EXPECT_EQ(textureResolved.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.material, &material);

    delete mesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RenderSceneCacheTests, ExistingSceneExplicitMaterialPathRemainsVisibleWhileDeclaredTexturesArePending)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Existing/textured-body.nmat");
    const auto texturePath = std::string("Library/Artifacts/Existing/textures/body-diffuse.ntex");
    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    const_cast<std::string&>(material.path) = materialPath;
    material.SetTextureResourcePath("u_DiffuseMap", texturePath);

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("ExistingSceneMaterialTexturePending");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshRenderer->SetMaterialPathHints({ materialPath });
    meshRenderer->SetResolvedMaterialFromReference(0u, material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    const auto texturePending = renderScene.Synchronize(scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({});

    EXPECT_EQ(texturePending.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(visible.opaques.size(), 1u)
        << "Saved or already-visible prefab instances must not disappear just because a drag preview introduced pending texture work for the same artifact.";
    EXPECT_EQ(visible.opaques.front().second.material, &material);

    delete mesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RenderSceneCacheTests, ExplicitMaterialPathBindsCachedDeclaredTexturesBeforeSuppressingDraw)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/textured-cached-body.nmat");
    const auto texturePath = std::string("Library/Artifacts/Preview/textures/cached-body-diffuse.ntex");
    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    const_cast<std::string&>(material.path) = materialPath;
    material.SetTextureResourcePath("u_DiffuseMap", texturePath);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(32u, 48u, 64u, 255u);
    ASSERT_NE(texture, nullptr);
    texture->path = texturePath;
    textureManager.RegisterResource(texturePath, texture);

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("MaterialTextureCached");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshRenderer->SetMaterialPathHints({ materialPath });
    meshRenderer->SetResolvedMaterialFromReference(0u, material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    const auto textureResolved = renderScene.Synchronize(scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({});

    EXPECT_EQ(textureResolved.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.material, &material);
    const auto* parameter = material.GetParameterBlock().TryGet("u_DiffuseMap");
    ASSERT_NE(parameter, nullptr);
    ASSERT_EQ(parameter->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter), texture);

    delete mesh;
    textureManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
}

TEST(RenderSceneCacheTests, ExplicitMaterialPathAcceptsEquivalentCachedDeclaredTexturePath)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_equivalent_texture_path_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";

    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/textured-equivalent-body.nmat");
    const auto texturePath = std::string("Library/Artifacts/Preview/textures/equivalent-body-diffuse.ntex");
    const auto absoluteTexturePath = NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);

    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    const_cast<std::string&>(material.path) = materialPath;
    material.SetTextureResourcePath("u_DiffuseMap", texturePath);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(32u, 48u, 64u, 255u);
    ASSERT_NE(texture, nullptr);
    texture->path = absoluteTexturePath;
    textureManager.RegisterResource(absoluteTexturePath, texture);

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("MaterialEquivalentTexturePath");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshRenderer->SetMaterialPathHints({ materialPath });
    meshRenderer->SetResolvedMaterialFromReference(0u, material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    const auto textureResolved = renderScene.Synchronize(scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({});

    EXPECT_EQ(textureResolved.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(visible.opaques.size(), 1u)
        << "A cached texture registered under an equivalent absolute artifact path must satisfy the material-declared Library path.";
    EXPECT_EQ(visible.opaques.front().second.material, &material);
    const auto* parameter = material.GetParameterBlock().TryGet("u_DiffuseMap");
    ASSERT_NE(parameter, nullptr);
    ASSERT_EQ(parameter->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter), texture);

    delete mesh;
    textureManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
}

TEST(RenderSceneCacheTests, ResourceManagersReturnEquivalentCachedArtifactsForAsyncRequests)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_equivalent_async_artifact_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;

    const auto materialPath = std::string("Library/Artifacts/Hero/materials/body.nmat");
    const auto texturePath = std::string("Library/Artifacts/Hero/textures/body.ntex");
    const auto absoluteMaterialPath =
        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(materialPath);
    const auto absoluteTexturePath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);

    auto* material = new NLS::Render::Resources::Material();
    material->path = absoluteMaterialPath;
    materialManager.RegisterResource(absoluteMaterialPath, material);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(12u, 34u, 56u, 255u);
    ASSERT_NE(texture, nullptr);
    texture->path = absoluteTexturePath;
    textureManager.RegisterResource(absoluteTexturePath, texture);

    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath, true), material)
        << "Dragging a prefab must reuse a material already cached under the equivalent absolute artifact path.";
    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(materialPath));
    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(absoluteMaterialPath));

    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath, true), texture)
        << "Dragging a prefab must reuse a texture already cached under the equivalent absolute artifact path.";
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(texturePath));
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(absoluteTexturePath));

    materialManager.UnloadResources();
    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
}

TEST(RenderSceneCacheTests, ResourceManagersTrackEquivalentPendingAsyncArtifactRequests)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_equivalent_pending_artifact_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;

    const auto materialPath = std::string("Library/Artifacts/Hero/materials/pending.nmat");
    const auto texturePath = std::string("Library/Artifacts/Hero/textures/pending.ntex");
    const auto absoluteMaterialPath =
        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(materialPath);
    const auto absoluteTexturePath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);

    materialManager.RequestAsyncArtifact(absoluteMaterialPath, true);
    textureManager.RequestAsyncArtifact(absoluteTexturePath, true);

    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(materialPath))
        << "Pending material artifact requests must be visible through equivalent Library/absolute path aliases.";
    EXPECT_TRUE(textureManager.IsAsyncArtifactLoadPending(texturePath))
        << "Pending texture artifact requests must be visible through equivalent Library/absolute path aliases.";

    materialManager.CancelAsyncArtifact(materialPath);
    for (size_t attempt = 0; attempt < 32u; ++attempt)
    {
        textureManager.PumpAsyncLoadsForPaths({ texturePath }, 8u);
        if (!textureManager.IsAsyncArtifactLoadPending(absoluteTexturePath))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(absoluteTexturePath))
        << "Preview-owned texture pumps must complete equivalent absolute-path requests when the preview tracks a Library path.";

    textureManager.RequestAsyncArtifact(absoluteTexturePath, true);
    textureManager.CancelAsyncArtifact(texturePath);
    for (size_t attempt = 0; attempt < 32u; ++attempt)
    {
        materialManager.PumpAsyncLoads(8u);
        textureManager.PumpAsyncLoads(8u);
        if (!materialManager.IsAsyncArtifactLoadPending(absoluteMaterialPath) &&
            !textureManager.IsAsyncArtifactLoadPending(absoluteTexturePath))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
}

TEST(RenderSceneCacheTests, MissingMaterialPathStillUsesDefaultMaterial)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("NoMaterialPath");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    const auto stats = renderScene.Synchronize(scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({});

    EXPECT_EQ(stats.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.material, &defaultMaterial);

    delete mesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RenderSceneCacheTests, BitsetVisibilityTracksPrimitiveAndMeshResults)
{
    ManyPrimitiveFixture fixture(9u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 9u);

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};

    const auto snapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto visible = renderScene.GatherVisibleCommands(visibilityOptions);

    ASSERT_EQ(snapshot.primitiveBits.size(), 1u);
    ASSERT_EQ(snapshot.meshBits.size(), 1u);
    EXPECT_EQ(snapshot.primitiveCount, 9u);
    EXPECT_EQ(snapshot.visiblePrimitiveCount, 6u);
    EXPECT_EQ(snapshot.visibleMeshCount, 6u);
    EXPECT_EQ(visible.opaques.size(), snapshot.visibleMeshCount);
    EXPECT_EQ(snapshot.primitiveBits[0] & (1ull << 0u), 0u);
    EXPECT_NE(snapshot.primitiveBits[0] & (1ull << 1u), 0u);
    EXPECT_NE(snapshot.meshBits[0] & (1ull << 1u), 0u);
}

TEST(RenderSceneCacheTests, SerialAndParallelVisibilityProduceEquivalentQueues)
{
    ScopedRenderSceneCacheJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    ManyPrimitiveFixture fixture(192u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 192u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = false;
    settings.parallelVisibilityPrimitivesPerTask = 32u;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto serialSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto parallelSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Parallel);
    auto serialQueues = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    auto parallelQueues = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Parallel);

    EXPECT_FALSE(serialSnapshot.usedParallelEvaluation);
    EXPECT_TRUE(parallelSnapshot.usedParallelEvaluation);
    EXPECT_EQ(serialSnapshot.primitiveBits, parallelSnapshot.primitiveBits);
    EXPECT_EQ(serialSnapshot.meshBits, parallelSnapshot.meshBits);
    EXPECT_EQ(serialSnapshot.visiblePrimitiveCount, parallelSnapshot.visiblePrimitiveCount);
    EXPECT_EQ(serialSnapshot.visibleMeshCount, parallelSnapshot.visibleMeshCount);
    EXPECT_EQ(serialQueues.opaques.size(), parallelQueues.opaques.size());
    EXPECT_EQ(ExtractMeshes(serialQueues), ExtractMeshes(parallelQueues));
}

TEST(RenderSceneCacheTests, VisibleLayerMaskAppliesConsistentlyAcrossVisibilityModes)
{
    ScopedRenderSceneCacheJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    ManyPrimitiveFixture fixture(192u);
    ASSERT_GE(fixture.scene.GetGameObjects().size(), 3u);
    for (size_t index = 0u; index < fixture.scene.GetGameObjects().size(); ++index)
        fixture.scene.GetGameObjects()[index]->SetLayer(index % 3u == 1u ? 1 : 0);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 192u);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = false;
    settings.parallelVisibilityPrimitiveThreshold = 128u;
    settings.parallelVisibilityPrimitivesPerTask = 32u;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.visibleLayerMask = 1u << 1u;
    visibilityOptions.largeSceneSettings = &settings;

    const auto serialSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto parallelSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Parallel);
    const auto autoSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Auto);

    EXPECT_FALSE(serialSnapshot.usedParallelEvaluation);
    EXPECT_TRUE(parallelSnapshot.usedParallelEvaluation);
    EXPECT_TRUE(autoSnapshot.usedParallelEvaluation);
    EXPECT_EQ(serialSnapshot.primitiveBits, parallelSnapshot.primitiveBits);
    EXPECT_EQ(serialSnapshot.meshBits, parallelSnapshot.meshBits);
    EXPECT_EQ(serialSnapshot.primitiveBits, autoSnapshot.primitiveBits);
    EXPECT_EQ(serialSnapshot.meshBits, autoSnapshot.meshBits);
    EXPECT_EQ(serialSnapshot.visiblePrimitiveCount, 64u);
    EXPECT_EQ(serialSnapshot.visiblePrimitiveCount, parallelSnapshot.visiblePrimitiveCount);
    EXPECT_EQ(serialSnapshot.visibleMeshCount, parallelSnapshot.visibleMeshCount);
    EXPECT_EQ(serialSnapshot.visiblePrimitiveCount, autoSnapshot.visiblePrimitiveCount);
    EXPECT_EQ(serialSnapshot.visibleMeshCount, autoSnapshot.visibleMeshCount);

    auto serialQueues = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    auto parallelQueues = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Parallel);
    auto autoQueues = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Auto);

    EXPECT_EQ(ExtractMeshes(serialQueues), ExtractMeshes(parallelQueues));
    EXPECT_EQ(ExtractMeshes(serialQueues), ExtractMeshes(autoQueues));
}

TEST(RenderSceneCacheTests, LargeSceneSettingsExposeNamedDefaultsForVisibilityThresholds)
{
    const auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();

    EXPECT_TRUE(settings.enableSpatialIndex);
    EXPECT_TRUE(settings.enableParallelVisibility);
    EXPECT_TRUE(settings.enableLOD);
    EXPECT_TRUE(settings.enableHLOD);
    EXPECT_FALSE(settings.enableHZBOcclusion);
    EXPECT_EQ(settings.parallelVisibilityPrimitiveThreshold, 1024u);
    EXPECT_EQ(settings.parallelVisibilityPrimitivesPerTask, 128u);
    EXPECT_EQ(settings.staticRebuildBudgetUs, 0u);
    EXPECT_STREQ(
        NLS::Engine::Rendering::LargeSceneSettings::DebugLabel(
            NLS::Engine::Rendering::LargeSceneSettingId::ParallelVisibilityPrimitiveThreshold),
        "parallelVisibilityPrimitiveThreshold");
    EXPECT_STREQ(
        NLS::Engine::Rendering::LargeSceneSettings::DebugLabel(
            NLS::Engine::Rendering::LargeSceneSettingId::ParallelVisibilityPrimitivesPerTask),
        "parallelVisibilityPrimitivesPerTask");
    EXPECT_STREQ(
        NLS::Engine::Rendering::LargeSceneSettings::DebugLabel(
            NLS::Engine::Rendering::LargeSceneSettingId::StaticRebuildBudgetUs),
        "staticRebuildBudgetUs");
}

TEST(RenderSceneCacheTests, AutoVisibilityUsesLargeSceneSettingsThresholds)
{
    ScopedRenderSceneCacheJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    ManyPrimitiveFixture fixture(192u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 192u);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = false;
    settings.parallelVisibilityPrimitiveThreshold = 128u;
    settings.parallelVisibilityPrimitivesPerTask = 32u;

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto autoSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Auto);
    const auto serialSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    EXPECT_TRUE(autoSnapshot.usedParallelEvaluation);
    EXPECT_EQ(autoSnapshot.primitiveBits, serialSnapshot.primitiveBits);
    EXPECT_EQ(autoSnapshot.meshBits, serialSnapshot.meshBits);
    EXPECT_EQ(autoSnapshot.visiblePrimitiveCount, serialSnapshot.visiblePrimitiveCount);
    EXPECT_EQ(autoSnapshot.visibleMeshCount, serialSnapshot.visibleMeshCount);
}

TEST(RenderSceneCacheTests, AutoVisibilitySettingsCanDisableParallelEvaluation)
{
    ScopedRenderSceneCacheJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    ManyPrimitiveFixture fixture(192u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 192u);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = false;
    settings.enableParallelVisibility = false;
    settings.parallelVisibilityPrimitiveThreshold = 128u;
    settings.parallelVisibilityPrimitivesPerTask = 32u;

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto autoSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Auto);
    const auto serialSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    EXPECT_FALSE(autoSnapshot.usedParallelEvaluation);
    EXPECT_EQ(autoSnapshot.primitiveBits, serialSnapshot.primitiveBits);
    EXPECT_EQ(autoSnapshot.meshBits, serialSnapshot.meshBits);
    EXPECT_EQ(autoSnapshot.visiblePrimitiveCount, serialSnapshot.visiblePrimitiveCount);
    EXPECT_EQ(autoSnapshot.visibleMeshCount, serialSnapshot.visibleMeshCount);
}

TEST(RenderSceneCacheTests, AutoVisibilityStaysSerialUntilPersistentJobSystemExists)
{
    ManyPrimitiveFixture fixture(192u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 192u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = false;
    settings.parallelVisibilityPrimitiveThreshold = 128u;
    settings.parallelVisibilityPrimitivesPerTask = 32u;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto autoSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Auto);
    const auto serialSnapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    EXPECT_FALSE(autoSnapshot.usedParallelEvaluation);
    EXPECT_EQ(autoSnapshot.primitiveBits, serialSnapshot.primitiveBits);
    EXPECT_EQ(autoSnapshot.meshBits, serialSnapshot.meshBits);
    EXPECT_EQ(autoSnapshot.visiblePrimitiveCount, serialSnapshot.visiblePrimitiveCount);
    EXPECT_EQ(autoSnapshot.visibleMeshCount, serialSnapshot.visibleMeshCount);
}

TEST(RenderSceneCacheTests, MeshBitsetCanCullIndividualImportedPrefabNodes)
{
    RenderableFixture fixture;
    auto* farMesh = CreateFarMesh();

    auto& farObject = fixture.scene.CreateGameObject("FarImportedNode");
    auto* farMeshFilter = farObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* farMeshRenderer = farObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(farMeshFilter, nullptr);
    ASSERT_NE(farMeshRenderer, nullptr);
    farMeshFilter->SetMesh(farMesh);
    farMeshRenderer->FillWithMaterial(fixture.material);

    fixture.meshRenderer->SetFrustumBehaviour(
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MESHES);
    farMeshRenderer->SetFrustumBehaviour(
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MESHES);
    fixture.meshRenderer->FillWithMaterial(fixture.material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};

    const auto snapshot = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(snapshot.primitiveBits.size(), 1u);
    ASSERT_EQ(snapshot.meshBits.size(), 1u);
    EXPECT_EQ(snapshot.visiblePrimitiveCount, 1u);
    EXPECT_EQ(snapshot.visibleMeshCount, 1u);
    EXPECT_NE(snapshot.primitiveBits[0] & 1ull, 0u);
    EXPECT_EQ(snapshot.primitiveBits[0] & (1ull << 1u), 0u);
    EXPECT_NE(snapshot.meshBits[0] & 1ull, 0u);
    EXPECT_EQ(snapshot.meshBits[0] & (1ull << 1u), 0u);
    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.mesh, fixture.mesh);

    delete farMesh;
}

TEST(RenderSceneCacheTests, OpaqueQueueGroupsCompatibleStateAndTransparentKeepsBackToFront)
{
    QueueSortFixture fixture;
    fixture.AddObject("OpaqueNearA", *fixture.sharedMesh, fixture.opaqueMaterialA, 5.0f);
    fixture.AddObject("OpaqueMiddleB", *fixture.otherMesh, fixture.opaqueMaterialB, 10.0f);
    fixture.AddObject("OpaqueFarA", *fixture.sharedMesh, fixture.opaqueMaterialA, 20.0f);
    fixture.AddObject("TransparentNear", *fixture.sharedMesh, fixture.transparentMaterial, 3.0f);
    fixture.AddObject("TransparentFar", *fixture.sharedMesh, fixture.transparentMaterial, 30.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 5u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });

    ASSERT_EQ(visible.opaques.size(), 2u);
    const auto firstInstances = ResolveVisibleInstanceCount(visible.opaques[0].second);
    const auto secondInstances = ResolveVisibleInstanceCount(visible.opaques[1].second);
    EXPECT_TRUE(
        (visible.opaques[0].second.mesh == fixture.sharedMesh && firstInstances == 2u) ||
        (visible.opaques[1].second.mesh == fixture.sharedMesh && secondInstances == 2u));

    ASSERT_EQ(visible.transparents.size(), 2u);
    EXPECT_GT(visible.transparents[0].first, visible.transparents[1].first);
}

TEST(RenderSceneCacheTests, SpatialVisibilityPipelineKeepsQueueFinalizationOwnedByRenderScene)
{
    QueueSortFixture fixture;
    fixture.AddObjectAt("OpaqueNearA", *fixture.sharedMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -5.0f });
    fixture.AddObjectAt("OpaqueMiddleB", *fixture.otherMesh, fixture.opaqueMaterialB, { 0.0f, 0.0f, -10.0f });
    fixture.AddObjectAt("OpaqueFarA", *fixture.sharedMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -20.0f });
    fixture.AddObjectAt("TransparentNear", *fixture.sharedMesh, fixture.transparentMaterial, { 0.0f, 0.0f, -3.0f });
    fixture.AddObjectAt("TransparentFar", *fixture.sharedMesh, fixture.transparentMaterial, { 0.0f, 0.0f, -30.0f });
    fixture.AddObjectAt("OutsideOpaque", *fixture.sharedMesh, fixture.opaqueMaterialA, { 250.0f, 0.0f, -6.0f });
    fixture.AddObjectAt("OutsideTransparent", *fixture.sharedMesh, fixture.transparentMaterial, { 250.0f, 0.0f, -6.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 7u);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    ASSERT_EQ(visible.opaques.size(), 2u);
    const auto firstInstances = ResolveVisibleInstanceCount(visible.opaques[0].second);
    const auto secondInstances = ResolveVisibleInstanceCount(visible.opaques[1].second);
    EXPECT_TRUE(
        (visible.opaques[0].second.mesh == fixture.sharedMesh && firstInstances == 2u) ||
        (visible.opaques[1].second.mesh == fixture.sharedMesh && secondInstances == 2u));

    ASSERT_EQ(visible.transparents.size(), 2u);
    EXPECT_GT(visible.transparents[0].first, visible.transparents[1].first);
    EXPECT_EQ(telemetry.commandOffsetRebuildCount, 1u);
    EXPECT_EQ(telemetry.finalizationTouchedPrimitiveCount, telemetry.visiblePrimitiveCount);
    EXPECT_EQ(telemetry.finalizationTouchedCommandCount, 5u);
    EXPECT_EQ(telemetry.rawVisibleDrawCount, 5u);
    EXPECT_EQ(telemetry.submittedDrawCount, 4u);
    EXPECT_EQ(telemetry.dynamicInstanceGroupCount, 1u);
    EXPECT_LT(telemetry.finalizationTouchedPrimitiveCount, telemetry.registeredPrimitiveCount);

    const auto secondVisible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto& secondTelemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    EXPECT_EQ(ExtractMeshes(secondVisible), ExtractMeshes(visible));
    EXPECT_EQ(secondTelemetry.commandOffsetRebuildCount, 0u);
    EXPECT_GE(secondTelemetry.primitiveRecordsTouched, secondTelemetry.spatialCandidateCount);
    EXPECT_GE(secondTelemetry.primitiveRecordsTouched, secondTelemetry.visibilityTestedPrimitiveCount);
    EXPECT_EQ(secondTelemetry.finalizationTouchedPrimitiveCount, secondTelemetry.visiblePrimitiveCount);
    EXPECT_LT(secondTelemetry.finalizationTouchedPrimitiveCount, secondTelemetry.registeredPrimitiveCount);
}

TEST(RenderSceneCacheTests, SpatialVisibilityTelemetryDoesNotUnderReportPipelineTouches)
{
    const auto source = ReadRepoTextFile("Runtime/Engine/Rendering/RenderScene.cpp");
    ASSERT_FALSE(source.empty());

    const auto spatialPath = source.find("RenderScene::EvaluateVisibilitySpatial(");
    ASSERT_NE(spatialPath, std::string::npos);
    const auto nextFunction = source.find("RenderScene::EvaluateVisibilityParallel(", spatialPath);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto spatialBody = source.substr(spatialPath, nextFunction - spatialPath);

    EXPECT_NE(
        spatialBody.find("candidates.primitiveRecordsTouched + pipelineResult.primitiveRecordsTouched"),
        std::string::npos);
    EXPECT_EQ(
        spatialBody.find("std::max(candidates.primitiveRecordsTouched, pipelineResult.primitiveRecordsTouched)"),
        std::string::npos);
    EXPECT_EQ(
        spatialBody.find("snapshot.primitiveRecordsTouched = candidates.primitiveRecordsTouched;"),
        std::string::npos);
}

TEST(RenderSceneCacheTests, GatherVisibleCommandsAppliesRegisteredLODGroups)
{
    QueueSortFixture fixture;
    auto* highMesh = CreateSingleMesh(0u);
    auto* lowMesh = CreateSingleMesh(0u);
    ASSERT_NE(highMesh, nullptr);
    ASSERT_NE(lowMesh, nullptr);
    fixture.AddObjectAt("LODHigh", *highMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -100.0f });
    fixture.AddObjectAt("LODLow", *lowMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -100.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting();
    const auto highHandle = FindPrimitiveHandleByMesh(snapshot, *highMesh);
    const auto lowHandle = FindPrimitiveHandleByMesh(snapshot, *lowMesh);
    ASSERT_TRUE(highHandle.IsValid());
    ASSERT_TRUE(lowHandle.IsValid());

    NLS::Engine::Rendering::LODGroupRecord lodGroup;
    lodGroup.groupHandle = { 0u };
    lodGroup.worldReferencePoint = { 0.0f, 0.0f, -100.0f };
    lodGroup.worldSize = 20.0f;
    lodGroup.levels = {
        NLS::Engine::Rendering::LODLevelRecord { 0.50f, { highHandle } },
        NLS::Engine::Rendering::LODLevelRecord { 0.00f, { lowHandle } }
    };
    renderScene.RegisterLODGroup(lodGroup);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableLOD = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    const auto meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes.front(), lowMesh);

    delete highMesh;
    delete lowMesh;
}

TEST(RenderSceneCacheTests, SpatialVisibilityWithoutFrustumStillAppliesRegisteredLODGroups)
{
    QueueSortFixture fixture;
    auto* highMesh = CreateSingleMesh(0u);
    auto* lowMesh = CreateSingleMesh(0u);
    ASSERT_NE(highMesh, nullptr);
    ASSERT_NE(lowMesh, nullptr);
    fixture.AddObjectAt("SpatialLODHigh", *highMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -100.0f });
    fixture.AddObjectAt("SpatialLODLow", *lowMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -100.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting();
    const auto highHandle = FindPrimitiveHandleByMesh(snapshot, *highMesh);
    const auto lowHandle = FindPrimitiveHandleByMesh(snapshot, *lowMesh);
    ASSERT_TRUE(highHandle.IsValid());
    ASSERT_TRUE(lowHandle.IsValid());

    NLS::Engine::Rendering::LODGroupRecord lodGroup;
    lodGroup.groupHandle = { 0u };
    lodGroup.worldReferencePoint = { 0.0f, 0.0f, -100.0f };
    lodGroup.worldSize = 20.0f;
    lodGroup.levels = {
        NLS::Engine::Rendering::LODLevelRecord { 0.50f, { highHandle } },
        NLS::Engine::Rendering::LODLevelRecord { 0.00f, { lowHandle } }
    };
    renderScene.RegisterLODGroup(lodGroup);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;
    settings.enableLOD = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    const auto meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes.front(), lowMesh);

    delete highMesh;
    delete lowMesh;
}

TEST(RenderSceneCacheTests, RegisteredLODHistoryIsIsolatedPerViewKey)
{
    QueueSortFixture fixture;
    auto* highMesh = CreateSingleMesh(0u);
    auto* lowMesh = CreateSingleMesh(0u);
    ASSERT_NE(highMesh, nullptr);
    ASSERT_NE(lowMesh, nullptr);
    fixture.AddObjectAt("ViewLocalLODHigh", *highMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -100.0f });
    fixture.AddObjectAt("ViewLocalLODLow", *lowMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -100.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting();
    const auto highHandle = FindPrimitiveHandleByMesh(snapshot, *highMesh);
    const auto lowHandle = FindPrimitiveHandleByMesh(snapshot, *lowMesh);
    ASSERT_TRUE(highHandle.IsValid());
    ASSERT_TRUE(lowHandle.IsValid());

    NLS::Engine::Rendering::LODGroupRecord lodGroup;
    lodGroup.groupHandle = { 0u };
    lodGroup.worldReferencePoint = { 0.0f, 0.0f, -100.0f };
    lodGroup.worldSize = 51.0f;
    lodGroup.hysteresis = 0.05f;
    lodGroup.levels = {
        NLS::Engine::Rendering::LODLevelRecord { 0.50f, { highHandle } },
        NLS::Engine::Rendering::LODLevelRecord { 0.00f, { lowHandle } }
    };
    renderScene.RegisterLODGroup(lodGroup);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableLOD = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions viewA;
    viewA.cameraPosition = {};
    viewA.largeSceneSettings = &settings;
    viewA.lodHistoryViewKey = 101u;
    auto visible = renderScene.GatherVisibleCommands(
        viewA,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    auto meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes.front(), highMesh);

    NLS::Engine::Rendering::RenderSceneVisibilityOptions viewB = viewA;
    viewB.lodHistoryViewKey = 202u;
    viewB.lodBias = 0.96f;
    visible = renderScene.GatherVisibleCommands(
        viewB,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes.front(), lowMesh);

    viewA.lodBias = 0.96f;
    visible = renderScene.GatherVisibleCommands(
        viewA,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes.front(), highMesh);

    delete highMesh;
    delete lowMesh;
}

TEST(RenderSceneCacheTests, GatherVisibleCommandsAppliesRegisteredHLODClusters)
{
    QueueSortFixture fixture;
    auto* childMeshA = CreateSingleMesh(0u);
    auto* childMeshB = CreateSingleMesh(0u);
    auto* proxyMesh = CreateSingleMesh(0u);
    ASSERT_NE(childMeshA, nullptr);
    ASSERT_NE(childMeshB, nullptr);
    ASSERT_NE(proxyMesh, nullptr);
    fixture.AddObjectAt("HLODChildA", *childMeshA, fixture.opaqueMaterialA, { -1.0f, 0.0f, -200.0f });
    fixture.AddObjectAt("HLODChildB", *childMeshB, fixture.opaqueMaterialA, { 1.0f, 0.0f, -200.0f });
    fixture.AddObjectAt("HLODProxy", *proxyMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -200.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting();
    const auto childHandleA = FindPrimitiveHandleByMesh(snapshot, *childMeshA);
    const auto childHandleB = FindPrimitiveHandleByMesh(snapshot, *childMeshB);
    const auto proxyHandle = FindPrimitiveHandleByMesh(snapshot, *proxyMesh);
    ASSERT_TRUE(childHandleA.IsValid());
    ASSERT_TRUE(childHandleB.IsValid());
    ASSERT_TRUE(proxyHandle.IsValid());

    NLS::Engine::Rendering::HLODClusterRecord cluster;
    cluster.clusterHandle = { 0u };
    cluster.childPrimitives = { childHandleA, childHandleB };
    cluster.proxyPrimitive = proxyHandle;
    cluster.worldReferencePoint = { 0.0f, 0.0f, -200.0f };
    cluster.worldSize = 40.0f;
    cluster.activationScreenRelativeSize = 0.50f;
    cluster.compatibilityFlags =
        NLS::Engine::Rendering::HLODCompatibilityFlags::OpaqueOnly |
        NLS::Engine::Rendering::HLODCompatibilityFlags::ProxySafe;
    renderScene.RegisterHLODCluster(cluster);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableHLOD = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    const auto meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes.front(), proxyMesh);

    delete childMeshA;
    delete childMeshB;
    delete proxyMesh;
}

TEST(RenderSceneCacheTests, GatherVisibleCommandsKeepsSelectedHLODChildInspectable)
{
    QueueSortFixture fixture;
    auto* childMeshA = CreateSingleMesh(0u);
    auto* childMeshB = CreateSingleMesh(0u);
    auto* proxyMesh = CreateSingleMesh(0u);
    ASSERT_NE(childMeshA, nullptr);
    ASSERT_NE(childMeshB, nullptr);
    ASSERT_NE(proxyMesh, nullptr);
    fixture.AddObjectAt("SelectedHLODChildA", *childMeshA, fixture.opaqueMaterialA, { -1.0f, 0.0f, -200.0f });
    fixture.AddObjectAt("SelectedHLODChildB", *childMeshB, fixture.opaqueMaterialA, { 1.0f, 0.0f, -200.0f });
    fixture.AddObjectAt("SelectedHLODProxy", *proxyMesh, fixture.opaqueMaterialA, { 0.0f, 0.0f, -200.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting();
    const auto childHandleA = FindPrimitiveHandleByMesh(snapshot, *childMeshA);
    const auto childHandleB = FindPrimitiveHandleByMesh(snapshot, *childMeshB);
    const auto proxyHandle = FindPrimitiveHandleByMesh(snapshot, *proxyMesh);
    ASSERT_TRUE(childHandleA.IsValid());
    ASSERT_TRUE(childHandleB.IsValid());
    ASSERT_TRUE(proxyHandle.IsValid());

    NLS::Engine::Rendering::HLODClusterRecord cluster;
    cluster.clusterHandle = { 0u };
    cluster.childPrimitives = { childHandleA, childHandleB };
    cluster.proxyPrimitive = proxyHandle;
    cluster.worldReferencePoint = { 0.0f, 0.0f, -200.0f };
    cluster.worldSize = 40.0f;
    cluster.activationScreenRelativeSize = 0.50f;
    cluster.compatibilityFlags =
        NLS::Engine::Rendering::HLODCompatibilityFlags::OpaqueOnly |
        NLS::Engine::Rendering::HLODCompatibilityFlags::ProxySafe;
    renderScene.RegisterHLODCluster(cluster);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableHLOD = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;
    visibilityOptions.editorInspectionView = true;
    visibilityOptions.selectedPrimitiveHandles = { childHandleA };

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    const auto meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), childMeshA), meshes.end());
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), proxyMesh), meshes.end());
    EXPECT_EQ(std::find(meshes.begin(), meshes.end(), childMeshB), meshes.end());

    delete childMeshA;
    delete childMeshB;
    delete proxyMesh;
}

TEST(RenderSceneCacheTests, HLODMissingProxyInterestPropagatesFromRenderSceneVisibility)
{
    QueueSortFixture fixture;
    auto* childMeshA = CreateSingleMesh(0u);
    auto* childMeshB = CreateSingleMesh(0u);
    ASSERT_NE(childMeshA, nullptr);
    ASSERT_NE(childMeshB, nullptr);
    fixture.AddObjectAt("MissingProxyChildA", *childMeshA, fixture.opaqueMaterialA, { -1.0f, 0.0f, -200.0f });
    fixture.AddObjectAt("MissingProxyChildB", *childMeshB, fixture.opaqueMaterialA, { 1.0f, 0.0f, -200.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting();
    const auto childHandleA = FindPrimitiveHandleByMesh(snapshot, *childMeshA);
    const auto childHandleB = FindPrimitiveHandleByMesh(snapshot, *childMeshB);
    ASSERT_TRUE(childHandleA.IsValid());
    ASSERT_TRUE(childHandleB.IsValid());

    const NLS::Engine::Rendering::ScenePrimitiveHandle missingProxyHandle {
        childHandleA.sceneId,
        childHandleB.index + 100u,
        1u
    };

    NLS::Engine::Rendering::HLODClusterRecord cluster;
    cluster.clusterHandle = { 0u };
    cluster.childPrimitives = { childHandleA, childHandleB };
    cluster.proxyPrimitive = missingProxyHandle;
    cluster.worldReferencePoint = { 0.0f, 0.0f, -200.0f };
    cluster.worldSize = 80.0f;
    cluster.activationScreenRelativeSize = 0.50f;
    cluster.compatibilityFlags =
        NLS::Engine::Rendering::HLODCompatibilityFlags::OpaqueOnly |
        NLS::Engine::Rendering::HLODCompatibilityFlags::ProxySafe;
    renderScene.RegisterHLODCluster(cluster);

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableHLOD = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto visibility = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    EXPECT_EQ(visibility.visiblePrimitiveCount, 2u);
    ASSERT_EQ(visibility.representationStreamingInterest.size(), 1u);
    EXPECT_EQ(visibility.representationStreamingInterest.front(), missingProxyHandle);

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), childMeshA), meshes.end());
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), childMeshB), meshes.end());

    delete childMeshA;
    delete childMeshB;
}

TEST(RenderSceneCacheTests, GatherVisibleCommandsConsumesOcclusionHistoryBeforeSubmission)
{
    QueueSortFixture fixture;
    auto* visibleMesh = CreateSingleMesh(0u);
    auto* hiddenMesh = CreateSingleMesh(0u);
    ASSERT_NE(visibleMesh, nullptr);
    ASSERT_NE(hiddenMesh, nullptr);
    fixture.AddObjectAt("VisibleOcclusionPrimitive", *visibleMesh, fixture.opaqueMaterialA, { -1.0f, 0.0f, -20.0f });
    fixture.AddObjectAt("HiddenOcclusionPrimitive", *hiddenMesh, fixture.opaqueMaterialA, { 1.0f, 0.0f, -20.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting(42u);
    const auto visibleHandle = FindPrimitiveHandleByMesh(snapshot, *visibleMesh);
    const auto hiddenHandle = FindPrimitiveHandleByMesh(snapshot, *hiddenMesh);
    ASSERT_TRUE(visibleHandle.IsValid());
    ASSERT_TRUE(hiddenHandle.IsValid());

    NLS::Engine::Rendering::SceneOcclusionFrameInput frameInput;
    frameInput.enabled = true;
    frameInput.backendSupported = true;
    frameInput.historyTextureValid = true;
    frameInput.frameSerial = snapshot.frameSerial;
    frameInput.maxHistoryAge = 2u;
    frameInput.viewKey = 77u;
    frameInput.viewCompatibilityHash = 88u;
    frameInput.projectionHash = 99u;
    frameInput.jitterHash = 0u;
    frameInput.depthFormatKey = 1u;
    frameInput.viewportWidth = 1280u;
    frameInput.viewportHeight = 720u;

    std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitiveInput> primitiveInputs;
    primitiveInputs.reserve(snapshot.primitiveRecords.size());
    for (const auto& record : snapshot.primitiveRecords)
    {
        primitiveInputs.push_back({
            record.handle,
            record.handle.generation,
            record.handle.generation,
            0u,
            record.hasValidMaterial ? 1u : 0u,
            record.hasValidMaterial
        });
    }

    const auto hiddenInput = std::find_if(
        primitiveInputs.begin(),
        primitiveInputs.end(),
        [hiddenHandle](const auto& input)
        {
            return input.handle == hiddenHandle;
        });
    ASSERT_NE(hiddenInput, primitiveInputs.end());

    NLS::Engine::Rendering::SceneOcclusionHistory history;
    history.RecordOccluded(
        NLS::Engine::Rendering::SceneOcclusionSystem::BuildHistoryKey(frameInput, *hiddenInput),
        frameInput.frameSerial - 1u);

    NLS::Engine::Rendering::SceneOcclusionState occlusion;
    occlusion.frameInput = frameInput;
    occlusion.history = &history;
    occlusion.primitiveInputs = &primitiveInputs;

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableHZBOcclusion = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;
    visibilityOptions.occlusion = &occlusion;

    const auto visibility = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    EXPECT_EQ(visibility.visiblePrimitiveCount, 1u);
    const auto occludedIndex = static_cast<size_t>(NLS::Engine::Rendering::CullReason::Occluded);
    ASSERT_LT(occludedIndex, visibility.culledByReason.size());
    EXPECT_EQ(visibility.culledByReason[occludedIndex], 1u);

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto meshes = ExtractMeshes(visible);
    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes.front(), visibleMesh);
    EXPECT_EQ(renderScene.GetLastLargeSceneTelemetryForTesting().occlusionCulledCount, 1u);

    delete visibleMesh;
    delete hiddenMesh;
}

TEST(RenderSceneCacheTests, VisibilitySnapshotCarriesVisiblePrimitiveHandlesForHZBPacketSources)
{
    QueueSortFixture fixture;
    auto* visibleMesh = CreateSingleMesh(0u);
    auto* hiddenMesh = CreateSingleMesh(0u);
    ASSERT_NE(visibleMesh, nullptr);
    ASSERT_NE(hiddenMesh, nullptr);
    fixture.AddObjectAt("VisibleHZBSourcePrimitive", *visibleMesh, fixture.opaqueMaterialA, { -1.0f, 0.0f, -20.0f });
    fixture.AddObjectAt("HiddenHZBSourcePrimitive", *hiddenMesh, fixture.opaqueMaterialA, { 1.0f, 0.0f, -20.0f });

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting(42u);
    const auto visibleHandle = FindPrimitiveHandleByMesh(snapshot, *visibleMesh);
    const auto hiddenHandle = FindPrimitiveHandleByMesh(snapshot, *hiddenMesh);
    ASSERT_TRUE(visibleHandle.IsValid());
    ASSERT_TRUE(hiddenHandle.IsValid());

    NLS::Engine::Rendering::SceneOcclusionFrameInput frameInput;
    frameInput.enabled = true;
    frameInput.backendSupported = true;
    frameInput.historyTextureValid = true;
    frameInput.frameSerial = snapshot.frameSerial;
    frameInput.maxHistoryAge = 2u;
    frameInput.viewKey = 77u;
    frameInput.viewCompatibilityHash = 88u;
    frameInput.projectionHash = 99u;
    frameInput.depthFormatKey = 1u;
    frameInput.viewportWidth = 1280u;
    frameInput.viewportHeight = 720u;

    std::vector<NLS::Engine::Rendering::SceneOcclusionPrimitiveInput> primitiveInputs;
    for (const auto& record : snapshot.primitiveRecords)
    {
        primitiveInputs.push_back({
            record.handle,
            record.handle.generation,
            record.handle.generation,
            0u,
            record.hasValidMaterial ? 1u : 0u,
            record.hasValidMaterial
        });
    }

    const auto hiddenInput = std::find_if(
        primitiveInputs.begin(),
        primitiveInputs.end(),
        [hiddenHandle](const auto& input)
        {
            return input.handle == hiddenHandle;
        });
    ASSERT_NE(hiddenInput, primitiveInputs.end());

    NLS::Engine::Rendering::SceneOcclusionHistory history;
    history.RecordOccluded(
        NLS::Engine::Rendering::SceneOcclusionSystem::BuildHistoryKey(frameInput, *hiddenInput),
        frameInput.frameSerial - 1u);

    NLS::Engine::Rendering::SceneOcclusionState occlusion;
    occlusion.frameInput = frameInput;
    occlusion.history = &history;
    occlusion.primitiveInputs = &primitiveInputs;

    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableHZBOcclusion = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;
    visibilityOptions.occlusion = &occlusion;

    const auto visibility = renderScene.EvaluateVisibilityForTesting(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(visibility.visiblePrimitiveHandles.size(), 1u);
    EXPECT_EQ(visibility.visiblePrimitiveHandles.front(), visibleHandle);
    EXPECT_EQ(
        std::find(visibility.visiblePrimitiveHandles.begin(), visibility.visiblePrimitiveHandles.end(), hiddenHandle),
        visibility.visiblePrimitiveHandles.end());

    const auto visibleQueues = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    EXPECT_EQ(ExtractMeshes(visibleQueues).size(), 1u);
    ASSERT_EQ(renderScene.GetLastVisiblePrimitiveHandles().size(), 1u);
    EXPECT_EQ(renderScene.GetLastVisiblePrimitiveHandles().front(), visibleHandle);

    delete visibleMesh;
    delete hiddenMesh;
}

TEST(RenderSceneCacheTests, SceneRendererKeepsTransparentBackToFrontAcrossAdditiveScenes)
{
    auto& driver = EnsureRenderSceneTestDriver();
    QueueSortFixture fixture;
    fixture.AddObject("MainTransparentNear", *fixture.sharedMesh, fixture.transparentMaterial, 3.0f);

    NLS::Engine::SceneSystem::Scene previewScene;
    auto& previewObject = previewScene.CreateGameObject("PreviewTransparentFar");
    auto* previewMeshFilter = previewObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* previewMeshRenderer = previewObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(previewMeshFilter, nullptr);
    ASSERT_NE(previewMeshRenderer, nullptr);
    previewMeshFilter->SetMesh(fixture.otherMesh);
    previewMeshRenderer->FillWithMaterial(fixture.transparentMaterial);
    previewObject.GetTransform()->SetWorldPosition({ 30.0f, 0.0f, 0.0f });

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        fixture.scene,
        std::nullopt,
        nullptr,
        { &previewScene }
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);

    ASSERT_EQ(drawables.transparents.size(), 2u);
    EXPECT_EQ(drawables.transparents[0].second.mesh, fixture.otherMesh);
    EXPECT_EQ(drawables.transparents[1].second.mesh, fixture.sharedMesh);
    EXPECT_GT(drawables.transparents[0].first, drawables.transparents[1].first);
}

TEST(RenderSceneCacheTests, SceneRendererDrawsExistingAndPreviewPrefabInstancesSharingAssetReferences)
{
    using namespace NLS::Engine::Serialize;

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    auto& driver = EnsureRenderSceneTestDriver();

    const auto meshGuid = NLS::Guid::Parse("24242424-2424-4424-8424-242424242424");
    const auto materialGuid = NLS::Guid::Parse("35353535-3535-4535-8535-353535353535");
    const auto meshPath = std::string("Library/Artifacts/Hero/shared/body.nmesh");
    const auto materialPath = std::string("Library/Artifacts/Hero/shared/body.nmat");
    const auto meshReference = ObjectIdentifier::Asset(
        AssetId(meshGuid),
        MakeLocalIdentifierInFile(meshGuid, "mesh:body"),
        meshPath);
    const auto materialReference = ObjectIdentifier::Asset(
        AssetId(materialGuid),
        MakeLocalIdentifierInFile(materialGuid, "material:body"),
        materialPath);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    auto* existingMesh = CreateSingleMesh(0u);
    ASSERT_NE(existingMesh, nullptr);
    NLS::Render::Resources::Material existingMaterial;
    existingMaterial.path = materialPath;
    existingMaterial.SetShader(shader);
    ASSERT_TRUE(existingMaterial.IsValid());

    NLS::Engine::SceneSystem::Scene mainScene;
    auto& existingObject = mainScene.CreateGameObject("ExistingPrefab");
    auto* existingMeshFilter = existingObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* existingMeshRenderer = existingObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(existingMeshFilter, nullptr);
    ASSERT_NE(existingMeshRenderer, nullptr);
    existingMeshFilter->SetMeshReference(MakeRenderScenePPtr<NLS::Render::Resources::Mesh>(meshReference));
    existingMeshFilter->SetResolvedMeshFromReference(existingMesh);
    existingMeshRenderer->SetMaterialReferences({
        MakeRenderScenePPtr<NLS::Render::Resources::Material>(materialReference)
    });
    existingMeshRenderer->SetResolvedMaterialFromReference(0u, existingMaterial);
    ASSERT_EQ(existingMeshFilter->ResolveMesh(), existingMesh);
    ASSERT_EQ(existingMeshRenderer->GetMaterialAtIndex(0u), &existingMaterial);

    auto previewMesh = std::shared_ptr<NLS::Render::Resources::Mesh>(CreateSingleMesh(0u));
    ASSERT_NE(previewMesh, nullptr);
    NLS::Render::Resources::Material previewMaterial;
    previewMaterial.path = materialPath;
    previewMaterial.SetShader(shader);
    ASSERT_TRUE(previewMaterial.IsValid());

    NLS::Engine::SceneSystem::Scene previewScene;
    auto& previewObject = previewScene.CreateGameObject("PreviewPrefab");
    auto* previewMeshFilter = previewObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* previewMeshRenderer = previewObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(previewMeshFilter, nullptr);
    ASSERT_NE(previewMeshRenderer, nullptr);
    previewMeshFilter->SetMeshReference(MakeRenderScenePPtr<NLS::Render::Resources::Mesh>(meshReference));
    previewMeshFilter->SetResolvedTransientMeshFromReference(previewMesh);
    previewMeshRenderer->SetMaterialReferences({
        MakeRenderScenePPtr<NLS::Render::Resources::Material>(materialReference)
    });
    previewMeshRenderer->SetResolvedMaterialFromReference(0u, previewMaterial);
    ASSERT_EQ(previewMeshFilter->ResolveMesh(), previewMesh.get());
    ASSERT_EQ(previewMeshRenderer->GetMaterialAtIndex(0u), &previewMaterial);

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        mainScene,
        std::nullopt,
        nullptr,
        { &previewScene }
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);

    ASSERT_EQ(drawables.opaques.size(), 2u)
        << "Scene View drag preview must not hide the already saved prefab, and the preview must render through the additive scene.";
    const auto drewExisting = std::any_of(
        drawables.opaques.begin(),
        drawables.opaques.end(),
        [existingMesh, &existingMaterial](const auto& entry)
        {
            return entry.second.mesh == existingMesh && entry.second.material == &existingMaterial;
        });
    const auto drewPreview = std::any_of(
        drawables.opaques.begin(),
        drawables.opaques.end(),
        [&previewMesh, &previewMaterial](const auto& entry)
        {
            return entry.second.mesh == previewMesh.get() && entry.second.material == &previewMaterial;
        });
    EXPECT_TRUE(drewExisting);
    EXPECT_TRUE(drewPreview);

    delete existingMesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
}

TEST(RenderSceneCacheTests, SceneRendererRespectsGlobalObjectDataCapacityAcrossAdditiveScenes)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ScopedObjectDataCountLimitOverride objectDataLimit(2u);
#endif

    auto& driver = EnsureRenderSceneTestDriver();
    QueueSortFixture fixture;
    fixture.AddObject("MainA", *fixture.sharedMesh, fixture.opaqueMaterialA, 3.0f);
    fixture.AddObject("MainB", *fixture.otherMesh, fixture.opaqueMaterialB, 6.0f);

    NLS::Engine::SceneSystem::Scene previewScene;
    auto& previewObject = previewScene.CreateGameObject("PreviewOverflow");
    auto* previewMeshFilter = previewObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* previewMeshRenderer = previewObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(previewMeshFilter, nullptr);
    ASSERT_NE(previewMeshRenderer, nullptr);
    previewMeshFilter->SetMesh(fixture.otherMesh);
    previewMeshRenderer->FillWithMaterial(fixture.opaqueMaterialA);
    previewObject.GetTransform()->SetWorldPosition({ 9.0f, 0.0f, 0.0f });

    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        fixture.scene,
        std::nullopt,
        nullptr,
        { &previewScene }
    });

    NLS::Render::Entities::Camera camera;
    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    const auto drawables = renderer.CaptureSceneDrawables(frameDescriptor);
    ASSERT_GE(drawables.opaques.size(), 2u);
    bool sawInvalidOverflowDrawable = false;
    for (const auto& entry : drawables.opaques)
    {
        NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
        ASSERT_TRUE(entry.second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(descriptor));
        if (descriptor.objectIndex == NLS::Engine::Rendering::EngineDrawableDescriptor::kInvalidObjectIndex)
        {
            sawInvalidOverflowDrawable = true;
            continue;
        }

        uint32_t lastObjectIndex = 0u;
        EXPECT_TRUE(NLS::Render::Data::TryResolveObjectDataRangeEnd(
            descriptor.objectIndex,
            std::max<uint32_t>(1u, descriptor.objectCount),
            lastObjectIndex));
    }
    EXPECT_TRUE(sawInvalidOverflowDrawable);
    EXPECT_EQ(renderer.GetFrameInfo().objectDataOverflowDroppedObjectCount, 1u);
}

TEST(RenderSceneCacheTests, DynamicInstancingMergesCompatibleOpaqueCommandsIntoObjectIndexRange)
{
    QueueSortFixture fixture;
    fixture.AddObject("InstanceNear", *fixture.sharedMesh, fixture.opaqueMaterialA, 4.0f);
    fixture.AddObject("InstanceMiddle", *fixture.sharedMesh, fixture.opaqueMaterialA, 12.0f);
    fixture.AddObject("InstanceFar", *fixture.sharedMesh, fixture.opaqueMaterialA, 24.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });

    ASSERT_EQ(visible.opaques.size(), 1u);
    const auto& drawable = visible.opaques.front().second;
    EXPECT_EQ(drawable.mesh, fixture.sharedMesh);
    EXPECT_EQ(ResolveVisibleInstanceCount(drawable), 3u);

    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    ASSERT_TRUE(drawable.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(descriptor));
    EXPECT_EQ(descriptor.objectIndex, 0u);
    EXPECT_EQ(descriptor.objectCount, 3u);
    ASSERT_EQ(descriptor.instanceModelMatrices.size(), 3u);
    EXPECT_FLOAT_EQ(descriptor.instanceModelMatrices[0].data[3], 4.0f);
    EXPECT_FLOAT_EQ(descriptor.instanceModelMatrices[1].data[3], 12.0f);
    EXPECT_FLOAT_EQ(descriptor.instanceModelMatrices[2].data[3], 24.0f);
}

TEST(RenderSceneCacheTests, DynamicInstancingBuildsMergedDescriptorInLinearPass)
{
    QueueSortFixture fixture;
    constexpr size_t kInstanceCount = 96u;
    for (size_t index = 0u; index < kInstanceCount; ++index)
    {
        fixture.AddObject(
            ("LinearInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            fixture.opaqueMaterialA,
            static_cast<float>(index));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, kInstanceCount);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });

    ASSERT_EQ(visible.opaques.size(), 1u);
    const auto& drawable = visible.opaques.front().second;
    EXPECT_EQ(ResolveVisibleInstanceCount(drawable), kInstanceCount);

    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    ASSERT_TRUE(drawable.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(descriptor));
    EXPECT_EQ(descriptor.objectCount, kInstanceCount);
    ASSERT_EQ(descriptor.instanceModelMatrices.size(), kInstanceCount);
    EXPECT_EQ(descriptor.instanceModelMatrices.capacity(), kInstanceCount);
    EXPECT_FLOAT_EQ(descriptor.instanceModelMatrices.front().data[3], 0.0f);
    EXPECT_FLOAT_EQ(descriptor.instanceModelMatrices.back().data[3], static_cast<float>(kInstanceCount - 1u));

    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();
    EXPECT_EQ(optimizationStats.rawVisibleObjectCount, kInstanceCount);
    EXPECT_EQ(optimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(optimizationStats.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(optimizationStats.largestInstanceGroupSize, kInstanceCount);
    EXPECT_EQ(optimizationStats.cachedCommandRebuildCount, kInstanceCount);
}

TEST(RenderSceneCacheTests, DynamicInstancingReducesOneThousandCompatibleOpaqueObjectsToOneSubmittedDraw)
{
    QueueSortFixture fixture;
    constexpr size_t kInstanceCount = 1000u;
    for (size_t index = 0u; index < kInstanceCount; ++index)
    {
        fixture.AddObject(
            ("StressInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            fixture.opaqueMaterialA,
            static_cast<float>(index));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;

    const auto firstSync = renderScene.Synchronize(fixture.scene, syncOptions);
    const auto firstVisible = renderScene.GatherVisibleCommands({ nullptr, {} });
    const auto firstOptimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();
    const auto secondSync = renderScene.Synchronize(fixture.scene, syncOptions);
    const auto secondVisible = renderScene.GatherVisibleCommands({ nullptr, {} });
    const auto secondOptimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    ASSERT_EQ(firstVisible.opaques.size(), 1u);
    ASSERT_EQ(secondVisible.opaques.size(), 1u);
    EXPECT_EQ(firstSync.rebuiltCachedCommandCount, kInstanceCount);
    EXPECT_EQ(secondSync.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(firstOptimizationStats.rawVisibleObjectCount, kInstanceCount);
    EXPECT_EQ(firstOptimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(firstOptimizationStats.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(firstOptimizationStats.largestInstanceGroupSize, kInstanceCount);
    EXPECT_EQ(firstOptimizationStats.cachedCommandRebuildCount, kInstanceCount);
    EXPECT_EQ(secondOptimizationStats.rawVisibleObjectCount, kInstanceCount);
    EXPECT_EQ(secondOptimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(secondOptimizationStats.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(secondOptimizationStats.largestInstanceGroupSize, kInstanceCount);
    EXPECT_EQ(secondOptimizationStats.cachedCommandRebuildCount, 0u);
}

TEST(RenderSceneCacheTests, SpatialVisibilityPipelinePreservesOneThousandCompatibleOpaqueReduction)
{
    QueueSortFixture fixture;
    constexpr size_t kInstanceCount = 1000u;
    for (size_t index = 0u; index < kInstanceCount; ++index)
    {
        fixture.AddObjectAt(
            ("SpatialStressInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            fixture.opaqueMaterialA,
            {
                static_cast<float>(index % 20u) * 0.1f - 1.0f,
                0.0f,
                -5.0f - static_cast<float>(index / 20u) * 0.1f
            });
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, kInstanceCount);

    auto frustum = CreateForwardFrustum();
    auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
    settings.enableSpatialIndex = true;

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &settings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();
    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques.front().second), kInstanceCount);
    EXPECT_EQ(optimizationStats.rawVisibleObjectCount, kInstanceCount);
    EXPECT_EQ(optimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(optimizationStats.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(optimizationStats.largestInstanceGroupSize, kInstanceCount);
    EXPECT_EQ(telemetry.rawVisibleDrawCount, kInstanceCount);
    EXPECT_EQ(telemetry.submittedDrawCount, 1u);
    EXPECT_EQ(telemetry.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(telemetry.finalizationTouchedPrimitiveCount, telemetry.visiblePrimitiveCount);
}

TEST(RenderSceneCacheTests, DynamicInstancingReducesTraceScaleCompatibleObjectsToOneSubmittedDraw)
{
    QueueSortFixture fixture;
    constexpr size_t kTraceScaleDrawCount = 259u;
    for (size_t index = 0u; index < kTraceScaleDrawCount; ++index)
    {
        fixture.AddObject(
            ("TraceScaleInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            fixture.opaqueMaterialA,
            static_cast<float>(index));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, kTraceScaleDrawCount);
    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques.front().second), kTraceScaleDrawCount);
    EXPECT_EQ(optimizationStats.rawVisibleObjectCount, kTraceScaleDrawCount);
    EXPECT_EQ(optimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(optimizationStats.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(optimizationStats.largestInstanceGroupSize, kTraceScaleDrawCount);
}

TEST(RenderSceneCacheTests, DynamicInstancingRejectsIncompatibleMeshMaterialAndTransparentCommands)
{
    QueueSortFixture fixture;
    fixture.AddObject("CompatibleA", *fixture.sharedMesh, fixture.opaqueMaterialA, 4.0f);
    fixture.AddObject("CompatibleB", *fixture.sharedMesh, fixture.opaqueMaterialA, 8.0f);
    fixture.AddObject("DifferentMaterial", *fixture.sharedMesh, fixture.opaqueMaterialB, 12.0f);
    fixture.AddObject("DifferentMesh", *fixture.otherMesh, fixture.opaqueMaterialA, 16.0f);
    fixture.AddObject("TransparentA", *fixture.sharedMesh, fixture.transparentMaterial, 20.0f);
    fixture.AddObject("TransparentB", *fixture.sharedMesh, fixture.transparentMaterial, 24.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 6u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });

    ASSERT_EQ(visible.opaques.size(), 3u);
    const auto mergedOpaqueCount = static_cast<size_t>(std::count_if(
        visible.opaques.begin(),
        visible.opaques.end(),
        [](const auto& entry)
        {
            return ResolveVisibleInstanceCount(entry.second) == 2u;
        }));
    EXPECT_EQ(mergedOpaqueCount, 1u);

    ASSERT_EQ(visible.transparents.size(), 2u);
    for (const auto& entry : visible.transparents)
        EXPECT_EQ(ResolveVisibleInstanceCount(entry.second), 1u);
}

TEST(RenderSceneCacheTests, DynamicInstancingRejectsDifferentUserMatrices)
{
    QueueSortFixture fixture;
    fixture.AddObject("UserMatrixA", *fixture.sharedMesh, fixture.opaqueMaterialA, 4.0f);
    fixture.AddObject("UserMatrixB", *fixture.sharedMesh, fixture.opaqueMaterialA, 8.0f);

    auto& objects = fixture.scene.GetGameObjects();
    ASSERT_GE(objects.size(), 2u);
    auto* secondRenderer = objects[1]->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(secondRenderer, nullptr);
    secondRenderer->SetUserMatrixElement(0u, 3u, 7.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });

    ASSERT_EQ(visible.opaques.size(), 2u);
    for (const auto& entry : visible.opaques)
        EXPECT_EQ(ResolveVisibleInstanceCount(entry.second), 1u);
}

TEST(RenderSceneCacheTests, DynamicInstancingSplitsSubmittedDrawsWhenObjectDataLimitIsExceeded)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    QueueSortFixture fixture;
    for (size_t index = 0u; index < 4u; ++index)
    {
        fixture.AddObject(
            ("SplitInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            fixture.opaqueMaterialA,
            static_cast<float>(index));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    const ScopedObjectDataCountPerDrawLimitOverride scopedPerDrawLimit(3u);
    EXPECT_EQ(NLS::Render::Data::GetObjectDataCountPerDrawLimitForTesting(), 3u);
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 4u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    ASSERT_EQ(visible.opaques.size(), 2u);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques[0].second), 3u);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques[1].second), 1u);

    NLS::Engine::Rendering::EngineDrawableDescriptor firstDescriptor;
    NLS::Engine::Rendering::EngineDrawableDescriptor secondDescriptor;
    ASSERT_TRUE(visible.opaques[0].second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(firstDescriptor));
    ASSERT_TRUE(visible.opaques[1].second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(secondDescriptor));
    EXPECT_EQ(firstDescriptor.objectIndex, 0u);
    EXPECT_EQ(firstDescriptor.objectCount, 3u);
    EXPECT_EQ(secondDescriptor.objectIndex, 3u);
    EXPECT_EQ(secondDescriptor.objectCount, 1u);

    EXPECT_EQ(optimizationStats.rawVisibleObjectCount, 4u);
    EXPECT_EQ(optimizationStats.submittedSceneDrawCount, 2u);
    EXPECT_EQ(optimizationStats.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(optimizationStats.largestInstanceGroupSize, 3u);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to override object-data limits.";
#endif
}

TEST(RenderSceneCacheTests, DynamicInstancingDropsOverflowObjectsWithDiagnosticWhenObjectDataCapacityIsExceeded)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    QueueSortFixture fixture;
    for (size_t index = 0u; index < 4u; ++index)
    {
        fixture.AddObject(
            ("OverflowInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            fixture.opaqueMaterialA,
            static_cast<float>(index));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    const ScopedObjectDataCountLimitOverride scopedTotalLimit(3u);
    EXPECT_EQ(NLS::Render::Data::GetObjectDataCountLimitForTesting(), 3u);
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 4u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques[0].second), 3u);

    NLS::Engine::Rendering::EngineDrawableDescriptor assignedDescriptor;
    ASSERT_TRUE(visible.opaques[0].second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(assignedDescriptor));
    EXPECT_EQ(assignedDescriptor.objectIndex, 0u);
    EXPECT_EQ(assignedDescriptor.objectCount, 3u);

    EXPECT_EQ(optimizationStats.rawVisibleObjectCount, 4u);
    EXPECT_EQ(optimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(optimizationStats.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(optimizationStats.largestInstanceGroupSize, 3u);
    EXPECT_EQ(optimizationStats.objectDataOverflowDroppedObjectCount, 1u);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to override object-data limits.";
#endif
}

TEST(RenderSceneCacheTests, NonIndexedObjectDataDrawsDoNotConsumeGlobalObjectDataCapacity)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    QueueSortFixture fixture;
    auto* legacyShader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Unlit.hlsl");
    ASSERT_NE(legacyShader, nullptr);
    ASSERT_FALSE(NLS::Render::Resources::ShaderSupportsIndexedObjectData(*legacyShader));

    NLS::Render::Resources::Material legacyMaterial;
    legacyMaterial.SetShader(legacyShader);
    for (size_t index = 0u; index < 4u; ++index)
    {
        fixture.AddObject(
            ("LegacyObjectDataInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            legacyMaterial,
            static_cast<float>(index));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    const ScopedObjectDataCountLimitOverride scopedTotalLimit(3u);
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &legacyMaterial;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 4u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    EXPECT_EQ(visible.opaques.size(), 4u);
    EXPECT_EQ(optimizationStats.objectDataOverflowDroppedObjectCount, 0u);
    for (const auto& entry : visible.opaques)
    {
        NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
        ASSERT_TRUE(entry.second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(descriptor));
        EXPECT_EQ(descriptor.objectIndex, NLS::Render::Data::DrawableObjectDescriptor::kInvalidObjectIndex);
    }

    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(legacyShader));
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to override object-data limits.";
#endif
}

TEST(RenderSceneCacheTests, DynamicInstancingDropsOverflowObjectsAfterPerDrawChunksWithDiagnostic)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    QueueSortFixture fixture;
    for (size_t index = 0u; index < 8u; ++index)
    {
        fixture.AddObject(
            ("OverflowChunkInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            fixture.opaqueMaterialA,
            static_cast<float>(index));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    const ScopedObjectDataCountLimitOverride scopedTotalLimit(3u);
    const ScopedObjectDataCountPerDrawLimitOverride scopedPerDrawLimit(3u);
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 8u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    ASSERT_EQ(visible.opaques.size(), 1u);
    const std::array<uint32_t, 1u> expectedCounts{ 3u };
    for (size_t index = 0u; index < visible.opaques.size(); ++index)
    {
        EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques[index].second), expectedCounts[index]);
        NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
        ASSERT_TRUE(visible.opaques[index].second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(descriptor));
        EXPECT_EQ(descriptor.objectCount, expectedCounts[index]);
        EXPECT_EQ(descriptor.objectIndex, 0u);
    }

    EXPECT_EQ(optimizationStats.rawVisibleObjectCount, 8u);
    EXPECT_EQ(optimizationStats.submittedSceneDrawCount, 1u);
    EXPECT_EQ(optimizationStats.dynamicInstanceGroupCount, 1u);
    EXPECT_EQ(optimizationStats.largestInstanceGroupSize, 3u);
    EXPECT_EQ(optimizationStats.objectDataOverflowDroppedObjectCount, 5u);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to override object-data limits.";
#endif
}

TEST(RenderSceneCacheTests, DenseCompatibleInstancesStayBoundedBySubmittedDrawLimit)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    QueueSortFixture fixture;
    constexpr size_t kInstanceCount = 513u;
    for (size_t index = 0u; index < kInstanceCount; ++index)
    {
        fixture.AddObject(
            ("DenseInstance" + std::to_string(index)).c_str(),
            *fixture.sharedMesh,
            fixture.opaqueMaterialA,
            static_cast<float>(index % 64u));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    const ScopedObjectDataCountPerDrawLimitOverride scopedPerDrawLimit(256u);
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, kInstanceCount);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    const auto optimizationStats = renderScene.GetLastDrawCallOptimizationStatsForTesting();

    ASSERT_EQ(visible.opaques.size(), 3u);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques[0].second), 256u);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques[1].second), 256u);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques[2].second), 1u);

    EXPECT_EQ(optimizationStats.rawVisibleObjectCount, kInstanceCount);
    EXPECT_EQ(optimizationStats.submittedSceneDrawCount, 3u);
    EXPECT_EQ(optimizationStats.dynamicInstanceGroupCount, 2u);
    EXPECT_EQ(optimizationStats.largestInstanceGroupSize, 256u);
#else
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to override object-data limits.";
#endif
}

TEST(RenderSceneCacheTests, RetainedSingleDrawPreservesMaterialGpuInstances)
{
    QueueSortFixture fixture;
    fixture.opaqueMaterialA.SetGPUInstances(4);
    fixture.AddObject("MaterialInstancedA", *fixture.sharedMesh, fixture.opaqueMaterialA, 4.0f);
    fixture.AddObject("MaterialInstancedB", *fixture.sharedMesh, fixture.opaqueMaterialA, 8.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });

    ASSERT_EQ(visible.opaques.size(), 2u);
    const std::array<float, 2u> expectedTranslations = { 4.0f, 8.0f };
    for (size_t drawIndex = 0u; drawIndex < visible.opaques.size(); ++drawIndex)
    {
        const auto& entry = visible.opaques[drawIndex];
        EXPECT_EQ(entry.second.instanceCount, 0u);
        EXPECT_EQ(ResolveVisibleInstanceCount(entry.second), 4u);

        NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
        ASSERT_TRUE(entry.second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(descriptor));
        EXPECT_EQ(descriptor.objectIndex, static_cast<uint32_t>(drawIndex * 4u));
        EXPECT_EQ(descriptor.objectCount, 4u);
        ASSERT_EQ(descriptor.instanceModelMatrices.size(), 4u);
        for (const auto& instanceMatrix : descriptor.instanceModelMatrices)
            EXPECT_FLOAT_EQ(instanceMatrix.data[3], expectedTranslations[drawIndex]);
    }
}
