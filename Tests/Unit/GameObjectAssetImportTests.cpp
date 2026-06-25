#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
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
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/AssetMeta.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/ImportProgressTracker.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PrefabEditorWorkflow.h"
#include "Assets/SceneRestorePrefabArtifactLoader.h"
#include "Components/MeshRenderer.h"
#include "Components/MeshFilter.h"
#include "Core/PrefabInstanceResourceLifetime.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Jobs/JobSystem.h"
#include "Rendering/Context/Driver.h"
#include "Components/MeshFilter.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphInstantiator.h"
#include "Serialize/ObjectGraphSerializer.h"

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

#ifndef NLS_HAS_ASSIMP_FBX_IMPORTER
#define NLS_HAS_ASSIMP_FBX_IMPORTER 0
#endif

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

template<typename T>
class ScopedServiceOverride
{
public:
    explicit ScopedServiceOverride(T& service)
    {
        m_hadPrevious = NLS::Core::ServiceLocator::Contains<T>();
        if (m_hadPrevious)
            m_previous = &NLS::Core::ServiceLocator::Get<T>();

        NLS::Core::ServiceLocator::Provide<T>(service);
    }

    ~ScopedServiceOverride()
    {
        if (m_hadPrevious && m_previous)
            NLS::Core::ServiceLocator::Provide<T>(*m_previous);
        else
            NLS::Core::ServiceLocator::Remove<T>();
    }

    ScopedServiceOverride(const ScopedServiceOverride&) = delete;
    ScopedServiceOverride& operator=(const ScopedServiceOverride&) = delete;

private:
    bool m_hadPrevious = false;
    T* m_previous = nullptr;
};

class ScopedGameObjectAssetImportJobSystem final
{
public:
    explicit ScopedGameObjectAssetImportJobSystem(const uint32_t backgroundWorkerCount = 1u)
    {
        if (NLS::Base::Jobs::IsJobSystemInitialized())
            return;

        NLS::Base::Jobs::JobSystemConfig config;
        config.workerCount = 1u;
        config.backgroundWorkerCount = backgroundWorkerCount;
        m_ownsRuntime = NLS::Base::Jobs::TryInitializeJobSystem(config);
    }

    ~ScopedGameObjectAssetImportJobSystem()
    {
        if (!m_ownsRuntime)
            return;

        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
        NLS::Base::Jobs::ResetJobSystemForTesting();
    }

    [[nodiscard]] bool IsAvailable() const
    {
        return NLS::Base::Jobs::IsJobSystemInitialized();
    }

    ScopedGameObjectAssetImportJobSystem(const ScopedGameObjectAssetImportJobSystem&) = delete;
    ScopedGameObjectAssetImportJobSystem& operator=(const ScopedGameObjectAssetImportJobSystem&) = delete;

private:
    bool m_ownsRuntime = false;
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

size_t CountSceneRootGameObjects(const NLS::Engine::SceneSystem::Scene& scene)
{
    const auto& gameObjects = scene.GetGameObjects();
    return static_cast<size_t>(std::count_if(
        gameObjects.begin(),
        gameObjects.end(),
        [](const NLS::Engine::GameObject* gameObject)
        {
            return gameObject && !gameObject->HasParent();
        }));
}

NLS::Engine::GameObject* FindSceneRootGameObject(const NLS::Engine::SceneSystem::Scene& scene)
{
    const auto& gameObjects = scene.GetGameObjects();
    const auto found = std::find_if(
        gameObjects.begin(),
        gameObjects.end(),
        [](const NLS::Engine::GameObject* gameObject)
        {
            return gameObject && !gameObject->HasParent();
    });
    return found != gameObjects.end() ? *found : nullptr;
}

std::string FormatDragDropDiagnostics(const NLS::Editor::Assets::EditorAssetDragDropBridgeResult& result)
{
    std::ostringstream stream;
    for (const auto& diagnostic : result.dragDrop.diagnostics)
        stream << diagnostic.code << ": " << diagnostic.message << '\n';
    return stream.str();
}

size_t CountDragDropDiagnosticCode(
    const NLS::Editor::Assets::EditorAssetDragDropBridgeResult& result,
    const char* code)
{
    return static_cast<size_t>(std::count_if(
        result.dragDrop.diagnostics.begin(),
        result.dragDrop.diagnostics.end(),
        [code](const NLS::Editor::Assets::AssetDragDropDiagnostic& diagnostic)
        {
            return diagnostic.code == code;
        }));
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

std::optional<NLS::Core::Assets::ArtifactManifest> LoadArtifactManifestForTest(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId sourceAssetId)
{
    NLS::Core::Assets::ArtifactDatabase database;
    if (!database.Load(projectRoot / "Library" / "ArtifactDB"))
        return std::nullopt;
    return database.BuildManifestForSource(sourceAssetId);
}

void SaveArtifactManifestForTest(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::string& sourcePath)
{
    NLS::Core::Assets::ArtifactDatabase database;
    const auto databasePath = projectRoot / "Library" / "ArtifactDB";
    ASSERT_TRUE(database.Load(databasePath));
    database.UpsertManifest(manifest, sourcePath, NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));
}

void ReplaceArtifactDatabaseFieldForTest(
    const std::filesystem::path& databasePath,
    const std::string& matchField,
    const size_t fieldIndex,
    const std::string& replacement)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    ASSERT_EQ(fieldIndex, 7u);
    NLS::Core::Assets::ArtifactDatabase database;
    ASSERT_TRUE(database.Load(databasePath));
    const auto replaced = database.MutateRecordsForTesting(
        [&](NLS::Core::Assets::ArtifactDatabaseRecord& record)
        {
            if (record.subAssetKey != matchField)
                return false;
            record.artifactPath = replacement;
            return true;
        });
    ASSERT_EQ(replaced, 1u);
    ASSERT_TRUE(database.Save(databasePath));
#else
    (void)databasePath;
    (void)matchField;
    (void)fieldIndex;
    (void)replacement;
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inject raw ArtifactDB records.";
#endif
}

void FlipArtifactDatabaseContentHashForTest(
    const std::filesystem::path& databasePath,
    const std::string& matchField)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Core::Assets::ArtifactDatabase database;
    ASSERT_TRUE(database.Load(databasePath));
    const auto replaced = database.MutateRecordsForTesting(
        [&](NLS::Core::Assets::ArtifactDatabaseRecord& record)
        {
            if (record.subAssetKey != matchField || record.contentHash.empty())
                return false;
            record.contentHash.back() = record.contentHash.back() == '0' ? '1' : '0';
            return true;
        });
    ASSERT_EQ(replaced, 1u);
    ASSERT_TRUE(database.Save(databasePath));
#else
    (void)databasePath;
    (void)matchField;
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inject raw ArtifactDB records.";
#endif
}

void ReplaceAllArtifactDatabaseArtifactPathsForTest(
    const std::filesystem::path& databasePath,
    const std::filesystem::path& replacementRoot)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Core::Assets::ArtifactDatabase database;
    ASSERT_TRUE(database.Load(databasePath));
    size_t nextArtifactIndex = 0u;
    const auto replaced = database.MutateRecordsForTesting(
        [&](NLS::Core::Assets::ArtifactDatabaseRecord& record)
        {
            record.artifactPath =
                (replacementRoot / ("artifact-" + std::to_string(nextArtifactIndex++))).lexically_normal().string();
            return true;
        });
    ASSERT_GT(replaced, 0u);
    ASSERT_TRUE(database.Save(databasePath));
#else
    (void)databasePath;
    (void)replacementRoot;
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inject raw ArtifactDB records.";
#endif
}

std::filesystem::path ResolveManifestArtifactPathForTest(
    const std::filesystem::path& projectRoot,
    const std::string& artifactPathText)
{
    if (artifactPathText.empty())
        return {};

    const auto artifactPath = std::filesystem::path(artifactPathText);
    if (artifactPath.is_absolute())
        return artifactPath.lexically_normal();

    const auto generic = artifactPath.generic_string();
    if (generic == "Library" || generic.rfind("Library/", 0u) == 0u)
        return (projectRoot / artifactPath).lexically_normal();

    return (projectRoot / artifactPath).lexically_normal();
}

std::filesystem::path FindPrefabArtifactPath(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId sourceAssetId,
    const std::string& prefabSubAssetKey)
{
    const auto manifest = LoadArtifactManifestForTest(projectRoot, sourceAssetId);
    if (!manifest.has_value())
        return {};

    const auto* subAsset = manifest->FindSubAsset(prefabSubAssetKey);
    if (!subAsset || subAsset->artifactPath.empty())
        return {};

    return ResolveManifestArtifactPathForTest(projectRoot, subAsset->artifactPath);
}

std::filesystem::path FindFirstArtifactPathForType(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId sourceAssetId,
    const NLS::Core::Assets::ArtifactType artifactType)
{
    const auto manifest = LoadArtifactManifestForTest(projectRoot, sourceAssetId);
    if (!manifest.has_value())
        return {};

    for (const auto& subAsset : manifest->subAssets)
    {
        if (subAsset.artifactType != artifactType || subAsset.artifactPath.empty())
            continue;

        return ResolveManifestArtifactPathForTest(projectRoot, subAsset.artifactPath);
    }

    return {};
}

std::string FindFirstContentHashForArtifactType(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::ArtifactType artifactType)
{
    for (const auto& subAsset : manifest.subAssets)
    {
        if (subAsset.artifactType != artifactType)
            continue;

        return subAsset.contentHash;
    }
    return {};
}

std::string FindFirstImportedArtifactHashForSubAssetPrefix(
    const std::filesystem::path& manifestPath,
    const std::string& subAssetPrefix)
{
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input.good())
        return {};

    auto manifest = nlohmann::json::parse(input, nullptr, false);
    if (!manifest.is_object() || !manifest["dependencies"].is_array())
        return {};

    for (const auto& dependency : manifest["dependencies"])
    {
        if (!dependency.is_object() ||
            dependency.value("kind", std::string {}) != "imported-artifact")
        {
            continue;
        }

        const auto value = dependency.value("value", std::string {});
        const auto separator = value.find('#');
        if (separator == std::string::npos)
            continue;

        const auto subAssetAndTarget = value.substr(separator + 1u);
        if (subAssetAndTarget.rfind(subAssetPrefix, 0u) == 0u)
            return dependency.value("hashOrVersion", std::string {});
    }

    return {};
}

