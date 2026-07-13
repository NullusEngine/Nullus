#include <gtest/gtest.h>

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetPath.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/SceneSystem/Scene.h"
#include "GameObject.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ServiceLocator.h"
#include "Profiling/PerformanceStageStats.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace NLS::Base::Profiling;

const PerformanceStageEntry* FindStage(
    const PerformanceStageStatsSnapshot& snapshot,
    const std::string& stageName);

class ScopedTempDirectory
{
public:
    explicit ScopedTempDirectory(const std::string& name)
        : m_path(std::filesystem::temp_directory_path() /
            (name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
        std::filesystem::create_directories(m_path, error);
    }

    ~ScopedTempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
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

template <typename T>
NLS::Engine::Serialize::PPtr<T> MakePPtr(const NLS::Engine::Serialize::ObjectIdentifier& identifier)
{
    return NLS::Engine::Serialize::PPtr<T>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));
}

struct SharedPrefabResourceReferences
{
    NLS::Engine::Serialize::ObjectIdentifier mesh;
    NLS::Engine::Serialize::ObjectIdentifier material;
};

SharedPrefabResourceReferences MakeSharedPrefabResourceReferences(
    const char* assetGuid,
    const char* meshPath,
    const char* materialPath)
{
    const auto assetId = NLS::Engine::Serialize::AssetId(NLS::Guid::Parse(assetGuid));
    return {
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            assetId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(assetId.GetGuid(), meshPath),
            meshPath),
        NLS::Engine::Serialize::ObjectIdentifier::Asset(
            assetId,
            NLS::Engine::Serialize::MakeLocalIdentifierInFile(assetId.GetGuid(), materialPath),
            materialPath)
    };
}

const PerformanceStageEntry* FindStage(
    const PerformanceStageStatsSnapshot& snapshot,
    const std::string& stageName)
{
    for (const auto& stage : snapshot.stages)
    {
        if (stage.domain == PerformanceStageDomain::Prefab && stage.stageName == stageName)
            return &stage;
    }
    return nullptr;
}

NLS::Engine::Assets::PrefabArtifact MakePrefabArtifact(
    const char* name,
    const char* assetGuid,
    const size_t objectCount = 1u,
    const size_t rendererEvery = 0u,
    const SharedPrefabResourceReferences* resourceReferences = nullptr)
{
    NLS::Engine::GameObject root(name, "Prefab");
    std::vector<NLS::Engine::GameObject*> childPointers;
    root.AddComponent<NLS::Engine::Components::LightComponent>()->SetIntensity(1.5f);
    if (rendererEvery == 1u)
    {
        auto* meshFilter = root.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = root.AddComponent<NLS::Engine::Components::MeshRenderer>();
        if (resourceReferences != nullptr)
        {
            meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(resourceReferences->mesh));
            meshRenderer->SetMaterialReferences({
                MakePPtr<NLS::Render::Resources::Material>(resourceReferences->material)
            });
        }
    }

    std::vector<std::unique_ptr<NLS::Engine::GameObject>> children;
    children.reserve(objectCount > 0u ? objectCount - 1u : 0u);
    for (size_t index = 1u; index < objectCount; ++index)
    {
        auto child = std::make_unique<NLS::Engine::GameObject>(
            std::string(name) + "_Child_" + std::to_string(index),
            "Prefab");
        if (rendererEvery > 0u && index % rendererEvery == 0u)
        {
            auto* meshFilter = child->AddComponent<NLS::Engine::Components::MeshFilter>();
            auto* meshRenderer = child->AddComponent<NLS::Engine::Components::MeshRenderer>();
            if (resourceReferences != nullptr)
            {
                meshFilter->SetMeshReference(MakePPtr<NLS::Render::Resources::Mesh>(resourceReferences->mesh));
                meshRenderer->SetMaterialReferences({
                    MakePPtr<NLS::Render::Resources::Material>(resourceReferences->material)
                });
            }
        }
        child->SetParent(root);
        childPointers.push_back(child.get());
        children.push_back(std::move(child));
    }

    const auto prefabDocument = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    for (auto iterator = childPointers.rbegin(); iterator != childPointers.rend(); ++iterator)
        (*iterator)->DetachFromParent();

    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(
            prefabDocument.graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid)));

    EXPECT_FALSE(importResult.diagnostics.HasErrors());
    return std::move(importResult.artifact);
}

PerformanceStageStatsSnapshot RunPrefabInstantiationScenario(
    NLS::Engine::Assets::PrefabArtifact artifact,
    std::chrono::microseconds* scenarioElapsed = nullptr)
{
    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    const auto scenarioBegin = std::chrono::steady_clock::now();
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);
    if (scenarioElapsed != nullptr)
    {
        *scenarioElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - scenarioBegin);
    }

    EXPECT_FALSE(instance.diagnostics.HasErrors());
    EXPECT_NE(instance.root, nullptr);
    return stats.Snapshot();
}

const NLS::Engine::Serialize::ObjectRecord* FindObjectRecordByType(
    const NLS::Engine::Serialize::ObjectGraphDocument& document,
    std::string_view typeName)
{
    for (const auto& record : document.objects)
    {
        if (record.typeName == typeName)
            return &record;
    }
    return nullptr;
}

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    std::string_view propertyName)
{
    for (const auto& property : record.properties)
    {
        if (property.name == propertyName)
            return &property;
    }
    return nullptr;
}

std::string ReadRepositoryTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void WriteTextFileForTest(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string ExtractJsonStringFieldForTest(const std::string& text, const std::string& key)
{
    const auto keyToken = "\"" + key + "\"";
    const auto keyPosition = text.find(keyToken);
    if (keyPosition == std::string::npos)
        return {};

    const auto colon = text.find(':', keyPosition + keyToken.size());
    if (colon == std::string::npos)
        return {};

    const auto valueBeginQuote = text.find('"', colon + 1u);
    if (valueBeginQuote == std::string::npos)
        return {};

    const auto valueEndQuote = text.find('"', valueBeginQuote + 1u);
    if (valueEndQuote == std::string::npos)
        return {};

    return text.substr(valueBeginQuote + 1u, valueEndQuote - valueBeginQuote - 1u);
}

std::string ComputeTestContentHash(const std::string& text)
{
    const auto* begin = reinterpret_cast<const uint8_t*>(text.data());
    return NLS::Core::Assets::ComputeNativeArtifactPayloadHash(begin, text.size());
}

std::string ComputeTestFileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return {};

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return {};

    return std::to_string(size) + ":" +
        std::to_string(static_cast<std::intmax_t>(writeTime.time_since_epoch().count()));
}

std::string MakeIndexedTextureGuid(const size_t index)
{
    std::ostringstream guid;
    guid << "60000000-0000-4000-8000-"
        << std::setfill('0') << std::setw(12) << index;
    return guid.str();
}

std::string MakeIndexedTextureArtifactPath(const size_t index)
{
    std::ostringstream fileName;
    fileName << "ab" << std::hex << std::setfill('0') << std::setw(62) << index;
    return "Library/Artifacts/ab/" + fileName.str();
}

uint64_t SourceHashContentReadsForFreshnessCheck(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetMeta& meta,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& assetPath,
    bool* current)
{
    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        *current = NLS::Editor::Assets::ManifestDependenciesAreCurrentForTesting(
            manifest,
            meta,
            projectRoot,
            assetPath);
    }

    const auto snapshot = stats.Snapshot();
    const auto* stage = FindStage(snapshot, "ManifestDependenciesAreCurrent");
    EXPECT_NE(stage, nullptr);
    if (stage == nullptr || !stage->counters.contains("sourceHashContentReads"))
        return 0u;
    return stage->counters.at("sourceHashContentReads");
}

NLS::Engine::Serialize::ObjectRecord* FindMutableObjectRecordByType(
    NLS::Engine::Serialize::ObjectGraphDocument& document,
    std::string_view typeName)
{
    for (auto& record : document.objects)
    {
        if (record.typeName == typeName)
            return &record;
    }
    return nullptr;
}

NLS::Engine::Serialize::PropertyRecord* FindMutableProperty(
    NLS::Engine::Serialize::ObjectRecord& record,
    std::string_view propertyName)
{
    for (auto& property : record.properties)
    {
        if (property.name == propertyName)
            return &property;
    }
    return nullptr;
}

}

TEST(AssetPrefabBehaviorTests, PrefabImportAndInstantiateEmitDiagnosticStages)
{
    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    auto artifact = MakePrefabArtifact(
        "ProfiledPrefab",
        "10101010-1010-4010-8010-101010101010");

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);
    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);

    const auto snapshot = stats.Snapshot();
    ASSERT_NE(FindStage(snapshot, "ParsePreparedPrefab"), nullptr);
    ASSERT_NE(FindStage(snapshot, "ResolveDependencies"), nullptr);
    ASSERT_NE(FindStage(snapshot, "TotalInstantiate"), nullptr);
    ASSERT_NE(FindStage(snapshot, "AllocateInstanceObjects"), nullptr);
    ASSERT_NE(FindStage(snapshot, "DeserializeComponents"), nullptr);
    ASSERT_NE(FindStage(snapshot, "FixupInternalReferences"), nullptr);
    ASSERT_NE(FindStage(snapshot, "ResolveExternalReferences"), nullptr);
    ASSERT_NE(FindStage(snapshot, "RegisterRenderers"), nullptr);
    ASSERT_NE(FindStage(snapshot, "RegisterPhysics"), nullptr);
    ASSERT_NE(FindStage(snapshot, "RegisterScripts"), nullptr);
    ASSERT_NE(FindStage(snapshot, "InvokeLifecycle"), nullptr);
    EXPECT_EQ(FindStage(snapshot, "WaitForResources"), nullptr);
    EXPECT_EQ(FindStage(snapshot, "UploadGpuResources"), nullptr);

    const auto* total = FindStage(snapshot, "TotalInstantiate");
    ASSERT_NE(total, nullptr);
    EXPECT_GE(total->counters.at("objectCount"), 1u);

    const auto* deserialize = FindStage(snapshot, "DeserializeComponents");
    ASSERT_NE(deserialize, nullptr);
    EXPECT_GE(deserialize->counters.at("componentCount"), 1u);

    const auto* externalReferences = FindStage(snapshot, "ResolveExternalReferences");
    ASSERT_NE(externalReferences, nullptr);
    ASSERT_TRUE(externalReferences->counters.contains("dependencyCount"));
}

