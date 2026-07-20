#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Guid.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Jobs/JobSystem.h"
#include "Math/Matrix4.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Data/Frustum.h"
#include "Rendering/Data/DrawableInstanceCount.h"
#include "Rendering/Data/ObjectDataLimits.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RenderScene.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Resources/IndexedObjectDataShaderSupport.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/SceneHLOD.h"
#include "Rendering/SceneVisibilityPipeline.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include "Rendering/ShaderLab/ShaderLabTypes.h"
#include "Serialize/ObjectReferenceResolver.h"
#include "Serialize/PPtr.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"

namespace
{
    class RenderSceneReadyTestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        RenderSceneReadyTestTexture()
        {
            m_desc.extent = {1u, 1u, 1u};
            m_desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
            m_desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
            m_desc.debugName = "RenderSceneReadyTestTexture";
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override
        {
            return NLS::Render::RHI::ResourceState::Unknown;
        }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    NLS::Render::Resources::Texture2D* CreateReadyTestTexture()
    {
        return NLS::Render::Resources::Texture2D::WrapExternal(
            std::make_shared<RenderSceneReadyTestTexture>(),
            1u,
            1u).release();
    }

    std::string ReadRepoText(const std::filesystem::path& relativePath)
    {
        std::ifstream stream(std::filesystem::path(NLS_ROOT_DIR) / relativePath, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    bool IsDxcUnavailableDiagnosticForTests(const std::string& diagnostics)
    {
        return diagnostics.find("Unable to locate dxc.exe.") != std::string::npos ||
            diagnostics.find("Unable to locate an executable native dxc.") != std::string::npos ||
            diagnostics.find("Failed to spawn shader compiler process (") != std::string::npos ||
            diagnostics.find("[dxc-exit-code] 126") != std::string::npos ||
            diagnostics.find("[dxc-exit-code] 127") != std::string::npos;
    }

    bool IsNativeDxcUnavailableForRenderSceneCacheTests()
    {
        const auto directory = std::filesystem::temp_directory_path() /
            "NullusRenderSceneCacheTests" /
            ("DxcProbe-" + NLS::Guid::New().ToString());
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
            return false;

        const auto shaderPath = directory / "NativeDxcProbe.hlsl";
        std::ofstream shaderFile(shaderPath, std::ios::binary | std::ios::trunc);
        if (!shaderFile)
            return false;

        shaderFile
            << "struct VSOutput { float4 position : SV_Position; };\n"
            << "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
            << "    VSOutput output;\n"
            << "    output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
            << "    return output;\n"
            << "}\n";
        shaderFile.close();
        if (!shaderFile)
            return false;

        NLS::Render::ShaderCompiler::ShaderCompilationInput input;
        input.assetPath = shaderPath.string();
        input.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
        input.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
        input.options.targetProfile = "vs_6_0";
        input.options.entryPoint = "VSMain";
        input.options.artifactDirectory = (directory / "Artifacts").string();
        input.options.includeDirectories.push_back(directory.string());

        NLS::Render::ShaderCompiler::ShaderCompiler compiler;
        const auto output = compiler.Compile(input);
        std::filesystem::remove_all(directory, error);
        return output.status != NLS::Render::ShaderCompiler::ShaderCompilationStatus::Succeeded &&
            IsDxcUnavailableDiagnosticForTests(output.diagnostics);
    }

    bool NativeDxcUnavailableForTests()
    {
        static const bool unavailable = IsNativeDxcUnavailableForRenderSceneCacheTests();
        return unavailable;
    }

#define NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE()        \
    do                                                                \
    {                                                                 \
        if (NativeDxcUnavailableForTests())                           \
            GTEST_SKIP() << "Native dxc is unavailable in this environment."; \
    } while (false)

    void ClearRenderSceneResourceManagerServicesForTesting()
    {
        NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
        NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
        NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
        NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
        NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
        NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
        NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
    }

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
        ClearRenderSceneResourceManagerServicesForTesting();
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
            shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

    struct ManyPrimitiveFixture
    {
        NLS::Engine::SceneSystem::Scene scene;
        NLS::Render::Resources::Shader* shader = nullptr;
        NLS::Render::Resources::Material material;
        NLS::Render::Resources::Material decalMaterial;
        NLS::Render::Resources::Material transparentMaterial;
        std::vector<NLS::Render::Resources::Mesh*> meshes;

        explicit ManyPrimitiveFixture(
            const size_t primitiveCount,
            const bool mixSurfaceModes = false)
        {
            EnsureRenderSceneTestDriver();
            shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
            material.SetShader(shader);
            decalMaterial.SetShader(shader);
            decalMaterial.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Decal);
            transparentMaterial.SetShader(shader);
            transparentMaterial.SetBlendable(true);
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
                auto* selectedMaterial = &material;
                if (mixSurfaceModes)
                {
                    if ((index % 4u) == 1u)
                        selectedMaterial = &decalMaterial;
                    else if ((index % 4u) == 2u)
                        selectedMaterial = &transparentMaterial;
                }
                meshRenderer->FillWithMaterial(*selectedMaterial);

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
        result.reserve(queues.opaques.size() + queues.decals.size() + queues.transparents.size());

        for (const auto& entry : queues.opaques)
            result.push_back(entry.second.mesh);
        for (const auto& entry : queues.decals)
            result.push_back(entry.second.mesh);
        for (const auto& entry : queues.transparents)
            result.push_back(entry.second.mesh);

        std::sort(result.begin(), result.end());
        return result;
    }

    struct QueueSortFixture
    {
        NLS::Engine::SceneSystem::Scene scene;
        NLS::Render::Resources::Shader* shader = nullptr;
        NLS::Render::Resources::Material opaqueMaterialA;
        NLS::Render::Resources::Material opaqueMaterialB;
        NLS::Render::Resources::Material decalMaterial;
        NLS::Render::Resources::Material transparentMaterial;
        NLS::Render::Resources::Mesh* sharedMesh = nullptr;
        NLS::Render::Resources::Mesh* otherMesh = nullptr;

        QueueSortFixture()
        {
            EnsureRenderSceneTestDriver();
            shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
            opaqueMaterialA.SetShader(shader);
            opaqueMaterialB.SetShader(shader);
            decalMaterial.SetShader(shader);
            decalMaterial.SetSurfaceMode(NLS::Render::Resources::MaterialSurfaceMode::Decal);
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

        NLS::Engine::GameObject& AddObject(
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
            return object;
        }
    };

    uint32_t ResolveVisibleInstanceCount(const NLS::Render::Entities::Drawable& drawable)
    {
        return NLS::Render::Data::ResolveDrawableInstanceCount(drawable).count;
    }

    NLS::Engine::Rendering::RenderSceneVisibleQueues::SceneDrawables ExpandMergedOpaqueForShadowFlagTest(
        const NLS::Engine::Rendering::RenderSceneVisibleQueues::SceneDrawables::value_type& mergedEntry)
    {
        NLS::Engine::Rendering::EngineDrawableDescriptor mergedDescriptor;
        EXPECT_TRUE(mergedEntry.second.TryGetDescriptor(mergedDescriptor));

        NLS::Engine::Rendering::RenderSceneVisibleQueues::SceneDrawables drawables;
        drawables.reserve(mergedDescriptor.instanceModelMatrices.size());
        for (size_t index = 0u; index < mergedDescriptor.instanceModelMatrices.size(); ++index)
        {
            NLS::Render::Entities::Drawable drawable;
            drawable.mesh = mergedEntry.second.mesh;
            drawable.material = mergedEntry.second.material;
            drawable.stateMask = mergedEntry.second.stateMask;
            drawable.primitiveMode = mergedEntry.second.primitiveMode;
            drawable.instanceCount = 1u;
            drawable.vertexStart = mergedEntry.second.vertexStart;
            drawable.vertexCount = mergedEntry.second.vertexCount;

            auto descriptor = mergedDescriptor;
            descriptor.modelMatrix = mergedDescriptor.instanceModelMatrices[index];
            descriptor.objectIndex = NLS::Engine::Rendering::EngineDrawableDescriptor::kInvalidObjectIndex;
            descriptor.objectCount = 1u;
            descriptor.instanceModelMatrices.clear();
            drawable.AddDescriptor(std::move(descriptor));
            drawables.emplace_back(mergedEntry.first + static_cast<float>(index), std::move(drawable));
        }
        return drawables;
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

        const NLS::Engine::Rendering::SceneOcclusionFrameInput& GetLastHZBFrameInputForTesting() const
        {
            return GetLastHZBOcclusionFrameInput();
        }
    };

    TEST(RenderSceneCacheTests, BaseSceneRendererStoresLargeSceneSettingsOverride)
    {
        NLS::Render::Settings::DriverSettings driverSettings;
        driverSettings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
        NLS::Render::Context::Driver driver(driverSettings);
        SceneDrawableProbeRenderer renderer(driver);

        auto settings = NLS::Engine::Rendering::LargeSceneSettings::Defaults();
        settings.enableSpatialIndex = false;
        settings.enableParallelVisibility = false;
        settings.enableLOD = false;
        settings.enableHLOD = true;
        settings.enableHZBOcclusion = true;
        settings.maxVisibilityJobs = 3u;
        settings.parallelVisibilityPrimitiveThreshold = 4096u;
        settings.parallelVisibilityPrimitivesPerTask = 256u;
        settings.staticRebuildDirtyRatio = 0.45;
        settings.staticRebuildBudgetUs = 500u;
        settings.streamingCpuBudgetUs = 2500u;
        settings.streamingGpuUploadBudgetBytes = 6ull * 1024ull * 1024ull;
        settings.streamingIoBudgetBytes = 12ull * 1024ull * 1024ull;
        settings.streamingCpuMemoryBudgetBytes = 384ull * 1024ull * 1024ull;
        settings.streamingGpuMemoryBudgetBytes = 768ull * 1024ull * 1024ull;
        settings.maxOcclusionHistoryAge = 4u;

        renderer.SetLargeSceneSettings(settings);
        const auto& stored = renderer.GetLargeSceneSettings();

        EXPECT_FALSE(stored.enableSpatialIndex);
        EXPECT_FALSE(stored.enableParallelVisibility);
        EXPECT_FALSE(stored.enableLOD);
        EXPECT_TRUE(stored.enableHLOD);
        EXPECT_TRUE(stored.enableHZBOcclusion);
        EXPECT_EQ(stored.maxVisibilityJobs, 3u);
        EXPECT_EQ(stored.parallelVisibilityPrimitiveThreshold, 4096u);
        EXPECT_EQ(stored.parallelVisibilityPrimitivesPerTask, 256u);
        EXPECT_DOUBLE_EQ(stored.staticRebuildDirtyRatio, 0.45);
        EXPECT_EQ(stored.staticRebuildBudgetUs, 500u);
        EXPECT_EQ(stored.streamingCpuBudgetUs, 2500u);
        EXPECT_EQ(stored.streamingGpuUploadBudgetBytes, 6ull * 1024ull * 1024ull);
        EXPECT_EQ(stored.streamingIoBudgetBytes, 12ull * 1024ull * 1024ull);
        EXPECT_EQ(stored.streamingCpuMemoryBudgetBytes, 384ull * 1024ull * 1024ull);
        EXPECT_EQ(stored.streamingGpuMemoryBudgetBytes, 768ull * 1024ull * 1024ull);
        EXPECT_EQ(stored.maxOcclusionHistoryAge, 4u);
    }

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

    std::filesystem::path BuiltinMeshArtifactPath(
        const std::filesystem::path& engineAssetsRoot,
        const std::string& virtualSourcePath)
    {
        return engineAssetsRoot /
            "Library" /
            "BuiltinArtifacts" /
            "Models" /
            NLS::Core::Assets::BuildArtifactStorageFileName("BuiltinMeshArtifact:" + virtualSourcePath);
    }

    void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void WriteMaterialArtifact(const std::filesystem::path& artifactPath)
    {
        NLS::Core::Assets::NativeArtifactMetadata metadata;
        metadata.artifactType = NLS::Core::Assets::ArtifactType::Material;
        metadata.schemaName = "material";
        metadata.schemaVersion = 1u;

        const std::string payload =
            "shaderLabMaterialVersion=1\n"
            "shader=?\n"
            "surfaceMode=Opaque\n"
            "alphaMode=Opaque\n"
            "doubleSided=false\n"
            "depthWrite=true\n";
        const std::vector<uint8_t> bytes(payload.begin(), payload.end());
        WriteBytes(artifactPath, NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), bytes));
    }

    void WriteTextureArtifact(const std::filesystem::path& artifactPath)
    {
        NLS::Render::Assets::TextureArtifactData texture;
        texture.width = 1u;
        texture.height = 1u;
        texture.format = NLS::Render::RHI::TextureFormat::RGBA8;
        texture.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
        texture.mips.push_back({0u, 1u, 1u, 4u, 4u, {255u, 255u, 255u, 255u}});

        WriteBytes(artifactPath, NLS::Render::Assets::SerializeTextureArtifact(texture));
    }

    std::filesystem::path ContentAddressedArtifactPath(
        const std::filesystem::path& root,
        const std::string& key)
    {
        const auto fileName = NLS::Core::Assets::BuildArtifactStorageFileName(key);
        return root / "Library" / "Artifacts" / NLS::Core::Assets::BuildArtifactStorageRelativePath(fileName);
    }

    void CopyTextFile(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
    }

	void RegisterImportedShaderLabArtifact(
		const std::filesystem::path& projectRoot,
		NLS::Core::ResourceManagement::ShaderManager& shaderManager,
		const std::string& sourcePath,
		const std::string& subAssetKey,
		const std::string& fileName)
	{
		NLS::Render::Assets::ShaderArtifact artifact;
		artifact.sourcePath = sourcePath;
		artifact.subAssetKey = subAssetKey;
		if (subAssetKey.find("ForwardPreview") != std::string::npos)
			artifact.shaderLabLightMode = "ForwardPreview";
		else if (subAssetKey.find("Picking") != std::string::npos)
			artifact.shaderLabLightMode = "Picking";
		else
			artifact.shaderLabLightMode = "Forward";
		artifact.shaderLabPassState = NLS::Render::ShaderLab::ShaderLabPassState{};
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
                "standard-pbr-forward-vs-cache",
                "StandardPBR.vs.dxil"
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
                "standard-pbr-forward-ps-cache",
                "StandardPBR.ps.dxil"
            }
        });

