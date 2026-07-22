#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
            EditorCameraPerformanceTelemetry {});

        EXPECT_DOUBLE_EQ(summary.meanFrameMs, 30.0);
        EXPECT_DOUBLE_EQ(summary.meanFps, 1000.0 / 30.0);
        EXPECT_DOUBLE_EQ(summary.p95FrameMs, 50.0);
        EXPECT_DOUBLE_EQ(summary.p99FrameMs, 50.0);
        EXPECT_DOUBLE_EQ(summary.maxFrameMs, 50.0);
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
        EditorCameraPerformanceTelemetry after = before;
        after.publishedFrameCount = 44u;
        after.reservedSlotWaitTotalNs = 350u;

        const auto delta = CalculateEditorCameraPerformanceTelemetryDelta(before, after);
        EXPECT_EQ(delta.publishedFrameCount, 4u);
        EXPECT_EQ(delta.reservedSlotWaitTotalNs, 250u);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, SerializesRequiredSummaryFields)
    {
        const auto summary = BuildEditorCameraPerformanceSummary(
            EditorCameraPerformanceMetadata { "Release", "DX12", false, 30u, 2u },
            { 8.0, 12.0 },
            EditorCameraPerformanceTelemetry {},
            EditorCameraPerformanceTelemetry {});
        const auto outputPath = std::filesystem::temp_directory_path() /
            "nullus-editor-camera-performance-test.json";

        std::string error;
        ASSERT_TRUE(NLS::Editor::Core::WriteEditorCameraPerformanceSummaryJson(
            outputPath,
            summary,
            &error)) << error;

        std::string body;
        {
            std::ifstream stream(outputPath);
            body.assign(std::istreambuf_iterator<char>(stream), {});
        }
        std::filesystem::remove(outputPath);
        EXPECT_NE(body.find("\"meanFrameMs\""), std::string::npos);
        EXPECT_NE(body.find("\"meanFps\""), std::string::npos);
        EXPECT_NE(body.find("\"p99FrameMs\""), std::string::npos);
        EXPECT_NE(body.find("\"telemetryDelta\""), std::string::npos);
    }

    TEST(EditorCameraPerformanceBenchmarkTests, RuntimeTimesOnlyCompletedCameraStepsAndWritesAfterSampling)
    {
        const auto applicationSource = ReadRepositoryTextFile("Project/Editor/Core/Application.cpp");
        const auto editorHeader = ReadRepositoryTextFile("Project/Editor/Core/Editor.h");
        const auto viewHeader = ReadRepositoryTextFile("Project/Editor/Panels/AView.h");
        const auto mainSource = ReadRepositoryTextFile("Project/Editor/Main.cpp");

        const auto completedBefore = applicationSource.find("GetValidationCameraForwardCompletedFrames()");
        const auto frameStart = applicationSource.find("const auto frameStart = std::chrono::steady_clock::now()");
        const auto tick = applicationSource.find("TickFrame(kEditorCameraPerformanceFixedDeltaSeconds, true)");
        const auto frameEnd = applicationSource.find("const auto frameEnd = std::chrono::steady_clock::now()");
        const auto addSample = applicationSource.find("m_cameraPerformanceFrameMs.push_back");
        const auto writeSummary = applicationSource.find("WriteEditorCameraPerformanceSummaryJson");

        ASSERT_NE(completedBefore, std::string::npos);
        ASSERT_NE(frameStart, std::string::npos);
        ASSERT_NE(tick, std::string::npos);
        ASSERT_NE(frameEnd, std::string::npos);
        ASSERT_NE(addSample, std::string::npos);
        ASSERT_NE(writeSummary, std::string::npos);
        EXPECT_LT(completedBefore, frameStart);
        EXPECT_LT(frameStart, tick);
        EXPECT_LT(tick, frameEnd);
        EXPECT_LT(frameEnd, addSample);
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
    }
}
