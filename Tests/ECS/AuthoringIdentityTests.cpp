#include <Engine/ECS/Baking.h>
#include <Engine/ECS/TransformHierarchy.h>
#include <Engine/GameObject.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/SceneSystem/SceneManager.h>
#include <Core/Assets/AssetMeta.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/Serialize/ObjectGraphSerializer.h>
#include <Engine/Serialize/ObjectGraphInstantiator.h>
#include <Engine/Serialize/ObjectGraphWriter.h>
#include <gtest/gtest.h>
#include <algorithm>

namespace
{
using namespace NLS::Engine;
using namespace NLS::Engine::Serialize;
using namespace NLS::Engine::ECSIntegration;
using SceneSystem::Scene;
struct SceneFile
{
    std::filesystem::path path = std::filesystem::absolute("tmp") /
        ("ecs-authoring-identity-" + NLS::Guid::New().ToString() + ".scene");
    ~SceneFile()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(NLS::Core::Assets::GetAssetMetaPath(path), error);
    }
};
ObjectId Identity(Scene& scene, const NLS::Object& object)
{
    return PersistentManager::Instance().GetOrCreateObjectGraphId(scene, object);
}

TEST(ECSAuthoringIdentity, AutomaticCaptureAndSerializationShareStableRealObjectIds)
{
    Scene scene;
    auto& first = scene.CreateGameObject("first");
    auto& second = scene.CreateGameObject("second");
    const auto sources = CaptureSceneBakingSources(scene);
    ASSERT_EQ(sources.size(), 2u);
    const auto original = ObjectGraphSerializer::SerializeScene(scene);
    const auto firstId = Identity(scene, first);
    const auto componentId = Identity(scene, *first.GetTransform());
    EXPECT_EQ(sources[0].id, firstId);
    EXPECT_NE(sources[0].id, sources[1].id);
    first.SetName("renamed"); first.SetTag("renamed-tag"); first.SetParent(second);
    scene.CreateGameObject("another");
    const auto changed = ObjectGraphSerializer::SerializeScene(scene);
    EXPECT_EQ(original.root, changed.root);
    EXPECT_EQ(Identity(scene, first), firstId);
    EXPECT_EQ(Identity(scene, *first.GetTransform()), componentId);
    const auto record = std::find_if(changed.objects.begin(), changed.objects.end(),
        [&](const auto& value) { return value.id == firstId; });
    ASSERT_NE(record, changed.objects.end());
    EXPECT_EQ(record->debugName, "renamed");
}

TEST(ECSAuthoringIdentity, SavingAndLoadingPreserveAuthoringAndComponentIdsWithoutRuntimeSlots)
{
    SceneFile file;
    ObjectGraphDocument saved;
    std::vector<ObjectId> ids;
    {
        Scene scene;
        auto& root = scene.CreateGameObject("root");
        auto& child = scene.CreateGameObject("child");
        child.SetParent(root);
        root.GetTransform()->SetLocalPosition({5, 0, 0});
        for (const auto& source : CaptureSceneBakingSources(scene)) ids.push_back(source.id);
        saved = ObjectGraphSerializer::SerializeScene(scene);
        ASSERT_TRUE(SceneSystem::SceneManager::SaveSceneToPath(scene, file.path.string()));
    }
    SceneSystem::SceneManager manager;
    ASSERT_TRUE(manager.LoadScene(file.path.string(), true));
    auto* loaded = manager.GetCurrentScene();
    ASSERT_NE(loaded, nullptr);
    const auto sources = CaptureSceneBakingSources(*loaded);
    ASSERT_EQ(sources.size(), ids.size());
    for (size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(sources[i].id, ids[i]);
    const auto resaved = ObjectGraphSerializer::SerializeScene(*loaded);
    EXPECT_EQ(ObjectGraphWriter::Write(saved), ObjectGraphWriter::Write(resaved));
}

TEST(ECSAuthoringIdentity, LoadedLegacyRecordIdsAreRetainedAndIndependentScenesAreIsolated)
{
    Scene original;
    auto& object = original.CreateGameObject("legacy");
    const ObjectId legacy(NLS::Guid::NewDeterministic("GameObject:0:legacy:Untagged"));
    ASSERT_TRUE(PersistentManager::Instance().BindObjectGraphId(original, object, legacy));
    auto saved = ObjectGraphSerializer::SerializeScene(original);
    saved.documentId = NLS::Guid::NewDeterministic("Legacy.ObjectGraph.Scene.Document");
    auto first = ObjectGraphInstantiator::InstantiateScene(saved);
    auto second = ObjectGraphInstantiator::InstantiateScene(saved);
    ASSERT_TRUE(first); ASSERT_TRUE(second);
    EXPECT_EQ(CaptureSceneBakingSources(*first).at(0).id, legacy);
    EXPECT_EQ(CaptureSceneBakingSources(*second).at(0).id, legacy);
    EXPECT_EQ(ObjectGraphSerializer::SerializeScene(*first).documentId, saved.documentId);
    auto& another = first->CreateGameObject("legacy");
    EXPECT_NE(Identity(*first, another), legacy);
    EXPECT_FALSE(PersistentManager::Instance().BindObjectGraphId(*first, another, legacy));
}

TEST(ECSAuthoringIdentity, PrefabInstancesHaveDistinctIdsWhichSurviveSceneSerialization)
{
    Scene source;
    auto& root = source.CreateGameObject("prefab");
    source.CreateGameObject("child").SetParent(root);
    const auto prefab = ObjectGraphSerializer::SerializePrefab(root);
    Scene scene;
    const auto first = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);
    const auto second = ObjectGraphInstantiator::InstantiatePrefab(prefab, scene);
    ASSERT_NE(first.root, nullptr); ASSERT_NE(second.root, nullptr);
    const auto firstId = Identity(scene, *first.root);
    const auto secondId = Identity(scene, *second.root);
    EXPECT_NE(firstId, secondId);
    EXPECT_EQ(firstId, first.sourceToInstance.at(prefab.graph.root));
    const auto saved = ObjectGraphSerializer::SerializeScene(scene);
    auto loaded = ObjectGraphInstantiator::InstantiateScene(saved);
    ASSERT_TRUE(loaded);
    const auto sources = CaptureSceneBakingSources(*loaded);
    EXPECT_EQ(std::count_if(sources.begin(), sources.end(), [&](const auto& value) { return value.id == firstId; }), 1);
    EXPECT_EQ(std::count_if(sources.begin(), sources.end(), [&](const auto& value) { return value.id == secondId; }), 1);
}

