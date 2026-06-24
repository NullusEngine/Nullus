#include <gtest/gtest.h>

#include <Json/json.hpp>

#include "Assets/AssetId.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/NativeArtifactContainer.h"
#include "Core/Assets/ArtifactManifest.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Assets/ModelPrefabBuilder.h"
#include "Assets/ModelTextureReferenceResolver.h"
#include "Engine/SceneSystem/Scene.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Profiling/PerformanceStageStats.h"
#include "Guid.h"
#include "Rendering/Assets/ImportedScene.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/SerializationDiagnostic.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <algorithm>

namespace
{
const NLS::Engine::Serialize::ObjectRecord* FindRecord(
    const NLS::Engine::Serialize::ObjectGraphDocument& document,
    const std::string& debugName,
    const std::string& typeName)
{
    for (const auto& record : document.objects)
    {
        if (record.debugName == debugName && record.typeName == typeName)
            return &record;
    }
    return nullptr;
}

const NLS::Engine::Serialize::ObjectRecord* FindRecordById(
    const NLS::Engine::Serialize::ObjectGraphDocument& document,
    const NLS::Engine::Serialize::ObjectId& id)
{
    for (const auto& record : document.objects)
    {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

NLS::Engine::Serialize::ObjectId MakeGeneratedModelPrefabObjectId(
    const NLS::Render::Assets::ImportedScene& scene,
    const std::string& suffix)
{
    return NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic(
        "GeneratedModelPrefab:" + scene.sourceAssetId.ToString() + ":" + scene.sceneKey + ":" + suffix));
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

bool ContainsResolvedAsset(
    const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& assets,
    const std::string& expectedType,
    const std::string& subAssetKey)
{
    for (const auto& asset : assets)
    {
        if (asset.expectedType == expectedType && asset.subAssetKey == subAssetKey)
            return true;
    }
    return false;
}

const NLS::Engine::Assets::PrefabResolvedAsset* FindResolvedAsset(
    const std::vector<NLS::Engine::Assets::PrefabResolvedAsset>& assets,
    const std::string& expectedType,
    const std::string& subAssetKey)
{
    for (const auto& asset : assets)
    {
        if (asset.expectedType == expectedType && asset.subAssetKey == subAssetKey)
            return &asset;
    }
    return nullptr;
}

const NLS::Base::Profiling::PerformanceStageEntry* FindPrefabStage(
    const NLS::Base::Profiling::PerformanceStageStatsSnapshot& snapshot,
    const std::string& stageName)
{
    for (const auto& stage : snapshot.stages)
    {
        if (stage.domain == NLS::Base::Profiling::PerformanceStageDomain::Prefab &&
            stage.stageName == stageName)
        {
            return &stage;
        }
    }
    return nullptr;
}

class ScopedCreatedEventListener
{
public:
    explicit ScopedCreatedEventListener(NLS::ListenerID listenerId)
        : m_listenerId(listenerId)
    {
    }

    ~ScopedCreatedEventListener()
    {
        if (m_active)
            NLS::Engine::GameObject::CreatedEvent -= m_listenerId;
    }

    ScopedCreatedEventListener(const ScopedCreatedEventListener&) = delete;
    ScopedCreatedEventListener& operator=(const ScopedCreatedEventListener&) = delete;

    ScopedCreatedEventListener(ScopedCreatedEventListener&& other) noexcept
        : m_listenerId(other.m_listenerId)
        , m_active(other.m_active)
    {
        other.m_active = false;
    }

    ScopedCreatedEventListener& operator=(ScopedCreatedEventListener&& other) noexcept
    {
        if (this != &other)
        {
            if (m_active)
                NLS::Engine::GameObject::CreatedEvent -= m_listenerId;
            m_listenerId = other.m_listenerId;
            m_active = other.m_active;
            other.m_active = false;
        }
        return *this;
    }

private:
    NLS::ListenerID m_listenerId = NLS::InvalidListenerID;
    bool m_active = true;
};

class LifecycleCounterComponent : public NLS::Engine::Components::Component
{
public:
    void OnAwake() override { ++awakeCount; }
    void OnStart() override { ++startCount; }
    void OnEnable() override { ++enableCount; }

    size_t awakeCount = 0u;
    size_t startCount = 0u;
    size_t enableCount = 0u;
};

NLS::Editor::Assets::UnifiedPrefabLoadKey MakePreparedPrefabFreshnessKey()
{
    NLS::Editor::Assets::UnifiedPrefabLoadKey key;
    key.source.projectRootId = "project:prepared-cache-freshness";
    key.source.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("12121212-1212-4212-8212-121212121212"));
    key.source.sourceAssetPath = "Assets/Models/FreshnessHero.gltf";
    key.source.prefabSubAssetKey = "prefab:FreshnessHero";
    key.source.assetType = NLS::Core::Assets::AssetType::ModelScene;
    key.source.importerId = "gltf";
    key.source.importerVersion = 7u;
    key.artifactIdentity =
        "project:prepared-cache-freshness|12121212-1212-4212-8212-121212121212|"
        "Assets/Models/FreshnessHero.gltf|prefab:FreshnessHero|ModelScene|gltf|7";
    key.manifestStamp = "manifest@baseline";
    key.dependencyStamp = "dependency@baseline";
    key.prefabArtifactStamp = "prefab@baseline";
    key.rendererArtifactStamp = "renderer@baseline";
    key.prefabImporterVersion = key.source.importerVersion;
    key.reflectionSchemaVersion = 3u;
    key.serializationFormatVersion = 5u;
    key.dependencyManifestVersion = 11u;
    key.runtimeCacheIdentity =
        key.artifactIdentity +
        "|manifest@" + key.manifestStamp +
        "|dependency@" + key.dependencyStamp +
        "|prefab@" + key.prefabArtifactStamp +
        "|renderer@" + key.rendererArtifactStamp +
        "|importer@7|reflection@3|serialization@5|dependencyManifest@11";
    return key;
}

std::filesystem::path MakePrefabPipelineTempRoot(const std::string& prefix)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        (prefix + "_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    return root;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void WriteBytesFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::string MakeSingleMeshGltf(const std::string& rootNodeName)
{
    return
        "{\n"
        "  \"asset\": { \"version\": \"2.0\" },\n"
        "  \"scene\": 0,\n"
        "  \"scenes\": [{ \"nodes\": [0] }],\n"
        "  \"buffers\": [\n"
        "    {\n"
        "      \"uri\": \"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA\",\n"
        "      \"byteLength\": 42\n"
        "    }\n"
        "  ],\n"
        "  \"bufferViews\": [\n"
        "    { \"buffer\": 0, \"byteOffset\": 0, \"byteLength\": 36, \"target\": 34962 },\n"
        "    { \"buffer\": 0, \"byteOffset\": 36, \"byteLength\": 6, \"target\": 34963 }\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    { \"bufferView\": 0, \"componentType\": 5126, \"count\": 3, \"type\": \"VEC3\" },\n"
        "    { \"bufferView\": 1, \"componentType\": 5123, \"count\": 3, \"type\": \"SCALAR\" }\n"
        "  ],\n"
        "  \"meshes\": [\n"
        "    {\n"
        "      \"name\": \"Body\",\n"
        "      \"primitives\": [\n"
        "        { \"attributes\": { \"POSITION\": 0 }, \"indices\": 1 }\n"
        "      ]\n"
        "    }\n"
        "  ],\n"
        "  \"nodes\": [\n"
        "    { \"name\": \"" + rootNodeName + "\", \"mesh\": 0 }\n"
        "  ]\n"
        "}\n";
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

bool ContainsArtifactTelemetryStagePath(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
    const std::filesystem::path& path)
{
    const auto expectedPath = path.lexically_normal().generic_string();
    return std::any_of(
        records.begin(),
        records.end(),
        [stage, &expectedPath](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == stage &&
                std::filesystem::path(record.path).lexically_normal().generic_string() == expectedPath;
        });
}

class ScopedPrefabPipelineTempRoot
{
public:
    explicit ScopedPrefabPipelineTempRoot(const std::string& prefix)
        : m_path(MakePrefabPipelineTempRoot(prefix))
    {
    }

    ~ScopedPrefabPipelineTempRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    ScopedPrefabPipelineTempRoot(const ScopedPrefabPipelineTempRoot&) = delete;
    ScopedPrefabPipelineTempRoot& operator=(const ScopedPrefabPipelineTempRoot&) = delete;

    const std::filesystem::path& Path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

NLS::Engine::Serialize::PropertyValue MakeObjectReference(
    NLS::Core::Assets::AssetId assetId,
    std::string filePath)
{
    const auto localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(
        assetId.GetGuid(),
        filePath);
    return NLS::Engine::Serialize::PropertyValue::ObjectReference(
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            NLS::Engine::Serialize::AssetId(assetId.GetGuid()),
            localIdentifierInFile,
            std::move(filePath)));
}
}

TEST(AssetPrefabPipelineTests, PrefabArtifactTracksBaseChainResolvedAssetsAndInstanceMap)
{
    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("33333333-3333-4333-8333-333333333333"));
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;

    const auto basePrefabId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("44444444-4444-4444-8444-444444444444"));
    const auto materialId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("55555555-5555-4555-8555-555555555555"));
    artifact.baseChain.push_back(basePrefabId);
    artifact.resolvedAssets.push_back({materialId, "Material", "material:Hero", "Library/Artifacts/Hero/material.nmat"});

    const auto sourceObject = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("66666666-6666-4666-8666-666666666666"));
    const auto runtimeObject = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("77777777-7777-4777-8777-777777777777"));
    artifact.sourceToRuntimeObject.emplace(sourceObject, runtimeObject);

    ASSERT_EQ(artifact.baseChain.size(), 1u);
    EXPECT_EQ(artifact.baseChain.front(), basePrefabId);
    ASSERT_EQ(artifact.resolvedAssets.size(), 1u);
    EXPECT_EQ(artifact.resolvedAssets.front().assetId, materialId);
    EXPECT_EQ(artifact.resolvedAssets.front().expectedType, "Material");
    EXPECT_EQ(artifact.resolvedAssets.front().subAssetKey, "material:Hero");
    EXPECT_EQ(artifact.resolvedAssets.front().artifactPath, "Library/Artifacts/Hero/material.nmat");

    const auto* mapped = artifact.FindRuntimeObject(sourceObject);
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(*mapped, runtimeObject);
    EXPECT_EQ(
        artifact.FindRuntimeObject(NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Missing"))),
        nullptr);
}

