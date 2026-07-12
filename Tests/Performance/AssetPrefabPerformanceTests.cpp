#include <gtest/gtest.h>

#include "Assets/ArtifactManifest.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetPath.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/SceneSystem/Scene.h"
#include "GameObject.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ServiceLocator.h"
#include "Profiling/PerformanceStageStats.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
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

bool IsEnvironmentFlagEnabled(const char* name)
{
    const auto* value = std::getenv(name);
    if (value == nullptr)
        return false;

    const std::string flag(value);
    std::string normalized = flag;
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return !normalized.empty() &&
        normalized != "0" &&
        normalized != "false" &&
        normalized != "off" &&
        normalized != "no";
}

std::string FindPrimaryPrefabSubAssetKey(const NLS::Core::Assets::ArtifactManifest& manifest)
{
    const auto isPrefabSubAsset = [](const NLS::Core::Assets::ImportedArtifact& artifact)
    {
        return artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab;
    };

    if (!manifest.primarySubAssetKey.empty())
    {
        const auto primary = std::find_if(
            manifest.subAssets.begin(),
            manifest.subAssets.end(),
            [&manifest, &isPrefabSubAsset](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.subAssetKey == manifest.primarySubAssetKey &&
                    isPrefabSubAsset(artifact);
            });
        if (primary != manifest.subAssets.end())
            return primary->subAssetKey;
    }

    const auto firstPrefab = std::find_if(
        manifest.subAssets.begin(),
        manifest.subAssets.end(),
        isPrefabSubAsset);
    return firstPrefab != manifest.subAssets.end() ? firstPrefab->subAssetKey : std::string {};
}

void WriteTextPerformanceReportIfRequested(
    const std::string& reportName,
    const std::string& contents)
{
    const auto* reportDirectory = std::getenv("NLS_PERFORMANCE_REPORT_DIR");
    if (reportDirectory == nullptr || std::string(reportDirectory).empty())
        return;

    std::filesystem::create_directories(reportDirectory);
    std::ofstream output(
        std::filesystem::path(reportDirectory) / (reportName + ".txt"),
        std::ios::binary | std::ios::trunc);
    output << contents;
}

void AppendTextPerformanceReportIfRequested(
    const std::string& reportName,
    const std::string& contents)
{
    const auto* reportDirectory = std::getenv("NLS_PERFORMANCE_REPORT_DIR");
    if (reportDirectory == nullptr || std::string(reportDirectory).empty())
        return;

    std::filesystem::create_directories(reportDirectory);
    std::ofstream output(
        std::filesystem::path(reportDirectory) / (reportName + ".txt"),
        std::ios::binary | std::ios::app);
    output << contents;
    output.flush();
}

void AppendNewSponzaProgressIfRequested(const std::string& step)
{
    AppendTextPerformanceReportIfRequested(
        "NewSponza_PreparedCacheProgress",
        step + "\n");
}

const char* ImportPhaseToString(const NLS::Editor::Assets::ImportPhase phase)
{
    using NLS::Editor::Assets::ImportPhase;
    switch (phase)
    {
    case ImportPhase::Queued:
        return "Queued";
    case ImportPhase::DependencyCopy:
        return "DependencyCopy";
    case ImportPhase::SourceParse:
        return "SourceParse";
    case ImportPhase::IntermediateConversion:
        return "IntermediateConversion";
    case ImportPhase::ArtifactWrite:
        return "ArtifactWrite";
    case ImportPhase::Postprocess:
        return "Postprocess";
    case ImportPhase::Commit:
        return "Commit";
    case ImportPhase::Finished:
        return "Finished";
    }
    return "Unknown";
}

