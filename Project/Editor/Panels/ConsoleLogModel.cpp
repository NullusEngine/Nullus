#include "Panels/ConsoleLogModel.h"
#include "Panels/ConsoleTextSearch.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace NLS::Editor::Panels
{
namespace
{
void AppendKey(std::string& key, const std::string& value)
{
    key += std::to_string(value.size());
    key.push_back(':');
    key += value;
}

std::string GroupKey(const ConsoleLogEntry& entry)
{
    const int severity = entry.level == Debug::ELogLevel::LOG_ERROR ? 2 :
        entry.level == Debug::ELogLevel::LOG_WARNING ? 1 : 0;
    std::string key = std::to_string(severity) + ":" +
        std::to_string(static_cast<int>(entry.source)) + ":" +
        std::to_string(entry.line) + ":" + std::to_string(entry.column) + ":";
    AppendKey(key, entry.message);
    AppendKey(key, entry.sourcePath);
    AppendKey(key, entry.scriptStack);
    // Symbol resolution must not change grouping for the same captured frames.
    if (entry.nativeFrames.empty()) AppendKey(key, entry.stackTrace);
    for (const auto frame : entry.nativeFrames) key += std::to_string(frame) + ":";
    return key;
}

bool Matches(const ConsoleLogEntry& entry, const ConsoleLogFilter& filter, const ConsoleTextSearch& query)
{
    switch (entry.level)
    {
    case Debug::ELogLevel::LOG_DEFAULT:
    case Debug::ELogLevel::LOG_INFO: if (!filter.messages) return false; break;
    case Debug::ELogLevel::LOG_WARNING: if (!filter.warnings) return false; break;
    case Debug::ELogLevel::LOG_ERROR: if (!filter.errors) return false; break;
    }
    if ((entry.source == ConsoleEntrySource::CSharp && !filter.csharp) ||
        (entry.source == ConsoleEntrySource::Lua && !filter.lua) ||
        (entry.source == ConsoleEntrySource::Build && !filter.build)) return false;
    if (query.Empty()) return true;
    const char* source = entry.source == ConsoleEntrySource::CSharp ? "C#" :
        entry.source == ConsoleEntrySource::Lua ? "Lua" : entry.source == ConsoleEntrySource::Build ? "Build" : "Editor";
    const auto text = entry.timestamp + "\n" + entry.message + "\n" + source + "\n" + entry.sourcePath +
        ":" + std::to_string(entry.line) + ":" + std::to_string(entry.column) + "\n" + entry.stackTrace;
    return static_cast<bool>(query.Find(text));
}
}

bool ConsoleLogModel::IsPersistentCompilerError(const ConsoleLogEntry& entry)
{
    return entry.compilerDiagnostic && entry.level == Debug::ELogLevel::LOG_ERROR;
}

uint64_t ConsoleLogModel::Add(ConsoleLogEntry entry)
{
    entry.id = ++m_nextId;
    const auto id = entry.id;
    if (!IsPersistentCompilerError(entry)) ++m_ordinaryCount;
    // Script and native queues are drained independently; use receipt time to
    // merge them, preserving insertion order for equal timestamps.
    const auto position = std::upper_bound(m_entries.begin(), m_entries.end(), entry.received,
        [](const auto& received, const ConsoleLogEntry& current) { return received < current.received; });
    m_entries.insert(position, std::move(entry));
    EnforceCapacity();
    ++m_revision;
    return id;
}

void ConsoleLogModel::EnforceCapacity()
{
    while (m_ordinaryCount > m_capacity)
    {
        const auto oldest = std::find_if(m_entries.begin(), m_entries.end(),
            [](const auto& entry) { return !IsPersistentCompilerError(entry); });
        if (oldest == m_entries.end()) break;
        m_entries.erase(oldest);
        --m_ordinaryCount;
        ++m_dropped;
    }
}

void ConsoleLogModel::Clear()
{
    std::erase_if(m_entries, [](const auto& entry) { return !IsPersistentCompilerError(entry); });
    m_ordinaryCount = 0;
    m_dropped = 0;
    ++m_revision;
}

void ConsoleLogModel::ReplaceCompilerDiagnostics(std::vector<ConsoleLogEntry> diagnostics)
{
    std::erase_if(m_entries, [](const auto& entry) { return entry.compilerDiagnostic; });
    m_ordinaryCount = static_cast<size_t>(std::count_if(m_entries.begin(), m_entries.end(),
        [](const auto& entry) { return !IsPersistentCompilerError(entry); }));
    ++m_revision;
    for (auto& entry : diagnostics)
    {
        entry.compilerDiagnostic = true;
        Add(std::move(entry));
    }
}

bool ConsoleLogModel::SetResolvedStack(uint64_t id, std::string stack)
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [id](const auto& entry) { return entry.id == id; });
    if (it == m_entries.end()) return false;
    it->stackTrace = it->scriptStack;
    if (!it->stackTrace.empty() && !stack.empty()) it->stackTrace += '\n';
    it->stackTrace += std::move(stack);
    ++m_revision;
    return true;
}

const ConsoleLogEntry* ConsoleLogModel::Find(uint64_t id) const
{
    const auto it = std::find_if(m_entries.begin(), m_entries.end(), [id](const auto& entry) { return entry.id == id; });
    return it == m_entries.end() ? nullptr : &*it;
}

std::vector<ConsoleLogRow> ConsoleLogModel::BuildRows(const ConsoleLogFilter& filter, uint64_t selectedId) const
{
    std::vector<ConsoleLogRow> rows;
    std::unordered_map<std::string, size_t> groups;
    const ConsoleTextSearch query(filter.search);
    for (const auto& entry : m_entries)
    {
        if (!Matches(entry, filter, query)) continue;
        if (filter.collapse)
        {
            auto key = GroupKey(entry);
            const auto [it, inserted] = groups.emplace(std::move(key), rows.size());
            if (!inserted)
            {
                ++rows[it->second].count;
                rows[it->second].containsSelection |= entry.id == selectedId;
                continue;
            }
        }
        rows.push_back({&entry, 1, entry.id == selectedId});
    }
    return rows;
}

ConsoleLogCounts ConsoleLogModel::Counts() const
{
    ConsoleLogCounts counts;
    for (const auto& entry : m_entries)
    {
        switch (entry.level)
        {
        case Debug::ELogLevel::LOG_DEFAULT:
        case Debug::ELogLevel::LOG_INFO: ++counts.messages; break;
        case Debug::ELogLevel::LOG_WARNING: ++counts.warnings; break;
        case Debug::ELogLevel::LOG_ERROR: ++counts.errors; break;
        }
    }
    return counts;
}
}
