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
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetImporterSettings.h"
#include "Assets/AssetMeta.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/ShaderLabMaterialDefaults.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/PrefabInstanceResourceLifetime.h"
#include "Core/EditorActions.h"
#include "Core/RendererResourcePrewarmRequest.h"
#include "Core/RendererResourceStreamingBudget.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "Jobs/JobSystem.h"
#include "Engine/Assets/PrefabAsset.h"
#include "GameObject.h"
#include "Guid.h"
#include "Panels/SceneView.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Debug/DebugDrawService.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "ResourceManagement/MaterialManager.h"
#include "ResourceManagement/TextureManager.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphSerializer.h"

#ifndef NLS_HAS_ASSIMP_FBX_IMPORTER
#define NLS_HAS_ASSIMP_FBX_IMPORTER 0
#endif

#define NLS_UNREGISTERED_TEST(suite, name) static void suite##_##name##_Unregistered()
#if defined(NLS_REGISTER_LONG_RUNNING_EDITOR_ASSET_DRAG_DROP_TESTS)
#undef TEST
#define TEST(suite, name) NLS_UNREGISTERED_TEST(suite, name)
#define NLS_LONG_RUNNING_TEST(performanceSuite, name) GTEST_TEST(performanceSuite, name)
#else
#define NLS_LONG_RUNNING_TEST(performanceSuite, name) NLS_UNREGISTERED_TEST(performanceSuite, name)
#endif

namespace
{
using NLS::Core::Assets::AssetId;
using NLS::Editor::Assets::AssetDragDropWorkflow;
using NLS::Editor::Assets::DragDropOperationStatus;
using NLS::Editor::Assets::UnifiedPrefabReadiness;
using NLS::Engine::Assets::PrefabArtifact;

template <typename T>
class ScopedServiceOverride final
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
        if (m_hadPrevious && m_previous != nullptr)
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

class ScopedResourceManagerAssetPaths final
{
public:
    ScopedResourceManagerAssetPaths(std::string projectAssetsRoot, std::string engineAssetsRoot)
    {
        NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
        NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths(projectAssetsRoot, engineAssetsRoot);
    }

    ~ScopedResourceManagerAssetPaths()
    {
        NLS::Core::ResourceManagement::MaterialManager::ProvideAssetPaths({}, {});
        NLS::Core::ResourceManagement::TextureManager::ProvideAssetPaths({}, {});
    }

    ScopedResourceManagerAssetPaths(const ScopedResourceManagerAssetPaths&) = delete;
    ScopedResourceManagerAssetPaths& operator=(const ScopedResourceManagerAssetPaths&) = delete;
};

class ScopedEditorAssetDragDropJobSystem final
{
public:
    explicit ScopedEditorAssetDragDropJobSystem(const uint32_t workerCount = 0u)
    {
        if (NLS::Base::Jobs::IsJobSystemInitialized())
            return;

        NLS::Base::Jobs::JobSystemConfig config;
        config.workerCount = workerCount;
        m_ownsRuntime = NLS::Base::Jobs::TryInitializeJobSystem(config);
    }

    ~ScopedEditorAssetDragDropJobSystem()
    {
        if (!m_ownsRuntime)
            return;
        NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
    }

    bool IsAvailable() const
    {
        return NLS::Base::Jobs::IsJobSystemInitialized();
    }

    ScopedEditorAssetDragDropJobSystem(const ScopedEditorAssetDragDropJobSystem&) = delete;
    ScopedEditorAssetDragDropJobSystem& operator=(const ScopedEditorAssetDragDropJobSystem&) = delete;

private:
    bool m_ownsRuntime = false;
};

class ScopedAsyncArtifactRequestStateForTesting final
{
public:
    ScopedAsyncArtifactRequestStateForTesting()
        : m_ready(ClearAndWait())
    {
    }

    ~ScopedAsyncArtifactRequestStateForTesting()
    {
        if (!ClearAndWait())
            ADD_FAILURE() << "Timed out waiting for async artifact workers during test cleanup.";
    }

    bool IsReady() const
    {
        return m_ready;
    }

    ScopedAsyncArtifactRequestStateForTesting(const ScopedAsyncArtifactRequestStateForTesting&) = delete;
    ScopedAsyncArtifactRequestStateForTesting& operator=(const ScopedAsyncArtifactRequestStateForTesting&) = delete;

private:
    static bool ClearAndWait()
    {
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Core::ResourceManagement::MaterialManager::ClearAsyncArtifactRequestStateForTesting();
        NLS::Core::ResourceManagement::TextureManager::ClearAsyncArtifactRequestStateForTesting();
        const bool materialWorkersDrained =
            NLS::Core::ResourceManagement::MaterialManager::WaitForAsyncArtifactWorkersForTesting();
        const bool textureWorkersDrained =
            NLS::Core::ResourceManagement::TextureManager::WaitForAsyncArtifactWorkersForTesting();
        return materialWorkersDrained && textureWorkersDrained;
#else
        return true;
#endif
    }

    bool m_ready = false;
};

AssetId Id(const char* guid)
{
    return AssetId(NLS::Guid::Parse(guid));
}

std::string TestArtifactPath(std::string_view key)
{
    const auto fileName = NLS::Core::Assets::BuildArtifactStorageFileName(key);
    return (std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(fileName)).generic_string();
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

std::vector<uint8_t> ReadBinaryTestFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
}

std::filesystem::path ResolveImportedArtifactPath(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ImportedArtifact& artifact)
{
    auto artifactPath = std::filesystem::path(artifact.artifactPath);
    if (artifactPath.empty())
        return {};
    if (artifactPath.is_relative())
        artifactPath = root / artifactPath;
    return artifactPath.lexically_normal();
}

std::filesystem::path FindFirstImportedArtifactPathForSubAssetPrefix(
    NLS::Editor::Assets::AssetDatabaseFacade& database,
    const std::filesystem::path& root,
    const std::string& assetPath,
    const std::string& subAssetPrefix,
    const NLS::Core::Assets::ArtifactType artifactType)
{
    const auto manifest = database.GetArtifactManifestForAssetPath(assetPath);
    if (!manifest.has_value())
        return {};

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != artifactType)
            continue;
        if (artifact.subAssetKey.rfind(subAssetPrefix, 0u) != 0u)
            continue;
        return ResolveImportedArtifactPath(root, artifact);
    }

    return {};
}

std::filesystem::path FindFirstImportedArtifactPathForAsset(
    NLS::Editor::Assets::AssetDatabaseFacade& database,
    const std::filesystem::path& root,
    const std::string& assetPath,
    const NLS::Core::Assets::ArtifactType artifactType)
{
    const auto manifest = database.GetArtifactManifestForAssetPath(assetPath);
    if (!manifest.has_value())
        return {};

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType == artifactType)
            return ResolveImportedArtifactPath(root, artifact);
    }

    return {};
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