const char* ImportTerminalStatusToString(const NLS::Editor::Assets::ImportJobTerminalStatus status)
{
    using NLS::Editor::Assets::ImportJobTerminalStatus;
    switch (status)
    {
    case ImportJobTerminalStatus::None:
        return "None";
    case ImportJobTerminalStatus::Succeeded:
        return "Succeeded";
    case ImportJobTerminalStatus::Failed:
        return "Failed";
    case ImportJobTerminalStatus::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
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

TEST(AssetPrefabPerformanceTests, NewSponzaImportedPrefabPerformanceReport)
{
    if (!IsEnvironmentFlagEnabled("NLS_RUN_NEWSPONZA_PREFAB_PERF"))
        GTEST_SKIP() << "Set NLS_RUN_NEWSPONZA_PREFAB_PERF=1 to run the real NewSponza prefab benchmark.";

    constexpr const char* kAssetPath = "Assets/Model/main_sponza/NewSponza_Main_glTF_003.gltf";
    const auto projectRoot = std::filesystem::path(NLS_ROOT_DIR) / "TestProject";
    const auto absoluteAssetPath = projectRoot / "Assets" / "Model" / "main_sponza" /
        "NewSponza_Main_glTF_003.gltf";
    if (!std::filesystem::exists(absoluteAssetPath))
        GTEST_SKIP() << "NewSponza test asset is missing: " << absoluteAssetPath.generic_string();

    NLS::Editor::Assets::AssetDatabaseFacade database(
        NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));

    WriteTextPerformanceReportIfRequested(
        "NewSponza_SetupProgress",
        std::string("Asset: ") + kAssetPath + '\n');
    AppendTextPerformanceReportIfRequested(
        "NewSponza_SetupProgress",
        "RefreshKnownSourceAssets begin\n");
    const auto refreshBegin = std::chrono::steady_clock::now();
    const std::vector<std::filesystem::path> refreshPaths {absoluteAssetPath};
    ASSERT_TRUE(database.RefreshKnownSourceAssets(refreshPaths));
    const auto refreshElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - refreshBegin);
    AppendTextPerformanceReportIfRequested(
        "NewSponza_SetupProgress",
        "RefreshKnownSourceAssets end elapsedUs=" + std::to_string(refreshElapsed.count()) + "\n");

    std::chrono::microseconds importElapsed{0};
    PerformanceStageStats importStats;
    NLS::Editor::Assets::ImportProgressTracker importProgress;
    WriteTextPerformanceReportIfRequested(
        "NewSponza_ImportProgress",
        std::string("Asset: ") + kAssetPath + '\n');
    const auto importProgressTraceBegin = std::chrono::steady_clock::now();
    auto lastLoggedPhase = std::make_shared<NLS::Editor::Assets::ImportPhase>(
        NLS::Editor::Assets::ImportPhase::Finished);
    auto lastLoggedProgressBucket = std::make_shared<int>(-1);
    importProgress.Subscribe(
        [importProgressTraceBegin, lastLoggedPhase, lastLoggedProgressBucket](
            const NLS::Editor::Assets::ImportProgressEvent& event)
    {
        const bool meshProgressMessage =
            event.message.rfind("Converting mesh:", 0u) == 0u ||
            event.message.rfind("Converted mesh:", 0u) == 0u;
        const auto progressBucket = static_cast<int>(event.normalizedProgress * 100.0);
        if (meshProgressMessage &&
            event.phase == *lastLoggedPhase &&
            progressBucket == *lastLoggedProgressBucket &&
            event.terminalStatus == NLS::Editor::Assets::ImportJobTerminalStatus::None)
        {
            return;
        }
        *lastLoggedPhase = event.phase;
        *lastLoggedProgressBucket = progressBucket;

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - importProgressTraceBegin).count();
        std::ostringstream line;
        line << "elapsedMs=" << elapsedMs
            << " job=" << event.jobId.value
            << " phase=" << ImportPhaseToString(event.phase)
            << " progress=" << event.normalizedProgress
            << " terminal=" << ImportTerminalStatusToString(event.terminalStatus)
            << " message=\"" << event.message << "\""
            << " source=\"" << event.sourcePath << "\""
            << '\n';
        AppendTextPerformanceReportIfRequested("NewSponza_ImportProgress", line.str());
    });
    bool imported = false;
    {
        PerformanceStageStatsCapture capture(importStats);
        const auto importBegin = std::chrono::steady_clock::now();
        imported = IsEnvironmentFlagEnabled("NLS_NEWSPONZA_FORCE_REIMPORT")
            ? database.ReimportAssetFromCurrentDatabase(kAssetPath, importProgress, 1u)
            : database.ImportAssetFromCurrentDatabase(kAssetPath, importProgress, 1u);
        importElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - importBegin);
    }
    WritePrefabPerformanceReportIfRequested(
        "NewSponza_ImportAsset",
        importStats.Snapshot(),
        importElapsed);
    ASSERT_TRUE(imported);

    const auto manifest = database.GetArtifactManifestForAssetPath(kAssetPath);
    ASSERT_TRUE(manifest.has_value());
    const auto prefabSubAssetKey = FindPrimaryPrefabSubAssetKey(*manifest);
    ASSERT_FALSE(prefabSubAssetKey.empty());

    std::chrono::microseconds loadElapsed{0};
    PerformanceStageStats loadStats;
    std::optional<NLS::Engine::Assets::PrefabArtifact> prefab;
    {
        PerformanceStageStatsCapture capture(loadStats);
        const auto loadBegin = std::chrono::steady_clock::now();
        prefab = database.LoadPrefabArtifactAtPath(kAssetPath, prefabSubAssetKey);
        loadElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - loadBegin);
    }
    const auto loadSnapshot = loadStats.Snapshot();
    WritePrefabPerformanceReportIfRequested(
        "NewSponza_LoadPrefabArtifact",
        loadSnapshot,
        loadElapsed);
    ASSERT_TRUE(prefab.has_value());
    EXPECT_TRUE(prefab->generatedModelPrefab);

    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    auto runInstantiation = [&prefab](const std::string& scenarioName, std::chrono::microseconds& elapsed)
    {
        PerformanceStageStats stats;
        PerformanceStageStatsCapture capture(stats);

        const auto scenarioBegin = std::chrono::steady_clock::now();
        NLS::Engine::SceneSystem::Scene scene;
        const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(*prefab, scene);
        elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - scenarioBegin);

        EXPECT_FALSE(instance.diagnostics.HasErrors());
        EXPECT_NE(instance.root, nullptr);

        const auto snapshot = stats.Snapshot();
        WritePrefabPerformanceReportIfRequested(scenarioName, snapshot, elapsed);
        return snapshot;
    };
    auto runInstantiationNoCapture = [&prefab](std::chrono::microseconds& elapsed)
    {
        const auto scenarioBegin = std::chrono::steady_clock::now();
        NLS::Engine::SceneSystem::Scene scene;
        const auto instance = NLS::Engine::Assets::InstantiatePrefabArtifact(*prefab, scene);
        elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - scenarioBegin);

        EXPECT_FALSE(instance.diagnostics.HasErrors());
        EXPECT_NE(instance.root, nullptr);
        return scene.GetGameObjects().size();
    };

    std::chrono::microseconds coldElapsed{0};
    const auto coldSnapshot = runInstantiation("NewSponza_ColdInstantiate", coldElapsed);
    std::chrono::microseconds hotElapsed{0};
    const auto hotSnapshot = runInstantiation("NewSponza_HotInstantiate", hotElapsed);
    std::chrono::microseconds hotNoCaptureElapsed{0};
    const auto hotNoCaptureSceneObjectCount = runInstantiationNoCapture(hotNoCaptureElapsed);

    const auto* coldTotal = FindStage(coldSnapshot, "TotalInstantiate");
    ASSERT_NE(coldTotal, nullptr);
    ASSERT_TRUE(coldTotal->counters.contains("objectCount"));
    const auto* hotTotal = FindStage(hotSnapshot, "TotalInstantiate");
    ASSERT_NE(hotTotal, nullptr);
    ASSERT_TRUE(hotTotal->counters.contains("objectCount"));
    EXPECT_EQ(hotTotal->counters.at("objectCount"), coldTotal->counters.at("objectCount"));

    const auto* hotPlan = FindStage(hotSnapshot, "PrepareInstantiatePlan");
    ASSERT_NE(hotPlan, nullptr);
    ASSERT_TRUE(hotPlan->counters.contains("instantiatePlanCacheHitCount"));
    const auto* hotResolve = FindStage(hotSnapshot, "ResolveExternalReferences");
    ASSERT_NE(hotResolve, nullptr);
    ASSERT_TRUE(hotResolve->counters.contains("runtimeResolvedGraphCopyCount"));

    std::ostringstream summary;
    summary << "Asset: " << kAssetPath << '\n';
    summary << "ProjectRoot: " << projectRoot.generic_string() << '\n';
    summary << "PrefabSubAssetKey: " << prefabSubAssetKey << '\n';
    summary << "ManifestSubAssetCount: " << manifest->subAssets.size() << '\n';
    summary << "Refresh: " << refreshElapsed.count() << "us\n";
    summary << "ImportAsset: " << importElapsed.count() << "us\n";
    summary << "LoadPrefabArtifact: " << loadElapsed.count() << "us\n";
    summary << "ColdInstantiate: " << coldElapsed.count() << "us\n";
    summary << "HotInstantiate: " << hotElapsed.count() << "us\n";
    summary << "HotInstantiateNoCapture: " << hotNoCaptureElapsed.count() << "us\n";
    summary << "HotInstantiateNoCaptureSceneObjectCount: " << hotNoCaptureSceneObjectCount << '\n';
    summary << "ObjectCount: " << hotTotal->counters.at("objectCount") << '\n';
    summary << "DependencyCount: " << hotTotal->counters.at("dependencyCount") << '\n';
    summary << "HotInstantiatePlanCacheHitCount: "
        << hotPlan->counters.at("instantiatePlanCacheHitCount") << '\n';
    summary << "HotRuntimeResolvedGraphCopyCount: "
        << hotResolve->counters.at("runtimeResolvedGraphCopyCount") << '\n';
    if (hotResolve->counters.contains("runtimeResolvedGraphCacheHitCount"))
    {
        summary << "HotRuntimeResolvedGraphCacheHitCount: "
            << hotResolve->counters.at("runtimeResolvedGraphCacheHitCount") << '\n';
    }
    WriteTextPerformanceReportIfRequested("NewSponza_Summary", summary.str());
}