TEST(AssetPrefabBehaviorTests, PrefabImportMovesParsedGraphIntoArtifact)
{
    const auto source = ReadRepositoryTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Assets/PrefabAsset.cpp");
    const auto functionStart = source.find("PrefabImportResult ImportPrefabArtifact(");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("PrefabArtifactInstantiationResult InstantiatePrefabArtifact", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("result.artifact.graph = std::move(*document);"), std::string::npos)
        << "Large prefab import must move the parsed ObjectGraphDocument into the artifact instead of deep-copying it.";
    EXPECT_EQ(body.find("result.artifact.graph = *document;"), std::string::npos)
        << "Copying the parsed graph adds avoidable LoadPrefabArtifact cost for NewSponza-scale prefabs.";
}

TEST(AssetPrefabBehaviorTests, PrefabDependencyResolutionUsesIndexedExistingAssetsAndDedup)
{
    const auto source = ReadRepositoryTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Assets/PrefabAsset.cpp");
    const auto functionStart = source.find("std::vector<PrefabResolvedAsset> BuildPrefabResolvedAssetsFromReferences(");
    ASSERT_NE(functionStart, std::string::npos);
    const auto functionEnd = source.find("void RefreshPrefabResolvedAssetsFromReferences", functionStart);
    ASSERT_NE(functionEnd, std::string::npos);
    const auto body = source.substr(functionStart, functionEnd - functionStart);

    EXPECT_NE(body.find("BuildPrefabResolvedAssetIndex(existingResolvedAssets)"), std::string::npos)
        << "Large prefab dependency resolution should index imported sub-assets once instead of scanning them per reference.";
    EXPECT_NE(body.find("std::unordered_set<PrefabResolvedAssetKey"), std::string::npos)
        << "Large prefab dependency resolution should deduplicate with a hash set instead of scanning the output vector.";
    EXPECT_EQ(body.find("FindExistingResolvedAssetForReference(existingResolvedAssets, reference)"), std::string::npos)
        << "Per-reference linear scans over imported sub-assets are too expensive for NewSponza-scale prefabs.";
    EXPECT_EQ(body.find("ContainsResolvedAssetReference(resolvedAssets, resolvedAsset)"), std::string::npos)
        << "Per-reference linear output deduplication is avoidable for large prefab graphs.";
}

TEST(AssetPrefabBehaviorTests, PrefabArtifactLoadTrustsManifestResolvedAssets)
{
    const auto prefabSource = ReadRepositoryTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Assets/PrefabAsset.cpp");
    const auto optionsParameter = prefabSource.find("PrefabImportOptions options)");
    ASSERT_NE(optionsParameter, std::string::npos);
    const auto importStart = prefabSource.rfind("PrefabImportResult ImportPrefabArtifact(", optionsParameter);
    ASSERT_NE(importStart, std::string::npos);
    const auto importEnd = prefabSource.find("PrefabArtifactInstantiationResult InstantiatePrefabArtifact", importStart);
    ASSERT_NE(importEnd, std::string::npos);
    const auto importBody = prefabSource.substr(importStart, importEnd - importStart);
    EXPECT_NE(importBody.find("if (!options.trustResolvedAssets)"), std::string::npos)
        << "Manifest-backed prefab loads should be able to skip graph-wide dependency rebuilding.";

    const auto databaseSource = ReadRepositoryTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/AssetDatabaseFacade.cpp");
    EXPECT_NE(
        databaseSource.find("trustResolvedAssets = record->assetType == NLS::Core::Assets::AssetType::ModelScene"),
        std::string::npos)
        << "AssetDatabaseFacade should only trust manifest resolved assets for generated model prefabs.";
    EXPECT_NE(
        databaseSource.find("trustResolvedAssets = manifestCopy.importerId == \"scene-model\""),
        std::string::npos)
        << "Persisted manifest prefab loads should only skip dependency rebuilds for scene-model artifacts.";

    const auto bridgeSource = ReadRepositoryTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp");
    EXPECT_NE(
        bridgeSource.find("trustResolvedAssets = assetType == NLS::Core::Assets::AssetType::ModelScene"),
        std::string::npos)
        << "Imported prefab hot-cache loads should only trust manifest resolved assets for generated model prefabs.";
}

TEST(AssetPrefabBehaviorTests, GeneratedModelPrefabArtifactLoadSkipsRepeatedGraphValidation)
{
    const auto prefabSource = ReadRepositoryTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Engine/Assets/PrefabAsset.cpp");
    const auto optionsParameter = prefabSource.find("PrefabImportOptions options)");
    ASSERT_NE(optionsParameter, std::string::npos);
    const auto importStart = prefabSource.rfind("PrefabImportResult ImportPrefabArtifact(", optionsParameter);
    ASSERT_NE(importStart, std::string::npos);
    const auto importEnd = prefabSource.find("PrefabArtifactInstantiationResult InstantiatePrefabArtifact", importStart);
    ASSERT_NE(importEnd, std::string::npos);
    const auto importBody = prefabSource.substr(importStart, importEnd - importStart);
    EXPECT_NE(importBody.find("const bool hasTrustedGraphValidation"), std::string::npos)
        << "Trusted generated model prefab artifacts should gate validation skips on a derived trust decision.";
    EXPECT_NE(
        importBody.find("options.trustedGraphValidationFingerprint == std::to_string(artifactFingerprint)"),
        std::string::npos)
        << "Generated prefab validation skips must require a proof matching the current graph and resolved assets.";
    EXPECT_NE(importBody.find("if (hasTrustedGraphValidation)"), std::string::npos)
        << "Trusted generated model prefab artifacts should skip repeated full graph validation only after proof matching.";
    EXPECT_NE(importBody.find("graphValidationTrusted"), std::string::npos)
        << "Trusted validation skips should be visible in prefab load telemetry.";

    const auto databaseSource = ReadRepositoryTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/AssetDatabaseFacade.cpp");
    EXPECT_NE(
        databaseSource.find("trustGraphValidation = record->assetType == NLS::Core::Assets::AssetType::ModelScene"),
        std::string::npos)
        << "AssetDatabaseFacade path-based loads should only trust generated model prefab graph validation.";
    EXPECT_NE(
        databaseSource.find("trustGraphValidation = manifestCopy.importerId == \"scene-model\""),
        std::string::npos)
        << "AssetDatabaseFacade asset-id loads should only trust scene-model graph validation.";
    EXPECT_NE(
        databaseSource.find("FindPrefabValidationProofFingerprint"),
        std::string::npos)
        << "AssetDatabaseFacade should pass the prefab container validation proof into graph-validation trust decisions.";
    EXPECT_NE(
        databaseSource.find("trustedGraphValidationFingerprint = validationProof"),
        std::string::npos)
        << "AssetDatabaseFacade should require matching prefab validation proof metadata before graph-validation skips.";

    const auto bridgeSource = ReadRepositoryTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Project/Editor/Assets/EditorAssetDragDropBridge.cpp");
    EXPECT_NE(
        bridgeSource.find("trustGraphValidation = assetType == NLS::Core::Assets::AssetType::ModelScene"),
        std::string::npos)
        << "Imported prefab hot-cache loads should only trust generated model prefab graph validation.";
    EXPECT_NE(
        bridgeSource.find("trustedGraphValidationFingerprint = validationProof"),
        std::string::npos)
        << "Imported prefab hot-cache loads should require matching prefab validation proof metadata before graph-validation skips.";
}

TEST(AssetPrefabBehaviorTests, TrustedGraphValidationPrimesValidationCache)
{
    NLS::Engine::GameObject root("TrustedGeneratedPrefab", "Prefab");
    const auto prefabDocument = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    NLS::Engine::Assets::PrefabArtifact proofArtifact;
    proofArtifact.graph = prefabDocument.graph;
    const auto validationProof = std::to_string(
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(proofArtifact));

    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(prefabDocument.graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("31313131-3131-4131-8131-313131313131")),
        {},
        NLS::Engine::Assets::PrefabImportOptions {
            .trustResolvedAssets = true,
            .trustGraphValidation = true,
            .trustedGraphValidationFingerprint = validationProof
        });
    ASSERT_FALSE(importResult.diagnostics.HasErrors());

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    const auto diagnostics = importResult.artifact.Validate();

    ASSERT_FALSE(diagnostics.HasErrors());
    const auto snapshot = stats.Snapshot();
    const auto* validation = FindStage(snapshot, "ValidatePrefabArtifact");
    ASSERT_NE(validation, nullptr);
    ASSERT_TRUE(validation->counters.contains("validationCacheHitCount"));
    EXPECT_EQ(validation->counters.at("validationCacheHitCount"), 1u)
        << "Trusted generated model prefab loads should prime validation diagnostics so first instantiation does not rerun full graph validation.";
    EXPECT_FALSE(validation->counters.contains("validationCacheMissCount"));
}