        const auto artifactPath =
            projectRoot /
            "Library" /
            "Artifacts" /
            "33333333444445558666777777777777" /
            fileName;
        WriteBytes(artifactPath, NLS::Render::Assets::SerializeShaderArtifact(artifact));

        auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::Create(artifactPath.string());
        ASSERT_NE(shader, nullptr);
        shaderManager.RegisterResource(artifactPath.generic_string(), shader);
    }

    void RegisterImportedFallbackShaderArtifact(
        const std::filesystem::path& projectRoot,
        NLS::Core::ResourceManagement::ShaderManager& shaderManager)
    {
        RegisterImportedShaderLabArtifact(
            projectRoot,
            shaderManager,
            "App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
            "shader:StandardPBR/Forward#0",
            "efb9b670c9b9f8ccf96ba7d887d80c19d3884bf71a87f8c3a02c97c6dc8dd55b");
    }

    void RegisterImportedNonDefaultShaderLabArtifact(
        const std::filesystem::path& projectRoot,
        NLS::Core::ResourceManagement::ShaderManager& shaderManager)
    {
        RegisterImportedShaderLabArtifact(
            projectRoot,
            shaderManager,
            "Assets/Project/Shaders/OnlyForPicking.shader",
            "shader:OnlyForPicking/Picking#0",
            "3a6a3e42e1d842508a0167200da19f4dce088e622f8346ca6bf5f7b449223bd0");
    }

    void RegisterImportedForwardPreviewShaderLabArtifact(
        const std::filesystem::path& projectRoot,
        NLS::Core::ResourceManagement::ShaderManager& shaderManager)
    {
        RegisterImportedShaderLabArtifact(
            projectRoot,
            shaderManager,
            "App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
            "shader:StandardPBR/ForwardPreview#1",
            "0a6a3e42e1d842508a0167200da19f4dce088e622f8346ca6bf5f7b449223bd1");
    }

    std::string ContentAddressedArtifactPath(const std::string& hash)
    {
        return (std::filesystem::path("Library") / "Artifacts" / hash.substr(0u, 2u) / hash).generic_string();
    }

    void WriteImportedShaderLabArtifactDatabaseRecord(
        const std::filesystem::path& projectRoot,
        const std::string& sourcePath,
        const std::string& subAssetKey,
        const std::string& artifactHash,
        const std::string& lightMode,
        const std::string& targetPlatform = "editor")
    {
        NLS::Render::Assets::ShaderArtifact artifact;
        artifact.sourcePath = sourcePath;
        artifact.subAssetKey = subAssetKey;
        artifact.shaderLabLightMode = lightMode;
        artifact.shaderLabPassState = NLS::Render::ShaderLab::ShaderLabPassState{};
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
                "standard-pbr-forward-vs-cache",
                "StandardPBR.vs.dxil"
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
                "standard-pbr-forward-ps-cache",
                "StandardPBR.ps.dxil"
            }
        });

        const auto artifactPath = ContentAddressedArtifactPath(artifactHash);
        WriteBytes(projectRoot / artifactPath, NLS::Render::Assets::SerializeShaderArtifact(artifact));

        const auto sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic(sourcePath));
        NLS::Core::Assets::ArtifactManifest manifest;
        manifest.sourceAssetId = sourceAssetId;
        manifest.importerId = "ShaderLabImporter";
        manifest.importerVersion = 1u;
        manifest.targetPlatform = targetPlatform;
        manifest.primarySubAssetKey = subAssetKey;
        manifest.subAssets.push_back({
            sourceAssetId,
            subAssetKey,
            NLS::Core::Assets::ArtifactType::Shader,
            "ShaderLoader",
            targetPlatform,
            artifactPath,
            artifactHash,
            "StandardPBR Forward"
        });

        NLS::Core::Assets::ArtifactDatabase database;
        database.Load(projectRoot / "Library" / "ArtifactDB");
        database.UpsertManifest(
            manifest,
            sourcePath,
            NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
        ASSERT_TRUE(database.Save(projectRoot / "Library" / "ArtifactDB"));
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

TEST(RenderSceneCacheTests, TrustedEditorSceneRevisionSkipsStablePrimitiveScan)
{
    ManyPrimitiveFixture fixture(128u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    options.trustSceneRenderContentRevision = true;

    const auto first = renderScene.Synchronize(fixture.scene, options);
    ASSERT_EQ(first.rebuiltCachedCommandCount, 128u);
    EXPECT_FALSE(first.usedSceneRenderContentRevisionFastPath);

    const auto second = renderScene.Synchronize(fixture.scene, options);
    EXPECT_TRUE(second.usedSceneRenderContentRevisionFastPath);
    EXPECT_EQ(second.syncTouchedPrimitiveCount, 0u);
    EXPECT_EQ(second.reusedPrimitiveCount, 128u);

    fixture.scene.MarkRenderContentChanged();
    const auto invalidated = renderScene.Synchronize(fixture.scene, options);
    EXPECT_FALSE(invalidated.usedSceneRenderContentRevisionFastPath);
    EXPECT_EQ(invalidated.reusedPrimitiveCount, 128u);
}

TEST(RenderSceneCacheTests, TrustedEditorSceneRevisionFastPathIsDisabledWhilePlaying)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    options.trustSceneRenderContentRevision = true;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).rebuiltCachedCommandCount, 1u);

    fixture.scene.Play();
    auto* transform = fixture.meshRenderer->gameobject()->GetTransform();
    ASSERT_NE(transform, nullptr);
    transform->TranslateLocal({ 1.0f, 0.0f, 0.0f });

    const auto playing = renderScene.Synchronize(fixture.scene, options);
    EXPECT_FALSE(playing.usedSceneRenderContentRevisionFastPath);
    EXPECT_EQ(playing.syncTouchedPrimitiveCount, 1u);
}

TEST(RenderSceneCacheTests, ReuseCheckValidatesCachedCommandStampBeforeMaterialDereference)
{
    const auto source = ReadRepoText("Runtime/Engine/Rendering/RenderScene.cpp");
    const auto functionStart = source.find("bool RenderScene::CanReuseSynchronizedPrimitive(");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void RenderScene::RemoveMissingPrimitives", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    const auto stampMaterialCheck = body.find("slot.stamp.material != slot.command.material");
    const auto texturePathDereference = body.find("GetTextureResourcePaths()");

    EXPECT_NE(stampMaterialCheck, std::string::npos)
        << "Preview-owned materials can be replaced between frames; the render-scene reuse fast path must "
           "reject stale cached commands before reading their material pointer.";
    EXPECT_NE(texturePathDereference, std::string::npos);
    if (stampMaterialCheck != std::string::npos && texturePathDereference != std::string::npos)
        EXPECT_LT(stampMaterialCheck, texturePathDereference);
}

TEST(RenderSceneCacheTests, MutableTransformAccessUpdatesPrimitiveWorldMatrixWithoutCommandRebuild)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).reusedPrimitiveCount, 1u);
    auto beforeMove = renderScene.CreatePrimitiveSnapshotForHandles(
        renderScene.GetLivePrimitiveHandles(),
        {});
    ASSERT_EQ(beforeMove.primitiveRecords.size(), 1u);

    auto* owner = fixture.meshRenderer->gameobject();
    ASSERT_NE(owner, nullptr);
    owner->GetTransform()->TranslateLocal({1.0f, 0.0f, 0.0f});

    const auto moved = renderScene.Synchronize(fixture.scene, options);
    auto afterMove = renderScene.CreatePrimitiveSnapshotForHandles(
        renderScene.GetLivePrimitiveHandles(),
        {});
    ASSERT_EQ(afterMove.primitiveRecords.size(), 1u);

    EXPECT_EQ(moved.syncTouchedPrimitiveCount, 1u);
    EXPECT_EQ(moved.reusedPrimitiveCount, 1u);
    EXPECT_EQ(moved.rebuiltCachedCommandCount, 0u);
    EXPECT_FALSE(NLS::Maths::Matrix4::AreEquals(
        beforeMove.primitiveRecords.front().worldMatrix,
        afterMove.primitiveRecords.front().worldMatrix));
    EXPECT_TRUE(NLS::Maths::Matrix4::AreEquals(
        owner->GetTransform()->GetWorldMatrix(),
        afterMove.primitiveRecords.front().worldMatrix));
    EXPECT_EQ(renderScene.GatherVisibleCommands({}).opaques.size(), 1u);
}

TEST(RenderSceneCacheTests, MeshContentRevisionChangeInvalidatesRetainedPrimitive)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;

    const auto first = renderScene.Synchronize(fixture.scene, options);
    ASSERT_EQ(first.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(renderScene.GatherVisibleCommands({}).opaques.size(), 1u);
    auto firstSnapshot = renderScene.CreatePrimitiveSnapshotForHandles(
        renderScene.GetLastVisiblePrimitiveHandles(),
        {});
    ASSERT_EQ(firstSnapshot.primitiveRecords.size(), 1u);
    EXPECT_EQ(firstSnapshot.primitiveRecords.front().modelBoundingSphere.radius, 1.0f);

    fixture.mesh->Reload(
        std::vector<NLS::Render::Geometry::Vertex>{
            VertexAt(-2.0f, -2.0f, 0.0f),
            VertexAt(2.0f, -2.0f, 0.0f),
            VertexAt(0.0f, 2.0f, 0.0f)
        },
        std::vector<uint32_t>{0u, 1u, 2u},
        0u,
        NLS::Render::Resources::MeshBufferUploadMode::GpuOnly,
        {{0.0f, 0.0f, 0.0f}, 4.0f});

    const auto changed = renderScene.Synchronize(fixture.scene, options);
    ASSERT_EQ(renderScene.GatherVisibleCommands({}).opaques.size(), 1u);
    auto changedSnapshot = renderScene.CreatePrimitiveSnapshotForHandles(
        renderScene.GetLastVisiblePrimitiveHandles(),
        {});

    EXPECT_EQ(changed.syncTouchedPrimitiveCount, 1u)
        << "In-place mesh reloads update bounds/material identity without changing MeshFilter revisions.";
    ASSERT_EQ(changedSnapshot.primitiveRecords.size(), 1u);
    EXPECT_EQ(changedSnapshot.primitiveRecords.front().modelBoundingSphere.radius, 4.0f);
}

TEST(RenderSceneCacheTests, DestroyedGameObjectInvalidatesFastAccessSoRenderSceneDropsPrimitive)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;

    const auto first = renderScene.Synchronize(fixture.scene, options);
    ASSERT_EQ(first.addedPrimitiveCount, 1u);
    ASSERT_EQ(renderScene.GetPrimitiveCount(), 1u);

    const auto revisionBeforeDestroy = fixture.scene.GetFastAccessComponentsRevision();
    ASSERT_TRUE(fixture.scene.DestroyGameObject(*fixture.meshRenderer->gameobject()));
    const auto revisionAfterDestroy = fixture.scene.GetFastAccessComponentsRevision();

    EXPECT_GT(revisionAfterDestroy, revisionBeforeDestroy)
        << "RenderScene only removes missing MeshRenderers when Scene fast-access membership changes.";
    const auto second = renderScene.Synchronize(fixture.scene, options);
    EXPECT_EQ(second.removedPrimitiveCount, 1u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u);
    EXPECT_TRUE(renderScene.GetLastRemovedPrimitiveHandles().size() >= 1u);
}

TEST(RenderSceneCacheTests, DisabledSpatialIndexDoesNotBuildIndexDuringSync)
{
    ManyPrimitiveFixture fixture(128u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::LargeSceneSettings largeSceneSettings;
    largeSceneSettings.enableSpatialIndex = false;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    syncOptions.largeSceneSettings = &largeSceneSettings;

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 128u);

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &largeSceneSettings;
    (void)renderScene.GatherVisibleCommands(visibilityOptions);

    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();
    EXPECT_EQ(telemetry.staticPrimitiveCount, 0u);
    EXPECT_EQ(telemetry.dynamicPrimitiveCount, 0u);
    EXPECT_EQ(telemetry.unclassifiedPrimitiveCount, 128u);
    EXPECT_EQ(telemetry.staticIndexRebuildCount, 0u);
    EXPECT_EQ(telemetry.staticIndexRefitCount, 0u);
    EXPECT_EQ(telemetry.dynamicIndexUpdateCount, 0u);
}

TEST(RenderSceneCacheTests, SpatialVisibilityFinalizationAvoidsGlobalBitsetsOnRuntimeGather)
{
    ManyPrimitiveFixture fixture(256u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 256u);

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    const auto visible = renderScene.GatherVisibleCommands(visibilityOptions);

    EXPECT_FALSE(visible.opaques.empty());
    const auto& telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();
    EXPECT_GT(telemetry.spatialCandidateCount, 0u);
    EXPECT_EQ(telemetry.visibilityBitsetWordCount, 0u);
    EXPECT_GT(telemetry.finalizationTouchedPrimitiveCount, 0u);
	EXPECT_LT(telemetry.finalizationTouchedPrimitiveCount, telemetry.registeredPrimitiveCount);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(RenderSceneCacheTests, SpatialVisibilityReusesCandidateSnapshotAndInvalidatesAfterSceneChange)
{
	RenderableFixture fixture;
	NLS::Engine::Rendering::RenderScene renderScene;
	fixture.meshRenderer->gameobject()->GetTransform()->SetWorldPosition({ 0.0f, 0.0f, -6.0f });

	NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
	syncOptions.defaultMaterial = &fixture.material;
	ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 1u);

	auto frustum = CreateForwardFrustum();
	NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
	visibilityOptions.frustum = &frustum;
	visibilityOptions.cameraPosition = {};
	ASSERT_EQ(renderScene.GatherVisibleCommands(visibilityOptions).opaques.size(), 1u);
	EXPECT_EQ(renderScene.GetSpatialCandidateSnapshotCacheHitCountForTesting(), 0u);
	ASSERT_EQ(renderScene.GatherVisibleCommands(visibilityOptions).opaques.size(), 1u);
	EXPECT_EQ(renderScene.GetSpatialCandidateSnapshotCacheHitCountForTesting(), 1u);

	fixture.meshRenderer->SetUserMatrixElement(0u, 3u, 7.0f);
	ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).syncTouchedPrimitiveCount, 1u);
	const auto changedVisible = renderScene.GatherVisibleCommands(visibilityOptions);
	ASSERT_EQ(changedVisible.opaques.size(), 1u);
	EXPECT_EQ(renderScene.GetSpatialCandidateSnapshotCacheHitCountForTesting(), 1u);
	const auto* changedDescriptor =
		changedVisible.opaques.front().second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>();
	ASSERT_NE(changedDescriptor, nullptr);
	EXPECT_FLOAT_EQ(changedDescriptor->userMatrix.data[3], 7.0f);

	ASSERT_EQ(renderScene.GatherVisibleCommands(visibilityOptions).opaques.size(), 1u);
	EXPECT_EQ(renderScene.GetSpatialCandidateSnapshotCacheHitCountForTesting(), 2u);
}
#endif

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

