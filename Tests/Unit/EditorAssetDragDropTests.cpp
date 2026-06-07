#include <gtest/gtest.h>

#include <Json/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/PrefabInstanceResourceLifetime.h"
#include "Core/EditorActions.h"
#include "Core/RendererResourcePrewarmRequest.h"
#include "Core/RendererResourceStreamingBudget.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "Engine/Assets/PrefabAsset.h"
#include "GameObject.h"
#include "Guid.h"
#include "Panels/ImportedPrefabDragPreviewSession.h"
#include "Panels/SceneView.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphSerializer.h"

#ifndef NLS_HAS_ASSIMP_FBX_IMPORTER
#define NLS_HAS_ASSIMP_FBX_IMPORTER 0
#endif

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

std::string JoinDiagnosticCodes(const NLS::Editor::Assets::AssetDragDropResult& result)
{
    std::string output;
    for (const auto& diagnostic : result.diagnostics)
    {
        if (!output.empty())
            output += "; ";
        output += diagnostic.code + ":" + diagnostic.message;
    }
    return output;
}

std::shared_ptr<NLS::Render::Resources::Mesh> CreateTestTransientMesh()
{
    return std::shared_ptr<NLS::Render::Resources::Mesh>(
        reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(0x1)),
        [](NLS::Render::Resources::Mesh*) {});
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

template <typename T>
NLS::Engine::Serialize::PPtr<T> MakePPtr(const NLS::Engine::Serialize::ObjectIdentifier& identifier)
{
    return NLS::Engine::Serialize::PPtr<T>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
}

size_t CountArtifactTelemetryStage(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
{
    return static_cast<size_t>(std::count_if(
        records.begin(),
        records.end(),
        [stage](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == stage;
        }));
}

void ExpectNoColdPrefabArtifactLoad(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::CacheMiss), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::DependencyScan), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::NativeArtifactFileRead), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::NativeContainerParseHash), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::NativeArtifactLowCopyView), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::CpuDeserialize), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u);
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

PrefabArtifact MakeRenderablePrefabArtifact(
    const char* name,
    AssetId assetId,
    const std::string& meshPath,
    const std::string& materialPath,
    bool generated = true)
{
    NLS::Engine::GameObject root(name, "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    EXPECT_NE(meshFilter, nullptr);
    EXPECT_NE(meshRenderer, nullptr);
    const auto meshId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("c2240313-0313-4313-8313-031303130313"));
    const auto materialId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("c2240323-0323-4323-8323-032303230323"));
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            meshId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshId.GetGuid(), meshPath),
            meshPath)));
    meshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                materialId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(materialId.GetGuid(), materialPath),
                materialPath))
    });

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

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

std::string TexturedSingleNodeGltf(const std::string& rootName)
{
    return R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA",
                    "byteLength": 42
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "meshes": [
                {
                    "name": "BodyMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 }
                    ]
                }
            ],
            "nodes": [
                { "name": ")" + rootName + R"(", "mesh": 0 }
            ]
        })";
}