TEST(EditorAssetDragDropTests, GeneratedModelBlockingDropReimportsWhenRendererTextureArtifactIsMissing)
{
    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "ReadyHero.gltf",
        TexturedSingleNodeGltf("ReadyHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/ReadyHero.gltf"));
    const auto textureArtifactPath = FindFirstImportedArtifactPathForSubAssetPrefix(
        database,
        root,
        "Assets/Models/ReadyHero.gltf",
        "texture:",
        NLS::Core::Assets::ArtifactType::Texture);
    const auto importedTextureArtifactPath = textureArtifactPath.empty()
        ? FindFirstImportedArtifactPathForAsset(
            database,
            root,
            "Assets/Textures/HeroBaseColor.png",
            NLS::Core::Assets::ArtifactType::Texture)
        : textureArtifactPath;
    ASSERT_FALSE(importedTextureArtifactPath.empty());
    ASSERT_TRUE(std::filesystem::exists(importedTextureArtifactPath));
    std::filesystem::remove(importedTextureArtifactPath);
    ASSERT_FALSE(std::filesystem::exists(importedTextureArtifactPath));

    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/ReadyHero.gltf",
        "prefab:ReadyHero");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto finalDropRequest = NLS::Editor::Assets::UnifiedPrefabLoadRequest {};
    finalDropRequest.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/ReadyHero.gltf",
        "prefab:ReadyHero",
        NLS::Editor::Assets::GetEditorAssetDragPayloadAssetId(payload),
        NLS::Core::Assets::AssetType::ModelScene);
    finalDropRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::FinalDrop;
    finalDropRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    finalDropRequest.ownerScopeId = "Assets/Models/ReadyHero.gltf";
    finalDropRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;
    const auto directLoad = bridge.LoadUnifiedPrefabShared(finalDropRequest);
    ASSERT_FALSE(directLoad.prefab) << "Final-drop ready load must reject the missing renderer texture dependency.";
    EXPECT_TRUE(directLoad.rendererDependencyMissing || directLoad.pending)
        << directLoad.diagnosticCode << ": " << directLoad.diagnosticMessage;

    NLS::Editor::Assets::ImportProgressTracker progress;
    const auto result = bridge.DropImportedAssetHandleIntoHierarchyBlocking(
        payload,
        scene,
        {},
        nullptr,
        nullptr,
        &progress);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "ReadyHeroRoot");
    EXPECT_TRUE(std::filesystem::exists(importedTextureArtifactPath))
        << "Blocking final drop must restore missing renderer texture artifacts before committing.";
    EXPECT_FALSE(progress.HasRunningJobs());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, GeneratedModelPreviewDropInstantiatesGraphOnlyWhenRendererTextureArtifactIsMissing)
{
    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "PreviewMissingTextureHero.gltf",
        TexturedSingleNodeGltf("PreviewMissingTextureHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PreviewMissingTextureHero.gltf"));
    const auto textureArtifactPath = FindFirstImportedArtifactPathForSubAssetPrefix(
        database,
        root,
        "Assets/Models/PreviewMissingTextureHero.gltf",
        "texture:",
        NLS::Core::Assets::ArtifactType::Texture);
    const auto importedTextureArtifactPath = textureArtifactPath.empty()
        ? FindFirstImportedArtifactPathForAsset(
            database,
            root,
            "Assets/Textures/HeroBaseColor.png",
            NLS::Core::Assets::ArtifactType::Texture)
        : textureArtifactPath;
    ASSERT_FALSE(importedTextureArtifactPath.empty());
    ASSERT_TRUE(std::filesystem::exists(importedTextureArtifactPath));
    std::filesystem::remove(importedTextureArtifactPath);
    ASSERT_FALSE(std::filesystem::exists(importedTextureArtifactPath));

    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/PreviewMissingTextureHero.gltf",
        "prefab:PreviewMissingTextureHero");

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    auto graphOnlyRequest = NLS::Editor::Assets::UnifiedPrefabLoadRequest {};
    graphOnlyRequest.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/PreviewMissingTextureHero.gltf",
        "prefab:PreviewMissingTextureHero",
        NLS::Editor::Assets::GetEditorAssetDragPayloadAssetId(payload),
        NLS::Core::Assets::AssetType::ModelScene);
    graphOnlyRequest.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::FinalDrop;
    graphOnlyRequest.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    graphOnlyRequest.ownerScopeId = "Assets/Models/PreviewMissingTextureHero.gltf";
    graphOnlyRequest.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    graphOnlyRequest.allowPending = false;
    const auto graphOnlyLoad = bridge.LoadUnifiedPrefabShared(graphOnlyRequest);
    ASSERT_NE(graphOnlyLoad.prefab, nullptr)
        << "Scene hover preview should be able to load the prefab graph before renderer resources are ready.";
    ASSERT_TRUE(graphOnlyLoad.key.has_value());
    ASSERT_FALSE(graphOnlyLoad.key->rendererArtifactReadinessRequired);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    const auto finalDrop = bridge.TryDropImportedAssetHandleFromHotCacheIntoHierarchy(
        "Assets/Models/PreviewMissingTextureHero.gltf",
        "prefab:PreviewMissingTextureHero",
        NLS::Editor::Assets::GetEditorAssetDragPayloadAssetId(payload),
        NLS::Core::Assets::AssetType::ModelScene,
        *graphOnlyLoad.key,
        scene,
        {},
        &prefabRegistry,
        nullptr,
        nullptr);
    ASSERT_TRUE(finalDrop.handled);
    EXPECT_NE(finalDrop.dragDrop.status, DragDropOperationStatus::Committed);
    EXPECT_TRUE(HasDiagnosticCode(finalDrop.dragDrop, "dragdrop-hot-cache-key-not-renderer-ready"))
        << JoinDiagnosticCodes(finalDrop.dragDrop);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    const auto preview = bridge.TryDropImportedAssetHandleFromHotCacheIntoHierarchy(
        "Assets/Models/PreviewMissingTextureHero.gltf",
        "prefab:PreviewMissingTextureHero",
        NLS::Editor::Assets::GetEditorAssetDragPayloadAssetId(payload),
        NLS::Core::Assets::AssetType::ModelScene,
        *graphOnlyLoad.key,
        scene,
        {},
        &prefabRegistry,
        nullptr,
        nullptr,
        true);

    ASSERT_TRUE(preview.handled) << JoinDiagnosticCodes(preview.dragDrop);
    ASSERT_EQ(preview.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(preview.dragDrop);
    ASSERT_TRUE(preview.dragDrop.instance.has_value());
    ASSERT_NE(preview.dragDrop.instance->instanceRoot, nullptr);
    EXPECT_EQ(preview.dragDrop.instance->instanceRoot->GetName(), "PreviewMissingTextureHeroRoot");
    EXPECT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_TRUE(preview.dragDrop.deferredAssetReferenceResolutionRequested)
        << "Graph-only hover preview must hand renderer binding to the async resolution path.";

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
    EXPECT_TRUE(result.dragDrop.deferredAssetReferenceResolutionRequested)
        << "Generated model final drops must still queue renderer resource resolution so a prefab graph that instantiates before every mesh/material is hot gets completed after commit.";
    EXPECT_NE(result.dragDrop.sharedArtifact, nullptr);
    ASSERT_NE(result.dragDrop.rendererDependencyTemplates, nullptr);
    EXPECT_FALSE(result.dragDrop.rendererDependencyTemplates->empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, ModelTextureMappingDependencyNameSearchReusesProjectScan)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDragDropRoot();
    const auto woodPath = root / "Assets" / "Textures" / "SharedWood.png";
    const auto metalPath = root / "Assets" / "Textures" / "SharedMetal.png";
    WriteBinaryFile(woodPath, TinyPng());
    WriteBinaryFile(metalPath, TinyPng());

    auto woodMeta = AssetMeta::CreateForAsset(woodPath);
    woodMeta.assetType = AssetType::Texture;
    woodMeta.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    ASSERT_TRUE(woodMeta.Save(GetAssetMetaPath(woodPath)));

    auto metalMeta = AssetMeta::CreateForAsset(metalPath);
    metalMeta.assetType = AssetType::Texture;
    metalMeta.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    ASSERT_TRUE(metalMeta.Save(GetAssetMetaPath(metalPath)));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto fingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        root,
        {
            "project|SharedWood|name-search",
            "project|SharedMetal|name-search"
        },
        "editor");

    ASSERT_EQ(fingerprints.size(), 2u);
    EXPECT_TRUE(fingerprints[0].has_value());
    EXPECT_TRUE(fingerprints[1].has_value());
    EXPECT_LE(GetModelTextureMappingDependencyFingerprintScanCountForTesting(), 1u)
        << "Validating many PathToGuidMapping dependencies must build one project texture index, not recursively rescan Assets/ per dependency.";

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, ModelTextureMappingDependencyFallbackScanDoesNotReuseStaleFingerprint)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDragDropRoot();
    const auto woodPath = root / "Assets" / "Textures" / "SharedWood.png";
    WriteBinaryFile(woodPath, TinyPng());

    auto woodMeta = AssetMeta::CreateForAsset(woodPath);
    woodMeta.assetType = AssetType::Texture;
    woodMeta.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    ASSERT_TRUE(woodMeta.Save(GetAssetMetaPath(woodPath)));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto firstFingerprint = ComputeModelTextureMappingDependencyFingerprintForTesting(
        root,
        "project|SharedWood|name-search",
        "editor");
    ASSERT_TRUE(firstFingerprint.has_value());
    const auto scansAfterFirstLookup = GetModelTextureMappingDependencyFingerprintScanCountForTesting();

    const auto duplicatePath = root / "Assets" / "Alternate" / "SharedWood.png";
    WriteBinaryFile(duplicatePath, TinyPng());
    auto duplicateMeta = AssetMeta::CreateForAsset(duplicatePath);
    duplicateMeta.assetType = AssetType::Texture;
    duplicateMeta.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    ASSERT_TRUE(duplicateMeta.Save(GetAssetMetaPath(duplicatePath)));

    const auto updatedFingerprint = ComputeModelTextureMappingDependencyFingerprintForTesting(
        root,
        "project|SharedWood|name-search",
        "editor");
    ASSERT_TRUE(updatedFingerprint.has_value());
    EXPECT_GT(
        GetModelTextureMappingDependencyFingerprintScanCountForTesting(),
        scansAfterFirstLookup)
        << "Fallback texture scans should not store a process-global fingerprint that misses later source asset changes.";

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, ModelTextureMappingDependencyBatchUsesOneArtifactDatabaseLoad)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDragDropRoot();
    const auto library = root / "Library" / "ArtifactDB";
    std::filesystem::create_directories(library.parent_path());

    ArtifactManifest woodManifest;
    woodManifest.sourceAssetId = Id("d7010101-0101-4101-8101-010101010101");
    woodManifest.importerId = "texture";
    woodManifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    woodManifest.targetPlatform = "editor";
    woodManifest.primarySubAssetKey = "texture:SharedWood";
    woodManifest.subAssets.push_back({
        woodManifest.sourceAssetId,
        "texture:SharedWood",
        ArtifactType::Texture,
        "texture",
        "editor",
        "Library/Artifacts/wood.texture",
        "wood-hash",
        "SharedWood"
    });

    ArtifactManifest metalManifest;
    metalManifest.sourceAssetId = Id("d7020202-0202-4202-8202-020202020202");
    metalManifest.importerId = "texture";
    metalManifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    metalManifest.targetPlatform = "editor";
    metalManifest.primarySubAssetKey = "texture:SharedMetal";
    metalManifest.subAssets.push_back({
        metalManifest.sourceAssetId,
        "texture:SharedMetal",
        ArtifactType::Texture,
        "texture",
        "editor",
        "Library/Artifacts/metal.texture",
        "metal-hash",
        "SharedMetal"
    });

    ArtifactDatabase database;
    database.UpsertManifest(
        woodManifest,
        "Assets/Textures/SharedWood.png",
        ArtifactRecordStatus::UpToDate);
    database.UpsertManifest(
        metalManifest,
        "Assets/Textures/SharedMetal.png",
        ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(library));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto fingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        root,
        {
            "project|SharedWood|name-search",
            "project|SharedMetal|name-search"
        },
        "editor");

    ASSERT_EQ(fingerprints.size(), 2u);
    EXPECT_TRUE(fingerprints[0].has_value());
    EXPECT_TRUE(fingerprints[1].has_value());
    EXPECT_LE(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u)
        << "Batch validation should load ArtifactDB once and answer all texture mapping dependencies from the same index.";

    const auto secondBatch = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        root,
        {
            "project|Wood|name-search"
        },
        "editor");
    ASSERT_EQ(secondBatch.size(), 1u);
    EXPECT_TRUE(secondBatch[0].has_value());
    EXPECT_LE(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u)
        << "Repeated startup prefab validations should reuse the stamped ArtifactDB texture index across batches.";

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, ModelTextureMappingDependencyBatchSourcePathMissesUseOneArtifactDatabaseLoad)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    const auto root = MakeAssetDragDropRoot();
    const auto library = root / "Library" / "ArtifactDB";
    std::filesystem::create_directories(library.parent_path());
    ArtifactDatabase database;
    ASSERT_TRUE(database.Save(library));

    const auto woodPath = root / "Assets" / "Textures" / "BulkWood.png";
    const auto metalPath = root / "Assets" / "Textures" / "BulkMetal.png";
    WriteBinaryFile(woodPath, TinyPng());
    WriteBinaryFile(metalPath, TinyPng());

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto fingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        root,
        {
            "project|Assets/Textures/BulkWood.png|source-path",
            "project|Assets/Textures/BulkMetal.png|source-path"
        },
        "editor");

    ASSERT_EQ(fingerprints.size(), 2u);
    ASSERT_TRUE(fingerprints[0].has_value());
    ASSERT_TRUE(fingerprints[1].has_value());
    EXPECT_TRUE(fingerprints[0]->empty());
    EXPECT_TRUE(fingerprints[1]->empty());
    EXPECT_LE(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u)
        << "Batch source-path misses should reuse the already loaded ArtifactDB index instead of reloading it per texture.";

    std::filesystem::remove_all(root);
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

