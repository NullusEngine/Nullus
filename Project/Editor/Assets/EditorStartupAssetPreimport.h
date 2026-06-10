#pragma once

#include "Assets/ImportProgressTracker.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
struct StartupAssetPreimportOptions
{
    std::filesystem::path projectRoot;
    size_t maxPreparedPrefabCachePreflightCount = 64u;
    std::chrono::milliseconds maxPreparedPrefabCachePreflightDuration {100};
    std::vector<std::string> priorityPreparedPrefabAssetPaths;
};

struct StartupAssetPreimportResult
{
    bool succeeded = false;
    size_t plannedAssetCount = 0u;
    size_t importedAssetCount = 0u;
    size_t preparedPrefabCachePreflightAttemptCount = 0u;
    size_t preparedPrefabCachePreflightCount = 0u;
    size_t prewarmedMaterialArtifactCount = 0u;
    bool hadRunningJobsAfterCompletion = false;
    NLS::Core::Assets::AssetDiagnostics diagnostics;
};

using StartupAssetPreimportProgressSink = std::function<void(const ImportProgressEvent&)>;

std::string FormatStartupAssetPreimportProgressLabel(const ImportProgressEvent& event);

StartupAssetPreimportResult RunBlockingStartupAssetPreimport(
    const StartupAssetPreimportOptions& options,
    StartupAssetPreimportProgressSink progressSink = {});
}