NLS::Editor::Assets::EditorAssetDragPayload MakeImportedGeneratedModelPayload(
    NLS::Editor::Assets::AssetDatabaseFacade& database,
    const std::string& assetPath,
    const std::string& prefabSubAssetKey)
{
    const auto guid = database.AssetPathToGUID(assetPath);
    EXPECT_FALSE(guid.empty());
    const auto assetId = AssetId(NLS::Guid::Parse(guid));
    return NLS::Editor::Assets::MakeEditorAssetDragPayload(
        assetPath,
        assetId,
        prefabSubAssetKey,
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);
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

TEST(EditorAssetDragDropTests, GeneratedModelDropCommitsGraphWhenRendererTextureArtifactIsMissing)
{
    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "ReadyHero.gltf",
        TexturedSingleNodeGltf("ReadyHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/ReadyHero.gltf"));
    auto texture = database.LoadSubAssetAtPath("Assets/Models/ReadyHero.gltf", "texture:image/0");
    ASSERT_TRUE(texture.has_value());
    ASSERT_TRUE(std::filesystem::exists(texture->artifactPath));
    std::filesystem::remove(texture->artifactPath);
    ASSERT_FALSE(std::filesystem::exists(texture->artifactPath));

    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/ReadyHero.gltf",
        "prefab:ReadyHero");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "ReadyHeroRoot");
    EXPECT_TRUE(result.dragDrop.deferredAssetReferenceResolutionRequested)
        << "Renderer resources can stream after the cheap prefab graph commit; a missing renderer artifact must not block Scene View release.";

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, GeneratedModelDropCommitsWhenRendererTextureArtifactIsReady)
{
    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "TexturedHero.gltf",
        TexturedSingleNodeGltf("TexturedHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/TexturedHero.gltf"));
    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/TexturedHero.gltf",
        "prefab:TexturedHero");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "TexturedHeroRoot");
    ASSERT_TRUE(result.dragDrop.instance.has_value());
    ASSERT_NE(result.dragDrop.instance->instanceRoot, nullptr);
    EXPECT_TRUE(result.dragDrop.instance->instanceRoot->IsSelfActive());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, PreviewPrefabArtifactFreshnessInvalidatesWhenManifestChangesBeforeCommit)
{
    const auto root = MakeAssetDragDropRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "FreshnessHero.gltf",
        TexturedSingleNodeGltf("FreshnessHeroRoot"));
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/FreshnessHero.gltf"));
    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/FreshnessHero.gltf",
        "prefab:FreshnessHero");

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto previewPrefab = bridge.TryLoadPreviewPrefabArtifactShared(payload);
    ASSERT_NE(previewPrefab, nullptr);
    EXPECT_TRUE(bridge.IsPreviewPrefabArtifactCurrent(payload, *previewPrefab, false));

    const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting(
        "Assets/Models/FreshnessHero.gltf");
    {
        std::ofstream output(artifactRoot / "manifest.json", std::ios::binary | std::ios::app);
        ASSERT_TRUE(output.good());
        output << '\n';
    }

    EXPECT_FALSE(bridge.IsPreviewPrefabArtifactCurrent(payload, *previewPrefab, false))
        << "Scene View release must reject a drag-start preview artifact if the importer manifest changed before mouse-up.";

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, PreviewPrefabArtifactFreshnessRejectsValueCopyWithoutRuntimeCacheIdentity)
{
    const auto root = MakeAssetDragDropRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "ValueCopyFreshnessHero.gltf",
        TexturedSingleNodeGltf("ValueCopyFreshnessHeroRoot"));
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/ValueCopyFreshnessHero.gltf"));
    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/ValueCopyFreshnessHero.gltf",
        "prefab:ValueCopyFreshnessHero");

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto previewPrefab = bridge.TryLoadPreviewPrefabArtifactShared(payload);
    ASSERT_NE(previewPrefab, nullptr);
    EXPECT_TRUE(bridge.IsPreviewPrefabArtifactCurrent(payload, *previewPrefab, false));

    const auto copiedPrefab = *previewPrefab;
    EXPECT_FALSE(bridge.IsPreviewPrefabArtifactCurrent(payload, copiedPrefab, false))
        << "Only the shared preview artifact carries the runtime cache identity needed to prove freshness at mouse-up.";

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewCreatesPendingSceneInstanceWithoutSerializing)
{
    auto prefab = MakePrefabArtifact(
        "PreviewSceneHero",
        Id("c2210101-0101-4101-8101-010101010101"),
        true);
    NLS::Engine::SceneSystem::Scene activeScene;
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;

    const NLS::Maths::Vector3 placement { 1.0f, 2.0f, 3.0f };
    const auto result = preview.BeginOrUpdate(
        prefab,
        activeScene,
        "guid-preview-hero",
        "prefab:PreviewSceneHero",
        placement);

    ASSERT_TRUE(result.created);
    ASSERT_NE(result.root, nullptr);
    ASSERT_NE(preview.GetPreviewScene(), nullptr);
    EXPECT_EQ(preview.GetPreviewScene(), &activeScene);
    ASSERT_EQ(activeScene.GetGameObjects().size(), 1u);
    EXPECT_EQ(activeScene.GetGameObjects().front(), result.root);
    EXPECT_TRUE(result.root->IsEditorTransient())
        << "Drag-start prefab instances render in the active Scene but stay pending until mouse release.";
    EXPECT_EQ(result.root->GetTransform()->GetWorldPosition(), placement);

    const auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(activeScene);
    const auto sceneRecord = std::find_if(
        document.objects.begin(),
        document.objects.end(),
        [&document](const NLS::Engine::Serialize::ObjectRecord& record)
        {
            return record.id == document.root;
        });
    ASSERT_NE(sceneRecord, document.objects.end());
    ASSERT_FALSE(sceneRecord->properties.empty());
    EXPECT_TRUE(sceneRecord->properties.front().value.GetArray().empty())
        << "Pending drag instances must not be serialized before commit.";
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewSuppressesNormalGameObjectCreatedEvents)
{
    auto prefab = MakePrefabArtifact(
        "PreviewEventIsolationHero",
        Id("c2210202-0202-4202-8202-020202020202"),
        true);
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;

    size_t createdEvents = 0u;
    const auto listener = NLS::Engine::GameObject::CreatedEvent +=
        [&createdEvents](NLS::Engine::GameObject&)
        {
            ++createdEvents;
        };

    const NLS::Maths::Vector3 placement { 1.0f, 0.0f, 3.0f };
    NLS::Engine::SceneSystem::Scene scene;
    const auto result = preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-preview-event-isolation",
        "prefab:PreviewEventIsolationHero",
        placement);

    NLS::Engine::GameObject::CreatedEvent -= listener;

    ASSERT_TRUE(result.created);
    ASSERT_NE(result.root, nullptr);
    EXPECT_EQ(createdEvents, 0u)
        << "Preview-scene prefab objects must not enter normal editor hierarchy/selection via global CreatedEvent.";
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewCancelDestroysPendingSceneInstance)
{
    auto prefab = MakePrefabArtifact(
        "PreviewIsolationLifetimeHero",
        Id("c2210252-0252-4252-8252-025202520252"),
        true);
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;
    NLS::Engine::SceneSystem::Scene scene;

    const auto result = preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-preview-isolation-lifetime",
        "prefab:PreviewIsolationLifetimeHero",
        { 0.0f, 0.0f, 2.0f });

    ASSERT_TRUE(result.created);
    ASSERT_NE(result.root, nullptr);
    EXPECT_TRUE(preview.ContainsObject(*result.root));
    NLS::Engine::GameObject unrelatedSceneObject("UnrelatedSceneObject", "SceneOnly");
    EXPECT_FALSE(preview.ContainsObject(unrelatedSceneObject))
        << "Preview hit-tests must be scoped to the preview root subtree, not every active scene object.";
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);

    preview.Clear();

    EXPECT_TRUE(scene.GetGameObjects().empty())
        << "Cancelling a drag must destroy the pending active-scene instance instead of leaving hidden roots behind.";
    EXPECT_EQ(preview.GetRoot(), nullptr);
}