TEST(RenderSceneCacheTests, OpaqueVisiblePrimitiveBuildsHZBOcclusionCandidateSource)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    fixture.material.SetBlendable(false);
    fixture.material.SetDepthTest(true);
    fixture.material.SetDepthWriting(true);

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).rebuiltCachedCommandCount, 1u);

    const auto visible = renderScene.GatherVisibleCommands({});
    ASSERT_EQ(visible.opaques.size(), 1u);
    ASSERT_EQ(renderScene.GetLastVisiblePrimitiveHandles().size(), 1u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForHandles(
        renderScene.GetLastVisiblePrimitiveHandles(),
        {});
    ASSERT_EQ(snapshot.primitiveRecords.size(), 1u);
    EXPECT_TRUE(snapshot.primitiveRecords.front().depthWriteEligibleForOcclusion);

    const auto sources = NLS::Engine::Rendering::SceneOcclusionSystem::BuildHZBPrimitivePacketSources(
        snapshot,
        renderScene.GetLastVisiblePrimitiveHandles());
    ASSERT_EQ(sources.sources.size(), 1u);
    EXPECT_EQ(sources.sources.front().primitive.handle, renderScene.GetLastVisiblePrimitiveHandles().front());
    EXPECT_EQ(sources.rejectedPrimitiveCount, 0u);
}

TEST(RenderSceneCacheTests, HZBOcclusionCandidateSourceUsesMeshAABBWhenFrustumUsesCustomSphere)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    fixture.material.SetBlendable(false);
    fixture.material.SetDepthTest(true);
    fixture.material.SetDepthWriting(true);
    fixture.meshRenderer->SetFrustumBehaviour(
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM);
    fixture.meshRenderer->SetCustomBoundingSphere({ { 0.0f, 0.0f, 0.0f }, 100.0f });

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, options).rebuiltCachedCommandCount, 1u);

    const auto visible = renderScene.GatherVisibleCommands({});
    ASSERT_EQ(visible.opaques.size(), 1u);
    ASSERT_EQ(renderScene.GetLastVisiblePrimitiveHandles().size(), 1u);

    const auto snapshot = renderScene.CreatePrimitiveSnapshotForHandles(
        renderScene.GetLastVisiblePrimitiveHandles(),
        {});
    const auto sources = NLS::Engine::Rendering::SceneOcclusionSystem::BuildHZBPrimitivePacketSources(
        snapshot,
        renderScene.GetLastVisiblePrimitiveHandles());

    NLS::Engine::Rendering::SceneOcclusionPrimitivePacketBuildInput buildInput;
    buildInput.viewProjection = NLS::Maths::Matrix4::Identity;
    buildInput.viewportWidth = 100u;
    buildInput.viewportHeight = 100u;
    const auto packets = NLS::Engine::Rendering::SceneOcclusionSystem::BuildHZBPrimitivePackets(
        buildInput,
        sources.sources);

    ASSERT_EQ(packets.primitivePackets.size(), 1u);
    const auto& packet = packets.primitivePackets.front();
    EXPECT_FLOAT_EQ(packet.screenMinX, 25.0f);
    EXPECT_FLOAT_EQ(packet.screenMaxX, 75.0f);
    EXPECT_FLOAT_EQ(packet.screenMinY, 25.0f);
    EXPECT_FLOAT_EQ(packet.screenMaxY, 75.0f);
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
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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

TEST(RenderSceneCacheTests, SynchronizeRetriesDeferredMeshAndMaterialReferencesAfterResourceRegistration)
{
    using namespace NLS::Engine::Serialize;

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    const auto meshGuid = NLS::Guid::Parse("11111111-2222-4333-8444-555555555555");
    const auto materialGuid = NLS::Guid::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    const auto meshPath = std::string("Library/Artifacts/Hero/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");
    const auto materialPath = std::string("Library/Artifacts/Hero/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b");
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
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    const auto meshGuid = NLS::Guid::Parse("22222222-3333-4444-8555-666666666666");
    const auto materialGuid = NLS::Guid::Parse("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");
    const auto meshPath = std::string("Library/Artifacts/Missing/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");
    const auto materialPath = std::string("Library/Artifacts/Missing/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b");
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
    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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
    auto& driver = EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_loaded_primitive_cube_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    WriteCubeMeshArtifact(
        BuiltinMeshArtifactPath(root.Path() / "EngineAssets", "Models/Cube.fbx"));

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedRenderSceneResourceManagers resourceManagers(
        meshManager,
        materialManager,
        shaderManager,
        projectAssetsRoot,
        engineAssetsRoot);

    RegisterImportedFallbackShaderArtifact(root.Path() / "Project", shaderManager);
    NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager);
    EXPECT_FALSE(shaderManager.GetResources().empty());

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
    EXPECT_NE(drawables.opaques.front().second.material->GetParameterBlock().TryGet("_BaseColor"), nullptr)
        << "Default scene fallback material must populate ShaderLab StandardPBR properties, not legacy HLSL parameter names.";
    EXPECT_EQ(drawables.opaques.front().second.material->GetParameterBlock().TryGet("u_Diffuse"), nullptr);
}

TEST(RenderSceneCacheTests, HZBFrameInputViewCompatibilityChangesWhenCameraMoves)
{
    auto& driver = EnsureRenderSceneTestDriver();

    NLS::Engine::SceneSystem::Scene scene;
    SceneDrawableProbeRenderer renderer(driver);
    renderer.AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
        scene,
        std::nullopt,
        nullptr
    });

    NLS::Maths::Transform cameraTransform;
    NLS::Render::Entities::Camera camera(&cameraTransform);
    camera.CacheMatrices(128u, 128u);

    NLS::Render::Data::FrameDescriptor frameDescriptor;
    frameDescriptor.renderWidth = 128u;
    frameDescriptor.renderHeight = 128u;
    frameDescriptor.camera = &camera;

    renderer.CaptureSceneDrawables(frameDescriptor);
    const auto firstHash = renderer.GetLastHZBFrameInputForTesting().viewCompatibilityHash;
    ASSERT_NE(firstHash, 0u);

    cameraTransform.SetWorldPosition({10.0f, 3.0f, -4.0f});
    camera.CacheMatrices(128u, 128u);

    renderer.CaptureSceneDrawables(frameDescriptor);
    const auto movedHash = renderer.GetLastHZBFrameInputForTesting().viewCompatibilityHash;

    EXPECT_NE(movedHash, firstHash);
}

TEST(RenderSceneCacheTests, SceneFallbackPreloadLoadsStandardPbrForwardFromArtifactDatabase)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_scene_fallback_artifactdb_" + NLS::Guid::New().ToString()));
    const auto projectRoot = root.Path() / "Project";
    const auto projectAssetsRoot = (projectRoot / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedRenderSceneResourceManagers resourceManagers(
        meshManager,
        materialManager,
        shaderManager,
        projectAssetsRoot,
        engineAssetsRoot);

    WriteImportedShaderLabArtifactDatabaseRecord(
        projectRoot,
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        "shader:StandardPBR/Forward#0",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        "Forward");

    EXPECT_TRUE(shaderManager.GetResources().empty());
    EXPECT_TRUE(NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager));

    const auto resources = shaderManager.GetResources();
    ASSERT_EQ(resources.size(), 1u);
    const auto* shader = resources.begin()->second;
    ASSERT_NE(shader, nullptr);
    EXPECT_TRUE(shader->GetShaderLabPassState().has_value());
    EXPECT_EQ(shader->GetShaderLabLightMode(), "Forward");
    EXPECT_EQ(shader->GetImportedArtifactSubAssetKey(), "shader:StandardPBR/Forward#0");
}

TEST(RenderSceneCacheTests, SceneFallbackPreloadAcceptsStandardPbrPrimarySubAssetForwardArtifact)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_scene_fallback_primary_subasset_" + NLS::Guid::New().ToString()));
    const auto projectRoot = root.Path() / "Project";
    const auto projectAssetsRoot = (projectRoot / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedRenderSceneResourceManagers resourceManagers(
        meshManager,
        materialManager,
        shaderManager,
        projectAssetsRoot,
        engineAssetsRoot);

    WriteImportedShaderLabArtifactDatabaseRecord(
        projectRoot,
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        "shader:StandardPBR",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        "Forward");

    EXPECT_TRUE(NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager));

    const auto resources = shaderManager.GetResources();
    ASSERT_EQ(resources.size(), 1u);
    const auto* shader = resources.begin()->second;
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(shader->GetShaderLabLightMode(), "Forward");
    EXPECT_EQ(shader->GetImportedArtifactSubAssetKey(), "shader:StandardPBR");
}

TEST(RenderSceneCacheTests, SceneFallbackPreloadAcceptsPackagedStandardPbrSourcePath)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_scene_fallback_packaged_source_" + NLS::Guid::New().ToString()));
    const auto projectRoot = root.Path() / "Project";
    const auto projectAssetsRoot = (projectRoot / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedRenderSceneResourceManagers resourceManagers(
        meshManager,
        materialManager,
        shaderManager,
        projectAssetsRoot,
        engineAssetsRoot);

    WriteImportedShaderLabArtifactDatabaseRecord(
        projectRoot,
        "App/Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        "shader:StandardPBR/Forward#0",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        "Forward");

    EXPECT_TRUE(NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager));

    const auto resources = shaderManager.GetResources();
    ASSERT_EQ(resources.size(), 1u);
    const auto* shader = resources.begin()->second;
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(shader->GetShaderLabLightMode(), "Forward");
    EXPECT_EQ(resources.begin()->first, "Library/Artifacts/2b/2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607");
}

TEST(RenderSceneCacheTests, SceneFallbackPreloadIgnoresRuntimeTargetStandardPbrArtifact)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_scene_fallback_runtime_target_ignored_" + NLS::Guid::New().ToString()));
    const auto projectRoot = root.Path() / "Project";
    const auto projectAssetsRoot = (projectRoot / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedRenderSceneResourceManagers resourceManagers(
        meshManager,
        materialManager,
        shaderManager,
        projectAssetsRoot,
        engineAssetsRoot);

    WriteImportedShaderLabArtifactDatabaseRecord(
        projectRoot,
        "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader",
        "shader:StandardPBR/Forward#0",
        "2b40aa9d7e26302abaee46d90172b24a111dda5b6d466fcf2e7a2aff001a0607",
        "Forward",
        "win64");

    EXPECT_FALSE(NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager, false));

    EXPECT_TRUE(shaderManager.GetResources().empty())
        << "Editor scene fallback selection must not load runtime-target ShaderLab records from ArtifactDB.";
}

TEST(RenderSceneCacheTests, SceneRendererDoesNotUseArbitraryShaderLabArtifactAsDefaultMaterialFallback)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    auto& driver = EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_loaded_primitive_cube_no_default_shader_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    WriteCubeMeshArtifact(
        BuiltinMeshArtifactPath(root.Path() / "EngineAssets", "Models/Cube.fbx"));

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedRenderSceneResourceManagers resourceManagers(
        meshManager,
        materialManager,
        shaderManager,
        projectAssetsRoot,
        engineAssetsRoot);

    RegisterImportedNonDefaultShaderLabArtifact(root.Path() / "Project", shaderManager);
    NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager);

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

    EXPECT_TRUE(drawables.opaques.empty());
}

TEST(RenderSceneCacheTests, SceneRendererUsesOnlyExactStandardPbrForwardArtifactAsFallback)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    auto& driver = EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_loaded_primitive_cube_exact_default_shader_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    WriteCubeMeshArtifact(
        BuiltinMeshArtifactPath(root.Path() / "EngineAssets", "Models/Cube.fbx"));

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    const ScopedRenderSceneResourceManagers resourceManagers(
        meshManager,
        materialManager,
        shaderManager,
        projectAssetsRoot,
        engineAssetsRoot);

    RegisterImportedForwardPreviewShaderLabArtifact(root.Path() / "Project", shaderManager);
    RegisterImportedFallbackShaderArtifact(root.Path() / "Project", shaderManager);
    NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager);

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

    ASSERT_EQ(drawables.opaques.size(), 1u);
    ASSERT_NE(drawables.opaques.front().second.material, nullptr);
    ASSERT_NE(drawables.opaques.front().second.material->GetShader(), nullptr);
    EXPECT_EQ(
        drawables.opaques.front().second.material->GetShader()->GetImportedArtifactSubAssetKey(),
        "shader:StandardPBR/Forward#0");
}

TEST(RenderSceneCacheTests, SynchronizeResolvesOnlyMaterialSlotUsedByMesh)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);
    auto* usedMaterial = new NLS::Render::Resources::Material();
    auto* unusedMaterial = new NLS::Render::Resources::Material();
    usedMaterial->SetShader(shader);
    unusedMaterial->SetShader(shader);

    const auto usedPath = std::string("Library/Artifacts/HotPath/56e3516af0658948400310dcb9266e24f19000c93d95470a205df88062fcb2b9");
    const auto unusedPath = std::string("Library/Artifacts/HotPath/71af9c5b3b34a1a2ab603d8f9d551fecab5809c48c5ce9a3ab27b326c6662efa");
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

