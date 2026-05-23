#include <gtest/gtest.h>

#include <algorithm>

#include "Assets/PrefabUtilityFacade.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/MeshFilter.h"
#include "GameObject.h"
#include "Guid.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/PPtr.h"
#include "Serialize/PPtrResourceTypes.h"

namespace
{
using NLS::Core::Assets::AssetId;
using NLS::Editor::Assets::PrefabAssetQuery;
using NLS::Editor::Assets::PrefabAssetType;
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

TEST(PrefabUtilityFacadeTests, SaveAsPrefabAssetResolvesMeshAndMaterialObjectReferences)
{
    PrefabUtilityFacade facade;
    NLS::Engine::GameObject root("RenderableCube", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();

    const auto meshAssetId = Id("b2040404-0404-4404-8404-040404040404");
    const auto materialAssetId = Id("b2050505-0505-4505-8505-050505050505");
    const std::string meshArtifactPath = "Library/Artifacts/Cube/mesh.nmesh";
    const std::string materialArtifactPath = "Library/Artifacts/Cube/material.nmat";
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
    const std::string meshArtifactPath = "Library/Artifacts/Editable/mesh.nmesh";
    const std::string materialArtifactPath = "Library/Artifacts/Editable/material.nmat";
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
    const std::string meshArtifactPath = "Library/Artifacts/Pruned/mesh.nmesh";
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