TEST(EditorAssetDragDropTests, FormalPrefabInstantiationStillBroadcastsNormalGameObjectCreatedEvents)
{
    auto prefab = MakePrefabArtifact(
        "FormalEventHero",
        Id("c2210303-0303-4303-8303-030303030303"),
        true);
    NLS::Engine::SceneSystem::Scene scene;

    size_t createdEvents = 0u;
    const auto listener = NLS::Engine::GameObject::CreatedEvent +=
        [&createdEvents](NLS::Engine::GameObject&)
        {
            ++createdEvents;
        };

    auto result = NLS::Engine::Assets::InstantiatePrefabArtifact(prefab, scene);

    NLS::Engine::GameObject::CreatedEvent -= listener;

    ASSERT_NE(result.root, nullptr);
    EXPECT_GT(createdEvents, 0u)
        << "Preview isolation must not globally silence normal prefab instance creation events used by Hierarchy.";
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewFollowsHoverPlacementWithoutReinstantiating)
{
    auto prefab = MakePrefabArtifact(
        "MouseFollowHero",
        Id("c2220101-0101-4101-8101-010101010101"),
        true);
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;
    NLS::Engine::SceneSystem::Scene scene;

    const NLS::Maths::Vector3 firstPlacement { 0.0f, 0.0f, 4.0f };
    const auto first = preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-mouse-follow",
        "prefab:MouseFollowHero",
        firstPlacement);
    ASSERT_TRUE(first.created);
    ASSERT_NE(first.root, nullptr);

    const NLS::Maths::Vector3 secondPlacement { 5.0f, 0.0f, -2.0f };
    const auto second = preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-mouse-follow",
        "prefab:MouseFollowHero",
        secondPlacement);

    EXPECT_FALSE(second.created);
    EXPECT_EQ(second.root, first.root);
    ASSERT_NE(second.root, nullptr);
    EXPECT_EQ(second.root->GetTransform()->GetWorldPosition(), secondPlacement);
    ASSERT_NE(preview.GetPreviewScene(), nullptr);
    EXPECT_EQ(scene.GetGameObjects().size(), 1u)
        << "Hover movement should reposition the preview root instead of instantiating duplicate preview objects.";
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewCommitHandoffUsesSamePendingInstance)
{
    auto prefab = MakePrefabArtifact(
        "CommitPreviewHero",
        Id("c2230101-0101-4101-8101-010101010101"),
        true);
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;
    NLS::Engine::SceneSystem::Scene activeScene;

    const NLS::Maths::Vector3 firstPlacement { 1.0f, 0.0f, 1.0f };
    const NLS::Maths::Vector3 finalPlacement { -3.0f, 0.0f, 7.0f };
    auto first = preview.BeginOrUpdate(
        prefab,
        activeScene,
        "guid-commit-preview",
        "prefab:CommitPreviewHero",
        firstPlacement);
    ASSERT_TRUE(first.created);
    ASSERT_NE(first.root, nullptr);
    ASSERT_FALSE(preview.BeginOrUpdate(
        prefab,
        activeScene,
        "guid-commit-preview",
        "prefab:CommitPreviewHero",
        finalPlacement).created);

    auto handoff = preview.EndForCommit();

    ASSERT_TRUE(handoff.placement.has_value());
    EXPECT_EQ(*handoff.placement, finalPlacement);
    EXPECT_EQ(handoff.root, first.root)
        << "Mouse release must commit the same object created at drag start, not destroy preview and instantiate a replacement.";
    ASSERT_NE(handoff.root, nullptr);
    EXPECT_FALSE(handoff.root->IsEditorTransient());
    EXPECT_EQ(preview.GetPreviewScene(), nullptr);
    EXPECT_EQ(preview.GetRoot(), nullptr);

    ASSERT_EQ(activeScene.GetGameObjects().size(), 1u)
        << "Dropping a dragged preview must not duplicate the active scene instance.";
    EXPECT_EQ(activeScene.GetGameObjects().front(), handoff.root);
    EXPECT_EQ(handoff.root->GetTransform()->GetWorldPosition(), finalPlacement);
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewReleaseCommitsRootAndContinuesStreamingResources)
{
    EXPECT_TRUE(NLS::Editor::Panels::CanCommitImportedAssetDragPreviewRootOnRelease(true, true))
        << "Mouse release should commit the same preview root even when renderer resources are still loading.";
    EXPECT_FALSE(NLS::Editor::Panels::CanCommitImportedAssetDragPreviewRootOnRelease(true, false));
    EXPECT_FALSE(NLS::Editor::Panels::CanCommitImportedAssetDragPreviewRootOnRelease(false, true));

    const auto pendingOptions =
        NLS::Editor::Core::BuildImportedPrefabPreviewCommitResolutionOptions(false);
    EXPECT_FALSE(pendingOptions.hideRootUntilRendererResourcesReady)
        << "Mouse release should keep the committed preview root visible and continue renderer-resource streaming.";
    EXPECT_FALSE(pendingOptions.keepRootRenderingSuppressedOnFailure)
        << "A failed renderer resource resolution should mark the prefab state without hiding the object after release.";

    const auto readyOptions =
        NLS::Editor::Core::BuildImportedPrefabPreviewCommitResolutionOptions(true);
    EXPECT_FALSE(readyOptions.hideRootUntilRendererResourcesReady);
    EXPECT_FALSE(readyOptions.keepRootRenderingSuppressedOnFailure);
}

TEST(EditorAssetDragDropTests, SceneLoadGeneratedPrefabResolutionSuppressesRenderingUntilAllResourcesReady)
{
    const auto options = NLS::Editor::Core::BuildSceneLoadPrefabResourceResolutionOptions();

    EXPECT_FALSE(options.hideRootUntilRendererResourcesReady)
        << "Scene-open generated/model prefab restoration should show restored instances immediately and stream renderer resources.";
    EXPECT_FALSE(options.keepRootRenderingSuppressedOnFailure)
        << "Scene-open resource failures should mark the prefab state without keeping unrelated restored instances hidden.";
    EXPECT_EQ(
        options.progressTargetPlatform,
        std::string(NLS::Editor::Core::kSceneLoadRendererResourceResolutionTargetPlatform));
}

