#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    std::string ReadRepositoryTextFile(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), {});
    }

    TEST(EditorCameraPerformanceScriptTests, RunnerUsesIdenticalDeterministicInputsAndSequentialTrials)
    {
        const auto source = ReadRepositoryTextFile("Tools/Performance/RunEditorCameraBenchmark.ps1");

        EXPECT_NE(source.find("[ValidateSet('Debug', 'Release')]"), std::string::npos);
        EXPECT_NE(source.find("[ValidateSet('before', 'after')]"), std::string::npos);
        EXPECT_NE(source.find("[int]$Trials = 3"), std::string::npos);
        EXPECT_NE(source.find("--backend"), std::string::npos);
        EXPECT_NE(source.find("dx12"), std::string::npos);
        EXPECT_NE(source.find("--editor-validation-exclusive-view"), std::string::npos);
        EXPECT_NE(source.find("--editor-validation-scene-camera"), std::string::npos);
        EXPECT_NE(source.find("-10,3,10;0,135,0"), std::string::npos);
        EXPECT_NE(source.find("--editor-camera-performance-warmup-frames"), std::string::npos);
        EXPECT_NE(source.find("--editor-camera-performance-frames"), std::string::npos);
        EXPECT_NE(source.find("Start-Process"), std::string::npos);
        EXPECT_NE(source.find("-Wait"), std::string::npos);
    }

    TEST(EditorCameraPerformanceScriptTests, ComparatorSelectsMedianMeanFrameTimeAndWritesJsonAndMarkdown)
    {
        const auto source = ReadRepositoryTextFile("Tools/Performance/CompareEditorCameraBenchmark.ps1");

        EXPECT_NE(source.find("Sort-Object meanFrameMs"), std::string::npos);
        EXPECT_NE(source.find("[math]::Floor($runs.Count / 2)"), std::string::npos);
        EXPECT_NE(source.find("ConvertTo-Json"), std::string::npos);
        EXPECT_NE(source.find("Set-Content -LiteralPath $JsonOutput"), std::string::npos);
        EXPECT_NE(source.find("Set-Content -LiteralPath $MarkdownOutput"), std::string::npos);
        EXPECT_NE(source.find("Debug"), std::string::npos);
        EXPECT_NE(source.find("Release"), std::string::npos);
        EXPECT_NE(source.find("p99FrameMs"), std::string::npos);
        EXPECT_NE(source.find("publicationRatio"), std::string::npos);
    }
}