TEST(AssetPrefabPipelineTests, PrefabArtifactValidationUsesObjectGraphDiagnostics)
{
    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;
    artifact.graph.documentId = NLS::Guid::NewDeterministic("BrokenPrefab");
    artifact.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("MissingRoot"));
    artifact.graph.overrides.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("MissingOverrideTarget")),
        "name",
        NLS::Engine::Serialize::PropertyValue::String("Broken")));

    const auto diagnostics = artifact.Validate();

    ASSERT_TRUE(diagnostics.HasErrors());
    ASSERT_GE(diagnostics.GetItems().size(), 2u);

    bool foundMissingRoot = false;
    bool foundInvalidOverride = false;
    for (const auto& diagnostic : diagnostics.GetItems())
    {
        foundMissingRoot |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::MissingObject;
        foundInvalidOverride |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidPrefabOverride;
    }

    EXPECT_TRUE(foundMissingRoot);
    EXPECT_TRUE(foundInvalidOverride);
}

TEST(AssetPrefabPipelineTests, ImportsPrefabSourceDocumentAndCapturesBaseDependency)
{
    NLS::Engine::Serialize::ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::Parse("88888888-8888-4888-8888-888888888888");
    document.root = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("99999999-9999-4999-8999-999999999999"));
    document.basePrefab = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(
            NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            "Assets/Prefabs/Base.prefab"),
        "Assets/Prefabs/Base.prefab");

    NLS::Engine::Serialize::ObjectRecord root;
    root.id = document.root;
    root.localIdentifierInFile = NLS::Engine::Serialize::MakeLocalIdentifierInFile(document.root);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "ImportedPrefab";
    document.objects.push_back(root);

    const auto text = NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));

    const auto result = NLS::Engine::Assets::ImportPrefabArtifact(text, assetId);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.artifact.assetId, assetId);
    EXPECT_EQ(result.artifact.graph.format, "Nullus.ObjectGraph.Prefab");
    ASSERT_EQ(result.artifact.baseChain.size(), 1u);
    EXPECT_EQ(result.artifact.baseChain.front().GetGuid().ToString(), "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
}

TEST(AssetPrefabPipelineTests, ImportPrefabSourceReportsObjectGraphDiagnostics)
{
    NLS::Engine::Serialize::ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    document.root = NLS::Engine::Serialize::ObjectId(
        NLS::Guid::Parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd"));
    document.overrides.push_back(NLS::Engine::Serialize::PatchOperation::ReplaceProperty(
        NLS::Engine::Serialize::ObjectId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")),
        "name",
        NLS::Engine::Serialize::PropertyValue::String("Broken")));

    const auto text = NLS::Engine::Serialize::ObjectGraphWriter::Write(document);
    const auto result = NLS::Engine::Assets::ImportPrefabArtifact(
        text,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("ffffffff-ffff-4fff-8fff-ffffffffffff")));

    ASSERT_TRUE(result.diagnostics.HasErrors());
    bool foundInvalidOverride = false;
    bool foundMissingRoot = false;
    for (const auto& diagnostic : result.diagnostics.GetItems())
    {
        foundInvalidOverride |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidPrefabOverride;
        foundMissingRoot |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::MissingObject;
    }
    EXPECT_TRUE(foundInvalidOverride);
    EXPECT_TRUE(foundMissingRoot);
}

TEST(AssetPrefabPipelineTests, SerializesGameObjectSelectionAsPrefabSource)
{
    NLS::Engine::GameObject root("WorkshopLamp", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(3.0f);
    NLS::Engine::GameObject child("Bulb", "Part");
    child.SetParent(root);

    const auto prefab = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    child.DetachFromParent();
    const auto text = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.graph);
    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("10101010-1010-4010-8010-101010101010"));

    const auto result = NLS::Engine::Assets::ImportPrefabArtifact(text, assetId);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.artifact.assetId, assetId);
    EXPECT_EQ(result.artifact.graph.format, "Nullus.ObjectGraph.Prefab");
    ASSERT_EQ(result.artifact.graph.objects.size(), 5u);

    const auto* importedRoot = FindRecord(
        result.artifact.graph,
        "WorkshopLamp",
        "NLS::Engine::GameObject");
    ASSERT_NE(importedRoot, nullptr);
    const auto* children = FindProperty(*importedRoot, "children");
    ASSERT_NE(children, nullptr);
    ASSERT_EQ(children->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(children->value.GetArray().size(), 1u);
}

TEST(AssetPrefabPipelineTests, ImportPrefabSourceRebuildsResolvedAssetsFromExternalReferences)
{
    using namespace NLS::Engine::Serialize;

    const auto prefabId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-2222-4333-8444-555555555555"));
    const auto meshId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("12121212-1212-4212-8212-121212121212"));
    const std::string meshArtifactPath = "Library/Artifacts/Hero/mesh.nmesh";

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::NewDeterministic("ImportResolvedAssets.Document");
    document.root = ObjectId(NLS::Guid::NewDeterministic("ImportResolvedAssets.Root"));
    document.objects.push_back({
        document.root,
        "NLS::Engine::GameObject",
        "Renderable",
        "",
        ObjectRecordState::Alive,
        {
            {"name", PropertyValue::String("Renderable")},
            {"tag", PropertyValue::String("Prefab")},
            {"components", PropertyValue::Array({})},
            {"children", PropertyValue::Array({})},
            {"parent", PropertyValue::Null()},
            {"mesh", MakeObjectReference(meshId, meshArtifactPath)}
        },
        MakeLocalIdentifierInFile(document.root)
    });

    const auto result = NLS::Engine::Assets::ImportPrefabArtifact(
        ObjectGraphWriter::Write(document),
        prefabId);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_EQ(result.artifact.resolvedAssets.size(), 1u);
    EXPECT_EQ(result.artifact.resolvedAssets.front().assetId, meshId);
    EXPECT_EQ(result.artifact.resolvedAssets.front().expectedType, "Mesh");
    EXPECT_EQ(result.artifact.resolvedAssets.front().subAssetKey, meshArtifactPath);
    EXPECT_EQ(result.artifact.resolvedAssets.front().artifactPath, meshArtifactPath);
}

TEST(AssetPrefabPipelineTests, InstantiatesPrefabArtifactWithStableSourceToInstanceMap)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(2.0f);
    auto artifact = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(
            NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root).graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("20202020-2020-4020-8020-202020202020"))).artifact;

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    EXPECT_EQ(instance.root->GetName(), "Crate");
    ASSERT_EQ(scene.GetGameObjects().size(), 1u);
    EXPECT_EQ(scene.GetGameObjects()[0], instance.root);
    EXPECT_GE(instance.sourceToInstance.size(), 2u);
    EXPECT_EQ(artifact.sourceToRuntimeObject.size(), instance.sourceToInstance.size());
    for (const auto& mapping : instance.sourceToInstance)
    {
        EXPECT_TRUE(mapping.first.IsValid());
        EXPECT_TRUE(mapping.second.IsValid());
        EXPECT_NE(mapping.first, mapping.second);
        const auto* runtimeObject = artifact.FindRuntimeObject(mapping.first);
        ASSERT_NE(runtimeObject, nullptr);
        EXPECT_EQ(*runtimeObject, mapping.second);
    }
}

TEST(AssetPrefabPipelineTests, RepeatedPrefabInstantiationKeepsHotPathIndexedAndInstancesIndependent)
{
    NLS::Engine::GameObject root("Crate", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(2.0f);
    auto child = std::make_unique<NLS::Engine::GameObject>("CrateChild", "Prop");
    child->AddComponent<NLS::Engine::Components::MeshRenderer>();
    child->SetParent(root);

    auto artifact = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(
            NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root).graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("24242424-2424-4424-8424-242424242424"))).artifact;
    child->DetachFromParent();

    NLS::Engine::Serialize::LoadPolicy policy;
    policy.suppressGameObjectCreatedEvents = true;

    size_t createdEventCount = 0u;
    ScopedCreatedEventListener createdEventListener(
        NLS::Engine::GameObject::CreatedEvent +=
        [&createdEventCount](NLS::Engine::GameObject&)
        {
            ++createdEventCount;
        });

    NLS::Base::Profiling::PerformanceStageStats stats;
    NLS::Base::Profiling::PerformanceStageStatsCapture capture(stats);

    NLS::Engine::SceneSystem::Scene firstScene;
    const auto first = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, firstScene, policy);
    NLS::Engine::SceneSystem::Scene secondScene;
    const auto second = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, secondScene, policy);

    ASSERT_FALSE(first.diagnostics.HasErrors());
    ASSERT_FALSE(second.diagnostics.HasErrors());
    ASSERT_NE(first.root, nullptr);
    ASSERT_NE(second.root, nullptr);
    EXPECT_NE(first.root, second.root);
    EXPECT_EQ(first.root->GetName(), "Crate");
    EXPECT_EQ(second.root->GetName(), "Crate");
    ASSERT_EQ(first.root->GetChildren().size(), 1u);
    ASSERT_EQ(second.root->GetChildren().size(), 1u);
    EXPECT_EQ(first.root->GetChildren()[0]->GetName(), "CrateChild");
    EXPECT_EQ(second.root->GetChildren()[0]->GetName(), "CrateChild");
    EXPECT_NE(first.root->GetChildren()[0], second.root->GetChildren()[0]);
    EXPECT_EQ(first.root->GetChildren()[0]->GetParent(), first.root);
    EXPECT_EQ(second.root->GetChildren()[0]->GetParent(), second.root);
    EXPECT_EQ(createdEventCount, 0u);

    ASSERT_EQ(first.sourceToInstance.size(), second.sourceToInstance.size());
    for (const auto& [sourceObject, firstInstance] : first.sourceToInstance)
    {
        const auto secondFound = second.sourceToInstance.find(sourceObject);
        ASSERT_NE(secondFound, second.sourceToInstance.end());
        EXPECT_NE(firstInstance, secondFound->second);
    }

    const auto snapshot = stats.Snapshot();
    const auto* createComponents = FindPrefabStage(snapshot, "CreateComponents");
    ASSERT_NE(createComponents, nullptr);
    EXPECT_EQ(createComponents->callCount, 2u);
    ASSERT_TRUE(createComponents->counters.contains("indexedRecordLookupCount"));
    ASSERT_TRUE(createComponents->counters.contains("linearRecordLookupCount"));

    const auto* deserialize = FindPrefabStage(snapshot, "DeserializeComponents");
    ASSERT_NE(deserialize, nullptr);
    EXPECT_EQ(deserialize->callCount, 2u);
    ASSERT_TRUE(deserialize->counters.contains("indexedRecordLookupCount"));
    ASSERT_TRUE(deserialize->counters.contains("linearRecordLookupCount"));
    EXPECT_GE(
        createComponents->counters.at("indexedRecordLookupCount") +
            deserialize->counters.at("indexedRecordLookupCount"),
        first.sourceToInstance.size() + second.sourceToInstance.size());
    EXPECT_EQ(createComponents->counters.at("linearRecordLookupCount"), 0u);
    EXPECT_EQ(deserialize->counters.at("linearRecordLookupCount"), 0u);
}