TEST(EditorAssetDragDropTests, SceneLoadPrefabStreamingBudgetMatchesDragPreviewBudget)
{
    const auto sceneLoadBudget =
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget();
    const auto dragPreviewBudget =
        NLS::Editor::Core::GetDragPreviewPrefabRendererResourceStreamingBudget();

    EXPECT_EQ(sceneLoadBudget.frameBudget, dragPreviewBudget.frameBudget);
    EXPECT_EQ(sceneLoadBudget.resourcePrewarmsPerFrame, dragPreviewBudget.resourcePrewarmsPerFrame);
    EXPECT_EQ(sceneLoadBudget.meshPrewarmsPerFrame, dragPreviewBudget.meshPrewarmsPerFrame);
    EXPECT_EQ(sceneLoadBudget.materialPrewarmsPerFrame, dragPreviewBudget.materialPrewarmsPerFrame);
    EXPECT_EQ(sceneLoadBudget.textureCompletionsPerFrame, dragPreviewBudget.textureCompletionsPerFrame);
    EXPECT_EQ(sceneLoadBudget.meshBindsPerFrame, dragPreviewBudget.meshBindsPerFrame);
    EXPECT_EQ(sceneLoadBudget.maxInflightMeshLoads, dragPreviewBudget.maxInflightMeshLoads);
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewReleaseDoesNotRevealPartialRendererResources)
{
    EXPECT_FALSE(NLS::Editor::Core::ShouldRevealRendererResourceResolutionObjectBeforeAllReady(true))
        << "A committed drag preview hidden until renderer resources are ready must reveal the whole prefab at once, not per renderer.";
    EXPECT_FALSE(NLS::Editor::Core::ShouldRevealRendererResourceResolutionObjectBeforeAllReady(false))
        << "Renderer resource resolution must not unsuppress individual renderers outside the final whole-root reveal path.";
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewFastHoverUpdateRefreshesCommitPlacement)
{
    auto prefab = MakePrefabArtifact(
        "FastHoverPreviewHero",
        Id("c2240101-0101-4101-8101-010101010101"),
        true);
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;
    NLS::Engine::SceneSystem::Scene scene;

    const NLS::Maths::Vector3 firstPlacement { 2.0f, 0.0f, 2.0f };
    const NLS::Maths::Vector3 finalPlacement { 8.0f, 0.0f, -4.0f };
    ASSERT_TRUE(preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-fast-hover-preview",
        "prefab:FastHoverPreviewHero",
        firstPlacement).created);

    preview.UpdatePlacement(finalPlacement);
    auto handoff = preview.EndForCommit();

    ASSERT_TRUE(handoff.placement.has_value());
    EXPECT_EQ(*handoff.placement, finalPlacement)
        << "SceneView's fast path updates an existing preview root, so the commit handoff must track that final hover placement too.";
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewReleaseDoesNotLeaveStaleCommitPlacement)
{
    auto prefab = MakePrefabArtifact(
        "CancelledPreviewHero",
        Id("c2240202-0202-4202-8202-020202020202"),
        true);
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;
    NLS::Engine::SceneSystem::Scene scene;

    const NLS::Maths::Vector3 stalePlacement { 6.0f, 0.0f, 3.0f };
    ASSERT_TRUE(preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-cancelled-preview",
        "prefab:CancelledPreviewHero",
        stalePlacement).created);

    preview.Clear();
    EXPECT_FALSE(preview.GetLastPlacement().has_value())
        << "Cancelling or replacing a preview must not leave a stale hover placement for a later drop.";

    auto handoff = preview.EndForCommit();
    EXPECT_FALSE(handoff.placement.has_value());
    EXPECT_EQ(handoff.root, nullptr);
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewCachesRendererPathsAfterCreation)
{
    const std::string meshPath = "Library/Artifacts/Preview/CachedRendererPaths/body.nmesh";
    const std::string materialPath = "Library/Artifacts/Preview/CachedRendererPaths/body.nmat";
    auto prefab = MakeRenderablePrefabArtifact(
        "CachedPreviewRendererPaths",
        Id("c2240303-0303-4303-8303-030303030303"),
        meshPath,
        materialPath);
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;
    NLS::Engine::SceneSystem::Scene scene;

    const NLS::Maths::Vector3 firstPlacement { 1.0f, 0.0f, 2.0f };
    ASSERT_TRUE(preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-cached-preview-paths",
        "prefab:CachedPreviewRendererPaths",
        firstPlacement).created);

    ASSERT_EQ(preview.GetCachedMeshPaths().size(), 1u);
    ASSERT_EQ(preview.GetCachedMaterialPaths().size(), 1u);
    ASSERT_EQ(preview.GetCachedRendererEntries().size(), 1u);
    EXPECT_EQ(preview.GetCachedMeshPaths().front(), meshPath);
    EXPECT_EQ(preview.GetCachedMaterialPaths().front(), materialPath);
    EXPECT_NE(preview.GetCachedRendererEntries().front().meshFilter, nullptr);
    EXPECT_NE(preview.GetCachedRendererEntries().front().meshRenderer, nullptr);

    const auto* cachedMeshPaths = &preview.GetCachedMeshPaths();
    const auto* cachedMaterialPaths = &preview.GetCachedMaterialPaths();
    const auto* cachedRendererEntries = &preview.GetCachedRendererEntries();
    const NLS::Maths::Vector3 secondPlacement { 3.0f, 0.0f, 4.0f };
    ASSERT_FALSE(preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-cached-preview-paths",
        "prefab:CachedPreviewRendererPaths",
        secondPlacement).created);

    EXPECT_EQ(&preview.GetCachedMeshPaths(), cachedMeshPaths)
        << "Hover updates should reuse the cached renderer-path list instead of recursively scanning the preview tree every frame.";
    EXPECT_EQ(&preview.GetCachedMaterialPaths(), cachedMaterialPaths);
    EXPECT_EQ(&preview.GetCachedRendererEntries(), cachedRendererEntries);
    ASSERT_EQ(preview.GetCachedMeshPaths().size(), 1u);
    ASSERT_EQ(preview.GetCachedMaterialPaths().size(), 1u);
    ASSERT_EQ(preview.GetCachedRendererEntries().size(), 1u);
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewPendingSceneInstanceExposesRendererToRenderPath)
{
    const std::string meshPath = "Library/Artifacts/Preview/PrivateScene/body.nmesh";
    const std::string materialPath = "Library/Artifacts/Preview/PrivateScene/body.nmat";
    auto prefab = MakeRenderablePrefabArtifact(
        "PreviewPrivateSceneHero",
        Id("c2240707-0707-4707-8707-070707070707"),
        meshPath,
        materialPath);
    NLS::Editor::Panels::ImportedPrefabDragPreviewSession preview;
    NLS::Engine::SceneSystem::Scene scene;

    const auto update = preview.BeginOrUpdate(
        prefab,
        scene,
        "guid-preview-private-scene",
        "prefab:PreviewPrivateSceneHero",
        { 1.0f, 2.0f, 3.0f });

    ASSERT_NE(update.root, nullptr);
    ASSERT_NE(update.scene, nullptr);
    EXPECT_FALSE(update.diagnostics.HasErrors());
    EXPECT_EQ(update.scene, &scene);
    ASSERT_EQ(scene.GetFastAccessComponents().modelRenderers.size(), 1u)
        << "Scene View preview rendering consumes the pending active-scene object through the normal RenderScene fast-access path.";
    EXPECT_EQ(
        scene.GetFastAccessComponents().modelRenderers.front(),
        preview.GetCachedRendererEntries().front().meshRenderer);
}

