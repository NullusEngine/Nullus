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
    bool enablePreparedPrefabCachePreflight = false;
    size_t maxPreparedPrefabCachePreflightCount = 64u;
    std::chrono::milliseconds maxPreparedPrefabCachePreflightDuration {100};
    std::vector<std::string> priorityPreparedPrefabAssetPaths;
};

struct StartupAssetPreimportCacheValidationProfile
{
    size_t sourceEntryCount = 0u;
    size_t sourceDirectoryEntryCount = 0u;
    size_t dependencyEntryCount = 0u;
    size_t artifactEntryCount = 0u;
    size_t trackedFileEntryCount = 0u;
    size_t fileMetadataQueryCount = 0u;
    size_t contentHashReadCount = 0u;
    size_t importerFingerprintComputeCount = 0u;
    uint64_t elapsedMilliseconds = 0u;
    std::string missReason;
};

struct StartupAssetPreimportResult
{
    bool succeeded = false;
    bool usedCache = false;
    size_t plannedAssetCount = 0u;
    size_t importedAssetCount = 0u;
    size_t preparedPrefabCachePreflightAttemptCount = 0u;
    size_t preparedPrefabCachePreflightCount = 0u;
    size_t prewarmedMaterialArtifactCount = 0u;
    StartupAssetPreimportCacheValidationProfile cacheValidationProfile;
    bool hadRunningJobsAfterCompletion = false;
    NLS::Core::Assets::AssetDiagnostics diagnostics;
};

using StartupAssetPreimportProgressSink = std::function<void(const ImportProgressEvent&)>;

struct StartupWatcherPreimportResult
{
    bool succeeded = false;
    bool requiresRuntimeAssetRefresh = false;
};

std::string FormatStartupAssetPreimportProgressLabel(const ImportProgressEvent& event);

StartupAssetPreimportResult RunBlockingStartupAssetPreimport(
    const StartupAssetPreimportOptions& options,
    StartupAssetPreimportProgressSink progressSink = {});

#if defined(NLS_ENABLE_TEST_HOOKS)
void ResetStartupAssetPreimportIndexLoadCountForTesting();
size_t GetStartupAssetPreimportIndexLoadCountForTesting();
void ResetStartupAssetPreimportSourceEnumerationCountForTesting();
size_t GetStartupAssetPreimportSourceEnumerationCountForTesting();
void ResetStartupAssetPreimportContentHashReadCountForTesting();
size_t GetStartupAssetPreimportContentHashReadCountForTesting();
void ResetStartupAssetPreimportFileMetadataQueryCountForTesting();
size_t GetStartupAssetPreimportFileMetadataQueryCountForTesting();
void ResetStartupAssetPreimportShardWriteCountForTesting();
size_t GetStartupAssetPreimportShardWriteCountForTesting();
void ResetStartupAssetPreimportFullIndexRebuildCountForTesting();
size_t GetStartupAssetPreimportFullIndexRebuildCountForTesting();
void ResetStartupAssetPreimportPatchedIndexWriteCountForTesting();
size_t GetStartupAssetPreimportPatchedIndexWriteCountForTesting();
void ResetStartupAssetPreimportImporterFingerprintComputeCountForTesting();
size_t GetStartupAssetPreimportImporterFingerprintComputeCountForTesting();
bool RewriteStartupAssetPreimportIndexForTesting(const std::filesystem::path& projectRoot);
#endif
}