std::filesystem::path FindFirstImportedArtifactPathForSubAssetPrefix(
    const std::filesystem::path& root,
    const std::filesystem::path& manifestPath,
    const std::string& subAssetPrefix,
    const std::string& extension)
{
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input.good())
        return {};

    auto manifest = nlohmann::json::parse(input, nullptr, false);
    if (!manifest.is_object() || !manifest["dependencies"].is_array())
        return {};

    for (const auto& dependency : manifest["dependencies"])
    {
        if (!dependency.is_object() ||
            dependency.value("kind", std::string {}) != "imported-artifact")
        {
            continue;
        }

        const auto value = dependency.value("value", std::string {});
        const auto separator = value.find('#');
        if (separator == std::string::npos)
            continue;

        const auto assetIdText = value.substr(0u, separator);
        const auto subAssetAndTarget = value.substr(separator + 1u);
        if (subAssetAndTarget.rfind(subAssetPrefix, 0u) != 0u)
            continue;

        const auto targetSeparator = subAssetAndTarget.rfind('@');
        const auto subAssetKey = targetSeparator == std::string::npos
            ? subAssetAndTarget
            : subAssetAndTarget.substr(0u, targetSeparator);
        const auto importedManifestPath =
            root / "Library" / "Artifacts" / assetIdText / "manifest.json";

        std::ifstream importedInput(importedManifestPath, std::ios::binary);
        if (!importedInput.good())
            continue;

        auto importedManifest = nlohmann::json::parse(importedInput, nullptr, false);
        if (!importedManifest.is_object() || !importedManifest["subAssets"].is_array())
            continue;

        for (const auto& subAsset : importedManifest["subAssets"])
        {
            if (!subAsset.is_object() ||
                subAsset.value("subAssetKey", std::string {}) != subAssetKey)
            {
                continue;
            }

            const auto artifactPathText = subAsset.value("artifactPath", std::string {});
            if (artifactPathText.empty())
                return {};

            auto artifactPath = std::filesystem::path(artifactPathText);
            if (artifactPath.extension() != extension)
                return {};
            if (artifactPath.is_relative())
                artifactPath = importedManifestPath.parent_path() / artifactPath;
            return artifactPath.lexically_normal();
        }
    }

    return {};
}

void FlipFirstContentHashForArtifactType(
    NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::ArtifactType artifactType)
{
    bool updated = false;
    for (auto& subAsset : manifest.subAssets)
    {
        if (subAsset.artifactType != artifactType)
            continue;

        auto hash = subAsset.contentHash;
        ASSERT_FALSE(hash.empty());
        hash.back() = hash.back() == '0' ? '1' : '0';
        subAsset.contentHash = std::move(hash);
        updated = true;
        break;
    }

    ASSERT_TRUE(updated);
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

size_t MaxArtifactTelemetryStageBytes(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
{
    size_t bytes = 0u;
    for (const auto& record : records)
    {
        if (record.stage == stage)
            bytes = std::max(bytes, record.byteCount);
    }
    return bytes;
}

NLS::Render::Geometry::Vertex ImportTestVertexAt(float x, float y, float z)
{
    NLS::Render::Geometry::Vertex vertex {};
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    return vertex;
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
    const auto guid = database.AssetPathToGUID("Assets/Models/ImportedHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
    const auto prefabPath = FindPrefabArtifactPath(root, assetId, "prefab:ImportedHero");
    ASSERT_FALSE(prefabPath.empty());
    EXPECT_TRUE(std::filesystem::exists(prefabPath));

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
    size_t artifactFileCount = 0u;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "Library" / "Artifacts"))
    {
        if (entry.is_regular_file() && !entry.path().filename().has_extension())
            ++artifactFileCount;
    }
    EXPECT_GT(artifactFileCount, 0u);
    EXPECT_TRUE(std::filesystem::exists(root / "Library" / "ArtifactDB"));

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmRawModelAsyncDropSchedulesRepairWhenRendererTextureArtifactIsMissing)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "AsyncRepairBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "AsyncRepairHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/AsyncRepairBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "AsyncRepairMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
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
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "AsyncRepairHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/AsyncRepairHero.gltf"));
    const auto artifactRoot = database.GetArtifactRootForAssetPathForTesting(
        "Assets/Models/AsyncRepairHero.gltf");
    const auto textureArtifactPath = FindFirstImportedArtifactPathForSubAssetPrefix(
        root,
        artifactRoot / "manifest.json",
        "texture:",
        ".ntex");
    ASSERT_FALSE(textureArtifactPath.empty());
    ASSERT_TRUE(std::filesystem::exists(textureArtifactPath));
    std::filesystem::remove(textureArtifactPath);
    ASSERT_FALSE(std::filesystem::exists(textureArtifactPath));

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::ImportProgressTracker tracker;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    std::atomic_bool completionCalled = false;
    std::function<void()> scheduledRepair;
    const auto result = bridge.DropModelAssetIntoHierarchyAsync(
        "Models/AsyncRepairHero.gltf",
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
                EXPECT_TRUE(completion.importSucceeded)
                    << FormatDragDropDiagnostics(completion);
                EXPECT_EQ(completion.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
                    << FormatDragDropDiagnostics(completion);
            },
            [&](std::function<void()> task)
            {
                scheduledRepair = std::move(task);
                return true;
            }
        });

    EXPECT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_TRUE(scene.GetGameObjects().empty());
    EXPECT_FALSE(completionCalled.load(std::memory_order_acquire));
    ASSERT_TRUE(static_cast<bool>(scheduledRepair));

    scheduledRepair();

    EXPECT_TRUE(completionCalled.load(std::memory_order_acquire));
    EXPECT_TRUE(std::filesystem::exists(textureArtifactPath));
    EXPECT_FALSE(tracker.HasRunningJobs());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, ColdRawModelAsyncDropRejectsWhenCompletionPipelineIsMissing)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "MissingCompletionHero.gltf";
    WriteTextFile(modelPath, R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[]}]})");
    auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(modelPath);
    ASSERT_TRUE(meta.Save(NLS::Core::Assets::GetAssetMetaPath(modelPath)));

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::ImportProgressTracker tracker;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    auto result = bridge.DropModelAssetIntoHierarchyAsync(
        "Models/MissingCompletionHero.gltf",
        scene,
        {
            {},
            nullptr,
            nullptr,
            &tracker,
            {},
            {}
        });

    EXPECT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport)
        << "Async drop cannot report pending when no scheduler/completion path exists.";
    EXPECT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Rejected)
        << FormatDragDropDiagnostics(result);
    EXPECT_EQ(
        CountDragDropDiagnosticCode(result, "dragdrop-background-task-unavailable"),
        1u)
        << FormatDragDropDiagnostics(result);
    EXPECT_TRUE(scene.GetGameObjects().empty());
    EXPECT_FALSE(tracker.HasRunningJobs());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmRawModelAsyncDropInstantiatesWithoutSchedulingBackgroundImport)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "WarmAsyncHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0] }
            ],
            "nodes": [
                { "name": "WarmAsyncHeroRoot" }
            ]
        })");

    {
        NLS::Editor::Assets::AssetDatabaseFacade database({root});
        ASSERT_TRUE(database.Refresh());
        ASSERT_TRUE(database.ImportAsset("Assets/Models/WarmAsyncHero.gltf"));
    }

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::ImportProgressTracker tracker;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    bool completionCalled = false;
    bool scheduleCalled = false;
    auto result = bridge.DropModelAssetIntoHierarchyAsync(
        "Models/WarmAsyncHero.gltf",
        scene,
        {
            {},
            nullptr,
            nullptr,
            nullptr,
            [&](NLS::Editor::Assets::EditorAssetDragDropBridgeResult)
            {
                completionCalled = true;
            },
            [&](std::function<void()>)
            {
                scheduleCalled = true;
                return true;
            }
        });

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    EXPECT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "WarmAsyncHeroRoot");
    EXPECT_FALSE(completionCalled);
    EXPECT_FALSE(scheduleCalled);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, ColdRawModelDropReportsTerminalFailureWhenBackgroundSchedulingIsUnavailable)
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
    NLS::Editor::Assets::ImportProgressTracker tracker;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    bool completionCalled = false;
    auto result = bridge.DropModelAssetIntoHierarchyAsync(
        "Models/RejectedAsyncHero.gltf",
        scene,
        {
            {},
            nullptr,
            nullptr,
            &tracker,
            [&](NLS::Editor::Assets::EditorAssetDragDropBridgeResult completion)
            {
                completionCalled = true;
                EXPECT_TRUE(completion.handled);
                EXPECT_FALSE(completion.pendingImport);
                EXPECT_FALSE(completion.importSucceeded)
                    << FormatDragDropDiagnostics(completion);
                EXPECT_EQ(completion.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Rejected)
                    << FormatDragDropDiagnostics(completion);
                EXPECT_EQ(
                    CountDragDropDiagnosticCode(completion, "dragdrop-background-task-rejected"),
                    1u)
                    << FormatDragDropDiagnostics(completion);
            },
            [](std::function<void()>)
            {
                return false;
            }
        });

    EXPECT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    EXPECT_EQ(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Rejected)
        << FormatDragDropDiagnostics(result);
    EXPECT_EQ(
        CountDragDropDiagnosticCode(result, "dragdrop-background-task-rejected"),
        1u)
        << FormatDragDropDiagnostics(result);
    EXPECT_TRUE(completionCalled);
    EXPECT_TRUE(scene.GetGameObjects().empty());
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "Artifacts" / meta.id.ToString()));
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB"));
    const auto dropProgressEvents = tracker.GetEvents({1u});
    ASSERT_FALSE(dropProgressEvents.empty());
    EXPECT_NE(
        dropProgressEvents.back().terminalStatus,
        NLS::Editor::Assets::ImportJobTerminalStatus::Succeeded);

    std::filesystem::remove_all(root);
}