TEST(EditorAssetDragDropTests, ImportedPrefabDragPreviewHandoffPreservesLoadedTransientMeshesForCommit)
{
    NLS::Editor::Core::RendererResourcePrewarmRequest request;
    request.ownerToken = "preview:handoff";
    auto load = std::make_shared<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>();
    auto transientMesh = CreateTestTransientMesh();
    {
        std::lock_guard lock(load->mutex);
        load->completed = true;
        load->accepted = true;
        load->transientMesh = transientMesh;
    }
    request.meshLoadsByPath.emplace("Library/Artifacts/Preview/Handoff/body.nmesh", load);

    auto handoff = NLS::Editor::Core::CollectPrefabInstancePreviewResourceHandoff(std::move(request));

    ASSERT_EQ(handoff.prewarm.meshLoadsByPath.size(), 1u);
    auto handedLoad = handoff.prewarm.meshLoadsByPath.begin()->second;
    ASSERT_NE(handedLoad, nullptr);
    std::lock_guard lock(handedLoad->mutex);
    EXPECT_EQ(handedLoad->transientMesh, transientMesh)
        << "Mouse-release commit must adopt the loaded preview mesh; clearing it here makes the formal instance invisible until a slow reload finishes.";
}

TEST(EditorAssetDragDropTests, PrefabDeletionCleanupReleasesSceneOwnedRendererResources)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    auto prefab = MakePrefabArtifact(
        "DeletionCleanupHero",
        Id("c2250101-0101-4101-8101-010101010101"),
        true);
    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    const auto result = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:DeletionCleanupHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260202-0202-4202-8202-020202020202"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry
    });
    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_NE(result.instance->instanceRoot, nullptr);

    auto* registered = prefabRegistry.FindRootInstance(*result.instance->instanceRoot);
    ASSERT_NE(registered, nullptr);
    const auto ownerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(*registered);
    ASSERT_FALSE(ownerToken.empty());

    ResourceLifetimeRegistry lifetimeRegistry;
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/deletion-body.nmesh",
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/model/deletion-body.nmat",
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/deletion-albedo.ntex",
        8192u,
        ResourceLifetimeOwnerKind::SceneInstance});

    const auto cleanup = NLS::Editor::Core::CleanupPrefabInstanceMarkedDestroy(
        prefabRegistry,
        lifetimeRegistry,
        *result.instance->instanceRoot);

    EXPECT_TRUE(cleanup.removedRootInstance);
    EXPECT_EQ(cleanup.releasedOwnerToken, ownerToken);
    EXPECT_EQ(prefabRegistry.FindRootInstance(*result.instance->instanceRoot), nullptr);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/deletion-body.nmesh"), 0u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/model/deletion-body.nmat"), 0u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/model/deletion-albedo.ntex"), 0u);
    EXPECT_EQ(lifetimeRegistry.CollectTrimCandidates({}).size(), 3u)
        << "After a prefab instance is deleted, instance-owned renderer resources must become trim-eligible.";
}

TEST(EditorAssetDragDropTests, PrefabDeletionCleanupKeepsSharedResourcesAliveForSiblingInstance)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const std::string meshPath = "Library/Artifacts/model/shared-delete-body.nmesh";
    const std::string materialPath = "Library/Artifacts/model/shared-delete-body.nmat";
    auto prefab = MakeRenderablePrefabArtifact(
        "SharedDeleteHero",
        Id("c2250505-0505-4505-8505-050505050505"),
        meshPath,
        materialPath,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    const auto first = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:SharedDeleteHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260505-0505-4505-8505-050505050505"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry,
        {},
        true
    });
    const auto second = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:SharedDeleteHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260606-0606-4606-8606-060606060606"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry,
        {},
        true
    });

    ASSERT_EQ(first.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(second.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(first.instance.has_value());
    ASSERT_TRUE(second.instance.has_value());
    ASSERT_NE(first.instance->instanceRoot, nullptr);
    ASSERT_NE(second.instance->instanceRoot, nullptr);
    ASSERT_NE(first.instance->instanceRoot, second.instance->instanceRoot);

    auto* firstRegistered = prefabRegistry.FindRootInstance(*first.instance->instanceRoot);
    auto* secondRegistered = prefabRegistry.FindRootInstance(*second.instance->instanceRoot);
    ASSERT_NE(firstRegistered, nullptr);
    ASSERT_NE(secondRegistered, nullptr);

    const auto firstOwnerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(*firstRegistered);
    const auto secondOwnerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(*secondRegistered);
    ASSERT_FALSE(firstOwnerToken.empty());
    ASSERT_FALSE(secondOwnerToken.empty());
    ASSERT_NE(firstOwnerToken, secondOwnerToken)
        << "Each prefab instance needs its own scene owner token so deleting one cannot release the sibling.";

    ResourceLifetimeRegistry lifetimeRegistry;
    lifetimeRegistry.Acquire({
        firstOwnerToken,
        ResourceLifetimeResourceType::Mesh,
        meshPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        firstOwnerToken,
        ResourceLifetimeResourceType::Material,
        materialPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        secondOwnerToken,
        ResourceLifetimeResourceType::Mesh,
        meshPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        secondOwnerToken,
        ResourceLifetimeResourceType::Material,
        materialPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});

    const auto cleanup = NLS::Editor::Core::CleanupPrefabInstanceMarkedDestroy(
        prefabRegistry,
        lifetimeRegistry,
        *first.instance->instanceRoot);

    EXPECT_TRUE(cleanup.removedRootInstance);
    EXPECT_EQ(cleanup.releasedOwnerToken, firstOwnerToken);
    EXPECT_EQ(prefabRegistry.FindRootInstance(*first.instance->instanceRoot), nullptr);
    EXPECT_NE(prefabRegistry.FindRootInstance(*second.instance->instanceRoot), nullptr);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialPath), 1u);
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty())
        << "Shared renderer resources must not become trim candidates while a sibling prefab instance still owns them.";
}

