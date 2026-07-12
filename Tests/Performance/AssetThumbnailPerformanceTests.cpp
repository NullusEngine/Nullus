#include <gtest/gtest.h>

#include "Assets/AssetThumbnailService.h"
#include "Guid.h"
#include "Profiling/PerformanceStageStats.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace NLS::Base::Profiling;

std::filesystem::path MakeThumbnailPerformanceRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_thumbnail_performance_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root / "Assets" / "Textures");
    return root;
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

NLS::Editor::Assets::AssetThumbnailRequest MakeTextureRequest(
    const std::filesystem::path& root,
    std::string freshness)
{
    NLS::Editor::Assets::AssetThumbnailRequest request;
    request.projectRoot = root;
    request.assetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse("20202020-2020-4020-8020-202020202020"));
    request.sourceAssetPath = "Assets/Textures/Hero.png";
    request.kind = NLS::Editor::Assets::AssetThumbnailKind::Texture;
    request.requestedSize = 64u;
    request.settingsFingerprint = "thumbnail-performance";
    request.freshnessInputs.push_back({"source", std::move(freshness)});
    return request;
}

const PerformanceStageEntry* FindThumbnailStage(
    const PerformanceStageStatsSnapshot& snapshot,
    const std::string& stageName)
{
    for (const auto& stage : snapshot.stages)
    {
        if (stage.domain == PerformanceStageDomain::Thumbnail && stage.stageName == stageName)
            return &stage;
    }
    return nullptr;
}

PerformanceBenchmarkRun MakeThumbnailBenchmarkRun(
    std::string scenarioName,
    const PerformanceStageStatsSnapshot& snapshot,
    std::chrono::microseconds totalDuration = std::chrono::microseconds{1})
{
    PerformanceBenchmarkRun run;
    run.scenarioName = std::move(scenarioName);
    run.runType = PerformanceBenchmarkRunType::Baseline;
    run.totalDuration = totalDuration;
    run.stageStats = snapshot;
    return run;
}

void WriteThumbnailPerformanceReportIfRequested(
    const std::string& scenarioName,
    const PerformanceStageStatsSnapshot& snapshot,
    std::chrono::microseconds totalDuration)
{
    const auto* reportDirectory = std::getenv("NLS_PERFORMANCE_REPORT_DIR");
    if (reportDirectory == nullptr || std::string(reportDirectory).empty())
        return;

    std::filesystem::create_directories(reportDirectory);
    PerformanceBenchmarkRun run;
    run.scenarioName = scenarioName;
    run.runType = PerformanceBenchmarkRunType::Baseline;
    run.totalDuration = totalDuration;
    run.stageStats = snapshot;

    std::ofstream output(
        std::filesystem::path(reportDirectory) / (scenarioName + ".txt"),
        std::ios::binary | std::ios::trunc);
    output << FormatPerformanceBenchmarkReport(run);
}

std::string MakeFreshnessForIndex(const size_t index)
{
    return "source:v" + std::to_string(index);
}

NLS::Editor::Assets::AssetThumbnailRequest MakeTextureRequestForIndex(
    const std::filesystem::path& root,
    const size_t index)
{
    auto request = MakeTextureRequest(root, MakeFreshnessForIndex(index));
    request.assetId = NLS::Core::Assets::AssetId(NLS::Guid::NewDeterministic(
        "thumbnail-performance-texture-" + std::to_string(index)));
    request.sourceAssetPath = "Assets/Textures/Hero" + std::to_string(index) + ".png";
    return request;
}
}

