#pragma once

#include <Debug/ILogHandler.h>
#include <Scripting/ScriptTypes.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Panels
{
enum class ConsoleEntrySource : uint8_t { General, CSharp, Lua, Build };

struct ConsoleLogEntry
{
    uint64_t id = 0;
    std::chrono::steady_clock::time_point received{};
    ::NLS::Debug::ELogLevel level = ::NLS::Debug::ELogLevel::LOG_DEFAULT;
    ConsoleEntrySource source = ConsoleEntrySource::General;
    std::string timestamp;
    std::string message;
    std::string stackTrace;
    std::string scriptStack;
    std::string sourcePath;
    int line = 0;
    int column = 0;
    std::vector<uintptr_t> nativeFrames;
    bool compilerDiagnostic = false;
    std::optional<NLS::Scripting::ScriptError> diagnostic;
};

struct ConsoleLogFilter
{
    bool messages = true;
    bool warnings = true;
    bool errors = true;
    bool csharp = true;
    bool lua = true;
    bool build = true;
    bool collapse = false;
    std::string search;
};

struct ConsoleLogCounts
{
    size_t messages = 0;
    size_t warnings = 0;
    size_t errors = 0;
};

struct ConsoleLogRow
{
    const ConsoleLogEntry* entry = nullptr;
    size_t count = 1;
    bool containsSelection = false;
};

// Owned and accessed by the editor thread. Producers hand off records through
// Console's pending queue; no ImGui objects or callbacks are stored here.
class ConsoleLogModel final
{
public:
    explicit ConsoleLogModel(size_t capacity = 10000) : m_capacity(capacity) {}

    uint64_t Add(ConsoleLogEntry entry);
    void Clear();
    void ReplaceCompilerDiagnostics(std::vector<ConsoleLogEntry> diagnostics);
    bool SetResolvedStack(uint64_t id, std::string stack);
    const ConsoleLogEntry* Find(uint64_t id) const;
    std::vector<ConsoleLogRow> BuildRows(const ConsoleLogFilter& filter, uint64_t selectedId = 0) const;
    ConsoleLogCounts Counts() const;
    void RecordDropped(uint64_t count) { m_dropped += count; if (count) ++m_revision; }
    const std::deque<ConsoleLogEntry>& Entries() const { return m_entries; }
    uint64_t DroppedCount() const { return m_dropped; }
    uint64_t Revision() const { return m_revision; }

private:
    static bool IsPersistentCompilerError(const ConsoleLogEntry& entry);
    void EnforceCapacity();

    size_t m_capacity;
    size_t m_ordinaryCount = 0;
    uint64_t m_nextId = 0;
    uint64_t m_dropped = 0;
    uint64_t m_revision = 0;
    std::deque<ConsoleLogEntry> m_entries;
};
}
