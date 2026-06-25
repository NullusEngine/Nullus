#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "Assets/PrefabUtilityFacade.h"
#include "Assets/ArtifactManifest.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MeshFilter.h"
#include "GameObject.h"
#include "Guid.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "SceneSystem/Scene.h"
#include "SceneSystem/SceneManager.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/PPtr.h"
#include "Serialize/PPtrResourceTypes.h"

namespace
{
using NLS::Core::Assets::AssetId;
using NLS::Editor::Assets::PrefabAssetQuery;
using NLS::Editor::Assets::PrefabAssetType;
using NLS::Editor::Assets::PrefabEditorApplyAvailability;
using NLS::Editor::Assets::PrefabEditorConnectionState;
using NLS::Editor::Assets::PrefabEditorResourceState;
using NLS::Editor::Assets::PrefabEditorStateQuery;
using NLS::Editor::Assets::PrefabInstanceQuery;
using NLS::Editor::Assets::PrefabInstanceStatus;
using NLS::Editor::Assets::PrefabOperationStatus;
using NLS::Editor::Assets::PrefabOverrideDescriptor;
using NLS::Editor::Assets::PrefabOverrideKind;
using NLS::Editor::Assets::PrefabUnpackMode;
using NLS::Editor::Assets::PrefabUtilityFacade;
using NLS::Engine::Assets::PrefabArtifact;
using NLS::Engine::Serialize::ObjectIdentifier;
using NLS::Engine::Serialize::ObjectGraphDocument;
using NLS::Engine::Serialize::ObjectId;
using NLS::Engine::Serialize::ObjectRecordState;
using NLS::Engine::Serialize::PropertyValue;

AssetId Id(const char* guid)
{
    return AssetId(NLS::Guid::Parse(guid));
}

std::string JoinPrefabDiagnostics(const NLS::Editor::Assets::PrefabOperationResult& result)
{
    std::string output;
    for (const auto& diagnostic : result.diagnostics)
    {
        if (!output.empty())
            output += ",";
        output += diagnostic.code;
    }
    return output;
}

std::string JoinSerializationDiagnostics(const NLS::Engine::Serialize::SerializationDiagnosticList& diagnostics)
{
    std::string output;
    for (const auto& diagnostic : diagnostics.GetItems())
    {
        if (!output.empty())
            output += ",";
        output += diagnostic.GetMessage();
    }
    return output;
}

template <typename T>
NLS::Engine::Serialize::PPtr<T> MakePPtr(const NLS::Engine::Serialize::ObjectIdentifier& identifier)
{
    return NLS::Engine::Serialize::PPtr<T>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
}

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    for (const auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

const NLS::Engine::Serialize::PropertyValue* FindObjectProperty(
    const NLS::Engine::Serialize::PropertyValue& value,
    const char* name)
{
    if (value.GetKind() != PropertyValue::Kind::Object)
        return nullptr;

    for (const auto& property : value.GetObject())
    {
        if (property.first == name)
            return &property.second;
    }
    return nullptr;
}

std::vector<ObjectId> ReadOwnedReferences(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const char* name)
{
    std::vector<ObjectId> ids;
    const auto* property = FindProperty(record, name);
    if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
        return ids;

    for (const auto& value : property->value.GetArray())
    {
        if (value.GetKind() == PropertyValue::Kind::OwnedReference)
            ids.push_back(value.GetObjectId());
    }
    return ids;
}

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecord(
    const ObjectGraphDocument& graph,
    const ObjectId& id)
{
    for (const auto& record : graph.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecordByType(
    const ObjectGraphDocument& graph,
    const char* typeName)
{
    for (const auto& record : graph.objects)
    {
        if (record.typeName == typeName)
            return &record;
    }
    return nullptr;
}

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecord(
    const ObjectGraphDocument& graph,
    const NLS::Engine::Serialize::PropertyValue& value)
{
    if (value.GetKind() != PropertyValue::Kind::OwnedReference)
        return nullptr;
    return FindObjectRecord(graph, value.GetObjectId());
}

const NLS::Engine::Serialize::ObjectRecord* FindGameObjectRecordByName(
    const ObjectGraphDocument& graph,
    const char* name)
{
    for (const auto& record : graph.objects)
    {
        const auto* property = FindProperty(record, "name");
        if (property &&
            property->value.GetKind() == PropertyValue::Kind::String &&
            property->value.GetString() == name)
        {
            return &record;
        }
    }
    return nullptr;
}

bool ContainsResolvedAsset(
    const PrefabArtifact& artifact,
    const AssetId& assetId,
    const std::string& expectedType,
    const std::string& artifactPath)
{
    return std::any_of(
        artifact.resolvedAssets.begin(),
        artifact.resolvedAssets.end(),
        [&](const NLS::Engine::Assets::PrefabResolvedAsset& resolved)
        {
            return resolved.assetId == assetId &&
                resolved.expectedType == expectedType &&
                resolved.artifactPath == artifactPath;
        });
}

std::string ContentArtifactPath(const std::string& owner, const std::string& suffix)
{
    const auto fileName = NLS::Core::Assets::BuildArtifactStorageFileName(owner + ":" + suffix);
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(fileName)).generic_string();
}

std::string ReadStringProperty(const PrefabArtifact& artifact, const char* propertyName)
{
    const auto* root = FindObjectRecord(artifact.graph, artifact.graph.root);
    if (!root)
        return {};
    const auto* property = FindProperty(*root, propertyName);
    if (!property || property->value.GetKind() != PropertyValue::Kind::String)
        return {};
    return property->value.GetString();
}

PrefabArtifact MakePrefabArtifact(const char* name, AssetId assetId)
{
    NLS::Engine::GameObject root(name, "Prefab");
    auto result = PrefabUtilityFacade().SaveAsPrefabAsset(
        root,
        assetId,
        std::string("Assets/Prefabs/") + name + ".prefab");
    EXPECT_EQ(result.status, PrefabOperationStatus::Committed);
    EXPECT_TRUE(result.artifact.has_value());
    return *result.artifact;
}

NLS::Engine::GameObject* FindChildByName(NLS::Engine::GameObject& root, const char* name)
{
    for (auto* child : root.GetChildren())
    {
        if (child && child->GetName() == name)
            return child;
    }
    return nullptr;
}

const NLS::Editor::Assets::PrefabOverrideDescriptor* FindOverride(
    const std::vector<NLS::Editor::Assets::PrefabOverrideDescriptor>& overrides,
    PrefabOverrideKind kind,
    const std::string& propertyPath = {})
{
    for (const auto& overrideRecord : overrides)
    {
        if (overrideRecord.kind != kind)
            continue;
        if (!propertyPath.empty() && overrideRecord.propertyPath != propertyPath)
            continue;
        return &overrideRecord;
    }
    return nullptr;
}

PrefabArtifact MakePrefabWithNestedReference(
    AssetId ownerId,
    AssetId nestedId,
    const std::string& nestedSubAssetKey)
{
    auto artifact = MakePrefabArtifact("NestedOwner", ownerId);
    auto* root = const_cast<NLS::Engine::Serialize::ObjectRecord*>(
        FindObjectRecord(artifact.graph, artifact.graph.root));
    EXPECT_NE(root, nullptr);
    root->properties.push_back({
        "nestedPrefab",
        PropertyValue::ObjectReference(ObjectIdentifier::Asset(
            NLS::Engine::Serialize::AssetId(nestedId.GetGuid()),
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                nestedId.GetGuid(),
                nestedSubAssetKey),
            nestedSubAssetKey))
    });
    artifact.resolvedAssets.push_back({nestedId, "Prefab", nestedSubAssetKey, {}});
    return artifact;
}
}

TEST(PrefabUtilityFacadeTests, ReportsPrefabAssetTypesAndInstanceStatuses)
{
    PrefabUtilityFacade facade;
    auto regular = MakePrefabArtifact("Crate", Id("b1010101-0101-4101-8101-010101010101"));

    EXPECT_EQ(facade.GetPrefabAssetType({&regular}), PrefabAssetType::Regular);

    EXPECT_EQ(
        facade.GetPrefabAssetType({&regular, true, false, false, "prefab:Model"}),
        PrefabAssetType::Model);

    auto variant = facade.CreateVariant({
        &regular,
        regular.assetId,
        "prefab:Crate",
        "Assets/Prefabs/CrateVariant.prefab",
        Id("b1020202-0202-4202-8202-020202020202"),
        false,
        false
    });
    ASSERT_EQ(variant.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(variant.artifact.has_value());
    EXPECT_EQ(facade.GetPrefabAssetType({&*variant.artifact}), PrefabAssetType::Variant);
    EXPECT_EQ(facade.GetPrefabAssetType({nullptr, false, true}), PrefabAssetType::MissingAsset);

    auto corrupt = regular;
    corrupt.graph.root = {};
    EXPECT_EQ(facade.GetPrefabAssetType({&corrupt}), PrefabAssetType::Corrupt);

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &regular,
        regular.assetId,
        "prefab:Crate",
        Id("b1030303-0303-4303-8303-030303030303")
    }, scene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    EXPECT_EQ(
        facade.GetPrefabInstanceStatus({&*instantiate.instance}),
        PrefabInstanceStatus::Connected);
    EXPECT_EQ(
        facade.GetPrefabInstanceStatus({&*instantiate.instance, false}),
        PrefabInstanceStatus::MissingAsset);

    auto unpack = facade.UnpackPrefabInstance(*instantiate.instance, PrefabUnpackMode::Completely);
    ASSERT_EQ(unpack.status, PrefabOperationStatus::Committed);
    EXPECT_EQ(
        facade.GetPrefabInstanceStatus({&*instantiate.instance}),
        PrefabInstanceStatus::Disconnected);

    NLS::Editor::Assets::PrefabInstanceRecord invalidInstance;
    invalidInstance.prefabAssetId = regular.assetId;
    EXPECT_EQ(
        facade.GetPrefabInstanceStatus({&invalidInstance, true, true}),
        PrefabInstanceStatus::Invalid);
}

TEST(PrefabUtilityFacadeTests, SavesConnectsAndEditsPrefabContentsThroughPrefabStage)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("Workbench", "Prop");
    NLS::Engine::SceneSystem::Scene scene;
    scene.AddGameObject(new NLS::Engine::GameObject("ExistingSceneObject", "Prop"));
    auto& sceneRoot = scene.CreateGameObject("ConnectedWorkbench", "Prop");

