#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <Json/json.hpp>

#include "Core/EditorCameraPerformanceBenchmark.h"

namespace
{
    using NLS::Editor::Core::BuildEditorCameraPerformanceSummary;
    using NLS::Editor::Core::CalculateEditorCameraPerformanceTelemetryDelta;
    using NLS::Editor::Core::EditorCameraPerformanceMetadata;
    using NLS::Editor::Core::EditorCameraPerformanceTelemetry;

    std::string ReadRepositoryTextFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), {});
    }

    TEST(EditorCameraPerformanceBenchmarkTests, UsesMeasuredSamplesAndNearestRankPercentiles)
    {
        const std::vector<double> samples { 10.0, 20.0, 30.0, 40.0, 50.0 };
        const auto summary = BuildEditorCameraPerformanceSummary(
            EditorCameraPerformanceMetadata { "Debug", "DX12", false, 30u, 5u },
            samples,
            EditorCameraPerformanceTelemetry {},
            EditorCameraPerformanceTelemetry {},
            0u,
            { 5.0, 10.0, 15.0, 20.0, 25.0 });

        EXPECT_DOUBLE_EQ(summary.meanFrameMs, 30.0);
        EXPECT_DOUBLE_EQ(summary.meanFps, 1000.0 / 30.0);
        EXPECT_DOUBLE_EQ(summary.p95FrameMs, 50.0);
        EXPECT_DOUBLE_EQ(summary.p99FrameMs, 50.0);
        EXPECT_DOUBLE_EQ(summary.maxFrameMs, 50.0);
        EXPECT_DOUBLE_EQ(summary.settleMeanFrameMs, 15.0);
        EXPECT_DOUBLE_EQ(summary.settleMeanFps, 1000.0 / 15.0);
        EXPECT_DOUBLE_EQ(summary.settleP95FrameMs, 25.0);
        EXPECT_DOUBLE_EQ(summary.settleP99FrameMs, 25.0);
        EXPECT_DOUBLE_EQ(summary.settleMaxFrameMs, 25.0);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, EmptySamplesProduceZeroSummary)
    {
        const auto summary = BuildEditorCameraPerformanceSummary(
            EditorCameraPerformanceMetadata {},
            {},
            EditorCameraPerformanceTelemetry {},
            EditorCameraPerformanceTelemetry {});

        EXPECT_DOUBLE_EQ(summary.meanFrameMs, 0.0);
        EXPECT_DOUBLE_EQ(summary.meanFps, 0.0);
        EXPECT_DOUBLE_EQ(summary.p95FrameMs, 0.0);
        EXPECT_DOUBLE_EQ(summary.p99FrameMs, 0.0);
        EXPECT_DOUBLE_EQ(summary.maxFrameMs, 0.0);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, SubtractsCumulativeTelemetryWithoutUnderflow)
    {
        EditorCameraPerformanceTelemetry before;
        before.publishedFrameCount = 40u;
        before.reservedSlotWaitTotalNs = 100u;
        before.reservedSlotWaitMaxNs = 10000000u;
        before.latestPublishedFrameId = 120u;
        before.latestRetiredFrameId = 118u;
        EditorCameraPerformanceTelemetry after = before;
        after.publishedFrameCount = 44u;
        after.reservedSlotWaitTotalNs = 350u;
        after.reservedSlotWaitMaxNs = 8000000u;
        after.latestPublishedFrameId = 124u;
        after.latestRetiredFrameId = 123u;

        const auto delta = CalculateEditorCameraPerformanceTelemetryDelta(before, after);
        EXPECT_EQ(delta.publishedFrameCount, 4u);
        EXPECT_EQ(delta.reservedSlotWaitTotalNs, 250u);
        EXPECT_EQ(delta.reservedSlotWaitMaxNs, 8000000u);
        EXPECT_EQ(delta.latestPublishedFrameId, 124u);
        EXPECT_EQ(delta.latestRetiredFrameId, 123u);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, SerializesCompleteTelemetrySchemaWithExactIntegerCounters)
    {
        constexpr uint64_t integerBase = 9007199254740992ull;
        EditorCameraPerformanceTelemetry before;
        before.blockedPublishCount = integerBase;
        before.publishedFrameCount = integerBase;
        before.reservedSlotWaitCount = integerBase;
        before.reservedSlotWaitTimeoutCount = integerBase;
        before.reservedSlotWaitTotalNs = integerBase;
        before.reservedSlotWaitMaxNs = integerBase;
        before.latestPublishedFrameId = integerBase;
        before.latestRetiredFrameId = integerBase;
        before.preparedStaticBaseCacheHitCount = integerBase;
        before.preparedStaticBaseCacheMissCount = integerBase;
        before.staticDrawFastPathHitCount = integerBase;
        before.staticDrawFastPathMissCount = integerBase;
        before.objectDataRevisionHitCount = integerBase;
        before.objectDataRevisionFallbackCount = integerBase;
        before.objectDataRevisionDescriptorFallbackCount = integerBase;
        before.objectDataRevisionMetadataUnavailableCount = integerBase;
        before.objectDataRevisionMetadataUninitializedCount = integerBase;
        before.objectDataRevisionMetadataMismatchCount = integerBase;
        before.objectDataRevisionMetadataInvalidCount = integerBase;
        before.objectDataRevisionStableIdentityMismatchCount = integerBase;
        before.objectDataRevisionTransformMismatchCount = integerBase;
        before.objectDataRevisionObjectIndexMismatchCount = integerBase;
        before.objectDataRevisionObjectCountMismatchCount = integerBase;
        before.opaqueSortTokenHitCount = integerBase;
        before.opaqueSortTokenRebuildCount = integerBase;
        before.descriptorAllocationFailureCount = integerBase;
        before.deviceLostCount = integerBase;
        before.unsafeGpuQuarantineCount = integerBase;
        before.objectDataOverflowCount = integerBase;
        before.largeScene.sceneRenderContentRevisionFastPathCount = integerBase;
        before.largeScene.syncTimeNs = integerBase;
        before.largeScene.serialVisibilityTimeNs = integerBase;
        before.largeScene.parallelVisibilityTimeNs = integerBase;
        before.largeScene.queueFinalizationTimeNs = integerBase;
        before.largeScene.visibleDrawableBuildTimeNs = integerBase;
        before.largeScene.opaqueQueueFinalizationTimeNs = integerBase;
        before.largeScene.visibleObjectIndexAssignmentTimeNs = integerBase;
        before.largeScene.visibilityTestedPrimitiveCount = integerBase;
        before.largeScene.culledByReason[3] = integerBase;
        before.largeScene.residentGpuBytes = integerBase;

        auto after = before;
        after.publishedFrameCount += 301u;
        after.reservedSlotWaitCount += 302u;
        after.reservedSlotWaitTimeoutCount += 303u;
        after.reservedSlotWaitTotalNs += 304u;
        after.reservedSlotWaitMaxNs += 305u;
        after.latestPublishedFrameId += 306u;
        after.latestRetiredFrameId += 307u;
        after.preparedStaticBaseCacheHitCount += 308u;
        after.preparedStaticBaseCacheMissCount += 309u;
        after.staticDrawFastPathHitCount += 310u;
        after.staticDrawFastPathMissCount += 311u;
        after.objectDataRevisionHitCount += 312u;
        after.objectDataRevisionFallbackCount += 313u;
        after.objectDataRevisionDescriptorFallbackCount += 320u;
        after.objectDataRevisionMetadataUnavailableCount += 321u;
        after.objectDataRevisionMetadataUninitializedCount += 328u;
        after.objectDataRevisionMetadataMismatchCount += 322u;
        after.objectDataRevisionMetadataInvalidCount += 323u;
        after.objectDataRevisionStableIdentityMismatchCount += 324u;
        after.objectDataRevisionTransformMismatchCount += 325u;
        after.objectDataRevisionObjectIndexMismatchCount += 326u;
        after.objectDataRevisionObjectCountMismatchCount += 327u;
        after.opaqueSortTokenHitCount += 314u;
        after.opaqueSortTokenRebuildCount += 315u;
        after.descriptorAllocationFailureCount += 316u;
        after.deviceLostCount += 317u;
        after.unsafeGpuQuarantineCount += 318u;
        after.objectDataOverflowCount += 319u;
        after.largeScene.sceneRenderContentRevisionFastPathCount += 329u;
        after.largeScene.syncTimeNs += 330u;
        after.largeScene.serialVisibilityTimeNs += 331u;
        after.largeScene.parallelVisibilityTimeNs += 332u;
        after.largeScene.queueFinalizationTimeNs += 333u;
        after.largeScene.visibleDrawableBuildTimeNs += 337u;
        after.largeScene.opaqueQueueFinalizationTimeNs += 338u;
        after.largeScene.visibleObjectIndexAssignmentTimeNs += 339u;
        after.largeScene.visibilityTestedPrimitiveCount += 334u;
        after.largeScene.culledByReason[3] += 335u;
        after.largeScene.residentGpuBytes = integerBase + 336u;

        std::vector<double> samples(300u, 10.0);
        EditorCameraPerformanceMetadata metadata { "Release", "DX12", false, 30u, 300u };
        metadata.requestedSettleFrameCount = 2u;
        const auto summary = BuildEditorCameraPerformanceSummary(
            std::move(metadata),
            samples,
            before,
            after,
            237u,
            { 6.0, 12.0 });
        const auto outputPath = std::filesystem::temp_directory_path() /
            "nullus-editor-camera-performance-test.json";

        std::string error;
        ASSERT_TRUE(NLS::Editor::Core::WriteEditorCameraPerformanceSummaryJson(
            outputPath,
            summary,
            &error)) << error;

        std::ifstream stream(outputPath);
        const auto document = nlohmann::json::parse(stream);
        stream.close();
        std::filesystem::remove(outputPath);

        ASSERT_TRUE(document.at("measuredFrameCount").is_number_unsigned());
        EXPECT_EQ(document.at("measuredFrameCount").get<uint64_t>(), 300u);
        ASSERT_TRUE(document.at("publishedCameraStepCount").is_number_unsigned());
        EXPECT_EQ(document.at("publishedCameraStepCount").get<uint64_t>(), 237u);
        ASSERT_TRUE(document.at("requestedSettleFrameCount").is_number_unsigned());
        EXPECT_EQ(document.at("requestedSettleFrameCount").get<uint64_t>(), 2u);
        ASSERT_TRUE(document.at("settleFrameCount").is_number_unsigned());
        EXPECT_EQ(document.at("settleFrameCount").get<uint64_t>(), 2u);
        EXPECT_EQ(document.at("settleFrameMs").get<std::vector<double>>(), (std::vector<double> { 6.0, 12.0 }));
        EXPECT_DOUBLE_EQ(document.at("settleMeanFrameMs").get<double>(), 9.0);
        EXPECT_DOUBLE_EQ(document.at("settleMeanFps").get<double>(), 1000.0 / 9.0);
        EXPECT_DOUBLE_EQ(document.at("settleP95FrameMs").get<double>(), 12.0);
        EXPECT_DOUBLE_EQ(document.at("settleP99FrameMs").get<double>(), 12.0);
        EXPECT_DOUBLE_EQ(document.at("settleMaxFrameMs").get<double>(), 12.0);
        ASSERT_TRUE(document.at("publicationRatio").is_number_float());
        EXPECT_DOUBLE_EQ(document.at("publicationRatio").get<double>(), 237.0 / 300.0);

        const auto& telemetry = document.at("telemetryDelta");
        const auto expectInteger = [&telemetry](const char* field, const uint64_t expected)
        {
            ASSERT_TRUE(telemetry.at(field).is_number_unsigned()) << field;
            EXPECT_EQ(telemetry.at(field).get<uint64_t>(), expected) << field;
        };
        expectInteger("blockedPublishCount", 63u);
        expectInteger("publishedFrameCount", 301u);
        expectInteger("reservedSlotWaitCount", 302u);
        expectInteger("reservedSlotWaitTimeoutCount", 303u);
        expectInteger("reservedSlotWaitTotalNs", 304u);
        expectInteger("reservedSlotWaitMaxNs", integerBase + 305u);
        expectInteger("latestPublishedFrameId", integerBase + 306u);
        expectInteger("latestRetiredFrameId", integerBase + 307u);
        expectInteger("preparedStaticBaseCacheHitCount", 308u);
        expectInteger("preparedStaticBaseCacheMissCount", 309u);
        expectInteger("staticDrawFastPathHitCount", 310u);
        expectInteger("staticDrawFastPathMissCount", 311u);
        expectInteger("objectDataRevisionHitCount", 312u);
        expectInteger("objectDataRevisionFallbackCount", 313u);
        expectInteger("objectDataRevisionDescriptorFallbackCount", 320u);
        expectInteger("objectDataRevisionMetadataUnavailableCount", 321u);
        expectInteger("objectDataRevisionMetadataUninitializedCount", 328u);
        expectInteger("objectDataRevisionMetadataMismatchCount", 322u);
        expectInteger("objectDataRevisionMetadataInvalidCount", 323u);
        expectInteger("objectDataRevisionStableIdentityMismatchCount", 324u);
        expectInteger("objectDataRevisionTransformMismatchCount", 325u);
        expectInteger("objectDataRevisionObjectIndexMismatchCount", 326u);
        expectInteger("objectDataRevisionObjectCountMismatchCount", 327u);
        expectInteger("opaqueSortTokenHitCount", 314u);
        expectInteger("opaqueSortTokenRebuildCount", 315u);
        expectInteger("descriptorAllocationFailureCount", 316u);
        expectInteger("deviceLostCount", 317u);
        expectInteger("unsafeGpuQuarantineCount", 318u);
        expectInteger("objectDataOverflowCount", 319u);
        const auto& largeScene = telemetry.at("largeScene");
        const auto expectLargeSceneInteger = [&largeScene](const char* field, const uint64_t expected)
        {
            ASSERT_TRUE(largeScene.at(field).is_number_unsigned()) << field;
            EXPECT_EQ(largeScene.at(field).get<uint64_t>(), expected) << field;
        };
        expectLargeSceneInteger("sceneRenderContentRevisionFastPathCount", 329u);
        expectLargeSceneInteger("syncTimeNs", 330u);
        expectLargeSceneInteger("serialVisibilityTimeNs", 331u);
        expectLargeSceneInteger("parallelVisibilityTimeNs", 332u);
        expectLargeSceneInteger("queueFinalizationTimeNs", 333u);
        expectLargeSceneInteger("visibleDrawableBuildTimeNs", 337u);
        expectLargeSceneInteger("opaqueQueueFinalizationTimeNs", 338u);
        expectLargeSceneInteger("visibleObjectIndexAssignmentTimeNs", 339u);
        expectLargeSceneInteger("visibilityTestedPrimitiveCount", 334u);
        expectLargeSceneInteger("residentGpuBytes", integerBase + 336u);
        ASSERT_TRUE(largeScene.at("culledByReason").is_array());
        ASSERT_GE(largeScene.at("culledByReason").size(), 4u);
        EXPECT_EQ(largeScene.at("culledByReason").at(3).get<uint64_t>(), 335u);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, SerializesMeasurementWindowWaitMaximumWithoutHistoricalSubtraction)
    {
        EditorCameraPerformanceTelemetry before;
        before.reservedSlotWaitMaxNs = 10000000u;
        EditorCameraPerformanceTelemetry after;
        after.reservedSlotWaitMaxNs = 8000000u;

        const auto outputPath = std::filesystem::temp_directory_path() /
            "nullus-editor-camera-performance-window-max-test.json";
        const auto writeAndReadMaximum = [&](const uint64_t measuredMaximum)
        {
            after.reservedSlotWaitMaxNs = measuredMaximum;
            const auto summary = BuildEditorCameraPerformanceSummary(
                EditorCameraPerformanceMetadata {},
                { 8.0 },
                before,
                after,
                1u);
            std::string error;
            EXPECT_TRUE(NLS::Editor::Core::WriteEditorCameraPerformanceSummaryJson(
                outputPath,
                summary,
                &error)) << error;
            std::ifstream stream(outputPath);
            const auto document = nlohmann::json::parse(stream);
            return document.at("telemetryDelta").at("reservedSlotWaitMaxNs").get<uint64_t>();
        };

        EXPECT_EQ(writeAndReadMaximum(8000000u), 8000000u);
        EXPECT_EQ(writeAndReadMaximum(12000000u), 12000000u);
        std::filesystem::remove(outputPath);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, BlockedPublicationCountSaturatesWhenPublishedCountExceedsSamples)
    {
        const auto summary = BuildEditorCameraPerformanceSummary(
            EditorCameraPerformanceMetadata {},
            { 8.0, 12.0 },
            EditorCameraPerformanceTelemetry {},
            EditorCameraPerformanceTelemetry {},
            3u);

        EXPECT_EQ(summary.telemetryDelta.blockedPublishCount, 0u);
        EXPECT_DOUBLE_EQ(summary.publicationRatio, 1.5);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, RuntimeTimesOnlyCompletedCameraStepsAndWritesAfterSampling)
    {
        const auto applicationSource = ReadRepositoryTextFile("Project/Editor/Core/Application.cpp");
        const auto editorHeader = ReadRepositoryTextFile("Project/Editor/Core/Editor.h");
        const auto viewHeader = ReadRepositoryTextFile("Project/Editor/Panels/AView.h");
        const auto mainSource = ReadRepositoryTextFile("Project/Editor/Main.cpp");

        const auto completedBefore = applicationSource.find("GetValidationCameraForwardCompletedFrames()");
        const auto resetWaitMaximum = applicationSource.find(
            "ResetThreadedReservedSlotWaitMeasurementWindowMaxNs",
            completedBefore);
        const auto telemetryBefore = applicationSource.find(
            "m_cameraPerformanceTelemetryBefore = CaptureCameraPerformanceTelemetry()",
            completedBefore);
        const auto frameStart = applicationSource.find("const auto frameStart = std::chrono::steady_clock::now()");
        const auto tick = applicationSource.find("TickFrame(kEditorCameraPerformanceFixedDeltaSeconds, true)");
        const auto frameEnd = applicationSource.find("const auto frameEnd = std::chrono::steady_clock::now()");
        const auto addSample = applicationSource.find("m_cameraPerformanceFrameMs.push_back");
        const auto addSettleSample = applicationSource.find("m_cameraPerformanceSettleFrameMs.push_back");
        const auto writeSummary = applicationSource.find("WriteEditorCameraPerformanceSummaryJson");

        ASSERT_NE(completedBefore, std::string::npos);
        ASSERT_NE(resetWaitMaximum, std::string::npos);
        ASSERT_NE(telemetryBefore, std::string::npos);
        ASSERT_NE(frameStart, std::string::npos);
        ASSERT_NE(tick, std::string::npos);
        ASSERT_NE(frameEnd, std::string::npos);
        ASSERT_NE(addSample, std::string::npos);
        ASSERT_NE(addSettleSample, std::string::npos);
        ASSERT_NE(writeSummary, std::string::npos);
        EXPECT_LT(completedBefore, frameStart);
        EXPECT_LT(completedBefore, resetWaitMaximum);
        EXPECT_LT(resetWaitMaximum, telemetryBefore);
        EXPECT_LT(completedBefore, telemetryBefore);
        EXPECT_LT(telemetryBefore, frameStart);
        EXPECT_LT(frameStart, tick);
        EXPECT_LT(tick, frameEnd);
        EXPECT_LT(frameEnd, addSample);
        EXPECT_LT(addSample, addSettleSample);
        EXPECT_LT(addSample, writeSummary);
        EXPECT_NE(editorHeader.find("GetValidationCameraForwardCompletedFrames() const"), std::string::npos);
        EXPECT_NE(editorHeader.find("WasLastSceneViewThreadedFramePublished() const"), std::string::npos);
        EXPECT_NE(viewHeader.find("WasLastRenderFramePublished() const"), std::string::npos);
        EXPECT_NE(mainSource.find("return app->DidRunSuccessfully();"), std::string::npos);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, RuntimeDoesNotAdvanceCameraOutsideTimedBenchmarkFrame)
    {
        const auto applicationSource = ReadRepositoryTextFile("Project/Editor/Core/Application.cpp");
        const auto benchmarkStart = applicationSource.find(
            "if (!diagnostics.editorCameraPerformanceOutput.empty())");
        const auto normalLoopStart = applicationSource.find(
            "Time::Clock clock;",
            benchmarkStart);

        ASSERT_NE(benchmarkStart, std::string::npos);
        ASSERT_NE(normalLoopStart, std::string::npos);
        const auto benchmarkLoop = applicationSource.substr(
            benchmarkStart,
            normalLoopStart - benchmarkStart);
        const auto normalLoop = applicationSource.substr(normalLoopStart);

        EXPECT_EQ(benchmarkLoop.find("FlushDeferredResizeTick();"), std::string::npos);
        EXPECT_NE(normalLoop.find("FlushDeferredResizeTick();"), std::string::npos);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, ContextPropagatesAllBenchmarkOverrides)
    {
        const auto contextHeader = ReadRepositoryTextFile("Project/Editor/Core/Context.h");
        EXPECT_NE(contextHeader.find("settings.editorCameraPerformanceOutput"), std::string::npos);
        EXPECT_NE(contextHeader.find("settings.editorCameraPerformanceWarmupFrames"), std::string::npos);
        EXPECT_NE(contextHeader.find("settings.editorCameraPerformanceFrames"), std::string::npos);
        EXPECT_NE(contextHeader.find("settings.editorCameraPerformanceSettleFrames"), std::string::npos);
    }
}