TEST(AssetPrefabBehaviorTests, PreparedCacheValidationFingerprintPrimesValidationCache)
{
    NLS::Engine::GameObject root("PreparedValidationCacheHero", "Prefab");
    const auto prefabDocument = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(prefabDocument.graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("53535353-5353-4353-8353-535353535353")));
    ASSERT_FALSE(importResult.diagnostics.HasErrors());

    const auto validationFingerprint =
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(importResult.artifact);
    const auto firstDiagnostics = importResult.artifact.Validate(validationFingerprint);
    ASSERT_FALSE(firstDiagnostics.HasErrors());

    PerformanceStageStats stats;
    NLS::Base::Profiling::PerformanceStageStatsCapture capture(stats);
    const auto secondDiagnostics = importResult.artifact.Validate(validationFingerprint);

    ASSERT_FALSE(secondDiagnostics.HasErrors());
    const auto snapshot = stats.Snapshot();
    const auto* validation = FindStage(snapshot, "ValidatePrefabArtifact");
    ASSERT_NE(validation, nullptr);
    ASSERT_TRUE(validation->counters.contains("validationCacheHitCount"));
    EXPECT_EQ(validation->counters.at("validationCacheHitCount"), 1u)
        << "Prepared-cache validation should use the same validation fingerprint it stores in the cache.";
}

TEST(AssetPrefabBehaviorTests, ManifestDependencyFreshnessCachesSourceFileContentHashes)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Core::Assets;

    ScopedTempDirectory temp("NullusSourceHashCache");
    const auto projectRoot = temp.Path();
    const auto assetPath = projectRoot / "Assets" / "Models" / "CachedHero.gltf";
    const auto metaPath = GetAssetMetaPath(assetPath);
    const std::string sourceText = R"({"asset":{"version":"2.0"},"scene":0})";
    WriteTextFileForTest(assetPath, sourceText);
    WriteTextFileForTest(
        metaPath,
        "guid=51515151515141518151515151515151\nimporter=scene-model\nversion=" +
            std::to_string(GetCurrentImporterVersion(AssetType::ModelScene)) + "\n");

    AssetMeta meta;
    meta.id = AssetId(NLS::Guid::Parse("51515151-5151-4151-8151-515151515151"));
    meta.assetType = AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = GetCurrentImporterVersion(AssetType::ModelScene);

    ArtifactManifest manifest;
    manifest.sourceAssetId = meta.id;
    manifest.importerId = meta.importerId;
    manifest.importerVersion = meta.importerVersion;
    manifest.targetPlatform = "editor";
    manifest.dependencies = {
        {
            AssetDependencyKind::SourceFileHash,
            "Assets/Models/CachedHero.gltf",
            ComputeTestContentHash(sourceText)
        },
        {
            AssetDependencyKind::PathToGuidMapping,
            "Assets/Models/CachedHero.gltf.meta",
            ComputeTestFileStamp(metaPath)
        }
    };

    bool current = false;
    EXPECT_EQ(
        SourceHashContentReadsForFreshnessCheck(manifest, meta, projectRoot, assetPath, &current),
        1u);
    EXPECT_TRUE(current);

    EXPECT_EQ(
        SourceHashContentReadsForFreshnessCheck(manifest, meta, projectRoot, assetPath, &current),
        0u)
        << "Repeated prefab freshness checks should reuse the cached content hash when the source file stamp is unchanged.";
    EXPECT_TRUE(current);

    const std::string changedSourceText = sourceText + " ";
    WriteTextFileForTest(assetPath, changedSourceText);
    std::error_code error;
    std::filesystem::last_write_time(
        assetPath,
        std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2),
        error);
    ASSERT_FALSE(error) << error.message();

    EXPECT_EQ(
        SourceHashContentReadsForFreshnessCheck(manifest, meta, projectRoot, assetPath, &current),
        1u)
        << "A source stamp change must force a fresh content hash so stale manifests are rejected.";
    EXPECT_FALSE(current);
}

TEST(AssetPrefabBehaviorTests, ManifestDependencyFreshnessRestoresSourceHashCacheFromDisk)
{
    using namespace NLS::Editor::Assets;
    using namespace NLS::Core::Assets;

    ScopedTempDirectory temp("NullusPersistentSourceHashCache");
    const auto projectRoot = temp.Path();
    const auto assetPath = projectRoot / "Assets" / "Models" / "PersistentHero.gltf";
    const auto metaPath = GetAssetMetaPath(assetPath);
    const std::string sourceText = R"({"asset":{"version":"2.0"},"scene":0})";
    WriteTextFileForTest(assetPath, sourceText);
    WriteTextFileForTest(
        metaPath,
        "guid=52525252525242528252525252525252\nimporter=scene-model\nversion=" +
            std::to_string(GetCurrentImporterVersion(AssetType::ModelScene)) + "\n");

    AssetMeta meta;
    meta.id = AssetId(NLS::Guid::Parse("52525252-5252-4252-8252-525252525252"));
    meta.assetType = AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = GetCurrentImporterVersion(AssetType::ModelScene);

    ArtifactManifest manifest;
    manifest.sourceAssetId = meta.id;
    manifest.importerId = meta.importerId;
    manifest.importerVersion = meta.importerVersion;
    manifest.targetPlatform = "editor";
    manifest.dependencies = {
        {
            AssetDependencyKind::SourceFileHash,
            "Assets/Models/PersistentHero.gltf",
            ComputeTestContentHash(sourceText)
        },
        {
            AssetDependencyKind::PathToGuidMapping,
            "Assets/Models/PersistentHero.gltf.meta",
            ComputeTestFileStamp(metaPath)
        }
    };

    ClearSourceFileHashCacheForTesting();
    RememberManifestSourceFileHashes(projectRoot, manifest);
    ClearSourceFileHashCacheForTesting();

    bool current = false;
    EXPECT_EQ(
        SourceHashContentReadsForFreshnessCheck(manifest, meta, projectRoot, assetPath, &current),
        0u)
        << "Prepared prefab loads after restart should validate unchanged source dependencies from the persisted hash cache.";
    EXPECT_TRUE(current);
}

TEST(AssetPrefabBehaviorTests, ManifestDependencyFreshnessRejectsSameSizeSameMtimeSourceReplacementOnWindows)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "Windows file-identity stamps are required for this cache invalidation edge case.";
#else
    using namespace NLS::Editor::Assets;
    using namespace NLS::Core::Assets;

    ScopedTempDirectory temp("NullusSourceHashWindowsStamp");
    const auto projectRoot = temp.Path();
    const auto assetPath = projectRoot / "Assets" / "Models" / "SameStampHero.gltf";
    const auto metaPath = GetAssetMetaPath(assetPath);
    const std::string sourceText = R"({"asset":{"version":"2.0"},"scene":0})";
    const std::string changedSourceText = R"({"asset":{"version":"2.0"},"scene":1})";
    ASSERT_EQ(sourceText.size(), changedSourceText.size());
    WriteTextFileForTest(assetPath, sourceText);
    WriteTextFileForTest(
        metaPath,
        "guid=54545454545444548454545454545454\nimporter=scene-model\nversion=" +
            std::to_string(GetCurrentImporterVersion(AssetType::ModelScene)) + "\n");
    const auto originalWriteTime = std::filesystem::last_write_time(assetPath);

    AssetMeta meta;
    meta.id = AssetId(NLS::Guid::Parse("54545454-5454-4454-8454-545454545454"));
    meta.assetType = AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = GetCurrentImporterVersion(AssetType::ModelScene);

    ArtifactManifest manifest;
    manifest.sourceAssetId = meta.id;
    manifest.importerId = meta.importerId;
    manifest.importerVersion = meta.importerVersion;
    manifest.targetPlatform = "editor";
    manifest.dependencies = {
        {
            AssetDependencyKind::SourceFileHash,
            "Assets/Models/SameStampHero.gltf",
            ComputeTestContentHash(sourceText)
        },
        {
            AssetDependencyKind::PathToGuidMapping,
            "Assets/Models/SameStampHero.gltf.meta",
            ComputeTestFileStamp(metaPath)
        }
    };

    ClearSourceFileHashCacheForTesting();
    bool current = false;
    EXPECT_EQ(
        SourceHashContentReadsForFreshnessCheck(manifest, meta, projectRoot, assetPath, &current),
        1u);
    EXPECT_TRUE(current);

    const auto originalCacheStamp = BuildSourceFileHashCacheFileStamp(assetPath);
    const auto replacementPath = assetPath.string() + ".replacement";
    WriteTextFileForTest(replacementPath, changedSourceText);
    std::error_code error;
    std::filesystem::last_write_time(replacementPath, originalWriteTime, error);
    ASSERT_FALSE(error) << error.message();
    std::filesystem::remove(assetPath, error);
    ASSERT_FALSE(error) << error.message();
    std::filesystem::rename(replacementPath, assetPath, error);
    ASSERT_FALSE(error) << error.message();
    ASSERT_NE(BuildSourceFileHashCacheFileStamp(assetPath), originalCacheStamp)
        << "Replacing the source must produce a distinct Windows file-identity stamp.";

    EXPECT_EQ(
        SourceHashContentReadsForFreshnessCheck(manifest, meta, projectRoot, assetPath, &current),
        1u)
        << "Windows file identity must invalidate same-size source replacements that preserve mtime.";
    EXPECT_FALSE(current);
#endif
}