TEST(EditorAssetDragDropTests, SceneLoadGeneratedPrefabResolutionRevealsMeshReadyObjectsBeforeAllRendererResourcesReady)
{
    const auto options = NLS::Editor::Core::BuildSceneLoadPrefabResourceResolutionOptions();

    EXPECT_TRUE(options.hideRootUntilRendererResourcesReady)
        << "Scene-open generated/model prefab restoration starts suppressed so stale references do not flash before resource recovery begins.";
    EXPECT_FALSE(options.keepRootRenderingSuppressedOnFailure)
        << "Scene-open resource failures should mark the prefab state without keeping unrelated restored instances hidden.";
    EXPECT_TRUE(NLS::Editor::Core::ShouldRevealRendererResourceResolutionObjectBeforeAllReady(
        options.hideRootUntilRendererResourcesReady,
        options.allowProgressiveRevealBeforeAllResourcesReady))
        << "Scene-open should progressively reveal mesh-ready objects so Scene View does not stay visually empty while materials/textures finish resolving.";
    EXPECT_FALSE(NLS::Editor::Core::ShouldRevealRendererResourceResolutionObjectBeforeAllReady(true, false))
        << "Non-scene-load deferred drops can keep roots suppressed on failure, so they must not reveal objects early.";
    EXPECT_TRUE(options.shareSceneLoadFrameBudget)
        << "Scene-open prefab resolution uses a shared per-frame budget so many prefab instances cannot multiply the frame cost.";
    EXPECT_EQ(
        options.progressTargetPlatform,
        std::string(NLS::Editor::Core::kSceneLoadRendererResourceResolutionTargetPlatform));
    EXPECT_EQ(
        options.streamingBudget.frameBudget,
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().frameBudget);
    EXPECT_EQ(
        options.streamingBudget.maxInflightMeshLoads,
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().maxInflightMeshLoads);
    EXPECT_LT(
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().maxInflightMeshLoads,
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().maxSharedSceneLoadMeshLoads)
        << "Scene-open restoration keeps each prefab's mesh window smaller than the shared queue so one large prefab cannot starve later prefab instances.";
    EXPECT_GE(
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().maxSharedSceneLoadMeshLoads,
        NLS::Editor::Core::GetInteractivePrefabRendererResourceStreamingBudget().maxInflightMeshLoads)
        << "The shared scene-load mesh queue keeps aggregate throughput high while per-prefab windows provide fairness.";
    EXPECT_GT(
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().maxInflightMeshLoads,
        NLS::Editor::Core::GetInteractivePrefabRendererResourceStreamingBudget().maxInflightMeshLoads)
        << "Scene-open restoration must keep enough per-prefab mesh loads in flight to make large prefabs visible quickly.";
    EXPECT_GT(
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().meshBindsPerFrame,
        NLS::Editor::Core::GetInteractivePrefabRendererResourceStreamingBudget().meshBindsPerFrame)
        << "Scene-open restoration should reveal ready mesh renderers more aggressively than ordinary interactive drops.";
    EXPECT_GE(
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().materialPrewarmsPerFrame,
        NLS::Editor::Core::GetInteractivePrefabRendererResourceStreamingBudget().materialPrewarmsPerFrame)
        << "Scene-open restoration should finish material bindings aggressively enough to avoid long blank-scene tails.";
    EXPECT_GE(
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().materialPrewarmsPerFrame,
        16u)
        << "Scene-open restoration still needs enough material completions per frame to avoid long blank-scene tails.";
    EXPECT_LT(
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().textureCompletionsPerFrame,
        NLS::Editor::Core::GetInteractivePrefabRendererResourceStreamingBudget().textureCompletionsPerFrame)
        << "Scene-open restoration should not complete a large texture batch in one delayed-action step.";
    EXPECT_GE(
        NLS::Editor::Core::GetSceneLoadPrefabRendererResourceStreamingBudget().textureCompletionsPerFrame,
        16u)
        << "Scene-open restoration still needs enough texture completions per frame to progress renderer readiness.";
}

TEST(EditorAssetDragDropTests, RendererResourceQueueInterleavesMatchingMeshAndMaterialTasks)
{
    using namespace NLS::Editor::Core;

    const std::vector<std::string> meshSources {
        "renderer-a",
        "renderer-b",
        "renderer-c"
    };
    const std::vector<std::string> materialSources {
        "renderer-b",
        "renderer-a",
        "renderer-c",
        "unmatched"
    };

    const auto plan = PlanRendererResourceResolutionQueue(meshSources, materialSources);

    ASSERT_EQ(plan.size(), 7u);
    EXPECT_EQ(plan[0].kind, RendererResourceResolutionQueueTaskKind::Mesh);
    EXPECT_EQ(plan[0].sourceIndex, 0u);
    EXPECT_EQ(plan[1].kind, RendererResourceResolutionQueueTaskKind::Material);
    EXPECT_EQ(plan[1].sourceIndex, 1u);
    EXPECT_EQ(plan[2].kind, RendererResourceResolutionQueueTaskKind::Mesh);
    EXPECT_EQ(plan[2].sourceIndex, 1u);
    EXPECT_EQ(plan[3].kind, RendererResourceResolutionQueueTaskKind::Material);
    EXPECT_EQ(plan[3].sourceIndex, 0u);
    EXPECT_EQ(plan[4].kind, RendererResourceResolutionQueueTaskKind::Mesh);
    EXPECT_EQ(plan[4].sourceIndex, 2u);
    EXPECT_EQ(plan[5].kind, RendererResourceResolutionQueueTaskKind::Material);
    EXPECT_EQ(plan[5].sourceIndex, 2u);
    EXPECT_EQ(plan[6].kind, RendererResourceResolutionQueueTaskKind::Material);
    EXPECT_EQ(plan[6].sourceIndex, 3u);
}

TEST(EditorAssetDragDropTests, SceneLoadSharedMeshArtifactLoadsUseGlobalInflightCapacity)
{
    EXPECT_FALSE(NLS::Editor::Core::CanStartSceneLoadSharedMeshArtifactLoad(0u, 0u))
        << "A zero shared scene-load mesh budget must not create background work.";
    EXPECT_TRUE(NLS::Editor::Core::CanStartSceneLoadSharedMeshArtifactLoad(63u, 64u))
        << "Scene load can create a new unique shared mesh task while below the global cap.";
    EXPECT_FALSE(NLS::Editor::Core::CanStartSceneLoadSharedMeshArtifactLoad(64u, 64u))
        << "Scene load must not multiply per-instance mesh load windows once the global cap is full.";
    EXPECT_FALSE(NLS::Editor::Core::CanStartSceneLoadSharedMeshArtifactLoad(65u, 64u));

    EXPECT_FALSE(NLS::Editor::Core::CanEvictSceneLoadSharedMeshArtifactLoad(false))
        << "Shared scene-load mesh bookkeeping must not evict unfinished loads, or the global cap loses visibility.";
    EXPECT_TRUE(NLS::Editor::Core::CanEvictSceneLoadSharedMeshArtifactLoad(true));
}

TEST(EditorAssetDragDropTests, PrefabResourceHandoffPreservesLoadedTransientMeshesForCommit)
{
    NLS::Editor::Core::RendererResourcePrewarmRequest request;
    request.ownerToken = "prefab:handoff";
    auto load = std::make_shared<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>();
    auto transientMesh = CreateTestTransientMesh();
    {
        std::lock_guard lock(load->mutex);
        load->completed = true;
        load->accepted = true;
        load->transientMesh = transientMesh;
    }
    request.meshLoadsByPath.emplace("Library/Artifacts/db/db7ffec2d25e80c7b075bc30a992e27e5f392f809146715c3cdf514a6fba8beb", load);

    auto handoff = NLS::Editor::Core::CollectPrefabInstancePreviewResourceHandoff(std::move(request));

    ASSERT_EQ(handoff.prewarm.meshLoadsByPath.size(), 1u);
    auto handedLoad = handoff.prewarm.meshLoadsByPath.begin()->second;
    ASSERT_NE(handedLoad, nullptr);
    std::lock_guard lock(handedLoad->mutex);
    EXPECT_EQ(handedLoad->transientMesh, transientMesh)
        << "Mouse-release commit must adopt the loaded handoff mesh; clearing it here makes the formal instance invisible until a slow reload finishes.";
}

