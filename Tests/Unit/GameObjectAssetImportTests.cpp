#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#include <Json/json.hpp>

#include "Guid.h"
#include "GameObject.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetMeta.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/ImportProgressTracker.h"
#include "Components/MeshRenderer.h"
#include "Components/MeshFilter.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Components/MeshFilter.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "SceneSystem/Scene.h"

namespace
{
std::filesystem::path MakeGameObjectAssetImportRoot()
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_gameobject_asset_import_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets" / "Models");
    return root;
}

class ScopedShaderManagerAssetPaths final
{
public:
    ScopedShaderManagerAssetPaths(
        const std::string& projectAssetsPath,
        const std::string& engineAssetsPath)
    {
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(
            projectAssetsPath,
            engineAssetsPath);
    }

    ~ScopedShaderManagerAssetPaths()
    {
        NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths({}, {});
        NLS::Render::Resources::Loaders::ShaderLoader::SetDefaultProjectAssetsPath({});
    }

    ScopedShaderManagerAssetPaths(const ScopedShaderManagerAssetPaths&) = delete;
    ScopedShaderManagerAssetPaths& operator=(const ScopedShaderManagerAssetPaths&) = delete;
};

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
        0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99,
        0x3D, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
    };
}

NLS::Engine::GameObject* FindChildByName(
    NLS::Engine::GameObject& parent,
    const std::string& name)
{
    for (auto* child : parent.GetChildren())
    {
        if (child && child->GetName() == name)
            return child;
    }
    return nullptr;
}

std::string FormatDragDropDiagnostics(const NLS::Editor::Assets::EditorAssetDragDropBridgeResult& result)
{
    std::ostringstream stream;
    for (const auto& diagnostic : result.dragDrop.diagnostics)
        stream << diagnostic.code << ": " << diagnostic.message << '\n';
    return stream.str();
}

std::string TestFileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

NLS::Render::Context::Driver& EnsureGameObjectAssetImportTestDriver()
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
}

TEST(GameObjectAssetImportTests, MissingModelDoesNotMutateScene)
{
    const auto root = MakeGameObjectAssetImportRoot();
    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropModelAssetIntoHierarchy("Models/Missing.gltf", scene);

    EXPECT_FALSE(result.handled);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, SceneRejectsNullGameObjectInsertionWithoutMutation)
{
    NLS::Engine::SceneSystem::Scene scene;

    EXPECT_FALSE(scene.AddGameObject(nullptr));
    EXPECT_TRUE(scene.GetGameObjects().empty());
}

