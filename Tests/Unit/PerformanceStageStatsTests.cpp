#include <gtest/gtest.h>

#include "Profiling/PerformanceStageStats.h"

#include <chrono>
#include <string>

namespace
{
using namespace NLS::Base::Profiling;
using namespace std::chrono_literals;
}

TEST(PerformanceStageStatsTests, RecordsDurationsCountersAndThreadSplits)
{
    PerformanceStageStats stats;

    PerformanceStageSample prefabLoad;
    prefabLoad.domain = PerformanceStageDomain::Prefab;
    prefabLoad.stageName = "LoadPrefabArtifact";
    prefabLoad.thread = PerformanceStageThread::Main;
    prefabLoad.duration = 120us;
    prefabLoad.counters["objectCount"] = 4u;
    prefabLoad.counters["cacheMisses"] = 1u;
    stats.Record(prefabLoad);

    PerformanceStageSample prefabLoadBackground = prefabLoad;
    prefabLoadBackground.thread = PerformanceStageThread::Background;
    prefabLoadBackground.duration = 80us;
    prefabLoadBackground.counters["objectCount"] = 6u;
    stats.Record(prefabLoadBackground);

    const auto snapshot = stats.Snapshot();
    ASSERT_EQ(snapshot.stages.size(), 1u);
    const auto& stage = snapshot.stages.front();
    EXPECT_EQ(stage.domain, PerformanceStageDomain::Prefab);
    EXPECT_EQ(stage.stageName, "LoadPrefabArtifact");
    EXPECT_EQ(stage.callCount, 2u);
    EXPECT_EQ(stage.totalDuration, 200us);
    EXPECT_EQ(stage.mainThreadDuration, 120us);
    EXPECT_EQ(stage.backgroundThreadDuration, 80us);
    ASSERT_EQ(stage.counters.at("objectCount"), 10u);
    ASSERT_EQ(stage.counters.at("cacheMisses"), 2u);
}

TEST(PerformanceStageStatsTests, AggregatesCountersByMetricSemantics)
{
    PerformanceStageStats stats;

    PerformanceStageSample first;
    first.domain = PerformanceStageDomain::Thumbnail;
    first.stageName = "TotalThumbnail";
    first.thread = PerformanceStageThread::Main;
    first.duration = 10us;
    first.counters["queueDepth"] = 5u;
    first.counters["queueBacklog"] = 4u;
    first.counters["inFlightRequestCount"] = 1u;
    first.counters["cancellationLatency"] = 9u;
    first.counters["cacheWriteBudgetRemaining"] = 3u;
    first.counters["previewRenderBudgetRemaining"] = 2u;
    first.counters["readbackBudgetRemaining"] = 1u;
    first.counters["duplicateThumbnailRequestCount"] = 7u;
    stats.Record(first);

    PerformanceStageSample second = first;
    second.duration = 20us;
    second.counters["queueDepth"] = 3u;
    second.counters["queueBacklog"] = 6u;
    second.counters["inFlightRequestCount"] = 2u;
    second.counters["cancellationLatency"] = 5u;
    second.counters["cacheWriteBudgetRemaining"] = 1u;
    second.counters["previewRenderBudgetRemaining"] = 4u;
    second.counters["readbackBudgetRemaining"] = 0u;
    second.counters["duplicateThumbnailRequestCount"] = 11u;
    stats.Record(second);

    const auto snapshot = stats.Snapshot();
    ASSERT_EQ(snapshot.stages.size(), 1u);
    const auto& counters = snapshot.stages.front().counters;
    EXPECT_EQ(counters.at("queueDepth"), 5u);
    EXPECT_EQ(counters.at("queueBacklog"), 6u);
    EXPECT_EQ(counters.at("inFlightRequestCount"), 2u);
    EXPECT_EQ(counters.at("cancellationLatency"), 9u);
    EXPECT_EQ(counters.at("cacheWriteBudgetRemaining"), 1u);
    EXPECT_EQ(counters.at("previewRenderBudgetRemaining"), 2u);
    EXPECT_EQ(counters.at("readbackBudgetRemaining"), 0u);
    EXPECT_EQ(counters.at("duplicateThumbnailRequestCount"), 18u);
}

TEST(PerformanceStageStatsTests, RanksTopBottlenecksDeterministically)
{
    PerformanceStageStats stats;

    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "EncodePreview",
        PerformanceStageThread::Main,
        200us,
        {} });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "WaitPreviewFence",
        PerformanceStageThread::Main,
        600us,
        {} });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "ReadbackPreview",
        PerformanceStageThread::Main,
        600us,
        {} });

    const auto top = stats.TopBottlenecks(PerformanceStageDomain::Thumbnail, 2u);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_EQ(top[0].stageName, "ReadbackPreview")
        << "Equal-duration stages are ordered by stage name for stable reports.";
    EXPECT_EQ(top[1].stageName, "WaitPreviewFence");
}