#if !NLS_HAS_AUTODESK_FBX_SDK
TEST(GameObjectAssetImportTests, ColdFbxRawModelDropForwardsBackgroundImportDiagnostics)
{
    const auto root = MakeGameObjectAssetImportRoot();
    const auto modelPath = root / "Assets" / "Models" / "DefaultReaderUnavailable.fbx";
    WriteTextFile(modelPath, "not a valid fbx");
    auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(modelPath);
    ASSERT_TRUE(meta.Save(NLS::Core::Assets::GetAssetMetaPath(modelPath)));

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    bool completionCalled = false;
    std::function<void()> scheduledImport;
    auto result = bridge.DropModelAssetIntoHierarchyAsync(
        "Models/DefaultReaderUnavailable.fbx",
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
                EXPECT_EQ(
                    CountDragDropDiagnosticCode(completion, "external-model-importer-fbx-reader-fallback"),
                    1u)
                    << FormatDragDropDiagnostics(completion);
                EXPECT_EQ(
                    CountDragDropDiagnosticCode(completion, "external-model-importer-autodesk-fbx-unavailable"),
                    0u)
                    << FormatDragDropDiagnostics(completion);
#if NLS_HAS_ASSIMP_FBX_IMPORTER
                EXPECT_EQ(
                    CountDragDropDiagnosticCode(completion, "external-model-importer-source-parse-failed"),
                    1u)
                    << FormatDragDropDiagnostics(completion);
                EXPECT_EQ(
                    CountDragDropDiagnosticCode(completion, "external-model-importer-assimp-fbx-unavailable"),
                    0u)
                    << FormatDragDropDiagnostics(completion);
#else
                EXPECT_EQ(
                    CountDragDropDiagnosticCode(completion, "external-model-importer-source-parse-failed"),
                    1u)
                    << FormatDragDropDiagnostics(completion);
                EXPECT_EQ(
                    CountDragDropDiagnosticCode(completion, "external-model-importer-assimp-fbx-unavailable"),
                    1u)
                    << FormatDragDropDiagnostics(completion);
#endif
            },
            [&](std::function<void()> task)
            {
                scheduledImport = std::move(task);
                return true;
            }
        });

    EXPECT_TRUE(result.handled);
    EXPECT_TRUE(result.pendingImport);
    EXPECT_FALSE(completionCalled);
    ASSERT_TRUE(static_cast<bool>(scheduledImport));

    scheduledImport();

    EXPECT_TRUE(completionCalled);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}
#endif