    auto save = facade.SaveAsPrefabAsset(
        root,
        Id("b2010101-0101-4101-8101-010101010101"),
        "Assets/Prefabs/Workbench.prefab");
    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());
    EXPECT_EQ(ReadStringProperty(*save.artifact, "name"), "Workbench");

    auto saveAndConnect = facade.SaveAsPrefabAssetAndConnect(
        sceneRoot,
        Id("b2020202-0202-4202-8202-020202020202"),
        "Assets/Prefabs/ConnectedWorkbench.prefab",
        scene,
        Id("b2030303-0303-4303-8303-030303030303"));
    ASSERT_EQ(saveAndConnect.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(saveAndConnect.artifact.has_value());
    ASSERT_TRUE(saveAndConnect.instance.has_value());
    EXPECT_TRUE(saveAndConnect.instance->prefabAssetId.IsValid());
    EXPECT_NE(saveAndConnect.instance->instanceRoot, nullptr);
    EXPECT_EQ(saveAndConnect.instance->instanceRoot, &sceneRoot);
    EXPECT_EQ(saveAndConnect.instance->instanceRoot->GetName(), "ConnectedWorkbench");
    EXPECT_EQ(scene.GetGameObjects().size(), 2u);
    EXPECT_NE(scene.GetGameObjects()[0], saveAndConnect.instance->instanceRoot);
    EXPECT_EQ(scene.GetGameObjects()[1], saveAndConnect.instance->instanceRoot);

    auto open = facade.LoadPrefabContents({
        &*save.artifact,
        save.artifact->assetId,
        "prefab:Workbench",
        false
    });
    ASSERT_EQ(open.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(open.stage.has_value());
    ASSERT_NE(open.stage->stageRoot, nullptr);
    open.stage->stageRoot->SetName("SavedWorkbench");
    facade.MarkPrefabContentsDirty(*open.stage);

    auto stage = std::move(*open.stage);
    auto stageSave = facade.SavePrefabContents(stage, *save.artifact);
    ASSERT_EQ(stageSave.status, PrefabOperationStatus::Committed);
    EXPECT_EQ(ReadStringProperty(*save.artifact, "name"), "SavedWorkbench");
    EXPECT_FALSE(stage.dirty);

    auto unload = facade.UnloadPrefabContents(stage, false, &*save.artifact);
    EXPECT_EQ(unload.status, PrefabOperationStatus::Committed);
    EXPECT_EQ(stage.stageRoot, nullptr);
    EXPECT_EQ(stage.stageScene, nullptr);
}

TEST(PrefabUtilityFacadeTests, ReportsDisconnectedStatusForReloadedGeneratedModelSceneObjectWithoutRegistryReconnection)
{
    using namespace NLS::Engine::Serialize;

    const auto scenePath =
        std::filesystem::temp_directory_path() /
        ("nullus_prefab_identity_reload_" + NLS::Guid::New().ToString() + ".scene");

    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = Id("f2010101-0101-4101-8101-010101010101");
    artifact.generatedModelPrefab = true;
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;
    artifact.graph.documentId = NLS::Guid::NewDeterministic("PrefabIdentityReload.Document");

    const auto rootId = ObjectId(NLS::Guid::NewDeterministic("PrefabIdentityReload.Root"));
    artifact.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "ReloadedGeneratedRoot";
    root.debugPath = "/ReloadedGeneratedRoot";
    root.properties.push_back({"name", PropertyValue::String("ReloadedGeneratedRoot")});
    root.properties.push_back({"tag", PropertyValue::String("Model")});
    root.properties.push_back({"components", PropertyValue::Array({})});
    root.properties.push_back({"children", PropertyValue::Array({})});
    root.properties.push_back({"parent", PropertyValue::Null()});
    artifact.graph.objects.push_back(std::move(root));

    NLS::Engine::SceneSystem::Scene sourceScene;
    auto instantiate = PrefabUtilityFacade().InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:ReloadedGeneratedRoot"
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());

    ASSERT_TRUE(NLS::Engine::SceneSystem::SceneManager::SaveSceneToPath(sourceScene, scenePath.string()));

    NLS::Engine::SceneSystem::SceneManager loadedManager;
    ASSERT_TRUE(loadedManager.LoadScene(scenePath.string(), true));

    auto* loadedRoot = loadedManager.GetCurrentScene()->FindGameObjectByName("ReloadedGeneratedRoot");
    ASSERT_NE(loadedRoot, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    PrefabUtilityFacade facade;
    EXPECT_EQ(
        facade.GetPrefabInstanceStatus({registry.FindInstance(*loadedRoot), true, false}),
        PrefabInstanceStatus::NotAPrefab);

    std::filesystem::remove(scenePath);
}

TEST(PrefabUtilityFacadeTests, AnnotatesSceneDocumentWithConnectedPrefabRootMetadata)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("SceneLinkedPrefab", Id("f3010101-0101-4101-8101-010101010101"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:SceneLinkedPrefab",
        Id("f3020202-0202-4202-8202-020202020202")
    }, scene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    registry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, scene, registry);

    const auto* sceneRecord = FindObjectRecord(document, document.root);
    ASSERT_NE(sceneRecord, nullptr);
    ASSERT_FALSE(sceneRecord->properties.empty());
    ASSERT_EQ(sceneRecord->properties.front().name, "gameObjects");
    ASSERT_EQ(sceneRecord->properties.front().value.GetKind(), PropertyValue::Kind::Array);
    ASSERT_FALSE(sceneRecord->properties.front().value.GetArray().empty());

    ASSERT_EQ(document.prefabInstances.size(), 1u);
    EXPECT_EQ(document.prefabInstances[0].sourcePrefab.guid, prefab.assetId.GetGuid());
    EXPECT_EQ(document.prefabInstances[0].sourcePrefab.filePath, "prefab:SceneLinkedPrefab");
    EXPECT_FALSE(document.prefabInstances[0].correspondence.empty());

    const auto* rootRecord = FindObjectRecord(
        document,
        sceneRecord->properties.front().value.GetArray().front());
    ASSERT_NE(rootRecord, nullptr);
    EXPECT_EQ(FindProperty(*rootRecord, "scenePrefab"), nullptr);
}

TEST(PrefabUtilityFacadeTests, AnnotatesSceneDocumentWithUnityStylePrefabInstanceRecord)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("UnityStyleScenePrefab", Id("f4171717-1717-4717-8717-171717171717"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:UnityStyleScenePrefab",
        Id("f4181818-1818-4818-8818-181818181818")
    }, scene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->SetName("SceneOnlyUnityStyleOverride");
    registry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, scene, registry);

    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
    EXPECT_NE(output.find("\"prefabInstances\""), std::string::npos);
    EXPECT_NE(output.find("\"sourcePrefab\""), std::string::npos);
    EXPECT_NE(output.find("\"instanceRoot\""), std::string::npos);
    EXPECT_NE(output.find("\"modifications\""), std::string::npos);
    EXPECT_NE(output.find("\"correspondence\""), std::string::npos);
    EXPECT_NE(output.find("\"filePath\": \"prefab:UnityStyleScenePrefab\""), std::string::npos);
    EXPECT_NE(output.find("SceneOnlyUnityStyleOverride"), std::string::npos);
    EXPECT_EQ(output.find("\"scenePrefab\""), std::string::npos);
}

TEST(PrefabUtilityFacadeTests, UnityStylePrefabInstanceRecordRoundTripsThroughReaderAndWriter)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("RoundTripScenePrefab", Id("f4191919-1919-4919-8919-191919191919"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:RoundTripScenePrefab",
        Id("f41a1a1a-1a1a-4a1a-8a1a-1a1a1a1a1a1a")
    }, scene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->SetName("RoundTrippedSceneOverride");
    registry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, scene, registry);
    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
    const auto parsed = NLS::Engine::Serialize::ObjectGraphReader::Read(output);

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->prefabInstances.size(), 1u);
    EXPECT_EQ(parsed->prefabInstances[0].sourcePrefab.guid, prefab.assetId.GetGuid());
    EXPECT_EQ(parsed->prefabInstances[0].sourcePrefab.filePath, "prefab:RoundTripScenePrefab");
    EXPECT_TRUE(parsed->prefabInstances[0].generatedReadOnly);
    EXPECT_FALSE(parsed->prefabInstances[0].modifications.empty());
    EXPECT_FALSE(parsed->prefabInstances[0].correspondence.empty());
}

TEST(PrefabUtilityFacadeTests, UnityStylePrefabInstanceRestoresFromStrippedScenePlaceholder)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("StrippedScenePrefab", Id("f4212121-2121-4121-8121-212121212121"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:StrippedScenePrefab",
        Id("f4222222-2222-4222-8222-222222222222")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->SetName("StrippedSceneOverride");
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);

    const auto* strippedRootRecord = FindObjectRecord(document, document.prefabInstances[0].instanceRoot);
    ASSERT_NE(strippedRootRecord, nullptr);
    EXPECT_EQ(strippedRootRecord->state, ObjectRecordState::Stripped);

    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
    EXPECT_NE(output.find("\"state\": \"Stripped\""), std::string::npos);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("StrippedSceneOverride"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4232323-2323-4323-8323-232323232323"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:StrippedScenePrefab")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(restore);
    auto* loadedRoot = loadedScene->FindGameObjectByName("StrippedSceneOverride");
    ASSERT_NE(loadedRoot, nullptr);
    auto* restored = restoredRegistry.FindInstance(*loadedRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->generatedReadOnly);
    EXPECT_FALSE(restored->localPatches.empty());
}

TEST(PrefabUtilityFacadeTests, RestoresConnectedPrefabRegistryEntriesFromAnnotatedSceneDocument)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("SceneRestoredPrefab", Id("f4010101-0101-4101-8101-010101010101"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:SceneRestoredPrefab",
        Id("f4020202-0202-4202-8202-020202020202")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("SceneRestoredPrefab"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4030303-0303-4303-8303-030303030303"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:SceneRestoredPrefab")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("SceneRestoredPrefab");
    ASSERT_NE(loadedRoot, nullptr);
    auto* restored = restoredRegistry.FindInstance(*loadedRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->prefabAssetId, prefab.assetId);
    EXPECT_EQ(restored->prefabSubAssetKey, "prefab:SceneRestoredPrefab");
    EXPECT_TRUE(restored->generatedReadOnly);
}

TEST(PrefabUtilityFacadeTests, RestoresNormalPrefabSceneLocalOverrideFromPrefabInstanceRecord)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("NormalOverrideScenePrefab", Id("f41b1b1b-1b1b-4b1b-8b1b-1b1b1b1b1b1b"));

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:NormalOverrideScenePrefab",
        Id("f41c1c1c-1c1c-4c1c-8c1c-1c1c1c1c1c1c")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->SetName("NormalSceneLocalName");
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].modifications.empty());

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("NormalSceneLocalName"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f41d1d1d-1d1d-4d1d-8d1d-1d1d1d1d1d1d"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:NormalOverrideScenePrefab")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("NormalSceneLocalName");
    ASSERT_NE(loadedRoot, nullptr);
    auto* restored = restoredRegistry.FindInstance(*loadedRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->generatedReadOnly);
    EXPECT_FALSE(restored->localPatches.empty());
    EXPECT_EQ(ReadStringProperty(prefab, "name"), "NormalOverrideScenePrefab");
}

TEST(PrefabUtilityFacadeTests, RestoresPrefabSceneLocalTransformOverrideFromPrefabInstanceRecord)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("TransformOverrideScenePrefab", Id("f4242424-2424-4424-8424-242424242424"));

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:TransformOverrideScenePrefab",
        Id("f4252525-2525-4525-8525-252525252525")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->GetTransform()->SetLocalPosition({7.0f, 3.0f, -2.0f});
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].modifications.empty());
    EXPECT_TRUE(std::any_of(
        document.prefabInstances[0].modifications.begin(),
        document.prefabInstances[0].modifications.end(),
        [](const auto& modification)
        {
            return modification.type == NLS::Engine::Serialize::PatchOperationType::ReplaceProperty &&
                modification.property == "localPosition";
        }));

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4262626-2626-4626-8626-262626262626"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:TransformOverrideScenePrefab")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("TransformOverrideScenePrefab");
    ASSERT_NE(loadedRoot, nullptr);
    const auto& restoredPosition = loadedRoot->GetTransform()->GetLocalPosition();
    EXPECT_FLOAT_EQ(restoredPosition.x, 7.0f);
    EXPECT_FLOAT_EQ(restoredPosition.y, 3.0f);
    EXPECT_FLOAT_EQ(restoredPosition.z, -2.0f);
    EXPECT_NE(restoredRegistry.FindInstance(*loadedRoot), nullptr);
}

TEST(PrefabUtilityFacadeTests, LiveRootReconnectAppliesPrefabSceneLocalTransformOverride)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("LiveTransformOverridePrefab", Id("f4272727-2727-4727-8727-272727272727"));

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:LiveTransformOverridePrefab",
        Id("f4282828-2828-4828-8828-282828282828")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->GetTransform()->SetLocalPosition({11.0f, 4.0f, -6.0f});
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].modifications.empty());

    auto* rootRecord = const_cast<NLS::Engine::Serialize::ObjectRecord*>(
        FindGameObjectRecordByName(document, "LiveTransformOverridePrefab"));
    ASSERT_NE(rootRecord, nullptr);
    rootRecord->state = ObjectRecordState::Alive;

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    auto* liveRoot = loadedScene->FindGameObjectByName("LiveTransformOverridePrefab");
    ASSERT_NE(liveRoot, nullptr);
    liveRoot->GetTransform()->SetLocalPosition({0.0f, 0.0f, 0.0f});

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4292929-2929-4929-8929-292929292929"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:LiveTransformOverridePrefab")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(restore);
    const auto& restoredPosition = liveRoot->GetTransform()->GetLocalPosition();
    EXPECT_FLOAT_EQ(restoredPosition.x, 11.0f);
    EXPECT_FLOAT_EQ(restoredPosition.y, 4.0f);
    EXPECT_FLOAT_EQ(restoredPosition.z, -6.0f);
    auto* restored = restoredRegistry.FindInstance(*liveRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->localPatches.empty());
    const auto& restoredSourceGraph = restored->SourceGraph();
    const auto* restoredSourceRoot = FindObjectRecord(restoredSourceGraph, restoredSourceGraph.root);
    ASSERT_NE(restoredSourceRoot, nullptr);
    const auto restoredSourceComponents = ReadOwnedReferences(*restoredSourceRoot, "components");
    ASSERT_FALSE(restoredSourceComponents.empty());
    const auto* restoredSourceTransform = FindObjectRecord(restoredSourceGraph, restoredSourceComponents.front());
    ASSERT_NE(restoredSourceTransform, nullptr);
    const auto* sourcePosition = FindProperty(*restoredSourceTransform, "localPosition");
    ASSERT_NE(sourcePosition, nullptr);
    const auto* sourceX = FindObjectProperty(sourcePosition->value, "x");
    ASSERT_NE(sourceX, nullptr);
    EXPECT_DOUBLE_EQ(sourceX->GetNumber(), 0.0)
        << "The restored registry must keep the original prefab source graph, not the materialized scene override graph.";
}