TEST(AssetPrefabPipelineTests, SceneDeferredActivationDelaysLifecycleUntilExplicitActivation)
{
    NLS::Engine::SceneSystem::Scene scene;
    scene.Play();

    auto* deferred = new NLS::Engine::GameObject(
        NLS::Engine::GameObject::SilentCreationTag {},
        "DeferredRuntimeObject",
        "Prefab");
    auto* lifecycleCounter = deferred->AddComponent<LifecycleCounterComponent>();
    ASSERT_NE(lifecycleCounter, nullptr);
    auto* child = new NLS::Engine::GameObject(
        NLS::Engine::GameObject::SilentCreationTag {},
        "DeferredRuntimeChild",
        "Prefab");
    auto* childLifecycleCounter = child->AddComponent<LifecycleCounterComponent>();
    ASSERT_NE(childLifecycleCounter, nullptr);
    child->SetParent(*deferred);

    ASSERT_TRUE(scene.AddGameObject(
        deferred,
        NLS::Engine::SceneSystem::Scene::AddGameObjectActivation::Deferred));
    EXPECT_FALSE(deferred->HasAwaked());
    EXPECT_FALSE(deferred->HasStarted());
    EXPECT_FALSE(child->HasAwaked());
    EXPECT_FALSE(child->HasStarted());

    EXPECT_EQ(scene.ActivateGameObjectForPlay(deferred), 2u);
    EXPECT_TRUE(deferred->HasAwaked());
    EXPECT_TRUE(deferred->HasStarted());
    EXPECT_TRUE(child->HasAwaked());
    EXPECT_TRUE(child->HasStarted());
    EXPECT_EQ(lifecycleCounter->awakeCount, 1u);
    EXPECT_EQ(lifecycleCounter->startCount, 1u);
    EXPECT_EQ(lifecycleCounter->enableCount, 1u);
    EXPECT_EQ(childLifecycleCounter->awakeCount, 1u);
    EXPECT_EQ(childLifecycleCounter->startCount, 1u);
    EXPECT_EQ(childLifecycleCounter->enableCount, 1u);

    EXPECT_EQ(scene.ActivateGameObjectForPlay(deferred), 0u);
    EXPECT_EQ(lifecycleCounter->awakeCount, 1u);
    EXPECT_EQ(lifecycleCounter->startCount, 1u);
    EXPECT_EQ(lifecycleCounter->enableCount, 1u);
    EXPECT_EQ(childLifecycleCounter->awakeCount, 1u);
    EXPECT_EQ(childLifecycleCounter->startCount, 1u);
    EXPECT_EQ(childLifecycleCounter->enableCount, 1u);
}

TEST(AssetPrefabPipelineTests, DeferredPrefabActivationRunsLifecycleInInvokeLifecyclePhase)
{
    NLS::Engine::GameObject root("RuntimeCrate", "Prop");
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(3.0f);
    auto child = std::make_unique<NLS::Engine::GameObject>("RuntimeCrateChild", "Prop");
    child->AddComponent<NLS::Engine::Components::MeshRenderer>();
    child->SetParent(root);

    auto artifact = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(
            NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root).graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("25252525-2525-4525-8525-252525252525"))).artifact;
    child->DetachFromParent();

    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferActivation = true;
    policy.suppressGameObjectCreatedEvents = true;

    NLS::Base::Profiling::PerformanceStageStats stats;
    NLS::Base::Profiling::PerformanceStageStatsCapture capture(stats);

    NLS::Engine::SceneSystem::Scene scene;
    scene.Play();
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    EXPECT_TRUE(instance.root->HasAwaked());
    EXPECT_TRUE(instance.root->HasStarted());
    ASSERT_EQ(instance.root->GetChildren().size(), 1u);
    EXPECT_TRUE(instance.root->GetChildren()[0]->HasAwaked());
    EXPECT_TRUE(instance.root->GetChildren()[0]->HasStarted());

    const auto snapshot = stats.Snapshot();
    const auto* lifecycle = FindPrefabStage(snapshot, "InvokeLifecycle");
    ASSERT_NE(lifecycle, nullptr);
    EXPECT_EQ(lifecycle->callCount, 1u);
    ASSERT_TRUE(lifecycle->counters.contains("activatedObjectCount"));
    EXPECT_EQ(lifecycle->counters.at("activatedObjectCount"), 2u);
}

TEST(AssetPrefabPipelineTests, AmbiguousEmptyAssetReferenceHintDoesNotResolveToFirstSubAsset)
{
    using namespace NLS::Engine::Serialize;

    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("21212121-2121-4121-8121-212121212121"));

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::NewDeterministic("AmbiguousAssetHint.Prefab");
    document.root = ObjectId(NLS::Guid::NewDeterministic("AmbiguousAssetHint.Root"));

    ObjectRecord root;
    root.id = document.root;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(document.root);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "AmbiguousRoot";
    root.properties.push_back({"name", PropertyValue::String("AmbiguousRoot")});
    root.properties.push_back({"tag", PropertyValue::String("Prefab")});
    root.properties.push_back({"active", PropertyValue::Bool(true)});

    const auto meshFilterId = ObjectId(NLS::Guid::NewDeterministic("AmbiguousAssetHint.MeshFilter"));
    root.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(meshFilterId)})});
    document.objects.push_back(root);

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.debugName = "AmbiguousMeshFilter";
    meshFilter.properties.push_back({"mesh", MakeObjectReference(assetId, {})});
    document.objects.push_back(meshFilter);

    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = assetId;
    artifact.graph = document;
    artifact.resolvedAssets.push_back({assetId, "Mesh", "mesh:mesh/0", "Library/Artifacts/Hero/mesh0.nmesh"});
    artifact.resolvedAssets.push_back({assetId, "Mesh", "mesh:mesh/1", "Library/Artifacts/Hero/mesh1.nmesh"});

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    auto* meshFilterComponent = instance.root->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(meshFilterComponent, nullptr);
    EXPECT_TRUE(meshFilterComponent->GetModelPath().empty());
}

TEST(AssetPrefabPipelineTests, ImportPrefabSourceKeepsAmbiguousEmptyAssetReferenceUnresolved)
{
    using namespace NLS::Engine::Serialize;

    const auto prefabId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("22222222-2121-4121-8121-212121212121"));
    const auto meshAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("23232323-2323-4323-8323-232323232323"));

    ObjectGraphDocument document;
    document.format = "Nullus.ObjectGraph.Prefab";
    document.version = 1;
    document.documentId = NLS::Guid::NewDeterministic("AmbiguousImportAssetHint.Prefab");
    document.root = ObjectId(NLS::Guid::NewDeterministic("AmbiguousImportAssetHint.Root"));

    ObjectRecord root;
    root.id = document.root;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(document.root);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "AmbiguousImportRoot";
    root.properties.push_back({"name", PropertyValue::String("AmbiguousImportRoot")});
    root.properties.push_back({"tag", PropertyValue::String("Prefab")});
    root.properties.push_back({"active", PropertyValue::Bool(true)});

    const auto meshFilterId = ObjectId(NLS::Guid::NewDeterministic("AmbiguousImportAssetHint.MeshFilter"));
    root.properties.push_back({"components", PropertyValue::Array({PropertyValue::OwnedReference(meshFilterId)})});
    document.objects.push_back(root);

    ObjectRecord meshFilter;
    meshFilter.id = meshFilterId;
    meshFilter.localIdentifierInFile = MakeLocalIdentifierInFile(meshFilterId);
    meshFilter.typeName = "NLS::Engine::Components::MeshFilter";
    meshFilter.debugName = "AmbiguousImportMeshFilter";
    meshFilter.properties.push_back({"mesh", MakeObjectReference(meshAssetId, {})});
    document.objects.push_back(meshFilter);

    std::vector<NLS::Engine::Assets::PrefabResolvedAsset> existingResolvedAssets;
    existingResolvedAssets.push_back(
        {meshAssetId, "Mesh", "mesh:mesh/0", "Library/Artifacts/Hero/mesh0.nmesh"});
    existingResolvedAssets.push_back(
        {meshAssetId, "Mesh", "mesh:mesh/1", "Library/Artifacts/Hero/mesh1.nmesh"});

    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        ObjectGraphWriter::Write(document),
        prefabId,
        std::move(existingResolvedAssets));

    ASSERT_FALSE(importResult.diagnostics.HasErrors());
    ASSERT_EQ(importResult.artifact.resolvedAssets.size(), 1u);
    EXPECT_TRUE(importResult.artifact.resolvedAssets.front().subAssetKey.empty());
    EXPECT_TRUE(importResult.artifact.resolvedAssets.front().artifactPath.empty());

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(
        importResult.artifact,
        scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    auto* meshFilterComponent = instance.root->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(meshFilterComponent, nullptr);
    EXPECT_TRUE(meshFilterComponent->GetModelPath().empty());
}

TEST(AssetPrefabPipelineTests, ValidatesPrefabBaseChainsAndMissingBases)
{
    const auto rootId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("30303030-3030-4030-8030-303030303030"));
    const auto variantId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("40404040-4040-4040-8040-404040404040"));
    const auto missingBaseId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("50505050-5050-4050-8050-505050505050"));

    NLS::Engine::Assets::PrefabArtifact root;
    root.assetId = rootId;
    root.graph.format = "Nullus.ObjectGraph.Prefab";
    root.graph.version = 1;
    root.graph.documentId = NLS::Guid::NewDeterministic("PrefabBase.Root");
    root.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("PrefabBase.Root.Object"));
    root.graph.objects.push_back({
        root.graph.root,
        "NLS::Engine::GameObject",
        "RootPrefab",
        "",
        NLS::Engine::Serialize::ObjectRecordState::Alive,
        {
            {"name", NLS::Engine::Serialize::PropertyValue::String("RootPrefab")},
            {"tag", NLS::Engine::Serialize::PropertyValue::String({})},
            {"components", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"children", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"parent", NLS::Engine::Serialize::PropertyValue::Null()}
        },
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(root.graph.root)
    });

    NLS::Engine::Assets::PrefabArtifact variant = root;
    variant.assetId = variantId;
    variant.baseChain = {rootId};

    const auto validDiagnostics = NLS::Engine::Assets::ValidatePrefabBaseChains({root, variant});
    EXPECT_FALSE(validDiagnostics.HasErrors());

    variant.baseChain = {missingBaseId};
    const auto missingDiagnostics = NLS::Engine::Assets::ValidatePrefabBaseChains({root, variant});
    ASSERT_TRUE(missingDiagnostics.HasErrors());
    EXPECT_EQ(missingDiagnostics.GetItems()[0].GetCode(), NLS::Engine::Serialize::SerializationDiagnosticCode::MissingAsset);

    root.baseChain = {variantId};
    variant.baseChain = {rootId};
    const auto cycleDiagnostics = NLS::Engine::Assets::ValidatePrefabBaseChains({root, variant});
    ASSERT_TRUE(cycleDiagnostics.HasErrors());
    EXPECT_EQ(cycleDiagnostics.GetItems()[0].GetCode(), NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidPrefabOverride);
}