TEST(RenderSceneCacheTests, SynchronizeResolvesMaterialSlotAboveLegacyByteRange)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    auto* material = new NLS::Render::Resources::Material();
    material->SetShader(shader);
    const auto materialPath = std::string("Library/Artifacts/HighSlot/7dff82104de34261a701858aeacb53f7955f1cbb7f0c46af8d1bcf2937d514f2");
    const_cast<std::string&>(material->path) = materialPath;
    materialManager.RegisterResource(materialPath, material);

    constexpr uint32_t kHighMaterialSlot = 300u;
    auto* mesh = CreateSingleMesh(kHighMaterialSlot);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("HighMaterialSlotPrimitive");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);

    NLS::Array<std::string> materialPaths;
    materialPaths.resize(static_cast<size_t>(kHighMaterialSlot) + 1u);
    materialPaths[kHighMaterialSlot] = materialPath;
    meshRenderer->SetMaterialPathHints(materialPaths);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    const auto stats = renderScene.Synchronize(scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({});

    EXPECT_EQ(stats.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.material, material);

    delete mesh;
    materialManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(RenderSceneCacheTests, ExplicitMaterialPathSuppressesDefaultMaterialUntilResolved)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b");
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
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/5a7b08f376165e7a24bca5d6717735939d1a03d386d31f76adac4e723db8b863");
    const auto texturePath = std::string("Library/Artifacts/Preview/textures/a55ac3737664a81088f904ab71979adda6090a6fae2b5bcf829cae2cbba29c37");
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

    auto* texture = CreateReadyTestTexture();
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

TEST(RenderSceneCacheTests, ExplicitMaterialTexturePathWithInvalidBytesDoesNotThrowDuringSync)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/textured-invalid-path.nmat");
    const std::string invalidTexturePath("Library/Artifacts/Preview/textures/body-\xff-diffuse.ntex", 58u);
    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    const_cast<std::string&>(material.path) = materialPath;
    material.SetTextureResourcePath("u_DiffuseMap", invalidTexturePath);

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("InvalidTexturePathPrefabInstance");
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

    EXPECT_NO_THROW({
        const auto stats = renderScene.Synchronize(scene, syncOptions);
        EXPECT_EQ(stats.rebuiltCachedCommandCount, 0u);
    });
    EXPECT_TRUE(renderScene.GatherVisibleCommands({}).opaques.empty());

    delete mesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
}

TEST(RenderSceneCacheTests, ExistingSceneExplicitMaterialPathRemainsVisibleWhileDeclaredTexturesArePending)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Existing/5a7b08f376165e7a24bca5d6717735939d1a03d386d31f76adac4e723db8b863");
    const auto texturePath = std::string("Library/Artifacts/Existing/textures/a55ac3737664a81088f904ab71979adda6090a6fae2b5bcf829cae2cbba29c37");
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
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/0c44be14dbdba8fbe4d726025cef9ade7d746f4deca0b6e540432b19191431cd");
    const auto texturePath = std::string("Library/Artifacts/Preview/textures/e7c47c17b703b2a444d73ae9c06978515fdc721130cc112034cc10a16ebe9185");
    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    const_cast<std::string&>(material.path) = materialPath;
    material.SetTextureResourcePath("u_DiffuseMap", texturePath);

    auto* staleTexture = CreateReadyTestTexture();
    ASSERT_NE(staleTexture, nullptr);
    staleTexture->path = texturePath;
    material.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", staleTexture);

    auto* texture = CreateReadyTestTexture();
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
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(staleTexture));
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
}

TEST(RenderSceneCacheTests, ExplicitMaterialPathRejectsManagerTextureWithoutGpuHandle)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/null-handle-material.nmat");
    const auto texturePath = std::string("Library/Artifacts/Preview/textures/null-handle-normal.ntex");
    auto* material = new NLS::Render::Resources::Material();
    material->SetShader(shader);
    material->path = materialPath;
    material->SetTextureResourcePath("_NormalMap", texturePath);
    materialManager.RegisterResource(materialPath, material);

    auto texture = NLS::Render::Resources::Texture2D::WrapExternal(nullptr, 1u, 1u);
    ASSERT_NE(texture, nullptr);
    texture->path = texturePath;
    auto* registeredTexture = textureManager.RegisterResource(texturePath, texture.release());
    ASSERT_NE(registeredTexture, nullptr);
    material->Set<NLS::Render::Resources::Texture2D*>("_NormalMap", registeredTexture);
    EXPECT_FALSE(NLS::Engine::Rendering::HasResolvedDeclaredMaterialTexturesForTesting(*material));

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("NullHandleDeclaredTexture");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshRenderer->SetMaterialPathHints({materialPath});
    meshRenderer->SetResolvedMaterialFromReference(0u, *material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;
    syncOptions.requireExplicitMaterialTextures = true;

    const auto stats = renderScene.Synchronize(scene, syncOptions);
    EXPECT_EQ(stats.rebuiltCachedCommandCount, 0u);
    EXPECT_TRUE(renderScene.GatherVisibleCommands({}).opaques.empty());

    delete mesh;
    textureManager.UnloadResources();
    materialManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
}

TEST(RenderSceneCacheTests, TextureManagerRequestsReplacementForCachedTextureWithoutGpuHandle)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_replace_null_handle_texture_" + NLS::Guid::New().ToString()));
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    const auto texturePath = ContentAddressedArtifactPath(root.Path(), "replace-null-handle:texture");
    WriteTextureArtifact(texturePath);

    NLS::Core::ResourceManagement::TextureManager textureManager;
    auto texture = NLS::Render::Resources::Texture2D::WrapExternal(nullptr, 1u, 1u);
    ASSERT_NE(texture, nullptr);
    texture->path = texturePath.string();
    textureManager.RegisterResource(texturePath.string(), texture.release());

    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), false), nullptr);
    EXPECT_TRUE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()));

    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, ExplicitMaterialPathRebindsDeclaredTextureWhenItArrivesAfterFirstSync)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/textured-late-body.nmat");
    const auto texturePath = std::string("Library/Artifacts/Preview/textures/late-body-diffuse.ntex");
    auto* material = new NLS::Render::Resources::Material();
    material->SetShader(shader);
    material->path = materialPath;
    material->SetTextureResourcePath("u_DiffuseMap", texturePath);
    materialManager.RegisterResource(materialPath, material);

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("MaterialTextureArrivesLate");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshRenderer->SetMaterialPathHints({ materialPath });
    meshRenderer->SetResolvedMaterialFromReference(0u, *material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    const auto firstSync = renderScene.Synchronize(scene, syncOptions);
    EXPECT_EQ(firstSync.rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(renderScene.GatherVisibleCommands({}).opaques.size(), 1u);
    const auto* firstParameter = material->GetParameterBlock().TryGet("u_DiffuseMap");
    ASSERT_NE(firstParameter, nullptr);
    ASSERT_EQ(firstParameter->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*firstParameter), nullptr);

    auto* texture = CreateReadyTestTexture();
    ASSERT_NE(texture, nullptr);
    texture->path = texturePath;
    textureManager.RegisterResource(texturePath, texture);

    const auto secondSync = renderScene.Synchronize(scene, syncOptions);
    EXPECT_EQ(secondSync.syncTouchedPrimitiveCount, 1u)
        << "A primitive with unresolved declared material textures must keep synchronizing until the texture cache can bind them.";
    EXPECT_EQ(secondSync.rebuiltCachedCommandCount, 1u);

    const auto* parameter = material->GetParameterBlock().TryGet("u_DiffuseMap");
    ASSERT_NE(parameter, nullptr);
    ASSERT_EQ(parameter->type(), typeid(NLS::Render::Resources::Texture2D*));
    EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter), texture);

    delete mesh;
    materialManager.UnloadResources();
    textureManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
}

TEST(RenderSceneCacheTests, ExplicitMaterialPathAcceptsEquivalentCachedDeclaredTexturePath)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_equivalent_texture_path_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";

    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto materialPath = std::string("Library/Artifacts/Preview/14e2231dbb294e9a4c2eee8ee1fd38020fbcf5d08eb69af2c2691123d65d1837");
    const auto texturePath = std::string("Library/Artifacts/Preview/textures/48cc145633ba31931db6c0d8f531d5abba0d4c24cdba065a88ae5620e691daa3");
    const auto absoluteTexturePath = NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);

    NLS::Render::Resources::Material material;
    material.SetShader(shader);
    const_cast<std::string&>(material.path) = materialPath;
    material.SetTextureResourcePath("u_DiffuseMap", texturePath);

    auto* texture = CreateReadyTestTexture();
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

TEST(RenderSceneCacheTests, SharedDeclaredTexturePathIsResolvedOncePerSync)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_shared_declared_texture_path_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";

    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    const auto texturePath = std::string("Library/Artifacts/Hero/shared/textures/body-basecolor.ntex");
    const auto absoluteTexturePath = NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);
    auto* texture = CreateReadyTestTexture();
    ASSERT_NE(texture, nullptr);
    texture->path = absoluteTexturePath;
    textureManager.RegisterResource(absoluteTexturePath, texture);

    constexpr size_t kMaterialCount = 48u;
    std::vector<std::unique_ptr<NLS::Render::Resources::Material>> materials;
    std::vector<NLS::Render::Resources::Mesh*> meshes;
    materials.reserve(kMaterialCount);
    meshes.reserve(kMaterialCount);

    NLS::Engine::SceneSystem::Scene scene;
    for (size_t index = 0u; index < kMaterialCount; ++index)
    {
        auto material = std::make_unique<NLS::Render::Resources::Material>();
        material->SetShader(shader);
        material->path = "Library/Artifacts/Hero/shared/materials/body-" + std::to_string(index) + ".nmat";
        material->SetTextureResourcePath("u_DiffuseMap", texturePath);
        ASSERT_TRUE(material->IsValid());

        auto* mesh = CreateSingleMesh(0u);
        ASSERT_NE(mesh, nullptr);
        meshes.push_back(mesh);

        auto& object = scene.CreateGameObject("SharedTextureMaterial" + std::to_string(index));
        auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        ASSERT_NE(meshFilter, nullptr);
        ASSERT_NE(meshRenderer, nullptr);
        meshFilter->SetMesh(mesh);
        meshRenderer->SetMaterialPathHints({ material->path });
        meshRenderer->SetResolvedMaterialFromReference(0u, *material);

        materials.push_back(std::move(material));
    }

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    const auto stats = renderScene.Synchronize(scene, syncOptions);

    EXPECT_EQ(stats.declaredTextureLookupCount, kMaterialCount);
    EXPECT_EQ(stats.declaredTextureCacheMissCount, 1u);
    EXPECT_EQ(stats.declaredTextureCacheHitCount, kMaterialCount - 1u);
    EXPECT_EQ(stats.declaredTextureResourceScanCount, 1u);
    EXPECT_EQ(renderScene.GatherVisibleCommands({}).opaques.size(), kMaterialCount);
    for (const auto& material : materials)
    {
        const auto* parameter = material->GetParameterBlock().TryGet("u_DiffuseMap");
        ASSERT_NE(parameter, nullptr);
        ASSERT_EQ(parameter->type(), typeid(NLS::Render::Resources::Texture2D*));
        EXPECT_EQ(std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter), texture);
    }

    for (auto* mesh : meshes)
        delete mesh;
    textureManager.UnloadResources();
    EXPECT_TRUE(NLS::Render::Resources::Loaders::ShaderLoader::Destroy(shader));
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::TextureManager>();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
}

TEST(RenderSceneCacheTests, ResourceManagersReturnEquivalentCachedArtifactsForAsyncRequests)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
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

    const auto materialPath = std::string("Library/Artifacts/Hero/000000000000d101");
    const auto texturePath = std::string("Library/Artifacts/Hero/000000000000d102");
    const auto absoluteMaterialPath =
        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(materialPath);
    const auto absoluteTexturePath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);

    auto* material = new NLS::Render::Resources::Material();
    material->path = absoluteMaterialPath;
    materialManager.RegisterResource(absoluteMaterialPath, material);

    auto* texture = CreateReadyTestTexture();
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

TEST(RenderSceneCacheTests, MaterialManagerMoveResourceDropsOldEquivalentArtifactPath)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_material_move_equivalent_artifact_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);

    NLS::Core::ResourceManagement::MaterialManager materialManager;

    const auto oldPath = std::string("Library/Artifacts/Hero/000000000000a001");
    const auto newPath = std::string("Library/Artifacts/Hero/000000000000a002");
    const auto absoluteOldPath =
        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(oldPath);
    const auto absoluteNewPath =
        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(newPath);

    auto* material = new NLS::Render::Resources::Material();
    material->path = oldPath;
    materialManager.RegisterResource(oldPath, material);

    ASSERT_EQ(materialManager.FindRegisteredMaterialByEquivalentArtifactPath(absoluteOldPath), material);
    ASSERT_TRUE(materialManager.MoveResource(oldPath, newPath));

    EXPECT_EQ(material->path, newPath);
    EXPECT_EQ(materialManager.FindRegisteredMaterialByEquivalentArtifactPath(absoluteOldPath), nullptr);
    EXPECT_EQ(materialManager.FindRegisteredMaterialByEquivalentArtifactPath(absoluteNewPath), material);

    materialManager.UnloadResources();
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
}

TEST(RenderSceneCacheTests, ResourceManagersTrackEquivalentPendingAsyncArtifactRequests)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();
    ScopedRenderSceneCacheJobSystem jobSystem(0u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_equivalent_pending_artifact_" + NLS::Guid::New().ToString()));
    const auto projectAssetsRoot = (root.Path() / "Project" / "Assets").string() + "/";
    const auto engineAssetsRoot = (root.Path() / "EngineAssets").string() + "/";
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;

    const auto materialPath = std::string("Library/Artifacts/Hero/000000000000d001");
    const auto texturePath = std::string("Library/Artifacts/Hero/000000000000d002");
    const auto absoluteMaterialPath =
        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(materialPath);
    const auto absoluteTexturePath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);
    WriteMaterialArtifact(absoluteMaterialPath);
    WriteTextureArtifact(absoluteTexturePath);

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