TEST(GameObjectAssetImportTests, ImportedModelInstantiatesGeneratedPrefabGameObject)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "ImportedHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "ImportedHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    {
        NLS::Editor::Assets::AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/ImportedHero.gltf"));
    }

    const auto result = bridge.DropModelAssetIntoHierarchy("Models/ImportedHero.gltf", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "ImportedHeroRoot");
    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting("Assets/Models/ImportedHero.gltf");
    EXPECT_TRUE(std::filesystem::exists(artifactRoot / "prefab.nprefab"));

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, ColdRawModelDropSchedulesBackgroundImportAndCompletion)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "AsyncHero.gltf";
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "AsyncHeroRoot", "mesh": 0 }
            ]
        })");
    auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(modelPath);
    ASSERT_TRUE(meta.Save(NLS::Core::Assets::GetAssetMetaPath(modelPath)));
    std::filesystem::create_directories(root / "Library" / "ArtifactDB");
    WriteTextFile(
        root / "Library" / "ArtifactDB" / "stale-import.db",
        "Assets/Models/RejectedAsyncHero.gltf\t" + meta.id.ToString() + "\told-hash\tmodel-scene\tprefab:RejectedAsyncHero\t0\t0\n");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::ImportProgressTracker tracker;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    std::atomic_bool completionCalled = false;
    std::function<void()> scheduledImport;
    auto result = bridge.DropModelAssetIntoHierarchyAsync(
        "Models/AsyncHero.gltf",
        scene,
        {
            {},
            nullptr,
            nullptr,
            &tracker,
            [&](NLS::Editor::Assets::EditorAssetDragDropBridgeResult completion)
            {
                completionCalled.store(true, std::memory_order_release);
                EXPECT_TRUE(completion.handled);
                EXPECT_FALSE(completion.pendingImport);
                EXPECT_TRUE(completion.importSucceeded);
            },
            [&](std::function<void()> task)
            {
                scheduledImport = std::move(task);
                return true;
            }
        });

    EXPECT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_TRUE(scene.GetGameObjects().empty());
    EXPECT_FALSE(completionCalled.load(std::memory_order_acquire));
    ASSERT_TRUE(static_cast<bool>(scheduledImport));

    scheduledImport();

    EXPECT_TRUE(completionCalled.load(std::memory_order_acquire));
    EXPECT_FALSE(tracker.HasRunningJobs());
    EXPECT_TRUE(std::filesystem::exists(root / "Library" / "Artifacts" / meta.id.ToString()));
    EXPECT_TRUE(std::filesystem::exists(root / "Library" / "ArtifactDB"));

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, ColdRawModelDropCompletesWhenBackgroundSchedulingIsRejected)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "RejectedAsyncHero.gltf";
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "RejectedAsyncHeroRoot", "mesh": 0 }
            ]
        })");
    auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(modelPath);
    ASSERT_TRUE(meta.Save(NLS::Core::Assets::GetAssetMetaPath(modelPath)));

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    bool completionCalled = false;
    auto result = bridge.DropModelAssetIntoHierarchyAsync(
        "Models/RejectedAsyncHero.gltf",
        scene,
        {
            {},
            nullptr,
            nullptr,
            nullptr,
            [&](NLS::Editor::Assets::EditorAssetDragDropBridgeResult completion)
            {
                completionCalled = true;
                EXPECT_TRUE(completion.handled);
                EXPECT_FALSE(completion.pendingImport);
                EXPECT_FALSE(completion.importSucceeded);
                EXPECT_EQ(completion.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Rejected);
                ASSERT_FALSE(completion.dragDrop.diagnostics.empty());
                EXPECT_EQ(completion.dragDrop.diagnostics.front().code, "dragdrop-background-task-rejected");
            },
            [](std::function<void()>)
            {
                return false;
            }
        });

    EXPECT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_TRUE(completionCalled);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, ColdEditorAssetHandleDropReportsPendingWithoutSynchronousImport)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "ColdHandleHero.gltf";
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        { "attributes": { "POSITION": 0 } }
                    ]
                }
            ],
            "nodes": [
                { "name": "ColdHandleHeroRoot", "mesh": 0 }
            ]
        })");

    auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(modelPath);
    ASSERT_TRUE(meta.Save(NLS::Core::Assets::GetAssetMetaPath(modelPath)));

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/ColdHandleHero.gltf",
        meta.id,
        "prefab:ColdHandleHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        false);

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    EXPECT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_FALSE(result.importSucceeded);
    EXPECT_TRUE(scene.GetGameObjects().empty());
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Library" / "Artifacts" / meta.id.ToString()));

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, EditorAssetDragPayloadCapacityCheckRejectsTruncatingValues)
{
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::New());

    EXPECT_TRUE(NLS::Editor::Assets::CanStoreEditorAssetDragPayload(
        "Assets/Models/Hero.gltf",
        assetId,
        "prefab:Hero"));
    EXPECT_FALSE(NLS::Editor::Assets::CanStoreEditorAssetDragPayload(
        std::string(NLS::Editor::Assets::kEditorAssetDragPayloadPathCapacity, 'a'),
        assetId,
        "prefab:Hero"));
    EXPECT_FALSE(NLS::Editor::Assets::CanStoreEditorAssetDragPayload(
        "Assets/Models/Hero.gltf",
        assetId,
        std::string(NLS::Editor::Assets::kEditorAssetDragPayloadSubAssetCapacity, 's')));
}