TEST(AssetPrefabPipelineTests, ReportsUnresolvedAssetReferencesInPrefabArtifacts)
{
    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("60606060-6060-4060-8060-606060606060"));
    artifact.graph.format = "Nullus.ObjectGraph.Prefab";
    artifact.graph.version = 1;
    artifact.graph.documentId = NLS::Guid::NewDeterministic("Prefab.UnresolvedAsset.Document");
    artifact.graph.root = NLS::Engine::Serialize::ObjectId(NLS::Guid::NewDeterministic("Prefab.UnresolvedAsset.Root"));
    artifact.graph.objects.push_back({
        artifact.graph.root,
        "NLS::Engine::GameObject",
        "BrokenAssetPrefab",
        "",
        NLS::Engine::Serialize::ObjectRecordState::Alive,
        {
            {"name", NLS::Engine::Serialize::PropertyValue::String("BrokenAssetPrefab")},
            {"tag", NLS::Engine::Serialize::PropertyValue::String({})},
            {"components", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"children", NLS::Engine::Serialize::PropertyValue::Array({})},
            {"parent", NLS::Engine::Serialize::PropertyValue::Null()},
            {"material", NLS::Engine::Serialize::PropertyValue::ObjectReference(
                NLS::Engine::Serialize::ObjectIdentifier::Asset(
                    NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("70707070-7070-4070-8070-707070707070")),
                    NLS::Engine::Serialize::MakeLocalIdentifierInFile(
                        NLS::Guid::Parse("70707070-7070-4070-8070-707070707070"),
                        "material:Missing"),
                    "material:Missing"))}
        },
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(artifact.graph.root)
    });

    const auto diagnostics = artifact.Validate();

    ASSERT_TRUE(diagnostics.HasErrors());
    bool foundUnresolvedAsset = false;
    for (const auto& diagnostic : diagnostics.GetItems())
        foundUnresolvedAsset |= diagnostic.GetCode() == NLS::Engine::Serialize::SerializationDiagnosticCode::MissingAsset;
    EXPECT_TRUE(foundUnresolvedAsset);
}

TEST(AssetPrefabPipelineTests, NormalizesPrefabOverridePatchesDeterministically)
{
    using namespace NLS::Engine::Serialize;

    const auto rootId = ObjectId(NLS::Guid::Parse("71717171-7171-4171-8171-717171717171"));
    const auto lightId = ObjectId(NLS::Guid::Parse("72727272-7272-4272-8272-727272727272"));
    const auto childId = ObjectId(NLS::Guid::Parse("73737373-7373-4373-8373-737373737373"));

    std::vector<PatchOperation> patches;
    patches.push_back(PatchOperation::MoveOwned(rootId, "children", childId, 2u));
    patches.push_back(PatchOperation::ReplaceProperty(rootId, "tag", PropertyValue::String("Prop")));
    patches.push_back(PatchOperation::ReplaceProperty(rootId, "name", PropertyValue::String("Crate")));
    patches.push_back(PatchOperation::ReplaceProperty(rootId, "name", PropertyValue::String("FinalCrate")));
    patches.push_back(PatchOperation::InsertOwned(rootId, "components", lightId, 1u));

    const auto normalized = NLS::Engine::Assets::NormalizePrefabOverridePatches(patches);

    ASSERT_EQ(normalized.size(), 4u);
    EXPECT_EQ(normalized[0].type, PatchOperationType::ReplaceProperty);
    EXPECT_EQ(normalized[0].property, "name");
    EXPECT_EQ(normalized[0].value.GetString(), "FinalCrate");
    EXPECT_EQ(normalized[1].type, PatchOperationType::ReplaceProperty);
    EXPECT_EQ(normalized[1].property, "tag");
    EXPECT_EQ(normalized[2].type, PatchOperationType::InsertOwned);
    EXPECT_EQ(normalized[2].property, "components");
    EXPECT_EQ(normalized[3].type, PatchOperationType::MoveOwned);
    EXPECT_EQ(normalized[3].property, "children");
}

TEST(AssetPrefabPipelineTests, ExtractsNestedPrefabDependenciesAndValidatesCycles)
{
    using namespace NLS::Engine::Serialize;

    const auto parentId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("81818181-8181-4181-8181-818181818181"));
    const auto childId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("82828282-8282-4282-8282-828282828282"));
    const auto missingId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("83838383-8383-4383-8383-838383838383"));

    NLS::Engine::Assets::PrefabArtifact parent;
    parent.assetId = parentId;
    parent.graph.format = "Nullus.ObjectGraph.Prefab";
    parent.graph.version = 1;
    parent.graph.documentId = NLS::Guid::NewDeterministic("Nested.Parent.Document");
    parent.graph.root = ObjectId(NLS::Guid::NewDeterministic("Nested.Parent.Root"));
    parent.graph.objects.push_back({
        parent.graph.root,
        "NLS::Engine::GameObject",
        "Parent",
        "",
        ObjectRecordState::Alive,
        {
            {"name", PropertyValue::String("Parent")},
            {"tag", PropertyValue::String({})},
            {"components", PropertyValue::Array({})},
            {"children", PropertyValue::Array({})},
            {"parent", PropertyValue::Null()},
            {"nestedPrefab", MakeObjectReference(childId, "prefab:Child")}
        },
        MakeLocalIdentifierInFile(parent.graph.root)
    });

    const auto dependencies = NLS::Engine::Assets::ExtractNestedPrefabDependencies(parent);
    ASSERT_EQ(dependencies.size(), 1u);
    EXPECT_EQ(dependencies.front(), childId);

    NLS::Engine::Assets::PrefabArtifact child = parent;
    child.assetId = childId;
    child.graph.objects.front().properties.back().value = MakeObjectReference(parentId, "prefab:Parent");

    const auto cycleDiagnostics = NLS::Engine::Assets::ValidateNestedPrefabDependencies({parent, child});
    ASSERT_TRUE(cycleDiagnostics.HasErrors());
    EXPECT_EQ(
        cycleDiagnostics.GetItems()[0].GetCode(),
        NLS::Engine::Serialize::SerializationDiagnosticCode::InvalidPrefabOverride);

    child.graph.objects.front().properties.back().value = MakeObjectReference(missingId, "prefab:Missing");
    const auto missingDiagnostics = NLS::Engine::Assets::ValidateNestedPrefabDependencies({parent, child});
    ASSERT_TRUE(missingDiagnostics.HasErrors());
    EXPECT_EQ(
        missingDiagnostics.GetItems()[0].GetCode(),
        NLS::Engine::Serialize::SerializationDiagnosticCode::MissingAsset);
}