TEST(RenderSceneCacheTests, ResourceManagersScopePendingAsyncArtifactRequestsPerOwner)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to clear global async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();
    ScopedRenderSceneCacheJobSystem jobSystem(0u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_owner_scoped_pending_artifact_" + NLS::Guid::New().ToString()));
    const auto meshPath = ContentAddressedArtifactPath(root.Path(), "owner-scoped:mesh:hero");
    const auto materialPath = ContentAddressedArtifactPath(root.Path(), "owner-scoped:material:hero");
    const auto texturePath = ContentAddressedArtifactPath(root.Path(), "owner-scoped:texture:hero");
    WriteCubeMeshArtifact(meshPath);
    WriteMaterialArtifact(materialPath);
    WriteTextureArtifact(texturePath);

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    NLS::Core::ResourceManagement::MeshManager meshA;
    NLS::Core::ResourceManagement::MeshManager meshB;
    NLS::Core::ResourceManagement::MaterialManager materialA;
    NLS::Core::ResourceManagement::MaterialManager materialB;
    NLS::Core::ResourceManagement::TextureManager textureA;
    NLS::Core::ResourceManagement::TextureManager textureB;

    EXPECT_EQ(meshA.RequestAsyncArtifact(meshPath.string(), true), nullptr);
    EXPECT_EQ(meshB.RequestAsyncArtifact(meshPath.string(), true), nullptr);
    EXPECT_TRUE(meshA.IsAsyncArtifactLoadPending(meshPath.string()));
    EXPECT_TRUE(meshB.IsAsyncArtifactLoadPending(meshPath.string()))
        << "Preview and final prefab managers may request the same mesh artifact concurrently; one must not hide the other.";

    EXPECT_EQ(materialA.RequestAsyncArtifact(materialPath.string(), true), nullptr);
    EXPECT_EQ(materialB.RequestAsyncArtifact(materialPath.string(), true), nullptr);
    EXPECT_TRUE(materialA.IsAsyncArtifactLoadPending(materialPath.string()));
    EXPECT_TRUE(materialB.IsAsyncArtifactLoadPending(materialPath.string()))
        << "Material async requests are manager-owned even when their artifact paths are identical.";

    EXPECT_EQ(textureA.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    EXPECT_EQ(textureB.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    EXPECT_TRUE(textureA.IsAsyncArtifactLoadPending(texturePath.string()));
    EXPECT_TRUE(textureB.IsAsyncArtifactLoadPending(texturePath.string()))
        << "Texture async requests are manager-owned even when their artifact paths are identical.";

    for (size_t attempt = 0; attempt < 64u; ++attempt)
    {
        meshB.PumpAsyncLoads(8u);
        materialB.PumpAsyncLoads(8u);
        textureB.PumpAsyncLoads(8u);
        if (!meshB.IsAsyncArtifactLoadPending(meshPath.string()) &&
            !materialB.IsAsyncArtifactLoadPending(materialPath.string()) &&
            !textureB.IsAsyncArtifactLoadPending(texturePath.string()))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_FALSE(meshB.IsAsyncArtifactLoadPending(meshPath.string()));
    EXPECT_TRUE(meshA.IsAsyncArtifactLoadPending(meshPath.string()))
        << "Pumping one manager must not consume another manager's same-path mesh request.";
    EXPECT_FALSE(materialB.IsAsyncArtifactLoadPending(materialPath.string()));
    EXPECT_TRUE(materialA.IsAsyncArtifactLoadPending(materialPath.string()))
        << "Pumping one manager must not consume another manager's same-path material request.";
    EXPECT_FALSE(textureB.IsAsyncArtifactLoadPending(texturePath.string()));
    EXPECT_TRUE(textureA.IsAsyncArtifactLoadPending(texturePath.string()))
        << "Pumping one manager must not consume another manager's same-path texture request.";

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, ResourceManagersBoundPendingAsyncArtifactRequests)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_bounded_pending_artifact_" + NLS::Guid::New().ToString()));
    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    for (size_t index = 0u; index < 80u; ++index)
    {
        const auto meshPath = ContentAddressedArtifactPath(root.Path(), "bounded:mesh:" + std::to_string(index));
        WriteCubeMeshArtifact(meshPath);
        EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), true), nullptr);

        const auto texturePath = ContentAddressedArtifactPath(root.Path(), "bounded:texture:" + std::to_string(index));
        WriteTextureArtifact(texturePath);
        EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    }

    EXPECT_LE(NLS::Core::ResourceManagement::MeshManager::GetPendingAsyncArtifactRequestCountForTesting(), 8u);
    EXPECT_EQ(NLS::Core::ResourceManagement::TextureManager::GetMaxPendingAsyncArtifactRequestCountForTesting(), 32u);
    EXPECT_LE(
        NLS::Core::ResourceManagement::TextureManager::GetPendingAsyncArtifactRequestCountForTesting(),
        NLS::Core::ResourceManagement::TextureManager::GetMaxPendingAsyncArtifactRequestCountForTesting());
    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(ContentAddressedArtifactPath(root.Path(), "bounded:mesh:79").string()))
        << "Requests beyond the active async cap must stay observable as pending instead of looking like missing resources.";
    EXPECT_TRUE(textureManager.IsAsyncArtifactLoadPending(ContentAddressedArtifactPath(root.Path(), "bounded:texture:79").string()))
        << "Texture requests beyond the active async cap must stay observable as pending instead of looking like missing resources.";

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, ResourceManagersBoundTotalQueuedAsyncArtifactRequests)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_total_bounded_pending_artifact_" + NLS::Guid::New().ToString()));
    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    for (size_t index = 0u; index < 320u; ++index)
    {
        const auto meshPath = ContentAddressedArtifactPath(root.Path(), "total-bounded:mesh:" + std::to_string(index));
        WriteCubeMeshArtifact(meshPath);
        EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), true), nullptr);

        const auto texturePath = ContentAddressedArtifactPath(root.Path(), "total-bounded:texture:" + std::to_string(index));
        WriteTextureArtifact(texturePath);
        EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    }

    EXPECT_LE(NLS::Core::ResourceManagement::MeshManager::GetTotalAsyncArtifactRequestCountForTesting(), 256u)
        << "Large prefab drags must not enqueue an unbounded number of mesh artifact records.";
    EXPECT_LE(NLS::Core::ResourceManagement::TextureManager::GetTotalAsyncArtifactRequestCountForTesting(), 256u)
        << "Large prefab drags must not enqueue an unbounded number of texture artifact records.";

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, ResourceManagersBoundGlobalActiveAsyncArtifactRequestsAcrossOwners)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_global_active_pending_artifact_" + NLS::Guid::New().ToString()));
    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    std::vector<std::unique_ptr<NLS::Core::ResourceManagement::MeshManager>> meshManagers;
    std::vector<std::unique_ptr<NLS::Core::ResourceManagement::TextureManager>> textureManagers;
    meshManagers.reserve(80u);
    textureManagers.reserve(80u);

    for (size_t index = 0u; index < 80u; ++index)
    {
        auto meshManager = std::make_unique<NLS::Core::ResourceManagement::MeshManager>();
        const auto meshPath = ContentAddressedArtifactPath(root.Path(), "global-active:mesh:" + std::to_string(index));
        WriteCubeMeshArtifact(meshPath);
        EXPECT_EQ(meshManager->RequestAsyncArtifact(meshPath.string(), true), nullptr);
        meshManagers.push_back(std::move(meshManager));

        auto textureManager = std::make_unique<NLS::Core::ResourceManagement::TextureManager>();
        const auto texturePath = ContentAddressedArtifactPath(root.Path(), "global-active:texture:" + std::to_string(index));
        WriteTextureArtifact(texturePath);
        EXPECT_EQ(textureManager->RequestAsyncArtifact(texturePath.string(), true), nullptr);
        textureManagers.push_back(std::move(textureManager));
    }

    EXPECT_LE(NLS::Core::ResourceManagement::MeshManager::GetPendingAsyncArtifactRequestCountForTesting(), 8u)
        << "Mesh artifact loading must use a global active cap so many preview owners cannot fill the editor background queue.";
    EXPECT_EQ(NLS::Core::ResourceManagement::TextureManager::GetMaxPendingAsyncArtifactRequestCountForTesting(), 32u);
    EXPECT_LE(
        NLS::Core::ResourceManagement::TextureManager::GetPendingAsyncArtifactRequestCountForTesting(),
        NLS::Core::ResourceManagement::TextureManager::GetMaxPendingAsyncArtifactRequestCountForTesting())
        << "Texture artifact loading must use a global active cap so many preview owners cannot fill the editor background queue.";
    EXPECT_EQ(NLS::Core::ResourceManagement::MeshManager::GetTotalAsyncArtifactRequestCountForTesting(), 80u);
    EXPECT_EQ(NLS::Core::ResourceManagement::TextureManager::GetTotalAsyncArtifactRequestCountForTesting(), 80u);

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, CancelAsyncArtifactDoesNotConsumeSharedMeshOrTextureInterest)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_cancel_shared_interest_artifact_" + NLS::Guid::New().ToString()));
    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    const auto meshPath = ContentAddressedArtifactPath(root.Path(), "cancel-shared:mesh:hero");
    WriteCubeMeshArtifact(meshPath);
    NLS::Core::ResourceManagement::MeshManager meshManager;
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), false), nullptr);
    EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), true), nullptr);
    meshManager.CancelAsyncArtifact(meshPath.string());
    meshManager.CancelAsyncArtifact(meshPath.string());
    EXPECT_TRUE(meshManager.IsAsyncArtifactLoadPending(meshPath.string()))
        << "Preview cleanup must not cancel shared/final mesh interest by repeating path-only cancellation.";

    const auto texturePath = ContentAddressedArtifactPath(root.Path(), "cancel-shared:texture:hero");
    WriteTextureArtifact(texturePath);
    NLS::Core::ResourceManagement::TextureManager textureManager;
    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), false), nullptr);
    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    textureManager.CancelAsyncArtifact(texturePath.string());
    textureManager.CancelAsyncArtifact(texturePath.string());
    EXPECT_TRUE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()))
        << "Preview cleanup must not cancel shared/final texture interest by repeating path-only cancellation.";

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, TextureManagerCancelAsyncArtifactCanReleaseSharedInterest)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_cancel_explicit_shared_texture_" + NLS::Guid::New().ToString()));
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    NLS::Core::ResourceManagement::TextureManager textureManager;
    std::filesystem::path queuedTexturePath;
    for (size_t index = 0u; index < 80u; ++index)
    {
        const auto texturePath = ContentAddressedArtifactPath(
            root.Path(),
            "explicit-shared:texture:" + std::to_string(index));
        WriteTextureArtifact(texturePath);
        EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), false), nullptr);
        queuedTexturePath = texturePath;
    }

    ASSERT_TRUE(textureManager.IsAsyncArtifactLoadPending(queuedTexturePath.string()));
    textureManager.CancelAsyncArtifact(queuedTexturePath.string(), false);
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(queuedTexturePath.string()))
        << "Scene-load cleanup must be able to release shared texture interest that it registered.";

    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, TextureManagerCancellationRemainsVisibleDuringCompletion)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to synchronize async completion.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();
    ScopedRenderSceneCacheJobSystem jobSystem(1u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_cancel_completing_texture_" + NLS::Guid::New().ToString()));
    const auto texturePath = ContentAddressedArtifactPath(root.Path(), "cancel-completing:texture");
    WriteTextureArtifact(texturePath);

    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager textureManager;
    bool enteredCompletion = false;
    bool pendingDuringCompletion = false;
    NLS::Core::ResourceManagement::TextureManager::SetBeforeAsyncArtifactCompletionForTesting([&]()
    {
        enteredCompletion = true;
        pendingDuringCompletion = textureManager.IsAsyncArtifactLoadPending(texturePath.string());
        textureManager.CancelAsyncArtifact(texturePath.string(), true);
    });

    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    for (size_t attempt = 0u; attempt < 1000u && !enteredCompletion; ++attempt)
    {
        textureManager.PumpAsyncLoads(1u);
        if (!enteredCompletion)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    NLS::Core::ResourceManagement::TextureManager::SetBeforeAsyncArtifactCompletionForTesting({});
    EXPECT_TRUE(enteredCompletion);
    EXPECT_TRUE(pendingDuringCompletion)
        << "A request being converted into a runtime texture must remain visible to cancellation.";
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(texturePath.string()));
    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadFailed(texturePath.string()));
    EXPECT_EQ(textureManager.GetArtifactResource(texturePath.string(), false), nullptr);

    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, ClearingAsyncArtifactStateDrainsMeshAndTextureWorkersForTesting)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_clear_drains_workers_artifact_" + NLS::Guid::New().ToString()));
    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    for (size_t index = 0u; index < 8u; ++index)
    {
        const auto meshPath = ContentAddressedArtifactPath(root.Path(), "clear-drains:mesh:" + std::to_string(index));
        WriteCubeMeshArtifact(meshPath);
        EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), true), nullptr);

        const auto texturePath = ContentAddressedArtifactPath(root.Path(), "clear-drains:texture:" + std::to_string(index));
        WriteTextureArtifact(texturePath);
        EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    }

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
    EXPECT_TRUE(NLS::Core::ResourceManagement::MeshManager::WaitForAsyncArtifactWorkersForTesting());
    EXPECT_TRUE(NLS::Core::ResourceManagement::TextureManager::WaitForAsyncArtifactWorkersForTesting());
#endif
}