TEST(PrefabUtilityFacadeTests, RestoresPrefabSceneLocalAddedChildOverrideFromPrefabInstanceRecord)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("StructuralOverrideScenePrefab", Id("f42a2a2a-2a2a-4a2a-8a2a-2a2a2a2a2a2a"));

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:StructuralOverrideScenePrefab",
        Id("f42b2b2b-2b2b-4b2b-8b2b-2b2b2b2b2b2b")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    auto* addedChild = new NLS::Engine::GameObject("SceneOnlyAddedChild", "PrefabOverride");
    addedChild->SetParent(*instantiate.instance->instanceRoot);
    sourceScene.AddGameObject(addedChild);
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].modifications.empty());

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("StructuralOverrideScenePrefab"), nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("SceneOnlyAddedChild"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f42c2c2c-2c2c-4c2c-8c2c-2c2c2c2c2c2c"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:StructuralOverrideScenePrefab")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("StructuralOverrideScenePrefab");
    ASSERT_NE(loadedRoot, nullptr);
    auto* loadedChild = loadedScene->FindGameObjectByName("SceneOnlyAddedChild");
    ASSERT_NE(loadedChild, nullptr);
    EXPECT_EQ(loadedChild->GetParent(), loadedRoot);

    auto* restored = restoredRegistry.FindInstance(*loadedRoot);
    ASSERT_NE(restored, nullptr);
    const auto overrides = facade.GetPrefabOverrides(prefab, *restored, true);
    EXPECT_NE(FindOverride(overrides, PrefabOverrideKind::AddedGameObject, "children"), nullptr);
}

TEST(PrefabUtilityFacadeTests, LiveRootReconnectPreservesStructuralPrefabInstancePayload)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("LiveStructuralOverridePrefab", Id("f4404040-4040-4040-8040-404040404040"));

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:LiveStructuralOverridePrefab",
        Id("f4414141-4141-4141-8141-414141414141")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    auto* addedChild = new NLS::Engine::GameObject("LiveSceneOnlyAddedChild", "PrefabOverride");
    addedChild->SetParent(*instantiate.instance->instanceRoot);
    sourceScene.AddGameObject(addedChild);
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].addedObjects.empty());
    ASSERT_FALSE(document.prefabInstances[0].correspondence.empty());

    auto* rootRecord = const_cast<NLS::Engine::Serialize::ObjectRecord*>(
        FindGameObjectRecordByName(document, "LiveStructuralOverridePrefab"));
    ASSERT_NE(rootRecord, nullptr);
    rootRecord->state = ObjectRecordState::Alive;

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    auto* liveRoot = loadedScene->FindGameObjectByName("LiveStructuralOverridePrefab");
    ASSERT_NE(liveRoot, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("LiveSceneOnlyAddedChild"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4424242-4242-4242-8242-424242424242"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:LiveStructuralOverridePrefab")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(restore);
    auto* restored = restoredRegistry.FindInstance(*liveRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->preservedAddedObjects.empty());
    EXPECT_FALSE(restored->preservedCorrespondence.empty());

    auto rewritten = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(*loadedScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(rewritten, *loadedScene, restoredRegistry);

    ASSERT_EQ(rewritten.prefabInstances.size(), 1u);
    EXPECT_FALSE(rewritten.prefabInstances[0].addedObjects.empty());
    EXPECT_FALSE(rewritten.prefabInstances[0].correspondence.empty());
    const auto rewrittenDiagnostics = rewritten.Validate();
    EXPECT_FALSE(rewrittenDiagnostics.HasErrors()) << JoinSerializationDiagnostics(rewrittenDiagnostics);
}

TEST(PrefabUtilityFacadeTests, RestoresRemovedPrefabChildSubtreeFromPrefabInstanceRecord)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject prefabRoot("RemovedSubtreeScenePrefab", "Prefab");
    auto* child = new NLS::Engine::GameObject("RemovedSubtreeChild", "Prefab");
    auto* grandchild = new NLS::Engine::GameObject("RemovedSubtreeGrandchild", "Prefab");
    grandchild->SetParent(*child);
    child->SetParent(prefabRoot);

    auto save = facade.SaveAsPrefabAsset(
        prefabRoot,
        Id("f43d3d3d-3d3d-4d3d-8d3d-3d3d3d3d3d3d"),
        "Assets/Prefabs/RemovedSubtreeScenePrefab.prefab");
    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());
    child->DetachFromParent();
    grandchild->DetachFromParent();
    delete grandchild;
    delete child;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &*save.artifact,
        save.artifact->assetId,
        "prefab:RemovedSubtreeScenePrefab",
        Id("f43e3e3e-3e3e-4e3e-8e3e-3e3e3e3e3e3e")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    auto* removedChild = FindChildByName(*instantiate.instance->instanceRoot, "RemovedSubtreeChild");
    ASSERT_NE(removedChild, nullptr);
    ASSERT_TRUE(sourceScene.DestroyGameObject(*removedChild));
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_TRUE(std::any_of(
        document.prefabInstances[0].modifications.begin(),
        document.prefabInstances[0].modifications.end(),
        [](const auto& operation)
        {
            return operation.type == NLS::Engine::Serialize::PatchOperationType::RemoveObject;
        }));

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f43f3f3f-3f3f-4f3f-8f3f-3f3f3f3f3f3f"),
        restoredRegistry,
        [&save](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == save.artifact->assetId && subAssetKey == "prefab:RemovedSubtreeScenePrefab")
                return *save.artifact;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(restore);
    ASSERT_NE(loadedScene->FindGameObjectByName("RemovedSubtreeScenePrefab"), nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("RemovedSubtreeChild"), nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("RemovedSubtreeGrandchild"), nullptr);
}

TEST(PrefabUtilityFacadeTests, RestoresGeneratedModelSceneLocalOverrideAndStillRejectsApplyToAsset)
{
    PrefabUtilityFacade facade;
    auto generated = MakePrefabArtifact("GeneratedOverrideScenePrefab", Id("f41e1e1e-1e1e-4e1e-8e1e-1e1e1e1e1e1e"));
    generated.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &generated,
        generated.assetId,
        "prefab:GeneratedOverrideScenePrefab",
        Id("f41f1f1f-1f1f-4f1f-8f1f-1f1f1f1f1f1f")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->SetName("GeneratedSceneLocalName");
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].modifications.empty());

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("GeneratedSceneLocalName"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4202020-2020-4020-8020-202020202020"),
        restoredRegistry,
        [&generated](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == generated.assetId && subAssetKey == "prefab:GeneratedOverrideScenePrefab")
                return generated;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("GeneratedSceneLocalName");
    ASSERT_NE(loadedRoot, nullptr);
    auto* restored = restoredRegistry.FindInstance(*loadedRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->generatedReadOnly);
    EXPECT_FALSE(restored->localPatches.empty());

    const auto overrides = facade.GetPrefabOverrides(generated, *restored, true);
    const auto* nameOverride = FindOverride(overrides, PrefabOverrideKind::Property, "name");
    ASSERT_NE(nameOverride, nullptr);
    auto applyToGenerated = facade.ApplySingleOverride(generated, *nameOverride);
    EXPECT_EQ(applyToGenerated.status, PrefabOperationStatus::Rejected);
    EXPECT_EQ(ReadStringProperty(generated, "name"), "GeneratedOverrideScenePrefab");
}

TEST(PrefabUtilityFacadeTests, PreservesScenePrefabMetadataWhenRestoreArtifactIsMissing)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("MissingScenePrefab", Id("f4070707-0707-4707-8707-070707070707"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:MissingScenePrefab",
        Id("f4080808-0808-4808-8808-080808080808")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("MissingScenePrefab"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4090909-0909-4909-8909-090909090909"),
        restoredRegistry,
        [](AssetId, const std::string&) -> std::optional<PrefabArtifact>
        {
            return std::nullopt;
        });

    EXPECT_EQ(restore.status, PrefabOperationStatus::Failed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("MissingScenePrefab (missing)");
    ASSERT_NE(loadedRoot, nullptr);
    auto* restored = restoredRegistry.FindInstance(*loadedRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->prefabAssetId, prefab.assetId);
    EXPECT_EQ(restored->prefabSubAssetKey, "prefab:MissingScenePrefab");
    EXPECT_TRUE(restored->generatedReadOnly);
    EXPECT_TRUE(restoredRegistry.GetPresentation(*loadedRoot).missingAsset);

    auto rewritten = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(*loadedScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(rewritten, *loadedScene, restoredRegistry);

    ASSERT_EQ(rewritten.prefabInstances.size(), 1u);
    EXPECT_EQ(rewritten.prefabInstances[0].sourcePrefab.guid, prefab.assetId.GetGuid());
    EXPECT_EQ(rewritten.prefabInstances[0].sourcePrefab.filePath, "prefab:MissingScenePrefab");
    EXPECT_TRUE(rewritten.prefabInstances[0].generatedReadOnly);
}

TEST(PrefabUtilityFacadeTests, MissingScenePrefabRestoreMarksNameAndSuppressesStaleRenderer)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("MissingRenderablePrefab", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);

    const auto meshAssetId = Id("f4070a0a-0a0a-4a0a-8a0a-0a0a0a0a0a0a");
    const auto materialAssetId = Id("f4070b0b-0b0b-4b0b-8b0b-0b0b0b0b0b0b");
    const std::string meshArtifactPath = ContentArtifactPath("MissingRenderable", "c001");
    const std::string materialArtifactPath = ContentArtifactPath("MissingRenderable", "c002");
    const auto meshReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(meshAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshAssetId.GetGuid(), meshArtifactPath),
        meshArtifactPath);
    const auto materialReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(materialAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(materialAssetId.GetGuid(), materialArtifactPath),
        materialArtifactPath);
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(meshReference));
    meshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(materialReference)
    });

    auto save = facade.SaveAsPrefabAsset(
        root,
        Id("f4070c0c-0c0c-4c0c-8c0c-0c0c0c0c0c0c"),
        "Assets/Prefabs/MissingRenderablePrefab.prefab");
    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());
    auto prefab = *save.artifact;
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:MissingRenderablePrefab",
        Id("f4070d0d-0d0d-4d0d-8d0d-0d0d0d0d0d0d")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4070e0e-0e0e-4e0e-8e0e-0e0e0e0e0e0e"),
        restoredRegistry,
        [](AssetId, const std::string&) -> std::optional<PrefabArtifact>
        {
            return std::nullopt;
        });

    EXPECT_EQ(restore.status, PrefabOperationStatus::Failed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("MissingRenderablePrefab (missing)");
    ASSERT_NE(loadedRoot, nullptr);
    EXPECT_TRUE(restoredRegistry.GetPresentation(*loadedRoot).missingAsset);
    EXPECT_EQ(loadedScene->FindGameObjectByName("MissingRenderablePrefab"), nullptr);
    auto* restoredMeshFilter = loadedRoot->GetComponent<NLS::Engine::Components::MeshFilter>();
    auto* restoredMeshRenderer = loadedRoot->GetComponent<NLS::Engine::Components::MeshRenderer>();
    EXPECT_TRUE(restoredMeshFilter == nullptr || restoredMeshFilter->GetModelPath().empty());
    EXPECT_TRUE(restoredMeshFilter == nullptr || restoredMeshFilter->ResolveMesh() == nullptr)
        << "A missing prefab placeholder must not keep resolving stale mesh data after the source prefab file is gone.";
    EXPECT_TRUE(restoredMeshRenderer == nullptr || restoredMeshRenderer->GetMaterialPaths().empty());
    EXPECT_TRUE(restoredMeshRenderer == nullptr || restoredMeshRenderer->IsTransientRenderingSuppressed());
}

TEST(PrefabUtilityFacadeTests, MissingScenePrefabRestorePreservesSceneLocalTransform)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact(
        "MissingTransformPrefab",
        Id("f4071414-1414-4f14-8f14-141414141414"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:MissingTransformPrefab",
        Id("f4071515-1515-4f15-8f15-151515151515")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->GetTransform()->SetLocalPosition({9.0f, 5.0f, -4.0f});
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].modifications.empty());

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4071616-1616-4f16-8f16-161616161616"),
        restoredRegistry,
        [](AssetId, const std::string&) -> std::optional<PrefabArtifact>
        {
            return std::nullopt;
        });

    EXPECT_EQ(restore.status, PrefabOperationStatus::Failed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("MissingTransformPrefab (missing)");
    ASSERT_NE(loadedRoot, nullptr);
    const auto& restoredPosition = loadedRoot->GetTransform()->GetLocalPosition();
    EXPECT_FLOAT_EQ(restoredPosition.x, 9.0f);
    EXPECT_FLOAT_EQ(restoredPosition.y, 5.0f);
    EXPECT_FLOAT_EQ(restoredPosition.z, -4.0f);
    EXPECT_TRUE(restoredRegistry.GetPresentation(*loadedRoot).missingAsset);
}

