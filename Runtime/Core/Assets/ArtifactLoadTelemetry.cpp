#include "Assets/ArtifactLoadTelemetry.h"

#include <algorithm>
#include <mutex>

namespace NLS::Core::Assets
{
namespace
{
std::mutex& ArtifactLoadTelemetryMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::vector<ArtifactLoadTelemetryRecord>& ArtifactLoadTelemetryRecords()
{
    static std::vector<ArtifactLoadTelemetryRecord> records;
    return records;
}

std::vector<ArtifactLoadBudgetMissRecord>& ArtifactLoadBudgetMissRecords()
{
    static std::vector<ArtifactLoadBudgetMissRecord> records;
    return records;
}

template <typename T>
void PushBounded(std::vector<T>& records, const T& record, const size_t maxRecords)
{
    if (records.size() >= maxRecords)
        records.erase(records.begin());
    records.push_back(record);
}
}

void RecordArtifactLoadTelemetry(const ArtifactLoadTelemetryRecord& record)
{
    if constexpr (!Detail::kArtifactLoadTelemetryEnabled)
        return;

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    PushBounded(
        ArtifactLoadTelemetryRecords(),
        record,
        Detail::kMaxArtifactLoadTelemetryRecords);
}

std::vector<ArtifactLoadTelemetryRecord> SnapshotArtifactLoadTelemetry()
{
    if constexpr (!Detail::kArtifactLoadTelemetryEnabled)
        return {};

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    return ArtifactLoadTelemetryRecords();
}

std::vector<ArtifactLoadTelemetryStageSummary> SummarizeArtifactLoadTelemetry()
{
    if constexpr (!Detail::kArtifactLoadTelemetryEnabled)
        return {};

    std::vector<ArtifactLoadTelemetryRecord> records;
    {
        std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
        records = ArtifactLoadTelemetryRecords();
    }

    std::vector<ArtifactLoadTelemetryStageSummary> summaries;
    for (const auto& record : records)
    {
        auto found = std::find_if(
            summaries.begin(),
            summaries.end(),
            [&record](const ArtifactLoadTelemetryStageSummary& summary)
            {
                return summary.stage == record.stage && summary.path == record.path;
            });
        if (found == summaries.end())
        {
            summaries.push_back({
                record.stage,
                record.path,
                1u,
                record.elapsed,
                record.byteCount });
            continue;
        }

        ++found->recordCount;
        found->totalElapsed += record.elapsed;
        found->totalBytes += record.byteCount;
    }
    return summaries;
}

std::vector<ArtifactLoadBudgetMissRecord> SnapshotArtifactLoadBudgetMisses()
{
    if constexpr (!Detail::kArtifactLoadTelemetryEnabled)
        return {};

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    return ArtifactLoadBudgetMissRecords();
}

void ClearArtifactLoadTelemetry()
{
    if constexpr (!Detail::kArtifactLoadTelemetryEnabled)
        return;

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    ArtifactLoadTelemetryRecords().clear();
    ArtifactLoadBudgetMissRecords().clear();
}

void RecordArtifactLoadBudgetMiss(const ArtifactLoadBudgetMissRecord& record)
{
    if constexpr (!Detail::kArtifactLoadTelemetryEnabled)
        return;

    std::lock_guard<std::mutex> lock(ArtifactLoadTelemetryMutex());
    PushBounded(
        ArtifactLoadBudgetMissRecords(),
        record,
        Detail::kMaxArtifactLoadBudgetMissRecords);
}
}
