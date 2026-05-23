#include "Assets/ImportProgressTracker.h"

#include <algorithm>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
double ClampProgress(const double progress)
{
    return std::clamp(progress, 0.0, 1.0);
}

}

void ImportCancellationToken::Cancel()
{
    m_cancelled.store(true, std::memory_order_release);
}

bool ImportCancellationToken::IsCancellationRequested() const
{
    return m_cancelled.load(std::memory_order_acquire);
}

ImportJobId ImportProgressTracker::BeginJob(
    NLS::Core::Assets::AssetId assetId,
    std::string sourcePath,
    std::string targetPlatform,
    const size_t batchTotalAssets)
{
    std::vector<Subscriber> subscribers;
    ImportProgressEvent publishedEvent;
    ImportJobId jobId;

    {
        std::lock_guard lock(m_mutex);
        if (!HasRunningJobsLocked() && GetFinishedBatchAssetCountLocked() >= m_batchProgress.totalAssets)
            m_batchProgress = {};

        jobId = {m_nextJobId++};

        auto [inserted, _] = m_jobs.try_emplace(jobId.value);
        inserted->second.current.jobId = jobId;
        inserted->second.current.assetId = assetId;
        inserted->second.current.sourcePath = std::move(sourcePath);
        inserted->second.current.targetPlatform = std::move(targetPlatform);
        inserted->second.current.phase = ImportPhase::Queued;
        inserted->second.current.message = "Queued";
        m_batchProgress.totalAssets = std::max(m_batchProgress.totalAssets, batchTotalAssets);
        m_batchProgress.activeJob = jobId;
        inserted->second.current.normalizedProgress = CalculateBatchProgressLocked();
        Publish(inserted->second, inserted->second.current);
        publishedEvent = inserted->second.current;
        subscribers = m_subscribers;
    }
    for (const auto& subscriber : subscribers)
        subscriber(publishedEvent);
    return jobId;
}

void ImportProgressTracker::ReportProgress(
    const ImportJobId jobId,
    const ImportPhase phase,
    const double normalizedProgress,
    std::string message)
{
    std::vector<Subscriber> subscribers;
    ImportProgressEvent publishedEvent;
    {
        std::lock_guard lock(m_mutex);
        auto* state = FindJob(jobId);
        if (!state || state->finished)
            return;

        auto event = state->current;
        event.phase = phase;
        state->localProgress = ClampProgress(normalizedProgress);
        event.normalizedProgress = CalculateBatchProgressLocked();
        event.message = std::move(message);
        event.cancellationRequested = state->cancellation.IsCancellationRequested();
        Publish(*state, std::move(event));
        publishedEvent = state->current;
        subscribers = m_subscribers;
    }
    for (const auto& subscriber : subscribers)
        subscriber(publishedEvent);
}

void ImportProgressTracker::FinishJob(
    const ImportJobId jobId,
    const ImportJobTerminalStatus status,
    NLS::Core::Assets::AssetDiagnostics diagnostics)
{
    std::vector<Subscriber> subscribers;
    ImportProgressEvent publishedEvent;
    {
        std::lock_guard lock(m_mutex);
        auto* state = FindJob(jobId);
        if (!state || state->finished)
            return;

        auto event = state->current;
        event.phase = ImportPhase::Finished;
        event.cancellationRequested = state->cancellation.IsCancellationRequested();
        event.terminalStatus = status;
        event.diagnostics = std::move(diagnostics);
        state->localProgress = 1.0;
        switch (status)
        {
        case ImportJobTerminalStatus::Succeeded:
            ++m_batchProgress.completedAssets;
            event.message = "Import succeeded";
            break;
        case ImportJobTerminalStatus::Failed:
            ++m_batchProgress.failedAssets;
            event.message = "Import failed";
            break;
        case ImportJobTerminalStatus::Cancelled:
            ++m_batchProgress.cancelledAssets;
            event.message = "Import cancelled";
            break;
        case ImportJobTerminalStatus::None:
        default:
            break;
        }
        state->finished = true;
        event.normalizedProgress = CalculateBatchProgressLocked();
        Publish(*state, std::move(event));

        m_batchProgress.activeJob.reset();
        for (const auto& [_, job] : m_jobs)
        {
            if (!job.finished)
            {
                m_batchProgress.activeJob = job.current.jobId;
                break;
            }
        }
        publishedEvent = state->current;
        subscribers = m_subscribers;
    }
    for (const auto& subscriber : subscribers)
        subscriber(publishedEvent);
}

