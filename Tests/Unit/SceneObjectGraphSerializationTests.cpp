#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>

#include "GameObject.h"
#include "Components/LightComponent.h"
#include "Components/MaterialRenderer.h"
#include "Components/TransformComponent.h"
#include "Rendering/Resources/Material.h"
#include "Reflection/Field.h"
#include "Reflection/MetaParserFieldMethodSample.h"
#include "Reflection/Method.h"
#include "Reflection/RuntimeMetaProperties.h"
#include "Reflection/Type.h"
#include "SceneSystem/Scene.h"
#include "SceneSystem/SceneManager.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"

namespace
{
std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream file(path);
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

NLS::Engine::Serialize::ObjectGraphDocument MakeSimpleSceneDocument()
{
    using namespace NLS::Engine::Serialize;

    const auto sceneId = ObjectId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-bbbb-bbbbbbbbbbbb"));
    const auto playerId = ObjectId(NLS::Guid::Parse("cccccccc-cccc-4ccc-cccc-cccccccccccc"));
    const auto transformId = ObjectId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd"));

    ObjectGraphDocument document;
    document.documentId = NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    document.root = sceneId;

    ObjectRecord scene;
    scene.id = sceneId;
    scene.typeName = "NLS::Engine::SceneSystem::Scene";
    scene.debugName = "Main Scene";
    scene.properties.push_back({"gameObjects", PropertyValue::Array({PropertyValue::OwnedReference(playerId)})});

    ObjectRecord player;
    player.id = playerId;
    player.typeName = "NLS::Engine::GameObject";
    player.debugName = "Player";
    player.debugPath = "/Player";
    player.properties.push_back({"name", PropertyValue::String("Player")});
    player.properties.push_back({"tag", PropertyValue::String("Player")});
    player.properties.push_back({"parent", PropertyValue::ObjectReference(sceneId)});
    player.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(transformId)})});

    ObjectRecord transform;
    transform.id = transformId;
    transform.typeName = "NLS::Engine::Components::TransformComponent";
    transform.debugName = "Transform";

    document.objects.push_back(std::move(player));
    document.objects.push_back(std::move(transform));
    document.objects.push_back(std::move(scene));
    return document;
}
}

TEST(SceneObjectGraphSerializationTests, SerializationMetadataTagsExposeIntent)
{
    EXPECT_EQ(NLS::meta::SerializeField().GetIntent(), NLS::meta::SerializationFieldIntent::Value);
    EXPECT_EQ(NLS::meta::Transient().GetIntent(), NLS::meta::SerializationFieldIntent::Transient);
    EXPECT_EQ(NLS::meta::OwnedReference().GetIntent(), NLS::meta::SerializationFieldIntent::OwnedReference);
    EXPECT_EQ(NLS::meta::ObjectReference().GetIntent(), NLS::meta::SerializationFieldIntent::ObjectReference);
    EXPECT_EQ(NLS::meta::AssetReference().GetIntent(), NLS::meta::SerializationFieldIntent::AssetReference);
    EXPECT_TRUE(NLS::meta::EditorOnly().IsEditorOnly());
    EXPECT_TRUE(NLS::meta::RuntimeOnly().IsRuntimeOnly());
    EXPECT_EQ(NLS::meta::FormerlySerializedAs("oldName").name, "oldName");
    EXPECT_EQ(NLS::meta::StableTypeName("Engine.MeshRenderer").name, "Engine.MeshRenderer");
    EXPECT_EQ(NLS::meta::FormerlyTypeName("Engine.ModelRenderer").name, "Engine.ModelRenderer");
}

TEST(SceneObjectGraphSerializationTests, SceneIsReflectedObjectRoot)
{
    const auto sceneType = NLS::meta::Type::GetFromName("NLS::Engine::SceneSystem::Scene");
    const auto objectType = NLS_TYPEOF(NLS::meta::Object);

    ASSERT_TRUE(sceneType.IsValid());
    ASSERT_TRUE(objectType.IsValid());
    EXPECT_TRUE(sceneType.DerivesFrom(objectType));
    EXPECT_TRUE((std::is_base_of_v<NLS::meta::Object, NLS::Engine::SceneSystem::Scene>));
}

TEST(SceneObjectGraphSerializationTests, GameObjectReflectionDoesNotExposeWorldId)
{
    const auto gameObjectType = NLS::meta::Type::GetFromName("NLS::Engine::GameObject");

    ASSERT_TRUE(gameObjectType.IsValid());
    EXPECT_FALSE(gameObjectType.GetField("worldID").IsValid());
    EXPECT_FALSE(gameObjectType.GetMethod("GetWorldID").IsValid());
    EXPECT_FALSE(gameObjectType.GetMethod("SetWorldID").IsValid());
}