TEST(PerformanceStageStatsTests, EmptySnapshotContainsNoStages)
{
    PerformanceStageStats stats;

    const auto snapshot = stats.Snapshot();

    EXPECT_TRUE(snapshot.stages.empty());
    EXPECT_TRUE(stats.TopBottlenecks(PerformanceStageDomain::Prefab, 5u).empty());
}

TEST(PerformanceStageStatsTests, CompareRunsRejectsMissingBaselineAndScenarioMismatch)
{
    PerformanceBenchmarkRun optimized;
    optimized.scenarioName = "Prefab/HotCache";
    optimized.runType = PerformanceBenchmarkRunType::Optimized;

    const auto missingBaseline = ComparePerformanceRuns(nullptr, &optimized);
    EXPECT_FALSE(missingBaseline.valid);
    EXPECT_EQ(missingBaseline.diagnostic, "performance-comparison-baseline-missing");

    PerformanceBenchmarkRun baseline;
    baseline.scenarioName = "Prefab/ColdCache";
    baseline.runType = PerformanceBenchmarkRunType::Baseline;

    const auto mismatch = ComparePerformanceRuns(&baseline, &optimized);
    EXPECT_FALSE(mismatch.valid);
    EXPECT_EQ(mismatch.diagnostic, "performance-comparison-scenario-mismatch");
}

TEST(PerformanceStageStatsTests, CompareRunsReportsPercentageChangeForMatchingScenario)
{
    PerformanceBenchmarkRun baseline;
    baseline.scenarioName = "Thumbnail/FirstGeneration";
    baseline.runType = PerformanceBenchmarkRunType::Baseline;
    baseline.totalDuration = 400us;

    PerformanceBenchmarkRun optimized;
    optimized.scenarioName = baseline.scenarioName;
    optimized.runType = PerformanceBenchmarkRunType::Optimized;
    optimized.totalDuration = 100us;

    const auto comparison = ComparePerformanceRuns(&baseline, &optimized);

    ASSERT_TRUE(comparison.valid);
    EXPECT_EQ(comparison.scenarioName, baseline.scenarioName);
    EXPECT_DOUBLE_EQ(comparison.percentChange, -75.0);
    EXPECT_TRUE(comparison.diagnostic.empty());
}

TEST(PerformanceStageStatsTests, ScopedCaptureRecordsIntoActiveCollector)
{
    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        PerformanceStageScope scope(
            PerformanceStageDomain::Prefab,
            "DeserializeComponents",
            PerformanceStageThread::Main);
        scope.AddCounter("componentCount", 7u);
    }

    const auto snapshot = stats.Snapshot();
    ASSERT_EQ(snapshot.stages.size(), 1u);
    const auto& stage = snapshot.stages.front();
    EXPECT_EQ(stage.domain, PerformanceStageDomain::Prefab);
    EXPECT_EQ(stage.stageName, "DeserializeComponents");
    EXPECT_EQ(stage.callCount, 1u);
    EXPECT_GT(stage.totalDuration.count(), 0);
    EXPECT_EQ(stage.counters.at("componentCount"), 7u);
}

TEST(PerformanceStageStatsTests, RequiredStageWarningsListMissingStagesDeterministically)
{
    PerformanceStageStats stats;
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "ThumbnailCacheLookup",
        PerformanceStageThread::Main,
        1us,
        {} });

    const auto warnings = FindMissingPerformanceStages(
        stats.Snapshot(),
        PerformanceStageDomain::Thumbnail,
        {"RecordPreviewRender", "ThumbnailCacheLookup", "EncodePreview"});

    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_EQ(warnings[0], "EncodePreview");
    EXPECT_EQ(warnings[1], "RecordPreviewRender");
}

TEST(PerformanceStageStatsTests, FormatsBenchmarkReportWithStableBottleneckRanking)
{
    PerformanceBenchmarkRun run;
    run.scenarioName = "Prefab/HotCache";
    run.runType = PerformanceBenchmarkRunType::Baseline;
    run.totalDuration = 1000us;
    run.stageStats.stages = {
        {
            PerformanceStageDomain::Prefab,
            "ResolveDependencies",
            2u,
            300us,
            250us,
            50us,
            0us,
            {{"cacheHits", 1u}},
        },
        {
            PerformanceStageDomain::Prefab,
            "DeserializeComponents",
            1u,
            600us,
            600us,
            0us,
            0us,
            {},
        },
    };

    const auto report = FormatPerformanceBenchmarkReport(run, 1u);

    EXPECT_NE(report.find("Scenario: Prefab/HotCache"), std::string::npos);
    EXPECT_NE(report.find("Run: Baseline"), std::string::npos);
    EXPECT_NE(report.find("Total: 1000us"), std::string::npos);
    EXPECT_NE(report.find("[Prefab] ResolveDependencies callCount=2"), std::string::npos);
    EXPECT_NE(report.find("counters=cacheHits=1"), std::string::npos);
    EXPECT_NE(report.find("- DeserializeComponents total=600us calls=1"), std::string::npos);
    EXPECT_EQ(report.find("- ResolveDependencies total=300us calls=2"), std::string::npos);
}
