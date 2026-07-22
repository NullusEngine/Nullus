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
}