TEST(SceneObjectGraphSerializationTests, GameObjectConstructionUsesNameAndTagOnly)
{
    NLS::Engine::GameObject actor("Serializable Actor", "Gameplay");

    EXPECT_EQ(actor.GetName(), "Serializable Actor");
    EXPECT_EQ(actor.GetTag(), "Gameplay");
    EXPECT_NE(actor.GetTransform(), nullptr);
}

TEST(SceneObjectGraphSerializationTests, NewSceneObjectGraphOutputContainsNoLegacySceneShape)
{
    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(MakeSimpleSceneDocument());

    EXPECT_NE(output.find("\"format\": \"Nullus.ObjectGraph.Scene\""), std::string::npos);
    EXPECT_NE(output.find("\"type\": \"NLS::Engine::SceneSystem::Scene\""), std::string::npos);
    EXPECT_NE(output.find("\"type\": \"NLS::Engine::GameObject\""), std::string::npos);
    EXPECT_NE(output.find("\"$owned\""), std::string::npos);
    EXPECT_NE(output.find("\"$ref\""), std::string::npos);
    EXPECT_EQ(output.find("worldID"), std::string::npos);
    EXPECT_EQ(output.find("SerializedSceneData"), std::string::npos);
    EXPECT_EQ(output.find("SerializedActorData"), std::string::npos);
    EXPECT_EQ(output.find("SerializedComponentData"), std::string::npos);
}

TEST(SceneObjectGraphSerializationTests, SimpleSceneObjectGraphMatchesGoldenOutput)
{
    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(MakeSimpleSceneDocument());
    const auto goldenPath =
        std::filesystem::path(NLS_ROOT_DIR) /
        "Tests/Unit/Fixtures/ObjectGraph/simple_scene.objectgraph.json";

    EXPECT_EQ(output, ReadTextFile(goldenPath));
}

TEST(SceneObjectGraphSerializationTests, SceneRoundTripsGameObjectsComponentsParentsAndCaches)
{
    using namespace NLS::Engine;
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    NLS::Engine::SceneSystem::Scene source;
    auto& parent = source.CreateGameObject("Parent", "Root");
    auto& child = source.CreateGameObject("Child", "Gameplay");
    child.SetParent(parent);
    child.SetActive(false);
    auto* light = child.AddComponent<LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetIntensity(3.5f);

    const auto document = ObjectGraphSerializer::SerializeScene(source);
    ASSERT_FALSE(document.Validate().HasErrors());

    auto loaded = ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loaded, nullptr);

    const auto& actors = loaded->GetActors();
    ASSERT_EQ(actors.size(), 2u);

    auto* loadedParent = loaded->FindActorByName("Parent");
    auto* loadedChild = loaded->FindActorByName("Child");
    ASSERT_NE(loadedParent, nullptr);
    ASSERT_NE(loadedChild, nullptr);

    EXPECT_EQ(loadedParent->GetTag(), "Root");
    EXPECT_EQ(loadedChild->GetTag(), "Gameplay");
    EXPECT_FALSE(loadedChild->IsSelfActive());
    EXPECT_EQ(loadedChild->GetParent(), loadedParent);
    ASSERT_EQ(loadedParent->GetChildren().size(), 1u);
    EXPECT_EQ(loadedParent->GetChildren()[0], loadedChild);

    const auto& components = loadedChild->GetComponents();
    ASSERT_EQ(components.size(), 2u);
    EXPECT_NE(dynamic_cast<TransformComponent*>(components[0].get()), nullptr);
    auto* loadedLight = dynamic_cast<LightComponent*>(components[1].get());
    ASSERT_NE(loadedLight, nullptr);
    EXPECT_EQ(loadedLight->gameobject(), loadedChild);
    EXPECT_FLOAT_EQ(loadedLight->GetIntensity(), 3.5f);

    const auto& fastAccess = loaded->GetFastAccessComponents();
    ASSERT_EQ(fastAccess.lights.size(), 1u);
    EXPECT_EQ(fastAccess.lights[0], loadedLight);
}

TEST(SceneObjectGraphSerializationTests, SceneSerializationAssignsUniqueComponentIdsForDuplicateNamedGameObjects)
{
    using namespace NLS::Engine::Components;
    using namespace NLS::Engine::Serialize;

    NLS::Engine::SceneSystem::Scene source;
    source.CreateGameObject("Duplicate", "Gameplay").AddComponent<LightComponent>();
    source.CreateGameObject("Duplicate", "Gameplay").AddComponent<LightComponent>();

    const auto document = ObjectGraphSerializer::SerializeScene(source);

    EXPECT_FALSE(document.Validate().HasErrors());
}

