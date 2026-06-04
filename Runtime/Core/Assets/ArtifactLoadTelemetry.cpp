#include "Assets/ArtifactLoadTelemetry.h"

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