TEST(AssetPrefabPipelineTests, BuildsGeneratedModelPrefabHierarchyWithRendererAssetReferences)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));
    scene.sceneKey = "HeroScene";
    scene.nodes.push_back({"node/root", "Hero", "", "", ""});
    scene.nodes.push_back({"node/body", "Body", "node/root", "mesh/body", ""});
    NLS::Render::Assets::ImportedScenePrimitive bodyPrimitive;
    bodyPrimitive.materialKey = "converted-material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord bodyMesh;
    bodyMesh.sourceKey = "mesh/body";
    bodyMesh.name = "BodyMesh";
    bodyMesh.primitives.push_back(std::move(bodyPrimitive));
    scene.meshes.push_back(std::move(bodyMesh));
    scene.materials.push_back({"converted-material/body", "BodyMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.primarySubAssetKey = "prefab:HeroScene";
    manifest.subAssets.push_back({
        scene.sourceAssetId,
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Hero/BodyMesh.nmesh",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        scene.sourceAssetId,
        "material:converted-material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/BodyMaterial.nmat",
        "material-hash"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_EQ(result.artifact.graph.format, "Nullus.ObjectGraph.Prefab");
    EXPECT_TRUE(result.artifact.assetId.IsValid());
    EXPECT_FALSE(result.artifact.graph.Validate().HasErrors());

    const auto* hero = FindRecord(result.artifact.graph, "Hero", "NLS::Engine::GameObject");
    ASSERT_NE(hero, nullptr);
    const auto* body = FindRecord(result.artifact.graph, "Body", "NLS::Engine::GameObject");
    ASSERT_NE(body, nullptr);

    const auto* bodyParent = FindProperty(*body, "parent");
    ASSERT_NE(bodyParent, nullptr);
    ASSERT_EQ(bodyParent->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    const auto resolvedBodyParent = result.artifact.graph.ResolveObjectReference(
        bodyParent->value.GetObjectReference());
    ASSERT_TRUE(resolvedBodyParent.has_value());
    EXPECT_EQ(*resolvedBodyParent, hero->id);

    const auto* meshFilter = FindRecord(
        result.artifact.graph,
        "Body MeshFilter",
        "NLS::Engine::Components::MeshFilter");
    ASSERT_NE(meshFilter, nullptr);
    const auto* meshProperty = FindProperty(*meshFilter, "mesh");
    ASSERT_NE(meshProperty, nullptr);
    ASSERT_EQ(meshProperty->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(meshProperty->value.GetObjectReference().filePath, "mesh:mesh/body");

    const auto* meshRenderer = FindRecord(
        result.artifact.graph,
        "Body MeshRenderer",
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(meshRenderer, nullptr);
    EXPECT_EQ(FindProperty(*meshRenderer, "mesh"), nullptr);
    const auto* frustumBehaviour = FindProperty(*meshRenderer, "frustumBehaviour");
    ASSERT_NE(frustumBehaviour, nullptr);
    ASSERT_EQ(frustumBehaviour->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(frustumBehaviour->value.GetString(), "CULL_MODEL");

    const auto* materials = FindProperty(*meshRenderer, "materials");
    ASSERT_NE(materials, nullptr);
    ASSERT_EQ(materials->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(materials->value.GetArray().size(), 1u);
    ASSERT_EQ(
        materials->value.GetArray()[0].GetKind(),
        NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(
        materials->value.GetArray()[0].GetObjectReference().filePath,
        "material:converted-material/body");

    EXPECT_TRUE(ContainsResolvedAsset(result.artifact.resolvedAssets, "Mesh", "mesh:mesh/body"));
    EXPECT_TRUE(ContainsResolvedAsset(result.artifact.resolvedAssets, "Material", "material:converted-material/body"));
    EXPECT_EQ(FindResolvedAsset(result.artifact.resolvedAssets, "Model", "model:HeroScene"), nullptr);
    EXPECT_EQ(result.artifact.sourceToRuntimeObject.size(), result.artifact.graph.objects.size());
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabUsesSceneKeyForSingleParserRootNodeName)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("45454545-4545-4545-8545-454545454545"));
    scene.sceneKey = "NewSponza_Main_Yup_003";
    scene.nodes.push_back({"parser/node/0", "RootNode", "", "", ""});
    scene.nodes.push_back({"parser/node/1", "SponzaMesh", "parser/node/0", "parser/mesh/0", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "parser/material/21";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "parser/mesh/0";
    mesh.name = "SponzaMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"parser/material/21", "dirt_decal"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.primarySubAssetKey = "prefab:NewSponza_Main_Yup_003";
    manifest.subAssets.push_back({
        scene.sourceAssetId,
        "mesh:parser/mesh/0",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Sponza/mesh.nmesh",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        scene.sourceAssetId,
        "material:parser/material/21",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Sponza/dirt_decal.nmat",
        "material-hash"
    });

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.artifact.graph.Validate().HasErrors());

    const auto expectedRootId = MakeGeneratedModelPrefabObjectId(scene, "node:parser/node/0");
    const auto expectedRootTransformId = MakeGeneratedModelPrefabObjectId(scene, "component:parser/node/0:transform");
    EXPECT_EQ(result.artifact.graph.root, expectedRootId);
    EXPECT_NE(result.artifact.sourceToRuntimeObject.find(expectedRootId), result.artifact.sourceToRuntimeObject.end());
    EXPECT_NE(
        result.artifact.sourceToRuntimeObject.find(expectedRootTransformId),
        result.artifact.sourceToRuntimeObject.end());

    const auto* root = FindRecordById(result.artifact.graph, result.artifact.graph.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->id, expectedRootId);
    EXPECT_EQ(root->typeName, "NLS::Engine::GameObject");
    EXPECT_EQ(root->debugName, scene.sceneKey);

    const auto* rootTransform = FindRecordById(result.artifact.graph, expectedRootTransformId);
    ASSERT_NE(rootTransform, nullptr);
    EXPECT_EQ(rootTransform->debugName, scene.sceneKey + " Transform");

    const auto* rootName = FindProperty(*root, "name");
    ASSERT_NE(rootName, nullptr);
    ASSERT_EQ(rootName->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(rootName->value.GetString(), scene.sceneKey);
    EXPECT_EQ(FindRecord(result.artifact.graph, "RootNode", "NLS::Engine::GameObject"), nullptr);

    const auto* child = FindRecord(result.artifact.graph, "SponzaMesh", "NLS::Engine::GameObject");
    ASSERT_NE(child, nullptr);
    const auto* childParent = FindProperty(*child, "parent");
    ASSERT_NE(childParent, nullptr);
    ASSERT_EQ(childParent->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    const auto resolvedChildParent = result.artifact.graph.ResolveObjectReference(
        childParent->value.GetObjectReference());
    ASSERT_TRUE(resolvedChildParent.has_value());
    EXPECT_EQ(*resolvedChildParent, root->id);
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabKeepsChildParserRootNodeName)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("56565656-5656-4656-8656-565656565656"));
    scene.sceneKey = "AuthoredChildRootNode";
    scene.nodes.push_back({"parser/node/0", "AuthoredRoot", "", "", ""});
    scene.nodes.push_back({"parser/node/1", "RootNode", "parser/node/0", "", ""});

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        {});

    ASSERT_FALSE(result.diagnostics.HasErrors());
    ASSERT_FALSE(result.artifact.graph.Validate().HasErrors());

    const auto* root = FindRecordById(result.artifact.graph, result.artifact.graph.root);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->debugName, "AuthoredRoot");

    const auto* child = FindRecord(result.artifact.graph, "RootNode", "NLS::Engine::GameObject");
    ASSERT_NE(child, nullptr);
    const auto* childName = FindProperty(*child, "name");
    ASSERT_NE(childName, nullptr);
    ASSERT_EQ(childName->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::String);
    EXPECT_EQ(childName->value.GetString(), "RootNode");
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabInstantiationResolvesSubAssetHintsToArtifacts)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("90909090-9090-4090-8090-909090909090"));
    scene.sceneKey = "Hero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"material/body", "BodyMaterial"});

    const auto meshSubAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("91919191-9191-4191-8191-919191919191"));
    const auto materialSubAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929292"));

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        meshSubAssetId,
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Hero/meshes/body.nmesh",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        materialSubAssetId,
        "material:material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/materials/body.nmat",
        "material-hash"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(result.artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);

    auto* meshFilter = instance.root->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(meshFilter, nullptr);
    EXPECT_EQ(meshFilter->GetModelPath(), "Library/Artifacts/Hero/meshes/body.nmesh");

    auto* meshRenderer = instance.root->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(meshRenderer, nullptr);
    EXPECT_EQ(
        meshRenderer->GetFrustumBehaviour(),
        NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);

    const auto materials = meshRenderer->GetMaterialPaths();
    ASSERT_EQ(materials.size(), 1u);
    EXPECT_EQ(materials[0], "Library/Artifacts/Hero/materials/body.nmat");
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabPreservesSparseMaterialSlots)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("73737373-7373-4373-8373-737373737373"));
    scene.sceneKey = "SparseHero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "parser/material/1";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"parser/material/0", "UnusedMaterial"});
    scene.materials.push_back({"parser/material/1", "VisibleMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("74747474-7474-4474-8474-747474747474")),
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Sparse/meshes/body.nmesh",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("75757575-7575-4575-8575-757575757575")),
        "material:parser/material/1",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Sparse/materials/visible.nmat",
        "material-hash"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());

    const auto* meshRenderer = FindRecord(
        result.artifact.graph,
        "Body MeshRenderer",
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(meshRenderer, nullptr);
    const auto* materials = FindProperty(*meshRenderer, "materials");
    ASSERT_NE(materials, nullptr);
    ASSERT_EQ(materials->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Array);
    ASSERT_EQ(materials->value.GetArray().size(), 2u);
    ASSERT_EQ(materials->value.GetArray()[0].GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    EXPECT_FALSE(materials->value.GetArray()[0].GetObjectReference().IsValid());
    ASSERT_EQ(materials->value.GetArray()[1].GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference);
    EXPECT_EQ(
        materials->value.GetArray()[1].GetObjectReference().filePath,
        "material:parser/material/1");

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(result.artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    auto* runtimeMeshRenderer = instance.root->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(runtimeMeshRenderer, nullptr);
    const auto runtimeMaterialPaths = runtimeMeshRenderer->GetMaterialPaths();
    ASSERT_EQ(runtimeMaterialPaths.size(), 2u);
    EXPECT_TRUE(runtimeMaterialPaths[0].empty());
    EXPECT_EQ(runtimeMaterialPaths[1], "Library/Artifacts/Sparse/materials/visible.nmat");
}

TEST(AssetPrefabPipelineTests, PreparedPrefabFreshnessRejectsStaleKeyVersionsAndStamps)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required for prepared prefab freshness inspection.";
#else
    const auto key = MakePreparedPrefabFreshnessKey();
    const auto fresh = NLS::Editor::Assets::BuildPreparedPrefabCacheFreshnessRecordForTesting(key);
    ASSERT_TRUE(NLS::Editor::Assets::IsPreparedPrefabCacheFreshForTesting(fresh, key));

    auto staleArtifact = fresh;
    staleArtifact.prefabArtifactStamp = "prefab@stale";
    EXPECT_FALSE(NLS::Editor::Assets::IsPreparedPrefabCacheFreshForTesting(staleArtifact, key));

    auto staleDependency = fresh;
    staleDependency.dependencyStamp = "dependency@stale";
    EXPECT_FALSE(NLS::Editor::Assets::IsPreparedPrefabCacheFreshForTesting(staleDependency, key));

    auto staleReflection = fresh;
    ++staleReflection.reflectionSchemaVersion;
    EXPECT_FALSE(NLS::Editor::Assets::IsPreparedPrefabCacheFreshForTesting(staleReflection, key));

    auto staleImporter = fresh;
    ++staleImporter.prefabImporterVersion;
    EXPECT_FALSE(NLS::Editor::Assets::IsPreparedPrefabCacheFreshForTesting(staleImporter, key));

    auto staleSerialization = fresh;
    ++staleSerialization.serializationFormatVersion;
    EXPECT_FALSE(NLS::Editor::Assets::IsPreparedPrefabCacheFreshForTesting(staleSerialization, key));

    auto staleDependencyManifest = fresh;
    ++staleDependencyManifest.dependencyManifestVersion;
    EXPECT_FALSE(NLS::Editor::Assets::IsPreparedPrefabCacheFreshForTesting(staleDependencyManifest, key));
#endif
}

TEST(AssetPrefabPipelineTests, EditorRestartPreparedPrefabCacheHitSkipsPrefabGraphLoad)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to clear the imported prefab L1 hot cache.";
#else
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const ScopedPrefabPipelineTempRoot tempRoot("nullus_pipeline_l2_prepared_cache");
    const auto& root = tempRoot.Path();
    WriteTextFile(
        root / "Assets" / "Models" / "PipelinePreparedCacheHero.gltf",
        MakeSingleMeshGltf("PipelinePreparedCacheHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PipelinePreparedCacheHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PipelinePreparedCacheHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    NLS::Editor::Assets::UnifiedPrefabLoadRequest request;
    request.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/PipelinePreparedCacheHero.gltf",
        "prefab:PipelinePreparedCacheHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);
    request.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    request.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    request.ownerScopeId = "scene:pipeline-prepared-cache";
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    request.allowPending = false;

    NLS::Editor::Assets::EditorAssetDragDropBridge firstSessionBridge(root / "Assets");

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto coldLoad = firstSessionBridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(coldLoad.prefab, nullptr);
    ASSERT_TRUE(coldLoad.key.has_value());
    const auto coldRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(coldRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

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

    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
    ASSERT_EQ(NLS::Editor::Assets::GetImportedPrefabHotCacheEntryCountForTesting(), 0u);
    NLS::Editor::Assets::EditorAssetDragDropBridge secondSessionBridge(root / "Assets");

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto preparedLoad = secondSessionBridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(preparedLoad.prefab, nullptr);
    ASSERT_TRUE(preparedLoad.key.has_value());
    EXPECT_EQ(preparedLoad.key->runtimeCacheIdentity, coldLoad.key->runtimeCacheIdentity);
    EXPECT_EQ(preparedLoad.prefab->graph.root, coldLoad.prefab->graph.root);
    EXPECT_EQ(preparedLoad.prefab->graph.objects.size(), coldLoad.prefab->graph.objects.size());
    EXPECT_EQ(preparedLoad.prefab->resolvedAssets.size(), coldLoad.prefab->resolvedAssets.size());

    const auto preparedRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_TRUE(ContainsArtifactTelemetryStagePath(
        preparedRecords,
        ArtifactLoadTelemetryStage::CacheHit,
        cachePath))
        << "The editor-restart hit must come from Library/PreparedPrefabCache, not the L1 hot cache.";
    EXPECT_EQ(CountArtifactTelemetryStage(preparedRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 0u)
        << "An editor-restart L1 miss should reuse Library/PreparedPrefabCache instead of reparsing .nprefab.";
#endif
}

TEST(AssetPrefabPipelineTests, EditorRestartPreparedPrefabCacheRejectsStaleFreshnessRecord)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to clear the imported prefab L1 hot cache.";
#else
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const ScopedPrefabPipelineTempRoot tempRoot("nullus_pipeline_l2_prepared_stale");
    const auto& root = tempRoot.Path();
    WriteTextFile(
        root / "Assets" / "Models" / "PipelinePreparedStaleHero.gltf",
        MakeSingleMeshGltf("PipelinePreparedStaleHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PipelinePreparedStaleHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PipelinePreparedStaleHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    NLS::Editor::Assets::UnifiedPrefabLoadRequest request;
    request.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/PipelinePreparedStaleHero.gltf",
        "prefab:PipelinePreparedStaleHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);
    request.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    request.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    request.ownerScopeId = "scene:pipeline-prepared-stale";
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    request.allowPending = false;

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto coldLoad = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(coldLoad.prefab, nullptr);

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
    ASSERT_TRUE(cacheJson.is_object());
    ASSERT_TRUE(cacheJson.contains("prefabArtifactStamp"));
    cacheJson["prefabArtifactStamp"] = "stale-prefab-artifact-stamp";
    {
        std::ofstream cacheOutput(cachePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(cacheOutput.is_open());
        cacheOutput << cacheJson.dump();
    }

    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
    ASSERT_EQ(NLS::Editor::Assets::GetImportedPrefabHotCacheEntryCountForTesting(), 0u);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto reloaded = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(reloaded.prefab, nullptr);
    const auto reloadRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_FALSE(ContainsArtifactTelemetryStagePath(
        reloadRecords,
        ArtifactLoadTelemetryStage::CacheHit,
        cachePath))
        << "A stale Library/PreparedPrefabCache entry must not be accepted as an L2 hit.";
    EXPECT_GE(CountArtifactTelemetryStage(reloadRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u)
        << "A stale prepared prefab cache freshness record must be rejected after an editor-restart L1 miss.";
#endif
}

TEST(AssetPrefabPipelineTests, EditorRestartPreparedPrefabCacheRejectsMalformedFreshnessTypes)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to clear the imported prefab L1 hot cache.";
#else
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;

    const ScopedPrefabPipelineTempRoot tempRoot("nullus_pipeline_l2_prepared_malformed");
    const auto& root = tempRoot.Path();
    WriteTextFile(
        root / "Assets" / "Models" / "PipelinePreparedMalformedHero.gltf",
        MakeSingleMeshGltf("PipelinePreparedMalformedHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/Models/PipelinePreparedMalformedHero.gltf"));
    const auto guid = database.AssetPathToGUID("Assets/Models/PipelinePreparedMalformedHero.gltf");
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    NLS::Editor::Assets::UnifiedPrefabLoadRequest request;
    request.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        "Assets/Models/PipelinePreparedMalformedHero.gltf",
        "prefab:PipelinePreparedMalformedHero",
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);
    request.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    request.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    request.ownerScopeId = "scene:pipeline-prepared-malformed";
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    request.allowPending = false;

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto coldLoad = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(coldLoad.prefab, nullptr);

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
    ASSERT_TRUE(cacheJson.is_object());
    cacheJson["schema"] = "wrong-type";
    cacheJson["runtimeCacheIdentity"] = nlohmann::json::array();
    cacheJson["prefabImporterVersion"] =
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1ull;
    cacheJson["reflectionSchemaVersion"] = -1;
    cacheJson["serializationFormatVersion"] =
        static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1ll;
    {
        std::ofstream cacheOutput(cachePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(cacheOutput.is_open());
        cacheOutput << cacheJson.dump();
    }

    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
    ASSERT_EQ(NLS::Editor::Assets::GetImportedPrefabHotCacheEntryCountForTesting(), 0u);

    NLS::Base::Profiling::PerformanceStageStats stats;
    NLS::Base::Profiling::PerformanceStageStatsCapture capture(stats);
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto reloaded = bridge.LoadUnifiedPrefabShared(request);
    ASSERT_NE(reloaded.prefab, nullptr);
    const auto reloadRecords = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_FALSE(ContainsArtifactTelemetryStagePath(
        reloadRecords,
        ArtifactLoadTelemetryStage::CacheHit,
        cachePath))
        << "A malformed Library/PreparedPrefabCache freshness record must fall back instead of throwing.";
    EXPECT_GE(CountArtifactTelemetryStage(reloadRecords, ArtifactLoadTelemetryStage::PrefabGraphLoad), 1u);

    const auto snapshot = stats.Snapshot();
    const auto* loadPrepared = FindPrefabStage(snapshot, "LoadPreparedPrefabCache");
    ASSERT_NE(loadPrepared, nullptr);
    ASSERT_TRUE(loadPrepared->counters.contains("cacheMisses"));
    EXPECT_GE(loadPrepared->counters.at("cacheMisses"), 1u);
    EXPECT_FALSE(loadPrepared->counters.contains("cacheHits"));
#endif
}

TEST(AssetPrefabPipelineTests, FailedPreparedPrefabLoadDoesNotEnterHotOrDiskCache)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required to inspect imported prefab L1 hot cache state.";
#else
    const ScopedPrefabPipelineTempRoot tempRoot("nullus_pipeline_failed_prepared_cache");
    const auto& root = tempRoot.Path();
    constexpr const char* kAssetPath = "Assets/Models/PipelineBrokenPreparedHero.gltf";
    constexpr const char* kPrefabSubAssetKey = "prefab:PipelineBrokenPreparedHero";
    WriteTextFile(
        root / kAssetPath,
        MakeSingleMeshGltf("PipelineBrokenPreparedHeroRoot"));

    NLS::Editor::Assets::AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset(kAssetPath));
    const auto guid = database.AssetPathToGUID(kAssetPath);
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));

    const auto manifestPath = root / "Library" / "Artifacts" / guid / "manifest.json";
    std::ifstream manifestInput(manifestPath, std::ios::binary);
    ASSERT_TRUE(manifestInput.is_open());
    auto manifestJson = nlohmann::json::parse(manifestInput, nullptr, false);
    ASSERT_TRUE(manifestJson.is_object());

    std::filesystem::path prefabPath;
    for (const auto& subAsset : manifestJson.value("subAssets", nlohmann::json::array()))
    {
        if (subAsset.value("artifactType", std::string {}) == "prefab" &&
            subAsset.value("subAssetKey", std::string {}) == kPrefabSubAssetKey)
        {
            prefabPath = subAsset.value("artifactPath", std::string {});
            break;
        }
    }
    ASSERT_FALSE(prefabPath.empty());

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Prefab;
    metadata.schemaName = "prefab";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = assetId;
    metadata.subAssetKey = kPrefabSubAssetKey;
    metadata.displayName = "PipelineBrokenPreparedHero";
    metadata.importerId = "gltf";
    metadata.importerVersion = 1u;
    metadata.targetPlatform = "editor";
    const std::string invalidPayload = "{ not valid prefab graph";
    const auto brokenContainer = NLS::Core::Assets::WriteNativeArtifactContainer(
        std::move(metadata),
        std::vector<uint8_t>(invalidPayload.begin(), invalidPayload.end()));
    ASSERT_FALSE(brokenContainer.empty());
    WriteBytesFile(prefabPath, brokenContainer);

    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
    ASSERT_EQ(NLS::Editor::Assets::GetImportedPrefabHotCacheEntryCountForTesting(), 0u);

    NLS::Editor::Assets::UnifiedPrefabLoadRequest request;
    request.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        root,
        kAssetPath,
        kPrefabSubAssetKey,
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);
    request.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    request.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    request.ownerScopeId = "scene:pipeline-failed-prepared-cache";
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    request.allowPending = false;

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(root / "Assets");
    const auto failedLoad = bridge.LoadUnifiedPrefabShared(request);
    EXPECT_EQ(failedLoad.prefab, nullptr);
    EXPECT_EQ(NLS::Editor::Assets::GetImportedPrefabHotCacheEntryCountForTesting(), 0u);

    const auto cacheRoot = root / "Library" / "PreparedPrefabCache";
    size_t cacheFileCount = 0u;
    std::error_code error;
    if (std::filesystem::exists(cacheRoot, error) && !error)
    {
        for (const auto& entry : std::filesystem::directory_iterator(cacheRoot))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                ++cacheFileCount;
        }
    }
    EXPECT_EQ(cacheFileCount, 0u)
        << "Prepared prefab L2 cache must only persist successfully imported prefab graphs.";
#endif
}

TEST(AssetPrefabPipelineTests, PreparedPrefabMappingDependencyFingerprintReusesStampedCache)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required for prepared prefab cache scan counters.";
#else
    const ScopedPrefabPipelineTempRoot tempRoot("nullus_prepared_mapping_cache");
    const auto& root = tempRoot.Path();
    const auto textureGuid = NLS::Guid::Parse("13131313-1313-4313-8313-131313131313");
    const auto textureAssetId = NLS::Core::Assets::AssetId(textureGuid);
    WriteTextFile(root / "Assets" / "Textures" / "SharedWood.png", "png");
    auto textureMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(root / "Assets" / "Textures" / "SharedWood.png");
    textureMeta.id = textureAssetId;
    textureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(textureMeta.Save(root / "Assets" / "Textures" / "SharedWood.png.meta"));
    WriteTextFile(
        root / "Library" / "Artifacts" / textureAssetId.ToString() / "manifest.json",
        "{\n"
        "  \"sourceAssetId\": \"13131313-1313-4313-8313-131313131313\",\n"
        "  \"importerId\": \"texture\",\n"
        "  \"importerVersion\": 1,\n"
        "  \"targetPlatform\": \"editor\",\n"
        "  \"primarySubAssetKey\": \"texture:SharedWood\",\n"
        "  \"subAssets\": [\n"
        "    {\n"
        "      \"sourceAssetId\": \"13131313-1313-4313-8313-131313131313\",\n"
        "      \"subAssetKey\": \"texture:SharedWood\",\n"
        "      \"artifactType\": \"texture\",\n"
        "      \"loaderId\": \"texture\",\n"
        "      \"targetPlatform\": \"editor\",\n"
        "      \"artifactPath\": \"texture.ntex\",\n"
        "      \"contentHash\": \"hash:wood\"\n"
        "    }\n"
        "  ],\n"
        "  \"dependencies\": []\n"
        "}\n");
    NLS::Core::Assets::ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureAssetId;
    textureManifest.importerId = "texture";
    textureManifest.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    textureManifest.targetPlatform = "editor";
    textureManifest.primarySubAssetKey = "texture:SharedWood";
    textureManifest.subAssets.push_back({
        textureAssetId,
        "texture:SharedWood",
        NLS::Core::Assets::ArtifactType::Texture,
        "texture",
        "editor",
        "Library/Artifacts/13131313-1313-4313-8313-131313131313/texture.ntex",
        "hash:wood",
        "SharedWood"
    });
    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    artifactDatabase.UpsertManifest(
        textureManifest,
        "Assets/Textures/SharedWood.png",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB" / "index.tsv"));

    NLS::Editor::Assets::ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto dependencyValue =
        NLS::Editor::Assets::MakeModelTextureMappingDependencyValue("SharedWood", "name-search");

    const auto first = NLS::Editor::Assets::ComputeModelTextureMappingDependencyFingerprintForTesting(
        root,
        dependencyValue,
        "editor");
    ASSERT_TRUE(first.has_value());
    ASSERT_FALSE(first->empty());
    EXPECT_EQ(NLS::Editor::Assets::GetModelTextureMappingDependencyFingerprintScanCountForTesting(), 0u);

    const auto second = NLS::Editor::Assets::ComputeModelTextureMappingDependencyFingerprintForTesting(
        root,
        dependencyValue,
        "editor");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, *first);
    EXPECT_EQ(NLS::Editor::Assets::GetModelTextureMappingDependencyFingerprintScanCountForTesting(), 0u)
        << "Prepared prefab freshness checks should use ArtifactDB-backed fingerprints instead of "
           "rescanning the full Assets tree for repeated cache/key checks.";

    NLS::Editor::Assets::ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto batched = NLS::Editor::Assets::ComputeModelTextureMappingDependencyFingerprintsForTesting(
        root,
        {dependencyValue, dependencyValue},
        "editor");
    ASSERT_EQ(batched.size(), 2u);
    ASSERT_TRUE(batched[0].has_value());
    ASSERT_TRUE(batched[1].has_value());
    EXPECT_EQ(*batched[0], *first);
    EXPECT_EQ(*batched[1], *first);
    EXPECT_EQ(NLS::Editor::Assets::GetModelTextureMappingDependencyFingerprintScanCountForTesting(), 0u)
        << "Batched manifest dependency validation must keep the ArtifactDB fast path at zero recursive scans.";

    textureManifest.subAssets.front().contentHash = "hash:wood-updated";
    artifactDatabase.UpsertManifest(
        textureManifest,
        "Assets/Textures/SharedWood.png",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB" / "index.tsv"));
    const auto third = NLS::Editor::Assets::ComputeModelTextureMappingDependencyFingerprintForTesting(
        root,
        dependencyValue,
        "editor");
    ASSERT_TRUE(third.has_value());
    EXPECT_NE(*third, *first);
    EXPECT_EQ(NLS::Editor::Assets::GetModelTextureMappingDependencyFingerprintScanCountForTesting(), 0u)
        << "Mapping fingerprint cache hits and ArtifactDB-stamped invalidation must not rescan Assets.";

#endif
}

TEST(AssetPrefabPipelineTests, PreparedPrefabSourcePathMappingFallbackTracksTextureManifestStamp)
{
#if !defined(NLS_ENABLE_TEST_HOOKS)
    GTEST_SKIP() << "NLS_ENABLE_TEST_HOOKS is required for prepared prefab cache scan counters.";
#else
    const ScopedPrefabPipelineTempRoot tempRoot("nullus_prepared_mapping_source_path");
    const auto& root = tempRoot.Path();
    const auto textureGuid = NLS::Guid::Parse("15151515-1515-4515-8515-151515151515");
    const auto textureAssetId = NLS::Core::Assets::AssetId(textureGuid);
    const auto texturePath = root / "Assets" / "Textures" / "ExactBaseColor.png";
    WriteTextFile(texturePath, "png");
    auto textureMeta = NLS::Core::Assets::AssetMeta::CreateForAsset(texturePath);
    textureMeta.id = textureAssetId;
    textureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(textureMeta.Save(root / "Assets" / "Textures" / "ExactBaseColor.png.meta"));

    const auto manifestPath = root / "Library" / "Artifacts" / textureAssetId.ToString() / "manifest.json";
    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"sourceAssetId\": \"15151515-1515-4515-8515-151515151515\",\n"
        "  \"importerId\": \"texture\",\n"
        "  \"importerVersion\": 1,\n"
        "  \"targetPlatform\": \"editor\",\n"
        "  \"primarySubAssetKey\": \"texture:main\",\n"
        "  \"subAssets\": [\n"
        "    {\n"
        "      \"sourceAssetId\": \"15151515-1515-4515-8515-151515151515\",\n"
        "      \"subAssetKey\": \"texture:main\",\n"
        "      \"artifactType\": \"texture\",\n"
        "      \"loaderId\": \"texture\",\n"
        "      \"targetPlatform\": \"editor\",\n"
        "      \"artifactPath\": \"Library/Artifacts/15151515-1515-4515-8515-151515151515/texture.ntex\",\n"
        "      \"contentHash\": \"hash:exact-v1\"\n"
        "    }\n"
        "  ],\n"
        "  \"dependencies\": []\n"
        "}\n");

    NLS::Core::Assets::ArtifactDatabase artifactDatabase;
    ASSERT_TRUE(artifactDatabase.Save(root / "Library" / "ArtifactDB" / "index.tsv"));

    NLS::Editor::Assets::ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto dependencyValue =
        NLS::Editor::Assets::MakeModelTextureMappingDependencyValue(
            "Assets/Textures/ExactBaseColor.png",
            "source-path");

    const auto first = NLS::Editor::Assets::ComputeModelTextureMappingDependencyFingerprintForTesting(
        root,
        dependencyValue,
        "editor");
    ASSERT_TRUE(first.has_value());
    EXPECT_NE(first->find("hash:exact-v1"), std::string::npos);
    EXPECT_EQ(NLS::Editor::Assets::GetModelTextureMappingDependencyFingerprintScanCountForTesting(), 0u);

    WriteTextFile(
        manifestPath,
        "{\n"
        "  \"sourceAssetId\": \"15151515-1515-4515-8515-151515151515\",\n"
        "  \"importerId\": \"texture\",\n"
        "  \"importerVersion\": 1,\n"
        "  \"targetPlatform\": \"editor\",\n"
        "  \"primarySubAssetKey\": \"texture:main\",\n"
        "  \"subAssets\": [\n"
        "    {\n"
        "      \"sourceAssetId\": \"15151515-1515-4515-8515-151515151515\",\n"
        "      \"subAssetKey\": \"texture:main\",\n"
        "      \"artifactType\": \"texture\",\n"
        "      \"loaderId\": \"texture\",\n"
        "      \"targetPlatform\": \"editor\",\n"
        "      \"artifactPath\": \"Library/Artifacts/15151515-1515-4515-8515-151515151515/texture.ntex\",\n"
        "      \"contentHash\": \"hash:exact-v2\"\n"
        "    }\n"
        "  ],\n"
        "  \"dependencies\": []\n"
        "}\n");

    const auto second = NLS::Editor::Assets::ComputeModelTextureMappingDependencyFingerprintForTesting(
        root,
        dependencyValue,
        "editor");
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(*second, *first);
    EXPECT_NE(second->find("hash:exact-v2"), std::string::npos);
    EXPECT_EQ(NLS::Editor::Assets::GetModelTextureMappingDependencyFingerprintScanCountForTesting(), 0u);

#endif
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabCreatesInactiveHLODProxyNode)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("41414141-4141-4141-8141-414141414141"));
    scene.sceneKey = "HLODCityBlock";
    scene.nodes.push_back({"node/root", "Root", "", "", ""});
    scene.nodes.push_back({"node/cluster", "Cluster", "node/root", "", ""});
    scene.nodes.push_back({"node/childA", "ChildA", "node/cluster", "mesh/childA", ""});
    scene.nodes.push_back({"node/childB", "ChildB", "node/cluster", "mesh/childB", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitiveA;
    primitiveA.materialKey = "material/body";
    NLS::Render::Assets::ImportedScenePrimitive primitiveB;
    primitiveB.materialKey = "material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord childAMesh;
    childAMesh.sourceKey = "mesh/childA";
    childAMesh.name = "ChildAMesh";
    childAMesh.primitiveCount = 1u;
    childAMesh.primitives.push_back(std::move(primitiveA));
    scene.meshes.push_back(std::move(childAMesh));
    NLS::Render::Assets::ImportedSceneNamedRecord childBMesh;
    childBMesh.sourceKey = "mesh/childB";
    childBMesh.name = "ChildBMesh";
    childBMesh.primitiveCount = 1u;
    childBMesh.primitives.push_back(std::move(primitiveB));
    scene.meshes.push_back(std::move(childBMesh));
    scene.materials.push_back({"material/body", "BodyMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("42424242-4242-4242-8242-424242424242")),
        "mesh:mesh/childA",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/HLOD/childA.nmesh",
        "mesh-a-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("43434343-4343-4343-8343-434343434343")),
        "mesh:mesh/childB",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/HLOD/childB.nmesh",
        "mesh-b-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("44444444-4444-4444-8444-444444444444")),
        "material:material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/HLOD/body.nmat",
        "material-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("45454545-4545-4545-8545-454545454545")),
        "hlod-proxy:node/cluster",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/HLOD/cluster_proxy.nmesh",
        "proxy-hash"
    });

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    const auto* clusterRecord = FindRecord(
        result.artifact.graph,
        "Cluster",
        "NLS::Engine::GameObject");
    ASSERT_NE(clusterRecord, nullptr);
    const auto* hlod = FindProperty(*clusterRecord, NLS::Engine::Assets::GeneratedModelPrefabHLODSchema::PropertyName);
    ASSERT_NE(hlod, nullptr);
    const auto* proxyRecord = FindRecord(
        result.artifact.graph,
        "__HLODProxy_Cluster",
        "NLS::Engine::GameObject");
    ASSERT_NE(proxyRecord, nullptr);
    const auto* active = FindProperty(*proxyRecord, "active");
    ASSERT_NE(active, nullptr);
    ASSERT_EQ(active->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Bool);
    EXPECT_FALSE(active->value.GetBool());

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(result.artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    auto* proxyObject = runtimeScene.FindGameObjectByName("__HLODProxy_Cluster");
    ASSERT_NE(proxyObject, nullptr);
    EXPECT_FALSE(proxyObject->IsSelfActive());
    EXPECT_EQ(proxyObject->GetSourceObjectKey(), "hlod-proxy:node/cluster");
    ASSERT_NE(proxyObject->GetComponent<NLS::Engine::Components::MeshRenderer>(), nullptr);
    auto* proxyFilter = proxyObject->GetComponent<NLS::Engine::Components::MeshFilter>();
    ASSERT_NE(proxyFilter, nullptr);
    EXPECT_EQ(proxyFilter->GetModelPath(), "Library/Artifacts/HLOD/cluster_proxy.nmesh");
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabSplitsMultiPrimitiveMeshIntoPrimitiveRenderers)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("34343434-3434-4434-8434-343434343434"));
    scene.sceneKey = "MultiPrimitiveHero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive firstPrimitive;
    firstPrimitive.materialKey = "parser/material/0";
    NLS::Render::Assets::ImportedScenePrimitive secondPrimitive;
    secondPrimitive.materialKey = "parser/material/1";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitiveCount = 2u;
    mesh.primitives.push_back(std::move(firstPrimitive));
    mesh.primitives.push_back(std::move(secondPrimitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"parser/material/0", "FirstMaterial"});
    scene.materials.push_back({"parser/material/1", "SecondMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("35353535-3535-4535-8535-353535353535")),
        "mesh:mesh/body/primitive/0",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Multi/meshes/body_primitive_0.nmesh",
        "mesh-hash-0"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("36363636-3636-4636-8636-363636363636")),
        "mesh:mesh/body/primitive/1",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Multi/meshes/body_primitive_1.nmesh",
        "mesh-hash-1"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("37373737-3737-4737-8737-373737373737")),
        "material:parser/material/0",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Multi/materials/first.nmat",
        "material-hash-0"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("38383838-3838-4838-8838-383838383838")),
        "material:parser/material/1",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Multi/materials/second.nmat",
        "material-hash-1"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());

    const auto* body = FindRecord(result.artifact.graph, "Body", "NLS::Engine::GameObject");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(FindRecord(result.artifact.graph, "Body MeshFilter", "NLS::Engine::Components::MeshFilter"), nullptr);
    EXPECT_EQ(FindRecord(result.artifact.graph, "Body MeshRenderer", "NLS::Engine::Components::MeshRenderer"), nullptr);

    const auto* firstMeshFilter = FindRecord(
        result.artifact.graph,
        "Body Primitive 0 MeshFilter",
        "NLS::Engine::Components::MeshFilter");
    ASSERT_NE(firstMeshFilter, nullptr);
    const auto* firstMesh = FindProperty(*firstMeshFilter, "mesh");
    ASSERT_NE(firstMesh, nullptr);
    EXPECT_EQ(firstMesh->value.GetObjectReference().filePath, "mesh:mesh/body/primitive/0");

    const auto* firstMeshRenderer = FindRecord(
        result.artifact.graph,
        "Body Primitive 0 MeshRenderer",
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(firstMeshRenderer, nullptr);
    const auto* firstMaterials = FindProperty(*firstMeshRenderer, "materials");
    ASSERT_NE(firstMaterials, nullptr);
    ASSERT_EQ(firstMaterials->value.GetArray().size(), 1u);
    EXPECT_EQ(firstMaterials->value.GetArray()[0].GetObjectReference().filePath, "material:parser/material/0");

    const auto* secondMeshFilter = FindRecord(
        result.artifact.graph,
        "Body Primitive 1 MeshFilter",
        "NLS::Engine::Components::MeshFilter");
    ASSERT_NE(secondMeshFilter, nullptr);
    const auto* secondMesh = FindProperty(*secondMeshFilter, "mesh");
    ASSERT_NE(secondMesh, nullptr);
    EXPECT_EQ(secondMesh->value.GetObjectReference().filePath, "mesh:mesh/body/primitive/1");

    const auto* secondMeshRenderer = FindRecord(
        result.artifact.graph,
        "Body Primitive 1 MeshRenderer",
        "NLS::Engine::Components::MeshRenderer");
    ASSERT_NE(secondMeshRenderer, nullptr);
    const auto* secondMaterials = FindProperty(*secondMeshRenderer, "materials");
    ASSERT_NE(secondMaterials, nullptr);
    ASSERT_EQ(secondMaterials->value.GetArray().size(), 2u);
    EXPECT_FALSE(secondMaterials->value.GetArray()[0].GetObjectReference().IsValid());
    EXPECT_EQ(secondMaterials->value.GetArray()[1].GetObjectReference().filePath, "material:parser/material/1");

    EXPECT_TRUE(ContainsResolvedAsset(result.artifact.resolvedAssets, "Mesh", "mesh:mesh/body/primitive/0"));
    EXPECT_TRUE(ContainsResolvedAsset(result.artifact.resolvedAssets, "Mesh", "mesh:mesh/body/primitive/1"));
}