TEST(EditorAssetDragDropTests, PrefabResourceHandoffPromotesPendingMaterialTextureLoadsForCommit)
{
    const ScopedEditorAssetDragDropJobSystem jobSystem;
    ASSERT_TRUE(jobSystem.IsAvailable());

    const auto root = MakeAssetDragDropRoot();
    const auto projectAssetsRoot = (root / "Assets").string() + "/";
    const ScopedResourceManagerAssetPaths assetPaths(
        projectAssetsRoot,
        "App/Assets/Engine/");

    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    const ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialService(materialManager);
    const ScopedServiceOverride<NLS::Core::ResourceManagement::TextureManager> textureService(textureManager);
    const ScopedAsyncArtifactRequestStateForTesting asyncRequestState;
    ASSERT_TRUE(asyncRequestState.IsReady());
    const std::string materialPath =
        "Library/Artifacts/5a/5a7b08f376165e7a24bca5d6717735939d1a03d386d31f76adac4e723db8b863";
    const std::string texturePath =
        "Library/Artifacts/a5/a55ac3737664a81088f904ab71979adda6090a6fae2b5bcf829cae2cbba29c37";
    const auto absoluteMaterialPath =
        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(materialPath);
    const auto absoluteTexturePath =
        NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(texturePath);

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
    materialMetadata.sourceAssetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic("HandoffMaterial"));
    materialMetadata.subAssetKey = "material:Handoff";
    materialMetadata.importerId = "test-material";
    materialMetadata.importerVersion = 1u;
    WriteBinaryFile(
        absoluteMaterialPath,
        NLS::Core::Assets::WriteNativeArtifactContainer(
            std::move(materialMetadata),
            std::vector<uint8_t>(materialPayload.begin(), materialPayload.end())));

    NLS::Render::Assets::TextureArtifactData textureArtifact;
    textureArtifact.width = 1u;
    textureArtifact.height = 1u;
    textureArtifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureArtifact.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    NLS::Render::Assets::TextureArtifactMip mip;
    mip.level = 0u;
    mip.width = 1u;
    mip.height = 1u;
    mip.rowPitch = 4u;
    mip.slicePitch = 4u;
    mip.pixels = {255u, 255u, 255u, 255u};
    textureArtifact.mips.push_back(std::move(mip));
    WriteBinaryFile(absoluteTexturePath, NLS::Render::Assets::SerializeTextureArtifact(textureArtifact));

    materialManager.RequestAsyncArtifact(materialPath, true);
    textureManager.RequestAsyncArtifact(texturePath, true);
    ASSERT_TRUE(materialManager.IsAsyncArtifactLoadPending(materialPath));
    ASSERT_TRUE(textureManager.IsAsyncArtifactLoadPending(texturePath));

    NLS::Editor::Core::PrefabInstancePreviewResourceHandoff handoff;
    handoff.prewarm.materialLoadsByPath.insert(materialPath);
    handoff.prewarm.textureLoadsByPath.insert(texturePath);

    NLS::Editor::Core::PromotePrefabInstancePreviewResourceHandoffForCommit(handoff);

    materialManager.CancelAsyncArtifact(materialPath);
    textureManager.CancelAsyncArtifact(texturePath);

    EXPECT_TRUE(materialManager.IsAsyncArtifactLoadPending(materialPath))
        << "Mouse-release commit must promote preview material loads to scene-owned shared interest before preview cleanup can cancel them.";
    EXPECT_TRUE(textureManager.IsAsyncArtifactLoadPending(texturePath))
        << "Mouse-release commit must promote preview texture loads to scene-owned shared interest before preview cleanup can cancel them.";

    for (size_t attempt = 0; attempt < 64u; ++attempt)
    {
        materialManager.PumpAsyncLoads(8u);
        textureManager.PumpAsyncLoads(8u);
        if (!materialManager.IsAsyncArtifactLoadPending(materialPath) &&
            !textureManager.IsAsyncArtifactLoadPending(texturePath))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const bool materialStillPending = materialManager.IsAsyncArtifactLoadPending(materialPath);
    const bool textureStillPending = textureManager.IsAsyncArtifactLoadPending(texturePath);
    ASSERT_FALSE(materialStillPending)
        << "Material artifact load should complete before the test-local MaterialManager is destroyed.";
    ASSERT_FALSE(textureStillPending)
        << "Texture artifact load should complete before the test-local TextureManager is destroyed.";

    materialManager.UnloadResources();
    textureManager.UnloadResources();
    std::filesystem::remove_all(root);
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
        "Library/Artifacts/38/388753bf0a69f98f9eb55be4743739aebac51132e803ed8fa09e73277b66440b",
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/89/898b86a8b0cf97b90b30da7330a3d7895085a850c6a3ec307404486ecdc3df34",
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/d9/d93ea27c49984550717f63add7c04e8e7fa068c55968e9b66f86ee0ea2169af9",
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
        "Library/Artifacts/38/388753bf0a69f98f9eb55be4743739aebac51132e803ed8fa09e73277b66440b"), 0u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Material,
        "Library/Artifacts/89/898b86a8b0cf97b90b30da7330a3d7895085a850c6a3ec307404486ecdc3df34"), 0u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        ResourceLifetimeResourceType::Texture,
        "Library/Artifacts/d9/d93ea27c49984550717f63add7c04e8e7fa068c55968e9b66f86ee0ea2169af9"), 0u);
    EXPECT_EQ(lifetimeRegistry.CollectTrimCandidates({}).size(), 3u)
        << "After a prefab instance is deleted, instance-owned renderer resources must become trim-eligible.";
}

TEST(EditorAssetDragDropTests, PrefabDeletionCleanupKeepsSharedResourcesAliveForSiblingInstance)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const std::string meshPath = "Library/Artifacts/6a/6a99837bfd1e500c2f54239136128afebd181bf77b9be0e8c3fe04f6def269fa";
    const std::string materialPath = "Library/Artifacts/15/158ec6acd7c687274f8f1cba616f3e5314ec400719a17b0532d237a9e9694a5f";
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

TEST(EditorAssetDragDropTests, InstantiatedImportedPrefabInstancesPreserveResolvedArtifactPathsForLifetime)
{
    using NLS::Engine::Serialize::ObjectIdentifier;

    constexpr auto* meshSubAssetKey = "mesh:body";
    constexpr auto* materialSubAssetKey = "material:body";
    const auto meshArtifactPath = TestArtifactPath("mesh:b001");
    const auto materialArtifactPath = TestArtifactPath("material:b002");

    NLS::Engine::GameObject root("ResolvedLifetimeHero", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);

    const auto meshGuid = NLS::Guid::Parse("c2240513-0513-4513-8513-051305130513");
    const auto materialGuid = NLS::Guid::Parse("c2240523-0523-4523-8523-052305230523");
    const auto meshReferenceId = NLS::Engine::Serialize::AssetId(meshGuid);
    const auto materialReferenceId = NLS::Engine::Serialize::AssetId(materialGuid);
    const auto meshAssetId = NLS::Core::Assets::AssetId(meshGuid);
    const auto materialAssetId = NLS::Core::Assets::AssetId(materialGuid);
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(
        ObjectIdentifier::Asset(
            meshReferenceId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshReferenceId.GetGuid(), meshSubAssetKey),
            meshSubAssetKey)));
    meshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(
            ObjectIdentifier::Asset(
                materialReferenceId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(materialReferenceId.GetGuid(), materialSubAssetKey),
                materialSubAssetKey))
    });

    auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        Id("c2250513-0513-4513-8513-051305130513"),
        "Assets/Prefabs/ResolvedLifetimeHero.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(created.artifact.has_value());
    auto prefab = *created.artifact;
    prefab.generatedModelPrefab = true;
    prefab.resolvedAssets = {
        {meshAssetId, "Mesh", meshSubAssetKey, meshArtifactPath},
        {materialAssetId, "Material", materialSubAssetKey, materialArtifactPath}
    };

    NLS::Engine::SceneSystem::Scene scene;
    auto instantiate = NLS::Editor::Assets::PrefabEditorWorkflow().InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:ResolvedLifetimeHero",
        {},
        true
    }, scene);

    ASSERT_EQ(instantiate.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(instantiate.instance.has_value());
    ASSERT_EQ(instantiate.instance->preservedResolvedAssets.size(), 2u);
    EXPECT_EQ(instantiate.instance->preservedResolvedAssets[0].artifactPath, meshArtifactPath);
    EXPECT_EQ(instantiate.instance->preservedResolvedAssets[1].artifactPath, materialArtifactPath);

    NLS::Core::ResourceManagement::ResourceLifetimeRegistry lifetimeRegistry;
    ASSERT_TRUE(NLS::Editor::Core::AcquirePrefabResolvedAssetResourceOwners(
        lifetimeRegistry,
        "scene-prefab:test-resolved-lifetime",
        instantiate.instance->preservedResolvedAssets));
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh,
        meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material,
        materialArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh,
        meshSubAssetKey), 0u)
        << "Prefab instance lifetime must protect the real artifact path, not the source sub-asset key.";
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(
        NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material,
        materialSubAssetKey), 0u);
}