TEST(AssetPrefabBehaviorTests, ModelTextureMappingDependencyBatchUsesArtifactDatabaseLookupIndex)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    ScopedTempDirectory temp("NullusTextureMappingLookupIndex");
    const auto projectRoot = temp.Path();
    const auto databasePath = projectRoot / "Library" / "ArtifactDB";

    ArtifactDatabase database;
    std::vector<std::string> dependencyValues;
    constexpr size_t kTextureCount = 128u;
    dependencyValues.reserve(kTextureCount);
    for (size_t index = 0u; index < kTextureCount; ++index)
    {
        const auto textureName = "IndexedTexture" + std::to_string(index);

        ArtifactManifest manifest;
        manifest.sourceAssetId = AssetId(NLS::Guid::Parse(MakeIndexedTextureGuid(index)));
        manifest.importerId = "texture";
        manifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
        manifest.targetPlatform = "editor";
        manifest.primarySubAssetKey = "texture:" + textureName;
        manifest.subAssets.push_back({
            manifest.sourceAssetId,
            "texture:" + textureName,
            ArtifactType::Texture,
            "texture",
            "editor",
            MakeIndexedTextureArtifactPath(index),
            "hash-" + textureName,
            textureName
        });

        database.UpsertManifest(
            manifest,
            "Assets/Textures/" + textureName + ".png",
            ArtifactRecordStatus::UpToDate);
        dependencyValues.push_back("project|" + textureName + "|name-search");
    }
    ASSERT_TRUE(database.Save(databasePath));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    PerformanceStageStats stats;
    std::vector<std::optional<std::string>> fingerprints;
    {
        PerformanceStageStatsCapture capture(stats);
        fingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
            projectRoot,
            dependencyValues,
            "editor");
    }

    ASSERT_EQ(fingerprints.size(), kTextureCount);
    for (size_t index = 0u; index < fingerprints.size(); ++index)
    {
        const auto textureName = "IndexedTexture" + std::to_string(index);
        ASSERT_TRUE(fingerprints[index].has_value()) << textureName;
        EXPECT_NE(fingerprints[index]->find("texture:" + textureName), std::string::npos)
            << textureName;
        EXPECT_NE(fingerprints[index]->find("hash-" + textureName), std::string::npos)
            << textureName;
        EXPECT_NE(fingerprints[index]->find("Assets/Textures/" + textureName + ".png"), std::string::npos)
            << textureName;
    }

    const auto snapshot = stats.Snapshot();
    const auto* batch = FindStage(snapshot, "ComputeModelTextureMappingDependencyFingerprintBatch");
    ASSERT_NE(batch, nullptr);
    ASSERT_TRUE(batch->counters.contains("artifactDatabaseHitCount"));
    EXPECT_EQ(batch->counters.at("artifactDatabaseHitCount"), kTextureCount);
    ASSERT_TRUE(batch->counters.contains("artifactDatabaseMissCount"));
    EXPECT_EQ(batch->counters.at("artifactDatabaseMissCount"), 0u);
    ASSERT_TRUE(batch->counters.contains("artifactDatabaseCandidateScanCount"));
    EXPECT_EQ(batch->counters.at("artifactDatabaseCandidateScanCount"), kTextureCount)
        << "Batch texture mapping validation should use an ArtifactDB lookup index instead of scanning every texture candidate for every dependency.";
    ASSERT_TRUE(batch->counters.contains("fingerprintCacheKeyBuildCount"));
    EXPECT_EQ(batch->counters.at("fingerprintCacheKeyBuildCount"), 0u)
        << "Cold startup ArtifactDB hits should not build per-dependency fingerprint cache keys before the process cache can contain entries.";
    ASSERT_TRUE(batch->counters.contains("nameSearchFallbackCount"));
    EXPECT_EQ(batch->counters.at("nameSearchFallbackCount"), 0u);
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u);

    PerformanceStageStats cachedStats;
    {
        PerformanceStageStatsCapture capture(cachedStats);
        const auto cachedFingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
            projectRoot,
            dependencyValues,
            "editor");
        ASSERT_EQ(cachedFingerprints.size(), fingerprints.size());
        for (size_t index = 0u; index < cachedFingerprints.size(); ++index)
        {
            ASSERT_TRUE(cachedFingerprints[index].has_value());
            EXPECT_EQ(*cachedFingerprints[index], *fingerprints[index]);
        }
    }
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u)
        << "Warm batches should reuse the cached immutable ArtifactDB lookup index.";
}

TEST(AssetPrefabBehaviorTests, ModelTextureMappingDependencyBatchReusesPersistentArtifactDatabaseLookupIndex)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    ScopedTempDirectory temp("NullusTextureMappingPersistentLookupIndex");
    const auto projectRoot = temp.Path();
    const auto databasePath = projectRoot / "Library" / "ArtifactDB";

    ArtifactDatabase database;
    std::vector<std::string> dependencyValues;
    constexpr size_t kTextureCount = 16u;
    dependencyValues.reserve(kTextureCount);
    for (size_t index = 0u; index < kTextureCount; ++index)
    {
        const auto textureName = "PersistentIndexedTexture" + std::to_string(index);

        ArtifactManifest manifest;
        manifest.sourceAssetId = AssetId(NLS::Guid::Parse(MakeIndexedTextureGuid(index)));
        manifest.importerId = "texture";
        manifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
        manifest.targetPlatform = "editor";
        manifest.primarySubAssetKey = "texture:" + textureName;
        manifest.subAssets.push_back({
            manifest.sourceAssetId,
            "texture:" + textureName,
            ArtifactType::Texture,
            "texture",
            "editor",
            MakeIndexedTextureArtifactPath(index),
            "hash-" + textureName,
            textureName
        });

        database.UpsertManifest(
            manifest,
            "Assets/Textures/" + textureName + ".png",
            ArtifactRecordStatus::UpToDate);
        dependencyValues.push_back("project|" + textureName + "|name-search");
    }
    ASSERT_TRUE(database.Save(databasePath));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto fingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        dependencyValues,
        "editor");
    ASSERT_EQ(fingerprints.size(), kTextureCount);
    for (const auto& fingerprint : fingerprints)
        ASSERT_TRUE(fingerprint.has_value());
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u);

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto persistentCachedFingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        dependencyValues,
        "editor");

    ASSERT_EQ(persistentCachedFingerprints.size(), fingerprints.size());
    for (size_t index = 0u; index < fingerprints.size(); ++index)
    {
        ASSERT_TRUE(persistentCachedFingerprints[index].has_value());
        EXPECT_EQ(*persistentCachedFingerprints[index], *fingerprints[index]);
    }
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 0u)
        << "A new editor process should reuse the persisted texture lookup index when ArtifactDB has not changed.";
}

TEST(AssetPrefabBehaviorTests, ModelTextureMappingPersistentArtifactDatabaseLookupIndexFallsBackWhenCorrupt)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    ScopedTempDirectory temp("NullusTextureMappingCorruptPersistentLookupIndex");
    const auto projectRoot = temp.Path();
    const auto databasePath = projectRoot / "Library" / "ArtifactDB";

    constexpr const char* kTextureName = "CorruptCacheIndexedTexture";
    ArtifactManifest manifest;
    manifest.sourceAssetId = AssetId(NLS::Guid::Parse(MakeIndexedTextureGuid(0u)));
    manifest.importerId = "texture";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = std::string("texture:") + kTextureName;
    manifest.subAssets.push_back({
        manifest.sourceAssetId,
        std::string("texture:") + kTextureName,
        ArtifactType::Texture,
        "texture",
        "editor",
        MakeIndexedTextureArtifactPath(0u),
        std::string("hash-") + kTextureName,
        kTextureName
    });

    ArtifactDatabase database;
    database.UpsertManifest(
        manifest,
        std::string("Assets/Textures/") + kTextureName + ".png",
        ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto initial = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        {std::string("project|") + kTextureName + "|name-search"},
        "editor");
    ASSERT_EQ(initial.size(), 1u);
    ASSERT_TRUE(initial[0].has_value());

    const auto cachePath = projectRoot / "Library" / "ModelTextureMappingArtifactDatabaseTextureIndex.cache";
    const auto artifactDatabaseStamp = ExtractJsonStringFieldForTest(
        ReadRepositoryTextFile(cachePath),
        "artifactDatabaseDataStamp");
    ASSERT_FALSE(artifactDatabaseStamp.empty());

    WriteTextFileForTest(
        cachePath,
        "{\n"
        " \"schemaVersion\": 1,\n"
        " \"targetPlatform\": \"editor\",\n"
        " \"artifactDatabaseDataStamp\": \"" + artifactDatabaseStamp + "\",\n"
        " \"candidates\": [{\"assetId\": 7, \"editorPath\": [], \"rootIndex\": {}}]\n"
        "}");

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto fingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        {std::string("project|") + kTextureName + "|name-search"},
        "editor");

    ASSERT_EQ(fingerprints.size(), 1u);
    ASSERT_TRUE(fingerprints[0].has_value());
    EXPECT_NE(fingerprints[0]->find(kTextureName), std::string::npos);
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u)
        << "Corrupt persistent lookup caches must be ignored and rebuilt from ArtifactDB.";
}

TEST(AssetPrefabBehaviorTests, ModelTextureMappingPersistentArtifactDatabaseLookupIndexInvalidatesWhenArtifactDatabaseChanges)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    ScopedTempDirectory temp("NullusTextureMappingPersistentLookupInvalidation");
    const auto projectRoot = temp.Path();
    const auto databasePath = projectRoot / "Library" / "ArtifactDB";

    auto addTexture = [](ArtifactDatabase& database, const size_t index, const std::string& textureName)
    {
        ArtifactManifest manifest;
        manifest.sourceAssetId = AssetId(NLS::Guid::Parse(MakeIndexedTextureGuid(index)));
        manifest.importerId = "texture";
        manifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
        manifest.targetPlatform = "editor";
        manifest.primarySubAssetKey = "texture:" + textureName;
        manifest.subAssets.push_back({
            manifest.sourceAssetId,
            "texture:" + textureName,
            ArtifactType::Texture,
            "texture",
            "editor",
            MakeIndexedTextureArtifactPath(index),
            "hash-" + textureName,
            textureName
        });
        database.UpsertManifest(
            manifest,
            "Assets/Textures/" + textureName + ".png",
            ArtifactRecordStatus::UpToDate);
    };

    ArtifactDatabase database;
    addTexture(database, 0u, "PersistentBeforeDatabaseChange");
    ASSERT_TRUE(database.Save(databasePath));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto first = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        {"project|PersistentBeforeDatabaseChange|name-search"},
        "editor");
    ASSERT_EQ(first.size(), 1u);
    ASSERT_TRUE(first[0].has_value());
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u);
    ASSERT_TRUE(std::filesystem::is_regular_file(
        projectRoot / "Library" / "ModelTextureMappingArtifactDatabaseTextureIndex.cache"));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto persistentBeforeChange = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        {"project|PersistentBeforeDatabaseChange|name-search"},
        "editor");
    ASSERT_EQ(persistentBeforeChange.size(), 1u);
    ASSERT_TRUE(persistentBeforeChange[0].has_value());
    EXPECT_EQ(*persistentBeforeChange[0], *first[0]);
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 0u)
        << "This test must prove the pre-change persistent lookup cache is readable before testing invalidation.";

    addTexture(database, 1u, "PersistentAfterDatabaseChange");
    ASSERT_TRUE(database.Save(databasePath));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto afterChange = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        {"project|PersistentAfterDatabaseChange|name-search"},
        "editor");

    ASSERT_EQ(afterChange.size(), 1u);
    ASSERT_TRUE(afterChange[0].has_value());
    EXPECT_NE(afterChange[0]->find("PersistentAfterDatabaseChange"), std::string::npos);
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u)
        << "A persistent lookup index stamped against an older ArtifactDB must be rejected after ArtifactDB changes.";
}