TEST(GameObjectAssetImportTests, WarmEditorAssetHandleDropInstantiatesCommittedPrefabWithoutImportFallback)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "WarmHandleHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "WarmHandleHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/WarmHandleHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/WarmHandleHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/WarmHandleHero.gltf",
        assetId,
        "prefab:WarmHandleHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "WarmHandleHeroRoot");
    EXPECT_TRUE(result.dragDrop.instance.has_value());
    EXPECT_TRUE(result.dragDrop.instance->generatedReadOnly);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmEditorAssetHandleProvidesPreviewPrefabWithoutRefreshFallback)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "WarmPreviewHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1 }
                    ]
                }
            ],
            "nodes": [
                { "name": "WarmPreviewHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/WarmPreviewHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/WarmPreviewHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/WarmPreviewHero.gltf",
        assetId,
        "prefab:WarmPreviewHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true,
        true);

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto prefab = bridge.TryLoadPreviewPrefabArtifact(payload);

    ASSERT_TRUE(prefab.has_value());
    EXPECT_EQ(prefab->assetId, assetId);
    EXPECT_TRUE(prefab->generatedModelPrefab);

    NLS::Engine::SceneSystem::Scene previewScene;
    auto preview = NLS::Engine::Assets::InstantiatePrefabArtifact(*prefab, previewScene);
    ASSERT_FALSE(preview.diagnostics.HasErrors());
    ASSERT_NE(preview.root, nullptr);
    EXPECT_EQ(preview.root->GetName(), "WarmPreviewHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmGeneratedModelHandleWithManifestPrimaryModelKeyInstantiatesPrefab)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "PrimaryModelKeyHero.gltf";
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "PrimaryModelKeyHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PrimaryModelKeyHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PrimaryModelKeyHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/PrimaryModelKeyHero.gltf",
        assetId,
        "model:PrimaryModelKeyHero",
        NLS::Core::Assets::ArtifactType::Model,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "PrimaryModelKeyHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmGeneratedModelHandleInstantiatesWhenArtifactsLiveInProjectLibrary)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "ProjectLibraryHero.gltf";
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ProjectLibraryHeroRoot" }]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({
        NLS::Editor::Assets::EditorAssetRoot {
            root / "Assets",
            false,
            "Assets",
            root / "Library"
        }
    });
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/ProjectLibraryHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/ProjectLibraryHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    EXPECT_TRUE(std::filesystem::exists(root / "Library" / "Artifacts" / guid / "manifest.json"));
    EXPECT_TRUE(std::filesystem::exists(root / "Library" / "Artifacts" / guid / "prefab.nprefab"));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Library"));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/ProjectLibraryHero.gltf",
        assetId,
        "prefab:ProjectLibraryHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "ProjectLibraryHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, StaleEditorAssetHandleImportFlagUsesCommittedArtifact)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "StaleImportFlagHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "StaleImportFlagHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/StaleImportFlagHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/StaleImportFlagHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto stalePayload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Models/StaleImportFlagHero.gltf",
        assetId,
        "prefab:StaleImportFlagHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        false);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(stalePayload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "StaleImportFlagHeroRoot");
    EXPECT_TRUE(result.dragDrop.instance.has_value());
    EXPECT_TRUE(result.dragDrop.instance->generatedReadOnly);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, EditorAssetHandleDropRejectsAssetIdentityMismatchWithoutPendingImport)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "IdentityMismatchHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "IdentityMismatchHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/IdentityMismatchHero.gltf"));

    const auto stalePayload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Models/IdentityMismatchHero.gltf",
        NLS::Core::Assets::AssetId::New(),
        "prefab:IdentityMismatchHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        false);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(stalePayload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    EXPECT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Rejected);
    ASSERT_FALSE(result.dragDrop.diagnostics.empty());
    EXPECT_EQ(result.dragDrop.diagnostics.front().code, "dragdrop-asset-identity-mismatch");
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, EditorAssetHandleDropRejectsChangedPathMetaIdentityBeforeArtifactLookup)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "ChangedMetaIdentityHero.gltf";
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "ChangedMetaIdentityHeroRoot" }]
        })");

    auto oldMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(modelPath);
    ASSERT_TRUE(oldMeta.Save(NLS::Core::Assets::GetAssetMetaPath(modelPath)));

    auto newMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(modelPath);
    ASSERT_NE(oldMeta.id, newMeta.id);
    ASSERT_TRUE(newMeta.Save(NLS::Core::Assets::GetAssetMetaPath(modelPath)));

    const auto stalePayload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Models/ChangedMetaIdentityHero.gltf",
        oldMeta.id,
        "prefab:ChangedMetaIdentityHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        false);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(stalePayload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    EXPECT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Rejected);
    ASSERT_FALSE(result.dragDrop.diagnostics.empty());
    EXPECT_EQ(result.dragDrop.diagnostics.front().code, "dragdrop-asset-identity-mismatch");
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, EditorAssetHandleDropReportsPendingWhenImporterManifestMetadataIsStale)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "StaleImporterMetadataHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "StaleImporterMetadataHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/StaleImporterMetadataHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/StaleImporterMetadataHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting(
        "Assets/Models/StaleImporterMetadataHero.gltf");
    const auto manifestPath = artifactRoot / "manifest.json";
    {
        std::ifstream input(manifestPath, std::ios::binary);
        ASSERT_TRUE(input.good());
        auto manifest = nlohmann::json::parse(input, nullptr, false);
        ASSERT_TRUE(manifest.is_object());
        manifest["importerVersion"] = manifest.value("importerVersion", 1u) + 1u;

        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.good());
        output << manifest.dump(2);
    }

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Models/StaleImporterMetadataHero.gltf",
        assetId,
        "prefab:StaleImporterMetadataHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Rejected);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, EditorAssetHandleDropTreatsMalformedManifestImporterVersionAsPendingWithoutThrow)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "MalformedManifestHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "MalformedManifestHeroRoot" }]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/MalformedManifestHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/MalformedManifestHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting(
        "Assets/Models/MalformedManifestHero.gltf");
    const auto manifestPath = artifactRoot / "manifest.json";
    {
        std::ifstream input(manifestPath, std::ios::binary);
        ASSERT_TRUE(input.good());
        auto manifest = nlohmann::json::parse(input, nullptr, false);
        ASSERT_TRUE(manifest.is_object());
        manifest["importerVersion"] = "1";

        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.good());
        output << manifest.dump(2);
    }

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Models/MalformedManifestHero.gltf",
        assetId,
        "prefab:MalformedManifestHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    NLS::Editor::Assets::EditorAssetDragDropBridgeResult result;
    ASSERT_NO_THROW(result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene));

    ASSERT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Rejected);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, EditorAssetHandleDropRejectsEscapingPrefabArtifactPath)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto outsideRoot =
        std::filesystem::temp_directory_path() /
        ("nullus_escaping_prefab_artifact_" + NLS::Guid::New().ToString());
    WriteTextFile(
        root / "Assets" / "Models" / "EscapingArtifactHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "EscapingArtifactHeroRoot" }]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/EscapingArtifactHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/EscapingArtifactHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting(
        "Assets/Models/EscapingArtifactHero.gltf");
    const auto manifestPath = artifactRoot / "manifest.json";
    const auto escapedPrefabPath = outsideRoot / "prefab.nprefab";
    {
        std::ifstream input(manifestPath, std::ios::binary);
        ASSERT_TRUE(input.good());
        auto manifest = nlohmann::json::parse(input, nullptr, false);
        ASSERT_TRUE(manifest.is_object());

        std::filesystem::create_directories(outsideRoot);
        std::filesystem::copy_file(
            artifactRoot / "prefab.nprefab",
            escapedPrefabPath,
            std::filesystem::copy_options::overwrite_existing);

        auto& subAssets = manifest["subAssets"];
        ASSERT_TRUE(subAssets.is_array());
        for (auto& subAsset : subAssets)
        {
            if (!subAsset.is_object() ||
                subAsset.value("subAssetKey", std::string {}) != "prefab:EscapingArtifactHero")
            {
                continue;
            }

            subAsset["artifactPath"] = "../" + outsideRoot.filename().generic_string() + "/prefab.nprefab";
        }

        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.good());
        output << manifest.dump(2);
    }

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Models/EscapingArtifactHero.gltf",
        assetId,
        "prefab:EscapingArtifactHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(outsideRoot);
    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, StaleEditorAssetHandleDropReportsPendingWithoutUsingOldArtifact)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "StaleHandleHero.gltf";
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "OldRoot" }]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/StaleHandleHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/StaleHandleHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "NewRoot" }]
        })");

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/StaleHandleHero.gltf",
        assetId,
        "prefab:StaleHandleHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, StaleExternalDependencyHandleDropReportsPendingWithoutUsingOldArtifact)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "DependencyHandleHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "HeroBaseColor.png";
    WriteBinaryFile(texturePath, TinyPng());
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "textures": [
                { "source": 0 }
            ],
            "nodes": [{ "name": "DependencyHandleHeroRoot" }]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/DependencyHandleHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/DependencyHandleHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    WriteTextFile(texturePath, "new-texture-with-different-size");

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/DependencyHandleHero.gltf",
        assetId,
        "prefab:DependencyHandleHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, EscapingManifestDependencyHandleDropReportsPending)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "EscapedDependencyHero.gltf";
    const auto escapedDependency = root.parent_path() / (root.filename().generic_string() + "_outside_texture.png");
    WriteTextFile(escapedDependency, "outside-texture");
    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "EscapedDependencyHeroRoot" }]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/EscapedDependencyHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/EscapedDependencyHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting(
        "Assets/Models/EscapedDependencyHero.gltf");
    const auto manifestPath = artifactRoot / "manifest.json";
    {
        std::ifstream input(manifestPath, std::ios::binary);
        ASSERT_TRUE(input.good());
        auto manifest = nlohmann::json::parse(input, nullptr, false);
        ASSERT_TRUE(manifest.is_object());
        manifest["dependencies"].push_back({
            {"kind", "source-file-hash"},
            {"value", "../" + escapedDependency.filename().generic_string()},
            {"hashOrVersion", TestFileStamp(escapedDependency)}
        });

        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.good());
        output << manifest.dump(2);
    }

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/EscapedDependencyHero.gltf",
        assetId,
        "prefab:EscapedDependencyHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove(escapedDependency);
    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, ImportedModelDragCreatesOneRootHierarchyWithRendererComponents)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "ImportedMultiRoot.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0, 2] }
            ],
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
            "materials": [
                { "name": "Stone" }
            ],
            "meshes": [
                {
                    "name": "ColumnMesh",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "Building", "children": [1] },
                { "name": "Column", "mesh": 0 },
                { "name": "LooseProp", "mesh": 0 }
            ]
        })");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    {
        NLS::Editor::Assets::AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/ImportedMultiRoot.gltf"));
    }

    const auto result = bridge.DropModelAssetIntoHierarchy("Models/ImportedMultiRoot.gltf", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);

    auto* importedRoot = scene.GetGameObjects().front();
    ASSERT_NE(importedRoot, nullptr);
    EXPECT_EQ(importedRoot->GetName(), "ImportedMultiRoot");
    ASSERT_EQ(importedRoot->GetChildren().size(), 2u);

    auto* building = FindChildByName(*importedRoot, "Building");
    auto* looseProp = FindChildByName(*importedRoot, "LooseProp");
    ASSERT_NE(building, nullptr);
    ASSERT_NE(looseProp, nullptr);
    EXPECT_EQ(building->GetParent(), importedRoot);
    EXPECT_EQ(looseProp->GetParent(), importedRoot);
    auto* looseMeshRenderer = looseProp->GetComponent<NLS::Engine::Components::MeshRenderer>();
    auto* looseMeshFilter = looseProp->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(looseMeshRenderer, nullptr);
    ASSERT_NE(looseMeshFilter, nullptr);
    EXPECT_NE(looseMeshFilter->GetModelPath().find("Library"), std::string::npos);
    EXPECT_EQ(std::filesystem::path(looseMeshFilter->GetModelPath()).extension(), ".nmesh");
    EXPECT_TRUE(NLS::Render::Assets::LoadMeshArtifact(looseMeshFilter->GetModelPath()).has_value());
    ASSERT_EQ(looseMeshRenderer->GetMaterialPaths().size(), 1u);
    EXPECT_NE(looseMeshRenderer->GetMaterialPaths()[0].find("Library"), std::string::npos);
    EXPECT_EQ(std::filesystem::path(looseMeshRenderer->GetMaterialPaths()[0]).extension(), ".nmat");

    ASSERT_EQ(building->GetChildren().size(), 1u);
    auto* column = building->GetChildren()[0];
    ASSERT_NE(column, nullptr);
    EXPECT_EQ(column->GetParent(), building);
    EXPECT_EQ(column->GetName(), "Column");
    auto* columnMeshRenderer = column->GetComponent<NLS::Engine::Components::MeshRenderer>();
    auto* columnMeshFilter = column->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(columnMeshRenderer, nullptr);
    ASSERT_NE(columnMeshFilter, nullptr);
    EXPECT_EQ(columnMeshFilter->GetModelPath(), looseMeshFilter->GetModelPath());
    ASSERT_EQ(columnMeshRenderer->GetMaterialPaths().size(), 1u);
    EXPECT_EQ(columnMeshRenderer->GetMaterialPaths()[0], looseMeshRenderer->GetMaterialPaths()[0]);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, GeneratedModelDragBindsRendererResourcesFromArtifacts)
{
    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    WriteTextFile(
        root / "Assets" / "Models" / "DeferredHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "materials": [
                { "name": "Stone" }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "DeferredHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/DeferredHero.gltf"));
    auto prefab = database.LoadPrefabArtifactAtPath(
        "Assets/Models/DeferredHero.gltf",
        "prefab:DeferredHero");
    ASSERT_TRUE(prefab.has_value());

    std::string meshPath;
    std::string materialPath;
    for (const auto& resolved : prefab->resolvedAssets)
    {
        if (resolved.expectedType == "Mesh")
            meshPath = resolved.artifactPath;
        else if (resolved.expectedType == "Material")
            materialPath = resolved.artifactPath;
    }
    ASSERT_FALSE(meshPath.empty());
    ASSERT_FALSE(materialPath.empty());

    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    auto* mesh = meshManager.GetResource(meshPath, true);
    ASSERT_NE(mesh, nullptr);

    auto* material = new NLS::Render::Resources::Material();
    const_cast<std::string&>(material->path) = materialPath;
    materialManager.RegisterResource(materialPath, material);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropModelAssetIntoHierarchy("Models/DeferredHero.gltf", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);

    auto* renderer = scene.GetGameObjects().front()->GetComponent<NLS::Engine::Components::MeshRenderer>();
    auto* meshFilter = scene.GetGameObjects().front()->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(renderer, nullptr);
    ASSERT_NE(meshFilter, nullptr);
    EXPECT_EQ(
        renderer->GetFrustumBehaviour(),
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
    EXPECT_EQ(meshFilter->ResolveMesh(), mesh);
    EXPECT_EQ(meshFilter->GetMeshReference().Get(), mesh);
    EXPECT_NE(meshFilter->GetModelPath().find("Library"), std::string::npos);
    ASSERT_EQ(renderer->GetMaterialPaths().size(), 1u);
    EXPECT_NE(renderer->GetMaterialPaths()[0].find("Library"), std::string::npos);
    EXPECT_NE(renderer->GetMaterialAtIndex(0), nullptr);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, GeneratedModelDragKeepsColdRendererResourcesDeferredForSceneInsertion)
{
    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    WriteTextFile(
        root / "Assets" / "Models" / "ColdHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
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
            "materials": [
                { "name": "Stone" }
            ],
            "meshes": [
                {
                    "name": "Body",
                    "primitives": [
                        {
                            "attributes": { "POSITION": 0 },
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "ColdHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/ColdHero.gltf"));
    auto prefab = database.LoadPrefabArtifactAtPath(
        "Assets/Models/ColdHero.gltf",
        "prefab:ColdHero");
    ASSERT_TRUE(prefab.has_value());

    bool hasExplicitModelPackage = false;
    bool hasMeshArtifact = false;
    for (const auto& resolved : prefab->resolvedAssets)
    {
        const auto extension = std::filesystem::path(resolved.artifactPath).extension();
        if (extension == ".nmodel")
            hasExplicitModelPackage = true;
        else if (resolved.expectedType == "Mesh" && extension == ".nmesh")
            hasMeshArtifact = true;
    }
    EXPECT_FALSE(hasExplicitModelPackage);
    EXPECT_TRUE(hasMeshArtifact);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropModelAssetIntoHierarchy("Models/ColdHero.gltf", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);

    auto* renderer = scene.GetGameObjects().front()->GetComponent<NLS::Engine::Components::MeshRenderer>();
    auto* meshFilter = scene.GetGameObjects().front()->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(renderer, nullptr);
    ASSERT_NE(meshFilter, nullptr);

    EXPECT_EQ(
        renderer->GetFrustumBehaviour(),
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
    EXPECT_FALSE(meshFilter->GetModelPath().empty());
    EXPECT_EQ(std::filesystem::path(meshFilter->GetModelPath()).extension(), ".nmesh");
    EXPECT_TRUE(NLS::Render::Assets::LoadMeshArtifact(meshFilter->GetModelPath()).has_value());
    EXPECT_EQ(meshFilter->GetMeshReference().Get(), nullptr);
    EXPECT_FALSE(meshManager.IsResourceRegistered(meshFilter->GetModelPath()));
    ASSERT_EQ(renderer->GetMaterialPaths().size(), 1u);
    EXPECT_EQ(renderer->GetMaterialAtIndex(0), nullptr);
    EXPECT_FALSE(renderer->GetMaterialPaths()[0].empty());
    EXPECT_FALSE(materialManager.IsResourceRegistered(renderer->GetMaterialPaths()[0]));

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, GeneratedModelDragDoesNotSynchronouslyPrewarmLargeImportedArtifacts)
{
    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::TextureManager>(textureManager);

    std::ostringstream meshes;
    std::ostringstream nodes;
    for (int index = 0; index < 9; ++index)
    {
        if (index > 0)
        {
            meshes << ",\n";
            nodes << ",\n";
        }
        meshes <<
            "                {\n"
            "                    \"name\": \"Part" << index << "\",\n"
            "                    \"primitives\": [\n"
            "                        {\n"
            "                            \"attributes\": { \"POSITION\": 0 },\n"
            "                            \"indices\": 1,\n"
            "                            \"material\": 0\n"
            "                        }\n"
            "                    ]\n"
            "                }";
        nodes <<
            "                { \"name\": \"Part" << index << "\", \"mesh\": " << index << " }";
    }

    WriteTextFile(
        root / "Assets" / "Models" / "LargeHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0, 1, 2, 3, 4, 5, 6, 7, 8] }
            ],
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
            "materials": [
                { "name": "Stone" }
            ],
            "meshes": [
)" + meshes.str() + R"(
            ],
            "nodes": [
)" + nodes.str() + R"(
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/LargeHero.gltf"));

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    const auto result = bridge.DropModelAssetIntoHierarchy("Models/LargeHero.gltf", scene);

    ASSERT_TRUE(result.handled);
    ASSERT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);

    EXPECT_TRUE(meshManager.GetResources().empty());
    EXPECT_TRUE(materialManager.GetResources().empty());

    std::filesystem::remove_all(root);
}