TEST(PrefabUtilityFacadeTests, MissingScenePrefabRestoreIgnoresChildTransformOverrideForRoot)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject prefabRoot("MissingRootWithChildOverride", "Prefab");
    auto* prefabChild = new NLS::Engine::GameObject("MissingChildWithOverride", "Prefab");
    prefabChild->SetParent(prefabRoot);

    auto save = facade.SaveAsPrefabAsset(
        prefabRoot,
        Id("f4071717-1717-4f17-8f17-171717171717"),
        "Assets/Prefabs/MissingRootWithChildOverride.prefab");
    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());
    save.artifact->generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &*save.artifact,
        save.artifact->assetId,
        "prefab:MissingRootWithChildOverride",
        Id("f4071818-1818-4f18-8f18-181818181818")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    instantiate.instance->instanceRoot->GetTransform()->SetLocalPosition({9.0f, 5.0f, -4.0f});
    auto* child = FindChildByName(*instantiate.instance->instanceRoot, "MissingChildWithOverride");
    ASSERT_NE(child, nullptr);
    child->GetTransform()->SetLocalPosition({99.0f, 88.0f, 77.0f});
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    const auto transformOverrideCount = std::count_if(
        document.prefabInstances[0].modifications.begin(),
        document.prefabInstances[0].modifications.end(),
        [](const auto& modification)
        {
            return modification.type == NLS::Engine::Serialize::PatchOperationType::ReplaceProperty &&
                modification.property == "localPosition";
        });
    ASSERT_GE(transformOverrideCount, 2)
        << "The regression requires both root and child transform overrides in the same missing prefab instance.";

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4071919-1919-4f19-8f19-191919191919"),
        restoredRegistry,
        [](AssetId, const std::string&) -> std::optional<PrefabArtifact>
        {
            return std::nullopt;
        });

    EXPECT_EQ(restore.status, PrefabOperationStatus::Failed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("MissingRootWithChildOverride (missing)");
    ASSERT_NE(loadedRoot, nullptr);
    const auto& restoredPosition = loadedRoot->GetTransform()->GetLocalPosition();
    EXPECT_FLOAT_EQ(restoredPosition.x, 9.0f);
    EXPECT_FLOAT_EQ(restoredPosition.y, 5.0f);
    EXPECT_FLOAT_EQ(restoredPosition.z, -4.0f);
}

TEST(PrefabUtilityFacadeTests, MissingScenePrefabRestoreDoesNotSuppressUnrelatedPrefabInstance)
{
    PrefabUtilityFacade facade;
    auto missingPrefab = MakePrefabArtifact(
        "MissingOnlyPrefab",
        Id("f4070f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f"));
    missingPrefab.generatedModelPrefab = true;

    NLS::Engine::GameObject visibleRoot("VisiblePrefab", "Prefab");
    auto* visibleRenderer = visibleRoot.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(visibleRenderer, nullptr);
    auto visibleSave = facade.SaveAsPrefabAsset(
        visibleRoot,
        Id("f4071010-1010-4f10-8f10-101010101010"),
        "Assets/Prefabs/VisiblePrefab.prefab");
    ASSERT_EQ(visibleSave.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(visibleSave.artifact.has_value());
    auto visiblePrefab = *visibleSave.artifact;
    visiblePrefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto missingInstance = facade.InstantiatePrefab({
        &missingPrefab,
        missingPrefab.assetId,
        "prefab:MissingOnlyPrefab",
        Id("f4071111-1111-4f11-8f11-111111111111")
    }, sourceScene);
    ASSERT_EQ(missingInstance.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(missingInstance.instance.has_value());
    sourceRegistry.Register(*missingInstance.instance);

    auto visibleInstance = facade.InstantiatePrefab({
        &visiblePrefab,
        visiblePrefab.assetId,
        "prefab:VisiblePrefab",
        Id("f4071212-1212-4f12-8f12-121212121212")
    }, sourceScene);
    ASSERT_EQ(visibleInstance.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(visibleInstance.instance.has_value());
    sourceRegistry.Register(*visibleInstance.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4071313-1313-4f13-8f13-131313131313"),
        restoredRegistry,
        [&](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == visiblePrefab.assetId && subAssetKey == "prefab:VisiblePrefab")
                return visiblePrefab;
            return std::nullopt;
        });

    EXPECT_EQ(restore.status, PrefabOperationStatus::Failed);
    auto* missingRoot = loadedScene->FindGameObjectByName("MissingOnlyPrefab (missing)");
    auto* visibleLoadedRoot = loadedScene->FindGameObjectByName("VisiblePrefab");
    ASSERT_NE(missingRoot, nullptr);
    ASSERT_NE(visibleLoadedRoot, nullptr);
    EXPECT_TRUE(restoredRegistry.GetPresentation(*missingRoot).missingAsset);

    const auto visiblePresentation = restoredRegistry.GetPresentation(*visibleLoadedRoot);
    EXPECT_EQ(visiblePresentation.state, NLS::Editor::Assets::PrefabHierarchyState::Root);
    EXPECT_EQ(visiblePresentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedRoot);
    EXPECT_FALSE(visiblePresentation.missingAsset)
        << "A missing prefab source must not poison unrelated prefab instances in the same scene.";
    auto* restoredVisibleRenderer = visibleLoadedRoot->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(restoredVisibleRenderer, nullptr);
    EXPECT_FALSE(restoredVisibleRenderer->IsTransientRenderingSuppressed());
}

TEST(PrefabUtilityFacadeTests, PreservesStructuralPrefabInstancePayloadWhenRestoreArtifactIsMissing)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject prefabRoot("MissingStructuralScenePrefab", "Prefab");
    auto* sourceChild = new NLS::Engine::GameObject("MissingStructuralSourceChild", "Prefab");
    sourceChild->SetParent(prefabRoot);
    auto save = facade.SaveAsPrefabAsset(
        prefabRoot,
        Id("f43a3a3a-3a3a-4a3a-8a3a-3a3a3a3a3a3a"),
        "Assets/Prefabs/MissingStructuralScenePrefab.prefab");
    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());
    sourceChild->DetachFromParent();
    delete sourceChild;
    auto prefab = *save.artifact;
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:MissingStructuralScenePrefab",
        Id("f43b3b3b-3b3b-4b3b-8b3b-3b3b3b3b3b3b")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    auto* addedChild = new NLS::Engine::GameObject("MissingStructuralSceneOnlyChild", "PrefabOverride");
    addedChild->SetParent(*instantiate.instance->instanceRoot);
    sourceScene.AddGameObject(addedChild);
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].modifications.empty());
    ASSERT_FALSE(document.prefabInstances[0].addedObjects.empty());
    ASSERT_GT(document.prefabInstances[0].correspondence.size(), 1u);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f43c3c3c-3c3c-4c3c-8c3c-3c3c3c3c3c3c"),
        restoredRegistry,
        [](AssetId, const std::string&) -> std::optional<PrefabArtifact>
        {
            return std::nullopt;
        });

    EXPECT_EQ(restore.status, PrefabOperationStatus::Failed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("MissingStructuralScenePrefab (missing)");
    ASSERT_NE(loadedRoot, nullptr);
    auto* restored = restoredRegistry.FindInstance(*loadedRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->localPatches.empty());
    EXPECT_FALSE(restored->preservedAddedObjects.empty());
    EXPECT_FALSE(restored->preservedCorrespondence.empty());

    auto rewritten = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(*loadedScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(rewritten, *loadedScene, restoredRegistry);

    ASSERT_EQ(rewritten.prefabInstances.size(), 1u);
    EXPECT_EQ(rewritten.prefabInstances[0].sourcePrefab.guid, prefab.assetId.GetGuid());
    EXPECT_EQ(rewritten.prefabInstances[0].sourcePrefab.filePath, "prefab:MissingStructuralScenePrefab");
    EXPECT_FALSE(rewritten.prefabInstances[0].modifications.empty());
    EXPECT_FALSE(rewritten.prefabInstances[0].addedObjects.empty());
    EXPECT_FALSE(rewritten.prefabInstances[0].correspondence.empty());
    const auto rewrittenDiagnostics = rewritten.Validate();
    EXPECT_FALSE(rewrittenDiagnostics.HasErrors()) << JoinSerializationDiagnostics(rewrittenDiagnostics);
}

TEST(PrefabUtilityFacadeTests, MissingPrefabRestoreStripsAddedOverrideDescendants)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("MissingAddedSubtreePrefab", Id("f4071a1a-1a1a-4f1a-8f1a-1a1a1a1a1a1a"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:MissingAddedSubtreePrefab",
        Id("f4071b1b-1b1b-4f1b-8f1b-1b1b1b1b1b1b")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);

    auto* addedChild = new NLS::Engine::GameObject("SceneOnlyAddedChildWithGrandchild", "PrefabOverride");
    auto* addedGrandchild = new NLS::Engine::GameObject("SceneOnlyAddedGrandchild", "PrefabOverride");
    addedGrandchild->SetParent(*addedChild);
    addedChild->SetParent(*instantiate.instance->instanceRoot);
    sourceScene.AddGameObject(addedChild);
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    ASSERT_FALSE(document.prefabInstances[0].addedObjects.empty());

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4071c1c-1c1c-4f1c-8f1c-1c1c1c1c1c1c"),
        restoredRegistry,
        [](AssetId, const std::string&) -> std::optional<PrefabArtifact>
        {
            return std::nullopt;
        });

    EXPECT_EQ(restore.status, PrefabOperationStatus::Failed);
    EXPECT_NE(loadedScene->FindGameObjectByName("MissingAddedSubtreePrefab (missing)"), nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("SceneOnlyAddedChildWithGrandchild"), nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("SceneOnlyAddedGrandchild"), nullptr)
        << "All descendants of a scene-added prefab override subtree must be stripped from the scene placeholder graph.";
}

TEST(PrefabUtilityFacadeTests, PreservesScenePrefabMetadataWhenRestoreConnectFails)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("DisconnectedScenePrefab", Id("f4101010-1010-4010-8010-101010101010"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:DisconnectedScenePrefab",
        Id("f4111111-1111-4111-8111-111111111111")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("DisconnectedScenePrefab"), nullptr);

    auto corruptPrefab = prefab;
    corruptPrefab.graph.root = ObjectId();

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4121212-1212-4212-8212-121212121212"),
        restoredRegistry,
        [&corruptPrefab](AssetId, const std::string&) -> std::optional<PrefabArtifact>
        {
            return corruptPrefab;
        });

    EXPECT_EQ(restore.status, PrefabOperationStatus::Failed);
    auto* loadedRoot = loadedScene->FindGameObjectByName("DisconnectedScenePrefab (missing)");
    ASSERT_NE(loadedRoot, nullptr);
    auto* restored = restoredRegistry.FindInstance(*loadedRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->prefabAssetId, prefab.assetId);
    EXPECT_EQ(restored->prefabSubAssetKey, "prefab:DisconnectedScenePrefab");
    EXPECT_TRUE(restored->generatedReadOnly);
    EXPECT_TRUE(restoredRegistry.GetPresentation(*loadedRoot).missingAsset);

    auto rewritten = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(*loadedScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(rewritten, *loadedScene, restoredRegistry);

    ASSERT_EQ(rewritten.prefabInstances.size(), 1u);
    EXPECT_EQ(rewritten.prefabInstances[0].sourcePrefab.guid, prefab.assetId.GetGuid());
    EXPECT_EQ(rewritten.prefabInstances[0].sourcePrefab.filePath, "prefab:DisconnectedScenePrefab");
    EXPECT_TRUE(rewritten.prefabInstances[0].generatedReadOnly);
}

TEST(PrefabUtilityFacadeTests, RestoresPrefabRegistryBySceneRootOrderWhenNamesCollide)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("DuplicateName", Id("f4040404-0404-4404-8404-040404040404"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    auto& plainRoot = sourceScene.CreateGameObject("DuplicateName", "Plain");

    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:DuplicateName",
        Id("f4050505-0505-4505-8505-050505050505")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, &plainRoot);
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    auto matches = loadedScene->FindGameObjectsByName("DuplicateName");
    ASSERT_EQ(matches.size(), 1u);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4060606-0606-4606-8606-060606060606"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:DuplicateName")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    matches = loadedScene->FindGameObjectsByName("DuplicateName");
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(restoredRegistry.FindInstance(matches[0].get()), nullptr);
    auto* restored = restoredRegistry.FindInstance(matches[1].get());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->prefabAssetId, prefab.assetId);
    EXPECT_EQ(restored->prefabSubAssetKey, "prefab:DuplicateName");
}

