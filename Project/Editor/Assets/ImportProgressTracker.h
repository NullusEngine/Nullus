#pragma once

#include "Assets/ArtifactWriter.h"
#include "Assets/AssetDiagnostics.h"
#include "Assets/AssetId.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Editor::Assets
{
struct ImportJobId
{
    uint64_t value = 0u;

    bool IsValid() const
    {
        return value != 0u;
    }

    friend bool operator==(const ImportJobId& lhs, const ImportJobId& rhs) = default;
};

enum class ImportPhase
{
    Queued,
    DependencyCopy,
    SourceParse,
    IntermediateConversion,
    ArtifactWrite,
    Postprocess,
    Commit,
    Finished
};

enum class ImportJobTerminalStatus
{
    None,
    Succeeded,
    Failed,
    Cancelled
};

class ImportCancellationToken final : public NLS::Core::Assets::IArtifactWriteCancellation
{
public:
    void Cancel();
    bool IsCancellationRequested() const override;

private:
    std::atomic_bool m_cancelled = false;
};

struct ImportProgressEvent
{
    ImportJobId jobId;
    NLS::Core::Assets::AssetId assetId;
    std::string sourcePath;
    std::string targetPlatform;
    ImportPhase phase = ImportPhase::Queued;
    double normalizedProgress = 0.0;
    std::string message;
    bool cancellationRequested = false;
    ImportJobTerminalStatus terminalStatus = ImportJobTerminalStatus::None;
    NLS::Core::Assets::AssetDiagnostics diagnostics;
};

struct ImportBatchProgress
{
    size_t totalAssets = 0u;
    size_t completedAssets = 0u;
    size_t failedAssets = 0u;
    size_t cancelledAssets = 0u;
    std::optional<ImportJobId> activeJob;
};

struct ImportCancellationTokenHandle
{
    ImportCancellationToken* token = nullptr;

    ImportCancellationToken& get() const
    {
        return *token;
    }

    explicit operator bool() const
    {
        return token != nullptr;
    }
};

class ImportProgressTracker
{
public:
    using Subscriber = std::function<void(const ImportProgressEvent&)>;

    ImportJobId BeginJob(
        NLS::Core::Assets::AssetId assetId,
        std::string sourcePath,
        std::string targetPlatform,
        size_t batchTotalAssets);
    void ReportProgress(
        ImportJobId jobId,
        ImportPhase phase,
        double normalizedProgress,
        std::string message);
    void FinishJob(
        ImportJobId jobId,
        ImportJobTerminalStatus status,
        NLS::Core::Assets::AssetDiagnostics diagnostics);

    std::optional<ImportCancellationTokenHandle> GetCancellationToken(ImportJobId jobId);
    void Subscribe(Subscriber subscriber);
    std::vector<ImportProgressEvent> GetEvents(ImportJobId jobId) const;
    std::optional<ImportProgressEvent> GetCurrentEvent(ImportJobId jobId) const;
    std::optional<ImportProgressEvent> GetActiveEvent() const;
    ImportBatchProgress GetBatchProgress() const;
    bool HasRunningJobs() const;

private:
    struct ImportJobState
    {
        ImportProgressEvent current;
        ImportCancellationToken cancellation;
        double localProgress = 0.0;
        bool finished = false;
    };

    void Publish(ImportJobState& state, ImportProgressEvent event);
    ImportJobState* FindJob(ImportJobId jobId);
    const ImportJobState* FindJob(ImportJobId jobId) const;
    bool HasRunningJobsLocked() const;
    size_t GetFinishedBatchAssetCountLocked() const;
    double CalculateBatchProgressLocked() const;

    uint64_t m_nextJobId = 1u;
    ImportBatchProgress m_batchProgress;
    std::unordered_map<uint64_t, ImportJobState> m_jobs;
    std::unordered_map<uint64_t, std::vector<ImportProgressEvent>> m_eventsByJob;
    std::vector<Subscriber> m_subscribers;
    mutable std::mutex m_mutex;
};
}

namespace std
{
template<>
struct hash<NLS::Editor::Assets::ImportJobId>
{
    size_t operator()(const NLS::Editor::Assets::ImportJobId& id) const noexcept
    {
        return std::hash<uint64_t>{}(id.value);
    }
};
}