TEST(AssetPrefabPipelineTests, DeferredAssetResolutionIgnoresLegacyStringMaterialPaths)
{
    using namespace NLS::Engine::Serialize;

    const auto rootId = ObjectId(NLS::Guid::Parse("91919191-1111-4111-8111-111111111111"));
    const auto meshRendererId = ObjectId(NLS::Guid::Parse("92929292-2222-4222-8222-222222222222"));

    NLS::Engine::Assets::PrefabArtifact artifact;
    artifact.graph.documentId = NLS::Guid::Parse("93939393-3333-4333-8333-333333333333");
    artifact.graph.root = rootId;

    ObjectRecord root;
    root.id = rootId;
    root.localIdentifierInFile = MakeLocalIdentifierInFile(rootId);
    root.typeName = "NLS::Engine::GameObject";
    root.debugName = "Legacy Path Root";
    root.properties.push_back({"name", PropertyValue::String("Legacy Path Root")});
    root.properties.push_back({"components", PropertyValue::Array({
        PropertyValue::OwnedReference(meshRendererId)
    })});

    ObjectRecord meshRenderer;
    meshRenderer.id = meshRendererId;
    meshRenderer.localIdentifierInFile = MakeLocalIdentifierInFile(meshRendererId);
    meshRenderer.typeName = "NLS::Engine::Components::MeshRenderer";
    meshRenderer.debugName = "Legacy Path MeshRenderer";
    meshRenderer.properties.push_back({"materials", PropertyValue::Array({
        PropertyValue::String("Library/Artifacts/Legacy/material.nmat")
    })});

    artifact.graph.objects.push_back(std::move(root));
    artifact.graph.objects.push_back(std::move(meshRenderer));

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    auto* runtimeMeshRenderer = instance.root->GetComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(runtimeMeshRenderer, nullptr);
    EXPECT_TRUE(runtimeMeshRenderer->GetMaterialPaths().empty());
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabInstantiationMapsDeepImportedHierarchy)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("96969696-9696-4696-8696-969696969696"));
    scene.sceneKey = "DeepModel";
    scene.nodes.push_back({"node/floor", "Floor", "", "", ""});
    scene.nodes.push_back({"node/group", "Group", "node/floor", "", ""});
    scene.nodes.push_back({"node/mesh", "MeshNode", "node/group", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"material/body", "BodyMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("97979797-9797-4797-8797-979797979797")),
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Deep/meshes/body.nmesh",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("98989898-9898-4898-8898-989898989898")),
        "material:material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Deep/materials/body.nmat",
        "material-hash"
    });

    auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());

    NLS::Engine::SceneSystem::Scene runtimeScene;
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(result.artifact, runtimeScene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    ASSERT_EQ(instance.root->GetChildren().size(), 1u);
    auto* group = instance.root->GetChildren()[0];
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->GetChildren().size(), 1u);
    auto* meshNode = group->GetChildren()[0];
    ASSERT_NE(meshNode, nullptr);
    ASSERT_NE(meshNode->GetComponent<NLS::Engine::Components::MeshRenderer>(), nullptr);

    EXPECT_EQ(instance.sourceByInstanceObject.size(), 3u);
    EXPECT_NE(instance.sourceByInstanceObject.find(instance.root), instance.sourceByInstanceObject.end());
    EXPECT_NE(instance.sourceByInstanceObject.find(group), instance.sourceByInstanceObject.end());
    EXPECT_NE(instance.sourceByInstanceObject.find(meshNode), instance.sourceByInstanceObject.end());

    const auto& fastAccess = runtimeScene.GetFastAccessComponents();
    ASSERT_EQ(fastAccess.modelRenderers.size(), 1u);
    EXPECT_EQ(fastAccess.modelRenderers[0], meshNode->GetComponent<NLS::Engine::Components::MeshRenderer>());
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabValidationAllowsResolvedSubAssetIds)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("93939393-9393-4393-8393-939393939393"));
    scene.sceneKey = "Hero";
    scene.nodes.push_back({"node/body", "Body", "", "mesh/body", ""});

    NLS::Render::Assets::ImportedScenePrimitive primitive;
    primitive.materialKey = "material/body";
    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "mesh/body";
    mesh.name = "BodyMesh";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));
    scene.materials.push_back({"material/body", "BodyMaterial"});

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = scene.sourceAssetId;
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("94949494-9494-4494-8494-949494949494")),
        "mesh:mesh/body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Hero/meshes/body.nmesh",
        "mesh-hash"
    });
    manifest.subAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("95959595-9595-4595-8595-959595959595")),
        "material:material/body",
        NLS::Core::Assets::ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/materials/body.nmat",
        "material-hash"
    });

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        manifest);

    ASSERT_FALSE(result.diagnostics.HasErrors());
    EXPECT_FALSE(result.artifact.Validate().HasErrors());
}