TEST(SceneObjectGraphSerializationTests, ObjectGraphSerializerWritesOrdinaryFieldsThroughReflection)
{
    NLS::meta::MetaParserFieldMethodSample object;
    object.SetValue(42);

    const auto record = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeObjectRecord(
        object,
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Reflected.Serializable.Object")));

    ASSERT_EQ(record.properties.size(), 1u);
    EXPECT_EQ(record.properties[0].name, "value");
    ASSERT_EQ(record.properties[0].value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Integer);
    EXPECT_EQ(record.properties[0].value.GetInteger(), 42);
}

TEST(SceneObjectGraphSerializationTests, MaterialRendererFillWithMaterialSerializesSingleFallbackMaterialPath)
{
    NLS::Engine::Components::MaterialRenderer renderer;
    NLS::Render::Resources::Material material;
    const_cast<std::string&>(material.path) = ":Materials\\Default.mat";

    renderer.FillWithMaterial(material);

    const auto paths = renderer.GetMaterialPaths();
    ASSERT_EQ(paths.size(), 1u);
    EXPECT_EQ(paths[0], ":Materials\\Default.mat");
}

TEST(SceneObjectGraphSerializationTests, SceneManagerTracksDirtyStateAroundSceneSaveAndLoad)
{
    NLS::Engine::SceneSystem::SceneManager sceneManager;

    EXPECT_FALSE(sceneManager.HasUnsavedSceneChanges());

    sceneManager.MarkCurrentSceneDirty();
    EXPECT_TRUE(sceneManager.HasUnsavedSceneChanges());

    sceneManager.LoadEmptyScene();
    EXPECT_FALSE(sceneManager.HasUnsavedSceneChanges());
}

TEST(SceneObjectGraphSerializationTests, SceneManagerSavesAndLoadsScenesThroughObjectGraphFiles)
{
    using namespace NLS::Engine::Components;

    const auto scenePath =
        std::filesystem::temp_directory_path() /
        ("nullus_scene_manager_object_graph_" + NLS::Guid::New().ToString() + ".scene");

    NLS::Engine::SceneSystem::SceneManager sourceManager;
    auto& actor = sourceManager.GetCurrentScene()->CreateGameObject("Persisted Light", "Gameplay");
    auto* light = actor.AddComponent<LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetIntensity(6.25f);

    sourceManager.MarkCurrentSceneDirty();
    ASSERT_TRUE(sourceManager.SaveCurrentScene(scenePath.string()));
    EXPECT_FALSE(sourceManager.HasUnsavedSceneChanges());
    EXPECT_EQ(sourceManager.GetCurrentSceneSourcePath(), scenePath.string());

    const auto savedText = ReadTextFile(scenePath);
    EXPECT_NE(savedText.find("\"format\": \"Nullus.ObjectGraph.Scene\""), std::string::npos);
    EXPECT_EQ(savedText.find("SerializedSceneData"), std::string::npos);
    EXPECT_EQ(savedText.find("worldID"), std::string::npos);
    ASSERT_TRUE(NLS::Engine::Serialize::ObjectGraphReader::Read(savedText).has_value());

    NLS::Engine::SceneSystem::SceneManager loadedManager;
    ASSERT_TRUE(loadedManager.LoadScene(scenePath.string(), true));
    EXPECT_FALSE(loadedManager.HasUnsavedSceneChanges());
    EXPECT_EQ(loadedManager.GetCurrentSceneSourcePath(), scenePath.string());

    auto* loadedActor = loadedManager.GetCurrentScene()->FindActorByName("Persisted Light");
    ASSERT_NE(loadedActor, nullptr);
    EXPECT_EQ(loadedActor->GetTag(), "Gameplay");

    auto* loadedLight = loadedActor->GetComponent<LightComponent>();
    ASSERT_NE(loadedLight, nullptr);
    EXPECT_FLOAT_EQ(loadedLight->GetIntensity(), 6.25f);

    std::filesystem::remove(scenePath);
}

TEST(SceneObjectGraphSerializationTests, SceneManagerUnloadsCurrentSceneBeforeLoadedActorsAreCreated)
{
    using namespace NLS::Engine;

    const auto scenePath =
        std::filesystem::temp_directory_path() /
        ("nullus_scene_manager_event_order_" + NLS::Guid::New().ToString() + ".scene");

    SceneSystem::Scene sourceScene;
    sourceScene.CreateGameObject("Loaded Actor", "Gameplay");
    const auto document = Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    std::ofstream output(scenePath);
    output << Serialize::ObjectGraphWriter::Write(document);
    output.close();

    SceneSystem::SceneManager manager;
    std::vector<std::string> events;
    auto unloadListener = manager.SceneUnloadEvent += [&events]
    {
        events.push_back("unload");
    };
    auto createdListener = GameObject::CreatedEvent += [&events](GameObject& actor)
    {
        if (actor.GetName() == "Loaded Actor")
            events.push_back("created");
    };

    ASSERT_TRUE(manager.LoadScene(scenePath.string(), true));

    GameObject::CreatedEvent -= createdListener;
    manager.SceneUnloadEvent -= unloadListener;
    std::filesystem::remove(scenePath);

    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events[0], "unload");
    EXPECT_EQ(events[1], "created");
}