TEST(GameObjectAssetImportTests, AsyncRawModelDropDoesNotRefreshAssetDatabaseOnCallingThread)
{
    const auto bridgeSourcePath =
        std::filesystem::path(NLS_ROOT_DIR) /
        "Project" /
        "Editor" /
        "Assets" /
        "EditorAssetDragDropBridge.cpp";
    std::ifstream input(bridgeSourcePath, std::ios::binary);
    ASSERT_TRUE(input.good());
    const std::string source {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    const auto functionStart = source.find(
        "EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropModelAssetIntoHierarchyAsync(");
    ASSERT_NE(functionStart, std::string::npos);
    const auto nextFunction = source.find(
        "EditorAssetDragDropBridgeResult EditorAssetDragDropBridge::DropImportedAssetHandleIntoHierarchy(",
        functionStart);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto functionBody = source.substr(functionStart, nextFunction - functionStart);

    EXPECT_EQ(functionBody.find("AssetDatabaseFacade database"), std::string::npos);
    EXPECT_EQ(functionBody.find(".Refresh()"), std::string::npos);
    EXPECT_EQ(functionBody.find("LoadSubAssetAtPath"), std::string::npos);
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
    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

TEST(GameObjectAssetImportTests, WarmEditorAssetHandlePreviewAcceptsProjectRelativePayloadPathWithoutAssetsPrefix)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "WarmPreviewProjectRelativeHero.gltf",
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
                { "name": "WarmPreviewProjectRelativeHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/WarmPreviewProjectRelativeHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/WarmPreviewProjectRelativeHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Models/WarmPreviewProjectRelativeHero.gltf",
        assetId,
        "prefab:WarmPreviewProjectRelativeHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true,
        true);

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto prefab = bridge.TryLoadPreviewPrefabArtifact(payload);

    ASSERT_TRUE(prefab.has_value());
    EXPECT_EQ(prefab->assetId, assetId);

    NLS::Engine::SceneSystem::Scene previewScene;
    auto preview = NLS::Engine::Assets::InstantiatePrefabArtifact(*prefab, previewScene);
    ASSERT_FALSE(preview.diagnostics.HasErrors());
    ASSERT_NE(preview.root, nullptr);
    EXPECT_EQ(preview.root->GetName(), "WarmPreviewProjectRelativeHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmEditorAssetHandlePreviewUsesHotCacheAndInvalidatesOnArtifactStamps)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "HotCacheHero.gltf",
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
                { "name": "HotCacheHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/HotCacheHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/HotCacheHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto manifestPath = root / "Library" / "ArtifactDB";
    const auto prefabPath = FindPrefabArtifactPath(root, assetId, "prefab:HotCacheHero");
    const auto meshPath = FindFirstArtifactPathForType(root, assetId, NLS::Core::Assets::ArtifactType::Mesh);
    ASSERT_FALSE(prefabPath.empty());
    ASSERT_TRUE(std::filesystem::is_regular_file(prefabPath));
    const auto prefabArtifactBytes = std::filesystem::file_size(prefabPath);
    ASSERT_FALSE(meshPath.empty());
    ASSERT_TRUE(std::filesystem::is_regular_file(meshPath));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/HotCacheHero.gltf",
        assetId,
        "prefab:HotCacheHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true,
        true);

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto firstPrefab = bridge.TryLoadPreviewPrefabArtifact(payload);
    ASSERT_TRUE(firstPrefab.has_value());
    auto firstRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(firstRecords, ArtifactLoadTelemetryStage::CacheMiss), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(firstRecords, ArtifactLoadTelemetryStage::CacheHit), 0u);
    EXPECT_GE(CountArtifactTelemetryStage(firstRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto cachedPrefab = bridge.TryLoadPreviewPrefabArtifact(payload);
    ASSERT_TRUE(cachedPrefab.has_value());
    auto cachedRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(cachedRecords, ArtifactLoadTelemetryStage::CacheHit), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(cachedRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u);
    EXPECT_GE(
        MaxArtifactTelemetryStageBytes(cachedRecords, ArtifactLoadTelemetryStage::CacheHit),
        static_cast<size_t>(prefabArtifactBytes))
        << "Hot-cache budget telemetry must include the retained prefab artifact payload bytes, not just shallow graph object sizes.";

    FlipArtifactDatabaseContentHashForTest(manifestPath, "prefab:HotCacheHero");
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto manifestInvalidatedPrefab = bridge.TryLoadPreviewPrefabArtifact(payload);
    ASSERT_TRUE(manifestInvalidatedPrefab.has_value());
    auto manifestInvalidatedRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(CountArtifactTelemetryStage(manifestInvalidatedRecords, ArtifactLoadTelemetryStage::CacheHit), 0u);
    EXPECT_GE(CountArtifactTelemetryStage(manifestInvalidatedRecords, ArtifactLoadTelemetryStage::CacheMiss), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(manifestInvalidatedRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    std::error_code error;
    const auto prefabWriteTime = std::filesystem::last_write_time(prefabPath, error);
    ASSERT_FALSE(error);
    std::filesystem::last_write_time(prefabPath, prefabWriteTime + std::chrono::seconds(1), error);
    ASSERT_FALSE(error);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto prefabInvalidatedPrefab = bridge.TryLoadPreviewPrefabArtifact(payload);
    ASSERT_TRUE(prefabInvalidatedPrefab.has_value());
    auto prefabInvalidatedRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(CountArtifactTelemetryStage(prefabInvalidatedRecords, ArtifactLoadTelemetryStage::CacheHit), 0u);
    EXPECT_GE(CountArtifactTelemetryStage(prefabInvalidatedRecords, ArtifactLoadTelemetryStage::CacheMiss), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(prefabInvalidatedRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    const auto meshInvalidationManifestWriteTime = std::filesystem::last_write_time(manifestPath, error);
    ASSERT_FALSE(error);
    {
        auto manifest = LoadArtifactManifestForTest(root, assetId);
        ASSERT_TRUE(manifest.has_value());
        FlipFirstContentHashForArtifactType(*manifest, NLS::Core::Assets::ArtifactType::Mesh);
        SaveArtifactManifestForTest(root, *manifest, "Assets/Models/HotCacheHero.gltf");
    }
    std::filesystem::last_write_time(manifestPath, meshInvalidationManifestWriteTime, error);
    ASSERT_FALSE(error);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto meshInvalidatedPrefab = bridge.TryLoadPreviewPrefabArtifact(payload);
    ASSERT_TRUE(meshInvalidatedPrefab.has_value());
    auto meshInvalidatedRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(CountArtifactTelemetryStage(meshInvalidatedRecords, ArtifactLoadTelemetryStage::CacheHit), 0u);
    EXPECT_GE(CountArtifactTelemetryStage(meshInvalidatedRecords, ArtifactLoadTelemetryStage::CacheMiss), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(meshInvalidatedRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, SceneRestoreAndDragDropBuildSameUnifiedPrefabArtifactKey)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "SharedKeyBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "SharedKeyHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/SharedKeyBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "SharedKeyMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
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
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "SharedKeyHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/SharedKeyHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/SharedKeyHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Models/SharedKeyHero.gltf",
        "prefab:SharedKeyHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);

    NLS::Editor::Assets::UnifiedPrefabLoadRequest sceneRequest;
    sceneRequest.source = source;
    sceneRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    sceneRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    sceneRequest.ownerScopeId = "scene:baseline";
    sceneRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;

    NLS::Editor::Assets::UnifiedPrefabLoadRequest dragRequest = sceneRequest;
    dragRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::DragPreview;
    dragRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::PreviewScene;
    dragRequest.ownerScopeId = "preview:drag";

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto sceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto dragKey = bridge.BuildUnifiedPrefabLoadKey(dragRequest);

    ASSERT_TRUE(sceneKey.has_value());
    ASSERT_TRUE(dragKey.has_value());
    EXPECT_EQ(sceneKey->artifactIdentity, dragKey->artifactIdentity);
    EXPECT_EQ(sceneKey->manifestStamp, dragKey->manifestStamp);
    EXPECT_EQ(sceneKey->prefabArtifactStamp, dragKey->prefabArtifactStamp);
    EXPECT_EQ(sceneKey->rendererArtifactStamp, dragKey->rendererArtifactStamp);
    EXPECT_EQ(sceneKey->runtimeCacheIdentity, dragKey->runtimeCacheIdentity);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, SceneRestorePrefabLoadRequestBlocksUntilRendererResourcesAreReady)
{
    const auto source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        std::filesystem::path("D:/Project"),
        "Assets/Models/StreamingHero.gltf",
        "prefab:StreamingHero",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("ca101010-1010-4010-8010-101010101010")),
        NLS::Core::Assets::AssetType::ModelScene);

    const auto request = NLS::Editor::Assets::MakeSceneRestoreUnifiedPrefabLoadRequest(
        source,
        "Assets/Scenes/Streaming.scene");

    EXPECT_EQ(request.loadMode, NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore);
    EXPECT_EQ(request.ownerKind, NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance);
    EXPECT_EQ(request.ownerScopeId, "Assets/Scenes/Streaming.scene");
    EXPECT_EQ(request.requiredReadiness, NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady);
    EXPECT_FALSE(request.allowPending)
        << "Scene restore is a blocking editor operation: it must not commit graph-only prefab instances.";
}

TEST(GameObjectAssetImportTests, SceneRestorePrefabStreamingUsesSceneScopeAndInstanceOwnerToken)
{
    const auto source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        std::filesystem::path("D:/Project"),
        "Assets/Models/StreamingOwnerHero.gltf",
        "prefab:StreamingOwnerHero",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("ca202020-2020-4020-8020-202020202020")),
        NLS::Core::Assets::AssetType::ModelScene);

    const auto request = NLS::Editor::Assets::MakeSceneRestoreUnifiedPrefabLoadRequest(
        source,
        "Assets/Scenes/StreamingOwner.scene");

    NLS::Engine::GameObject root("StreamingOwnerHero", "Model");
    NLS::Editor::Assets::PrefabInstanceRecord instance;
    instance.prefabAssetId = source.sourceAssetId;
    instance.prefabSubAssetKey = source.prefabSubAssetKey;
    instance.generatedReadOnly = true;
    instance.instanceRoot = &root;

    const auto ownerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(instance);

    EXPECT_EQ(request.loadMode, NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore);
    EXPECT_EQ(request.ownerKind, NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance);
    EXPECT_EQ(request.ownerScopeId, "Assets/Scenes/StreamingOwner.scene");
    EXPECT_EQ(ownerToken.rfind("scene-prefab:", 0u), 0u)
        << "Scene-loaded prefab renderer streaming must use the scene-instance owner identity shared by deletion, scene switch, and trim.";
}

TEST(GameObjectAssetImportTests, SceneRestoreAndDragPreviewShareUnifiedPrefabHotCachePath)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const auto root = MakeGameObjectAssetImportRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "SharedPathBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "SharedPathHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/SharedPathBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "SharedPathMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
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
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "SharedPathHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/SharedPathHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/SharedPathHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/SharedPathHero.gltf",
        "prefab:SharedPathHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);

    NLS::Editor::Assets::UnifiedPrefabLoadRequest sceneRequest;
    sceneRequest.source = source;
    sceneRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    sceneRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    sceneRequest.ownerScopeId = "scene:shared-path";
    sceneRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;

    NLS::Editor::Assets::UnifiedPrefabLoadRequest previewRequest = sceneRequest;
    previewRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::DragPreview;
    previewRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::PreviewScene;
    previewRequest.ownerScopeId = "preview:shared-path";

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto sceneLoad = bridge.LoadUnifiedPrefab(sceneRequest);
    ASSERT_TRUE(sceneLoad.prefab.has_value());
    ASSERT_TRUE(sceneLoad.key.has_value());
    const auto sceneRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(sceneRecords, ArtifactLoadTelemetryStage::CacheMiss), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(sceneRecords, ArtifactLoadTelemetryStage::CacheHit), 0u);
    EXPECT_GE(CountArtifactTelemetryStage(sceneRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto previewLoad = bridge.LoadUnifiedPrefab(previewRequest);
    ASSERT_TRUE(previewLoad.prefab.has_value());
    ASSERT_TRUE(previewLoad.key.has_value());
    EXPECT_EQ(previewLoad.key->runtimeCacheIdentity, sceneLoad.key->runtimeCacheIdentity);
    const auto previewRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(previewRecords, ArtifactLoadTelemetryStage::CacheHit), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(previewRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, UnifiedPrefabSharedHotCacheReusesPrefabGraphWithoutCopyingOnWarmPreview)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "SharedGraphBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "SharedGraphHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/SharedGraphBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "SharedGraphMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
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
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "SharedGraphHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/SharedGraphHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/SharedGraphHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/SharedGraphHero.gltf",
        "prefab:SharedGraphHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);

    NLS::Editor::Assets::UnifiedPrefabLoadRequest sceneRequest;
    sceneRequest.source = source;
    sceneRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    sceneRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    sceneRequest.ownerScopeId = "scene:shared-graph";
    sceneRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;

    NLS::Editor::Assets::UnifiedPrefabLoadRequest previewRequest = sceneRequest;
    previewRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::DragPreview;
    previewRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::PreviewScene;
    previewRequest.ownerScopeId = "preview:shared-graph";

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto sceneLoad = bridge.LoadUnifiedPrefabShared(sceneRequest);
    ASSERT_TRUE(sceneLoad.prefab);
    ASSERT_TRUE(sceneLoad.key.has_value());
    const auto sceneRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(sceneRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheMiss), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(sceneRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto previewLoad = bridge.LoadUnifiedPrefabShared(previewRequest);
    ASSERT_TRUE(previewLoad.prefab);
    ASSERT_TRUE(previewLoad.key.has_value());
    EXPECT_EQ(previewLoad.key->runtimeCacheIdentity, sceneLoad.key->runtimeCacheIdentity);
    EXPECT_EQ(previewLoad.prefab.get(), sceneLoad.prefab.get())
        << "Warm preview should reuse the cached prefab graph handle instead of deep-copying the graph from hot cache.";
    const auto previewRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(previewRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheHit), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(previewRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(previewRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy), 0u);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, FinalDropReusesDragPreviewPrefabGraphHotCache)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "PreviewFinalBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "PreviewFinalHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/PreviewFinalBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "PreviewFinalMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
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
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "PreviewFinalHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PreviewFinalHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PreviewFinalHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/PreviewFinalHero.gltf",
        "prefab:PreviewFinalHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);

    NLS::Editor::Assets::UnifiedPrefabLoadRequest previewRequest;
    previewRequest.source = source;
    previewRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::DragPreview;
    previewRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::PreviewScene;
    previewRequest.ownerScopeId = "preview:preview-final";
    previewRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;

    NLS::Editor::Assets::UnifiedPrefabLoadRequest finalDropRequest = previewRequest;
    finalDropRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::FinalDrop;
    finalDropRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    finalDropRequest.ownerScopeId = "final:preview-final";
    finalDropRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto previewLoad = bridge.LoadUnifiedPrefabShared(previewRequest);
    ASSERT_TRUE(previewLoad.prefab);
    const auto previewRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(previewRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto finalLoad = bridge.LoadUnifiedPrefabShared(finalDropRequest);
    ASSERT_TRUE(finalLoad.prefab);
    EXPECT_EQ(finalLoad.prefab.get(), previewLoad.prefab.get());
    const auto finalRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(finalRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::DependencyScan), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(finalRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheHit), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(finalRecords, NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, UnifiedPrefabLoadKeyInvalidatesManifestPrefabMeshMaterialAndTextureStamps)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "InvalidationBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "InvalidationHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/InvalidationBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "InvalidationMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
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
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "InvalidationHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/InvalidationHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/InvalidationHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
    const auto manifestPath = root / "Library" / "ArtifactDB";
    auto manifest = LoadArtifactManifestForTest(root, assetId);
    ASSERT_TRUE(manifest.has_value());
    const auto prefabPath = FindPrefabArtifactPath(root, assetId, "prefab:InvalidationHero");
    const auto meshPath = FindFirstArtifactPathForType(root, assetId, NLS::Core::Assets::ArtifactType::Mesh);
    const auto materialPath = FindFirstArtifactPathForType(root, assetId, NLS::Core::Assets::ArtifactType::Material);
    const auto texturePath = FindFirstArtifactPathForType(root, assetId, NLS::Core::Assets::ArtifactType::Texture);
    ASSERT_TRUE(std::filesystem::is_directory(manifestPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(manifestPath / "data.mdb"));
    ASSERT_TRUE(std::filesystem::is_regular_file(prefabPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(meshPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(materialPath));
    ASSERT_TRUE(std::filesystem::is_regular_file(texturePath));
    ASSERT_FALSE(FindFirstContentHashForArtifactType(*manifest, NLS::Core::Assets::ArtifactType::Mesh).empty());
    ASSERT_FALSE(FindFirstContentHashForArtifactType(*manifest, NLS::Core::Assets::ArtifactType::Material).empty());
    ASSERT_FALSE(FindFirstContentHashForArtifactType(*manifest, NLS::Core::Assets::ArtifactType::Texture).empty());

    const auto source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/InvalidationHero.gltf",
        "prefab:InvalidationHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);

    NLS::Editor::Assets::UnifiedPrefabLoadRequest sceneRequest;
    sceneRequest.source = source;
    sceneRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    sceneRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    sceneRequest.ownerScopeId = "scene:baseline";
    sceneRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;

    NLS::Editor::Assets::UnifiedPrefabLoadRequest finalDropRequest = sceneRequest;
    finalDropRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::FinalDrop;
    finalDropRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    finalDropRequest.ownerScopeId = "scene:drop";

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto baselineSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto baselineDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(baselineSceneKey.has_value());
    ASSERT_TRUE(baselineDropKey.has_value());
    EXPECT_EQ(baselineSceneKey->runtimeCacheIdentity, baselineDropKey->runtimeCacheIdentity);

    FlipArtifactDatabaseContentHashForTest(manifestPath, "prefab:InvalidationHero");
    const auto manifestSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto manifestDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(manifestSceneKey.has_value());
    ASSERT_TRUE(manifestDropKey.has_value());
    EXPECT_NE(manifestSceneKey->runtimeCacheIdentity, baselineSceneKey->runtimeCacheIdentity);
    EXPECT_EQ(manifestSceneKey->runtimeCacheIdentity, manifestDropKey->runtimeCacheIdentity);

    std::error_code error;
    const auto prefabWriteTime = std::filesystem::last_write_time(prefabPath, error);
    ASSERT_FALSE(error);
    std::filesystem::last_write_time(prefabPath, prefabWriteTime + std::chrono::seconds(1), error);
    ASSERT_FALSE(error);
    const auto prefabSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto prefabDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(prefabSceneKey.has_value());
    ASSERT_TRUE(prefabDropKey.has_value());
    EXPECT_NE(prefabSceneKey->runtimeCacheIdentity, manifestSceneKey->runtimeCacheIdentity);
    EXPECT_EQ(prefabSceneKey->runtimeCacheIdentity, prefabDropKey->runtimeCacheIdentity);

    manifest = LoadArtifactManifestForTest(root, assetId);
    ASSERT_TRUE(manifest.has_value());
    FlipFirstContentHashForArtifactType(*manifest, NLS::Core::Assets::ArtifactType::Mesh);
    SaveArtifactManifestForTest(root, *manifest, "Assets/Models/InvalidationHero.gltf");
    const auto meshSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto meshDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(meshSceneKey.has_value());
    ASSERT_TRUE(meshDropKey.has_value());
    EXPECT_NE(meshSceneKey->runtimeCacheIdentity, prefabSceneKey->runtimeCacheIdentity);
    EXPECT_EQ(meshSceneKey->runtimeCacheIdentity, meshDropKey->runtimeCacheIdentity);

    FlipFirstContentHashForArtifactType(*manifest, NLS::Core::Assets::ArtifactType::Material);
    SaveArtifactManifestForTest(root, *manifest, "Assets/Models/InvalidationHero.gltf");
    const auto materialSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto materialDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(materialSceneKey.has_value());
    ASSERT_TRUE(materialDropKey.has_value());
    EXPECT_NE(materialSceneKey->runtimeCacheIdentity, meshSceneKey->runtimeCacheIdentity);
    EXPECT_EQ(materialSceneKey->runtimeCacheIdentity, materialDropKey->runtimeCacheIdentity);

    FlipFirstContentHashForArtifactType(*manifest, NLS::Core::Assets::ArtifactType::Texture);
    SaveArtifactManifestForTest(root, *manifest, "Assets/Models/InvalidationHero.gltf");
    const auto textureSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto textureDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(textureSceneKey.has_value());
    ASSERT_TRUE(textureDropKey.has_value());
    EXPECT_NE(textureSceneKey->runtimeCacheIdentity, materialSceneKey->runtimeCacheIdentity);
    EXPECT_EQ(textureSceneKey->runtimeCacheIdentity, textureDropKey->runtimeCacheIdentity);

    const auto textureWriteTime = std::filesystem::last_write_time(texturePath, error);
    ASSERT_FALSE(error);
    std::filesystem::last_write_time(texturePath, textureWriteTime + std::chrono::seconds(1), error);
    ASSERT_FALSE(error);
    const auto textureFileSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto textureFileDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(textureFileSceneKey.has_value());
    ASSERT_TRUE(textureFileDropKey.has_value());
    EXPECT_NE(textureFileSceneKey->runtimeCacheIdentity, textureSceneKey->runtimeCacheIdentity);
    EXPECT_EQ(textureFileSceneKey->runtimeCacheIdentity, textureFileDropKey->runtimeCacheIdentity);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, SceneRestoreReadyKeyIncludesRendererArtifactFileStamps)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "GraphOnlyBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "GraphOnlySceneHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "images": [
                { "uri": "../Textures/GraphOnlyBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [{ "source": 0 }],
            "materials": [
                {
                    "name": "GraphOnlyMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
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
                            "indices": 1,
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [
                { "name": "GraphOnlySceneHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/GraphOnlySceneHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/GraphOnlySceneHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
    const auto meshPath = FindFirstArtifactPathForType(root, assetId, NLS::Core::Assets::ArtifactType::Mesh);
    ASSERT_TRUE(std::filesystem::is_regular_file(meshPath));

    const auto source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/GraphOnlySceneHero.gltf",
        "prefab:GraphOnlySceneHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);

    auto sceneRequest = NLS::Editor::Assets::MakeSceneRestoreUnifiedPrefabLoadRequest(
        source,
        "scene:graph-only");
    ASSERT_EQ(sceneRequest.requiredReadiness, NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady);

    auto finalDropRequest = sceneRequest;
    finalDropRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::FinalDrop;
    finalDropRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto baselineSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto baselineDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(baselineSceneKey.has_value());
    ASSERT_TRUE(baselineDropKey.has_value());
    EXPECT_FALSE(baselineSceneKey->rendererArtifactStamp.empty());
    EXPECT_FALSE(baselineDropKey->rendererArtifactStamp.empty());

    std::error_code error;
    const auto meshWriteTime = std::filesystem::last_write_time(meshPath, error);
    ASSERT_FALSE(error);
    std::filesystem::last_write_time(meshPath, meshWriteTime + std::chrono::seconds(1), error);
    ASSERT_FALSE(error);

    const auto meshTouchedSceneKey = bridge.BuildUnifiedPrefabLoadKey(sceneRequest);
    const auto meshTouchedDropKey = bridge.BuildUnifiedPrefabLoadKey(finalDropRequest);
    ASSERT_TRUE(meshTouchedSceneKey.has_value());
    ASSERT_TRUE(meshTouchedDropKey.has_value());
    EXPECT_NE(meshTouchedSceneKey->runtimeCacheIdentity, baselineSceneKey->runtimeCacheIdentity)
        << "Scene restore keys must include renderer artifact stamps so blocking restore does not reuse stale renderer dependencies.";
    EXPECT_NE(meshTouchedDropKey->runtimeCacheIdentity, baselineDropKey->runtimeCacheIdentity)
        << "Ready final drop keys still need renderer artifact invalidation.";

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, ReadyUnifiedPrefabLoadDoesNotHideMissingRendererDependencies)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "HotCacheMissingMeshHero.gltf",
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
                { "name": "HotCacheMissingMeshHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/HotCacheMissingMeshHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/HotCacheMissingMeshHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto meshPath = FindFirstArtifactPathForType(root, assetId, NLS::Core::Assets::ArtifactType::Mesh);
    ASSERT_FALSE(meshPath.empty());
    ASSERT_TRUE(std::filesystem::is_regular_file(meshPath));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/HotCacheMissingMeshHero.gltf",
        assetId,
        "prefab:HotCacheMissingMeshHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true,
        true);

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto warmPrefab = bridge.TryLoadPreviewPrefabArtifact(payload);
    ASSERT_TRUE(warmPrefab.has_value());

    std::filesystem::remove(meshPath);
    ASSERT_FALSE(std::filesystem::exists(meshPath));

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    NLS::Editor::Assets::UnifiedPrefabLoadRequest readyRequest;
    readyRequest.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/HotCacheMissingMeshHero.gltf",
        "prefab:HotCacheMissingMeshHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);
    readyRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::FinalDrop;
    readyRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    readyRequest.ownerScopeId = "scene:ready-load";
    readyRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;
    auto missingDependencyLoad = bridge.LoadUnifiedPrefabShared(readyRequest);
    EXPECT_FALSE(missingDependencyLoad.prefab);
    EXPECT_TRUE(missingDependencyLoad.rendererDependencyMissing);
    EXPECT_EQ(missingDependencyLoad.diagnosticCode, "dragdrop-renderer-dependency-missing");
    const auto records = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::CacheHit), 0u);
    EXPECT_GE(CountArtifactTelemetryStage(records, ArtifactLoadTelemetryStage::DependencyScan), 1u);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, FinalDropRejectsGraphOnlyCommitWhenRendererDependenciesStillPending)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "PendingRendererDropHero.gltf",
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
                { "name": "PendingRendererDropHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PendingRendererDropHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PendingRendererDropHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto meshPath = FindFirstArtifactPathForType(root, assetId, NLS::Core::Assets::ArtifactType::Mesh);
    ASSERT_FALSE(meshPath.empty());
    ASSERT_TRUE(std::filesystem::is_regular_file(meshPath));
    std::filesystem::remove(meshPath);
    ASSERT_FALSE(std::filesystem::exists(meshPath));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayloadForTesting(
        "Assets/Models/PendingRendererDropHero.gltf",
        assetId,
        "prefab:PendingRendererDropHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(payload, scene);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport)
        << "Final drop must fail explicitly instead of leaving an unowned pending drop when renderer dependencies are missing.";
    EXPECT_NE(result.dragDrop.status, NLS::Editor::Assets::DragDropOperationStatus::Committed)
        << FormatDragDropDiagnostics(result);
    EXPECT_EQ(
        CountDragDropDiagnosticCode(result, "dragdrop-renderer-dependency-missing"),
        1u)
        << FormatDragDropDiagnostics(result);
    EXPECT_FALSE(result.dragDrop.instance.has_value());
    EXPECT_FALSE(result.dragDrop.deferredAssetReferenceResolutionRequested);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, SceneRestoreRejectsGraphOnlyCommitWhenRendererDependenciesStillPending)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "PendingRendererSceneHero.gltf",
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
                { "name": "PendingRendererSceneHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PendingRendererSceneHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PendingRendererSceneHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto meshPath = FindFirstArtifactPathForType(
        root,
        assetId,
        NLS::Core::Assets::ArtifactType::Mesh);
    ASSERT_FALSE(meshPath.empty());
    ASSERT_TRUE(std::filesystem::is_regular_file(meshPath));
    std::filesystem::remove(meshPath);
    ASSERT_FALSE(std::filesystem::exists(meshPath));

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto request = NLS::Editor::Assets::MakeSceneRestoreUnifiedPrefabLoadRequest(
        NLS::Editor::Assets::NormalizePrefabSourceIdentity(
            root,
            "Assets/Models/PendingRendererSceneHero.gltf",
            "prefab:PendingRendererSceneHero",
            assetId,
            NLS::Core::Assets::AssetType::ModelScene),
        "Assets/Scenes/PendingRendererScene.scene");

    const auto result = bridge.LoadUnifiedPrefabShared(request);

    EXPECT_FALSE(result.prefab)
        << "Scene restore must not return a graph-only prefab when renderer dependencies are missing.";
    EXPECT_FALSE(result.pending)
        << "Scene restore is blocking; missing renderer dependencies are a restore failure, not a pending commit.";
    EXPECT_TRUE(result.rendererDependencyMissing);
    EXPECT_EQ(result.diagnosticCode, "dragdrop-renderer-dependency-missing");

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, SceneRestoreLoaderSynchronouslyReimportsMissingRendererArtifacts)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "RepairRendererSceneHero.gltf",
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
                { "name": "RepairRendererSceneHeroRoot", "mesh": 0 }
            ]
        })");

    constexpr const char* assetPath = "Assets/Models/RepairRendererSceneHero.gltf";
    constexpr const char* subAssetKey = "prefab:RepairRendererSceneHero";
    constexpr const char* sceneOwnerScope = "Assets/Scenes/RepairRendererScene.scene";

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset(assetPath));
    const auto guid = database.AssetPathToGUID(assetPath);
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto meshPath = FindFirstArtifactPathForType(
        root,
        assetId,
        NLS::Core::Assets::ArtifactType::Mesh);
    ASSERT_FALSE(meshPath.empty());
    ASSERT_TRUE(std::filesystem::is_regular_file(meshPath));
    std::filesystem::remove(meshPath);
    ASSERT_FALSE(std::filesystem::exists(meshPath));

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto request = NLS::Editor::Assets::MakeSceneRestoreUnifiedPrefabLoadRequest(
        NLS::Editor::Assets::NormalizePrefabSourceIdentity(
            root,
            assetPath,
            subAssetKey,
            assetId,
            NLS::Core::Assets::AssetType::ModelScene),
        sceneOwnerScope);
    const auto graphOnlyWouldDisappear = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_FALSE(graphOnlyWouldDisappear.prefab);
    ASSERT_TRUE(graphOnlyWouldDisappear.rendererDependencyMissing);

    const auto artifact = NLS::Editor::Assets::LoadSceneRestorePrefabArtifactReady(
        bridge,
        root,
        assetPath,
        subAssetKey,
        assetId,
        (root / assetPath).lexically_normal(),
        sceneOwnerScope);

    ASSERT_NE(artifact, nullptr)
        << "Top-level scene restore must synchronously repair missing renderer artifacts instead of "
           "letting generated-model prefab instances disappear.";
    EXPECT_TRUE(std::filesystem::is_regular_file(meshPath));

    NLS::Engine::SceneSystem::Scene loadedScene;
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    NLS::Editor::Assets::PrefabUtilityFacade facade;
    auto mutableArtifact = *artifact;
    const auto restore = facade.InstantiatePrefab({
        &mutableArtifact,
        assetId,
        subAssetKey,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("ac303030-3030-4030-8030-303030303030")),
        true
    }, loadedScene);

    ASSERT_EQ(restore.status, NLS::Editor::Assets::PrefabOperationStatus::Committed);
    ASSERT_TRUE(restore.instance.has_value());
    registry.Register(*restore.instance);
    EXPECT_NE(loadedScene.FindGameObjectByName("RepairRendererSceneHeroRoot"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, MeshArtifactPrewarmReusesEquivalentResourceManagerCacheEntry)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");

    const std::string relativeMeshPath =
        "Library/Artifacts/11/1111111111111111111111111111111111111111111111111111111111111111";
    const auto absoluteMeshPath = (root / relativeMeshPath).lexically_normal();

    NLS::Render::Assets::MeshArtifactData meshArtifact;
    meshArtifact.vertices = {
        ImportTestVertexAt(-1.0f, 0.0f, 0.0f),
        ImportTestVertexAt(1.0f, 0.0f, 0.0f),
        ImportTestVertexAt(0.0f, 1.0f, 0.0f)
    };
    meshArtifact.indices = { 0u, 1u, 2u };
    meshArtifact.materialIndex = 0u;
    WriteBinaryFile(absoluteMeshPath, NLS::Render::Assets::SerializeMeshArtifact(meshArtifact));

    NLS::Core::ResourceManagement::MeshManager meshManager;

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto* firstMesh = meshManager.PrewarmArtifact(relativeMeshPath);
    ASSERT_NE(firstMesh, nullptr);
    const auto firstRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(firstRecords, ArtifactLoadTelemetryStage::NativeArtifactFileRead), 1u);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto* secondMesh = meshManager.PrewarmArtifact(absoluteMeshPath.string());
    ASSERT_NE(secondMesh, nullptr);
    EXPECT_EQ(secondMesh, firstMesh)
        << "Generated-model preview/drop paths must reuse the resource-manager Mesh cache even when prefab "
           "resolved assets arrive as equivalent relative or absolute artifact paths.";
    const auto secondRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(CountArtifactTelemetryStage(secondRecords, ArtifactLoadTelemetryStage::NativeArtifactFileRead), 0u)
        << "A repeated equivalent mesh artifact prewarm should not cold-read the same mesh artifact again.";

    meshManager.UnloadResources();
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, RepeatedGeneratedModelPreviewReusesManySubmeshRuntimeCacheEntries)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");

    std::ostringstream meshes;
    std::ostringstream nodes;
    for (int index = 0; index < 4; ++index)
    {
        if (index > 0)
        {
            meshes << ",\n";
            nodes << ",\n";
        }
        meshes <<
            "                {\n"
            "                    \"name\": \"Piece" << index << "\",\n"
            "                    \"primitives\": [\n"
            "                        { \"attributes\": { \"POSITION\": 0 }, \"indices\": 1 }\n"
            "                    ]\n"
            "                }";
        nodes <<
            "                { \"name\": \"Piece" << index << "\", \"mesh\": " << index << " }";
    }

    WriteTextFile(
        root / "Assets" / "Models" / "RuntimeCacheHero.gltf",
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [
                { "nodes": [0, 1, 2, 3] }
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
)" + meshes.str() + R"(
            ],
            "nodes": [
)" + nodes.str() + R"(
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/RuntimeCacheHero.gltf"));

    auto prefab = database.LoadPrefabArtifactAtPath(
        "Assets/Models/RuntimeCacheHero.gltf",
        "prefab:RuntimeCacheHero");
    ASSERT_TRUE(prefab.has_value());

    std::vector<std::string> meshArtifactPaths;
    for (const auto& resolved : prefab->resolvedAssets)
    {
        if (resolved.expectedType == "Mesh" && !resolved.artifactPath.empty())
            meshArtifactPaths.push_back(std::filesystem::path(resolved.artifactPath).lexically_normal().generic_string());
    }
    ASSERT_GE(meshArtifactPaths.size(), 4u);

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    for (const auto& meshPath : meshArtifactPaths)
        ASSERT_NE(meshManager.PrewarmArtifact(meshPath), nullptr);

    const auto coldRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(
        CountArtifactTelemetryStage(coldRecords, ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        meshArtifactPaths.size());

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    for (const auto& meshPath : meshArtifactPaths)
        ASSERT_NE(meshManager.PrewarmArtifact(meshPath), nullptr);

    const auto warmRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountArtifactTelemetryStage(warmRecords, ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u)
        << "Repeated generated-model preview/drop must reuse loaded submesh runtime cache entries instead of "
           "cold-reading each submesh artifact again through equivalent project-relative paths.";

    meshManager.UnloadResources();
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, GeneratedModelPrefabLoadPersistsPreparedGraphCacheAcrossEditorSession)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to clear the in-memory prefab hot cache.";
#else
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "PersistentPreparedCacheHero.gltf",
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
                { "name": "PersistentPreparedCacheHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PersistentPreparedCacheHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PersistentPreparedCacheHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    NLS::Editor::Assets::UnifiedPrefabLoadRequest request;
    request.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/PersistentPreparedCacheHero.gltf",
        "prefab:PersistentPreparedCacheHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);
    request.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    request.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    request.ownerScopeId = "scene:persistent-prepared-cache";
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    request.allowPending = false;

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto coldLoad = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(coldLoad.prefab, nullptr);
    const auto coldRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(coldRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto preparedLoad = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(preparedLoad.prefab, nullptr);
    EXPECT_EQ(preparedLoad.prefab->graph.root, coldLoad.prefab->graph.root);
    EXPECT_EQ(preparedLoad.prefab->resolvedAssets.size(), coldLoad.prefab->resolvedAssets.size());
    const auto preparedRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(preparedRecords, ArtifactLoadTelemetryStage::CacheHit), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(preparedRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u)
        << "After an editor-session memory cache miss, scene restore should reuse the persistent prepared "
           "prefab graph cache instead of reparsing the .prefab payload.";

    std::filesystem::remove_all(root);
#endif
}

TEST(GameObjectAssetImportTests, PreparedGraphCachePersistsRendererDependencyTemplatesAcrossEditorSession)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to clear the in-memory prefab hot cache.";
#else
    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "PersistentRendererTemplateHero.gltf",
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
                { "name": "PersistentRendererTemplateHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PersistentRendererTemplateHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PersistentRendererTemplateHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    NLS::Editor::Assets::UnifiedPrefabLoadRequest request;
    request.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/PersistentRendererTemplateHero.gltf",
        "prefab:PersistentRendererTemplateHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);
    request.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    request.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    request.ownerScopeId = "scene:persistent-renderer-template";
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;
    request.allowPending = false;

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto coldLoad = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(coldLoad.prefab, nullptr);
    ASSERT_TRUE(coldLoad.key.has_value());

    auto coldTemplate = NLS::Editor::Assets::TryGetImportedPrefabRendererDependencyTemplates(*coldLoad.key);
    ASSERT_TRUE(coldTemplate.has_value());
    EXPECT_EQ(coldTemplate->size(), 1u);
    EXPECT_FALSE(coldTemplate->front().meshPath.empty());

    const auto cacheRoot = root / "Library" / "PreparedPrefabCache";
    std::filesystem::path cachePath;
    size_t cacheFileCount = 0u;
    for (const auto& entry : std::filesystem::directory_iterator(cacheRoot))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            ++cacheFileCount;
            cachePath = entry.path();
        }
    }
    ASSERT_EQ(cacheFileCount, 1u);
    ASSERT_FALSE(cachePath.empty());
    std::ifstream cacheInput(cachePath, std::ios::binary);
    ASSERT_TRUE(cacheInput.is_open());
    auto cacheJson = nlohmann::json::parse(cacheInput, nullptr, false);
    cacheInput.close();
    ASSERT_FALSE(cacheJson.is_discarded());
    ASSERT_TRUE(cacheJson.contains("rendererDependencyTemplates"));
    ASSERT_TRUE(cacheJson["rendererDependencyTemplates"].is_array());
    ASSERT_FALSE(cacheJson["rendererDependencyTemplates"].empty());
    cacheJson["rendererDependencyTemplates"][0]["meshPath"] =
        "Library/Artifacts/11/1111111111111111111111111111111111111111111111111111111111111111";
    {
        std::ofstream cacheOutput(cachePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(cacheOutput.is_open());
        cacheOutput << cacheJson.dump();
    }

    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    auto preparedLoad = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(preparedLoad.prefab, nullptr);
    ASSERT_TRUE(preparedLoad.key.has_value());

    auto preparedTemplate = NLS::Editor::Assets::TryGetImportedPrefabRendererDependencyTemplates(*preparedLoad.key);
    ASSERT_TRUE(preparedTemplate.has_value());
    ASSERT_EQ(preparedTemplate->size(), coldTemplate->size());
    EXPECT_EQ(preparedTemplate->front().sourceObject, coldTemplate->front().sourceObject);
    EXPECT_EQ(
        preparedTemplate->front().meshPath,
        "Library/Artifacts/11/1111111111111111111111111111111111111111111111111111111111111111");
    EXPECT_GE(
        CountArtifactTelemetryStage(
            NLS::Core::Assets::SnapshotArtifactLoadTelemetry(),
            NLS::Core::Assets::ArtifactLoadTelemetryStage::CacheHit),
        1u);

    std::filesystem::remove_all(root);
#endif
}

TEST(GameObjectAssetImportTests, WarmEditorAssetHandlePreviewDerivesDefaultPrefabKeyWhenPayloadSubAssetKeyIsEmpty)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "WarmPreviewWholeFileHero.gltf",
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
                { "name": "WarmPreviewWholeFileHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/WarmPreviewWholeFileHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/WarmPreviewWholeFileHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Models/WarmPreviewWholeFileHero.gltf",
        assetId,
        {},
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
    EXPECT_EQ(preview.root->GetName(), "WarmPreviewWholeFileHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmEditorAssetHandlePreviewRejectsStaleAbsoluteArtifactPaths)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "RemappedPreviewHero.gltf",
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
                { "name": "RemappedPreviewHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/RemappedPreviewHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/RemappedPreviewHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    {
        auto manifest = LoadArtifactManifestForTest(root, assetId);
        ASSERT_TRUE(manifest.has_value());

        const auto staleArtifactRoot =
            root.parent_path() / "StaleProjectCopy" / "Library" / "Artifacts";
        ReplaceAllArtifactDatabaseArtifactPathsForTest(
            root / "Library" / "ArtifactDB",
            staleArtifactRoot);
    }
    EXPECT_FALSE(LoadArtifactManifestForTest(root, assetId).has_value());
    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
    std::filesystem::remove_all(root / "Library" / "PreparedPrefabCache");

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Models/RemappedPreviewHero.gltf",
        assetId,
        "prefab:RemappedPreviewHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true,
        true);

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto prefab = bridge.TryLoadPreviewPrefabArtifact(payload);

    EXPECT_FALSE(prefab.has_value());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmEditorAssetHandlePreviewAcceptsLegacyManifestWithoutRootTargetPlatform)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "LegacyPreviewHero.gltf",
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
                { "name": "LegacyPreviewHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/LegacyPreviewHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/LegacyPreviewHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Models/LegacyPreviewHero.gltf",
        assetId,
        "prefab:LegacyPreviewHero",
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
    EXPECT_EQ(preview.root->GetName(), "LegacyPreviewHeroRoot");

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, WarmEditorAssetHandlePreviewRejectsLegacyManifestWithMixedArtifactPlatforms)
{
    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "MixedPlatformPreviewHero.gltf",
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
                { "name": "MixedPlatformPreviewHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/MixedPlatformPreviewHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/MixedPlatformPreviewHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    {
        auto manifest = LoadArtifactManifestForTest(root, assetId);
        ASSERT_TRUE(manifest.has_value());
        ASSERT_FALSE(manifest->subAssets.empty());
        manifest->subAssets.front().targetPlatform = "win64";
        SaveArtifactManifestForTest(root, *manifest, "Assets/Models/MixedPlatformPreviewHero.gltf");
    }

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Models/MixedPlatformPreviewHero.gltf",
        assetId,
        "prefab:MixedPlatformPreviewHero",
        NLS::Core::Assets::ArtifactType::Prefab,
        true,
        true,
        true);

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto prefab = bridge.TryLoadPreviewPrefabArtifact(payload);

    EXPECT_FALSE(prefab.has_value());

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

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    EXPECT_TRUE(std::filesystem::exists(root / "Library" / "ArtifactDB"));
    const auto prefabPath = FindPrefabArtifactPath(root, assetId, "prefab:ProjectLibraryHero");
    ASSERT_FALSE(prefabPath.empty());
    EXPECT_TRUE(std::filesystem::exists(prefabPath));
    EXPECT_FALSE(std::filesystem::exists(root / "Assets" / "Library"));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    const auto stalePayload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    const auto stalePayload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    const auto stalePayload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    {
        auto manifest = LoadArtifactManifestForTest(root, assetId);
        ASSERT_TRUE(manifest.has_value());
        ++manifest->importerVersion;
        SaveArtifactManifestForTest(root, *manifest, "Assets/Models/StaleImporterMetadataHero.gltf");
    }

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

TEST(GameObjectAssetImportTests, EditorAssetHandleDropTreatsStaleArtifactDatabaseImporterVersionAsPendingWithoutThrow)
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

    auto manifest = LoadArtifactManifestForTest(root, assetId);
    ASSERT_TRUE(manifest.has_value());
    ++manifest->importerVersion;
    SaveArtifactManifestForTest(root, *manifest, "Assets/Models/MalformedManifestHero.gltf");

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    const auto escapedPrefabPath = outsideRoot /
        "1111111111111111111111111111111111111111111111111111111111111111";
    {
        std::filesystem::create_directories(outsideRoot);
        const auto prefabPath = FindPrefabArtifactPath(root, assetId, "prefab:EscapingArtifactHero");
        ASSERT_FALSE(prefabPath.empty());
        std::filesystem::copy_file(
            prefabPath,
            escapedPrefabPath,
            std::filesystem::copy_options::overwrite_existing);

        ReplaceArtifactDatabaseFieldForTest(
            root / "Library" / "ArtifactDB",
            "prefab:EscapingArtifactHero",
            7u,
            "../" + outsideRoot.filename().generic_string() +
                "/1111111111111111111111111111111111111111111111111111111111111111");
    }

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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
    std::error_code error;
    const auto importedModelWriteTime = std::filesystem::last_write_time(modelPath, error);
    ASSERT_FALSE(error);
    const auto importedModelStamp = TestFileStamp(modelPath);
    ASSERT_FALSE(importedModelStamp.empty());

    WriteTextFile(
        modelPath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "NewRoot" }]
        })");
    std::filesystem::last_write_time(modelPath, importedModelWriteTime + std::chrono::seconds(1), error);
    ASSERT_FALSE(error);
    ASSERT_NE(TestFileStamp(modelPath), importedModelStamp);
    ASSERT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/Models/StaleHandleHero.gltf"));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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

    {
        auto manifest = LoadArtifactManifestForTest(root, assetId);
        ASSERT_TRUE(manifest.has_value());
        manifest->dependencies.push_back({
            NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
            "../" + escapedDependency.filename().generic_string(),
            TestFileStamp(escapedDependency)
        });
        SaveArtifactManifestForTest(root, *manifest, "Assets/Models/EscapedDependencyHero.gltf");
    }

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
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
    ASSERT_EQ(CountSceneRootGameObjects(scene), 1u);

    auto* importedRoot = FindSceneRootGameObject(scene);
    ASSERT_NE(importedRoot, nullptr);
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
    EXPECT_FALSE(std::filesystem::path(looseMeshFilter->GetModelPath()).has_extension());
    EXPECT_TRUE(NLS::Render::Assets::LoadMeshArtifact(looseMeshFilter->GetModelPath()).has_value());
    ASSERT_EQ(looseMeshRenderer->GetMaterialPaths().size(), 1u);
    EXPECT_NE(looseMeshRenderer->GetMaterialPaths()[0].find("Library"), std::string::npos);
    EXPECT_FALSE(std::filesystem::path(looseMeshRenderer->GetMaterialPaths()[0]).has_extension());

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
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerService(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerService(materialManager);

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

TEST(GameObjectAssetImportTests, GeneratedModelDragSynchronouslyPrewarmsRendererResourcesForSceneInsertion)
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
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerService(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerService(materialManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::ShaderManager> shaderManagerService(shaderManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureManagerService(textureManager);

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

    bool hasMeshArtifact = false;
    for (const auto& resolved : prefab->resolvedAssets)
    {
        if (!resolved.artifactPath.empty())
            EXPECT_FALSE(std::filesystem::path(resolved.artifactPath).filename().has_extension())
                << resolved.artifactPath;
        if (resolved.expectedType == "Mesh" && !resolved.artifactPath.empty())
            hasMeshArtifact = true;
    }
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
    EXPECT_FALSE(std::filesystem::path(meshFilter->GetModelPath()).has_extension());
    EXPECT_TRUE(NLS::Render::Assets::LoadMeshArtifact(meshFilter->GetModelPath()).has_value());
    EXPECT_NE(meshFilter->GetMeshReference().Get(), nullptr);
    EXPECT_TRUE(meshManager.IsResourceRegistered(meshFilter->GetModelPath()));
    ASSERT_EQ(renderer->GetMaterialPaths().size(), 1u);
    EXPECT_NE(renderer->GetMaterialAtIndex(0), nullptr);
    EXPECT_FALSE(renderer->GetMaterialPaths()[0].empty());
    EXPECT_TRUE(materialManager.IsResourceRegistered(renderer->GetMaterialPaths()[0]));

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, GeneratedModelSceneRestoreSynchronouslyPrewarmsRuntimeResources)
{
    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "SceneRestoreColdHero.gltf",
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
                { "name": "SceneRestoreColdHeroRoot", "mesh": 0 }
            ]
        })");

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/SceneRestoreColdHero.gltf"));
    auto prefab = database.LoadPrefabArtifactAtPath(
        "Assets/Models/SceneRestoreColdHero.gltf",
        "prefab:SceneRestoreColdHero");
    ASSERT_TRUE(prefab.has_value());
    prefab->generatedModelPrefab = true;

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

    NLS::Editor::Assets::PrefabUtilityFacade facade;
    NLS::Engine::SceneSystem::Scene sourceScene;
    NLS::Editor::Assets::PrefabInstanceRegistry sourceRegistry;
    auto instantiate = facade.InstantiatePrefab({
        &*prefab,
        prefab->assetId,
        "prefab:SceneRestoreColdHero",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("ac101010-1010-4010-8010-101010101010")),
        true
    }, sourceScene);
    ASSERT_EQ(instantiate.status, NLS::Editor::Assets::PrefabOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    sourceRegistry.Register(*instantiate.instance);

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(sourceScene);
    facade.AnnotateSceneDocumentWithPrefabInstances(document, sourceScene, sourceRegistry);
    ASSERT_EQ(document.prefabInstances.size(), 1u);

    auto loadedScene = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiateScene(document);
    ASSERT_NE(loadedScene, nullptr);
    EXPECT_EQ(loadedScene->FindGameObjectByName("SceneRestoreColdHeroRoot"), nullptr);

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
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerService(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerService(materialManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::ShaderManager> shaderManagerService(shaderManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureManagerService(textureManager);

    NLS::Editor::Assets::PrefabInstanceRegistry restoredRegistry;
    const auto restore = facade.RestorePrefabInstancesFromSceneDocument(
        document,
        *loadedScene,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("ac202020-2020-4020-8020-202020202020")),
        restoredRegistry,
        [&](NLS::Core::Assets::AssetId assetId, const std::string& subAssetKey) -> std::optional<NLS::Engine::Assets::PrefabArtifact>
        {
            if (assetId == prefab->assetId && subAssetKey == "prefab:SceneRestoreColdHero")
                return prefab;
            return std::nullopt;
        });

    ASSERT_EQ(restore.status, NLS::Editor::Assets::PrefabOperationStatus::Committed);
    auto* rootObject = loadedScene->FindGameObjectByName("SceneRestoreColdHeroRoot");
    ASSERT_NE(rootObject, nullptr);
    auto* meshFilter = rootObject->GetComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = rootObject->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    EXPECT_EQ(meshFilter->GetModelPath(), meshPath);
    EXPECT_NE(meshFilter->ResolveMesh(), nullptr);
    ASSERT_EQ(meshRenderer->GetMaterialPaths().size(), 1u);
    EXPECT_EQ(meshRenderer->GetMaterialPaths()[0], materialPath);
    EXPECT_NE(meshRenderer->GetMaterialAtIndex(0), nullptr);
    EXPECT_TRUE(meshManager.IsResourceRegistered(meshPath))
        << "Scene restore must synchronously prewarm mesh artifacts so loaded scenes are immediately visible.";
    EXPECT_TRUE(materialManager.IsResourceRegistered(materialPath))
        << "Scene restore must synchronously prewarm material artifacts so loaded scenes are immediately visible.";

    meshManager.UnloadResources();
    materialManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
    NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, GeneratedModelDragSynchronouslyPrewarmsLargeImportedArtifacts)
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
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerService(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerService(materialManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::ShaderManager> shaderManagerService(shaderManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureManagerService(textureManager);

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
    ASSERT_EQ(CountSceneRootGameObjects(scene), 1u);
    EXPECT_EQ(FindSceneRootGameObject(scene), result.dragDrop.instance->instanceRoot);

    EXPECT_FALSE(meshManager.GetResources().empty());
    EXPECT_FALSE(materialManager.GetResources().empty());

    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, CancelledRendererResourcePrewarmCanResumeWithoutCommittingStaleMaterial)
{
    const ScopedGameObjectAssetImportJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsAvailable());
    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");
    const ScopedShaderManagerAssetPaths shaderAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerService(materialManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::ShaderManager> shaderManagerService(shaderManager);

    const auto materialPath =
        std::string("Library/Artifacts/11/1111111111111111111111111111111111111111111111111111111111111111");
    const auto absoluteMaterialPath =
        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(materialPath);
    const std::string materialPayload =
        "shaderLabMaterialVersion=1\n"
        "shader=?\n"
        "surfaceMode=Opaque\n"
        "alphaMode=Opaque\n"
        "doubleSided=true\n"
        "depthWrite=true\n";
    NLS::Core::Assets::NativeArtifactMetadata materialMetadata;
    materialMetadata.artifactType = NLS::Core::Assets::ArtifactType::Material;
    materialMetadata.schemaName = "material";
    materialMetadata.schemaVersion = 1u;
    materialMetadata.sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("HoverMaterial"));
    materialMetadata.subAssetKey = "material:Hover";
    materialMetadata.importerId = "test-material";
    materialMetadata.importerVersion = 1u;
    WriteBinaryFile(
        absoluteMaterialPath,
        NLS::Core::Assets::WriteNativeArtifactContainer(
            std::move(materialMetadata),
            std::vector<uint8_t>(materialPayload.begin(), materialPayload.end())));

    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath, true), nullptr);
    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(materialPath));

    materialManager.CancelAsyncArtifact(materialPath);
    for (size_t attempt = 0; attempt < 64u; ++attempt)
    {
        materialManager.PumpAsyncLoads(8u);
        if (!materialManager.IsAsyncArtifactLoadPending(materialPath))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_FALSE(materialManager.IsAsyncArtifactLoadPending(materialPath));
    EXPECT_FALSE(materialManager.IsResourceRegistered(materialPath))
        << "Preview-owned material prewarm cancellation must not leave a stale runtime material registered after drag exit.";
    EXPECT_FALSE(materialManager.IsResourceRegistered(absoluteMaterialPath));

    EXPECT_EQ(materialManager.RequestAsyncArtifact(materialPath, true), nullptr);
    for (size_t attempt = 0; attempt < 64u; ++attempt)
    {
        materialManager.PumpAsyncLoads(8u);
        if (materialManager.IsResourceRegistered(materialPath) ||
            materialManager.IsResourceRegistered(absoluteMaterialPath))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(
        materialManager.IsResourceRegistered(materialPath) ||
        materialManager.IsResourceRegistered(absoluteMaterialPath))
        << "A later hover/drop must be able to resume material prewarm after the previous owner cancelled.";

    materialManager.UnloadResources();
    NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}

TEST(GameObjectAssetImportTests, CancelledRendererResourcePrewarmCanResumeWithoutCommittingStaleTexture)
{
    const ScopedGameObjectAssetImportJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsAvailable());
    EnsureGameObjectAssetImportTestDriver();

    const auto root = MakeGameObjectAssetImportRoot();
    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");

    NLS::Core::ResourceManagement::TextureManager textureManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureManagerService(textureManager);

    const auto texturePath =
        std::string("Library/Artifacts/22/2222222222222222222222222222222222222222222222222222222222222222");
    const auto absoluteTexturePath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 1u;
    artifact.height = 1u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    NLS::Render::Assets::TextureArtifactMip mip;
    mip.level = 0u;
    mip.width = 1u;
    mip.height = 1u;
    mip.rowPitch = 4u;
    mip.slicePitch = 4u;
    mip.pixels = { 255u, 128u, 64u, 255u };
    artifact.mips.push_back(std::move(mip));
    WriteBinaryFile(absoluteTexturePath, NLS::Render::Assets::SerializeTextureArtifact(artifact));

    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath, true), nullptr);
    EXPECT_TRUE(textureManager.IsAsyncArtifactLoadPending(texturePath));

    textureManager.CancelAsyncArtifact(texturePath);
    for (size_t attempt = 0; attempt < 64u; ++attempt)
    {
        textureManager.PumpAsyncLoads(8u);
        if (!textureManager.IsAsyncArtifactLoadPending(texturePath))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_FALSE(textureManager.IsAsyncArtifactLoadPending(texturePath));
    EXPECT_FALSE(textureManager.IsResourceRegistered(texturePath))
        << "Preview-owned texture prewarm cancellation must not leave a stale runtime texture registered after drag exit.";
    EXPECT_FALSE(textureManager.IsResourceRegistered(absoluteTexturePath));

    EXPECT_EQ(textureManager.RequestAsyncArtifact(texturePath, true), nullptr);
    for (size_t attempt = 0; attempt < 64u; ++attempt)
    {
        textureManager.PumpAsyncLoads(8u);
        if (textureManager.IsResourceRegistered(texturePath) ||
            textureManager.IsResourceRegistered(absoluteTexturePath))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_TRUE(
        textureManager.IsResourceRegistered(texturePath) ||
        textureManager.IsResourceRegistered(absoluteTexturePath))
        << "A later hover/drop must be able to resume texture prewarm after the previous owner cancelled.";

    textureManager.UnloadResources();
    NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    std::filesystem::remove_all(root);
}