std::optional<ImportCancellationTokenHandle> ImportProgressTracker::GetCancellationToken(
    const ImportJobId jobId)
{
    std::lock_guard lock(m_mutex);
    auto* state = FindJob(jobId);
    if (!state)
        return std::nullopt;
    return ImportCancellationTokenHandle {&state->cancellation};
}

void ImportProgressTracker::Subscribe(Subscriber subscriber)
{
    std::lock_guard lock(m_mutex);
    m_subscribers.push_back(std::move(subscriber));
}

std::vector<ImportProgressEvent> ImportProgressTracker::GetEvents(const ImportJobId jobId) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_eventsByJob.find(jobId.value);
    if (found == m_eventsByJob.end())
        return {};
    return found->second;
}

std::optional<ImportProgressEvent> ImportProgressTracker::GetCurrentEvent(const ImportJobId jobId) const
{
    std::lock_guard lock(m_mutex);
    const auto* state = FindJob(jobId);
    if (!state)
        return std::nullopt;
    return state->current;
}

std::optional<ImportProgressEvent> ImportProgressTracker::GetActiveEvent() const
{
    std::lock_guard lock(m_mutex);
    if (!m_batchProgress.activeJob.has_value())
        return std::nullopt;
    const auto* state = FindJob(*m_batchProgress.activeJob);
    if (!state)
        return std::nullopt;
    return state->current;
}

ImportBatchProgress ImportProgressTracker::GetBatchProgress() const
{
    std::lock_guard lock(m_mutex);
    return m_batchProgress;
}

bool ImportProgressTracker::HasRunningJobs() const
{
    std::lock_guard lock(m_mutex);
    return HasRunningJobsLocked();
}

bool ImportProgressTracker::HasRunningJobsLocked() const
{
    return std::any_of(
        m_jobs.begin(),
        m_jobs.end(),
        [](const auto& entry)
        {
            return !entry.second.finished;
        });
}

size_t ImportProgressTracker::GetFinishedBatchAssetCountLocked() const
{
    return m_batchProgress.completedAssets +
        m_batchProgress.failedAssets +
        m_batchProgress.cancelledAssets;
}

double ImportProgressTracker::CalculateBatchProgressLocked() const
{
    if (m_batchProgress.totalAssets == 0u)
        return 0.0;

    double localProgressSum = 0.0;
    for (const auto& [_, job] : m_jobs)
    {
        if (!job.finished)
            localProgressSum += ClampProgress(job.localProgress);
    }

    const auto finishedAssets =
        std::min(GetFinishedBatchAssetCountLocked(), m_batchProgress.totalAssets);
    const auto aggregateProgress =
        (static_cast<double>(finishedAssets) + localProgressSum) /
        static_cast<double>(m_batchProgress.totalAssets);
    return ClampProgress(aggregateProgress);
}

void ImportProgressTracker::Publish(ImportJobState& state, ImportProgressEvent event)
{
    event.cancellationRequested = state.cancellation.IsCancellationRequested();
    state.current = event;
    m_eventsByJob[event.jobId.value].push_back(event);
}

ImportProgressTracker::ImportJobState* ImportProgressTracker::FindJob(const ImportJobId jobId)
{
    const auto found = m_jobs.find(jobId.value);
    if (found == m_jobs.end())
        return nullptr;
    return &found->second;
}

const ImportProgressTracker::ImportJobState* ImportProgressTracker::FindJob(const ImportJobId jobId) const
{
    const auto found = m_jobs.find(jobId.value);
    if (found == m_jobs.end())
        return nullptr;
    return &found->second;
}
}
