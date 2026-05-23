#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>

#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Components/MeshRenderer.h"
#include "Engine/Assets/PrefabAsset.h"
#include "GameObject.h"
#include "Guid.h"
#include "SceneSystem/Scene.h"

namespace
{
using NLS::Core::Assets::AssetId;
using NLS::Editor::Assets::AssetDragDropWorkflow;
using NLS::Editor::Assets::DragDropOperationStatus;
using NLS::Engine::Assets::PrefabArtifact;

AssetId Id(const char* guid)
{
    return AssetId(NLS::Guid::Parse(guid));
}

bool HasDiagnosticCode(
    const NLS::Editor::Assets::AssetDragDropResult& result,
    const std::string& code)
{
    return std::any_of(
        result.diagnostics.begin(),
        result.diagnostics.end(),
        [&code](const NLS::Editor::Assets::AssetDragDropDiagnostic& diagnostic)
        {
            return diagnostic.code == code;
        });
}

bool HasCommand(
    const std::vector<NLS::Editor::Assets::EditorAssetCommandDescriptor>& commands,
    const std::string& commandId,
    const bool enabled)
{
    return std::any_of(
        commands.begin(),
        commands.end(),
        [&commandId, enabled](const NLS::Editor::Assets::EditorAssetCommandDescriptor& command)
        {
            return command.commandId == commandId && command.enabled == enabled;
        });
}

bool ContainsAssetId(
    const std::vector<AssetId>& assets,
    AssetId assetId)
{
    return std::find(assets.begin(), assets.end(), assetId) != assets.end();
}

std::filesystem::path MakeAssetDragDropRoot()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_editor_asset_dragdrop_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets");
    return root;
}

PrefabArtifact MakePrefabArtifact(const char* name, AssetId assetId, bool generated = false)
{
    NLS::Engine::GameObject root(name, "Prefab");
    auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        assetId,
        std::string("Assets/Prefabs/") + name + ".prefab"
    });
    EXPECT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    EXPECT_TRUE(created.artifact.has_value());
    created.artifact->generatedModelPrefab = generated;
    return *created.artifact;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
}
}

TEST(EditorAssetDragDropTests, DropsPrefabAssetIntoHierarchyAsConnectedInstance)
{
    auto prefab = MakePrefabArtifact("Crate", Id("c1010101-0101-4101-8101-010101010101"));
    NLS::Engine::SceneSystem::Scene scene;
    AssetDragDropWorkflow workflow;

    const auto result = workflow.Execute({
        {NLS::Editor::Assets::DragPayloadKind::PrefabAsset, prefab.assetId, "prefab:Crate", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c1020202-0202-4202-8202-020202020202")
    });

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_NE(result.instance->instanceRoot, nullptr);
    EXPECT_EQ(result.instance->instanceRoot->GetName(), "Crate");
    EXPECT_EQ(result.instance->prefabAssetId, prefab.assetId);
    EXPECT_EQ(result.instance->prefabSubAssetKey, "prefab:Crate");
    EXPECT_FALSE(result.instance->sourceToInstance.empty());
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front(), result.instance->instanceRoot);
    ASSERT_EQ(result.selectedObjects.size(), 1u);
    EXPECT_EQ(result.selectedObjects.front(), result.instance->instanceRoot);
    ASSERT_EQ(result.modifiedScenes.size(), 1u);
    EXPECT_EQ(result.modifiedScenes.front(), Id("c1020202-0202-4202-8202-020202020202"));
    EXPECT_FALSE(result.dependencyRefreshRequests.empty());
    EXPECT_TRUE(HasCommand(result.commandDescriptors, "dragdrop.instantiate-prefab", true));
}

