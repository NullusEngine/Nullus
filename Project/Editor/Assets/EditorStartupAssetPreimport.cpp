#include "Assets/EditorStartupAssetPreimport.h"

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorAssetPath.h"
#include "Debug/Logger.h"
#include "Guid.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <unordered_map>

namespace NLS::Editor::Assets
{
namespace
{
constexpr const char* kProjectStandardPbrShaderPath = "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader";

struct PreparedPrefabCachePreflightSummary
{
    size_t attemptedCount = 0u;
    size_t preparedCount = 0u;
};

void LogStartupAssetPreimportProgress(const ImportProgressEvent& event)
{
    NLS_LOG_INFO("[StartupAssetPreimport] " + FormatStartupAssetPreimportProgressLabel(event));
}

void PublishStartupAssetPreimportProgress(
    const StartupAssetPreimportProgressSink& progressSink,
    const ImportPhase phase,
    const double normalizedProgress,
    std::string message,
    std::string sourcePath)
{
    ImportProgressEvent event;
    event.sourcePath = std::move(sourcePath);
    event.targetPlatform = "editor";
    event.phase = phase;
    event.normalizedProgress = normalizedProgress;
    event.message = std::move(message);
    LogStartupAssetPreimportProgress(event);
    if (progressSink)
        progressSink(event);
}

PreparedPrefabCachePreflightSummary PreflightPreparedPrefabCache(
    const std::filesystem::path& projectRoot,
    const AssetDatabaseFacade& database,
    const size_t maxPreflightCount,
    const std::chrono::milliseconds maxPreflightDuration,
    const std::vector<std::string>& priorityAssetPaths,
    const StartupAssetPreimportProgressSink& progressSink)
{
    PreparedPrefabCachePreflightSummary summary;
    if (maxPreflightCount == 0u)
        return summary;

    auto modelAssets = database.FindAssets("type:model-scene", {});
    for (auto& assetPath : modelAssets)
        assetPath = NormalizeEditorAssetPath(assetPath);
    modelAssets.erase(
        std::remove_if(
            modelAssets.begin(),
            modelAssets.end(),
            [](const std::string& assetPath)
            {
                return assetPath.rfind("Assets/", 0u) != 0u;
            }),
        modelAssets.end());
    std::unordered_map<std::string, size_t> priorityByAssetPath;
    for (size_t index = 0u; index < priorityAssetPaths.size(); ++index)
    {
        const auto normalizedPath = NormalizeEditorAssetPath(priorityAssetPaths[index]);
        if (!normalizedPath.empty() &&
            normalizedPath.rfind("Assets/", 0u) == 0u)
        {
            priorityByAssetPath.emplace(normalizedPath, index);
        }
    }
    std::sort(
        modelAssets.begin(),
        modelAssets.end(),
        [&priorityByAssetPath](const std::string& lhs, const std::string& rhs)
        {
            const auto lhsPriority = priorityByAssetPath.find(lhs);
            const auto rhsPriority = priorityByAssetPath.find(rhs);
            const bool lhsHasPriority = lhsPriority != priorityByAssetPath.end();
            const bool rhsHasPriority = rhsPriority != priorityByAssetPath.end();
            if (lhsHasPriority != rhsHasPriority)
                return lhsHasPriority;
            if (lhsHasPriority && rhsHasPriority && lhsPriority->second != rhsPriority->second)
                return lhsPriority->second < rhsPriority->second;
            return lhs < rhs;
        });
    modelAssets.erase(std::unique(modelAssets.begin(), modelAssets.end()), modelAssets.end());
    if (modelAssets.empty())
        return summary;

    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        0.96,
        "Preparing imported prefab cache",
        std::to_string(std::min(maxPreflightCount, modelAssets.size())) + " model assets");

    EditorAssetDragDropBridge bridge(projectRoot / "Assets");
    const auto preflightBegin = std::chrono::steady_clock::now();
    for (const auto& assetPath : modelAssets)
    {
        if (summary.attemptedCount >= maxPreflightCount)
            break;
        if (summary.attemptedCount > 0u &&
            maxPreflightDuration.count() >= 0 &&
            std::chrono::steady_clock::now() - preflightBegin >= maxPreflightDuration)
        {
            break;
        }
        ++summary.attemptedCount;

        const auto guid = database.AssetPathToGUID(assetPath);
        auto assetId = guid.empty()
            ? NLS::Core::Assets::AssetId {}
            : NLS::Core::Assets::AssetId(NLS::Guid::Parse(guid));
        if (!assetId.IsValid())
            continue;

        UnifiedPrefabLoadRequest request;
        request.source = NormalizePrefabSourceIdentity(
            projectRoot,
            assetPath,
            "prefab:" + std::filesystem::path(assetPath).stem().generic_string(),
            assetId,
            NLS::Core::Assets::AssetType::ModelScene);
        request.loadMode = UnifiedPrefabLoadMode::Prewarm;
        request.ownerKind = UnifiedPrefabOwnerKind::AsyncJob;
        request.ownerScopeId = "startup-prepared-prefab-cache";
        request.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
        request.allowPending = true;

        if (bridge.PreloadPreparedPrefabHotCache(request))
            ++summary.preparedCount;
    }
    return summary;
}
}

