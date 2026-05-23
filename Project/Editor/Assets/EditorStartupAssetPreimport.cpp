#include "Assets/EditorStartupAssetPreimport.h"

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorAssetPath.h"
#include "Debug/Logger.h"

namespace NLS::Editor::Assets
{
namespace
{
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
    result.importedAssetCount = database.GetCompletedImportCount();
    result.hadRunningJobsAfterCompletion = tracker.HasRunningJobs();
    result.diagnostics = database.GetDiagnostics();
    return result;
}
}
