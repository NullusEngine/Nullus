#include <gtest/gtest.h>

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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace NLS::Base::Profiling;
using namespace std::chrono_literals;

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

void WritePrefabPerformanceReportIfRequested(
    const std::string& scenarioName,
    const PerformanceStageStatsSnapshot& snapshot,
    std::chrono::microseconds totalDuration);

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
    const std::string& scenarioName,
    NLS::Engine::Assets::PrefabArtifact artifact,
    std::chrono::microseconds* scenarioElapsed)
{
    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    const auto scenarioBegin = std::chrono::steady_clock::now();
    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);
    *scenarioElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - scenarioBegin);

    EXPECT_FALSE(instance.diagnostics.HasErrors());
    EXPECT_NE(instance.root, nullptr);

    const auto snapshot = stats.Snapshot();
    WritePrefabPerformanceReportIfRequested(scenarioName, snapshot, *scenarioElapsed);
    return snapshot;
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

void WritePrefabPerformanceReportIfRequested(
    const std::string& scenarioName,
    const PerformanceStageStatsSnapshot& snapshot,
    std::chrono::microseconds totalDuration)
{
    const auto* reportDirectory = std::getenv("NLS_PERFORMANCE_REPORT_DIR");
    if (reportDirectory == nullptr || std::string(reportDirectory).empty())
        return;

    std::filesystem::create_directories(reportDirectory);
    PerformanceBenchmarkRun run;
    run.scenarioName = scenarioName;
    run.runType = PerformanceBenchmarkRunType::Baseline;
    run.totalDuration = totalDuration;
    run.stageStats = snapshot;

    std::ofstream output(
        std::filesystem::path(reportDirectory) / (scenarioName + ".txt"),
        std::ios::binary | std::ios::trunc);
    output << FormatPerformanceBenchmarkReport(run);
}
}

TEST(AssetPrefabPerformanceTests, PrefabImportAndInstantiateEmitDiagnosticStages)
{
    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);

    const auto scenarioBegin = std::chrono::steady_clock::now();
    auto artifact = MakePrefabArtifact(
        "ProfiledPrefab",
        "10101010-1010-4010-8010-101010101010");

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(artifact, scene);
    const auto scenarioElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - scenarioBegin);

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

    WritePrefabPerformanceReportIfRequested(
        "Prefab_ImportAndInstantiate",
        snapshot,
        scenarioElapsed);
}

TEST(AssetPrefabPerformanceTests, PrefabInstantiationDoesNotSynchronouslyPrewarmResourcesByDefault)
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

TEST(AssetPrefabPerformanceTests, ExplicitSynchronousResourcePrewarmRetainsDiagnosticStages)
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

TEST(AssetPrefabPerformanceTests, DeferredAssetResolutionSuppressesSynchronousPrewarmOptIn)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_DeferSuppressesSyncResourcePrewarm",
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
    EXPECT_EQ(FindStage(snapshot, "WaitForResources"), nullptr);
    EXPECT_EQ(FindStage(snapshot, "UploadGpuResources"), nullptr);
}

TEST(AssetPrefabPerformanceTests, MissingBaselineAndMismatchedScenarioComparisonsAreInvalid)
{
    PerformanceBenchmarkRun optimized;
    optimized.scenarioName = "Prefab/RepeatedHotCache";
    optimized.runType = PerformanceBenchmarkRunType::Optimized;
    optimized.totalDuration = 50us;

    const auto missingBaseline = ComparePerformanceRuns(nullptr, &optimized);
    EXPECT_FALSE(missingBaseline.valid);
    EXPECT_EQ(missingBaseline.diagnostic, "performance-comparison-baseline-missing");

    PerformanceBenchmarkRun baseline;
    baseline.scenarioName = "Prefab/ColdCache";
    baseline.runType = PerformanceBenchmarkRunType::Baseline;
    baseline.totalDuration = 100us;

    const auto mismatch = ComparePerformanceRuns(&baseline, &optimized);
    EXPECT_FALSE(mismatch.valid);
    EXPECT_EQ(mismatch.diagnostic, "performance-comparison-scenario-mismatch");
}