TEST(EditorAssetDragDropTests, PrefabResolvedAssetOwnerAcquireDeduplicatesPreservedArtifactPaths)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const auto meshArtifactPath = TestArtifactPath("mesh:b011");
    const auto materialArtifactPath = TestArtifactPath("material:b012");
    const auto meshAssetId = Id("c2240b13-0b13-4b13-8b13-0b130b130b13");
    const auto materialAssetId = Id("c2240b23-0b23-4b23-8b23-0b230b230b23");

    const std::vector<NLS::Engine::Assets::PrefabResolvedAsset> resolvedAssets = {
        {meshAssetId, "Mesh", "mesh:duplicate-preserved-body", meshArtifactPath},
        {meshAssetId, "Mesh", "mesh:duplicate-preserved-body", meshArtifactPath},
        {materialAssetId, "Material", "material:duplicate-preserved-body", materialArtifactPath},
        {materialAssetId, "Material", "material:duplicate-preserved-body", materialArtifactPath}
    };

    ResourceLifetimeRegistry lifetimeRegistry;
    ASSERT_TRUE(NLS::Editor::Core::AcquirePrefabResolvedAssetResourceOwners(
        lifetimeRegistry,
        "scene-prefab:duplicate-preserved",
        resolvedAssets));

    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialArtifactPath), 1u);
}

TEST(EditorAssetDragDropTests, TrimRefreshKeepsSiblingImportedPrefabArtifactOwnersAfterDeletingTwin)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;
    using NLS::Engine::Serialize::ObjectIdentifier;

    constexpr auto* meshSubAssetKey = "mesh:shared-body";
    constexpr auto* materialSubAssetKey = "material:shared-body";
    const auto meshArtifactPath = TestArtifactPath("mesh:b021");
    const auto materialArtifactPath = TestArtifactPath("material:b022");

    NLS::Engine::GameObject root("SharedResolvedLifetimeHero", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);

    const auto meshGuid = NLS::Guid::Parse("c2240613-0613-4613-8613-061306130613");
    const auto materialGuid = NLS::Guid::Parse("c2240623-0623-4623-8623-062306230623");
    const auto meshReferenceId = NLS::Engine::Serialize::AssetId(meshGuid);
    const auto materialReferenceId = NLS::Engine::Serialize::AssetId(materialGuid);
    meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(
        ObjectIdentifier::Asset(
            meshReferenceId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(meshReferenceId.GetGuid(), meshSubAssetKey),
            meshSubAssetKey)));
    meshRenderer->SetMaterialReferences({
        MakePPtr<NLS::Render::Resources::Material>(
            ObjectIdentifier::Asset(
                materialReferenceId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(materialReferenceId.GetGuid(), materialSubAssetKey),
                materialSubAssetKey))
    });

    auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &root,
        {},
        Id("c2250613-0613-4613-8613-061306130613"),
        "Assets/Prefabs/SharedResolvedLifetimeHero.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);
    ASSERT_TRUE(created.artifact.has_value());
    auto prefab = *created.artifact;
    prefab.generatedModelPrefab = true;
    prefab.resolvedAssets = {
        {NLS::Core::Assets::AssetId(meshGuid), "Mesh", meshSubAssetKey, meshArtifactPath},
        {NLS::Core::Assets::AssetId(materialGuid), "Material", materialSubAssetKey, materialArtifactPath}
    };

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry prefabRegistry;
    auto first = NLS::Editor::Assets::PrefabEditorWorkflow().InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:SharedResolvedLifetimeHero",
        {},
        true
    }, scene);
    auto second = NLS::Editor::Assets::PrefabEditorWorkflow().InstantiatePrefab({
        &prefab,
        prefab.assetId,
        "prefab:SharedResolvedLifetimeHero",
        {},
        true
    }, scene);
    ASSERT_TRUE(first.instance.has_value());
    ASSERT_TRUE(second.instance.has_value());
    auto& firstRegistered = prefabRegistry.Register(std::move(*first.instance));
    auto& secondRegistered = prefabRegistry.Register(std::move(*second.instance));
    const auto firstOwnerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(firstRegistered);
    const auto secondOwnerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(secondRegistered);
    ASSERT_NE(firstOwnerToken, secondOwnerToken);
    auto* deletedRoot = firstRegistered.instanceRoot;
    auto* siblingRoot = secondRegistered.instanceRoot;
    ASSERT_NE(deletedRoot, nullptr);
    ASSERT_NE(siblingRoot, nullptr);

    ResourceLifetimeRegistry lifetimeRegistry;
    ASSERT_TRUE(NLS::Editor::Core::AcquirePrefabResolvedAssetResourceOwners(
        lifetimeRegistry,
        firstOwnerToken,
        firstRegistered.preservedResolvedAssets));
    ASSERT_TRUE(NLS::Editor::Core::AcquirePrefabResolvedAssetResourceOwners(
        lifetimeRegistry,
        secondOwnerToken,
        secondRegistered.preservedResolvedAssets));

    const auto cleanup = NLS::Editor::Core::CleanupPrefabInstanceMarkedDestroy(
        prefabRegistry,
        lifetimeRegistry,
        *deletedRoot);
    ASSERT_TRUE(cleanup.removedRootInstance);

    lifetimeRegistry.ReleaseOwner(secondOwnerToken);
    auto* liveSibling = prefabRegistry.FindRootInstance(*siblingRoot);
    ASSERT_NE(liveSibling, nullptr);
    ASSERT_TRUE(NLS::Editor::Core::AcquirePrefabResolvedAssetResourceOwners(
        lifetimeRegistry,
        secondOwnerToken,
        liveSibling->preservedResolvedAssets));

    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshSubAssetKey), 0u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialSubAssetKey), 0u);
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty())
        << "Trim refresh after deleting one prefab twin must keep the sibling's real artifact resources owned.";
}

TEST(EditorAssetDragDropTests, TrimRefreshDoesNotReleaseLegacyPrefabOwnerWithoutResolvedArtifactPaths)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;

    constexpr auto* meshSubAssetKey = "mesh:legacy-body";
    constexpr auto* materialSubAssetKey = "material:legacy-body";
    const auto meshArtifactPath = TestArtifactPath("mesh:b031");
    const auto materialArtifactPath = TestArtifactPath("material:b032");

    NLS::Engine::GameObject root("LegacyResolvedLifetimeHero", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetModelPathHint(meshSubAssetKey);
    meshRenderer->SetMaterialPathHints({materialSubAssetKey});

    NLS::Editor::Assets::PrefabInstanceRecord legacyInstance;
    legacyInstance.instanceRoot = &root;
    legacyInstance.preservedResolvedAssets.clear();

    ResourceLifetimeRegistry lifetimeRegistry;
    constexpr auto* ownerToken = "scene-prefab:legacy";
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Mesh,
        meshArtifactPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Material,
        materialArtifactPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});

    EXPECT_FALSE(NLS::Editor::Core::RebuildPrefabInstancePreservedResourceOwnersForTrim(
        lifetimeRegistry,
        ownerToken,
        legacyInstance));
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshSubAssetKey), 0u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialSubAssetKey), 0u);
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty())
        << "Legacy instances without authoritative prefab resolved assets must not lose their existing real artifact owners during trim refresh.";
}

TEST(EditorAssetDragDropTests, TrimRefreshRejectsSubAssetKeysAsAuthoritativeResolvedArtifactOwners)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;

    constexpr auto* meshSubAssetKey = "mesh:stale-body";
    constexpr auto* materialSubAssetKey = "material:stale-body";
    const auto meshArtifactPath = TestArtifactPath("mesh:b041");
    const auto materialArtifactPath = TestArtifactPath("material:b042");

    NLS::Editor::Assets::PrefabInstanceRecord staleInstance;
    staleInstance.preservedResolvedAssets = {
        {
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("c2240713-0713-4713-8713-071307130713")),
            "Mesh",
            meshSubAssetKey,
            meshSubAssetKey
        },
        {
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("c2240723-0723-4723-8723-072307230723")),
            "Material",
            materialSubAssetKey,
            materialSubAssetKey
        }
    };

    ResourceLifetimeRegistry lifetimeRegistry;
    constexpr auto* ownerToken = "scene-prefab:stale";
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Mesh,
        meshArtifactPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Material,
        materialArtifactPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});

    EXPECT_FALSE(NLS::Editor::Core::RebuildPrefabInstancePreservedResourceOwnersForTrim(
        lifetimeRegistry,
        ownerToken,
        staleInstance));
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshSubAssetKey), 0u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialSubAssetKey), 0u);
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty());
}

TEST(EditorAssetDragDropTests, ImportedResourceTrimDefersWhenGeneratedPrefabLacksAuthoritativeResolvedOwners)
{
    NLS::Editor::Assets::PrefabInstanceRegistry registry;

    NLS::Engine::GameObject generatedRoot("GeneratedMissingResolvedOwners", "Prefab");
    NLS::Editor::Assets::PrefabInstanceRecord generatedInstance;
    generatedInstance.generatedReadOnly = true;
    generatedInstance.instanceRoot = &generatedRoot;
    generatedInstance.preservedResolvedAssets = {
        {
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("c2240813-0813-4813-8813-081308130813")),
            "Mesh",
            "mesh:defer-body",
            "mesh:defer-body"
        }
    };
    registry.Register(std::move(generatedInstance));

    EXPECT_TRUE(NLS::Editor::Core::ShouldDeferImportedResourceTrimForPrefabInstances(registry))
        << "Generated/imported prefab instances without authoritative artifact paths must block trim to avoid unloading visible siblings.";

    registry.Clear();
    NLS::Engine::GameObject normalRoot("NormalPrefab", "Prefab");
    NLS::Editor::Assets::PrefabInstanceRecord normalInstance;
    normalInstance.generatedReadOnly = false;
    normalInstance.instanceRoot = &normalRoot;
    registry.Register(std::move(normalInstance));

    EXPECT_FALSE(NLS::Editor::Core::ShouldDeferImportedResourceTrimForPrefabInstances(registry))
        << "Non-generated prefab instances should not globally block imported renderer resource trimming.";
}