TEST(AssetThumbnailPerformanceTests, RapidScrollDuplicateRequestsReportBacklogAndCoalescingPressure)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    constexpr size_t uniqueRequestCount = 32u;
    constexpr size_t duplicateRequestCount = 500u;
    for (size_t index = 0; index < uniqueRequestCount; ++index)
        WriteBinaryFile(root / "Assets" / "Textures" / ("Hero" + std::to_string(index) + ".png"), TinyPng());

    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.cacheWriteCountBudget = 0u;
    service.SetThumbnailGenerationBudget(budget);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    for (size_t index = 0; index < duplicateRequestCount; ++index)
    {
        const auto request = MakeTextureRequestForIndex(root, index % uniqueRequestCount);
        EXPECT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    }
    const auto generated = service.GenerateNextThumbnail();
    EXPECT_FALSE(generated.has_value());

    const auto snapshot = stats.Snapshot();
    const auto* lookup = FindThumbnailStage(snapshot, "ThumbnailCacheLookup");
    ASSERT_NE(lookup, nullptr);
    ASSERT_TRUE(lookup->counters.contains("duplicateThumbnailRequestCount"));
    EXPECT_EQ(lookup->counters.at("duplicateThumbnailRequestCount"), duplicateRequestCount - uniqueRequestCount);
    ASSERT_TRUE(lookup->counters.contains("coalescingPressure"));
    EXPECT_EQ(lookup->counters.at("coalescingPressure"), duplicateRequestCount - uniqueRequestCount);
    ASSERT_TRUE(lookup->counters.contains("queueDepth"));
    EXPECT_EQ(lookup->counters.at("queueDepth"), uniqueRequestCount);

    const auto* total = FindThumbnailStage(snapshot, "TotalThumbnail");
    ASSERT_NE(total, nullptr);
    ASSERT_TRUE(total->counters.contains("queueBacklog"));
    EXPECT_EQ(total->counters.at("queueBacklog"), uniqueRequestCount);
    ASSERT_TRUE(total->counters.contains("inFlightRequestCount"));
    EXPECT_EQ(total->counters.at("inFlightRequestCount"), 0u);

    WriteThumbnailPerformanceReportIfRequested(
        "Thumbnail_RapidScrollDeduplication",
        snapshot,
        std::chrono::microseconds{1});

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, RapidScrollBenchmarkHonorsCacheWriteBudgetPerFrame)
{
    using namespace NLS::Editor::Assets;

    const auto root = MakeThumbnailPerformanceRoot();
    constexpr size_t uniqueRequestCount = 4u;
    constexpr size_t duplicateRequestCount = 40u;
    for (size_t index = 0; index < uniqueRequestCount; ++index)
        WriteBinaryFile(root / "Assets" / "Textures" / ("Hero" + std::to_string(index) + ".png"), TinyPng());

    AssetThumbnailService service;
    ThumbnailGenerationBudget budget;
    budget.cacheWriteCountBudget = 1u;
    service.SetThumbnailGenerationBudget(budget);

    PerformanceStageStats stats;
    PerformanceStageStatsCapture capture(stats);
    for (size_t index = 0; index < duplicateRequestCount; ++index)
    {
        const auto request = MakeTextureRequestForIndex(root, index % uniqueRequestCount);
        ASSERT_EQ(service.GetThumbnail(request).status, AssetThumbnailServiceStatus::Pending);
    }
    ASSERT_EQ(service.GetQueuedRequestCount(), uniqueRequestCount);

    const auto generated = service.GenerateNextThumbnail();
    ASSERT_TRUE(generated.has_value());
    EXPECT_EQ(generated->status, AssetThumbnailServiceStatus::Fresh);
    EXPECT_EQ(service.GetQueuedRequestCount(), uniqueRequestCount - 1u);

    const auto budgetExhausted = service.GenerateNextThumbnail();
    EXPECT_FALSE(budgetExhausted.has_value());
    EXPECT_EQ(service.GetQueuedRequestCount(), uniqueRequestCount - 1u);

    const auto snapshot = stats.Snapshot();
    const auto* lookup = FindThumbnailStage(snapshot, "ThumbnailCacheLookup");
    ASSERT_NE(lookup, nullptr);
    ASSERT_TRUE(lookup->counters.contains("duplicateThumbnailRequestCount"));
    EXPECT_EQ(lookup->counters.at("duplicateThumbnailRequestCount"), duplicateRequestCount - uniqueRequestCount);
    ASSERT_TRUE(lookup->counters.contains("coalescingPressure"));
    EXPECT_EQ(lookup->counters.at("coalescingPressure"), duplicateRequestCount - uniqueRequestCount);

    const auto* total = FindThumbnailStage(snapshot, "TotalThumbnail");
    ASSERT_NE(total, nullptr);
    ASSERT_TRUE(total->counters.contains("thumbnailsGeneratedThisFrame"));
    EXPECT_EQ(total->counters.at("thumbnailsGeneratedThisFrame"), 1u);
    ASSERT_TRUE(total->counters.contains("cacheWriteBudgetRemaining"));
    EXPECT_EQ(total->counters.at("cacheWriteBudgetRemaining"), 0u);
    ASSERT_TRUE(total->counters.contains("queueBacklog"));
    EXPECT_GE(total->counters.at("queueBacklog"), uniqueRequestCount - 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetThumbnailPerformanceTests, ThumbnailReportIncludesTopFiveAndSchedulerCounters)
{
    using namespace NLS::Editor::Assets;

    PerformanceStageStats stats;
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "TotalThumbnail",
        PerformanceStageThread::Main,
        std::chrono::microseconds{700},
        {
            {"thumbnailsGeneratedThisFrame", 1u},
            {"queueBacklog", 12u},
            {"inFlightRequestCount", 3u},
            {"cancellationLatency", 25u}
        }
    });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "ThumbnailCacheLookup",
        PerformanceStageThread::Main,
        std::chrono::microseconds{250},
        {
            {"duplicateThumbnailRequestCount", 5u},
            {"queueDepth", 8u},
            {"coalescingPressure", 4u}
        }
    });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "WaitPreviewFence",
        PerformanceStageThread::Main,
        std::chrono::microseconds{500},
        {{"fenceWaitTime", 500u}}
    });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "EncodePreview",
        PerformanceStageThread::Background,
        std::chrono::microseconds{150},
        {{"encodedByteCount", 64u}}
    });
    stats.Record({
        PerformanceStageDomain::Thumbnail,
        "StorePreviewCache",
        PerformanceStageThread::Background,
        std::chrono::microseconds{100},
        {{"cacheWriteCount", 1u}}
    });

    const auto snapshot = stats.Snapshot();
    const auto topBottlenecks = stats.TopBottlenecks(PerformanceStageDomain::Thumbnail, 5u);
    ASSERT_EQ(topBottlenecks.size(), 5u);
    EXPECT_EQ(topBottlenecks[0].stageName, "TotalThumbnail");
    EXPECT_EQ(topBottlenecks[1].stageName, "WaitPreviewFence");
    EXPECT_EQ(topBottlenecks[2].stageName, "ThumbnailCacheLookup");
    EXPECT_EQ(topBottlenecks[3].stageName, "EncodePreview");
    EXPECT_EQ(topBottlenecks[4].stageName, "StorePreviewCache");

    auto run = MakeThumbnailBenchmarkRun("Thumbnail_ReportCounters", snapshot);
    const auto report = FormatPerformanceBenchmarkReport(run, 5u);

    EXPECT_NE(report.find("TopBottlenecks:"), std::string::npos);
    EXPECT_NE(report.find("TotalThumbnail"), std::string::npos);
    EXPECT_NE(report.find("WaitPreviewFence"), std::string::npos);
    EXPECT_NE(report.find("ThumbnailCacheLookup"), std::string::npos);
    EXPECT_NE(report.find("thumbnailsGeneratedThisFrame=1"), std::string::npos);
    EXPECT_NE(report.find("duplicateThumbnailRequestCount=5"), std::string::npos);
    EXPECT_NE(report.find("queueDepth=8"), std::string::npos);
    EXPECT_NE(report.find("queueBacklog=12"), std::string::npos);
    EXPECT_NE(report.find("inFlightRequestCount=3"), std::string::npos);
    EXPECT_NE(report.find("cancellationLatency=25"), std::string::npos);
    EXPECT_NE(report.find("coalescingPressure=4"), std::string::npos);
    EXPECT_NE(report.find("fenceWaitTime=500"), std::string::npos);
}