TEST(AssetPrefabPerformanceTests, PrefabReportIncludesTopFiveAndComparisonOutput)
{
    PerformanceStageStats stats;
    stats.Record({
        PerformanceStageDomain::Prefab,
        "TotalInstantiate",
        PerformanceStageThread::Main,
        100us,
        {{"objectCount", 3u}}
    });
    stats.Record({
        PerformanceStageDomain::Prefab,
        "DeserializeComponents",
        PerformanceStageThread::Main,
        60us,
        {{"componentCount", 4u}}
    });
    stats.Record({
        PerformanceStageDomain::Prefab,
        "RegisterRenderers",
        PerformanceStageThread::Main,
        30us,
        {{"rendererCount", 1u}}
    });

    PerformanceBenchmarkRun baseline;
    baseline.scenarioName = "Prefab_Report";
    baseline.runType = PerformanceBenchmarkRunType::Baseline;
    baseline.totalDuration = 100us;
    baseline.stageStats = stats.Snapshot();

    PerformanceBenchmarkRun optimized = baseline;
    optimized.runType = PerformanceBenchmarkRunType::Optimized;
    optimized.totalDuration = 50us;

    const auto report = FormatPerformanceBenchmarkReport(baseline, 5u);
    EXPECT_NE(report.find("TopBottlenecks:"), std::string::npos);
    EXPECT_NE(report.find("TotalInstantiate"), std::string::npos);
    EXPECT_NE(report.find("DeserializeComponents"), std::string::npos);
    EXPECT_NE(report.find("objectCount"), std::string::npos);

    const auto comparison = FormatPerformanceComparisonReport(&baseline, &optimized, 5u);
    EXPECT_NE(comparison.find("Comparison: Prefab_Report"), std::string::npos);
    EXPECT_NE(comparison.find("change="), std::string::npos);
    EXPECT_NE(comparison.find("BaselineTotal="), std::string::npos);
    EXPECT_NE(comparison.find("OptimizedTotal="), std::string::npos);
}

TEST(AssetPrefabPerformanceTests, LargePrefabScenariosEmitObjectAndComponentScaleCounters)
{
    const struct
    {
        const char* name;
        const char* guid;
        size_t objectCount;
    } scenarios[] = {
        {"Prefab_SmallBaseline", "11111111-1111-4111-8111-111111111111", 3u},
        {"Prefab_100Objects", "12121212-1212-4212-8212-121212121212", 100u},
        {"Prefab_1000Objects", "13131313-1313-4313-8313-131313131313", 1000u}
    };

    for (const auto& scenario : scenarios)
    {
        auto artifact = MakePrefabArtifact(
            scenario.name,
            scenario.guid,
            scenario.objectCount);
        std::chrono::microseconds elapsed{0};
        const auto snapshot = RunPrefabInstantiationScenario(
            scenario.name,
            std::move(artifact),
            &elapsed);

        const auto* total = FindStage(snapshot, "TotalInstantiate");
        ASSERT_NE(total, nullptr) << scenario.name;
        EXPECT_GE(total->counters.at("objectCount"), scenario.objectCount) << scenario.name;

        const auto* allocate = FindStage(snapshot, "AllocateInstanceObjects");
        ASSERT_NE(allocate, nullptr) << scenario.name;
        EXPECT_EQ(allocate->counters.at("objectCount"), scenario.objectCount) << scenario.name;

        const auto* deserialize = FindStage(snapshot, "DeserializeComponents");
        ASSERT_NE(deserialize, nullptr) << scenario.name;
        EXPECT_GE(deserialize->counters.at("componentCount"), scenario.objectCount) << scenario.name;

        const auto* sceneAdd = FindStage(snapshot, "SceneAddGameObjects");
        ASSERT_NE(sceneAdd, nullptr) << scenario.name;
        ASSERT_TRUE(sceneAdd->counters.contains("objectCount")) << scenario.name;
        EXPECT_EQ(sceneAdd->counters.at("objectCount"), scenario.objectCount) << scenario.name;

        const auto* sceneComponents = FindStage(snapshot, "SceneRegisterComponents");
        ASSERT_NE(sceneComponents, nullptr) << scenario.name;
        ASSERT_TRUE(sceneComponents->counters.contains("componentCount")) << scenario.name;
        EXPECT_GE(sceneComponents->counters.at("componentCount"), scenario.objectCount) << scenario.name;

        const auto* rebuildFastAccess = FindStage(snapshot, "SceneRebuildFastAccess");
        ASSERT_NE(rebuildFastAccess, nullptr) << scenario.name;
        ASSERT_TRUE(rebuildFastAccess->counters.contains("rebuildCount")) << scenario.name;
        EXPECT_EQ(rebuildFastAccess->counters.at("rebuildCount"), 1u) << scenario.name;
        EXPECT_GT(elapsed.count(), 0) << scenario.name;
    }
}