TEST(EditorAssetDragDropTests, TrimRefreshRecoversGeneratedPrefabOwnersFromLiveArtifactPathsBeforeDefer)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const auto meshArtifactPath = TestArtifactPath("mesh:b051");
    const auto materialArtifactPath = TestArtifactPath("material:b052");

    NLS::Engine::GameObject root("LiveRecoveredPrefab", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetModelPathHint(meshArtifactPath);
    meshRenderer->SetMaterialPathHints({materialArtifactPath});

    NLS::Editor::Assets::PrefabInstanceRecord generatedInstance;
    generatedInstance.generatedReadOnly = true;
    generatedInstance.instanceRoot = &root;
    generatedInstance.preservedResolvedAssets = {
        {
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("c2240913-0913-4913-8913-091309130913")),
            "Mesh",
            "mesh:live-recovered-body",
            "mesh:live-recovered-body"
        }
    };

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto& registered = registry.Register(std::move(generatedInstance));
    const auto ownerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(registered);
    ASSERT_FALSE(ownerToken.empty());

    ResourceLifetimeRegistry lifetimeRegistry;
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Mesh,
        meshArtifactPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});

    const auto refresh = NLS::Editor::Core::RefreshPrefabInstanceResourceOwnersForTrim(
        lifetimeRegistry,
        registry);

    EXPECT_EQ(refresh.rebuiltOwnerCount, 1u);
    EXPECT_FALSE(refresh.hasDeferredGeneratedInstances)
        << "A generated prefab with stale preserved metadata but live artifact blob renderer paths is safe to trim after owner recovery.";
    EXPECT_FALSE(NLS::Editor::Core::ShouldDeferImportedResourceTrimForPrefabInstances(registry));
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialArtifactPath), 1u);
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty())
        << "Deleting a twin must not leave the live sibling's recovered artifact paths trim-eligible.";
}

TEST(EditorAssetDragDropTests, TrimRefreshMergesPreservedAndLiveArtifactOwnersForPartiallyResolvedPrefab)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const auto meshArtifactPath = TestArtifactPath("mesh:b061");
    const auto materialArtifactPath = TestArtifactPath("material:b062");

    NLS::Engine::GameObject root("PartialPreservedPrefab", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetModelPathHint(meshArtifactPath);
    meshRenderer->SetMaterialPathHints({materialArtifactPath});

    NLS::Editor::Assets::PrefabInstanceRecord generatedInstance;
    generatedInstance.generatedReadOnly = true;
    generatedInstance.instanceRoot = &root;
    generatedInstance.preservedResolvedAssets = {
        {
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("c2240a13-0a13-4a13-8a13-0a130a130a13")),
            "Mesh",
            "mesh:partial-preserved-body",
            meshArtifactPath
        }
    };

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto& registered = registry.Register(std::move(generatedInstance));
    const auto ownerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(registered);
    ASSERT_FALSE(ownerToken.empty());

    ResourceLifetimeRegistry lifetimeRegistry;
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Mesh,
        meshArtifactPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Material,
        materialArtifactPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});

    const auto refresh = NLS::Editor::Core::RefreshPrefabInstanceResourceOwnersForTrim(
        lifetimeRegistry,
        registry);

    EXPECT_EQ(refresh.rebuiltOwnerCount, 1u);
    EXPECT_FALSE(refresh.hasDeferredGeneratedInstances);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialArtifactPath), 1u)
        << "Partial preserved metadata must merge live renderer artifact paths instead of dropping material owners.";
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty());
}

TEST(EditorAssetDragDropTests, TrimRefreshDefersAndPreservesOldOwnersWhenGeneratedPrefabIsPartiallyLoaded)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const auto meshArtifactPath = TestArtifactPath("mesh:b071");
    const auto materialArtifactPath = TestArtifactPath("material:b072");

    NLS::Engine::GameObject root("PartialLivePrefab", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetModelPathHint(meshArtifactPath);
    meshRenderer->SetMaterialPathHints({"material:partial-live-body"});

    NLS::Editor::Assets::PrefabInstanceRecord generatedInstance;
    generatedInstance.generatedReadOnly = true;
    generatedInstance.instanceRoot = &root;
    generatedInstance.preservedResolvedAssets.clear();

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto& registered = registry.Register(std::move(generatedInstance));
    const auto ownerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(registered);
    ASSERT_FALSE(ownerToken.empty());

    ResourceLifetimeRegistry lifetimeRegistry;
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Mesh,
        meshArtifactPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Material,
        materialArtifactPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});

    const auto refresh = NLS::Editor::Core::RefreshPrefabInstanceResourceOwnersForTrim(
        lifetimeRegistry,
        registry);

    EXPECT_EQ(refresh.rebuiltOwnerCount, 0u);
    EXPECT_TRUE(refresh.hasDeferredGeneratedInstances)
        << "A half-loaded generated prefab must keep old owners and block trim until all renderer dependency paths are authoritative artifacts.";
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, materialArtifactPath), 1u);
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty());
}

TEST(EditorAssetDragDropTests, TrimRefreshDefersWhenGeneratedPrefabHasUnresolvedSameTypeDependency)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const auto meshArtifactPath = TestArtifactPath("mesh:b081");
    const auto firstMaterialArtifactPath = TestArtifactPath("material:b082");
    const auto secondMaterialArtifactPath = TestArtifactPath("material:b083");
    constexpr auto* secondMaterialSubAssetKey = "material:same-type-partial-b";

    NLS::Engine::GameObject root("SameTypePartialPrefab", "Prefab");
    auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshFilter, nullptr);
    ASSERT_NE(meshRenderer, nullptr);
    meshFilter->SetModelPathHint(meshArtifactPath);
    meshRenderer->SetMaterialPathHints({firstMaterialArtifactPath, secondMaterialSubAssetKey});

    NLS::Editor::Assets::PrefabInstanceRecord generatedInstance;
    generatedInstance.generatedReadOnly = true;
    generatedInstance.instanceRoot = &root;
    generatedInstance.preservedResolvedAssets = {
        {
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("c2240c13-0c13-4c13-8c13-0c130c130c13")),
            "Mesh",
            "mesh:same-type-partial-body",
            meshArtifactPath
        },
        {
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("c2240c23-0c23-4c23-8c23-0c230c230c23")),
            "Material",
            "material:same-type-partial-a",
            firstMaterialArtifactPath
        }
    };

    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    auto& registered = registry.Register(std::move(generatedInstance));
    const auto ownerToken = NLS::Editor::Core::BuildPrefabInstanceResourceOwnerToken(registered);
    ASSERT_FALSE(ownerToken.empty());

    ResourceLifetimeRegistry lifetimeRegistry;
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Mesh,
        meshArtifactPath,
        4096u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Material,
        firstMaterialArtifactPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});
    lifetimeRegistry.Acquire({
        ownerToken,
        ResourceLifetimeResourceType::Material,
        secondMaterialArtifactPath,
        1024u,
        ResourceLifetimeOwnerKind::SceneInstance});

    const auto refresh = NLS::Editor::Core::RefreshPrefabInstanceResourceOwnersForTrim(
        lifetimeRegistry,
        registry);

    EXPECT_EQ(refresh.rebuiltOwnerCount, 0u);
    EXPECT_TRUE(refresh.hasDeferredGeneratedInstances)
        << "One resolved material slot does not make another same-type unresolved material dependency safe to trim.";
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Mesh, meshArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, firstMaterialArtifactPath), 1u);
    EXPECT_EQ(lifetimeRegistry.GetActiveOwnerCount(ResourceLifetimeResourceType::Material, secondMaterialArtifactPath), 1u);
    EXPECT_TRUE(lifetimeRegistry.CollectTrimCandidates({}).empty());
}