TEST(EditorAssetDragDropTests, DeletingOneOfTwoSamePrefabInstancesLeavesSiblingVisibleAndRegistered)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const std::string meshPath = "Library/Artifacts/model/sibling-visible-body.nmesh";
    const std::string materialPath = "Library/Artifacts/model/sibling-visible-body.nmat";
    auto prefab = MakeRenderablePrefabArtifact(
        "SiblingVisibleHero",
        Id("c2250909-0909-4909-8909-090909090909"),
        meshPath,
        materialPath,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    const auto first = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:SiblingVisibleHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260909-0909-4909-8909-090909090909"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry,
        {},
        true
    });
    const auto second = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:SiblingVisibleHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260a0a-0a0a-4a0a-8a0a-0a0a0a0a0a0a"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry,
        {},
        true
    });

    ASSERT_EQ(first.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(second.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(first.instance.has_value());
    ASSERT_TRUE(second.instance.has_value());
    ASSERT_NE(first.instance->instanceRoot, nullptr);
    ASSERT_NE(second.instance->instanceRoot, nullptr);

    auto* firstRegistered = prefabRegistry.FindRootInstance(*first.instance->instanceRoot);
    auto* secondRegistered = prefabRegistry.FindRootInstance(*second.instance->instanceRoot);
    ASSERT_NE(firstRegistered, nullptr);
    ASSERT_NE(secondRegistered, nullptr);
    const auto firstOwnerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(*firstRegistered);
    const auto secondOwnerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(*secondRegistered);

    ResourceLifetimeRegistry lifetimeRegistry;
    lifetimeRegistry.Acquire({
        firstOwnerToken,
        ResourceLifetimeResourceType::Mesh,
        meshPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        secondOwnerToken,
        ResourceLifetimeResourceType::Mesh,
        meshPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});

    auto* deletedRoot = first.instance->instanceRoot;
    auto* siblingRoot = second.instance->instanceRoot;
    const auto cleanup = NLS::Editor::Core::CleanupPrefabInstanceMarkedDestroy(
        prefabRegistry,
        lifetimeRegistry,
        *deletedRoot);
    ASSERT_TRUE(cleanup.removedRootInstance);
    ASSERT_TRUE(scene.DestroyGameObject(*deletedRoot));

    EXPECT_EQ(prefabRegistry.FindRootInstance(*deletedRoot), nullptr);
    EXPECT_NE(prefabRegistry.FindRootInstance(*siblingRoot), nullptr);
    EXPECT_TRUE(siblingRoot->IsAlive());
    EXPECT_NE(std::find(scene.GetGameObjects().begin(), scene.GetGameObjects().end(), siblingRoot), scene.GetGameObjects().end());
    EXPECT_NE(siblingRoot->GetComponent<NLS::Engine::Components::MeshFilter>(), nullptr);
    EXPECT_NE(siblingRoot->GetComponent<NLS::Engine::Components::MeshRenderer>(), nullptr);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshPath), 1u);
}

TEST(EditorAssetDragDropTests, MarkDestroyEventCleanupKeepsSiblingPrefabInstanceAlive)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const std::string rootMeshPath = "Library/Artifacts/model/event-delete-root.nmesh";
    const std::string childMeshPath = "Library/Artifacts/model/event-delete-child.nmesh";
    const std::string rootMaterialPath = "Library/Artifacts/model/event-delete-root.nmat";
    const std::string childMaterialPath = "Library/Artifacts/model/event-delete-child.nmat";

    NLS::Engine::GameObject root("EventDeleteHero", "Prefab");
    auto* rootMeshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* rootMeshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    auto* child = new NLS::Engine::GameObject("EventDeleteHeroChild", "Prefab");
    auto* childMeshFilter = child->AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* childMeshRenderer = child->AddComponent<NLS::Engine::Components::MeshRenderer>();
    child->SetParent(root);
    ASSERT_NE(rootMeshFilter, nullptr);
    ASSERT_NE(rootMeshRenderer, nullptr);
    ASSERT_NE(childMeshFilter, nullptr);
    ASSERT_NE(childMeshRenderer, nullptr);

    const auto rootMeshId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("c2240413-0413-4413-8413-041304130413"));
    const auto childMeshId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("c2240414-0414-4414-8414-041404140414"));
    const auto rootMaterialId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("c2240423-0423-4423-8423-042304230423"));
    const auto childMaterialId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("c2240424-0424-4424-8424-042404240424"));
    rootMeshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            rootMeshId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(rootMeshId.GetGuid(), rootMeshPath),
            rootMeshPath)));
    childMeshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            childMeshId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(childMeshId.GetGuid(), childMeshPath),
            childMeshPath)));
    rootMeshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                rootMaterialId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(rootMaterialId.GetGuid(), rootMaterialPath),
                rootMaterialPath))
    });
    childMeshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                childMaterialId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(childMaterialId.GetGuid(), childMaterialPath),
                childMaterialPath))
    });

    auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        Id("c2250b0b-0b0b-4b0b-8b0b-0b0b0b0b0b0b"),
        "Assets/Prefabs/EventDeleteHero.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(created.artifact.has_value());
    created.artifact->generatedModelPrefab = true;
    auto prefab = *created.artifact;

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    const auto first = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:EventDeleteHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260b0b-0b0b-4b0b-8b0b-0b0b0b0b0b0b"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry,
        {},
        true
    });
    const auto second = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:EventDeleteHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260c0c-0c0c-4c0c-8c0c-0c0c0c0c0c0c"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry,
        {},
        true
    });
    ASSERT_EQ(first.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(second.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(first.instance.has_value());
    ASSERT_TRUE(second.instance.has_value());
    ASSERT_NE(first.instance->instanceRoot, nullptr);
    ASSERT_NE(second.instance->instanceRoot, nullptr);

    auto* firstRoot = first.instance->instanceRoot;
    auto* secondRoot = second.instance->instanceRoot;
    auto* firstChild = firstRoot->GetChildren().empty() ? nullptr : firstRoot->GetChildren().front();
    auto* secondChild = secondRoot->GetChildren().empty() ? nullptr : secondRoot->GetChildren().front();
    ASSERT_NE(firstChild, nullptr);
    ASSERT_NE(secondChild, nullptr);

    auto* firstRegistered = prefabRegistry.FindRootInstance(*firstRoot);
    auto* secondRegistered = prefabRegistry.FindRootInstance(*secondRoot);
    ASSERT_NE(firstRegistered, nullptr);
    ASSERT_NE(secondRegistered, nullptr);
    const auto firstOwnerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(*firstRegistered);
    const auto secondOwnerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(*secondRegistered);
    ASSERT_NE(firstOwnerToken, secondOwnerToken);

    ResourceLifetimeRegistry lifetimeRegistry;
    for (const auto& path : {rootMeshPath, childMeshPath})
    {
        lifetimeRegistry.Acquire({
            firstOwnerToken,
            ResourceLifetimeResourceType::Mesh,
            path,
            4096u,
            ResourceLifetimeOwnerKind::SceneInstance});
        lifetimeRegistry.Acquire({
            secondOwnerToken,
            ResourceLifetimeResourceType::Mesh,
            path,
            4096u,
            ResourceLifetimeOwnerKind::SceneInstance});
    }
    for (const auto& path : {rootMaterialPath, childMaterialPath})
    {
        lifetimeRegistry.Acquire({
            firstOwnerToken,
            ResourceLifetimeResourceType::Material,
            path,
            1024u,
            ResourceLifetimeOwnerKind::SceneInstance});
        lifetimeRegistry.Acquire({
            secondOwnerToken,
            ResourceLifetimeResourceType::Material,
            path,
            1024u,
            ResourceLifetimeOwnerKind::SceneInstance});
    }

    const auto listener = NLS::Engine::GameObject::MarkedDestroyEvent +=
        [&prefabRegistry, &lifetimeRegistry](NLS::Engine::GameObject& object)
    {
        (void)NLS::Editor::Core::CleanupPrefabInstanceMarkedDestroy(
            prefabRegistry,
            lifetimeRegistry,
            object);
    };

    firstRoot->MarkAsDestroy();
    NLS::Engine::GameObject::MarkedDestroyEvent -= listener;

    ASSERT_TRUE(scene.DestroyGameObject(*firstRoot));
    EXPECT_EQ(prefabRegistry.FindRootInstance(*firstRoot), nullptr);
    EXPECT_NE(prefabRegistry.FindRootInstance(*secondRoot), nullptr);
    EXPECT_TRUE(secondRoot->IsAlive());
    EXPECT_TRUE(secondChild->IsAlive());
    EXPECT_NE(prefabRegistry.FindInstance(*secondChild), nullptr);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, rootMeshPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, childMeshPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, rootMaterialPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, childMaterialPath), 1u);
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty());

    child->DetachFromParent();
    delete child;
}