TEST(AssetPrefabPerformanceTests, LargePrefabSceneRegistrationDefersFastAccessRebuilds)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_DeferredSceneRegistration",
        "15151515-1515-4515-8515-151515151515",
        1000u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_DeferredSceneRegistration",
        std::move(artifact),
        &elapsed);

    const auto* sceneAdd = FindStage(snapshot, "SceneAddGameObjects");
    ASSERT_NE(sceneAdd, nullptr);
    ASSERT_TRUE(sceneAdd->counters.contains("objectCount"));
    EXPECT_EQ(sceneAdd->counters.at("objectCount"), 1000u);

    const auto* rebuildFastAccess = FindStage(snapshot, "SceneRebuildFastAccess");
    ASSERT_NE(rebuildFastAccess, nullptr);
    ASSERT_TRUE(rebuildFastAccess->counters.contains("rebuildCount"));
    EXPECT_EQ(rebuildFastAccess->counters.at("rebuildCount"), 1u);
    EXPECT_GT(elapsed.count(), 0);
}

TEST(AssetPrefabPerformanceTests, DeferredSceneRegistrationPreservesFastAccessComponents)
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

TEST(AssetPrefabPerformanceTests, LargePrefabComponentRestoreUsesIndexedRecordLookup)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_IndexedRecordLookup",
        "17171717-1717-4717-8717-171717171717",
        1000u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_IndexedRecordLookup",
        std::move(artifact),
        &elapsed);

    const auto* deserialize = FindStage(snapshot, "DeserializeComponents");
    ASSERT_NE(deserialize, nullptr);
    ASSERT_TRUE(deserialize->counters.contains("indexedRecordLookupCount"));
    ASSERT_TRUE(deserialize->counters.contains("linearRecordLookupCount"));
    EXPECT_GE(deserialize->counters.at("indexedRecordLookupCount"), 1000u);
    EXPECT_EQ(deserialize->counters.at("linearRecordLookupCount"), 0u);
    EXPECT_GT(elapsed.count(), 0);
}

TEST(AssetPrefabPerformanceTests, PrefabInstantiationReportsBatchCreatePopulateAndBindPhases)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_BatchCreatePopulateBind",
        "1b1b1b1b-1b1b-4b1b-8b1b-1b1b1b1b1b1b",
        32u,
        2u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_BatchCreatePopulateBind",
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
    EXPECT_GE(createComponents->counters.at("indexedRecordLookupCount"), 32u);
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
    EXPECT_EQ(
        deserialize->counters.at("indexedRecordLookupCount"),
        deserialize->counters.at("restoredComponentCount"));
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

