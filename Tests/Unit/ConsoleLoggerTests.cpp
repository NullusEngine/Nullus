#include <Debug/Logger.h>
#include <Debug/StackTrace.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

using namespace NLS::Debug;

namespace
{
class ScopedTraceMode
{
public:
    ScopedTraceMode(ELogLevel level, StackTraceMode mode) : m_level(level), m_old(Logger::GetStackTraceMode(level))
    {
        Logger::SetStackTraceMode(level, mode);
    }
    ~ScopedTraceMode() { Logger::SetStackTraceMode(m_level, m_old); }
private:
    ELogLevel m_level;
    StackTraceMode m_old;
};
}

TEST(ConsoleLoggerTests, MacroEvaluatesOnceAndPreservesOutputModeEvents)
{
    ScopedTraceMode mode(ELogLevel::LOG_INFO, StackTraceMode::None);
    int eventCount = 0;
    LogData captured;
    const auto listener = Logger::LogEvent += [&](const LogData& data) { ++eventCount; captured = data; };
    int evaluated = 0;
    NLS_LOG_INFO((++evaluated, std::string("console logger test")));
    EXPECT_EQ(evaluated, 1);
    EXPECT_EQ(eventCount, 1);
    EXPECT_GT(captured.id, 0u);
    EXPECT_FALSE(captured.sourceFile.empty());
    EXPECT_GT(captured.sourceLine, 0);
    Logger::Log("file only", ELogLevel::LOG_INFO, ELogMode::FILE);
    EXPECT_EQ(eventCount, 1);
    Logger::LogEvent -= listener;
    Logger::Flush();
}

TEST(ConsoleLoggerTests, ConcurrentProducersDeliverEveryRecordOnce)
{
    ScopedTraceMode mode(ELogLevel::LOG_INFO, StackTraceMode::None);
    std::atomic<int> count{0};
    const auto listener = Logger::LogEvent += [&](const LogData& data)
    {
        if (data.message == "concurrent console test") ++count;
    };
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([]
        {
            for (int n = 0; n < 25; ++n)
                Logger::Log("concurrent console test", ELogLevel::LOG_INFO, ELogMode::CONSOLE);
        });
    for (auto& thread : threads) thread.join();
    Logger::LogEvent -= listener;
    EXPECT_EQ(count.load(), 100);
}

TEST(ConsoleLoggerTests, FlushPersistsScriptStackForFileOnlyRecord)
{
    ScopedTraceMode mode(ELogLevel::LOG_INFO, StackTraceMode::ScriptOnly);
    const auto previous = std::filesystem::path(FileHandler::GetLogFilePath());
    const auto directory = std::filesystem::current_path() / "build" / "console-validation" / "log-test";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    ASSERT_FALSE(error);
    FileHandler::SetLogFilePath(directory.string());
    Logger::LogWithContext("flush stack record", ELogLevel::LOG_INFO, ELogMode::FILE,
        "Example.cs", 12, nullptr, ELogSource::CSharp, "Example.Run:12");
    Logger::Flush();
    std::ifstream file(FileHandler::GetLogFilePath(), std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    file.close();
    FileHandler::SetLogFilePath(previous.parent_path().empty() ? "." : previous.parent_path().string());
    EXPECT_NE(content.find("flush stack record"), std::string::npos);
    EXPECT_NE(content.find("Example.Run:12"), std::string::npos);
}

#if defined(_WIN32)
__declspec(noinline) static void EmitNativeStackTestLog()
{
    NLS_LOG_WARNING("native stack test");
}

TEST(ConsoleLoggerTests, FullModeCapturesAndResolvesOrPreservesAddresses)
{
    ScopedTraceMode mode(ELogLevel::LOG_WARNING, StackTraceMode::Full);
    std::shared_ptr<NativeStackTrace> trace;
    const auto listener = Logger::LogEvent += [&](const LogData& data)
    {
        if (data.message == "native stack test") trace = data.nativeStack;
    };
    EmitNativeStackTestLog();
    Logger::LogEvent -= listener;
    ASSERT_NE(trace, nullptr);
    ASSERT_FALSE(trace->frames.empty());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!trace->ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(trace->ready.load(std::memory_order_acquire));
    std::lock_guard lock(trace->mutex);
    EXPECT_NE(trace->resolved.find("0x"), std::string::npos);
    EXPECT_NE(trace->resolved.find("EmitNativeStackTestLog"), std::string::npos) << trace->resolved;
}
#endif