TEST(AssetPrefabBehaviorTests, ModelTextureMappingPersistentArtifactDatabaseLookupIndexFallsBackWhenOversized)
{
    using namespace NLS::Core::Assets;
    using namespace NLS::Editor::Assets;

    ScopedTempDirectory temp("NullusTextureMappingOversizedPersistentLookupIndex");
    const auto projectRoot = temp.Path();
    const auto databasePath = projectRoot / "Library" / "ArtifactDB";

    constexpr const char* kTextureName = "OversizedCacheIndexedTexture";
    ArtifactManifest manifest;
    manifest.sourceAssetId = AssetId(NLS::Guid::Parse(MakeIndexedTextureGuid(0u)));
    manifest.importerId = "texture";
    manifest.importerVersion = GetCurrentImporterVersion(AssetType::Texture);
    manifest.targetPlatform = "editor";
    manifest.primarySubAssetKey = std::string("texture:") + kTextureName;
    manifest.subAssets.push_back({
        manifest.sourceAssetId,
        std::string("texture:") + kTextureName,
        ArtifactType::Texture,
        "texture",
        "editor",
        MakeIndexedTextureArtifactPath(0u),
        std::string("hash-") + kTextureName,
        kTextureName
    });

    ArtifactDatabase database;
    database.UpsertManifest(
        manifest,
        std::string("Assets/Textures/") + kTextureName + ".png",
        ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(database.Save(databasePath));

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto initial = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        {std::string("project|") + kTextureName + "|name-search"},
        "editor");
    ASSERT_EQ(initial.size(), 1u);
    ASSERT_TRUE(initial[0].has_value());

    const auto cachePath = projectRoot / "Library" / "ModelTextureMappingArtifactDatabaseTextureIndex.cache";
    const auto artifactDatabaseStamp = ExtractJsonStringFieldForTest(
        ReadRepositoryTextFile(cachePath),
        "artifactDatabaseDataStamp");
    ASSERT_FALSE(artifactDatabaseStamp.empty());

    std::string oversizedCache =
        "{\n"
        " \"schemaVersion\": 1,\n"
        " \"targetPlatform\": \"editor\",\n"
        " \"artifactDatabaseDataStamp\": \"" + artifactDatabaseStamp + "\",\n"
        " \"candidates\": [{\n"
        "  \"assetId\": \"11111111-1111-4111-8111-111111111111\",\n"
        "  \"subAssetKey\": \"texture:OversizedCacheIndexedTexture\",\n"
        "  \"editorPath\": \"Assets/Textures/OversizedCacheIndexedTexture.png\",\n"
        "  \"artifactPath\": \"Library/Artifacts/Stale/oversized.ntex\",\n"
        "  \"displayName\": \"OversizedCacheIndexedTexture\",\n"
        "  \"imported\": true,\n"
        "  \"rootIndex\": 0,\n"
        "  \"artifactHashOrVersion\": \"stale-oversized-cache-hash";
    oversizedCache.append((4u * 1024u * 1024u + 1u) - oversizedCache.size(), 'x');
    oversizedCache += "\"\n }]}\n";
    WriteTextFileForTest(
        cachePath,
        oversizedCache);

    ClearModelTextureMappingDependencyFingerprintCacheForTesting();
    const auto fingerprints = ComputeModelTextureMappingDependencyFingerprintsForTesting(
        projectRoot,
        {std::string("project|") + kTextureName + "|name-search"},
        "editor");

    ASSERT_EQ(fingerprints.size(), 1u);
    ASSERT_TRUE(fingerprints[0].has_value());
    EXPECT_NE(fingerprints[0]->find(kTextureName), std::string::npos);
    EXPECT_EQ(fingerprints[0]->find("stale-oversized-cache-hash"), std::string::npos)
        << "If the oversized cache were parsed, this stale candidate hash would leak into the fingerprint.";
    EXPECT_EQ(GetModelTextureMappingDependencyArtifactDatabaseLoadCountForTesting(), 1u)
        << "Oversized persistent lookup caches must be ignored instead of parsed on startup.";
}

TEST(AssetPrefabBehaviorTests, PrefabValidationFingerprintIgnoresRuntimeArtifactPaths)
{
    const auto references = MakeSharedPrefabResourceReferences(
        "36363636-3636-4636-8636-363636363636",
        "Library/Artifacts/Perf/validation-body.nmesh",
        "Library/Artifacts/Perf/validation-body.nmat");
    auto artifact = MakePrefabArtifact(
        "Prefab_ValidationFingerprintStablePaths",
        "37373737-3737-4737-8737-373737373737",
        2u,
        1u,
        &references);
    ASSERT_FALSE(artifact.resolvedAssets.empty());

    auto relocatedArtifact = artifact;
    for (auto& resolved : relocatedArtifact.resolvedAssets)
        resolved.artifactPath = "Library/Artifacts/Relocated/" + resolved.expectedType;

    EXPECT_EQ(
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(artifact),
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(relocatedArtifact))
        << "Validation proofs should track the prefab graph and logical asset references, not runtime artifact storage paths.";
}

TEST(AssetPrefabBehaviorTests, PrefabValidationFingerprintIgnoresResolvedAssetOrder)
{
    const auto references = MakeSharedPrefabResourceReferences(
        "41414141-4141-4141-8141-414141414141",
        "Library/Artifacts/Perf/order-body.nmesh",
        "Library/Artifacts/Perf/order-body.nmat");
    auto artifact = MakePrefabArtifact(
        "Prefab_ValidationFingerprintStableOrder",
        "42424242-4242-4242-8242-424242424242",
        2u,
        1u,
        &references);
    ASSERT_GT(artifact.resolvedAssets.size(), 1u);

    auto reorderedArtifact = artifact;
    std::reverse(reorderedArtifact.resolvedAssets.begin(), reorderedArtifact.resolvedAssets.end());

    EXPECT_EQ(
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(artifact),
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(reorderedArtifact))
        << "Validation proofs should treat imported sub-assets as a set so ArtifactDB restore order cannot invalidate trusted generated-prefab validation.";
}

TEST(AssetPrefabBehaviorTests, PrefabValidationResolvedAssetsNormalizeInvalidManifestSubAssetSourceIds)
{
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("38383838-3838-4838-8838-383838383838"));
    manifest.subAssets.push_back({
        {},
        "mesh:body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor",
        "Library/Artifacts/Sparse/meshes/body",
        "mesh-hash"
    });

    const auto resolvedAssets =
        NLS::Engine::Assets::BuildPrefabValidationResolvedAssetsFromManifest(manifest);

    ASSERT_EQ(resolvedAssets.size(), 1u);
    EXPECT_EQ(resolvedAssets[0].assetId, manifest.sourceAssetId)
        << "Validation proof fingerprints should match ArtifactDB manifest restore, which normalizes invalid sub-asset source ids to the owning source asset id.";
}

TEST(AssetPrefabBehaviorTests, MissingTrustedGraphValidationProofFallsBackToValidation)
{
    NLS::Engine::GameObject root("UnprovenGeneratedPrefab", "Prefab");
    const auto prefabDocument = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(prefabDocument.graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("34343434-3434-4434-8434-343434343434")),
        {},
        NLS::Engine::Assets::PrefabImportOptions {
            .trustResolvedAssets = true,
            .trustGraphValidation = true
        });
    ASSERT_FALSE(importResult.diagnostics.HasErrors());

    const auto snapshot = stats.Snapshot();
    const auto* validation = FindStage(snapshot, "ValidatePrefabArtifact");
    ASSERT_NE(validation, nullptr);
    ASSERT_TRUE(validation->counters.contains("validationCacheMissCount"));
    EXPECT_EQ(validation->counters.at("validationCacheMissCount"), 1u)
        << "Generated prefab loads must not skip validation unless the artifact carries a matching importer validation proof.";
    EXPECT_FALSE(validation->counters.contains("validationCacheHitCount"));
}

TEST(AssetPrefabBehaviorTests, MismatchedTrustedGraphValidationProofFallsBackToValidation)
{
    NLS::Engine::GameObject root("MismatchedProofGeneratedPrefab", "Prefab");
    const auto prefabDocument = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(prefabDocument.graph),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("35353535-3535-4535-8535-353535353535")),
        {},
        NLS::Engine::Assets::PrefabImportOptions {
            .trustResolvedAssets = true,
            .trustGraphValidation = true,
            .trustedGraphValidationFingerprint = "0"
        });
    ASSERT_FALSE(importResult.diagnostics.HasErrors());

    const auto snapshot = stats.Snapshot();
    const auto* validation = FindStage(snapshot, "ValidatePrefabArtifact");
    ASSERT_NE(validation, nullptr);
    ASSERT_TRUE(validation->counters.contains("validationCacheMissCount"));
    EXPECT_EQ(validation->counters.at("validationCacheMissCount"), 1u)
        << "Generated prefab validation proofs must match the current graph and resolved assets before validation can be skipped.";
    EXPECT_FALSE(validation->counters.contains("validationCacheHitCount"));
}