TEST(AssetPrefabPerformanceTests, LargePrefabComponentRestoreReportsReflectionSubstages)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_ReflectionSubstages",
        "18181818-1818-4818-8818-181818181818",
        1000u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_ReflectionSubstages",
        std::move(artifact),
        &elapsed);

    const auto* fieldLookup = FindStage(snapshot, "ReflectFieldLookup");
    const auto* convertValue = FindStage(snapshot, "ConvertPropertyValue");
    const auto* deserializeValue = FindStage(snapshot, "DeserializePropertyValue");
    const auto* setField = FindStage(snapshot, "SetReflectedField");

    ASSERT_NE(fieldLookup, nullptr);
    ASSERT_NE(convertValue, nullptr);
    ASSERT_NE(deserializeValue, nullptr);
    ASSERT_NE(setField, nullptr);
    EXPECT_LT(fieldLookup->counters.at("propertyCount"), 2000u)
        << "GameObject and Transform hot-path properties should bypass reflection field lookup.";
    EXPECT_LT(convertValue->counters.at("propertyCount"), 100u);
    EXPECT_LT(deserializeValue->counters.at("propertyCount"), 100u);
    EXPECT_LT(setField->counters.at("propertyCount"), 100u);
    EXPECT_GT(elapsed.count(), 0);
}

TEST(AssetPrefabPerformanceTests, TransformMathPropertiesAreSerializedAsDirectlyReadableObjects)
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

TEST(AssetPrefabPerformanceTests, LargePrefabComponentRestoreAppliesSimplePropertiesDirectly)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_DirectSimpleProperties",
        "19191919-1919-4919-8919-191919191919",
        1000u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_DirectSimpleProperties",
        std::move(artifact),
        &elapsed);

    const auto* directApply = FindStage(snapshot, "ApplyDirectPropertyValue");
    const auto* deserializeValue = FindStage(snapshot, "DeserializePropertyValue");
    const auto* externalReferences = FindStage(snapshot, "ResolveExternalReferences");
    const auto* restoreGameObjects = FindStage(snapshot, "RestoreGameObjectState");
    ASSERT_NE(directApply, nullptr);
    ASSERT_NE(deserializeValue, nullptr);
    ASSERT_NE(externalReferences, nullptr);
    ASSERT_NE(restoreGameObjects, nullptr);
    EXPECT_LT(directApply->counters.at("propertyCount"), 100u)
        << "GameObject and Transform hot-path properties should not inflate reflected direct-apply work.";
    ASSERT_TRUE(restoreGameObjects->counters.contains("directGameObjectPropertyCount"));
    EXPECT_GT(restoreGameObjects->counters.at("directGameObjectPropertyCount"), 3000u);
    EXPECT_LT(deserializeValue->counters.at("propertyCount"), 100u)
        << "Transform vector/quaternion properties should bypass JSON reflection deserialization on large prefab loads.";
    ASSERT_TRUE(externalReferences->counters.contains("runtimeResolvedGraphCopyCount"));
    EXPECT_EQ(externalReferences->counters.at("runtimeResolvedGraphCopyCount"), 0u)
        << "Prefabs with no external asset dependencies should instantiate directly from their prepared graph.";
    EXPECT_GT(elapsed.count(), 0);

    NLS::Engine::SceneSystem::Scene scene;
    const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(
        MakePrefabArtifact(
            "Prefab_DirectSimpleProperties_Semantics",
            "1a1a1a1a-1a1a-4a1a-8a1a-1a1a1a1a1a1a",
            3u),
        scene);

    ASSERT_FALSE(instance.diagnostics.HasErrors());
    ASSERT_NE(instance.root, nullptr);
    EXPECT_EQ(instance.root->GetName(), "Prefab_DirectSimpleProperties_Semantics");
    EXPECT_EQ(instance.root->GetTag(), "Prefab");

    const auto* light = instance.root->GetComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    EXPECT_FLOAT_EQ(light->GetIntensity(), 1.5f);
}