TEST(AssetPrefabPerformanceTests, NewSponzaPreparedPrefabCacheRestartPerformanceReport)
{
    if (!IsEnvironmentFlagEnabled("NLS_RUN_NEWSPONZA_PREFAB_PERF"))
        GTEST_SKIP() << "Set NLS_RUN_NEWSPONZA_PREFAB_PERF=1 to run the real NewSponza prefab benchmark.";

    constexpr const char* kAssetPath = "Assets/Model/main_sponza/NewSponza_Main_glTF_003.gltf";
    const auto projectRoot = std::filesystem::path(NLS_ROOT_DIR) / "TestProject";
    const auto absoluteAssetPath = projectRoot / "Assets" / "Model" / "main_sponza" /
        "NewSponza_Main_glTF_003.gltf";
    if (!std::filesystem::exists(absoluteAssetPath))
        GTEST_SKIP() << "NewSponza test asset is missing: " << absoluteAssetPath.generic_string();

    if (IsEnvironmentFlagEnabled("NLS_NEWSPONZA_RESET_PREPARED_PREFAB_CACHE"))
    {
        std::error_code error;
        std::filesystem::remove_all(projectRoot / "Library" / "PreparedPrefabCache", error);
        ASSERT_FALSE(error) << error.message();
    }

    NLS::Editor::Assets::AssetDatabaseFacade database(
        NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
    const std::vector<std::filesystem::path> refreshPaths {absoluteAssetPath};
    ASSERT_TRUE(database.RefreshKnownSourceAssets(refreshPaths));

    NLS::Editor::Assets::ImportProgressTracker importProgress;
    const bool imported = IsEnvironmentFlagEnabled("NLS_NEWSPONZA_FORCE_REIMPORT")
        ? database.ReimportAssetFromCurrentDatabase(kAssetPath, importProgress, 1u)
        : database.ImportAssetFromCurrentDatabase(kAssetPath, importProgress, 1u);
    ASSERT_TRUE(imported);

    const auto guid = database.AssetPathToGUID(kAssetPath);
    ASSERT_FALSE(guid.empty());
    const auto assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
    const auto manifest = database.GetArtifactManifestForAssetPath(kAssetPath);
    ASSERT_TRUE(manifest.has_value());
    const auto prefabSubAssetKey = FindPrimaryPrefabSubAssetKey(*manifest);
    ASSERT_FALSE(prefabSubAssetKey.empty());

    NLS::Editor::Assets::UnifiedPrefabLoadRequest request;
    request.source = NLS::Editor::Assets::NormalizePrefabSourceIdentity(
        projectRoot,
        kAssetPath,
        prefabSubAssetKey,
        assetId,
        NLS::Core::Assets::AssetType::ModelScene);
    request.loadMode = NLS::Editor::Assets::UnifiedPrefabLoadMode::SceneRestore;
    request.ownerKind = NLS::Editor::Assets::UnifiedPrefabOwnerKind::SceneInstance;
    request.ownerScopeId = "scene:new-sponza-prepared-cache-performance";
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    request.allowPending = false;

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(projectRoot / "Assets");
    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
    AppendNewSponzaProgressIfRequested("before-cold-load");

    std::chrono::microseconds coldElapsed{0};
    PerformanceStageStats coldStats;
    NLS::Editor::Assets::UnifiedPrefabSharedLoadResult coldLoad;
    {
        PerformanceStageStatsCapture capture(coldStats);
        const auto begin = std::chrono::steady_clock::now();
        coldLoad = bridge.LoadUnifiedPrefabShared(request);
        coldElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin);
    }
    AppendNewSponzaProgressIfRequested("after-cold-load");
    if (coldLoad.prefab == nullptr || !coldLoad.key.has_value())
    {
        std::ostringstream failure;
        failure << "Asset: " << kAssetPath << '\n';
        failure << "ProjectRoot: " << projectRoot.generic_string() << '\n';
        failure << "PrefabSubAssetKey: " << prefabSubAssetKey << '\n';
        failure << "ColdUnifiedSharedLoad: " << coldElapsed.count() << "us\n";
        failure << "HasPrefab: " << (coldLoad.prefab != nullptr ? 1 : 0) << '\n';
        failure << "HasKey: " << (coldLoad.key.has_value() ? 1 : 0) << '\n';
        failure << "Pending: " << (coldLoad.pending ? 1 : 0) << '\n';
        failure << "RendererDependencyMissing: " << (coldLoad.rendererDependencyMissing ? 1 : 0) << '\n';
        failure << "DiagnosticCode: " << coldLoad.diagnosticCode << '\n';
        failure << "DiagnosticMessage: " << coldLoad.diagnosticMessage << '\n';
        WriteTextPerformanceReportIfRequested("NewSponza_PreparedCacheColdLoadFailure", failure.str());
    }
    ASSERT_NE(coldLoad.prefab, nullptr);
    ASSERT_TRUE(coldLoad.key.has_value());
    AppendNewSponzaProgressIfRequested("after-cold-prefab-key-asserts");
    WritePrefabPerformanceReportIfRequested(
        "NewSponza_PreparedCacheColdLoad",
        coldStats.Snapshot(),
        coldElapsed);
    ASSERT_NE(coldLoad.prefab->runtimeResolvedGraph, nullptr);
    AppendNewSponzaProgressIfRequested("after-cold-runtime-graph-assert");

    NLS::Editor::Assets::ClearImportedPrefabHotCacheForTesting();
    ASSERT_EQ(NLS::Editor::Assets::GetImportedPrefabHotCacheEntryCountForTesting(), 0u);
    AppendNewSponzaProgressIfRequested("before-prepared-restart-load");

    std::chrono::microseconds preparedElapsed{0};
    PerformanceStageStats preparedStats;
    NLS::Editor::Assets::UnifiedPrefabSharedLoadResult preparedLoad;
    {
        PerformanceStageStatsCapture capture(preparedStats);
        const auto begin = std::chrono::steady_clock::now();
        preparedLoad = bridge.LoadUnifiedPrefabShared(request);
        preparedElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin);
    }
    AppendNewSponzaProgressIfRequested("after-prepared-restart-load");
    ASSERT_NE(preparedLoad.prefab, nullptr);
    ASSERT_TRUE(preparedLoad.key.has_value());
    ASSERT_NE(preparedLoad.prefab->runtimeResolvedGraph, nullptr);
    AppendNewSponzaProgressIfRequested("after-prepared-runtime-graph-assert");
    EXPECT_EQ(preparedLoad.key->runtimeCacheIdentity, coldLoad.key->runtimeCacheIdentity);
    EXPECT_EQ(
        preparedLoad.prefab->runtimeResolvedGraphFingerprint,
        coldLoad.prefab->runtimeResolvedGraphFingerprint);
    AppendNewSponzaProgressIfRequested("after-prepared-fingerprint-asserts");
    WritePrefabPerformanceReportIfRequested(
        "NewSponza_PreparedCacheRestartLoad",
        preparedStats.Snapshot(),
        preparedElapsed);

    std::ostringstream summary;
    summary << "Asset: " << kAssetPath << '\n';
    summary << "ProjectRoot: " << projectRoot.generic_string() << '\n';
    summary << "PrefabSubAssetKey: " << prefabSubAssetKey << '\n';
    summary << "ManifestSubAssetCount: " << manifest->subAssets.size() << '\n';
    summary << "ColdUnifiedSharedLoad: " << coldElapsed.count() << "us\n";
    summary << "PreparedRestartUnifiedSharedLoad: " << preparedElapsed.count() << "us\n";
    if (coldElapsed.count() > 0)
    {
        const auto savedPercent =
            (coldElapsed.count() - preparedElapsed.count()) * 100 / coldElapsed.count();
        summary << "PreparedRestartSavedPercent: " << savedPercent << "%\n";
    }
    summary << "ObjectCount: " << preparedLoad.prefab->graph.objects.size() << '\n';
    summary << "DependencyCount: " << preparedLoad.prefab->resolvedAssets.size() << '\n';
    WriteTextPerformanceReportIfRequested("NewSponza_PreparedCacheSummary", summary.str());
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

TEST(AssetPrefabPerformanceTests, LargePrefabComponentRestoreUsesCompiledComponentPlan)
{
    auto artifact = MakePrefabArtifact(
        "Prefab_CompiledComponentPlan",
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
    ASSERT_TRUE(deserialize->counters.contains("directComponentBindingPopulateCount"));
    EXPECT_EQ(deserialize->counters.at("indexedRecordLookupCount"), 0u);
    EXPECT_EQ(deserialize->counters.at("linearRecordLookupCount"), 0u);
    EXPECT_EQ(
        deserialize->counters.at("directComponentBindingPopulateCount"),
        deserialize->counters.at("restoredComponentCount"))
        << "Compiled prefab instantiation should populate the component records it just bound instead of re-resolving component ids through the object graph.";
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
