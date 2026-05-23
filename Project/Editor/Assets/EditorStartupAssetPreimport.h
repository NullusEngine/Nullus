#pragma once

#include "Assets/ImportProgressTracker.h"

#include <filesystem>
#include <functional>
#include <string>

namespace NLS::Editor::Assets
{
struct StartupAssetPreimportOptions
{
    std::filesystem::path projectRoot;
};

struct StartupAssetPreimportResult
{
    bool succeeded = false;
    size_t plannedAssetCount = 0u;
    size_t importedAssetCount = 0u;
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
