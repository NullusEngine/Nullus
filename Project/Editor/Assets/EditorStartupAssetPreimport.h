#pragma once

#include "Assets/ImportProgressTracker.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
class ResidentPrefabPreviewRegistry;

struct StartupAssetPreimportOptions
{
    std::filesystem::path projectRoot;
    bool enablePreparedPrefabCachePreflight = false;
    size_t maxPreparedPrefabCachePreflightCount = 64u;
    std::chrono::milliseconds maxPreparedPrefabCachePreflightDuration {100};
    std::vector<std::string> priorityPreparedPrefabAssetPaths;
    std::shared_ptr<ResidentPrefabPreviewRegistry> residentPrefabPreviewRegistry;
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

// Owns a single cache-analysis future that can run while the editor initializes its graphics stack.
class StartupAssetPreimportCacheAnalysisTask final
{
public:
    StartupAssetPreimportCacheAnalysisTask() = default;
    StartupAssetPreimportCacheAnalysisTask(const StartupAssetPreimportCacheAnalysisTask&) = delete;
    StartupAssetPreimportCacheAnalysisTask& operator=(const StartupAssetPreimportCacheAnalysisTask&) = delete;
    StartupAssetPreimportCacheAnalysisTask(StartupAssetPreimportCacheAnalysisTask&&) noexcept = default;
    StartupAssetPreimportCacheAnalysisTask& operator=(StartupAssetPreimportCacheAnalysisTask&&) noexcept = default;

private:
    struct State;

    friend StartupAssetPreimportCacheAnalysisTask StartStartupAssetPreimportCacheAnalysis(
        const std::filesystem::path& projectRoot);
    friend StartupAssetPreimportResult RunBlockingStartupAssetPreimport(
        const StartupAssetPreimportOptions& options,
        StartupAssetPreimportProgressSink progressSink,
        StartupAssetPreimportCacheAnalysisTask* cacheAnalysisTask);

    std::shared_ptr<State> m_state;
};

struct StartupWatcherPreimportResult
{
    bool succeeded = false;
    bool requiresRuntimeAssetRefresh = false;
};

std::string FormatStartupAssetPreimportProgressLabel(const ImportProgressEvent& event);

StartupAssetPreimportCacheAnalysisTask StartStartupAssetPreimportCacheAnalysis(
    const std::filesystem::path& projectRoot);

StartupAssetPreimportResult RunBlockingStartupAssetPreimport(
    const StartupAssetPreimportOptions& options,
    StartupAssetPreimportProgressSink progressSink = {},
    StartupAssetPreimportCacheAnalysisTask* cacheAnalysisTask = nullptr);

#if defined(NLS_ENABLE_TEST_HOOKS)
void ResetStartupAssetPreimportIndexLoadCountForTesting();
size_t GetStartupAssetPreimportIndexLoadCountForTesting();
void ResetStartupAssetPreimportSourceEnumerationCountForTesting();
size_t GetStartupAssetPreimportSourceEnumerationCountForTesting();
void ResetStartupAssetPreimportContentHashReadCountForTesting();
size_t GetStartupAssetPreimportContentHashReadCountForTesting();
void ResetStartupAssetPreimportFileMetadataQueryCountForTesting();
size_t GetStartupAssetPreimportFileMetadataQueryCountForTesting();
void ResetStartupAssetPreimportFastFileMetadataQueryCountForTesting();
size_t GetStartupAssetPreimportFastFileMetadataQueryCountForTesting();
void ResetStartupAssetPreimportShardWriteCountForTesting();
size_t GetStartupAssetPreimportShardWriteCountForTesting();
void ResetStartupAssetPreimportFullIndexRebuildCountForTesting();
size_t GetStartupAssetPreimportFullIndexRebuildCountForTesting();
void ResetStartupAssetPreimportPatchedIndexWriteCountForTesting();
size_t GetStartupAssetPreimportPatchedIndexWriteCountForTesting();
void ResetStartupAssetPreimportImporterFingerprintComputeCountForTesting();
size_t GetStartupAssetPreimportImporterFingerprintComputeCountForTesting();
bool RewriteStartupAssetPreimportIndexForTesting(const std::filesystem::path& projectRoot);
bool RewriteStartupAssetPreimportIndexAsLegacyTextForTesting(const std::filesystem::path& projectRoot);
bool IsStartupAssetPreimportIndexBinaryForTesting(const std::filesystem::path& stampPath);
#endif
}