std::string FormatStartupAssetPreimportProgressLabel(const ImportProgressEvent& event)
{
    if (event.message.empty())
        return event.sourcePath;
    if (event.sourcePath.empty())
        return event.message;

    auto fileName = std::filesystem::path(event.sourcePath).filename().generic_string();
    if (fileName.empty())
        fileName = event.sourcePath;
    if (fileName == event.sourcePath)
        return event.message + ": " + event.sourcePath;
    return event.message + ": " + fileName + " (" + event.sourcePath + ")";
}

StartupAssetPreimportResult RunBlockingStartupAssetPreimport(
    const StartupAssetPreimportOptions& options,
    StartupAssetPreimportProgressSink progressSink)
{
    StartupAssetPreimportResult result;
    if (options.projectRoot.empty())
        return result;

    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(options.projectRoot));
    ImportProgressTracker tracker;
    tracker.Subscribe([progressSink](const ImportProgressEvent& event)
    {
        LogStartupAssetPreimportProgress(event);
        if (progressSink)
            progressSink(event);
    });

    AssetPreimportScheduler scheduler;
    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        0.01,
        "Scanning project assets",
        "Assets");
    if (!database.Refresh())
    {
        result.diagnostics = database.GetDiagnostics();
        result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
        return result;
    }

    if (database.AssetPathToGUID(kProjectStandardPbrShaderPath).empty())
    {
        NLS::Core::Assets::AssetDiagnostic diagnostic;
        diagnostic.severity = NLS::Core::Assets::AssetDiagnosticSeverity::Error;
        diagnostic.code = "startup-standard-pbr-source-missing";
        diagnostic.path = kProjectStandardPbrShaderPath;
        diagnostic.message =
            "Startup asset preimport could not find the built-in StandardPBR ShaderLab source in the mounted engine shader root.";
        result.diagnostics.push_back(std::move(diagnostic));
        result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
        return result;
    }

    size_t internalImportCount = 0u;
    if (!database.IsArtifactManifestCurrentForAssetPath(kProjectStandardPbrShaderPath))
    {
        PublishStartupAssetPreimportProgress(
            progressSink,
            ImportPhase::Queued,
            0.025,
            "Importing built-in shader dependency",
            kProjectStandardPbrShaderPath);
        const auto beforeInternalImportCount = database.GetCompletedImportCount();
        if (!database.ImportAsset(kProjectStandardPbrShaderPath))
        {
            NLS_LOG_WARNING(
                "[StartupAssetPreimport] Built-in StandardPBR ShaderLab artifact import failed; "
                "model material imports will keep referencing the ShaderLab source and shader variants can be rebuilt later.");
        }
        else
        {
            internalImportCount = database.GetCompletedImportCount() - beforeInternalImportCount;
            if (!database.Refresh())
            {
                result.diagnostics = database.GetDiagnostics();
                result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
                return result;
            }
        }
    }

    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        0.03,
        "Planning startup asset imports",
        "Assets");
    const auto plan = scheduler.BuildPlan(database, AssetPreimportReason::EditorStartup);
    result.plannedAssetCount = plan.assetPaths.size();
    PublishStartupAssetPreimportProgress(
        progressSink,
        ImportPhase::Queued,
        plan.assetPaths.empty() ? 1.0 : 0.04,
        plan.assetPaths.empty()
            ? "Startup asset artifacts are current"
            : "Importing startup assets",
        plan.assetPaths.empty()
            ? "Assets"
            : std::to_string(plan.assetPaths.size()) + " assets");

    result.succeeded = scheduler.RunAlreadyPlanned(
        database,
        tracker,
        {AssetPreimportReason::EditorStartup, {}},
        plan);
    result.importedAssetCount =
        database.GetCompletedImportCount() >= internalImportCount
            ? database.GetCompletedImportCount() - internalImportCount
            : 0u;
    if (result.succeeded)
    {
        const auto preflight = PreflightPreparedPrefabCache(
            options.projectRoot,
            database,
            options.maxPreparedPrefabCachePreflightCount,
            options.maxPreparedPrefabCachePreflightDuration,
            options.priorityPreparedPrefabAssetPaths,
            progressSink);
        result.preparedPrefabCachePreflightAttemptCount = preflight.attemptedCount;
        result.preparedPrefabCachePreflightCount = preflight.preparedCount;
    }
    result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
    result.diagnostics = database.GetDiagnostics();
    return result;
}
}
