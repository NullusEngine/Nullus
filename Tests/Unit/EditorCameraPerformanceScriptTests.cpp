#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <Json/json.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

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

        EXPECT_NE(source.find("[ValidateSet('Debug', 'Release', 'RelWithDebInfo')]"), std::string::npos);
        EXPECT_NE(source.find("[ValidateSet('before', 'after')]"), std::string::npos);
        EXPECT_NE(source.find("[int]$Trials = 3"), std::string::npos);
        EXPECT_NE(source.find("--backend"), std::string::npos);
        EXPECT_NE(source.find("dx12"), std::string::npos);
        EXPECT_NE(source.find("--editor-validation-exclusive-view"), std::string::npos);
        EXPECT_NE(source.find("[ValidateSet('scene', 'game')]"), std::string::npos);
        EXPECT_NE(source.find("--editor-validation-select-gameobject"), std::string::npos);
        EXPECT_NE(source.find("--editor-validation-scene-camera"), std::string::npos);
        EXPECT_NE(source.find("-10,3,10;0,135,0"), std::string::npos);
        EXPECT_NE(source.find("--editor-camera-performance-warmup-frames"), std::string::npos);
        EXPECT_NE(source.find("--editor-camera-performance-frames"), std::string::npos);
        EXPECT_NE(source.find("Start-Process"), std::string::npos);
        EXPECT_NE(source.find("summaryDeadline"), std::string::npos);
        EXPECT_NE(source.find("WaitForExit"), std::string::npos);
    }

