#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Math/Matrix4.h"
#include "Rendering/Data/Frustum.h"
#include "Rendering/Data/DrawableInstanceCount.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/Geometry/Vertex.h"
#include "Rendering/RenderScene.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
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
    };

    uint32_t ResolveVisibleInstanceCount(const NLS::Render::Entities::Drawable& drawable)
    {
        return NLS::Render::Data::ResolveDrawableInstanceCount(drawable).count;
    }

    template <typename T>
    NLS::Engine::Serialize::PPtr<T> MakeRenderScenePPtr(
        const NLS::Engine::Serialize::ObjectIdentifier& identifier)
    {
        return NLS::Engine::Serialize::PPtr<T>(
            NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
    }
}

TEST(RenderSceneCacheTests, StableSceneReusesPersistentPrimitivesAndCachedCommandsAcrossFrames)
{
    RenderableFixture fixture;
    NLS::Engine::Rendering::RenderScene renderScene;

    NLS::Engine::Rendering::RenderSceneSyncOptions options;
    options.defaultMaterial = &fixture.material;

    auto first = renderScene.Synchronize(fixture.scene, options);
    auto second = renderScene.Synchronize(fixture.scene, options);

    ASSERT_EQ(renderScene.GetPrimitiveCount(), 1u);
    EXPECT_EQ(first.addedPrimitiveCount, 1u);
    EXPECT_EQ(first.reusedPrimitiveCount, 0u);
    EXPECT_EQ(first.rebuiltCachedCommandCount, 1u);
    EXPECT_EQ(second.addedPrimitiveCount, 0u);
    EXPECT_EQ(second.reusedPrimitiveCount, 1u);
    EXPECT_EQ(second.rebuiltCachedCommandCount, 0u);
    EXPECT_EQ(renderScene.GetCachedCommandBuildCountForTesting(), 1u);
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

    ASSERT_EQ(visible.opaques.size(), 1u);
    EXPECT_EQ(unchangedCommands.rebuiltCachedCommandCount, 0u);

    NLS::Engine::Rendering::EngineDrawableDescriptor descriptor;
    ASSERT_TRUE(visible.opaques.front().second.TryGetDescriptor<NLS::Engine::Rendering::EngineDrawableDescriptor>(descriptor));
    const auto& expectedWorldMatrix = fixture.meshRenderer->gameobject()->GetTransform()->GetWorldMatrix();
    for (size_t index = 0u; index < 16u; ++index)
        EXPECT_FLOAT_EQ(descriptor.modelMatrix.data[index], expectedWorldMatrix.data[index]);
    EXPECT_FLOAT_EQ(descriptor.userMatrix.data[3], 9.0f);
    EXPECT_EQ(renderScene.GetCachedCommandBuildCountForTesting(), 1u);
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
    ManyPrimitiveFixture fixture(192u);
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