TEST(AssetPrefabBehaviorTests, MismatchedTrustedGraphValidationProofPrimesRuntimeResolvedGraphAfterValidation)
{
    const auto references = MakeSharedPrefabResourceReferences(
        "36363636-3636-4636-8636-363636363636",
        "Library/Artifacts/Perf/mismatch-body.nmesh",
        "Library/Artifacts/Perf/mismatch-body.nmat");
    const auto sourceArtifact = MakePrefabArtifact(
        "Prefab_MismatchedProofRuntimeResolvedGraph",
        "37373737-3737-4737-8737-373737373737",
        4u,
        1u,
        &references);

    PerformanceStageStats importStats;
    NLS::Engine::Assets::PrefabImportResult importResult;
    {
        PerformanceStageStatsCapture importCapture(importStats);
        importResult = NLS::Engine::Assets::ImportPrefabArtifact(
            NLS::Engine::Serialize::ObjectGraphWriter::Write(sourceArtifact.graph),
            sourceArtifact.assetId,
            sourceArtifact.resolvedAssets,
            NLS::Engine::Assets::PrefabImportOptions {
                .trustResolvedAssets = true,
                .trustGraphValidation = true,
                .trustedGraphValidationFingerprint = "0"
            });
    }
    ASSERT_FALSE(importResult.diagnostics.HasErrors());

    const auto importSnapshot = importStats.Snapshot();
    const auto* validation = FindStage(importSnapshot, "ValidatePrefabArtifact");
    ASSERT_NE(validation, nullptr);
    ASSERT_TRUE(validation->counters.contains("validationCacheMissCount"));
    EXPECT_EQ(validation->counters.at("validationCacheMissCount"), 1u);

    PerformanceStageStats instantiateStats;
    PerformanceStageStatsCapture instantiateCapture(instantiateStats);
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(importResult.artifact, scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    const auto instantiateSnapshot = instantiateStats.Snapshot();
    const auto* externalReferences = FindStage(instantiateSnapshot, "ResolveExternalReferences");
    ASSERT_NE(externalReferences, nullptr);
    ASSERT_TRUE(externalReferences->counters.contains("runtimeResolvedGraphCacheHitCount"));
    EXPECT_EQ(externalReferences->counters.at("runtimeResolvedGraphCacheHitCount"), 1u)
        << "Fallback validation should still prime trusted resolved graphs before first instantiation.";
    ASSERT_TRUE(externalReferences->counters.contains("runtimeResolvedGraphCopyCount"));
    EXPECT_EQ(externalReferences->counters.at("runtimeResolvedGraphCopyCount"), 0u);
}

TEST(AssetPrefabBehaviorTests, TrustedResolvedAssetsPrimeRuntimeResolvedGraphCache)
{
    const auto references = MakeSharedPrefabResourceReferences(
        "32323232-3232-4232-8232-323232323232",
        "Library/Artifacts/Perf/trusted-body.nmesh",
        "Library/Artifacts/Perf/trusted-body.nmat");
    const auto sourceArtifact = MakePrefabArtifact(
        "Prefab_TrustedRuntimeResolvedGraph",
        "33333333-3333-4333-8333-333333333333",
        4u,
        1u,
        &references);
    const auto validationProof = std::to_string(
        NLS::Engine::Assets::BuildPrefabArtifactValidationFingerprint(sourceArtifact));

    auto importResult = NLS::Engine::Assets::ImportPrefabArtifact(
        NLS::Engine::Serialize::ObjectGraphWriter::Write(sourceArtifact.graph),
        sourceArtifact.assetId,
        sourceArtifact.resolvedAssets,
        NLS::Engine::Assets::PrefabImportOptions {
            .trustResolvedAssets = true,
            .trustGraphValidation = true,
            .trustedGraphValidationFingerprint = validationProof
        });
    ASSERT_FALSE(importResult.diagnostics.HasErrors());

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(importResult.artifact, scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    const auto snapshot = stats.Snapshot();
    const auto* externalReferences = FindStage(snapshot, "ResolveExternalReferences");
    ASSERT_NE(externalReferences, nullptr);
    ASSERT_TRUE(externalReferences->counters.contains("runtimeResolvedGraphCacheHitCount"));
    EXPECT_EQ(externalReferences->counters.at("runtimeResolvedGraphCacheHitCount"), 1u)
        << "Trusted manifest-backed generated prefab loads should prime the runtime-resolved graph cache before first instantiation.";
    ASSERT_TRUE(externalReferences->counters.contains("runtimeResolvedGraphCopyCount"));
    EXPECT_EQ(externalReferences->counters.at("runtimeResolvedGraphCopyCount"), 0u);
}

TEST(AssetPrefabBehaviorTests, PrefabInstantiationDoesNotSynchronouslyPrewarmResourcesByDefault)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_NoSyncResourcePrewarm",
        "1c1c1c1c-1c1c-4c1c-8c1c-1c1c1c1c1c1c",
        1u,
        1u);
    artifact.resolvedAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("2c2c2c2c-2c2c-4c2c-8c2c-2c2c2c2c2c2c")),
        "Mesh",
        "mesh:body",
        "Library/Artifacts/body.nmesh"
    });
    artifact.resolvedAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("3c3c3c3c-3c3c-4c3c-8c3c-3c3c3c3c3c3c")),
        "Material",
        "material:body",
        "Library/Artifacts/body.nmat"
    });

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);

    const auto snapshot = stats.Snapshot();
    EXPECT_EQ(FindStage(snapshot, "WaitForResources"), nullptr);
    EXPECT_EQ(FindStage(snapshot, "UploadGpuResources"), nullptr);
}

TEST(AssetPrefabBehaviorTests, ExplicitSynchronousResourcePrewarmRetainsDiagnosticStages)
{
    NLS::Core::ResourceManagement::MeshManager meshManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerService(meshManager);

    auto artifact = MakePrefabArtifact(
        "Prefab_ExplicitSyncResourcePrewarm",
        "1d1d1d1d-1d1d-4d1d-8d1d-1d1d1d1d1d1d",
        1u,
        1u);
    artifact.resolvedAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("2d2d2d2d-2d2d-4d2d-8d2d-2d2d2d2d2d2d")),
        "Mesh",
        "mesh:body",
        "Library/Artifacts/body.nmesh"
    });
    artifact.resolvedAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("3d3d3d3d-3d3d-4d3d-8d3d-3d3d3d3d3d3d")),
        "Material",
        "material:body",
        "Library/Artifacts/body.nmat"
    });

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    NLS::Engine::Serialize::LoadPolicy policy;
    policy.synchronousAssetReferencePrewarm = true;

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);

    const auto snapshot = stats.Snapshot();
    const auto* waitForResources = FindStage(snapshot, "WaitForResources");
    ASSERT_NE(waitForResources, nullptr);
    const auto* uploadGpuResources = FindStage(snapshot, "UploadGpuResources");
    ASSERT_NE(uploadGpuResources, nullptr);
    ASSERT_TRUE(uploadGpuResources->counters.contains("dependencyCount"));
    EXPECT_GE(uploadGpuResources->counters.at("dependencyCount"), 2u);
    ASSERT_TRUE(uploadGpuResources->counters.contains("synchronousResourceLoadCount"));
    EXPECT_EQ(uploadGpuResources->counters.at("synchronousResourceLoadCount"), 1u);
}

TEST(AssetPrefabBehaviorTests, DeferredAssetResolutionPreservesSynchronousPrewarmOptIn)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_DeferPreservesSyncResourcePrewarm",
        "1e1e1e1e-1e1e-4e1e-8e1e-1e1e1e1e1e1e",
        1u,
        1u);
    artifact.resolvedAssets.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("2e2e2e2e-2e2e-4e2e-8e2e-2e2e2e2e2e2e")),
        "Mesh",
        "mesh:body",
        "Library/Artifacts/body.nmesh"
    });

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    policy.synchronousAssetReferencePrewarm = true;

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene, policy);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);

    const auto snapshot = stats.Snapshot();
    const auto* waitForResources = FindStage(snapshot, "WaitForResources");
    ASSERT_NE(waitForResources, nullptr);
    const auto* uploadGpuResources = FindStage(snapshot, "UploadGpuResources");
    ASSERT_NE(uploadGpuResources, nullptr);
    ASSERT_TRUE(uploadGpuResources->counters.contains("dependencyCount"));
    EXPECT_GE(uploadGpuResources->counters.at("dependencyCount"), 1u);
}

TEST(AssetPrefabBehaviorTests, DeferredSceneRegistrationPreservesFastAccessComponents)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_FastAccessPreserved",
        "16161616-1616-4616-8616-161616161616",
        8u,
        2u);

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    EXPECT_EQ(scene.GetFastAccessComponents().modelRenderers.size(), 3u);
}