TEST(PrefabUtilityFacadeTests, RestoresMixedUnityStyleAndLegacyScenePrefabMetadata)
{
    PrefabUtilityFacade facade;
    auto unityStylePrefab = MakePrefabArtifact("MixedUnityStylePrefab", Id("f42d2d2d-2d2d-4d2d-8d2d-2d2d2d2d2d2d"));
    unityStylePrefab.generatedModelPrefab = true;
    auto legacyPrefab = MakePrefabArtifact("MixedLegacyPrefab", Id("f42e2e2e-2e2e-4e2e-8e2e-2e2e2e2e2e2e"));

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto unityStyle = facade.InstantiatePrefab({
        &unityStylePrefab,
        unityStylePrefab.assetId,
        "prefab:MixedUnityStylePrefab",
        Id("f42f2f2f-2f2f-4f2f-8f2f-2f2f2f2f2f2f")
    }, sourceScene);
    ASSERT_EQ(unityStyle.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(unityStyle.instance.has_value());
    sourceRegistry.Register(*unityStyle.instance);

    auto legacy = facade.InstantiatePrefab({
        &legacyPrefab,
        legacyPrefab.assetId,
        "prefab:MixedLegacyPrefab",
        Id("f4303030-3030-4030-8030-303030303030")
    }, sourceScene);
    ASSERT_EQ(legacy.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(legacy.instance.has_value());

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);

    auto* legacyRootRecord = const_cast<NLS::Engine::Serialize::ObjectRecord*>(
        FindGameObjectRecordByName(document, "MixedLegacyPrefab"));
    ASSERT_NE(legacyRootRecord, nullptr);
    legacyRootRecord->properties.push_back({
        "scenePrefab",
        PropertyValue::ObjectReference(ObjectIdentifier::Asset(
            NLS::Engine::Serialize::AssetId(legacyPrefab.assetId.GetGuid()),
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                legacyPrefab.assetId.GetGuid(),
                "prefab:MixedLegacyPrefab"),
            "prefab:MixedLegacyPrefab"))
    });
    legacyRootRecord->properties.push_back({
        "scenePrefabGeneratedReadOnly",
        PropertyValue::Bool(false)
    });

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("MixedUnityStylePrefab"), nullptr);
    ASSERT_NE(loadedScene->FindGameObjectByName("MixedLegacyPrefab"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4313131-3131-4131-8131-313131313131"),
        restoredRegistry,
        [&](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == unityStylePrefab.assetId && subAssetKey == "prefab:MixedUnityStylePrefab")
                return unityStylePrefab;
            if (assetId == legacyPrefab.assetId && subAssetKey == "prefab:MixedLegacyPrefab")
                return legacyPrefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    auto* loadedUnityStyleRoot = loadedScene->FindGameObjectByName("MixedUnityStylePrefab");
    ASSERT_NE(loadedUnityStyleRoot, nullptr);
    auto* loadedLegacyRoot = loadedScene->FindGameObjectByName("MixedLegacyPrefab");
    ASSERT_NE(loadedLegacyRoot, nullptr);
    EXPECT_NE(restoredRegistry.FindInstance(*loadedUnityStyleRoot), nullptr);
    EXPECT_NE(restoredRegistry.FindInstance(*loadedLegacyRoot), nullptr);
}

TEST(PrefabUtilityFacadeTests, RestoresStrippedPrefabInstanceAtSavedSceneOrder)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("OrderedPrefab", Id("f4323232-3232-4232-8232-323232323232"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    sourceScene.CreateGameObject("BeforePrefab");
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:OrderedPrefab",
        Id("f4333333-3333-4333-8333-333333333333")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    sourceRegistry.Register(*instantiate.instance);
    sourceScene.CreateGameObject("AfterPrefab");

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    ASSERT_EQ(loadedScene->GetGameObjects().size(), 2u);
    EXPECT_EQ(loadedScene->GetGameObjects()[0]->GetName(), "BeforePrefab");
    EXPECT_EQ(loadedScene->GetGameObjects()[1]->GetName(), "AfterPrefab");

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4343434-3434-4434-8434-343434343434"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:OrderedPrefab")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    ASSERT_EQ(loadedScene->GetGameObjects().size(), 3u);
    EXPECT_EQ(loadedScene->GetGameObjects()[0]->GetName(), "BeforePrefab");
    EXPECT_EQ(loadedScene->GetGameObjects()[1]->GetName(), "OrderedPrefab");
    EXPECT_EQ(loadedScene->GetGameObjects()[2]->GetName(), "AfterPrefab");

    auto* beforePrefab = loadedScene->FindGameObjectByName("BeforePrefab");
    auto* orderedPrefab = loadedScene->FindGameObjectByName("OrderedPrefab");
    auto* afterPrefab = loadedScene->FindGameObjectByName("AfterPrefab");
    ASSERT_NE(beforePrefab, nullptr);
    ASSERT_NE(orderedPrefab, nullptr);
    ASSERT_NE(afterPrefab, nullptr);
    EXPECT_EQ(restoredRegistry.FindInstance(*beforePrefab), nullptr)
        << "Ordinary scene roots must not inherit prefab metadata from stripped prefab placeholders.";
    auto* restoredPrefab = restoredRegistry.FindInstance(*orderedPrefab);
    ASSERT_NE(restoredPrefab, nullptr);
    EXPECT_EQ(restoredPrefab->instanceRoot, orderedPrefab);
    EXPECT_EQ(restoredRegistry.FindInstance(*afterPrefab), nullptr)
        << "Ordinary roots after a stripped prefab placeholder should remain non-prefab hierarchy items.";
    EXPECT_EQ(
        restoredRegistry.GetPresentation(*orderedPrefab).state,
        NLS::Editor::Assets::PrefabHierarchyState::Root);
    EXPECT_EQ(
        restoredRegistry.GetPresentation(*beforePrefab).state,
        NLS::Editor::Assets::PrefabHierarchyState::None);
    EXPECT_EQ(
        restoredRegistry.GetPresentation(*afterPrefab).state,
        NLS::Editor::Assets::PrefabHierarchyState::None);
}

TEST(PrefabUtilityFacadeTests, RestoresParentedGeneratedModelPrefabRegistryFromSceneDocument)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("ParentedGeneratedModel", Id("f4141414-1414-4414-8414-141414141414"));
    prefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    auto& parent = sourceScene.CreateGameObject("SceneParent");
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:ParentedGeneratedModel",
        Id("f4151515-1515-4515-8515-151515151515")
    }, sourceScene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->SetParent(parent);
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);

    ASSERT_EQ(document.prefabInstances.size(), 1u);
    EXPECT_EQ(document.prefabInstances[0].sourcePrefab.guid, prefab.assetId.GetGuid());
    EXPECT_EQ(document.prefabInstances[0].sourcePrefab.filePath, "prefab:ParentedGeneratedModel");

    const auto* parentedRootRecord = FindGameObjectRecordByName(document, "ParentedGeneratedModel");
    ASSERT_NE(parentedRootRecord, nullptr);
    EXPECT_EQ(FindProperty(*parentedRootRecord, "scenePrefab"), nullptr);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("ParentedGeneratedModel"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4161616-1616-4616-8616-161616161616"),
        restoredRegistry,
        [&prefab](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == prefab.assetId && subAssetKey == "prefab:ParentedGeneratedModel")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed);
    auto* loadedParentedRoot = loadedScene->FindGameObjectByName("ParentedGeneratedModel");
    ASSERT_NE(loadedParentedRoot, nullptr);
    ASSERT_NE(loadedParentedRoot->GetParent(), nullptr);
    EXPECT_EQ(loadedParentedRoot->GetParent()->GetName(), "SceneParent");
    auto* restored = restoredRegistry.FindInstance(*loadedParentedRoot);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->prefabAssetId, prefab.assetId);
    EXPECT_EQ(restored->prefabSubAssetKey, "prefab:ParentedGeneratedModel");
    EXPECT_TRUE(restored->generatedReadOnly);
}

TEST(PrefabUtilityFacadeTests, RestoresStrippedPrefabInstanceUnderRestoredPrefabParent)
{
    PrefabUtilityFacade facade;
    auto parentPrefab = MakePrefabArtifact("RestoredPrefabParent", Id("f4353535-3535-4535-8535-353535353535"));
    parentPrefab.generatedModelPrefab = true;
    auto childPrefab = MakePrefabArtifact("RestoredPrefabChild", Id("f4363636-3636-4636-8636-363636363636"));
    childPrefab.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto parent = facade.InstantiatePrefab({
        &parentPrefab,
        parentPrefab.assetId,
        "prefab:RestoredPrefabParent",
        Id("f4373737-3737-4737-8737-373737373737")
    }, sourceScene);
    ASSERT_EQ(parent.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(parent.instance.has_value());
    ASSERT_NE(parent.instance->instanceRoot, nullptr);

    auto child = facade.InstantiatePrefab({
        &childPrefab,
        childPrefab.assetId,
        "prefab:RestoredPrefabChild",
        Id("f4383838-3838-4838-8838-383838383838")
    }, sourceScene);
    ASSERT_EQ(child.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(child.instance.has_value());
    ASSERT_NE(child.instance->instanceRoot, nullptr);
    child.instance->instanceRoot->SetParent(*parent.instance->instanceRoot);

    sourceRegistry.Register(*parent.instance);
    sourceRegistry.Register(*child.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 2u);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("RestoredPrefabParent"), nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("RestoredPrefabChild"), nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4393939-3939-4939-8939-393939393939"),
        restoredRegistry,
        [&](AssetId assetId, const std::string& subAssetKey) -> std::optional<PrefabArtifact>
        {
            if (assetId == parentPrefab.assetId && subAssetKey == "prefab:RestoredPrefabParent")
                return parentPrefab;
            if (assetId == childPrefab.assetId && subAssetKey == "prefab:RestoredPrefabChild")
                return childPrefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(restore);
    auto* loadedParent = loadedScene->FindGameObjectByName("RestoredPrefabParent");
    ASSERT_NE(loadedParent, nullptr);
    auto* loadedChild = loadedScene->FindGameObjectByName("RestoredPrefabChild");
    ASSERT_NE(loadedChild, nullptr);
    EXPECT_EQ(loadedChild->GetParent(), loadedParent);
    EXPECT_NE(restoredRegistry.FindInstance(*loadedParent), nullptr);
    EXPECT_NE(restoredRegistry.FindInstance(*loadedChild), nullptr);
}

TEST(PrefabUtilityFacadeTests, SceneRestoreCanUseSharedPrefabResolverWithoutCopyingPrefabGraph)
{
    PrefabUtilityFacade facade;
    auto prefab = std::make_shared<PrefabArtifact>(
        MakePrefabArtifact("SharedRestorePrefab", Id("f4404040-4040-4440-8440-404040404040")));
    prefab->generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    for (int i = 0; i < 2; ++i)
    {
        auto instance = facade.InstantiatePrefab({
            nullptr,
            prefab->assetId,
            "prefab:SharedRestorePrefab",
            Id("f4414141-4141-4441-8441-414141414141"),
            false,
            prefab.get()
        }, sourceScene);
        ASSERT_EQ(instance.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(instance);
        ASSERT_TRUE(instance.instance.has_value());
        ASSERT_NE(instance.instance->instanceRoot, nullptr);
        instance.instance->instanceRoot->SetName("SharedRestorePrefab_" + std::to_string(i));
        sourceRegistry.Register(*instance.instance);
    }

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 2u);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);

    size_t resolveCount = 0u;
    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f4424242-4242-4442-8442-424242424242"),
        restoredRegistry,
        [&](AssetId assetId, const std::string& subAssetKey)
            -> std::shared_ptr<const PrefabArtifact>
        {
            ++resolveCount;
            if (assetId == prefab->assetId && subAssetKey == "prefab:SharedRestorePrefab")
                return prefab;
            return nullptr;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(restore);
    EXPECT_EQ(resolveCount, 2u);
    auto* firstRoot = loadedScene->FindGameObjectByName("SharedRestorePrefab_0");
    auto* secondRoot = loadedScene->FindGameObjectByName("SharedRestorePrefab_1");
    ASSERT_NE(firstRoot, nullptr);
    ASSERT_NE(secondRoot, nullptr);
    EXPECT_NE(restoredRegistry.FindInstance(*firstRoot), nullptr);
    EXPECT_NE(restoredRegistry.FindInstance(*secondRoot), nullptr);
}

TEST(PrefabUtilityFacadeTests, StrippedSceneRestoreStoresSharedSourcePrefabForRepeatedLargePrefabInstances)
{
    PrefabUtilityFacade facade;
    auto prefab = std::make_shared<PrefabArtifact>(
        MakePrefabArtifact("SharedLargeRestorePrefab", Id("f4484848-4848-4448-8448-484848484848")));
    prefab->generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    for (int i = 0; i < 3; ++i)
    {
        auto instance = facade.InstantiatePrefab({
            nullptr,
            prefab->assetId,
            "prefab:SharedLargeRestorePrefab",
            Id("f4494949-4949-4449-8449-494949494949"),
            false,
            prefab.get()
        }, sourceScene);
        ASSERT_EQ(instance.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(instance);
        ASSERT_TRUE(instance.instance.has_value());
        ASSERT_NE(instance.instance->instanceRoot, nullptr);
        instance.instance->instanceRoot->SetName("SharedLargeRestorePrefab_" + std::to_string(i));
        sourceRegistry.Register(*instance.instance);
    }

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 3u);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        Id("f44a4a4a-4a4a-444a-844a-4a4a4a4a4a4a"),
        restoredRegistry,
        [&](AssetId assetId, const std::string& subAssetKey)
            -> std::shared_ptr<const PrefabArtifact>
        {
            if (assetId == prefab->assetId && subAssetKey == "prefab:SharedLargeRestorePrefab")
                return prefab;
            return nullptr;
        });

    ASSERT_EQ(restore.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(restore);
    ASSERT_EQ(restoredRegistry.GetInstances().size(), 3u);
    for (const auto& restored : restoredRegistry.GetInstances())
    {
        EXPECT_EQ(restored.SharedSourcePrefab(), prefab.get());
        EXPECT_EQ(restored.SourceGraph().root, prefab->graph.root);
        EXPECT_TRUE(restored.sourceGraph.objects.empty());
    }
}

TEST(PrefabUtilityFacadeTests, AnnotatesNestedPrefabInstancesUnderSceneParents)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("NestedUnderSceneParent", Id("f4434343-4343-4443-8443-434343434343"));

    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("SceneParent", "Empty");
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:NestedUnderSceneParent",
        Id("f4444444-4444-4444-8444-444444444444")
    }, scene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(instantiate);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    instantiate.instance->instanceRoot->SetParent(parent);

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    registry.Register(*instantiate.instance);
    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene);

    facade.AnnotateSceneDocumentWithPrefabInstances(document, scene, registry);

    ASSERT_EQ(document.prefabInstances.size(), 1u)
        << "Prefab instances nested below ordinary scene parents must still persist as prefab instances.";
    EXPECT_EQ(document.prefabInstances.front().sourcePrefab.guid, prefab.assetId.GetGuid());
    EXPECT_EQ(document.prefabInstances.front().sourcePrefab.filePath, "prefab:NestedUnderSceneParent");
}

