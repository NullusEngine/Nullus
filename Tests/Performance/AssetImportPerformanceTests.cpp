#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Assets/AssetDatabaseFacade.h"
#include "Profiling/PerformanceStageStats.h"

namespace
{
bool IsEnvironmentFlagEnabledForAssetImportTest(const char* name)
{
    const auto* value = std::getenv(name);
    if (value == nullptr)
        return false;
    const std::string text(value);
    return text == "1" || text == "true" || text == "TRUE" || text == "on" || text == "ON";
}

void WriteAssetImportPerformanceReportIfRequested(
    const std::string& scenarioName,
    const NLS::Base::Profiling::PerformanceStageStatsSnapshot& snapshot,
    const std::chrono::microseconds totalDuration)
{
    const auto* reportDirectory = std::getenv("NLS_PERFORMANCE_REPORT_DIR");
    if (reportDirectory == nullptr || std::string(reportDirectory).empty())
        return;

    std::filesystem::create_directories(reportDirectory);
    NLS::Base::Profiling::PerformanceBenchmarkRun run;
    run.scenarioName = scenarioName;
    run.runType = NLS::Base::Profiling::PerformanceBenchmarkRunType::Baseline;
    run.totalDuration = totalDuration;
    run.stageStats = snapshot;

    std::ofstream output(
        std::filesystem::path(reportDirectory) / (scenarioName + ".txt"),
        std::ios::binary | std::ios::trunc);
    output << NLS::Base::Profiling::FormatPerformanceBenchmarkReport(run);
}
}

TEST(AssetImportPerformanceTests, NewSponzaStandalonePngImportPerformanceReport)
{
    if (!IsEnvironmentFlagEnabledForAssetImportTest("NLS_RUN_PNG_IMPORT_PERF"))
        GTEST_SKIP() << "Set NLS_RUN_PNG_IMPORT_PERF=1 to run the real PNG import benchmark.";

    constexpr const char* kAssetPath =
        "Assets/Model/main_sponza/textures/curtain_fabric_Normal.png";
    const auto projectRoot = std::filesystem::path(NLS_ROOT_DIR) / "TestProject";
    const auto absoluteAssetPath = projectRoot /
        "Assets" / "Model" / "main_sponza" / "textures" / "curtain_fabric_Normal.png";
    if (!std::filesystem::exists(absoluteAssetPath))
        GTEST_SKIP() << "PNG test asset is missing: " << absoluteAssetPath.generic_string();

    NLS::Editor::Assets::AssetDatabaseFacade database({projectRoot});
    const std::vector<std::filesystem::path> refreshPaths {absoluteAssetPath};
    ASSERT_TRUE(database.RefreshKnownSourceAssets(refreshPaths));

    NLS::Base::Profiling::PerformanceStageStats stats;
    std::chrono::microseconds elapsed{0};
    {
        NLS::Base::Profiling::PerformanceStageStatsCapture capture(stats);
        NLS::Editor::Assets::ImportProgressTracker progress;
        const auto begin = std::chrono::steady_clock::now();
        ASSERT_TRUE(database.ReimportAssetFromCurrentDatabase(kAssetPath, progress, 1u));
        elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin);
    }

    const auto snapshot = stats.Snapshot();
    WriteAssetImportPerformanceReportIfRequested(
        "NewSponza_StandalonePngImport",
        snapshot,
        elapsed);
}