TEST(AssetPrefabPerformanceTests, LargePrefabTransformRestoreBatchesLocalTransformProperties)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_BatchedTransformRestore",
        "1b1b1b1b-1b1b-4b1b-8b1b-1b1b1b1b1b1b",
        1000u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_BatchedTransformRestore",
        std::move(artifact),
        &elapsed);

    const auto* directApply = FindStage(snapshot, "ApplyDirectPropertyValue");
    const auto* deserializeValue = FindStage(snapshot, "DeserializePropertyValue");
    ASSERT_NE(directApply, nullptr);
    ASSERT_NE(deserializeValue, nullptr);
    EXPECT_LT(directApply->counters.at("propertyCount"), 5000u)
        << "Transform localPosition/localRotation/localScale should be restored as one transform update.";
    EXPECT_LT(deserializeValue->counters.at("propertyCount"), 100u);
    EXPECT_GT(elapsed.count(), 0);
}

TEST(AssetPrefabPerformanceTests, LargePrefabSceneRegistrationAvoidsLinearTrackedObjectLookup)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_SceneRegistrationLookup",
        "1c1c1c1c-1c1c-4c1c-8c1c-1c1c1c1c1c1c",
        1000u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_SceneRegistrationLookup",
        std::move(artifact),
        &elapsed);

    const auto* sceneAdd = FindStage(snapshot, "SceneAddGameObjects");
    ASSERT_NE(sceneAdd, nullptr);
    ASSERT_TRUE(sceneAdd->counters.contains("hashTrackedLookupCount"));
    ASSERT_TRUE(sceneAdd->counters.contains("linearTrackedLookupCount"));
    EXPECT_EQ(sceneAdd->counters.at("hashTrackedLookupCount"), 1000u);
    EXPECT_EQ(sceneAdd->counters.at("linearTrackedLookupCount"), 0u)
        << "Large prefab scene registration should not call std::find for every object in the growing scene list.";
    EXPECT_GT(elapsed.count(), 0);
}