#if defined(_WIN32)
    class EditorCameraComparatorTests : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root = std::filesystem::temp_directory_path() /
                ("NullusCameraComparator-" + std::to_string(stamp));
            ownsDirectory = std::filesystem::create_directory(root);
            ASSERT_TRUE(ownsDirectory);
            before = root / "before";
            after = root / "after";
            std::filesystem::create_directory(before);
            std::filesystem::create_directory(after);
        }

        void TearDown() override
        {
            if (ownsDirectory)
            {
                std::error_code error;
                std::filesystem::remove_all(root, error);
            }
        }

        static nlohmann::json RunData(const std::vector<double>& samples)
        {
            return {
                {"backend", "DX12"}, {"configuration", "RelWithDebInfo"},
                {"focusView", "scene"}, {"projectPath", "fixture.nullus"},
                {"scenePath", "fixture.scene"}, {"viewportWidth", 64}, {"viewportHeight", 64},
                {"vsync", false}, {"warmupFrameCount", 20}, {"settleFrameCount", 0},
                {"cameraForwardStep", 0.1}, {"selectedGameObject", ""},
                {"requestedFrameCount", samples.size()}, {"measuredFrameCount", samples.size()},
                {"measuredFrameMs", samples}, {"meanFrameMs", 10.0},
                {"p95FrameMs", 20.0}, {"publicationRatio", 1.0}
            };
        }

        static void WriteRun(const std::filesystem::path& directory, int trial, const nlohmann::json& run)
        {
            std::ofstream stream(directory / ("relwithdebinfo-trial-" + std::to_string(trial) + ".json"));
            stream << run.dump(2);
            ASSERT_TRUE(stream.good());
        }

        DWORD Compare()
        {
            const auto script = std::filesystem::path(NLS_ROOT_DIR) /
                "Tools/Performance/CompareEditorCameraBenchmark.ps1";
            const auto quote = [](const std::filesystem::path& path) { return L"\"" + path.wstring() + L"\""; };
            wchar_t systemDirectory[MAX_PATH]{};
            if (GetSystemDirectoryW(systemDirectory, MAX_PATH) == 0u)
                throw std::runtime_error("Cannot locate Windows PowerShell");
            const auto powershellDirectory = std::filesystem::path(systemDirectory) / "WindowsPowerShell/v1.0";
            const auto executable = powershellDirectory / "powershell.exe";
            std::wstring command = quote(executable) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                quote(script) + L" -BeforeDirectory " + quote(before) + L" -AfterDirectory " + quote(after) +
                L" -Configurations RelWithDebInfo -JsonOutput " + quote(root / "result.json") +
                L" -MarkdownOutput " + quote(root / "result.md") + L" -EnforceFrameTimeGate";
            // A native child of pwsh otherwise inherits its incompatible module
            // search paths instead of Windows PowerShell's built-in modules.
            std::vector<std::wstring> environmentEntries;
            const auto inheritedEnvironment = GetEnvironmentStringsW();
            if (inheritedEnvironment == nullptr)
                throw std::runtime_error("Cannot read the process environment");
            for (auto entry = inheritedEnvironment; *entry != L'\0'; entry += std::wcslen(entry) + 1u)
                if (_wcsnicmp(entry, L"PSModulePath=", 13u) != 0)
                    environmentEntries.emplace_back(entry);
            FreeEnvironmentStringsW(inheritedEnvironment);
            environmentEntries.push_back(L"PSModulePath=" + (powershellDirectory / "Modules").wstring());
            std::sort(environmentEntries.begin(), environmentEntries.end(), [](const auto& left, const auto& right)
            {
                return _wcsicmp(left.c_str(), right.c_str()) < 0;
            });
            std::wstring environment;
            for (const auto& entry : environmentEntries)
            {
                environment.append(entry);
                environment.push_back(L'\0');
            }
            environment.push_back(L'\0');
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            const auto output = CreateFileW((root / "console.log").c_str(), GENERIC_WRITE,
                FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (output == INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cannot capture the PowerShell comparator output");
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = output;
            startup.hStdError = output;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup, &process))
            {
                CloseHandle(output);
                throw std::runtime_error("Cannot start the PowerShell comparator");
            }
            const auto waited = WaitForSingleObject(process.hProcess, 60'000u);
            DWORD exitCode = 1u;
            if (waited == WAIT_OBJECT_0)
                GetExitCodeProcess(process.hProcess, &exitCode);
            else
            {
                TerminateProcess(process.hProcess, 1u);
                WaitForSingleObject(process.hProcess, 5'000u);
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(output);
            diagnostic = ReadRepositoryTextFile(root / "console.log");
            if (waited != WAIT_OBJECT_0)
                throw std::runtime_error("PowerShell comparator timed out");
            return exitCode;
        }

        nlohmann::json Result() const
        {
            std::ifstream stream(root / "result.json", std::ios::binary);
            return nlohmann::json::parse(stream).at("RelWithDebInfo");
        }

        std::filesystem::path root;
        std::filesystem::path before;
        std::filesystem::path after;
        bool ownsDirectory = false;
        std::string diagnostic;
    };

    TEST_F(EditorCameraComparatorTests, PoolsRawFramesAndCalculatesTheEvenMedianAndNearestRankP95)
    {
        for (const auto& directory : {before, after})
        {
            WriteRun(directory, 1, RunData({1.0, 2.0}));
            WriteRun(directory, 2, RunData({3.0, 100.0}));
        }
        ASSERT_EQ(Compare(), 0u) << diagnostic;
        const auto result = Result();
        EXPECT_EQ(result["before"]["runCount"], 2);
        EXPECT_EQ(result["before"]["measuredFrameCount"], 4);
        EXPECT_EQ(result["before"]["medianFrameMs"], 2.5);
        EXPECT_EQ(result["before"]["p95FrameMs"], 100.0);
        EXPECT_EQ(result["before"]["meanFrameMs"], 26.5);
        EXPECT_TRUE(result["frameTimeGate"]["passed"].get<bool>());
        EXPECT_TRUE(std::filesystem::is_regular_file(root / "result.md"));
        EXPECT_EQ(result["before"]["runs"][0]["sha256"].get<std::string>().size(), 64u);
    }

    TEST_F(EditorCameraComparatorTests, RejectsMedianRegressionEvenWhenP95IsUnchanged)
    {
        WriteRun(before, 1, RunData({1.0, 1.0, 2.0, 100.0}));
        WriteRun(after, 1, RunData({2.0, 2.0, 3.0, 100.0}));
        EXPECT_NE(Compare(), 0u);
        ASSERT_TRUE(std::filesystem::exists(root / "result.json")) << diagnostic;
        const auto gate = Result()["frameTimeGate"];
        EXPECT_FALSE(gate["passed"].get<bool>());
        EXPECT_GT(gate["medianRegressionPercent"].get<double>(), 5.0);
        EXPECT_EQ(gate["p95RegressionPercent"], 0.0);
    }

    TEST_F(EditorCameraComparatorTests, RejectsP95RegressionEvenWhenMedianIsUnchanged)
    {
        WriteRun(before, 1, RunData({1.0, 2.0, 3.0, 100.0}));
        WriteRun(after, 1, RunData({1.0, 2.0, 3.0, 106.0}));
        EXPECT_NE(Compare(), 0u);
        ASSERT_TRUE(std::filesystem::exists(root / "result.json")) << diagnostic;
        const auto gate = Result()["frameTimeGate"];
        EXPECT_FALSE(gate["passed"].get<bool>());
        EXPECT_EQ(gate["medianRegressionPercent"], 0.0);
        EXPECT_NEAR(gate["p95RegressionPercent"].get<double>(), 6.0, 1e-10);
    }

    TEST_F(EditorCameraComparatorTests, RejectsMismatchedWorkloadsBeforeWritingAnAcceptanceResult)
    {
        WriteRun(before, 1, RunData({1.0, 2.0}));
        auto unmatched = RunData({1.0, 2.0});
        unmatched["viewportWidth"] = 65;
        WriteRun(after, 1, unmatched);
        EXPECT_NE(Compare(), 0u);
        EXPECT_FALSE(std::filesystem::exists(root / "result.json"));
    }
#endif
}
