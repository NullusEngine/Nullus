#include <gtest/gtest.h>

#include "Assets/ArtifactLoadTelemetry.h"

#include <chrono>
#include <string>
#include <vector>

using namespace NLS::Core::Assets;

namespace
{
class ArtifactLoadTelemetryTestScope
{
public:
    ArtifactLoadTelemetryTestScope()
        : m_previouslyEnabled(IsArtifactLoadTelemetryEnabled())
    {
        SetArtifactLoadTelemetryEnabled(true);
        ClearArtifactLoadTelemetry();
    }

    ~ArtifactLoadTelemetryTestScope()
    {
        ClearArtifactLoadTelemetry();
        SetArtifactLoadTelemetryEnabled(m_previouslyEnabled);
    }

private:
    bool m_previouslyEnabled = false;
};

ArtifactLoadTelemetryRecord MakeTelemetryRecord(const size_t index)
{
    ArtifactLoadTelemetryRecord record;
    record.stage = ArtifactLoadTelemetryStage::CacheHit;
    record.elapsed = std::chrono::microseconds(static_cast<int64_t>(index));
    record.byteCount = index;
    record.path = std::to_string(index);
    return record;
}

ArtifactLoadBudgetMissRecord MakeBudgetMissRecord(const size_t index)
{
    ArtifactLoadBudgetMissRecord record;
    record.kind = ArtifactLoadBudgetKind::HotCacheLookup;
    record.elapsed = std::chrono::milliseconds(static_cast<int64_t>(index));
    record.budgetMs = ArtifactLoadBudget::kHotCacheLookupBudgetMs;
    record.path = std::to_string(index);
    return record;
}
}

TEST(ArtifactLoadTelemetryTests, BoundedHistoryKeepsNewestRecordsInSnapshotOrder)
{
    ArtifactLoadTelemetryTestScope telemetryScope;
    constexpr size_t extraRecordCount = 7u;
    const size_t totalRecordCount = Detail::kMaxArtifactLoadTelemetryRecords + extraRecordCount;

    for (size_t index = 0u; index < totalRecordCount; ++index)
        RecordArtifactLoadTelemetry(MakeTelemetryRecord(index));

    const std::vector<ArtifactLoadTelemetryRecord> snapshot = SnapshotArtifactLoadTelemetry();
    ASSERT_EQ(snapshot.size(), Detail::kMaxArtifactLoadTelemetryRecords);
    EXPECT_EQ(snapshot.front().path, std::to_string(extraRecordCount));
    EXPECT_EQ(snapshot.back().path, std::to_string(totalRecordCount - 1u));

    for (size_t index = 0u; index < snapshot.size(); ++index)
    {
        EXPECT_EQ(snapshot[index].path, std::to_string(index + extraRecordCount));
        EXPECT_EQ(snapshot[index].byteCount, index + extraRecordCount);
    }
}

TEST(ArtifactLoadTelemetryTests, BoundedBudgetMissHistoryKeepsNewestRecordsInSnapshotOrder)
{
    ArtifactLoadTelemetryTestScope telemetryScope;
    constexpr size_t extraRecordCount = 5u;
    const size_t totalRecordCount = Detail::kMaxArtifactLoadBudgetMissRecords + extraRecordCount;

    for (size_t index = 0u; index < totalRecordCount; ++index)
        RecordArtifactLoadBudgetMiss(MakeBudgetMissRecord(index));

    const std::vector<ArtifactLoadBudgetMissRecord> snapshot = SnapshotArtifactLoadBudgetMisses();
    ASSERT_EQ(snapshot.size(), Detail::kMaxArtifactLoadBudgetMissRecords);
    EXPECT_EQ(snapshot.front().path, std::to_string(extraRecordCount));
    EXPECT_EQ(snapshot.back().path, std::to_string(totalRecordCount - 1u));

    for (size_t index = 0u; index < snapshot.size(); ++index)
    {
        EXPECT_EQ(snapshot[index].kind, ArtifactLoadBudgetKind::HotCacheLookup);
        EXPECT_EQ(snapshot[index].path, std::to_string(index + extraRecordCount));
    }
}

TEST(ArtifactLoadTelemetryTests, ClearResetsBothBoundedHistoriesAndAllowsReuse)
{
    ArtifactLoadTelemetryTestScope telemetryScope;
    RecordArtifactLoadTelemetry(MakeTelemetryRecord(11u));
    RecordArtifactLoadBudgetMiss(MakeBudgetMissRecord(12u));

    ClearArtifactLoadTelemetry();
    EXPECT_TRUE(SnapshotArtifactLoadTelemetry().empty());
    EXPECT_TRUE(SnapshotArtifactLoadBudgetMisses().empty());

    RecordArtifactLoadTelemetry(MakeTelemetryRecord(13u));
    RecordArtifactLoadBudgetMiss(MakeBudgetMissRecord(14u));
    const auto telemetrySnapshot = SnapshotArtifactLoadTelemetry();
    const auto budgetSnapshot = SnapshotArtifactLoadBudgetMisses();
    ASSERT_EQ(telemetrySnapshot.size(), 1u);
    ASSERT_EQ(budgetSnapshot.size(), 1u);
    EXPECT_EQ(telemetrySnapshot.front().path, "13");
    EXPECT_EQ(budgetSnapshot.front().path, "14");
}