TEST(RenderSceneCacheTests, ResourceManagersBoundPendingAsyncArtifactRequestsPerOwner)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_owner_bounded_pending_artifact_" + NLS::Guid::New().ToString()));
    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    NLS::Core::ResourceManagement::MeshManager meshOwnerA;
    NLS::Core::ResourceManagement::MeshManager meshOwnerB;
    NLS::Core::ResourceManagement::MaterialManager materialOwnerA;
    NLS::Core::ResourceManagement::MaterialManager materialOwnerB;
    NLS::Core::ResourceManagement::TextureManager textureOwnerA;
    NLS::Core::ResourceManagement::TextureManager textureOwnerB;

    for (size_t index = 0u; index < 64u; ++index)
    {
        const auto meshPath = ContentAddressedArtifactPath(root.Path(), "owner-bounded:a:mesh:" + std::to_string(index));
        WriteCubeMeshArtifact(meshPath);
        EXPECT_EQ(meshOwnerA.RequestAsyncArtifact(meshPath.string(), true), nullptr);

        const auto materialPath = ContentAddressedArtifactPath(root.Path(), "owner-bounded:a:material:" + std::to_string(index));
        WriteMaterialArtifact(materialPath);
        EXPECT_EQ(materialOwnerA.RequestAsyncArtifact(materialPath.string(), true), nullptr);

        const auto texturePath = ContentAddressedArtifactPath(root.Path(), "owner-bounded:a:texture:" + std::to_string(index));
        WriteTextureArtifact(texturePath);
        EXPECT_EQ(textureOwnerA.RequestAsyncArtifact(texturePath.string(), true), nullptr);
    }

    const auto meshPathB = ContentAddressedArtifactPath(root.Path(), "owner-bounded:b:mesh:hero");
    const auto materialPathB = ContentAddressedArtifactPath(root.Path(), "owner-bounded:b:material:hero");
    const auto texturePathB = ContentAddressedArtifactPath(root.Path(), "owner-bounded:b:texture:hero");
    WriteCubeMeshArtifact(meshPathB);
    WriteMaterialArtifact(materialPathB);
    WriteTextureArtifact(texturePathB);

    EXPECT_EQ(meshOwnerB.RequestAsyncArtifact(meshPathB.string(), true), nullptr);
    EXPECT_TRUE(meshOwnerB.IsAsyncArtifactLoadPending(meshPathB.string()))
        << "A full preview/scene owner must not make another owner treat required mesh work as permanently unavailable.";

    EXPECT_EQ(materialOwnerB.RequestAsyncArtifact(materialPathB.string(), true), nullptr);
    EXPECT_TRUE(materialOwnerB.IsAsyncArtifactLoadPending(materialPathB.string()))
        << "A full preview/scene owner must not make another owner treat required material work as permanently unavailable.";

    EXPECT_EQ(textureOwnerB.RequestAsyncArtifact(texturePathB.string(), true), nullptr);
    EXPECT_TRUE(textureOwnerB.IsAsyncArtifactLoadPending(texturePathB.string()))
        << "A full preview/scene owner must not make another owner treat required texture work as permanently unavailable.";

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, DestroyedResourceManagersReleasePendingAsyncArtifactRequests)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect async request state.";
#else
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    const ScopedTempDirectory root(
        std::filesystem::temp_directory_path() /
        ("nullus_destroyed_owner_pending_artifact_" + NLS::Guid::New().ToString()));
    const auto meshPath = ContentAddressedArtifactPath(root.Path(), "destroyed-owner:mesh");
    const auto materialPath = ContentAddressedArtifactPath(root.Path(), "destroyed-owner:material");
    const auto texturePath = ContentAddressedArtifactPath(root.Path(), "destroyed-owner:texture");
    WriteCubeMeshArtifact(meshPath);
    WriteMaterialArtifact(materialPath);
    WriteTextureArtifact(texturePath);

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();

    {
        NLS::Core::ResourceManagement::MeshManager meshManager;
        NLS::Core::ResourceManagement::MaterialManager materialManager;
        NLS::Core::ResourceManagement::TextureManager textureManager;
        EXPECT_EQ(meshManager.RequestAsyncArtifact(meshPath.string(), true), nullptr);
        EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath.string(), true), nullptr);
        EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath.string(), true), nullptr);
        ASSERT_EQ(NLS::Core::ResourceManagement::MeshManager::GetTotalAsyncArtifactRequestCountForTesting(), 1u);
        ASSERT_EQ(NLS::Core::ResourceManagement::MaterialManager::GetTotalAsyncArtifactRequestCountForTesting(), 1u);
        ASSERT_EQ(NLS::Core::ResourceManagement::TextureManager::GetTotalAsyncArtifactRequestCountForTesting(), 1u);
    }

    EXPECT_EQ(NLS::Core::ResourceManagement::MeshManager::GetTotalAsyncArtifactRequestCountForTesting(), 0u);
    EXPECT_EQ(NLS::Core::ResourceManagement::MaterialManager::GetTotalAsyncArtifactRequestCountForTesting(), 0u);
    EXPECT_EQ(NLS::Core::ResourceManagement::TextureManager::GetTotalAsyncArtifactRequestCountForTesting(), 0u);

    NLS::Core::ResourceManagement::MeshManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::MaterialManager::ClearAsyncArtifactRequestStateForTesting();
    NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
#endif
}

TEST(RenderSceneCacheTests, MissingMaterialPathStillUsesDefaultMaterial)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

TEST(RenderSceneCacheTests, ExplicitMaterialPathUsesDefaultMaterialOnlyWhenFallbackEnabled)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    EnsureRenderSceneTestDriver();

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
    ASSERT_NE(shader, nullptr);

    NLS::Render::Resources::Material defaultMaterial;
    defaultMaterial.SetShader(shader);
    ASSERT_TRUE(defaultMaterial.IsValid());

    auto* mesh = CreateSingleMesh(0u);
    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("PendingMaterialPath");
    auto* meshFilter = actor.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = actor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetMesh(mesh);
    meshRenderer->SetMaterialPaths({"Library/ArtifactDB/MissingPendingSceneLoadMaterial.mat"});

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &defaultMaterial;

    EXPECT_EQ(renderScene.Synchronize(scene, syncOptions).rebuiltCachedCommandCount, 0u);
    EXPECT_TRUE(renderScene.GatherVisibleCommands({}).opaques.empty());

    syncOptions.allowDefaultMaterialForUnresolvedExplicitMaterials = true;
    EXPECT_EQ(renderScene.Synchronize(scene, syncOptions).rebuiltCachedCommandCount, 1u);
    const auto visible = renderScene.GatherVisibleCommands({});

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

TEST(RenderSceneCacheTests, TransientRenderSuppressionHidesMeshWithoutChangingGameObjectActiveState)
{
    RenderableFixture fixture;
    ASSERT_NE(fixture.meshRenderer, nullptr);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 1u);
    auto visible = renderScene.GatherVisibleCommands({});
    ASSERT_EQ(visible.opaques.size(), 1u);

    auto* owner = fixture.meshRenderer->gameobject();
    ASSERT_NE(owner, nullptr);
    ASSERT_TRUE(owner->IsSelfActive());
    fixture.meshRenderer->SetTransientRenderingSuppressed(true);
    EXPECT_TRUE(owner->IsSelfActive())
        << "Render-only suppression must not mutate serialized GameObject active state.";

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 0u)
        << "Render-only suppression should skip retained primitive synchronization.";
    visible = renderScene.GatherVisibleCommands({});
    EXPECT_TRUE(visible.opaques.empty());

    fixture.meshRenderer->SetTransientRenderingSuppressed(false);
    EXPECT_TRUE(owner->IsSelfActive());

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 1u)
        << "Restoring render-only suppression should rebuild the retained command after the primitive was skipped.";
    visible = renderScene.GatherVisibleCommands({});
    ASSERT_EQ(visible.opaques.size(), 1u);
}

TEST(RenderSceneCacheTests, TransientRenderSuppressionRemovesPrimitiveFromRetainedSyncSet)
{
    RenderableFixture fixture;
    ASSERT_NE(fixture.meshRenderer, nullptr);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 1u);
    ASSERT_EQ(renderScene.GetPrimitiveCount(), 1u);

    fixture.meshRenderer->SetTransientRenderingSuppressed(true);

    const auto suppressedStats = renderScene.Synchronize(fixture.scene, syncOptions);
    EXPECT_EQ(suppressedStats.syncTouchedPrimitiveCount, 0u);
    EXPECT_EQ(suppressedStats.removedPrimitiveCount, 1u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u);
    EXPECT_TRUE(renderScene.GatherVisibleCommands({}).opaques.empty());

    const auto steadySuppressedStats = renderScene.Synchronize(fixture.scene, syncOptions);
    EXPECT_EQ(steadySuppressedStats.syncTouchedPrimitiveCount, 0u);
    EXPECT_EQ(steadySuppressedStats.removedPrimitiveCount, 0u);
    EXPECT_EQ(steadySuppressedStats.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 0u);

    fixture.meshRenderer->SetTransientRenderingSuppressed(false);

    const auto restoredStats = renderScene.Synchronize(fixture.scene, syncOptions);
    EXPECT_EQ(restoredStats.addedPrimitiveCount, 1u);
    EXPECT_EQ(restoredStats.rebuiltCachedCommandCount, 1u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), 1u);
    EXPECT_EQ(renderScene.GatherVisibleCommands({}).opaques.size(), 1u);
}

TEST(RenderSceneCacheTests, SerialAndParallelVisibilityProduceEquivalentQueues)
{
    ScopedRenderSceneCacheJobSystem jobSystem(2u);
    ASSERT_TRUE(jobSystem.IsInitialized());

    ManyPrimitiveFixture fixture(192u, true);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 192u);

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::LargeSceneSettings largeSceneSettings;
    largeSceneSettings.enableSpatialIndex = false;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    visibilityOptions.largeSceneSettings = &largeSceneSettings;

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
    EXPECT_EQ(serialQueues.decals.size(), parallelQueues.decals.size());
    EXPECT_EQ(serialQueues.transparents.size(), parallelQueues.transparents.size());
    EXPECT_FALSE(serialQueues.decals.empty());
    EXPECT_FALSE(serialQueues.transparents.empty());
    EXPECT_EQ(ExtractMeshes(serialQueues), ExtractMeshes(parallelQueues));
}

TEST(RenderSceneCacheTests, AutoVisibilityStaysSerialUntilPersistentJobSystemExists)
{
    ManyPrimitiveFixture fixture(1152u);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 1152u);

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};

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
    fixture.meshRenderer->gameobject()->GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});

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

TEST(RenderSceneCacheTests, PickablePrimitiveDrawSourcesUseLastVisibleHandles)
{
    RenderableFixture fixture;
    auto* farMesh = CreateFarMesh();

    auto& farObject = fixture.scene.CreateGameObject("FarPickableNode");
    auto* farMeshFilter = farObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* farMeshRenderer = farObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(farMeshFilter, nullptr);
    ASSERT_NE(farMeshRenderer, nullptr);
    farMeshFilter->SetMesh(farMesh);
    farMeshRenderer->FillWithMaterial(fixture.material);

    fixture.meshRenderer->SetFrustumBehaviour(
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
    farMeshRenderer->SetFrustumBehaviour(
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
    fixture.meshRenderer->gameobject()->GetTransform()->SetWorldPosition({0.0f, 0.0f, -6.0f});

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    auto frustum = CreateForwardFrustum();
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};
    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(visible.opaques.size(), 1u);
    ASSERT_EQ(renderScene.GetLastVisiblePrimitiveHandles().size(), 1u);

    const auto sources = renderScene.CreatePickablePrimitiveDrawSourcesForHandles(
        renderScene.GetLastVisiblePrimitiveHandles());

    ASSERT_EQ(sources.size(), 1u);
    EXPECT_EQ(sources.front().owner, fixture.meshRenderer->gameobject());
    EXPECT_EQ(sources.front().mesh, fixture.mesh);
    EXPECT_EQ(sources.front().stateMask.mask, fixture.material.GenerateStateMask().mask);

    delete farMesh;
}

TEST(RenderSceneCacheTests, GatherVisibleCommandsAppliesImportedHierarchyHLODMetadata)
{
    RenderableFixture fixture;
    auto* childMeshB = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* proxyMesh = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 3.0f});

    auto& cluster = fixture.scene.CreateGameObject("ImportedCluster");
    cluster.SetSourceObjectKey("node/cluster");
    cluster.SetLargeSceneHLODMetadata(std::string(
        "{\"children\":[\"node/childA\",\"node/childB\"],"
        "\"clusterKey\":\"node/cluster\","
        "\"proxySubAssetKey\":\"hlod-proxy:node/cluster\","
        "\"source\":\"imported-hierarchy\"}"));

    auto* childA = fixture.meshRenderer->gameobject();
    ASSERT_NE(childA, nullptr);
    childA->SetName("ChildA");
    childA->SetParent(cluster);
    childA->SetSourceObjectKey("node/childA");

    auto& childB = fixture.scene.CreateGameObject("ChildB");
    childB.SetParent(cluster);
    childB.SetSourceObjectKey("node/childB");
    auto* childBFilter = childB.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* childBRenderer = childB.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(childBFilter, nullptr);
    ASSERT_NE(childBRenderer, nullptr);
    childBFilter->SetMesh(childMeshB);
    childBRenderer->FillWithMaterial(fixture.material);

    auto& proxy = fixture.scene.CreateGameObject("__HLODProxy_Cluster");
    proxy.SetParent(cluster);
    proxy.SetActive(false);
    proxy.SetSourceObjectKey("hlod-proxy:node/cluster");
    auto* proxyFilter = proxy.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* proxyRenderer = proxy.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(proxyFilter, nullptr);
    ASSERT_NE(proxyRenderer, nullptr);
    proxyFilter->SetMesh(proxyMesh);
    proxyRenderer->FillWithMaterial(fixture.material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    NLS::Engine::Rendering::LargeSceneSettings largeSceneSettings;
    largeSceneSettings.enableSpatialIndex = false;
    largeSceneSettings.enableHLOD = true;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {0.0f, 0.0f, 500.0f};
    visibilityOptions.largeSceneSettings = &largeSceneSettings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();

    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.mesh, proxyMesh);
    EXPECT_EQ(telemetry.culledByReason[
        static_cast<size_t>(NLS::Engine::Rendering::CullReason::HLODChildSuppressed)], 2u);
    EXPECT_EQ(telemetry.activeHLODClusterCount, 1u);

    delete childMeshB;
    delete proxyMesh;
}