TEST(AssetPrefabPipelineTests, GeneratedModelPrefabReportsSkinningAndMorphRuntimeCapabilityGaps)
{
    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("22222222-2222-4222-8222-222222222222"));
    scene.sceneKey = "AnimatedHero";
    scene.nodes.push_back({"node/root", "AnimatedHero", "", "mesh/body", "skin/hero"});
    scene.meshes.push_back({"mesh/body", "BodyMesh"});
    scene.skins.push_back({"skin/hero", "HeroSkin"});
    scene.animations.push_back({"anim/idle", "Idle"});
    scene.morphTargets.push_back({"morph/smile", "Smile"});

    const auto result = NLS::Engine::Assets::BuildGeneratedModelPrefab(
        scene,
        NLS::Render::Assets::GenerateSceneSubAssets(scene),
        NLS::Core::Assets::ArtifactManifest {});

    ASSERT_FALSE(result.diagnostics.GetItems().empty());

    bool foundSkinningGap = false;
    bool foundMorphGap = false;
    for (const auto& diagnostic : result.diagnostics.GetItems())
    {
        foundSkinningGap |= diagnostic.GetMessage().find("runtime-skinning-component-missing") != std::string::npos;
        foundMorphGap |= diagnostic.GetMessage().find("runtime-morph-component-missing") != std::string::npos;
    }

    EXPECT_TRUE(foundSkinningGap);
    EXPECT_TRUE(foundMorphGap);
}
