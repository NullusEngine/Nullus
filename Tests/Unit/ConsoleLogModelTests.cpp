#include "Panels/ConsoleLogModel.h"
#include "Panels/ConsoleTextSearch.h"
#include "Panels/ConsoleSourceLocation.h"

#include <gtest/gtest.h>
#include <chrono>
#include <fstream>

using namespace NLS::Editor::Panels;
using NLS::Debug::ELogLevel;

namespace
{
ConsoleLogEntry Entry(std::string message, ELogLevel level = ELogLevel::LOG_INFO,
    ConsoleEntrySource source = ConsoleEntrySource::General)
{
    ConsoleLogEntry entry;
    entry.message = std::move(message);
    entry.level = level;
    entry.source = source;
    return entry;
}
}

TEST(ConsoleLogModelTests, CollapseUsesSourceLocationAndStackButCountsOccurrences)
{
    ConsoleLogModel model;
    auto first = Entry("Repeated message");
    first.timestamp = "10:00:00";
    first.sourcePath = "first.cpp";
    first.stackTrace = "frame A";
    const auto firstId = model.Add(first);
    first.timestamp = "10:00:01";
    const auto selectedDuplicate = model.Add(first);
    first.sourcePath = "second.cpp";
    model.Add(first);
    first.sourcePath = "first.cpp";
    first.stackTrace = "frame B";
    model.Add(first);
    model.Add(Entry("Warning", ELogLevel::LOG_WARNING));

    ConsoleLogFilter filter;
    filter.collapse = true;
    const auto rows = model.BuildRows(filter, selectedDuplicate);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].entry->id, firstId);
    EXPECT_EQ(rows[0].count, 2u);
    EXPECT_TRUE(rows[0].containsSelection);
    EXPECT_EQ(model.Counts().messages, 4u);
    EXPECT_EQ(model.Counts().warnings, 1u);

    filter.messages = false;
    EXPECT_EQ(model.BuildRows(filter).size(), 1u);
    EXPECT_EQ(model.Counts().messages, 4u);
}

TEST(ConsoleLogModelTests, ClearAndCapacityPreserveOnlyCurrentCompilerErrors)
{
    ConsoleLogModel model(2);
    auto compiler = Entry("compiler error", ELogLevel::LOG_ERROR, ConsoleEntrySource::Build);
    compiler.compilerDiagnostic = true;
    const auto compilerId = model.Add(compiler);
    const auto evicted = model.Add(Entry("old"));
    const auto kept = model.Add(Entry("new"));
    model.Add(Entry("newest"));
    EXPECT_EQ(model.Find(evicted), nullptr);
    EXPECT_NE(model.Find(kept), nullptr);
    EXPECT_NE(model.Find(compilerId), nullptr);
    EXPECT_EQ(model.DroppedCount(), 1u);
    model.Clear();
    ASSERT_EQ(model.Entries().size(), 1u);
    EXPECT_EQ(model.Entries().front().id, compilerId);
    model.ReplaceCompilerDiagnostics({});
    EXPECT_TRUE(model.Entries().empty());
    EXPECT_GT(model.Add(Entry("after clear")), compilerId);
}

TEST(ConsoleLogModelTests, DefaultAndInfoCollapseAsTheSameMessageSeverity)
{
    ConsoleLogModel model;
    model.Add(Entry("same message", ELogLevel::LOG_DEFAULT));
    model.Add(Entry("same message", ELogLevel::LOG_INFO));
    ConsoleLogFilter filter;
    filter.collapse = true;
    const auto rows = model.BuildRows(filter);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].count, 2u);
    EXPECT_EQ(model.Counts().messages, 2u);
}

TEST(ConsoleLogModelTests, SearchUsesCompleteTextAndStaleResultsDoNotResurrect)
{
    ConsoleLogModel model;
    auto entry = Entry("first line\nHidden Mismatch", ELogLevel::LOG_WARNING, ConsoleEntrySource::CSharp);
    entry.sourcePath = "Assets/Script.cs";
    const auto id = model.Add(entry);
    EXPECT_TRUE(model.SetResolvedStack(id, "Frame at Assets/Script.cs:42"));
    ConsoleLogFilter filter;
    filter.search = "mIsMaTcH";
    EXPECT_EQ(model.BuildRows(filter).size(), 1u);
    filter.search = "script.cs:42";
    EXPECT_EQ(model.BuildRows(filter).size(), 1u);
    filter.csharp = false;
    EXPECT_TRUE(model.BuildRows(filter).empty());
    model.Clear();
    EXPECT_FALSE(model.SetResolvedStack(id, "late symbol"));
    EXPECT_TRUE(model.Entries().empty());
}

TEST(ConsoleLogModelTests, EntriesAreMergedByReceiptTime)
{
    ConsoleLogModel model;
    auto later = Entry("later");
    auto earlier = Entry("earlier");
    later.received = std::chrono::steady_clock::time_point{} + std::chrono::seconds(2);
    earlier.received = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1);
    model.Add(later);
    model.Add(earlier);
    const auto rows = model.BuildRows({});
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].entry->message, "earlier");
    EXPECT_EQ(rows[1].entry->message, "later");
}

TEST(ConsoleLogModelTests, UnicodeSearchReturnsOriginalUtf8HighlightOffsets)
{
    const std::string text = "Start \xc3\x84PFEL and \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
    const ConsoleTextSearch latin("\xc3\xa4pfel");
    const auto first = latin.Find(text);
    ASSERT_TRUE(first);
    EXPECT_EQ(text.substr(first.begin, first.end - first.begin), "\xc3\x84PFEL");
    const ConsoleTextSearch cyrillic("\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82");
    EXPECT_TRUE(cyrillic.Find(text));
}

TEST(ConsoleLogModelTests, StackLocationsHandleCSharpLuaAndNativePaths)
{
    const auto csharp = ParseConsoleStackLocation("at Foo() in C:\\Project Files\\Player.cs:line 42");
    EXPECT_EQ(csharp.path, "C:\\Project Files\\Player.cs");
    EXPECT_EQ(csharp.line, 42);
    const auto lua = ParseConsoleStackLocation("[string \"Assets/Probe.lua\"]:7: in function 'Update'");
    EXPECT_EQ(lua.path, "Assets/Probe.lua");
    EXPECT_EQ(lua.line, 7);
    const auto native = ParseConsoleStackLocation("0x123 engine!Run (D:\\Code\\File.cpp:123)");
    EXPECT_EQ(native.path, "D:\\Code\\File.cpp");
    EXPECT_EQ(native.line, 123);
    EXPECT_TRUE(ParseConsoleStackLocation("no source available").path.empty());
}

TEST(ConsoleLogModelTests, SourcePathsResolveAgainstTheProjectRatherThanTheEditorWorkingDirectory)
{
    const auto project = std::filesystem::current_path() / "build" / "console-validation" /
        ("source-path-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto assets = project / "Assets";
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(assets, error));
    ASSERT_FALSE(error);
    const auto source = assets / "Probe.lua";
    { std::ofstream file(source); file << "return {}"; }
    EXPECT_EQ(ResolveConsoleSourcePath(std::filesystem::path("Assets") / "Probe.lua", project), source);
    EXPECT_EQ(ResolveConsoleSourcePath(source, project), source);
    EXPECT_EQ(ResolveConsoleSourcePath("missing.lua", project), std::filesystem::path("missing.lua"));
    std::filesystem::remove(source, error); EXPECT_FALSE(error);
    std::filesystem::remove(assets, error); EXPECT_FALSE(error);
    std::filesystem::remove(project, error); EXPECT_FALSE(error);
}