TEST(EditorAssetDragDropTests, RepeatedPrefabDropsCreateDistinctInstanceCorrespondenceIds)
{
    const std::string meshPath = "Library/Artifacts/model/distinct-instance-body.nmesh";
    const std::string materialPath = "Library/Artifacts/model/distinct-instance-body.nmat";
    auto prefab = MakeRenderablePrefabArtifact(
        "DistinctInstanceIdHero",
        Id("c2250707-0707-4707-8707-070707070707"),
        meshPath,
        materialPath,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    const auto first = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:DistinctInstanceIdHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260707-0707-4707-8707-070707070707"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry,
        {},
        true
    });
    const auto second = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:DistinctInstanceIdHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, nullptr, 0u, false},
        Id("c2260808-0808-4808-8808-080808080808"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry,
        {},
        true
    });

    ASSERT_EQ(first.status, DragDropOperationStatus::Committed);
    ASSERT_EQ(second.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(first.instance.has_value());
    ASSERT_TRUE(second.instance.has_value());

    const auto rootSource = prefab.graph.root;
    const auto firstRootId = first.instance->sourceToInstance.find(rootSource);
    const auto secondRootId = second.instance->sourceToInstance.find(rootSource);
    ASSERT_NE(firstRootId, first.instance->sourceToInstance.end());
    ASSERT_NE(secondRootId, second.instance->sourceToInstance.end());
    EXPECT_NE(firstRootId->second, secondRootId->second)
        << "Multiple instances of the same prefab must not share scene-instance correspondence ids.";
}

TEST(EditorAssetDragDropTests, PrefabDeletionCleanupReleasesNestedPrefabRootsUnderDestroyedParent)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    NLS::Engine::SceneSystem::Scene scene;
    auto& parent = scene.CreateGameObject("DeleteParent", "Empty");
    auto prefab = MakePrefabArtifact(
        "NestedDeletionHero",
        Id("c2250303-0303-4303-8303-030303030303"),
        true);
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    const auto result = AssetDragDropWorkflow().Execute({
        {NLS::Editor::Assets::DragPayloadKind::GeneratedModelPrefabAsset, prefab.assetId, "prefab:NestedDeletionHero", &prefab},
        {NLS::Editor::Assets::DropTargetKind::Hierarchy, &scene, &parent, 0u, false},
        Id("c2260404-0404-4404-8404-040404040404"),
        NLS::Editor::Assets::DragDropOperationKind::None,
        nullptr,
        &prefabRegistry
    });
    ASSERT_EQ(result.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.instance.has_value());
    ASSERT_NE(result.instance->instanceRoot, nullptr);
    ASSERT_TRUE(result.instance->instanceRoot->IsDescendantOf(&parent));

    auto* registered = prefabRegistry.FindRootInstance(*result.instance->instanceRoot);
    ASSERT_NE(registered, nullptr);
    const auto ownerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(*registered);
    ASSERT_FALSE(ownerToken.empty());

    ResourceLifetimeRegistry lifetimeRegistry;
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/nested-deletion-body.nmesh",
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});

    const auto cleanup = NLS::Editor::Core::CleanupPrefabInstanceMarkedDestroy(
        prefabRegistry,
        lifetimeRegistry,
        parent);

    EXPECT_TRUE(cleanup.removedRootInstance)
        << "Deleting a parent object must release prefab instances rooted anywhere in the deleted subtree.";
    EXPECT_EQ(prefabRegistry.FindRootInstance(*result.instance->instanceRoot), nullptr);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Mesh,
        "Library/Artifacts/model/nested-deletion-body.nmesh"), 0u);
}

