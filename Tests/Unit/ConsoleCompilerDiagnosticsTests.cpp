#include "Scripting/ManagedScriptBuildService.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>

using namespace NLS::Editor::Scripting;

TEST(ConsoleCompilerDiagnosticsTests, ReadsSarifVersionsAndDeduplicatesLocations)
{
    const auto directory = std::filesystem::current_path() / "build" / "console-validation" / "sarif-test";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    ASSERT_FALSE(error);
    const auto first = directory / "v1.sarif";
    const auto second = directory / "v2.sarif";
    {
        std::ofstream output(first, std::ios::binary);
        output << R"({"runs":[{"results":[{"level":"error","ruleId":"CS1234","message":"broken","locations":[{"resultFile":{"uri":"Assets/Source%20File.cs","region":{"startLine":12,"startColumn":3}}}]}]}]})";
    }
    {
        std::ofstream output(second, std::ios::binary);
        output << R"({"runs":[{"results":[{"level":"error","ruleId":"CS1234","message":{"text":"broken"},"locations":[{"physicalLocation":{"artifactLocation":{"uri":"Assets/Source%20File.cs"},"region":{"startLine":12,"startColumn":3}}}]},{"level":"warning","message":{"text":"careful"}}]}]})";
    }
    ManagedScriptBuildResult build;
    build.projectRoot = directory;
    build.sarifDiagnosticFiles = {first, second};
    const auto diagnostics = ManagedScriptBuildService::ReadDiagnostics(build);
    ASSERT_EQ(diagnostics.size(), 2u);
    EXPECT_EQ(diagnostics[0].severity, ManagedBuildDiagnostic::Severity::Error);
    EXPECT_EQ(diagnostics[0].message, "CS1234: broken");
    EXPECT_EQ(diagnostics[0].line, 12);
    EXPECT_EQ(diagnostics[0].column, 3);
    EXPECT_NE(diagnostics[0].sourcePath.find("Source File.cs"), std::string::npos);
    EXPECT_EQ(diagnostics[1].severity, ManagedBuildDiagnostic::Severity::Warning);
}

TEST(ConsoleCompilerDiagnosticsTests, MissingDiagnosticsPreserveFailureSummary)
{
    ManagedScriptBuildResult build;
    build.output = "dotnet could not start";
    const auto diagnostics = ManagedScriptBuildService::ReadDiagnostics(build);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].severity, ManagedBuildDiagnostic::Severity::Error);
    EXPECT_EQ(diagnostics[0].message, build.output);
    build.succeeded = true;
    EXPECT_TRUE(ManagedScriptBuildService::ReadDiagnostics(build).empty());
}

TEST(ConsoleCompilerDiagnosticsTests, EarlyBuildFailureCapturesCurrentOutputInsteadOfStaleLog)
{
    const auto directory = std::filesystem::current_path() / "build" / "console-validation" /
        ("early failure (" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ")");
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(directory, error));
    ASSERT_FALSE(error);
#ifdef _WIN32
    const auto executable = directory / "fake-dotnet.cmd";
#else
    const auto executable = directory / "fake-dotnet.sh";
#endif
    {
        std::ofstream script(executable, std::ios::binary);
#ifdef _WIN32
        script << "@echo FIRST_STEP_FAILED_MARKER\r\n@exit /b 23\r\n";
#else
        script << "#!/bin/sh\necho FIRST_STEP_FAILED_MARKER >&2\nexit 23\n";
#endif
    }
#ifndef _WIN32
    std::filesystem::permissions(executable, std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read,
        std::filesystem::perm_options::add, error);
    ASSERT_FALSE(error);
#endif
    const auto output = directory / "output";
    std::filesystem::create_directories(output, error);
    ASSERT_FALSE(error);
    { std::ofstream stale(output / "last-build.log"); stale << "OLD_SUCCESSFUL_BUILD"; }
    ManagedScriptBuildRequest request;
    request.projectRoot = directory;
    request.outputRoot = output;
    request.dotnetPath = executable;
    const auto result = ManagedScriptBuildService::Build(request);
    EXPECT_FALSE(result.succeeded);
    EXPECT_EQ(result.exitCode, 23);
    EXPECT_NE(result.output.find("FIRST_STEP_FAILED_MARKER"), std::string::npos);
    EXPECT_EQ(result.output.find("OLD_SUCCESSFUL_BUILD"), std::string::npos);
    const auto diagnostics = ManagedScriptBuildService::ReadDiagnostics(result);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_NE(diagnostics[0].message.find("FIRST_STEP_FAILED_MARKER"), std::string::npos);
    std::filesystem::remove(output / "last-build.log", error);
    EXPECT_FALSE(error);
    std::filesystem::remove(output / ".staging", error);
    EXPECT_FALSE(error);
    std::filesystem::remove(output, error);
    EXPECT_FALSE(error);
    std::filesystem::remove(executable, error);
    EXPECT_FALSE(error);
    std::filesystem::remove(directory, error);
    EXPECT_FALSE(error);
}