TEST(EditorAssetDragDropTests, DeletingOneOfTwoSamePrefabInstancesLeavesSiblingVisibleAndRegistered)
{
    using NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;
    using NLS::Core::ResourceManagement::ResourceLifetimeRegistry;
    using NLS::Core::ResourceManagement::ResourceLifetimeResourceType;

    const std::string meshPath = "Library/Artifacts/bb/bb7619ad05298b72a931c2828511fb3fbba497990f02559a28f52c379dc20c5b";
    const std::string materialPath = "Library/Artifacts/84/847baee87a75f0b1b23025b44c9a393fd44ded045f88472bcace811a34feebca";
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

    const std::string rootMeshPath = "Library/Artifacts/75/753ca2cb82c807e2ed9f0edfa5ea8570d00fdfc7275803db1342d010f36fd08a";
    const std::string childMeshPath = "Library/Artifacts/11/111f421cbcd0db7c2ff80deddeee408ea3a7086c913d7a727197a7a9da3de46e";
    const std::string rootMaterialPath = "Library/Artifacts/69/69046cc23a038e167a4bda318a961a5c9f999874c2d81981497412730f4edb9a";
    const std::string childMaterialPath = "Library/Artifacts/d1/d16e4d13474653d357eee504224b1ed3741e4903f0f83c06723f6b8bfcc17d8e";

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
    const std::string meshPath = "Library/Artifacts/61/615163ab16dcb343d84c732a81af22bfa1eb723730fc6e33ceb9e9b0d03c7ae4";
    const std::string materialPath = "Library/Artifacts/fb/fb41ae9d32fa428ecd683bd8d7fe37596daac1cb232ae5814da99c65ca29aa5e";
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
        "Library/Artifacts/f4/f4ecd92dfc33c7355c091d5b62052282e4a247a586159afdabf06bd39da2036b",
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
        "Library/Artifacts/f4/f4ecd92dfc33c7355c091d5b62052282e4a247a586159afdabf06bd39da2036b"), 0u);
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
    EXPECT_GE(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::DependencyScan), 1u)
        << "Generated model final-drop should validate renderer dependency metadata without reloading the prefab graph.";
    EXPECT_EQ(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy), 0u)
        << "Final-drop readiness must not fully deserialize mesh/material/texture artifacts on mouse release.";
    EXPECT_EQ(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::CpuDeserialize), 0u)
        << "Final-drop readiness must stay metadata/header based so large prefabs do not reload on release.";
    EXPECT_EQ(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::CacheMiss), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u);
    EXPECT_EQ(scene.GetGameObjects().size(), 2u);

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, SceneViewHotCacheKeyPollingDoesNotTouchColdArtifactPath)
{
    const auto root = MakeAssetDragDropRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "SceneViewPollHero.gltf",
        TexturedSingleNodeGltf("SceneViewPollHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/SceneViewPollHero.gltf"));
    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/SceneViewPollHero.gltf",
        "prefab:SceneViewPollHero");

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    ASSERT_TRUE(bridge.PreloadImportedAssetHandlePrefabHotCache(payload));

    const auto assetId = NLS::Editor::Assets::GetEditorAssetDragPayloadAssetId(payload);
    const auto firstKey = bridge.TryFindImportedPrefabHotCacheKey(
        "Assets/Models/SceneViewPollHero.gltf",
        "prefab:SceneViewPollHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene,
        UnifiedPrefabReadiness::MeshMaterialTextureReady);
    ASSERT_TRUE(firstKey.has_value());
    ASSERT_TRUE(firstKey->rendererArtifactReadinessRequired);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    for (size_t frame = 0u; frame < 60u; ++frame)
    {
        const auto polledKey = bridge.TryFindImportedPrefabHotCacheKey(
            "Assets/Models/SceneViewPollHero.gltf",
            "prefab:SceneViewPollHero",
            assetId,
            NLS::Core::Assets::AssetType::ModelScene,
            UnifiedPrefabReadiness::MeshMaterialTextureReady);
        ASSERT_TRUE(polledKey.has_value()) << frame;
        EXPECT_EQ(polledKey->runtimeCacheIdentity, firstKey->runtimeCacheIdentity) << frame;
        EXPECT_EQ(
            polledKey->rendererArtifactReadinessRequired,
            firstKey->rendererArtifactReadinessRequired) << frame;
    }

    ExpectNoColdPrefabArtifactLoad(NLS::Core::Assets::SnapshotArtifactLoadTelemetry());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, SceneViewPrefabDragProxyDescriptorTracksPlacementBeforeRootExists)
{
    const NLS::Maths::Vector3 firstPlacement { 1.0f, 0.0f, 2.0f };
    const auto firstDescriptor = NLS::Editor::Panels::BuildSceneViewPrefabDragProxyDescriptor(
        firstPlacement,
        true,
        nullptr);

    ASSERT_TRUE(firstDescriptor.has_value())
        << "Scene View must have an immediate lightweight visual while the real prefab root is not ready.";
    EXPECT_EQ(firstDescriptor->position, firstPlacement);

    const NLS::Maths::Vector3 secondPlacement { -3.0f, 0.0f, 5.0f };
    const auto secondDescriptor = NLS::Editor::Panels::BuildSceneViewPrefabDragProxyDescriptor(
        secondPlacement,
        true,
        nullptr);

    ASSERT_TRUE(secondDescriptor.has_value());
    EXPECT_EQ(secondDescriptor->position, secondPlacement)
        << "The lightweight drag proxy must follow the latest mouse-resolved placement.";
    EXPECT_FALSE(NLS::Editor::Panels::BuildSceneViewPrefabDragProxyDescriptor(
        secondPlacement,
        true,
        reinterpret_cast<NLS::Engine::GameObject*>(static_cast<uintptr_t>(0x1))).has_value())
        << "Once a real prefab root exists the debug proxy must stop drawing to avoid double previews.";
    EXPECT_TRUE(NLS::Editor::Panels::BuildSceneViewPrefabDragProxyDescriptor(
        secondPlacement,
        true,
        reinterpret_cast<NLS::Engine::GameObject*>(static_cast<uintptr_t>(0x1)),
        false).has_value())
        << "If the real root is still hidden while renderer resources resolve, the lightweight proxy must remain visible.";
    EXPECT_FALSE(NLS::Editor::Panels::BuildSceneViewPrefabDragProxyDescriptor(
        secondPlacement,
        false,
        nullptr).has_value())
        << "Stale placements must not draw after the drag payload has been cleared.";
}

TEST(EditorAssetDragDropTests, SceneViewDisablesTrustedRenderRevisionFastPathDuringPrefabDrag)
{
    EXPECT_TRUE(NLS::Editor::Panels::ShouldTrustSceneViewRenderContentRevision(false, false));
    EXPECT_FALSE(NLS::Editor::Panels::ShouldTrustSceneViewRenderContentRevision(true, false))
        << "Transform-only preview movement must be synchronized while the prefab follows the cursor.";
    EXPECT_FALSE(NLS::Editor::Panels::ShouldTrustSceneViewRenderContentRevision(false, true))
        << "A cold prefab drop must keep synchronizing until its final preview transform is committed.";
    EXPECT_FALSE(NLS::Editor::Panels::ShouldTrustSceneViewRenderContentRevision(true, true));
}

TEST(EditorAssetDragDropTests, DebugSceneRendererSubmitsPrefabDragProxyPrimitives)
{
    NLS::Render::Debug::DebugDrawService debugDrawService;
    NLS::Editor::Rendering::DebugSceneRenderer::PrefabDragProxyDescriptor descriptor;
    descriptor.position = { 2.0f, 0.0f, -4.0f };
    descriptor.size = 1.5f;

    const auto submitted = NLS::Editor::Rendering::SubmitPrefabDragProxyDebugPrimitives(
        debugDrawService,
        descriptor);

    EXPECT_GT(submitted, 0u)
        << "The descriptor must be converted into frame debug primitives so the proxy is actually visible.";
    EXPECT_GT(debugDrawService.GetQueuedPrimitiveCount(), 0u);
}