TEST(EditorAssetDragDropTests, DropsPrefabAssetIntoHierarchyRegistersConnectedInstance)
{
    auto prefab = MakePrefabArtifact("RegisteredCrate", Id("c1110101-0101-4101-8101-010101010101"));
    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    AssetDragDropWorkflow workflow;

    const auto result = workflow.Execute({
        {NLS::Editor::Assets::DragPayloadKind::PrefabAsset, prefab.assetId, "prefab:RegisteredCrate", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c1120202-0202-4202-8202-020202020202"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &registry
    });

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_NE(result.instance->instanceRoot, nullptr);

    auto* registered = registry.FindInstance(*result.instance->instanceRoot);
    ASSERT_NE(registered, nullptr);
    EXPECT_EQ(registered->prefabAssetId, prefab.assetId);
    EXPECT_EQ(registered->prefabSubAssetKey, "prefab:RegisteredCrate");

    const auto presentation = registry.GetPresentation(*result.instance->instanceRoot);
    EXPECT_EQ(presentation.state, NLS::Editor::Assets::PrefabHierarchyState::Root);
    EXPECT_EQ(presentation.color, NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedRoot);
}

TEST(EditorAssetDragDropTests, DropsPrefabAssetIntoHierarchyUnderRequestedParent)
{
    auto prefab = MakePrefabArtifact("ChildCrate", Id("c1210101-0101-4101-8101-010101010101"));
    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("Parent", "Scene");
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    AssetDragDropWorkflow workflow;

    const auto result = workflow.Execute({
        {NLS::Editor::Assets::DragPayloadKind::PrefabAsset, prefab.assetId, "prefab:ChildCrate", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, &parent, 0u, false},
        Id("c1220202-0202-4202-8202-020202020202"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &registry
    });

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_NE(result.instance->instanceRoot, nullptr);
    EXPECT_EQ(result.instance->instanceRoot->GetParent(), &parent);
    ASSERT_EQ(parent.GetChildren().size(), 1u);
    EXPECT_EQ(parent.GetChildren().front(), result.instance->instanceRoot);
    EXPECT_NE(registry.FindInstance(*result.instance->instanceRoot), nullptr);
}

TEST(EditorAssetDragDropTests, DropsGeneratedModelPrefabAsReadOnlyConnectedInstance)
{
    auto prefab = MakePrefabArtifact("ImportedHero", Id("c2010101-0101-4101-8101-010101010101"), true);
    NLS::Engine::SceneSystem::Scene scene;
    AssetDragDropWorkflow workflow;

    const auto result = workflow.Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:ImportedHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2020202-0202-4202-8202-020202020202")
    });

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    EXPECT_EQ(result.instance->prefabAssetId, prefab.assetId);
    ASSERT_NE(result.instance->instanceRoot, nullptr);
    EXPECT_EQ(result.instance->instanceRoot->GetName(), "ImportedHero");
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front(), result.instance->instanceRoot);
}

TEST(EditorAssetDragDropTests, RejectsPrefabDropIntoReadOnlyHierarchyTarget)
{
    auto prefab = MakePrefabArtifact("Locked", Id("c3010101-0101-4101-8101-010101010101"));
    NLS::Engine::SceneSystem::Scene scene;
    AssetDragDropWorkflow workflow;

    const auto result = workflow.Execute({
        {NLS::Editor::Assets::DragPayloadKind::PrefabAsset, prefab.assetId, "prefab:Locked", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, true},
        Id("c3020202-0202-4202-8202-020202020202")
    });

    EXPECT_EQ(result.status, DragDropOperationStatus::Rejected);
    EXPECT_TRUE(HasDiagnosticCode(result, "dragdrop-read-only-target"));
    EXPECT_TRUE(scene.GetGameObjects().empty());
}

TEST(EditorAssetDragDropTests, DropsMaterialAssetOntoRendererMaterialSlotAsGuidReference)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& receiver = scene.CreateGameObject("Receiver", "Prop");
    receiver.AddComponent<NLS::Engine::Components::MeshRenderer>();
    AssetDragDropWorkflow workflow;
    const auto materialId = Id("c4010101-0101-4101-8101-010101010101");

    const auto result = workflow.Execute({
        {NLS::Editor::Assets::DragPayloadKind::MaterialAsset, materialId, "material:Body"},
        {NLS::Editor::Assets::DropTargetKind::RendererMaterialSlot, &scene, nullptr, 2u, false, {}, &receiver},
        Id("c4020202-0202-4202-8202-020202020202")
    });

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(result.materialAssignments.size(), 1u);
    EXPECT_EQ(result.materialAssignments.front().targetObject, &receiver);
    EXPECT_EQ(result.materialAssignments.front().slot, 2u);
    EXPECT_EQ(result.materialAssignments.front().material.guid, materialId.GetGuid());
    EXPECT_EQ(result.materialAssignments.front().material.filePath, "material:Body");
    ASSERT_EQ(result.modifiedScenes.size(), 1u);
    EXPECT_FALSE(result.commandDescriptors.empty());
}