TEST(EditorAssetDragDropTests, RepeatedGeneratedModelDropFastBindsThroughUnifiedHotCache)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "RepeatedDropHero.gltf",
        TexturedSingleNodeGltf("RepeatedDropHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/RepeatedDropHero.gltf"));
    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/RepeatedDropHero.gltf",
        "prefab:RepeatedDropHero");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto first = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);
    ASSERT_TRUE(first.handled);
    ASSERT_FALSE(first.pendingImport) << JoinDiagnosticCodes(first.dragDrop);
    ASSERT_EQ(first.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(first.dragDrop);
    const auto firstRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(firstRecords, ArtifactLoadTelemetryStage::CacheMiss), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(firstRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto repeated = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);
    ASSERT_TRUE(repeated.handled);
    ASSERT_FALSE(repeated.pendingImport) << JoinDiagnosticCodes(repeated.dragDrop);
    ASSERT_EQ(repeated.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(repeated.dragDrop);
    const auto repeatedRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::CacheHit), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::DependencyScan), 0u)
        << "Generated model final-drop uses the prefab graph hot path; renderer artifact dependency scans are deferred to streaming.";
    ExpectNoColdPrefabArtifactLoad(repeatedRecords);
    EXPECT_EQ(scene.GetGameObjects().size(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, RepeatedImportedFbxDropFastBindsThroughUnifiedHotCache)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const auto root = MakeAssetDragDropRoot();
    const auto sourceFbx = std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Models/Cube.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFbx));
    std::filesystem::create_directories(root / "Assets" / "Models");
    std::filesystem::copy_file(
        sourceFbx,
        root / "Assets" / "Models" / "RepeatedDropCube.fbx",
        std::filesystem::copy_options::overwrite_existing);

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/RepeatedDropCube.fbx"));
    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/RepeatedDropCube.fbx",
        "prefab:RepeatedDropCube");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto first = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);
    ASSERT_TRUE(first.handled);
    ASSERT_FALSE(first.pendingImport) << JoinDiagnosticCodes(first.dragDrop);
    ASSERT_EQ(first.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(first.dragDrop);
    const auto firstRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(firstRecords, ArtifactLoadTelemetryStage::CacheMiss), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(firstRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);
    const auto objectCountAfterFirstDrop = scene.GetGameObjects().size();

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto repeated = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);
    ASSERT_TRUE(repeated.handled);
    ASSERT_FALSE(repeated.pendingImport) << JoinDiagnosticCodes(repeated.dragDrop);
    ASSERT_EQ(repeated.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(repeated.dragDrop);
    const auto repeatedRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::CacheHit), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::DependencyScan), 0u)
        << "Imported FBX final-drop must match glTF by reusing the prefab graph hot path without synchronous renderer dependency scans.";
    ExpectNoColdPrefabArtifactLoad(repeatedRecords);
    EXPECT_GT(scene.GetGameObjects().size(), objectCountAfterFirstDrop)
        << "A hot-cache FBX drop should still instantiate another scene object graph.";
    ASSERT_TRUE(first.dragDrop.instance.has_value());
    ASSERT_TRUE(repeated.dragDrop.instance.has_value());
    ASSERT_NE(first.dragDrop.instance->instanceRoot, nullptr);
    ASSERT_NE(repeated.dragDrop.instance->instanceRoot, nullptr);
    EXPECT_NE(first.dragDrop.instance->instanceRoot, repeated.dragDrop.instance->instanceRoot);

    std::filesystem::remove_all(root);
#endif
}

TEST(EditorAssetDragDropTests, GeneratedModelDropCommitsAfterReimportRefreshesRendererTextureArtifact)
{
    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "ReimportedHero.gltf",
        TexturedSingleNodeGltf("ReimportedHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/ReimportedHero.gltf"));
    ASSERT_TRUE(database.ImportAsset("Assets/Models/ReimportedHero.gltf"));
    ASSERT_TRUE(database.Refresh());

    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/ReimportedHero.gltf",
        "prefab:ReimportedHero");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "ReimportedHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, GeneratedModelDropCommitsGraphWhenRendererMeshArtifactIsInvalid)
{
    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "InvalidMeshHero.gltf",
        TexturedSingleNodeGltf("InvalidMeshHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/InvalidMeshHero.gltf"));
    auto mesh = database.LoadSubAssetAtPath("Assets/Models/InvalidMeshHero.gltf", "mesh:mesh/0");
    ASSERT_TRUE(mesh.has_value());
    WriteTextFile(mesh->artifactPath, "not a native mesh artifact");

    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/InvalidMeshHero.gltf",
        "prefab:InvalidMeshHero");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "InvalidMeshHeroRoot");
    EXPECT_TRUE(result.dragDrop.deferredAssetReferenceResolutionRequested);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, GeneratedModelDropCommitsGraphWhenRendererMaterialArtifactIsInvalid)
{
    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "InvalidMaterialHero.gltf",
        TexturedSingleNodeGltf("InvalidMaterialHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/InvalidMaterialHero.gltf"));
    auto material = database.LoadSubAssetAtPath("Assets/Models/InvalidMaterialHero.gltf", "material:material/0");
    ASSERT_TRUE(material.has_value());
    WriteTextFile(material->artifactPath, "not a native material artifact");

    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/InvalidMaterialHero.gltf",
        "prefab:InvalidMaterialHero");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "InvalidMaterialHeroRoot");
    EXPECT_TRUE(result.dragDrop.deferredAssetReferenceResolutionRequested);

    std::filesystem::remove_all(root);
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
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    const auto prefabId = Id("c7010101-0101-4101-8101-010101010101");

    NLS::Editor::Assets::AssetDragDropRequest request;
    request.payload.kind = NLS::Editor::Assets::DragPayloadKind::HierarchyObject;
    request.payload.object = &lamp;
    request.target.kind = NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder;
    request.target.assetFolder = "Assets/Prefabs";
    request.sceneAssetId = Id("c7020202-0202-4202-8202-020202020202");
    request.requestedOperation = NLS::Editor::Assets::DragDropOperationKind::SaveAsPrefab;
    request.assetDatabase = &database;
    request.prefabInstanceRegistry = &prefabRegistry;
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
    ASSERT_TRUE(result.instance.has_value());
    EXPECT_EQ(result.instance->instanceRoot, &lamp);
    EXPECT_EQ(result.instance->prefabAssetId, prefabId);
    EXPECT_EQ(result.instance->prefabSubAssetKey, "prefab:Lamp");
    EXPECT_NE(prefabRegistry.FindRootInstance(lamp), nullptr)
        << "Saving a scene object as a prefab should immediately connect that scene object to the newly created prefab asset.";
    EXPECT_EQ(prefabRegistry.GetPresentation(lamp).color, NLS::Editor::Assets::PrefabHierarchyColorToken::ConnectedRoot);
    EXPECT_TRUE(ContainsAssetId(result.modifiedAssets, prefabId));
    ASSERT_EQ(result.modifiedScenes.size(), 1u);
    EXPECT_EQ(result.modifiedScenes.front(), request.sceneAssetId);
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

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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