TEST(AssetPrefabBehaviorTests, PrefabInstantiationReportsBatchCreatePopulateAndBindPhases)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_BatchCreatePopulateBind",
        "1b1b1b1b-1b1b-4b1b-8b1b-1b1b1b1b1b1b",
        32u,
        2u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        std::move(artifact),
        &elapsed);

    const auto* allocate = FindStage(snapshot, "AllocateInstanceObjects");
    ASSERT_NE(allocate, nullptr);
    ASSERT_TRUE(allocate->counters.contains("objectCount"));
    ASSERT_TRUE(allocate->counters.contains("reservedObjectCount"));
    EXPECT_EQ(allocate->counters.at("objectCount"), 32u);
    EXPECT_EQ(allocate->counters.at("reservedObjectCount"), 32u);

    const auto* restoreGameObjects = FindStage(snapshot, "RestoreGameObjectState");
    ASSERT_NE(restoreGameObjects, nullptr);
    ASSERT_TRUE(restoreGameObjects->counters.contains("restoredGameObjectCount"));
    EXPECT_EQ(restoreGameObjects->counters.at("restoredGameObjectCount"), 32u);

    const auto* createComponents = FindStage(snapshot, "CreateComponents");
    ASSERT_NE(createComponents, nullptr);
    ASSERT_TRUE(createComponents->counters.contains("createdComponentCount"));
    ASSERT_TRUE(createComponents->counters.contains("componentRecordCount"));
    ASSERT_TRUE(createComponents->counters.contains("indexedRecordLookupCount"));
    ASSERT_TRUE(createComponents->counters.contains("linearRecordLookupCount"));
    EXPECT_GE(createComponents->counters.at("createdComponentCount"), 32u);
    EXPECT_EQ(
        createComponents->counters.at("createdComponentCount"),
        createComponents->counters.at("componentRecordCount"));
    EXPECT_EQ(createComponents->counters.at("indexedRecordLookupCount"), 0u);
    EXPECT_EQ(createComponents->counters.at("linearRecordLookupCount"), 0u);

    const auto* deserialize = FindStage(snapshot, "DeserializeComponents");
    ASSERT_NE(deserialize, nullptr);
    ASSERT_TRUE(deserialize->counters.contains("restoredGameObjectCount"));
    ASSERT_TRUE(deserialize->counters.contains("restoredComponentCount"));
    ASSERT_TRUE(deserialize->counters.contains("indexedRecordLookupCount"));
    ASSERT_TRUE(deserialize->counters.contains("linearRecordLookupCount"));
    EXPECT_EQ(deserialize->counters.at("restoredGameObjectCount"), 32u);
    EXPECT_EQ(
        deserialize->counters.at("restoredComponentCount"),
        createComponents->counters.at("createdComponentCount"));
    EXPECT_EQ(deserialize->counters.at("indexedRecordLookupCount"), 0u);
    EXPECT_EQ(deserialize->counters.at("linearRecordLookupCount"), 0u);

    const auto* bindExternalReferences = FindStage(snapshot, "BindExternalAssetReferences");
    ASSERT_NE(bindExternalReferences, nullptr);
    ASSERT_TRUE(bindExternalReferences->counters.contains("assetReferenceBindingCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("assetReferenceElementBindingCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("componentCount"));
    EXPECT_GE(bindExternalReferences->counters.at("assetReferenceBindingCount"), 16u);
    EXPECT_EQ(
        bindExternalReferences->counters.at("componentCount"),
        createComponents->counters.at("createdComponentCount"));

    const auto* fixup = FindStage(snapshot, "FixupInternalReferences");
    ASSERT_NE(fixup, nullptr);
    ASSERT_TRUE(fixup->counters.contains("parentFixupCount"));
    EXPECT_EQ(fixup->counters.at("parentFixupCount"), 31u);

    EXPECT_GT(elapsed.count(), 0);
}

TEST(AssetPrefabBehaviorTests, TransformMathPropertiesAreSerializedAsDirectlyReadableObjects)
{
    NLS::Engine::GameObject root("Prefab_DirectMathShape", "Prefab");
    const auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root).graph;

    const auto* transformRecord = FindObjectRecordByType(
        document,
        "NLS::Engine::Components::TransformComponent");
    ASSERT_NE(transformRecord, nullptr);

    const auto transformType =
        NLS::meta::Type::GetFromName("NLS::Engine::Components::TransformComponent");
    ASSERT_TRUE(transformType.IsValid());

    struct ExpectedMathProperty
    {
        const char* propertyName;
        const char* typeName;
        std::vector<std::string> expectedKeys;
    };

    const ExpectedMathProperty expectedProperties[] = {
        {"localPosition", "NLS::Maths::Vector3", {"x", "y", "z"}},
        {"localRotation", "NLS::Maths::Quaternion", {"x", "y", "z", "w"}},
        {"localScale", "NLS::Maths::Vector3", {"x", "y", "z"}}
    };

    for (const auto& expected : expectedProperties)
    {
        const auto field = transformType.GetField(expected.propertyName);
        ASSERT_TRUE(field.IsValid()) << expected.propertyName;
        EXPECT_EQ(field.GetType().GetName(), expected.typeName) << expected.propertyName;

        const auto* property = FindProperty(*transformRecord, expected.propertyName);
        ASSERT_NE(property, nullptr) << expected.propertyName;
        ASSERT_EQ(property->value.GetKind(), NLS::Engine::Serialize::PropertyValue::Kind::Object)
            << expected.propertyName;

        for (const auto& key : expected.expectedKeys)
        {
            EXPECT_NE(
                std::find_if(
                    property->value.GetObject().begin(),
                    property->value.GetObject().end(),
                    [&key](const auto& item)
                    {
                        return item.first == key;
                    }),
                property->value.GetObject().end())
                << expected.propertyName << "." << key;
        }
    }
}

TEST(AssetPrefabBehaviorTests, MaterialReferenceBindingCapsSerializedMaterialSlots)
{
    const auto references = MakeSharedPrefabResourceReferences(
        "25252525-2525-4525-8525-252525252525",
        "Library/Artifacts/Perf/capped-body.nmesh",
        "Library/Artifacts/Perf/capped-body.nmat");
    auto artifact = MakePrefabArtifact(
        "Prefab_MaterialSlotCap",
        "26262626-2626-4626-8626-262626262626",
        1u,
        1u,
        &references);

    auto* meshRendererRecord = FindMutableObjectRecordByType(
        artifact.graph,
        NLS_TYPEOF(NLS::Engine::Components::MeshRenderer).GetName());
    ASSERT_NE(meshRendererRecord, nullptr);
    auto* materials = FindMutableProperty(*meshRendererRecord, "materials");
    ASSERT_NE(materials, nullptr);

    const auto assetId = NLS::Engine::Serialize::AssetId(
        NLS::Guid::Parse("27272727-2727-4727-8727-272727272727"));
    NLS::Engine::Serialize::PropertyValue::ArrayValue materialReferences;
    materialReferences.reserve(
        static_cast<size_t>(NLS::Engine::Components::MeshRenderer::kMaxMaterialCount) + 1u);
    for (uint32_t index = 0u;
         index < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount + 1u;
         ++index)
    {
        const auto materialPath = "Assets/Perf/Material_" + std::to_string(index) + ".nmat";
        materialReferences.push_back(NLS::Engine::Serialize::PropertyValue::ObjectReference(
            NLS::Engine::Serialize::ObjectIdentifier::Asset(
                assetId,
                NLS::Engine::Serialize::MakeLocalIdentifierInFile(assetId.GetGuid(), materialPath),
                materialPath)));
    }
    materials->value = NLS::Engine::Serialize::PropertyValue::Array(std::move(materialReferences));

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiatePrefabGraph(
        artifact.graph,
        scene,
        {});

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);

    const auto snapshot = stats.Snapshot();
    const auto* bindExternalReferences = FindStage(snapshot, "BindExternalAssetReferences");
    ASSERT_NE(bindExternalReferences, nullptr);
    ASSERT_TRUE(bindExternalReferences->counters.contains("assetReferenceBindingCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("assetReferenceElementBindingCount"));
    EXPECT_EQ(bindExternalReferences->counters.at("assetReferenceBindingCount"), 2u);
    const auto expectedBindingElementCount =
        static_cast<uint64_t>(NLS::Engine::Components::MeshRenderer::kMaxMaterialCount) + 1u;
    EXPECT_EQ(
        bindExternalReferences->counters.at("assetReferenceElementBindingCount"),
        expectedBindingElementCount)
        << "Binding should count one MeshFilter mesh plus material entries capped at the renderer slot limit.";
}

TEST(AssetPrefabBehaviorTests, RepeatedPrefabInstantiationReusesRuntimeResolvedGraph)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerService(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerService(materialManager);

    constexpr const char* meshPath = "Library/Artifacts/Perf/repeated-body.nmesh";
    constexpr const char* materialPath = "Library/Artifacts/Perf/repeated-body.nmat";
    auto* mesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource(meshPath, mesh);
    auto* material = new NLS::Render::Resources::Material();
    const_cast<std::string&>(material->path) = materialPath;
    materialManager.RegisterResource(materialPath, material);

    const auto references = MakeSharedPrefabResourceReferences(
        "21212121-2121-4121-8121-212121212121",
        meshPath,
        materialPath);
    auto artifact = MakePrefabArtifact(
        "Prefab_RepeatedRuntimeResolvedGraph",
        "22222222-2222-4222-8222-222222222222",
        257u,
        2u,
        &references);

    NLS::Engine::SceneSystem::Scene firstScene;
    const auto first = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, firstScene);
    ASSERT_FALSE(first.diagnostics.HasErrors());
    ASSERT_NE(first.root, nullptr);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    NLS::Engine::SceneSystem::Scene secondScene;
    const auto second = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, secondScene);

    ASSERT_FALSE(second.diagnostics.HasErrors());
    ASSERT_NE(second.root, nullptr);

    const auto snapshot = stats.Snapshot();
    const auto* validation = FindStage(snapshot, "ValidatePrefabArtifact");
    ASSERT_NE(validation, nullptr);
    ASSERT_TRUE(validation->counters.contains("validationGraphCopyCount"));
    EXPECT_EQ(validation->counters.at("validationGraphCopyCount"), 0u)
        << "Repeated instantiation should validate the source graph by reference instead of copying it.";
    ASSERT_TRUE(validation->counters.contains("validationCacheHitCount"));
    EXPECT_GE(validation->counters.at("validationCacheHitCount"), 1u)
        << "Repeated instantiation should reuse validation results for an unchanged prefab artifact.";

    const auto* externalReferences = FindStage(snapshot, "ResolveExternalReferences");
    ASSERT_NE(externalReferences, nullptr);
    ASSERT_TRUE(externalReferences->counters.contains("runtimeResolvedGraphCopyCount"));
    EXPECT_EQ(externalReferences->counters.at("runtimeResolvedGraphCopyCount"), 0u)
        << "Repeated instantiation of an already prepared prefab should reuse the runtime-resolved graph.";
    ASSERT_TRUE(externalReferences->counters.contains("runtimeResolvedGraphCacheHitCount"));
    EXPECT_GE(externalReferences->counters.at("runtimeResolvedGraphCacheHitCount"), 1u);

    meshManager.UnloadResources();
    materialManager.UnloadResources();
}