TEST(EditorAssetDragDropTests, TextureDropCreatesMaterialAssetAndAssignsItDeterministically)
{
    const auto root = std::filesystem::temp_directory_path() /
        "NullusEditorAssetDragDropTests_TextureDropCreatesMaterialAssetAndAssignsItDeterministically";
    std::filesystem::remove_all(root);
    WriteTextFile(root / "Assets" / "Textures" / "Albedo.png", "texture");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto textureId = Id(database.AssetPathToGUID("Assets/Textures/Albedo.png").c_str());

    NLS::Engine::SceneSystem::Scene scene;
    auto& receiver = scene.CreateGameObject("Receiver", "Prop");
    receiver.AddComponent<NLS::Engine::Components::MeshRenderer>();
    AssetDragDropWorkflow workflow;

    const auto result = workflow.Execute({
        {
            NLS::Editor::Assets::DragPayloadKind::TextureAsset,
            textureId,
            "texture:Albedo",
            nullptr,
            nullptr,
            nullptr,
            "Assets/Textures/Albedo.png"
        },
        {
            NLS::Editor::Assets::DropTargetKind::RendererMaterialSlot,
            &scene,
            nullptr,
            0u,
            false,
            "Assets/Materials",
            &receiver
        },
        Id("c5020202-0202-4202-8202-020202020202"),
        NLS::Editor::Assets::DragDropOperationKind::CreateMaterialAndAssign,
        &database
    });

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(result.createdAssets.size(), 1u);
    ASSERT_EQ(result.materialAssignments.size(), 1u);
    EXPECT_EQ(result.materialAssignments.front().targetObject, &receiver);
    EXPECT_EQ(result.materialAssignments.front().slot, 0u);
    EXPECT_EQ(result.materialAssignments.front().material.guid, result.createdAssets.front().GetGuid());
    EXPECT_EQ(result.materialAssignments.front().material.filePath, "material:Albedo");
    EXPECT_FALSE(database.AssetPathToGUID("Assets/Materials/Albedo.mat").empty());
    EXPECT_TRUE(ContainsAssetId(result.modifiedAssets, result.createdAssets.front()));
    EXPECT_TRUE(HasCommand(result.commandDescriptors, "dragdrop.create-material-and-assign", true));

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, RejectsMaterialDropWithoutRendererTarget)
{
    NLS::Engine::SceneSystem::Scene scene;
    auto& receiver = scene.CreateGameObject("Receiver", "Prop");
    AssetDragDropWorkflow workflow;

    const auto result = workflow.Execute({
        {NLS::Editor::Assets::DragPayloadKind::MaterialAsset, Id("c6010101-0101-4101-8101-010101010101"), "material:Body"},
        {NLS::Editor::Assets::DropTargetKind::RendererMaterialSlot, &scene, nullptr, 0u, false, {}, &receiver},
        Id("c6020202-0202-4202-8202-020202020202")
    });

    EXPECT_EQ(result.status, DragDropOperationStatus::Rejected);
    EXPECT_TRUE(HasDiagnosticCode(result, "dragdrop-missing-mesh-renderer"));
}

