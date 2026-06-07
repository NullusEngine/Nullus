#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Jobs/JobSystem.h"
#include "Math/Matrix4.h"
#include "Rendering/Assets/MeshArtifact.h"
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
#include "Rendering/Resources/IndexedObjectDataShaderSupport.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Serialize/ObjectReferenceResolver.h"
#include "Serialize/PPtr.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "SceneSystem/Scene.h"

namespace
{
    std::optional<std::filesystem::path> FindNativeDxcExecutableForTests()
    {
        const auto tryPath = [](const std::filesystem::path& candidate) -> std::optional<std::filesystem::path>
        {
            std::error_code error;
            if (candidate.empty() || !std::filesystem::exists(candidate, error) || error)
                return std::nullopt;

            return candidate;
        };

        if (const char* envPath = std::getenv("DXC_PATH"); envPath != nullptr && *envPath != '\0')
        {
            if (auto found = tryPath(envPath))
                return found;
        }

        const std::vector<const char*> sdkVars = {"VULKAN_SDK", "VK_SDK_PATH"};
        for (const char* varName : sdkVars)
        {
            const char* envPath = std::getenv(varName);
            if (envPath == nullptr || *envPath == '\0')
                continue;

            const std::filesystem::path sdkRoot(envPath);
            const std::vector<std::filesystem::path> candidates = {
#if defined(_WIN32)
                sdkRoot / "Bin" / "dxc.exe",
                sdkRoot / "Bin" / "x64" / "dxc.exe"
#else
                sdkRoot / "bin" / "dxc",
                sdkRoot / "Bin" / "dxc",
                sdkRoot / "bin" / "x64" / "dxc"
#endif
            };

            for (const auto& candidate : candidates)
            {
                if (auto found = tryPath(candidate))
                    return found;
            }
        }

        const std::filesystem::path repoRoot(NLS_ROOT_DIR);
        const std::vector<std::filesystem::path> bundledCandidates = {
#if defined(_WIN32)
            repoRoot / "Tools" / "DXC" / "bin" / "x64" / "dxc.exe",
            repoRoot / "Tools" / "DXC" / "bin" / "arm64" / "dxc.exe",
            repoRoot / "ThirdParty" / "DirectXShaderCompiler" / "bin" / "x64" / "dxc.exe"
#else
            repoRoot / "Tools" / "DXC" / "bin" / "dxc",
            repoRoot / "Tools" / "DXC" / "bin" / "x64" / "dxc",
            repoRoot / "Tools" / "DXC" / "bin" / "arm64" / "dxc",
            repoRoot / "ThirdParty" / "DirectXShaderCompiler" / "bin" / "dxc"
#endif
        };

        for (const auto& candidate : bundledCandidates)
        {
            if (auto found = tryPath(candidate))
                return found;
        }

        return std::nullopt;
    }

    void SkipIfNativeDxcUnavailable()
    {
        if (!FindNativeDxcExecutableForTests().has_value())
            GTEST_SKIP() << "Native dxc is unavailable in this environment.";
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
            shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
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
            shader = NLS::Render::Resources::Loaders::ShaderLoader::Create("App/Assets/Engine/Shaders/Standard.hlsl");
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
    SkipIfNativeDxcUnavailable();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
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
    SkipIfNativeDxcUnavailable();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
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
    SkipIfNativeDxcUnavailable();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
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
    SkipIfNativeDxcUnavailable();

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
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
    SkipIfNativeDxcUnavailable();

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
        << "Render-only suppression should reuse cached commands and only affect visibility.";
    visible = renderScene.GatherVisibleCommands({});
    EXPECT_TRUE(visible.opaques.empty());

    fixture.meshRenderer->SetTransientRenderingSuppressed(false);
    EXPECT_TRUE(owner->IsSelfActive());

    ASSERT_EQ(renderScene.Synchronize(fixture.scene, syncOptions).rebuiltCachedCommandCount, 0u)
        << "Restoring render-only suppression should not rebuild cached commands.";
    visible = renderScene.GatherVisibleCommands({});
    ASSERT_EQ(visible.opaques.size(), 1u);
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
    NLS::Engine::Rendering::RenderSceneVisibilityOptions visibilityOptions;
    visibilityOptions.frustum = &frustum;
    visibilityOptions.cameraPosition = {};

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

TEST(RenderSceneCacheTests, AdditivePreviewSceneSkipsTexturedModelUntilMaterialTexturesAreBound)
{
    using namespace NLS::Engine::Serialize;

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::ObjectTestAccess::ClearObjectRegistry();
    auto& driver = EnsureRenderSceneTestDriver();

    const auto meshGuid = NLS::Guid::Parse("25252525-2525-4525-8525-252525252525");
    const auto materialGuid = NLS::Guid::Parse("36363636-3636-4636-8636-363636363636");
    const auto meshPath = std::string("Library/Artifacts/Hero/preview/body.nmesh");
    const auto materialPath = std::string("Library/Artifacts/Hero/preview/body.nmat");
    const auto texturePath = std::string("Library/Artifacts/Hero/preview/body-basecolor.ntex");
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

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreatePixel(32u, 48u, 64u, 255u);
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

TEST(RenderSceneCacheTests, DynamicInstancingKeepsSiblingVisibleAfterDeletingOneSharedPrefabLikeObject)
{
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