TEST(PrefabUtilityFacadeTests, AnnotatedSceneWithPrefabInstanceValidatesForEditorSave)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("SaveableScenePrefab", Id("f4454545-4545-4545-8545-454545454545"));

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:SaveableScenePrefab",
        Id("f4464646-4646-4646-8646-464646464646")
    }, scene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed) << JoinPrefabDiagnostics(instantiate);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_NE(instantiate.instance->instanceRoot, nullptr);
    registry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(scene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, scene, registry);

    EXPECT_FALSE(document.Validate().HasErrors())
        << "Editor scene save rejects prefab scenes when prefab instance metadata serializes invalid object references.";
    ASSERT_EQ(document.prefabInstances.size(), 1u);
    EXPECT_EQ(document.prefabInstances.front().sourcePrefab.guid, prefab.assetId.GetGuid());
    EXPECT_EQ(document.prefabInstances.front().sourcePrefab.filePath, "prefab:SaveableScenePrefab");

    const auto output = NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
    EXPECT_NE(output.find("\"prefabInstances\""), std::string::npos);
    EXPECT_NE(output.find("\"filePath\": \"prefab:SaveableScenePrefab\""), std::string::npos);
}

TEST(PrefabUtilityFacadeTests, SaveAsPrefabAssetResolvesMeshAndMaterialObjectReferences)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("RenderableCube", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();

    const auto meshAssetId = Id("b2040404-0404-4404-8404-040404040404");
    const auto materialAssetId = Id("b2050505-0505-4505-8505-050505050505");
    const std::string meshArtifactPath = ContentArtifactPath("Cube", "c011");
    const std::string materialArtifactPath = ContentArtifactPath("Cube", "c012");
    const auto meshReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(meshAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshAssetId.GetGuid(), meshArtifactPath),
        meshArtifactPath);
    const auto materialReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(materialAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(materialAssetId.GetGuid(), materialArtifactPath),
        materialArtifactPath);
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(meshReference));
    meshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(materialReference)
    });

    auto save = facade.SaveAsPrefabAsset(
        root,
        Id("b2060606-0606-4606-8606-060606060606"),
        "Assets/Prefabs/RenderableCube.prefab");

    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());
    EXPECT_FALSE(save.artifact->Validate().HasErrors());
    EXPECT_TRUE(ContainsResolvedAsset(*save.artifact, meshAssetId, "Mesh", meshArtifactPath));
    EXPECT_TRUE(ContainsResolvedAsset(*save.artifact, materialAssetId, "Material", materialArtifactPath));

    const auto* meshRecord = FindObjectRecordByType(
        save.artifact->graph,
        "NLS::Engine::Components::MeshFilter");
    ASSERT_NE(meshRecord, nullptr);
    const auto* meshProperty = FindProperty(*meshRecord, "mesh");
    ASSERT_NE(meshProperty, nullptr);
    ASSERT_EQ(meshProperty->value.GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(meshProperty->value.GetObjectReference(), meshReference);
    EXPECT_EQ(meshProperty->value.GetObjectReference().filePath, meshArtifactPath);

    const auto* materialRecord = FindObjectRecordByType(
        save.artifact->graph,
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(materialRecord, nullptr);
    const auto* materialsProperty = FindProperty(*materialRecord, "materials");
    ASSERT_NE(materialsProperty, nullptr);
    ASSERT_EQ(materialsProperty->value.GetKind(), PropertyValue::Kind::Array);
    ASSERT_EQ(materialsProperty->value.GetArray().size(), 1u);
    ASSERT_EQ(materialsProperty->value.GetArray()[0].GetKind(), PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(materialsProperty->value.GetArray()[0].GetObjectReference(), materialReference);
    EXPECT_EQ(materialsProperty->value.GetArray()[0].GetObjectReference().filePath, materialArtifactPath);
}

TEST(PrefabUtilityFacadeTests, SavePrefabContentsRefreshesResolvedAssetReferences)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("EditableRenderable", "Prefab");
    auto save = facade.SaveAsPrefabAsset(
        root,
        Id("b2070707-0707-4707-8707-070707070707"),
        "Assets/Prefabs/EditableRenderable.prefab");
    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());

    auto open = facade.LoadPrefabContents({
        &*save.artifact,
        save.artifact->assetId,
        "prefab:EditableRenderable",
        false
    });
    ASSERT_EQ(open.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(open.stage.has_value());
    ASSERT_NE(open.stage->stageRoot, nullptr);

    auto* meshFilter = open.stage->stageRoot->AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = open.stage->stageRoot->AddComponent<NLS::Engine::Components::MeshRenderer>();
    const auto meshAssetId = Id("b2080808-0808-4808-8808-080808080808");
    const auto materialAssetId = Id("b2090909-0909-4909-8909-090909090909");
    const std::string meshArtifactPath = ContentArtifactPath("Editable", "c021");
    const std::string materialArtifactPath = ContentArtifactPath("Editable", "c022");
    const auto meshReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(meshAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshAssetId.GetGuid(), meshArtifactPath),
        meshArtifactPath);
    const auto materialReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(materialAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(materialAssetId.GetGuid(), materialArtifactPath),
        materialArtifactPath);
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(meshReference));
    meshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(materialReference)
    });
    facade.MarkPrefabContentsDirty(*open.stage);

    auto stage = std::move(*open.stage);
    auto stageSave = facade.SavePrefabContents(stage, *save.artifact);

    ASSERT_EQ(stageSave.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(stageSave.artifact.has_value());
    EXPECT_FALSE(stageSave.artifact->Validate().HasErrors());
    EXPECT_TRUE(ContainsResolvedAsset(*stageSave.artifact, meshAssetId, "Mesh", meshArtifactPath));
    EXPECT_TRUE(ContainsResolvedAsset(*stageSave.artifact, materialAssetId, "Material", materialArtifactPath));
}