TEST(AssetPrefabPerformanceTests, LargePrefabWithoutAssetReferencesSkipsExternalBindingScan)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_NoAssetReferenceBindingScan",
        "1d1d1d1d-1d1d-4d1d-8d1d-1d1d1d1d1d1d",
        1000u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_NoAssetReferenceBindingScan",
        std::move(artifact),
        &elapsed);

    const auto* bindExternalReferences = FindStage(snapshot, "BindExternalAssetReferences");
    ASSERT_NE(bindExternalReferences, nullptr);
    ASSERT_TRUE(bindExternalReferences->counters.contains("candidateComponentCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("scannedComponentCount"));
    EXPECT_EQ(bindExternalReferences->counters.at("candidateComponentCount"), 0u);
    EXPECT_EQ(bindExternalReferences->counters.at("scannedComponentCount"), 0u);
    EXPECT_EQ(bindExternalReferences->counters.at("assetReferenceBindingCount"), 0u);
    EXPECT_GT(elapsed.count(), 0);
}

TEST(AssetPrefabPerformanceTests, LargePrefabSharedMeshAndMaterialReferencesReusePathResolution)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    ScopedServiceOverride<NLS::Core::ResourceManagement::MeshManager> meshManagerService(meshManager);
    ScopedServiceOverride<NLS::Core::ResourceManagement::MaterialManager> materialManagerService(materialManager);

    constexpr const char* meshPath = "Library/Artifacts/Perf/shared-body.nmesh";
    constexpr const char* materialPath = "Library/Artifacts/Perf/shared-body.nmat";
    auto* mesh = new NLS::Render::Resources::Mesh({}, {}, 0u);
    meshManager.RegisterResource(meshPath, mesh);
    auto* material = new NLS::Render::Resources::Material();
    const_cast<std::string&>(material->path) = materialPath;
    materialManager.RegisterResource(materialPath, material);

    const auto references = MakeSharedPrefabResourceReferences(
        "1f1f1f1f-1f1f-4f1f-8f1f-1f1f1f1f1f1f",
        meshPath,
        materialPath);
    auto artifact = MakePrefabArtifact(
        "Prefab_SharedResourceReferences",
        "20202020-2020-4020-8020-202020202020",
        1001u,
        2u,
        &references);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_SharedResourceReferences",
        std::move(artifact),
        &elapsed);

    const auto* bindExternalReferences = FindStage(snapshot, "BindExternalAssetReferences");
    ASSERT_NE(bindExternalReferences, nullptr);
    ASSERT_TRUE(bindExternalReferences->counters.contains("candidateComponentCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("assetReferenceElementBindingCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("meshReferenceCacheHitCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("meshReferenceCacheMissCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("materialReferenceCacheHitCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("materialReferenceCacheMissCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("meshResourceLookupCount"));
    ASSERT_TRUE(bindExternalReferences->counters.contains("materialResourceLookupCount"));

    EXPECT_EQ(bindExternalReferences->counters.at("candidateComponentCount"), 1000u);
    EXPECT_EQ(bindExternalReferences->counters.at("assetReferenceElementBindingCount"), 1000u);
    EXPECT_EQ(bindExternalReferences->counters.at("meshReferenceCacheMissCount"), 1u);
    EXPECT_EQ(bindExternalReferences->counters.at("materialReferenceCacheMissCount"), 1u);
    EXPECT_GE(bindExternalReferences->counters.at("meshReferenceCacheHitCount"), 499u);
    EXPECT_GE(bindExternalReferences->counters.at("materialReferenceCacheHitCount"), 499u);
    EXPECT_EQ(bindExternalReferences->counters.at("meshResourceLookupCount"), 1u);
    EXPECT_EQ(bindExternalReferences->counters.at("materialResourceLookupCount"), 1u);
    EXPECT_GT(elapsed.count(), 0);

    meshManager.UnloadResources();
    materialManager.UnloadResources();
}

TEST(AssetPrefabPerformanceTests, LargePrefabGameObjectStateBypassesReflectionFieldLookup)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_DirectGameObjectState",
        "1e1e1e1e-1e1e-4e1e-8e1e-1e1e1e1e1e1e",
        1000u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_DirectGameObjectState",
        std::move(artifact),
        &elapsed);

    const auto* restoreGameObjects = FindStage(snapshot, "RestoreGameObjectState");
    const auto* fieldLookup = FindStage(snapshot, "ReflectFieldLookup");
    ASSERT_NE(restoreGameObjects, nullptr);
    ASSERT_NE(fieldLookup, nullptr);
    ASSERT_TRUE(restoreGameObjects->counters.contains("directGameObjectPropertyCount"));
    EXPECT_GE(restoreGameObjects->counters.at("directGameObjectPropertyCount"), 4000u);
    EXPECT_LT(fieldLookup->counters.at("propertyCount"), 2000u)
        << "GameObject name/tag/active/layer should restore without the generic reflection field lookup loop.";
    EXPECT_GT(elapsed.count(), 0);
}

TEST(AssetPrefabPerformanceTests, MultiRendererPrefabReportsRendererCount)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_MultiRenderer",
        "14141414-1414-4414-8414-141414141414",
        64u,
        2u);

    std::chrono::microseconds elapsed{0};
    const auto snapshot = RunPrefabInstantiationScenario(
        "Prefab_MultiRenderer",
        std::move(artifact),
        &elapsed);

    const auto* registerRenderers = FindStage(snapshot, "RegisterRenderers");
    ASSERT_NE(registerRenderers, nullptr);
    ASSERT_TRUE(registerRenderers->counters.contains("rendererCount"));
    EXPECT_GE(registerRenderers->counters.at("rendererCount"), 31u);
}