TEST(RenderSceneCacheTests, ImportedHierarchyHLODUsesWorldBoundsForSelection)
{
    RenderableFixture fixture;
    auto* childMeshB = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* proxyMesh = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 3.0f});

    auto& cluster = fixture.scene.CreateGameObject("ImportedCluster");
    cluster.SetSourceObjectKey("node/cluster");
    cluster.SetLargeSceneHLODMetadata(std::string(
        "{\"children\":[\"node/childA\",\"node/childB\"],"
        "\"clusterKey\":\"node/cluster\","
        "\"proxySubAssetKey\":\"hlod-proxy:node/cluster\","
        "\"source\":\"imported-hierarchy\"}"));

    auto* childA = fixture.meshRenderer->gameobject();
    ASSERT_NE(childA, nullptr);
    childA->SetName("ChildA");
    childA->SetParent(cluster);
    childA->SetSourceObjectKey("node/childA");
    childA->GetTransform()->SetWorldPosition({-100.0f, 0.0f, 0.0f});

    auto& childB = fixture.scene.CreateGameObject("ChildB");
    childB.SetParent(cluster);
    childB.SetSourceObjectKey("node/childB");
    childB.GetTransform()->SetWorldPosition({100.0f, 0.0f, 0.0f});
    auto* childBFilter = childB.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* childBRenderer = childB.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(childBFilter, nullptr);
    ASSERT_NE(childBRenderer, nullptr);
    childBFilter->SetMesh(childMeshB);
    childBRenderer->FillWithMaterial(fixture.material);

    auto& proxy = fixture.scene.CreateGameObject("__HLODProxy_Cluster");
    proxy.SetParent(cluster);
    proxy.SetActive(false);
    proxy.SetSourceObjectKey("hlod-proxy:node/cluster");
    auto* proxyFilter = proxy.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* proxyRenderer = proxy.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(proxyFilter, nullptr);
    ASSERT_NE(proxyRenderer, nullptr);
    proxyFilter->SetMesh(proxyMesh);
    proxyRenderer->FillWithMaterial(fixture.material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    NLS::Engine::Rendering::LargeSceneSettings largeSceneSettings;
    largeSceneSettings.enableSpatialIndex = false;
    largeSceneSettings.enableHLOD = true;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {0.0f, 0.0f, 500.0f};
    visibilityOptions.largeSceneSettings = &largeSceneSettings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();
    const auto meshes = ExtractMeshes(visible);

    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), fixture.mesh), meshes.end());
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), childMeshB), meshes.end());
    EXPECT_EQ(std::find(meshes.begin(), meshes.end(), proxyMesh), meshes.end());
    EXPECT_EQ(telemetry.culledByReason[
        static_cast<size_t>(NLS::Engine::Rendering::CullReason::HLODChildSuppressed)], 0u);
    EXPECT_EQ(telemetry.activeHLODClusterCount, 0u);

    delete childMeshB;
    delete proxyMesh;
}

TEST(RenderSceneCacheTests, EditorInspectionRootKeepsImportedHierarchyHLODChildrenVisible)
{
    RenderableFixture fixture;
    auto* childMeshB = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* proxyMesh = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 3.0f});

    auto& cluster = fixture.scene.CreateGameObject("ImportedCluster");
    cluster.SetSourceObjectKey("node/cluster");
    cluster.SetLargeSceneHLODMetadata(std::string(
        "{\"children\":[\"node/childA\",\"node/childB\"],"
        "\"clusterKey\":\"node/cluster\","
        "\"proxySubAssetKey\":\"hlod-proxy:node/cluster\","
        "\"source\":\"imported-hierarchy\"}"));

    auto* childA = fixture.meshRenderer->gameobject();
    ASSERT_NE(childA, nullptr);
    childA->SetName("ChildA");
    childA->SetParent(cluster);
    childA->SetSourceObjectKey("node/childA");

    auto& childB = fixture.scene.CreateGameObject("ChildB");
    childB.SetParent(cluster);
    childB.SetSourceObjectKey("node/childB");
    auto* childBFilter = childB.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* childBRenderer = childB.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(childBFilter, nullptr);
    ASSERT_NE(childBRenderer, nullptr);
    childBFilter->SetMesh(childMeshB);
    childBRenderer->FillWithMaterial(fixture.material);

    auto& proxy = fixture.scene.CreateGameObject("__HLODProxy_Cluster");
    proxy.SetParent(cluster);
    proxy.SetActive(false);
    proxy.SetSourceObjectKey("hlod-proxy:node/cluster");
    auto* proxyFilter = proxy.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* proxyRenderer = proxy.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(proxyFilter, nullptr);
    ASSERT_NE(proxyRenderer, nullptr);
    proxyFilter->SetMesh(proxyMesh);
    proxyRenderer->FillWithMaterial(fixture.material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    NLS::Engine::Rendering::LargeSceneSettings largeSceneSettings;
    largeSceneSettings.enableSpatialIndex = false;
    largeSceneSettings.enableHLOD = true;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {0.0f, 0.0f, 500.0f};
    visibilityOptions.largeSceneSettings = &largeSceneSettings;
    visibilityOptions.editorInspectionView = true;
    visibilityOptions.inspectionRootObjects = { &cluster };

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();
    const auto meshes = ExtractMeshes(visible);

    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), fixture.mesh), meshes.end());
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), childMeshB), meshes.end());
    EXPECT_EQ(std::find(meshes.begin(), meshes.end(), proxyMesh), meshes.end());
    EXPECT_EQ(telemetry.culledByReason[
        static_cast<size_t>(NLS::Engine::Rendering::CullReason::HLODChildSuppressed)], 0u);
    EXPECT_EQ(telemetry.activeHLODClusterCount, 0u);

    delete childMeshB;
    delete proxyMesh;
}

TEST(RenderSceneCacheTests, ImportedHierarchyHLODMetadataScopesRepeatedSourceKeysPerInstance)
{
    RenderableFixture fixture;
    auto* childMeshB0 = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* childMeshA1 = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* childMeshB1 = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* proxyMesh0 = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 3.0f});
    auto* proxyMesh1 = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 3.0f});

    const std::string metadata =
        "{\"children\":[\"node/childA\",\"node/childB\"],"
        "\"clusterKey\":\"node/cluster\","
        "\"proxySubAssetKey\":\"hlod-proxy:node/cluster\","
        "\"source\":\"imported-hierarchy\"}";

    auto addRenderable = [&](NLS::Engine::GameObject& object, NLS::Render::Resources::Mesh& mesh)
    {
        auto* meshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        EXPECT_NE(meshFilter, nullptr);
        EXPECT_NE(meshRenderer, nullptr);
        meshFilter->SetMesh(&mesh);
        meshRenderer->FillWithMaterial(fixture.material);
    };

    auto addInstance = [&](const char* suffix, NLS::Render::Resources::Mesh& childMeshA, NLS::Render::Resources::Mesh& childMeshB, NLS::Render::Resources::Mesh& proxyMesh)
    {
        auto& cluster = fixture.scene.CreateGameObject(std::string("ImportedCluster") + suffix);
        cluster.SetSourceObjectKey("node/cluster");
        cluster.SetLargeSceneHLODMetadata(metadata);

        auto& childA = fixture.scene.CreateGameObject(std::string("ChildA") + suffix);
        childA.SetParent(cluster);
        childA.SetSourceObjectKey("node/childA");
        addRenderable(childA, childMeshA);

        auto& childB = fixture.scene.CreateGameObject(std::string("ChildB") + suffix);
        childB.SetParent(cluster);
        childB.SetSourceObjectKey("node/childB");
        addRenderable(childB, childMeshB);

        auto& proxy = fixture.scene.CreateGameObject(std::string("__HLODProxy_Cluster") + suffix);
        proxy.SetParent(cluster);
        proxy.SetActive(false);
        proxy.SetSourceObjectKey("hlod-proxy:node/cluster");
        addRenderable(proxy, proxyMesh);
    };

    fixture.meshRenderer->gameobject()->SetName("UnusedFixtureActor");
    fixture.meshRenderer->gameobject()->SetActive(false);
    addInstance("0", *fixture.mesh, *childMeshB0, *proxyMesh0);
    addInstance("1", *childMeshA1, *childMeshB1, *proxyMesh1);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 7u);

    NLS::Engine::Rendering::LargeSceneSettings largeSceneSettings;
    largeSceneSettings.enableSpatialIndex = false;
    largeSceneSettings.enableHLOD = true;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {0.0f, 0.0f, 500.0f};
    visibilityOptions.largeSceneSettings = &largeSceneSettings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);
    const auto telemetry = renderScene.GetLastLargeSceneTelemetryForTesting();
    const auto meshes = ExtractMeshes(visible);

    ASSERT_EQ(meshes.size(), 2u);
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), proxyMesh0), meshes.end());
    EXPECT_NE(std::find(meshes.begin(), meshes.end(), proxyMesh1), meshes.end());
    EXPECT_EQ(telemetry.culledByReason[
        static_cast<size_t>(NLS::Engine::Rendering::CullReason::HLODChildSuppressed)], 4u);
    EXPECT_EQ(telemetry.activeHLODClusterCount, 2u);

    delete childMeshB0;
    delete childMeshA1;
    delete childMeshB1;
    delete proxyMesh0;
    delete proxyMesh1;
}

TEST(RenderSceneCacheTests, SynchronizeKeepsManuallyRegisteredHLODClusters)
{
    RenderableFixture fixture;
    auto* childMeshB = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 1.0f});
    auto* proxyMesh = CreateTriangleMesh(0u, {{0.0f, 0.0f, 0.0f}, 3.0f});

    auto& childB = fixture.scene.CreateGameObject("ManualChildB");
    auto* childBFilter = childB.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* childBRenderer = childB.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(childBFilter, nullptr);
    ASSERT_NE(childBRenderer, nullptr);
    childBFilter->SetMesh(childMeshB);
    childBRenderer->FillWithMaterial(fixture.material);

    auto& proxy = fixture.scene.CreateGameObject("ManualProxy");
    auto* proxyFilter = proxy.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* proxyRenderer = proxy.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(proxyFilter, nullptr);
    ASSERT_NE(proxyRenderer, nullptr);
    proxyFilter->SetMesh(proxyMesh);
    proxyRenderer->FillWithMaterial(fixture.material);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.material;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);
    const auto snapshot = renderScene.CreatePrimitiveSnapshotForTesting();
    std::vector<NLS::Engine::Rendering::ScenePrimitiveHandle> childHandles;
    std::optional<NLS::Engine::Rendering::ScenePrimitiveHandle> proxyHandle;
    for (const auto& record : snapshot.primitiveRecords)
    {
        if (record.mesh == proxyMesh)
            proxyHandle = record.handle;
        else
            childHandles.push_back(record.handle);
    }
    ASSERT_EQ(childHandles.size(), 2u);
    ASSERT_TRUE(proxyHandle.has_value());

    NLS::Engine::Rendering::HLODClusterRecord cluster;
    cluster.childPrimitives = childHandles;
    cluster.proxyPrimitive = *proxyHandle;
    cluster.worldReferencePoint = {};
    cluster.worldSize = 2.0f;
    cluster.activationScreenRelativeSize = 0.03f;
    cluster.compatibilityFlags =
        NLS::Engine::Rendering::HLODCompatibilityFlags::OpaqueOnly |
        NLS::Engine::Rendering::HLODCompatibilityFlags::ProxySafe;
    renderScene.RegisterHLODClusterForTesting(cluster);

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 0u);

    NLS::Engine::Rendering::LargeSceneSettings largeSceneSettings;
    largeSceneSettings.enableSpatialIndex = false;
    largeSceneSettings.enableHLOD = true;
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = {0.0f, 0.0f, 500.0f};
    visibilityOptions.largeSceneSettings = &largeSceneSettings;

    const auto visible = renderScene.GatherVisibleCommands(
        visibilityOptions,
        NLS::Engine::Rendering::RenderSceneVisibilityMode::Serial);

    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(visible.opaques.front().second.mesh, proxyMesh);

    delete childMeshB;
    delete proxyMesh;
}

TEST(RenderSceneCacheTests, OpaqueQueueGroupsCompatibleStateAndTransparentKeepsBackToFront)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    QueueSortFixture fixture;
    fixture.AddObject("OpaqueNearA", *fixture.sharedMesh, fixture.opaqueMaterialA, 5.0f);
    fixture.AddObject("OpaqueMiddleB", *fixture.otherMesh, fixture.opaqueMaterialB, 10.0f);
    fixture.AddObject("OpaqueFarA", *fixture.sharedMesh, fixture.opaqueMaterialA, 20.0f);
    fixture.AddObject("DecalNear", *fixture.sharedMesh, fixture.decalMaterial, 4.0f);
    fixture.AddObject("DecalFar", *fixture.sharedMesh, fixture.decalMaterial, 40.0f);
    fixture.AddObject("TransparentNear", *fixture.sharedMesh, fixture.transparentMaterial, 3.0f);
    fixture.AddObject("TransparentFar", *fixture.sharedMesh, fixture.transparentMaterial, 30.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 7u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });

    ASSERT_EQ(visible.opaques.size(), 2u);
    const auto firstInstances = ResolveVisibleInstanceCount(visible.opaques[0].second);
    const auto secondInstances = ResolveVisibleInstanceCount(visible.opaques[1].second);
    EXPECT_TRUE(
        (visible.opaques[0].second.mesh == fixture.sharedMesh && firstInstances == 2u) ||
        (visible.opaques[1].second.mesh == fixture.sharedMesh && secondInstances == 2u));

    ASSERT_EQ(visible.decals.size(), 2u);
    EXPECT_GT(visible.decals[0].first, visible.decals[1].first);

    ASSERT_EQ(visible.transparents.size(), 2u);
    EXPECT_GT(visible.transparents[0].first, visible.transparents[1].first);
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