TEST(PrefabUtilityFacadeTests, SavePrefabContentsPrunesRemovedResolvedAssetReferences)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("PrunedRenderable", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();

    const auto meshAssetId = Id("b20a0a0a-0a0a-4a0a-8a0a-0a0a0a0a0a0a");
    const std::string meshArtifactPath = ContentArtifactPath("Pruned", "c031");
    const auto meshReference = ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(meshAssetId.GetGuid()),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshAssetId.GetGuid(), meshArtifactPath),
        meshArtifactPath);
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(meshReference));

    auto save = facade.SaveAsPrefabAsset(
        root,
        Id("b20b0b0b-0b0b-4b0b-8b0b-0b0b0b0b0b0b"),
        "Assets/Prefabs/PrunedRenderable.prefab");
    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());
    ASSERT_TRUE(ContainsResolvedAsset(*save.artifact, meshAssetId, "Mesh", meshArtifactPath));

    auto open = facade.LoadPrefabContents({
        &*save.artifact,
        save.artifact->assetId,
        "prefab:PrunedRenderable",
        false
    });
    ASSERT_EQ(open.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(open.stage.has_value());
    ASSERT_NE(open.stage->stageRoot, nullptr);
    auto* openedMeshFilter = open.stage->stageRoot->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(openedMeshFilter, nullptr);
    openedMeshFilter->SetMeshReference({});
    facade.MarkPrefabContentsDirty(*open.stage);

    auto stage = std::move(*open.stage);
    auto stageSave = facade.SavePrefabContents(stage, *save.artifact);

    ASSERT_EQ(stageSave.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(stageSave.artifact.has_value());
    EXPECT_FALSE(stageSave.artifact->Validate().HasErrors());
    EXPECT_FALSE(ContainsResolvedAsset(*stageSave.artifact, meshAssetId, "Mesh", meshArtifactPath));
}

TEST(PrefabUtilityFacadeTests, ReportsPropertyComponentAndChildOverridesWithStableModificationRecords)
{
    PrefabUtilityFacade facade;
    auto artifact = MakePrefabArtifact("OverrideRoot", Id("b3010101-0101-4101-8101-010101010101"));

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:OverrideRoot",
        Id("b3020202-0202-4202-8202-020202020202")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->SetName("RenamedRoot");
    instantiate.instance->instanceRoot->AddComponent<NLS::Engine::Components::LightComponent>();
    NLS::Engine::GameObject child("AddedChild", "Prop");
    child.SetParent(*instantiate.instance->instanceRoot);

    const auto overrides = facade.GetPrefabOverrides(artifact, *instantiate.instance, true);
    child.DetachFromParent();

    const auto* property = FindOverride(overrides, PrefabOverrideKind::Property, "name");
    ASSERT_NE(property, nullptr);
    ASSERT_TRUE(property->baseValue.has_value());
    ASSERT_TRUE(property->localValue.has_value());
    EXPECT_EQ(property->baseValue->GetString(), "OverrideRoot");
    EXPECT_EQ(property->localValue->GetString(), "RenamedRoot");
    EXPECT_TRUE(property->canApply);
    EXPECT_TRUE(property->canRevert);

    EXPECT_NE(FindOverride(overrides, PrefabOverrideKind::AddedComponent, "components"), nullptr);
    EXPECT_NE(FindOverride(overrides, PrefabOverrideKind::AddedGameObject, "children"), nullptr);
}

TEST(PrefabUtilityFacadeTests, ClassifiesComponentAndChildApplyRevertOperations)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("Assembly", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>();
    NLS::Engine::GameObject first("First", "Part");
    NLS::Engine::GameObject second("Second", "Part");
    first.SetParent(root);
    second.SetParent(root);
    auto artifact = facade.SaveAsPrefabAsset(
        root,
        Id("b4010101-0101-4101-8101-010101010101"),
        "Assets/Prefabs/Assembly.prefab").artifact;
    first.DetachFromParent();
    second.DetachFromParent();
    ASSERT_TRUE(artifact.has_value());

    NLS::Engine::SceneSystem::Scene componentScene;
    auto componentInstance = facade.InstantiatePrefab({
        &*artifact,
        artifact->assetId,
        "prefab:Assembly",
        Id("b4020202-0202-4202-8202-020202020202")
    }, componentScene);
    ASSERT_TRUE(componentInstance.instance.has_value());
    auto* light = componentInstance.instance->instanceRoot
        ->GetComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    ASSERT_TRUE(componentInstance.instance->instanceRoot->RemoveComponent(light));
    auto componentOverrides = facade.GetPrefabOverrides(*artifact, *componentInstance.instance, true);
    EXPECT_NE(FindOverride(componentOverrides, PrefabOverrideKind::RemovedComponent, "components"), nullptr);

    NLS::Engine::SceneSystem::Scene childScene;
    auto childInstance = facade.InstantiatePrefab({
        &*artifact,
        artifact->assetId,
        "prefab:Assembly",
        Id("b4030303-0303-4303-8303-030303030303")
    }, childScene);
    ASSERT_TRUE(childInstance.instance.has_value());
    auto& children = childInstance.instance->instanceRoot->GetChildren();
    ASSERT_EQ(children.size(), 2u);
    std::swap(children[0], children[1]);
    auto childOverrides = facade.GetPrefabOverrides(*artifact, *childInstance.instance, true);
    EXPECT_NE(FindOverride(childOverrides, PrefabOverrideKind::ReorderedGameObject, "children"), nullptr);

    for (const auto& overrideRecord : childOverrides)
        childInstance.instance->localPatches.push_back(overrideRecord.patch);
    auto revertAll = facade.RevertPrefabInstance(*childInstance.instance);
    EXPECT_EQ(revertAll.status, PrefabOperationStatus::Committed);
    EXPECT_TRUE(childInstance.instance->localPatches.empty());
}

TEST(PrefabUtilityFacadeTests, AppliesOverridesToNearestEditableLayerAndRejectsInvalidTargets)
{
    PrefabUtilityFacade facade;
    auto artifact = MakePrefabArtifact("ApplyRoot", Id("b5010101-0101-4101-8101-010101010101"));

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:ApplyRoot",
        Id("b5020202-0202-4202-8202-020202020202")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->SetName("AppliedRoot");
    auto overrides = facade.GetPrefabOverrides(artifact, *instantiate.instance, true);
    const auto* property = FindOverride(overrides, PrefabOverrideKind::Property, "name");
    ASSERT_NE(property, nullptr);

    auto apply = facade.ApplySingleOverride(artifact, *property);
    ASSERT_EQ(apply.status, PrefabOperationStatus::Committed);
    EXPECT_EQ(ReadStringProperty(artifact, "name"), "AppliedRoot");

    instantiate.instance->localPatches.push_back(property->patch);
    auto revert = facade.RevertSingleOverride(*instantiate.instance, *property);
    EXPECT_EQ(revert.status, PrefabOperationStatus::Committed);
    EXPECT_EQ(instantiate.instance->instanceRoot->GetName(), "ApplyRoot");
    EXPECT_TRUE(instantiate.instance->localPatches.empty());

    auto rejectReadOnly = facade.ApplySingleOverride(artifact, *property, true);
    EXPECT_EQ(rejectReadOnly.status, PrefabOperationStatus::Rejected);
}

TEST(PrefabUtilityFacadeTests, DerivesUnityLikePrefabEditorStateForConnectedOverridesAndUnpackedObjects)
{
    PrefabUtilityFacade facade;
    auto artifact = MakePrefabArtifact("EditorStateRoot", Id("b5210101-0101-4101-8101-010101010101"));

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:EditorStateRoot",
        Id("b5220202-0202-4202-8202-020202020202")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->SetName("EditorStateOverride");

    PrefabEditorStateQuery stateQuery;
    stateQuery.instance = &*instantiate.instance;
    stateQuery.prefab = &artifact;
    stateQuery.sourceAssetExists = true;
    stateQuery.editableSourceArtifactContext = true;
    const auto state = facade.GetPrefabEditorState(stateQuery);

    EXPECT_EQ(state.connectionState, PrefabEditorConnectionState::Connected);
    EXPECT_EQ(state.resourceState, PrefabEditorResourceState::Ready);
    EXPECT_TRUE(state.hasOverrides);
    EXPECT_EQ(state.overrideCount, 1u);
    EXPECT_EQ(state.applyAvailability, PrefabEditorApplyAvailability::Allowed);
    EXPECT_TRUE(state.canRevert);
    EXPECT_TRUE(state.diagnostics.empty());

    const auto unpack = facade.UnpackPrefabInstance(*instantiate.instance, PrefabUnpackMode::Completely);
    ASSERT_EQ(unpack.status, PrefabOperationStatus::Committed);
    EXPECT_TRUE(instantiate.instance->unpacked);
    PrefabEditorStateQuery unpackedQuery;
    unpackedQuery.instance = &*instantiate.instance;
    unpackedQuery.prefab = &artifact;
    unpackedQuery.sourceAssetExists = true;
    unpackedQuery.editableSourceArtifactContext = true;
    const auto unpackedState = facade.GetPrefabEditorState(unpackedQuery);
    EXPECT_EQ(unpackedState.connectionState, PrefabEditorConnectionState::Unpacked);
    EXPECT_EQ(unpackedState.applyAvailability, PrefabEditorApplyAvailability::Unavailable);
    EXPECT_FALSE(unpackedState.canRevert);
}

TEST(PrefabUtilityFacadeTests, ModelPrefabEditorStateReportsReadOnlyApplyDiagnostic)
{
    PrefabUtilityFacade facade;
    auto generated = MakePrefabArtifact("ReadOnlyModelState", Id("b5230303-0303-4303-8303-030303030303"));
    generated.generatedModelPrefab = true;

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &generated,
        generated.assetId,
        "prefab:ReadOnlyModelState",
        Id("b5240404-0404-4404-8404-040404040404")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->SetName("SceneOnlyReadOnlyOverride");

    PrefabEditorStateQuery stateQuery;
    stateQuery.instance = &*instantiate.instance;
    stateQuery.prefab = &generated;
    stateQuery.sourceAssetExists = true;
    stateQuery.editableSourceArtifactContext = true;
    const auto state = facade.GetPrefabEditorState(stateQuery);

    ASSERT_TRUE(state.hasOverrides);
    EXPECT_TRUE(state.generatedReadOnly);
    EXPECT_EQ(state.applyAvailability, PrefabEditorApplyAvailability::ReadOnlyRejected);
    EXPECT_TRUE(state.canRevert);
    ASSERT_FALSE(state.diagnostics.empty());
    EXPECT_EQ(state.diagnostics.front().code, "prefab-generated-read-only");

    const auto overrides = facade.GetPrefabOverrides(generated, *instantiate.instance, true);
    const auto* nameOverride = FindOverride(overrides, PrefabOverrideKind::Property, "name");
    ASSERT_NE(nameOverride, nullptr);
    const auto apply = facade.ApplySingleOverride(generated, *nameOverride);
    ASSERT_EQ(apply.status, PrefabOperationStatus::Rejected);
    ASSERT_FALSE(apply.diagnostics.empty());
    EXPECT_EQ(apply.diagnostics.front().code, "prefab-generated-read-only");
}

TEST(PrefabUtilityFacadeTests, PrefabEditorStateDisablesApplyWithoutEditableSourceArtifactContext)
{
    PrefabUtilityFacade facade;
    auto artifact = MakePrefabArtifact("InspectorNoSourceContext", Id("b5230505-0505-4505-8505-050505050505"));

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:InspectorNoSourceContext",
        Id("b5240606-0606-4606-8606-060606060606")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());
    instantiate.instance->instanceRoot->SetName("InspectorLocalOverride");

    PrefabEditorStateQuery query;
    query.instance = &*instantiate.instance;
    query.prefab = &artifact;
    query.sourceAssetExists = true;
    query.editableSourceArtifactContext = false;
    query.includeDefaultOverrides = true;

    const auto state = facade.GetPrefabEditorState(query);

    ASSERT_TRUE(state.hasOverrides);
    EXPECT_EQ(state.connectionState, PrefabEditorConnectionState::Connected);
    EXPECT_EQ(state.applyAvailability, PrefabEditorApplyAvailability::Unavailable)
        << "Inspector must not present Apply as executable when it only has a copied source graph and no asset writeback context.";
    EXPECT_TRUE(state.canRevert);
    ASSERT_FALSE(state.diagnostics.empty());
    EXPECT_EQ(state.diagnostics.back().code, "prefab-apply-source-context-missing");
}

TEST(PrefabUtilityFacadeTests, PrefabEditorStateSurfacesMissingSourceAndPendingResources)
{
    PrefabUtilityFacade facade;
    auto artifact = MakePrefabArtifact("PendingSourceState", Id("b5250505-0505-4505-8505-050505050505"));

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:PendingSourceState",
        Id("b5260606-0606-4606-8606-060606060606")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());

    PrefabEditorStateQuery missingQuery;
    missingQuery.instance = &*instantiate.instance;
    missingQuery.prefab = &artifact;
    missingQuery.sourceAssetExists = false;
    missingQuery.editableSourceArtifactContext = true;
    const auto missingState = facade.GetPrefabEditorState(missingQuery);
    EXPECT_EQ(missingState.connectionState, PrefabEditorConnectionState::MissingSource);
    EXPECT_EQ(missingState.applyAvailability, PrefabEditorApplyAvailability::Unavailable);
    ASSERT_FALSE(missingState.diagnostics.empty());
    EXPECT_EQ(missingState.diagnostics.front().code, "prefab-source-missing");

    PrefabEditorStateQuery pendingQuery;
    pendingQuery.instance = &*instantiate.instance;
    pendingQuery.prefab = &artifact;
    pendingQuery.sourceAssetExists = true;
    pendingQuery.resourcesPending = true;
    pendingQuery.editableSourceArtifactContext = true;
    const auto pendingState = facade.GetPrefabEditorState(pendingQuery);
    EXPECT_EQ(pendingState.connectionState, PrefabEditorConnectionState::Connected);
    EXPECT_EQ(pendingState.resourceState, PrefabEditorResourceState::Pending);
    ASSERT_FALSE(pendingState.diagnostics.empty());
    EXPECT_EQ(pendingState.diagnostics.front().code, "prefab-resources-pending");
}

TEST(PrefabUtilityFacadeTests, PreservesNestedPrefabDiagnosticsAndMissingRecoveryRecords)
{
    PrefabUtilityFacade facade;
    auto nested = MakePrefabArtifact("NestedChild", Id("b6010101-0101-4101-8101-010101010101"));
    auto parent = MakePrefabWithNestedReference(
        Id("b6020202-0202-4202-8202-020202020202"),
        nested.assetId,
        "prefab:NestedChild");
    auto missing = MakePrefabWithNestedReference(
        Id("b6030303-0303-4303-8303-030303030303"),
        Id("b6040404-0404-4404-8404-040404040404"),
        "prefab:Missing");
    missing.resolvedAssets.clear();

    auto valid = facade.ValidateNestedPrefabs({parent, nested});
    EXPECT_EQ(valid.status, PrefabOperationStatus::Committed);

    auto missingResult = facade.ValidateNestedPrefabs({missing});
    EXPECT_EQ(missingResult.status, PrefabOperationStatus::Failed);

    parent.resolvedAssets.push_back({missing.assetId, "Prefab", "prefab:Missing", {}});
    auto cycleA = MakePrefabWithNestedReference(
        Id("b6050505-0505-4505-8505-050505050505"),
        Id("b6060606-0606-4606-8606-060606060606"),
        "prefab:CycleB");
    auto cycleB = MakePrefabWithNestedReference(
        Id("b6060606-0606-4606-8606-060606060606"),
        cycleA.assetId,
        "prefab:CycleA");
    cycleA.resolvedAssets.push_back({cycleB.assetId, "Prefab", "prefab:CycleB", {}});
    auto cycle = facade.ValidateNestedPrefabs({cycleA, cycleB});
    EXPECT_EQ(cycle.status, PrefabOperationStatus::Failed);

    NLS::Editor::Assets::PrefabInstanceRecord missingInstance;
    missingInstance.prefabAssetId = Id("b6070707-0707-4707-8707-070707070707");
    missingInstance.prefabSubAssetKey = "prefab:Missing";
    missingInstance.localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        ObjectId(NLS::Guid::NewDeterministic("Missing.Recovery.Source")),
        "name",
        PropertyValue::String("RecoveredWhenAssetReturns")));

    const auto recovery = facade.BuildMissingPrefabRecoveryRecord(missingInstance);
    EXPECT_EQ(recovery.missingPrefabAssetId, missingInstance.prefabAssetId);
    EXPECT_EQ(recovery.preservedOverrides.size(), 1u);
}

TEST(PrefabUtilityFacadeTests, MissingPrefabRecoveryPreservesStructuralOverridePayloadsAndMappings)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("Recoverable", "Prop");
    auto artifact = MakePrefabArtifact("Recoverable", Id("b6080808-0808-4808-8808-080808080808"));

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:Recoverable",
        Id("b6090909-0909-4909-8909-090909090909")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());

    NLS::Engine::GameObject addedChild("RecoveryChild", "Part");
    addedChild.SetParent(*instantiate.instance->instanceRoot);
    const auto overrides = facade.GetPrefabOverrides(artifact, *instantiate.instance, true);
    addedChild.DetachFromParent();

    const auto* childOverride = FindOverride(overrides, PrefabOverrideKind::AddedGameObject, "children");
    ASSERT_NE(childOverride, nullptr);
    ASSERT_FALSE(childOverride->objectRecords.empty());
    for (const auto& overrideRecord : overrides)
        instantiate.instance->localPatches.push_back(overrideRecord.patch);

    auto recovery = facade.BuildMissingPrefabRecoveryRecord(*instantiate.instance, overrides);

    ASSERT_EQ(recovery.preservedOverrides.size(), overrides.size());
    ASSERT_EQ(recovery.preservedOverrideRecords.size(), overrides.size());
    const auto recoveredChild = std::find_if(
        recovery.preservedOverrideRecords.begin(),
        recovery.preservedOverrideRecords.end(),
        [](const NLS::Editor::Assets::PrefabOverrideDescriptor& overrideRecord)
        {
            return overrideRecord.kind == PrefabOverrideKind::AddedGameObject &&
                !overrideRecord.objectRecords.empty();
        });
    EXPECT_NE(recoveredChild, recovery.preservedOverrideRecords.end());
    EXPECT_FALSE(recovery.preservedSourceToInstance.empty());
    EXPECT_FALSE(recovery.preservedSourceByInstanceObject.empty());
}

