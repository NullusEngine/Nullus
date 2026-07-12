#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Data/DrawableInstanceCount.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RenderScene.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "SceneSystem/Scene.h"

namespace
{
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

NLS::Render::Resources::Mesh* CreateSingleMesh(const uint32_t materialIndex = 0u)
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
        {{0.0f, 0.0f, 0.0f}, 1.0f});
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
        shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
            "App/Assets/Engine/Shaders/Standard.hlsl");
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
            meshRenderer->SetFrustumBehaviour(
                NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
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

struct QueueSortFixture
{
    NLS::Engine::SceneSystem::Scene scene;
    NLS::Render::Resources::Shader* shader = nullptr;
    NLS::Render::Resources::Material opaqueMaterialA;
    NLS::Render::Resources::Mesh* sharedMesh = nullptr;

    QueueSortFixture()
    {
        EnsureRenderSceneTestDriver();
        shader = NLS::Render::Resources::Loaders::ShaderLoader::CreateBuiltInHlsl(
            "App/Assets/Engine/Shaders/Standard.hlsl");
        opaqueMaterialA.SetShader(shader);
        sharedMesh = CreateSingleMesh();
    }

    ~QueueSortFixture()
    {
        delete sharedMesh;
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
}

TEST(RenderScenePerformanceTests, StableLargeSceneAvoidsFullPrimitiveSynchronizationAcrossFrames)
{
    constexpr size_t kPrimitiveCount = 256u;
    ManyPrimitiveFixture fixture(kPrimitiveCount);
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;

    const auto first = renderScene.Synchronize(fixture.scene, options);
    const auto second = renderScene.Synchronize(fixture.scene, options);

    EXPECT_EQ(first.syncTouchedPrimitiveCount, kPrimitiveCount);
    EXPECT_EQ(first.rebuiltCachedCommandCount, kPrimitiveCount);
    EXPECT_EQ(second.addedPrimitiveCount, 0u);
    EXPECT_EQ(second.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(second.syncTouchedPrimitiveCount, 0u);
    EXPECT_EQ(renderScene.GetPrimitiveCount(), kPrimitiveCount);
}

TEST(RenderScenePerformanceTests, DynamicInstancingReducesOneThousandCompatibleOpaqueObjectsToOneSubmittedDraw)
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

TEST(RenderScenePerformanceTests, DynamicInstancingReducesTraceScaleCompatibleObjectsToOneSubmittedDraw)
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