TEST(RenderSceneCacheTests, SceneRendererKeepsDecalsBackToFrontAcrossAdditiveScenes)
{
    auto& driver = EnsureRenderSceneTestDriver();
    QueueSortFixture fixture;
    fixture.AddObject("MainDecalNear", *fixture.sharedMesh, fixture.decalMaterial, 3.0f);

    NLS::Engine::SceneSystem::Scene previewScene;
    auto& previewObject = previewScene.CreateGameObject("PreviewDecalFar");
    auto* previewMeshFilter = previewObject.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* previewMeshRenderer = previewObject.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(previewMeshFilter, nullptr);
    ASSERT_NE(previewMeshRenderer, nullptr);
    previewMeshFilter->SetMesh(fixture.otherMesh);
    previewMeshRenderer->FillWithMaterial(fixture.decalMaterial);
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

    ASSERT_EQ(drawables.decals.size(), 2u);
    EXPECT_EQ(drawables.decals[0].second.mesh, fixture.otherMesh);
    EXPECT_EQ(drawables.decals[1].second.mesh, fixture.sharedMesh);
    EXPECT_GT(drawables.decals[0].first, drawables.decals[1].first);
}

TEST(RenderSceneCacheTests, SceneRendererDrawsExistingAndPreviewPrefabInstancesSharingAssetReferences)
{
    using namespace NLS::Engine::Serialize;

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    auto& driver = EnsureRenderSceneTestDriver();

    const auto meshGuid = NLS::Guid::Parse("24242424-2424-4424-8424-242424242424");
    const auto materialGuid = NLS::Guid::Parse("35353535-3535-4535-8535-353535353535");
    const auto meshPath = std::string("Library/Artifacts/Hero/shared/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");
    const auto materialPath = std::string("Library/Artifacts/Hero/shared/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b");
    const auto meshReference = ObjectIdentifier::Asset(
        AssetId(meshGuid),
        MakeLocalIdentifierInFile(meshGuid, "mesh:body"),
        meshPath);
    const auto materialReference = ObjectIdentifier::Asset(
        AssetId(materialGuid),
        MakeLocalIdentifierInFile(materialGuid, "material:body"),
        materialPath);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

TEST(RenderSceneCacheTests, AdditivePreviewSceneSkipsTexturedModelUntilMaterialTexturesAreBound)
{
    using namespace NLS::Engine::Serialize;

    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    auto& driver = EnsureRenderSceneTestDriver();

    const auto meshGuid = NLS::Guid::Parse("25252525-2525-4525-8525-252525252525");
    const auto materialGuid = NLS::Guid::Parse("36363636-3636-4636-8636-363636363636");
    const auto meshPath = std::string("Library/Artifacts/Hero/preview/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb");
    const auto materialPath = std::string("Library/Artifacts/Hero/preview/8ca977f3a8a054ff6767e381b334be9e47456f725e02f84e11a3b5b1f3f4218b");
    const auto texturePath = std::string("Library/Artifacts/Hero/preview/86e6c89ab4ea41e95f16e80c3e267f36ba9a256daf79cb4d867ba2e0fdf555cf");
    const auto meshReference = ObjectIdentifier::Asset(
        AssetId(meshGuid),
        MakeLocalIdentifierInFile(meshGuid, "mesh:body"),
        meshPath);
    const auto materialReference = ObjectIdentifier::Asset(
        AssetId(materialGuid),
        MakeLocalIdentifierInFile(materialGuid, "material:body"),
        materialPath);

    auto* shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Standard.hlsl");
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

    auto previewMesh = std::shared_ptr<NLS::Render::Resources::Mesh>(CreateSingleMesh(0u));
    ASSERT_NE(previewMesh, nullptr);
    NLS::Render::Resources::Material previewMaterial;
    previewMaterial.path = materialPath;
    previewMaterial.SetShader(shader);
    previewMaterial.SetTextureResourcePath("u_DiffuseMap", texturePath);
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
    previewMeshRenderer->SetMaterialPathHints({ materialPath });
    previewMeshRenderer->SetResolvedMaterialFromReference(0u, previewMaterial);

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

    const auto texturePendingDrawables = renderer.CaptureSceneDrawables(frameDescriptor);
    ASSERT_EQ(texturePendingDrawables.opaques.size(), 1u)
        << "The saved scene must remain visible, but the additive preview must not render as a white model while its declared texture is missing.";
    EXPECT_EQ(texturePendingDrawables.opaques.front().second.mesh, existingMesh);
    EXPECT_EQ(texturePendingDrawables.opaques.front().second.material, &existingMaterial);

    auto* texture = CreateReadyTestTexture();
    ASSERT_NE(texture, nullptr);
    texture->path = texturePath;
    previewMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", texture);

    const auto textureReadyDrawables = renderer.CaptureSceneDrawables(frameDescriptor);
    ASSERT_EQ(textureReadyDrawables.opaques.size(), 2u);
    const auto drewPreview = std::any_of(
        textureReadyDrawables.opaques.begin(),
        textureReadyDrawables.opaques.end(),
        [&previewMesh, &previewMaterial](const auto& entry)
        {
            return entry.second.mesh == previewMesh.get() && entry.second.material == &previewMaterial;
        });
    EXPECT_TRUE(drewPreview);

    delete existingMesh;
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
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
}

TEST(RenderSceneCacheTests, DynamicInstancingMergesCompatibleOpaqueCommandsIntoObjectIndexRange)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
    EXPECT_EQ(
        descriptor.objectFlags,
        NLS::Render::Data::kDrawableObjectFlagReceiveShadows |
            NLS::Render::Data::kDrawableObjectFlagCastShadows);
    ASSERT_EQ(descriptor.instanceModelMatrices.size(), 3u);
    EXPECT_FLOAT_EQ(descriptor.instanceModelMatrices[0].data[3], 4.0f);
    EXPECT_FLOAT_EQ(descriptor.instanceModelMatrices[1].data[3], 12.0f);
    EXPECT_FLOAT_EQ(descriptor.instanceModelMatrices[2].data[3], 24.0f);
}

TEST(RenderSceneCacheTests, OpaqueInstanceOrderRemainsStableWhenCameraMoves)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    QueueSortFixture fixture;
    fixture.AddObject("StableFirst", *fixture.sharedMesh, fixture.opaqueMaterialA, 3.0f);
    fixture.AddObject("StableSecond", *fixture.sharedMesh, fixture.opaqueMaterialA, 1.0f);
    fixture.AddObject("StableThird", *fixture.sharedMesh, fixture.opaqueMaterialA, 2.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.cameraPosition = { 0.0f, 0.0f, 0.0f };
    const auto firstVisible = renderScene.GatherVisibleCommands(visibilityOptions);
    visibilityOptions.cameraPosition = { 10.0f, 0.0f, 0.0f };
    const auto secondVisible = renderScene.GatherVisibleCommands(visibilityOptions);

    ASSERT_EQ(firstVisible.opaques.size(), 1u);
    ASSERT_EQ(secondVisible.opaques.size(), 1u);

    NLS::Engine::Rendering::EngineDrawableDescriptor firstDescriptor;
    NLS::Engine::Rendering::EngineDrawableDescriptor secondDescriptor;
    ASSERT_TRUE(firstVisible.opaques.front().second.TryGetDescriptor(firstDescriptor));
    ASSERT_TRUE(secondVisible.opaques.front().second.TryGetDescriptor(secondDescriptor));
    ASSERT_EQ(firstDescriptor.instanceModelMatrices.size(), 3u);
    ASSERT_EQ(secondDescriptor.instanceModelMatrices.size(), 3u);
    for (size_t index = 0u; index < firstDescriptor.instanceModelMatrices.size(); ++index)
        EXPECT_FLOAT_EQ(
            firstDescriptor.instanceModelMatrices[index].data[3],
            secondDescriptor.instanceModelMatrices[index].data[3]);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
TEST(RenderSceneCacheTests, OpaqueSortKeepsStableAndDistanceFallbackKeysInStrictOrder)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    QueueSortFixture fixture;
    fixture.AddObject("StableHigh", *fixture.sharedMesh, fixture.opaqueMaterialA, 4.0f);
    fixture.AddObject("StableLow", *fixture.sharedMesh, fixture.opaqueMaterialA, 12.0f);
    fixture.AddObject("DistanceFallback", *fixture.sharedMesh, fixture.opaqueMaterialA, 24.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 3u);

    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    ASSERT_EQ(visible.opaques.size(), 1u);
    auto splitDrawables = ExpandMergedOpaqueForShadowFlagTest(visible.opaques.front());
    ASSERT_EQ(splitDrawables.size(), 3u);

    const std::array<uint64_t, 3u> stableSortKeys = {
        20u,
        10u,
        NLS::Engine::Rendering::EngineDrawableDescriptor::kInvalidStableSortKey
    };
    const std::array<float, 3u> distances = { 100.0f, 0.0f, 50.0f };
    for (size_t index = 0u; index < splitDrawables.size(); ++index)
    {
        auto* descriptor = splitDrawables[index].second.TryGetDescriptor<
            NLS::Engine::Rendering::EngineDrawableDescriptor>();
        ASSERT_NE(descriptor, nullptr);
        descriptor->stableSortKey = stableSortKeys[index];
        splitDrawables[index].first = distances[index];
    }

    renderScene.FinalizeOpaqueQueueForTesting(splitDrawables);

    ASSERT_EQ(splitDrawables.size(), 1u);
    NLS::Engine::Rendering::EngineDrawableDescriptor mergedDescriptor;
    ASSERT_TRUE(splitDrawables.front().second.TryGetDescriptor(mergedDescriptor));
    ASSERT_EQ(mergedDescriptor.instanceModelMatrices.size(), 3u);
    EXPECT_FLOAT_EQ(mergedDescriptor.instanceModelMatrices[0].data[3], 12.0f);
    EXPECT_FLOAT_EQ(mergedDescriptor.instanceModelMatrices[1].data[3], 4.0f);
    EXPECT_FLOAT_EQ(mergedDescriptor.instanceModelMatrices[2].data[3], 24.0f);
}

TEST(RenderSceneCacheTests, ShadowCastFlagDifferenceSplitsDynamicInstances)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    QueueSortFixture fixture;
    fixture.AddObject("CastA", *fixture.sharedMesh, fixture.opaqueMaterialA, 4.0f);
    fixture.AddObject("CastB", *fixture.sharedMesh, fixture.opaqueMaterialA, 8.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    ASSERT_EQ(visible.opaques.size(), 1u);
    auto splitDrawables = ExpandMergedOpaqueForShadowFlagTest(visible.opaques.front());
    ASSERT_EQ(splitDrawables.size(), 2u);

    NLS::Engine::Rendering::EngineDrawableDescriptor secondDescriptor;
    ASSERT_TRUE(splitDrawables[1].second.TryGetDescriptor(secondDescriptor));
    secondDescriptor.objectFlags &= ~NLS::Render::Data::kDrawableObjectFlagCastShadows;
    splitDrawables[1].second.RemoveDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>();
    splitDrawables[1].second.AddDescriptor(secondDescriptor);

    renderScene.FinalizeOpaqueQueueForTesting(splitDrawables);
    EXPECT_EQ(splitDrawables.size(), 2u);
}

TEST(RenderSceneCacheTests, ShadowReceiveFlagDifferenceSplitsDynamicInstances)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    QueueSortFixture fixture;
    fixture.AddObject("ReceiveA", *fixture.sharedMesh, fixture.opaqueMaterialA, 4.0f);
    fixture.AddObject("ReceiveB", *fixture.sharedMesh, fixture.opaqueMaterialA, 8.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);

    auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });
    ASSERT_EQ(visible.opaques.size(), 1u);
    auto splitDrawables = ExpandMergedOpaqueForShadowFlagTest(visible.opaques.front());
    ASSERT_EQ(splitDrawables.size(), 2u);

    NLS::Engine::Rendering::EngineDrawableDescriptor secondDescriptor;
    ASSERT_TRUE(splitDrawables[1].second.TryGetDescriptor(secondDescriptor));
    secondDescriptor.objectFlags &= ~NLS::Render::Data::kDrawableObjectFlagReceiveShadows;
    splitDrawables[1].second.RemoveDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>();
    splitDrawables[1].second.AddDescriptor(secondDescriptor);

    renderScene.FinalizeOpaqueQueueForTesting(splitDrawables);
    EXPECT_EQ(splitDrawables.size(), 2u);
}
#endif

TEST(RenderSceneCacheTests, DynamicInstancingKeepsSiblingVisibleAfterDeletingOneSharedPrefabLikeObject)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

    QueueSortFixture fixture;
    auto& first = fixture.AddObject("SharedPrefabA", *fixture.sharedMesh, fixture.opaqueMaterialA, 4.0f);
    fixture.AddObject("SharedPrefabB", *fixture.sharedMesh, fixture.opaqueMaterialA, 12.0f);

    NLS::Engine::Rendering::RenderScene renderScene;
    NLS::Engine::Rendering::RenderSceneSyncOptions syncOptions;
    syncOptions.defaultMaterial = &fixture.opaqueMaterialA;
    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 2u);
    ASSERT_EQ(renderScene.GatherVisibleCommands({ nullptr, {} }).opaques.size(), 1u);

    ASSERT_TRUE(fixture.scene.DestroyGameObject(first));
    const auto afterDelete = renderScene.Synchronize(fixture.scene, syncOptions);
    const auto visible = renderScene.GatherVisibleCommands({ nullptr, {} });

    EXPECT_EQ(afterDelete.removedPrimitiveCount, 1u);
    ASSERT_EQ(visible.opaques.size(), 1u)
        << "Deleting one instance from a dynamically-instanced prefab group must not drop the sibling draw.";
    EXPECT_EQ(visible.opaques.front().second.mesh, fixture.sharedMesh);
    EXPECT_EQ(ResolveVisibleInstanceCount(visible.opaques.front().second), 1u);
}

TEST(RenderSceneCacheTests, DynamicInstancingBuildsMergedDescriptorInLinearPass)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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

TEST(RenderSceneCacheTests, DynamicInstancingRejectsIncompatibleMeshMaterialAndTransparentCommands)
{
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
    auto* legacyShader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl("App/Assets/Engine/Shaders/Unlit.hlsl");
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
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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
    NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE();

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

#undef NLS_RENDER_SCENE_CACHE_SKIP_IF_NATIVE_DXC_UNAVAILABLE