TEST(ECSAuthoringIdentity, UndoRuntimeSlotRebindingCanReclaimTheOriginalSerializedIdentity)
{
    Scene scene;
    auto& original = scene.CreateGameObject("deleted");
    const auto id = Identity(scene, original);
    auto& manager = PersistentManager::Instance();
    const auto target = manager.GetOrCreateLocalObjectIdentifier(original);
    scene.DestroyGameObject(original); scene.CollectGarbages();
    ASSERT_TRUE(manager.ReserveObjectIdentifier(target));
    auto& restored = scene.CreateGameObject("restored");
    const auto provisional = Identity(scene, restored);
    EXPECT_NE(provisional, id);
    ASSERT_NE(manager.BindObjectIdentifier(restored, target), NLS::InstanceID_None);
    EXPECT_EQ(manager.FindObjectGraphId(scene, restored), provisional);
    ASSERT_TRUE(manager.BindObjectGraphId(scene, restored, id));
    EXPECT_EQ(CaptureSceneBakingSources(scene).at(0).id, id);
    manager.ReleaseReservedObjectIdentifier(target);
}

TEST(ECSAuthoringIdentity, AutomaticSourcesExcludeTransientObjectsAndRejectRecoveryAndPlay)
{
    Scene scene;
    auto& object = scene.CreateGameObject("normal");
    auto& temporary = scene.CreateGameObject("temporary");
    temporary.SetEditorTransient(true);
    ASSERT_EQ(CaptureSceneBakingSources(scene).size(), 1u);
    scene.DestroyGameObject(object);
    EXPECT_TRUE(CaptureSceneBakingSources(scene).empty());
    scene.CollectGarbages();
    scene.Play();
    EXPECT_THROW(CaptureSceneBakingSources(scene), std::invalid_argument);
    scene.Stop();
    scene.MarkSerializationRecovery("unresolved");
    EXPECT_THROW(CaptureSceneBakingSources(scene), std::invalid_argument);
}

TEST(ECSAuthoringIdentity, DestroyedSceneScopesDoNotLeakIdentityIntoReclaimedRuntimeSlots)
{
    auto& manager = PersistentManager::Instance();
    ObjectId oldId;
    ObjectIdentifier target;
    {
        auto scene = std::make_unique<Scene>();
        oldId = Identity(*scene, *scene);
        target = manager.GetOrCreateLocalObjectIdentifier(*scene);
    }
    ASSERT_TRUE(manager.ReserveObjectIdentifier(target));
    Scene replacement;
    ASSERT_NE(manager.BindObjectIdentifier(replacement, target), NLS::InstanceID_None);
    EXPECT_FALSE(manager.FindObjectGraphId(replacement, replacement).has_value());
    EXPECT_NE(Identity(replacement, replacement), oldId);
    manager.ReleaseReservedObjectIdentifier(target);
}

TEST(ECSAuthoringIdentity, AutomaticBakingReusesCapturedIdsAfterAReloadAndReparent)
{
    NLS::ECS::World schemas;
    RegisterTransformComponents(schemas);
    BakingPipeline pipeline(schemas.GetRegistryLifetime()); RegisterTransformBaker(pipeline);
    Scene scene;
    auto& root = scene.CreateGameObject("root");
    auto& child = scene.CreateGameObject("child");
    child.SetParent(root); child.GetTransform()->SetLocalPosition({3, 0, 0});
    const auto first = pipeline.Bake(CaptureSceneBakingSources(scene));
    ASSERT_TRUE(first.succeeded);
    auto loaded = ObjectGraphInstantiator::InstantiateScene(ObjectGraphSerializer::SerializeScene(scene));
    ASSERT_TRUE(loaded);
    const auto same = pipeline.Bake(CaptureSceneBakingSources(*loaded));
    ASSERT_TRUE(same.succeeded);
    EXPECT_EQ(same.statistics.rebuilt, 0u);
    EXPECT_EQ(same.artifact, first.artifact);
    loaded->GetGameObjects().back()->DetachFromParent();
    const auto changed = pipeline.Bake(CaptureSceneBakingSources(*loaded));
    ASSERT_TRUE(changed.succeeded);
    EXPECT_EQ(changed.statistics.rebuilt, 1u);
}
}