TEST(EditorAssetDragDropTests, DropsHierarchyObjectIntoAssetBrowserFolderCreatesPrefabSourceAsset)
{
    const auto root = MakeAssetDragDropRoot();
    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    NLS::Engine::SceneSystem::Scene scene;
    auto& lamp = scene.CreateGameObject("Lamp", "Prop");
    AssetDragDropWorkflow workflow;
    const auto prefabId = Id("c7010101-0101-4101-8101-010101010101");

    NLS::Editor::Assets::AssetDragDropRequest request;
    request.payload.kind = NLS::Editor::Assets::DragPayloadKind::HierarchyObject;
    request.payload.object = &lamp;
    request.target.kind = NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder;
    request.target.assetFolder = "Assets/Prefabs";
    request.sceneAssetId = Id("c7020202-0202-4202-8202-020202020202");
    request.requestedOperation = NLS::Editor::Assets::DragDropOperationKind::SaveAsPrefab;
    request.assetDatabase = &database;
    request.destinationAssetId = prefabId;

    const auto result = workflow.Execute(request);

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.artifact.has_value());
    EXPECT_EQ(result.artifact->assetId, prefabId);
    ASSERT_EQ(result.createdAssets.size(), 1u);
    EXPECT_EQ(result.createdAssets.front(), prefabId);
    ASSERT_EQ(result.createdAssetPaths.size(), 1u);
    EXPECT_EQ(result.createdAssetPaths.front(), std::filesystem::path("Assets/Prefabs/Lamp.prefab"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Prefabs/Lamp.prefab"), prefabId.ToString());
    EXPECT_TRUE(std::filesystem::exists(root / "Assets" / "Prefabs" / "Lamp.prefab"));
    EXPECT_TRUE(ContainsAssetId(result.modifiedAssets, prefabId));
    EXPECT_TRUE(result.modifiedScenes.empty());
    EXPECT_TRUE(HasCommand(result.commandDescriptors, "dragdrop.save-as-prefab", true));
    EXPECT_FALSE(result.dependencyRefreshRequests.empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, DropsImportedPrefabHandleAfterFileWatcherPreimport)
{
    const auto root = MakeAssetDragDropRoot();

    NLS::Engine::GameObject gameObject("Validation Cube", "Prefab");
    const auto prefabId = Id("cb010101-0101-4101-8101-010101010101");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Validation Cube.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CreateTextAsset(
        created.prefabSourceText,
        "Assets/Validation Cube.prefab",
        prefabId));

    NLS::Editor::Assets::AssetPreimportScheduler scheduler;
    NLS::Editor::Assets::ImportProgressTracker tracker;
    ASSERT_TRUE(scheduler.Run(database, tracker, {
        NLS::Editor::Assets::AssetPreimportReason::FileWatcherChanged,
        {root / "Assets" / "Validation Cube.prefab"}
    }));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Validation Cube.prefab",
        prefabId,
        "prefab:Validation Cube",
        NLS::Core::Assets::ArtifactType::Prefab,
        false,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(
        payload,
        scene,
        Id("cb020202-0202-4202-8202-020202020202"));

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    EXPECT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.dragDrop.instance.has_value());
    ASSERT_NE(result.dragDrop.instance->instanceRoot, nullptr);
    EXPECT_EQ(result.dragDrop.instance->instanceRoot->GetName(), "Validation Cube");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, DropsConnectedPrefabInstanceIntoAssetBrowserFolderCreatesVariantWithConflictSuffix)
{
    const auto root = MakeAssetDragDropRoot();
    WriteTextFile(root / "Assets" / "Prefabs" / "Crate Variant.prefab", "{}");
    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    auto prefab = MakePrefabArtifact("Crate", Id("c8010101-0101-4101-8101-010101010101"));
    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = NLS::Editor::Assets::PrefabEditorWorkflow().InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:Crate",
        Id("c8020202-0202-4202-8202-020202020202")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());

    AssetDragDropWorkflow workflow;
    const auto variantId = Id("c8030303-0303-4303-8303-030303030303");
    NLS::Editor::Assets::AssetDragDropRequest request;
    request.payload.kind = NLS::Editor::Assets::DragPayloadKind::PrefabInstance;
    request.payload.assetId = prefab.assetId;
    request.payload.subAssetKey = "prefab:Crate";
    request.payload.prefab = &prefab;
    request.payload.object = instantiate.instance->instanceRoot;
    request.payload.prefabInstance = &*instantiate.instance;
    request.target.kind = NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder;
    request.target.assetFolder = "Assets/Prefabs";
    request.requestedOperation = NLS::Editor::Assets::DragDropOperationKind::CreateVariant;
    request.assetDatabase = &database;
    request.destinationAssetId = variantId;

    const auto result = workflow.Execute(request);

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.artifact.has_value());
    ASSERT_TRUE(result.artifact->graph.basePrefab.has_value());
    EXPECT_EQ(result.artifact->graph.basePrefab->guid, prefab.assetId.GetGuid());
    EXPECT_EQ(result.artifact->graph.basePrefab->filePath, "prefab:Crate");
    ASSERT_EQ(result.createdAssetPaths.size(), 1u);
    EXPECT_EQ(result.createdAssetPaths.front(), std::filesystem::path("Assets/Prefabs/Crate Variant 1.prefab"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Prefabs/Crate Variant 1.prefab"), variantId.ToString());
    EXPECT_TRUE(ContainsAssetId(result.modifiedAssets, variantId));
    EXPECT_FALSE(result.dependencyChanges.empty());
    EXPECT_TRUE(HasCommand(result.commandDescriptors, "dragdrop.create-variant", true));

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, DropsGeneratedModelPrefabInstanceIntoAssetBrowserFolderCreatesUnpackedCopy)
{
    const auto root = MakeAssetDragDropRoot();
    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    auto prefab = MakePrefabArtifact("ImportedHero", Id("c9010101-0101-4101-8101-010101010101"), true);
    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = NLS::Editor::Assets::PrefabEditorWorkflow().InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:ImportedHero",
        Id("c9020202-0202-4202-8202-020202020202")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());

    AssetDragDropWorkflow workflow;
    const auto copyId = Id("c9030303-0303-4303-8303-030303030303");
    NLS::Editor::Assets::AssetDragDropRequest request;
    request.payload.kind = NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabInstance;
    request.payload.assetId = prefab.assetId;
    request.payload.subAssetKey = "prefab:ImportedHero";
    request.payload.prefab = &prefab;
    request.payload.object = instantiate.instance->instanceRoot;
    request.payload.prefabInstance = &*instantiate.instance;
    request.target.kind = NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder;
    request.target.assetFolder = "Assets/Prefabs";
    request.requestedOperation = NLS::Editor::Assets::DragDropOperationKind::CreateUnpackedCopy;
    request.assetDatabase = &database;
    request.destinationAssetId = copyId;

    const auto result = workflow.Execute(request);

    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.artifact.has_value());
    EXPECT_FALSE(result.artifact->generatedModelPrefab);
    EXPECT_FALSE(result.artifact->graph.basePrefab.has_value());
    ASSERT_EQ(result.createdAssetPaths.size(), 1u);
    EXPECT_EQ(result.createdAssetPaths.front(), std::filesystem::path("Assets/Prefabs/ImportedHero.prefab"));
    EXPECT_EQ(database.AssetPathToGUID("Assets/Prefabs/ImportedHero.prefab"), copyId.ToString());
    EXPECT_TRUE(instantiate.instance->prefabAssetId.IsValid());
    EXPECT_TRUE(HasCommand(result.commandDescriptors, "dragdrop.create-unpacked-copy", true));

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, RejectsGeneratedModelPrefabInstanceSaveAsPrefabMutation)
{
    const auto root = MakeAssetDragDropRoot();
    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());

    auto prefab = MakePrefabArtifact("ImportedLocked", Id("ca010101-0101-4101-8101-010101010101"), true);
    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = NLS::Editor::Assets::PrefabEditorWorkflow().InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:ImportedLocked",
        Id("ca020202-0202-4202-8202-020202020202")
    }, scene);
    ASSERT_TRUE(instantiate.instance.has_value());

    AssetDragDropWorkflow workflow;
    NLS::Editor::Assets::AssetDragDropRequest request;
    request.payload.kind = NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabInstance;
    request.payload.assetId = prefab.assetId;
    request.payload.subAssetKey = "prefab:ImportedLocked";
    request.payload.prefab = &prefab;
    request.payload.object = instantiate.instance->instanceRoot;
    request.payload.prefabInstance = &*instantiate.instance;
    request.target.kind = NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder;
    request.target.assetFolder = "Assets/Prefabs";
    request.requestedOperation = NLS::Editor::Assets::DragDropOperationKind::SaveAsPrefab;
    request.assetDatabase = &database;

    const auto result = workflow.Execute(request);

    EXPECT_EQ(result.status, DragDropOperationStatus::Rejected);
    EXPECT_TRUE(HasDiagnosticCode(result, "dragdrop-generated-artifact-mutation"));
    EXPECT_TRUE(database.AssetPathToGUID("Assets/Prefabs/ImportedLocked.prefab").empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, EditorAssetDatabaseExposesDragDropCommandSurface)
{
    NLS::Editor::Assets::EditorAssetDatabase database;

    auto hierarchy = database.GetAssetDragDropCommandSurface({
        NLS::Editor::Assets::AssetDragDropCommandSubject::HierarchyObjectToAssetFolder,
        true,
        false,
        false,
        true,
        false
    });
    EXPECT_TRUE(HasCommand(hierarchy, "dragdrop.save-as-prefab", true));
    EXPECT_TRUE(HasCommand(hierarchy, "dragdrop.create-variant", false));

    auto generatedInstance = database.GetAssetDragDropCommandSurface({
        NLS::Editor::Assets::AssetDragDropCommandSubject::GeneratedModelPrefabInstanceToAssetFolder,
        true,
        false,
        false,
        true,
        true
    });
    EXPECT_TRUE(HasCommand(generatedInstance, "dragdrop.save-as-prefab", false));
    EXPECT_TRUE(HasCommand(generatedInstance, "dragdrop.create-variant", true));
    EXPECT_TRUE(HasCommand(generatedInstance, "dragdrop.create-unpacked-copy", true));

    auto readOnlyPrefabDrop = database.GetAssetDragDropCommandSurface({
        NLS::Editor::Assets::AssetDragDropCommandSubject::PrefabAssetToHierarchy,
        true,
        true,
        false,
        true,
        false
    });
    EXPECT_TRUE(HasCommand(readOnlyPrefabDrop, "dragdrop.instantiate-prefab", false));

    auto textureToRenderer = database.GetAssetDragDropCommandSurface({
        NLS::Editor::Assets::AssetDragDropCommandSubject::TextureAssetToRenderer,
        true,
        false,
        true,
        true,
        false
    });
    EXPECT_TRUE(HasCommand(textureToRenderer, "dragdrop.create-material-and-assign", true));
}