TEST(PrefabUtilityFacadeTests, UnpackModesPreserveOrDetachNestedPrefabLinks)
{
    PrefabUtilityFacade facade;
    const auto nestedId = Id("b7010101-0101-4101-8101-010101010101");
    auto artifact = MakePrefabWithNestedReference(
        Id("b7020202-0202-4202-8202-020202020202"),
        nestedId,
        "prefab:Nested");

    NLS::Engine::SceneSystem::Scene outerScene;
    auto outerInstance = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:Owner",
        Id("b7030303-0303-4303-8303-030303030303")
    }, outerScene);
    ASSERT_TRUE(outerInstance.instance.has_value());
    auto outerUnpack = facade.UnpackPrefabInstance(*outerInstance.instance, PrefabUnpackMode::OutermostRoot);
    ASSERT_EQ(outerUnpack.status, PrefabOperationStatus::Committed);
    EXPECT_EQ(outerUnpack.preservedNestedPrefabLinks.size(), 1u);
    EXPECT_TRUE(outerUnpack.detachedNestedPrefabLinks.empty());
    ASSERT_EQ(outerInstance.instance->nestedInstances.size(), 1u);
    EXPECT_EQ(outerInstance.instance->nestedInstances.front().prefabAssetId, nestedId);
    EXPECT_EQ(outerInstance.instance->nestedInstances.front().prefabSubAssetKey, "prefab:Nested");

    NLS::Engine::SceneSystem::Scene completeScene;
    auto completeInstance = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:Owner",
        Id("b7040404-0404-4404-8404-040404040404")
    }, completeScene);
    ASSERT_TRUE(completeInstance.instance.has_value());
    auto completeUnpack = facade.UnpackPrefabInstance(*completeInstance.instance, PrefabUnpackMode::Completely);
    ASSERT_EQ(completeUnpack.status, PrefabOperationStatus::Committed);
    EXPECT_TRUE(completeUnpack.preservedNestedPrefabLinks.empty());
    EXPECT_EQ(completeUnpack.detachedNestedPrefabLinks.size(), 1u);
    EXPECT_TRUE(completeInstance.instance->nestedInstances.empty());
}

TEST(PrefabUtilityFacadeTests, NestedPrefabInstancesTrackRootsAndIndependentOverrideChains)
{
    PrefabUtilityFacade facade;
    const auto nestedId = Id("b7050505-0505-4505-8505-050505050505");
    auto artifact = MakePrefabWithNestedReference(
        Id("b7060606-0606-4606-8606-060606060606"),
        nestedId,
        "prefab:Nested");

    NLS::Engine::SceneSystem::Scene scene;
    auto instance = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:Owner",
        Id("b7070707-0707-4707-8707-070707070707")
    }, scene);
    ASSERT_TRUE(instance.instance.has_value());
    ASSERT_EQ(instance.instance->nestedInstances.size(), 1u);
    auto& nestedInstance = instance.instance->nestedInstances.front();
    EXPECT_EQ(nestedInstance.prefabAssetId, nestedId);
    EXPECT_EQ(nestedInstance.prefabSubAssetKey, "prefab:Nested");
    EXPECT_EQ(nestedInstance.sceneAssetId, instance.instance->sceneAssetId);
    EXPECT_NE(nestedInstance.instanceRoot, nullptr);
    EXPECT_TRUE(nestedInstance.localPatches.empty());

    nestedInstance.localPatches.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact.graph.root,
        "name",
        PropertyValue::String("NestedLocalOverride")));

    EXPECT_TRUE(instance.instance->localPatches.empty());
    ASSERT_EQ(instance.instance->nestedInstances.size(), 1u);
    EXPECT_EQ(instance.instance->nestedInstances.front().localPatches.size(), 1u);
}

TEST(PrefabUtilityFacadeTests, GeneratedModelPrefabsRejectAssetWritesButAllowOverridesAndVariants)
{
    PrefabUtilityFacade facade;
    auto generated = MakePrefabArtifact("ImportedModel", Id("b8010101-0101-4101-8101-010101010101"));
    generated.generatedModelPrefab = true;

    auto open = facade.LoadPrefabContents({
        &generated,
        generated.assetId,
        "prefab:ImportedModel",
        false
    });
    ASSERT_EQ(open.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(open.stage.has_value());
    EXPECT_TRUE(open.stage->generatedReadOnly);
    open.stage->stageRoot->SetName("ShouldNotOverwriteImporterOutput");
    facade.MarkPrefabContentsDirty(*open.stage);
    auto stage = std::move(*open.stage);
    auto save = facade.SavePrefabContents(stage, generated);
    EXPECT_EQ(save.status, PrefabOperationStatus::Rejected);
    EXPECT_EQ(ReadStringProperty(generated, "name"), "ImportedModel");

    auto variant = facade.CreateVariant({
        &generated,
        generated.assetId,
        "prefab:ImportedModel",
        "Assets/Prefabs/ImportedModelVariant.prefab",
        Id("b8020202-0202-4202-8202-020202020202"),
        true,
        false
    });
    EXPECT_EQ(variant.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(variant.artifact.has_value());
    EXPECT_EQ(facade.GetPrefabAssetType({&*variant.artifact}), PrefabAssetType::Variant);

    NLS::Engine::SceneSystem::Scene scene;
    auto instance = facade.InstantiatePrefab({
        &generated,
        generated.assetId,
        "prefab:ImportedModel",
        Id("b8030303-0303-4303-8303-030303030303")
    }, scene);
    ASSERT_TRUE(instance.instance.has_value());
    instance.instance->instanceRoot->SetName("SceneOnlyOverride");
    const auto overrides = facade.GetPrefabOverrides(generated, *instance.instance, true);
    EXPECT_NE(FindOverride(overrides, PrefabOverrideKind::Property, "name"), nullptr);

    const auto* nameOverride = FindOverride(overrides, PrefabOverrideKind::Property, "name");
    ASSERT_NE(nameOverride, nullptr);
    auto applyToGenerated = facade.ApplySingleOverride(generated, *nameOverride);
    EXPECT_EQ(applyToGenerated.status, PrefabOperationStatus::Rejected);
}

TEST(PrefabUtilityFacadeTests, SavingPrefabContentsReturnsSerializedSourceAndRefreshesRegisteredInstances)
{
    PrefabUtilityFacade facade;
    auto prefab = MakePrefabArtifact("StageLamp", Id("b8111111-1111-4111-8111-111111111111"));

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = facade.InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:StageLamp"
    }, scene);
    ASSERT_EQ(instantiate.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto& registered = registry.Register(std::move(*instantiate.instance));

    auto open = facade.LoadPrefabContents({
        &prefab,
        prefab.assetId,
        "prefab:StageLamp",
        false,
        "Assets/Prefabs/StageLamp.prefab"
    });
    ASSERT_EQ(open.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(open.stage.has_value());
    open.stage->stageRoot->SetName("SavedStageLamp");
    facade.MarkPrefabContentsDirty(*open.stage);
    auto stage = std::move(*open.stage);

    const auto save = facade.SavePrefabContents(stage, prefab, &registry);

    ASSERT_EQ(save.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(save.artifact.has_value());
    EXPECT_FALSE(save.prefabSourceText.empty());
    EXPECT_EQ(stage.prefabAssetPath, "Assets/Prefabs/StageLamp.prefab");
    EXPECT_EQ(registered.instanceRoot->GetName(), "SavedStageLamp");
    const auto parsed = NLS::Engine::Serialize::ObjectGraphReader::Read(save.prefabSourceText);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(ReadStringProperty(*save.artifact, "name"), "SavedStageLamp");
}

TEST(PrefabUtilityFacadeTests, ResolvesNearestEditableApplyTargetWithoutWritingGeneratedBase)
{
    PrefabUtilityFacade facade;
    auto generated = MakePrefabArtifact("GeneratedBase", Id("b8040404-0404-4404-8404-040404040404"));
    generated.generatedModelPrefab = true;

    auto variant = facade.CreateVariant({
        &generated,
        generated.assetId,
        "prefab:GeneratedBase",
        "Assets/Prefabs/GeneratedBaseVariant.prefab",
        Id("b8050505-0505-4505-8505-050505050505"),
        true,
        false
    });
    ASSERT_EQ(variant.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(variant.artifact.has_value());

    PrefabOverrideDescriptor overrideRecord;
    overrideRecord.owningPrefabLayer = "prefab:GeneratedBaseVariant";
    overrideRecord.patch = NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        variant.artifact->graph.root,
        "name",
        PropertyValue::String("VariantLayerName"));

    auto generatedTarget = facade.ResolveNearestEditableApplyTarget(generated, overrideRecord);
    EXPECT_TRUE(generatedTarget.rejected);
    EXPECT_EQ(generatedTarget.editablePrefab, nullptr);

    auto variantTarget = facade.ResolveNearestEditableApplyTarget(*variant.artifact, overrideRecord);
    EXPECT_FALSE(variantTarget.rejected);
    EXPECT_EQ(variantTarget.editablePrefab, &*variant.artifact);
    EXPECT_EQ(variantTarget.prefabLayer, "prefab:GeneratedBaseVariant");
}

TEST(PrefabUtilityFacadeTests, DefaultOverridesCanBeFilteredOrIncludedSeparately)
{
    PrefabUtilityFacade facade;
    auto artifact = MakePrefabArtifact("DefaultOverrideRoot", Id("b9010101-0101-4101-8101-010101010101"));

    NLS::Engine::SceneSystem::Scene scene;
    auto instance = facade.InstantiatePrefab({
        &artifact,
        artifact.assetId,
        "prefab:DefaultOverrideRoot",
        Id("b9020202-0202-4202-8202-020202020202")
    }, scene);
    ASSERT_TRUE(instance.instance.has_value());

    auto transformOverride = NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        artifact.graph.root,
        "transform.localPosition",
        PropertyValue::String("1,2,3"));
    instance.instance->localPatches.push_back(transformOverride);

    const auto withoutDefaultOverrides = facade.GetPrefabOverrides(artifact, *instance.instance, false);
    EXPECT_EQ(
        FindOverride(withoutDefaultOverrides, PrefabOverrideKind::DefaultOverride, "transform.localPosition"),
        nullptr);
    EXPECT_EQ(
        FindOverride(withoutDefaultOverrides, PrefabOverrideKind::Property, "transform.localPosition"),
        nullptr);

    const auto withDefaultOverrides = facade.GetPrefabOverrides(artifact, *instance.instance, true);
    const auto* defaultOverride = FindOverride(
        withDefaultOverrides,
        PrefabOverrideKind::DefaultOverride,
        "transform.localPosition");
    ASSERT_NE(defaultOverride, nullptr);
    EXPECT_TRUE(defaultOverride->defaultOverride);
    EXPECT_TRUE(defaultOverride->canRevert);
}

TEST(PrefabUtilityFacadeTests, CorrespondingSourceAndInstanceRootQueriesUseStableMappings)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("LookupRoot", "Prefab");
    NLS::Engine::GameObject child("LookupChild", "Prefab");
    child.SetParent(root);

    auto saved = facade.SaveAsPrefabAsset(
        root,
        Id("b9030303-0303-4303-8303-030303030303"),
        "Assets/Prefabs/LookupRoot.prefab");
    child.DetachFromParent();
    ASSERT_EQ(saved.status, PrefabOperationStatus::Committed);
    ASSERT_TRUE(saved.artifact.has_value());

    NLS::Engine::SceneSystem::Scene scene;
    auto instance = facade.InstantiatePrefab({
        &*saved.artifact,
        saved.artifact->assetId,
        "prefab:LookupRoot",
        Id("b9040404-0404-4404-8404-040404040404")
    }, scene);
    ASSERT_TRUE(instance.instance.has_value());
    ASSERT_NE(instance.instance->instanceRoot, nullptr);
    ASSERT_EQ(instance.instance->instanceRoot->GetChildren().size(), 1u);
    auto* instanceChild = instance.instance->instanceRoot->GetChildren()[0];

    const auto sourceRoot = facade.GetCorrespondingObjectFromSource(
        *instance.instance,
        *instance.instance->instanceRoot);
    ASSERT_TRUE(sourceRoot.has_value());
    EXPECT_EQ(*sourceRoot, saved.artifact->graph.root);

    const auto sourceChild = facade.GetCorrespondingObjectFromSource(*instance.instance, *instanceChild);
    ASSERT_TRUE(sourceChild.has_value());
    EXPECT_NE(*sourceChild, saved.artifact->graph.root);

    const auto originalSourceChild = facade.GetOriginalSourceObject(*instance.instance, *instanceChild);
    ASSERT_TRUE(originalSourceChild.has_value());
    EXPECT_EQ(*originalSourceChild, *sourceChild);

    EXPECT_EQ(
        facade.GetNearestPrefabInstanceRoot(*instance.instance, *instanceChild),
        instance.instance->instanceRoot);
    EXPECT_EQ(
        facade.GetOutermostPrefabInstanceRoot(*instance.instance, *instanceChild),
        instance.instance->instanceRoot);

    NLS::Engine::GameObject outsider("Outsider", "Scene");
    EXPECT_FALSE(facade.GetCorrespondingObjectFromSource(*instance.instance, outsider).has_value());
    EXPECT_EQ(facade.GetNearestPrefabInstanceRoot(*instance.instance, outsider), nullptr);
}