TEST(EditorAssetDragDropTests, DebugSceneRendererPrefabDragProxyIgnoresDisabledDebugDrawSettings)
{
    NLS::Render::Debug::DebugDrawService debugDrawService;
    debugDrawService.SetEnabled(false);
    NLS::Editor::Rendering::DebugSceneRenderer::PrefabDragProxyDescriptor descriptor;
    descriptor.position = { 0.0f, 0.0f, 0.0f };

    const auto submitted = NLS::Editor::Rendering::SubmitPrefabDragProxyDebugPrimitives(
        debugDrawService,
        descriptor);

    EXPECT_GT(submitted, 0u);
    EXPECT_FALSE(debugDrawService.CollectVisiblePrimitives().empty())
        << "Prefab drag feedback is editor interaction feedback, not an optional debug overlay.";
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
    const auto fbxMetaPath = NLS::Core::Assets::GetAssetMetaPath(
        root / "Assets" / "Models" / "RepeatedDropCube.fbx");
    auto fbxMeta = NLS::Core::Assets::AssetMeta::Load(fbxMetaPath);
    ASSERT_TRUE(fbxMeta.has_value());
    fbxMeta->settings["MODEL_FBX_READER"] =
        NLS::Editor::Assets::FbxReaderSelectionToImporterSettingString(
            NLS::Editor::Assets::FbxReaderSelection::Assimp);
    ASSERT_TRUE(fbxMeta->Save(fbxMetaPath));
    ASSERT_TRUE(database.Refresh());
    const auto refreshedFbxMeta = NLS::Core::Assets::AssetMeta::Load(fbxMetaPath);
    ASSERT_TRUE(refreshedFbxMeta.has_value());
    EXPECT_EQ(
        NLS::Editor::Assets::ModelImporterSettingsFromSerialized(refreshedFbxMeta->settings).fbxReaderSelection,
        NLS::Editor::Assets::FbxReaderSelection::Assimp);
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
    EXPECT_GE(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::DependencyScan), 1u)
        << "Imported FBX final-drop must synchronously validate renderer dependencies even when the prefab graph is hot.";
    EXPECT_EQ(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::CacheMiss), 0u);
    EXPECT_EQ(CountArtifactTelemetryStage(repeatedRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u);
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

NLS_LONG_RUNNING_TEST(EditorAssetDragDropIntegrationPerformanceTests, GeneratedModelBlockingDropReimportsWhenPrefabArtifactIsMissing)
{
    const auto root = MakeAssetDragDropRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "MissingPrefabArtifactHero.gltf",
        TexturedSingleNodeGltf("MissingPrefabArtifactHeroRoot"));
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/MissingPrefabArtifactHero.gltf"));
    const auto prefabArtifactPath = FindFirstImportedArtifactPathForSubAssetPrefix(
        database,
        root,
        "Assets/Models/MissingPrefabArtifactHero.gltf",
        "prefab:",
        NLS::Core::Assets::ArtifactType::Prefab);
    ASSERT_FALSE(prefabArtifactPath.empty());
    ASSERT_TRUE(std::filesystem::exists(prefabArtifactPath));
    std::filesystem::remove(prefabArtifactPath);
    ASSERT_FALSE(std::filesystem::exists(prefabArtifactPath));

    const auto payload = MakeImportedGeneratedModelPayload(
        database,
        "Assets/Models/MissingPrefabArtifactHero.gltf",
        "prefab:MissingPrefabArtifactHero");

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::ImportProgressTracker tracker;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchyBlocking(
        payload,
        scene,
        {},
        nullptr,
        nullptr,
        &tracker);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    EXPECT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed)
        << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects().front()->GetName(), "MissingPrefabArtifactHeroRoot");
    EXPECT_TRUE(std::filesystem::exists(prefabArtifactPath));
    EXPECT_FALSE(tracker.HasRunningJobs());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, GeneratedModelDropRejectsWhenRendererMeshArtifactIsInvalid)
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
    EXPECT_NE(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    EXPECT_TRUE(HasDiagnosticCode(result.dragDrop, "dragdrop-renderer-dependency-missing"))
        << JoinDiagnosticCodes(result.dragDrop);
    EXPECT_TRUE(scene.GetGameObjects().empty());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, GeneratedModelDropRejectsWhenRendererMaterialArtifactIsInvalid)
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
    EXPECT_NE(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    EXPECT_TRUE(HasDiagnosticCode(result.dragDrop, "dragdrop-renderer-dependency-missing"))
        << JoinDiagnosticCodes(result.dragDrop);
    EXPECT_TRUE(scene.GetGameObjects().empty());

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
    const auto materialArtifact = database.LoadMainAssetAtPath("Assets/Materials/Albedo.mat");
    ASSERT_TRUE(materialArtifact.has_value());
    EXPECT_NE(
        materialArtifact->assetId,
        NLS::Core::Assets::AssetId {});
    const auto materialPath = root / "Assets" / "Materials" / "Albedo.mat";
    const auto materialBytes = ReadBinaryTestFile(materialPath);
    const auto materialContainer = NLS::Core::Assets::ReadNativeArtifactContainer(
        materialBytes,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    ASSERT_TRUE(materialContainer.has_value());
    const std::string materialPayload(materialContainer->payload.begin(), materialContainer->payload.end());
    EXPECT_NE(
        materialPayload.find(
            std::string("shader=") + NLS::Editor::Assets::kDefaultShaderLabMaterialShaderPath + "\n"),
        std::string::npos);
    EXPECT_EQ(materialPayload.find("shader=?"), std::string::npos);
    EXPECT_NE(
        materialPayload.find("property _BaseMap Texture2D " + textureId.ToString() + "#texture:Albedo"),
        std::string::npos);
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

TEST(EditorAssetDragDropTests, ImportedPrefabHandleDropPublishesBlockingLoadProgress)
{
    const auto root = MakeAssetDragDropRoot();

    NLS::Engine::GameObject gameObject("Progress Cube", "Prefab");
    const auto prefabId = Id("cb030303-0303-4303-8303-030303030303");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Progress Cube.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CreateTextAsset(
        created.prefabSourceText,
        "Assets/Progress Cube.prefab",
        prefabId));

    NLS::Editor::Assets::AssetPreimportScheduler scheduler;
    NLS::Editor::Assets::ImportProgressTracker importTracker;
    ASSERT_TRUE(scheduler.Run(database, importTracker, {
        NLS::Editor::Assets::AssetPreimportReason::FileWatcherChanged,
        {root / "Assets" / "Progress Cube.prefab"}
    }));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Progress Cube.prefab",
        prefabId,
        "prefab:Progress Cube",
        NLS::Core::Assets::ArtifactType::Prefab,
        false,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    NLS::Editor::Assets::ImportProgressTracker dropTracker;
    const auto result = bridge.DropImportedAssetHandleIntoHierarchy(
        payload,
        scene,
        Id("cb040404-0404-4404-8404-040404040404"),
        nullptr,
        nullptr,
        &dropTracker);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport);
    EXPECT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed);
    ASSERT_TRUE(result.dragDrop.instance.has_value());

    const auto events = dropTracker.GetEvents({1u});
    ASSERT_GE(events.size(), 4u);
    EXPECT_EQ(events.front().phase, NLS::Editor::Assets::ImportPhase::Queued);
    EXPECT_EQ(events.front().assetId, prefabId);
    EXPECT_EQ(events.front().sourcePath, "Assets/Progress Cube.prefab");
    EXPECT_NE(std::find_if(
        events.begin(),
        events.end(),
        [](const auto& event)
        {
            return event.phase == NLS::Editor::Assets::ImportPhase::SourceParse;
        }), events.end());
    EXPECT_NE(std::find_if(
        events.begin(),
        events.end(),
        [](const auto& event)
        {
            return event.phase == NLS::Editor::Assets::ImportPhase::Commit;
        }), events.end());
    EXPECT_EQ(events.back().phase, NLS::Editor::Assets::ImportPhase::Finished);
    EXPECT_EQ(events.back().terminalStatus, NLS::Editor::Assets::ImportJobTerminalStatus::Succeeded);
    EXPECT_FALSE(dropTracker.HasRunningJobs());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, ImportedPrefabHandleBlockingDropPublishesProgressWhenPayloadAlreadyImported)
{
    const auto root = MakeAssetDragDropRoot();

    NLS::Engine::GameObject gameObject("Blocking Progress Cube", "Prefab");
    const auto prefabId = Id("cb070707-0707-4707-8707-070707070707");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Blocking Progress Cube.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CreateTextAsset(
        created.prefabSourceText,
        "Assets/Blocking Progress Cube.prefab",
        prefabId));

    NLS::Editor::Assets::AssetPreimportScheduler scheduler;
    NLS::Editor::Assets::ImportProgressTracker importTracker;
    ASSERT_TRUE(scheduler.Run(database, importTracker, {
        NLS::Editor::Assets::AssetPreimportReason::FileWatcherChanged,
        {root / "Assets" / "Blocking Progress Cube.prefab"}
    }));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Blocking Progress Cube.prefab",
        prefabId,
        "prefab:Blocking Progress Cube",
        NLS::Core::Assets::ArtifactType::Prefab,
        false,
        true);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    NLS::Editor::Assets::ImportProgressTracker dropTracker;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchyBlocking(
        payload,
        scene,
        Id("cb080808-0808-4808-8808-080808080808"),
        &registry,
        nullptr,
        &dropTracker);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);

    const auto events = dropTracker.GetEvents({1u});
    ASSERT_GE(events.size(), 4u);
    EXPECT_EQ(events.front().phase, NLS::Editor::Assets::ImportPhase::Queued);
    EXPECT_EQ(events.front().assetId, prefabId);
    EXPECT_EQ(events.front().sourcePath, "Assets/Blocking Progress Cube.prefab");
    EXPECT_NE(std::find_if(
        events.begin(),
        events.end(),
        [](const auto& event)
        {
            return event.phase == NLS::Editor::Assets::ImportPhase::SourceParse &&
                event.message == "Loading prefab artifact";
        }), events.end());
    EXPECT_NE(std::find_if(
        events.begin(),
        events.end(),
        [](const auto& event)
        {
            return event.phase == NLS::Editor::Assets::ImportPhase::Commit &&
                event.message == "Instantiating prefab";
        }), events.end());
    EXPECT_EQ(events.back().phase, NLS::Editor::Assets::ImportPhase::Finished);
    EXPECT_EQ(events.back().terminalStatus, NLS::Editor::Assets::ImportJobTerminalStatus::Succeeded);
    EXPECT_FALSE(dropTracker.HasRunningJobs());

    std::filesystem::remove_all(root);
}

TEST(EditorAssetDragDropTests, ImportedPrefabHandleBlockingDropImportsBeforeCommitWhenPayloadIsCold)
{
    const auto root = MakeAssetDragDropRoot();

    NLS::Engine::GameObject gameObject("Cold Blocking Cube", "Prefab");
    const auto prefabId = Id("cb050505-0505-4505-8505-050505050505");
    const auto created = NLS::Editor::Assets::PrefabEditorWorkflow().CreatePrefabFromSelection({
        &gameObject,
        {},
        prefabId,
        "Assets/Cold Blocking Cube.prefab"
    });
    ASSERT_EQ(created.status, NLS::Editor::Assets::PrefabEditorOperationStatus::Committed);

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.CreateTextAsset(
        created.prefabSourceText,
        "Assets/Cold Blocking Cube.prefab",
        prefabId));

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Cold Blocking Cube.prefab",
        prefabId,
        "prefab:Cold Blocking Cube",
        NLS::Core::Assets::ArtifactType::Prefab,
        false,
        false);

    NLS::Engine::SceneSystem::Scene scene;
    NLS::Editor::Assets::PrefabInstanceRegistry registry;
    NLS::Editor::Assets::ImportProgressTracker progress;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto result = bridge.DropImportedAssetHandleIntoHierarchyBlocking(
        payload,
        scene,
        Id("cb060606-0606-4606-8606-060606060606"),
        &registry,
        nullptr,
        &progress);

    ASSERT_TRUE(result.handled);
    EXPECT_FALSE(result.pendingImport) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_EQ(result.dragDrop.status, DragDropOperationStatus::Committed) << JoinDiagnosticCodes(result.dragDrop);
    ASSERT_TRUE(result.dragDrop.instance.has_value());
    ASSERT_NE(result.dragDrop.instance->instanceRoot, nullptr);
    EXPECT_EQ(result.dragDrop.instance->instanceRoot->GetName(), "Cold Blocking Cube");
    EXPECT_NE(registry.FindInstance(*result.dragDrop.instance->instanceRoot), nullptr);
    EXPECT_FALSE(progress.HasRunningJobs());

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