TEST(AssetPrefabBehaviorTests, RepeatedPrefabInstantiationReusesCompiledInstantiatePlan)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_RepeatedInstantiatePlan",
        "23232323-2323-4323-8323-232323232323",
        257u,
        2u);

    NLS::Engine::SceneSystem::Scene firstScene;
    const auto first = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, firstScene);
    ASSERT_FALSE(first.diagnostics.HasErrors());
    ASSERT_NE(first.root, nullptr);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    NLS::Engine::SceneSystem::Scene secondScene;
    const auto second = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, secondScene);

    ASSERT_FALSE(second.diagnostics.HasErrors());
    ASSERT_NE(second.root, nullptr);

    const auto snapshot = stats.Snapshot();
    const auto* preparePlan = FindStage(snapshot, "PrepareInstantiatePlan");
    ASSERT_NE(preparePlan, nullptr);
    ASSERT_TRUE(preparePlan->counters.contains("instantiatePlanCacheHitCount"));
    ASSERT_TRUE(preparePlan->counters.contains("instantiatePlanBuildCount"));
    ASSERT_TRUE(preparePlan->counters.contains("instantiatePlanComponentCount"));
    EXPECT_EQ(preparePlan->counters.at("instantiatePlanCacheHitCount"), 1u)
        << "Repeated instantiation should reuse the compiled prefab instantiate plan.";
    EXPECT_EQ(preparePlan->counters.at("instantiatePlanBuildCount"), 0u)
        << "Repeated instantiation should not rebuild the compiled prefab instantiate plan.";
    EXPECT_GT(preparePlan->counters.at("instantiatePlanComponentCount"), 0u);

    const auto* createComponents = FindStage(snapshot, "CreateComponents");
    ASSERT_NE(createComponents, nullptr);
    ASSERT_TRUE(createComponents->counters.contains("indexedRecordLookupCount"));
    EXPECT_EQ(createComponents->counters.at("indexedRecordLookupCount"), 0u)
        << "Compiled component lists should avoid per-component record index lookups while creating components.";

    const auto* deserializeComponents = FindStage(snapshot, "DeserializeComponents");
    ASSERT_NE(deserializeComponents, nullptr);
    ASSERT_TRUE(deserializeComponents->counters.contains("indexedRecordLookupCount"));
    EXPECT_EQ(deserializeComponents->counters.at("indexedRecordLookupCount"), 0u)
        << "Compiled component lists should avoid per-component record index lookups while deserializing components.";

    const auto* restoreGameObjects = FindStage(snapshot, "RestoreGameObjectState");
    ASSERT_NE(restoreGameObjects, nullptr);
    ASSERT_TRUE(restoreGameObjects->counters.contains("compiledGameObjectStatePlanUsedCount"));
    ASSERT_TRUE(restoreGameObjects->counters.contains("gameObjectStatePropertyLookupCount"));
    EXPECT_EQ(restoreGameObjects->counters.at("compiledGameObjectStatePlanUsedCount"), 257u)
        << "Compiled prefab plans should restore GameObject hot-path state without per-object named property lookups.";
    EXPECT_EQ(restoreGameObjects->counters.at("gameObjectStatePropertyLookupCount"), 0u)
        << "Compiled GameObject state should use cached property slots instead of repeated name lookups.";
}

TEST(AssetPrefabBehaviorTests, StaleCompiledInstantiatePlanFallsBackWhenGraphChangesInPlace)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_StaleInstantiatePlan",
        "24242424-2424-4424-8424-242424242424");
    const auto plan = NLS::Engine::Serialize::ObjectGraphInstantiator::BuildPrefabInstantiatePlan(
        artifact.graph);

    auto* rootRecord = FindMutableObjectRecordByType(
        artifact.graph,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName());
    ASSERT_NE(rootRecord, nullptr);
    auto* components = FindMutableProperty(*rootRecord, "components");
    ASSERT_NE(components, nullptr);
    components->value = NLS::Engine::Serialize::PropertyValue::Array({});

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiatePrefabGraph(
        artifact.graph,
        scene,
        {},
        &plan);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    EXPECT_EQ(instance.root->GetComponent<NLS::Engine::Components::LightComponent>(), nullptr)
        << "A stale plan must not recreate components removed from the graph after the plan was compiled.";

    const auto snapshot = stats.Snapshot();
    const auto* resolveDependencies = FindStage(snapshot, "ResolveDependencies");
    ASSERT_NE(resolveDependencies, nullptr);
    EXPECT_FALSE(resolveDependencies->counters.contains("compiledInstantiatePlanUsedCount"))
        << "A stale compiled plan should be rejected and the safe graph-scanning path should run.";
}

TEST(AssetPrefabBehaviorTests, CompiledGameObjectStatePlanReadsCurrentPropertyValues)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_CurrentGameObjectStateValue",
        "29292929-2929-4929-8929-292929292929");
    const auto plan = NLS::Engine::Serialize::ObjectGraphInstantiator::BuildPrefabInstantiatePlan(
        artifact.graph);

    auto* rootRecord = FindMutableObjectRecordByType(
        artifact.graph,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName());
    ASSERT_NE(rootRecord, nullptr);
    auto* name = FindMutableProperty(*rootRecord, "name");
    ASSERT_NE(name, nullptr);
    name->value = NLS::Engine::Serialize::PropertyValue::String("Prefab_CurrentNameFromGraph");

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiatePrefabGraph(
        artifact.graph,
        scene,
        {},
        &plan);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    EXPECT_EQ(instance.root->GetName(), "Prefab_CurrentNameFromGraph")
        << "The compiled state plan must cache property slots, not stale property values.";

    const auto snapshot = stats.Snapshot();
    const auto* resolveDependencies = FindStage(snapshot, "ResolveDependencies");
    ASSERT_NE(resolveDependencies, nullptr);
    ASSERT_TRUE(resolveDependencies->counters.contains("compiledInstantiatePlanUsedCount"));
}

TEST(AssetPrefabBehaviorTests, StaleCompiledInstantiatePlanFallsBackWhenGameObjectStateLayoutChangesInPlace)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_StaleGameObjectStatePlan",
        "30303030-3030-4030-8030-303030303030");
    const auto plan = NLS::Engine::Serialize::ObjectGraphInstantiator::BuildPrefabInstantiatePlan(
        artifact.graph);

    auto* rootRecord = FindMutableObjectRecordByType(
        artifact.graph,
        NLS_TYPEOF(NLS::Engine::GameObject).GetName());
    ASSERT_NE(rootRecord, nullptr);
    rootRecord->properties.push_back({
        "sourceObjectKey",
        NLS::Engine::Serialize::PropertyValue::String("AddedAfterPlan")
    });

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiatePrefabGraph(
        artifact.graph,
        scene,
        {},
        &plan);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    EXPECT_EQ(instance.root->GetSourceObjectKey(), "AddedAfterPlan")
        << "A stale compiled GameObject state plan must not skip newly added runtime metadata.";

    const auto snapshot = stats.Snapshot();
    const auto* resolveDependencies = FindStage(snapshot, "ResolveDependencies");
    ASSERT_NE(resolveDependencies, nullptr);
    EXPECT_FALSE(resolveDependencies->counters.contains("compiledInstantiatePlanUsedCount"))
        << "A stale compiled plan should be rejected when GameObject state property slots change.";
}

TEST(AssetPrefabBehaviorTests, StaleCompiledInstantiatePlanFallsBackWhenGameObjectSetChangesInPlace)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_StaleGameObjectSetPlan",
        "28282828-2828-4828-8828-282828282828");
    const auto plan = NLS::Engine::Serialize::ObjectGraphInstantiator::BuildPrefabInstantiatePlan(
        artifact.graph);

    auto* lightRecord = FindMutableObjectRecordByType(
        artifact.graph,
        NLS_TYPEOF(NLS::Engine::Components::LightComponent).GetName());
    ASSERT_NE(lightRecord, nullptr);
    lightRecord->typeName = NLS_TYPEOF(NLS::Engine::GameObject).GetName();
    lightRecord->debugName = "PromotedRoot";
    lightRecord->properties.clear();
    artifact.graph.root = lightRecord->id;

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Serialize::ObjectGraphInstantiator::InstantiatePrefabGraph(
        artifact.graph,
        scene,
        {},
        &plan);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr)
        << "A stale plan must not hide a GameObject introduced after the plan was compiled.";
    EXPECT_EQ(instance.root->GetName(), "PromotedRoot");

    const auto snapshot = stats.Snapshot();
    const auto* resolveDependencies = FindStage(snapshot, "ResolveDependencies");
    ASSERT_NE(resolveDependencies, nullptr);
    EXPECT_FALSE(resolveDependencies->counters.contains("compiledInstantiatePlanUsedCount"))
        << "A stale compiled plan should be rejected when the current graph GameObject set changes.";
}

TEST(AssetPrefabBehaviorTests, MultiRendererPrefabReportsRendererCount)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_MultiRenderer",
        "14141414-1414-4414-8414-141414141414",
        64u,
        2u);

    const auto snapshot = RunPrefabInstantiationScenario(std::move(artifact));

    const auto* registerRenderers = FindStage(snapshot, "RegisterRenderers");
    ASSERT_NE(registerRenderers, nullptr);
    ASSERT_TRUE(registerRenderers->counters.contains("rendererCount"));
    EXPECT_GE(registerRenderers->counters.at("rendererCount"), 31u);
}
