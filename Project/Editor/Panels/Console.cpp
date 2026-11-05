#include "Panels/Console.h"
#include "Panels/ConsoleTextSearch.h"
#include "Panels/ConsoleSourceLocation.h"

#include <Scripting/ScriptErrorConsole.h>
#include <Core/ServiceLocator.h>
#include <Debug/StackTrace.h>
#include <UI/UIManager.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <utility>

using namespace NLS;
using namespace NLS::Editor::Panels;

namespace
{
Debug::ELogLevel ToLogLevel(Scripting::ScriptErrorSeverity severity)
{
    switch (severity)
    {
    case Scripting::ScriptErrorSeverity::Info: return Debug::ELogLevel::LOG_INFO;
    case Scripting::ScriptErrorSeverity::Warning: return Debug::ELogLevel::LOG_WARNING;
    case Scripting::ScriptErrorSeverity::Error:
    case Scripting::ScriptErrorSeverity::Fatal: return Debug::ELogLevel::LOG_ERROR;
    }
    return Debug::ELogLevel::LOG_ERROR;
}

ConsoleEntrySource ToSource(Scripting::ScriptLanguage language)
{
    if (language == Scripting::ScriptLanguage::CSharp) return ConsoleEntrySource::CSharp;
    if (language == Scripting::ScriptLanguage::Lua) return ConsoleEntrySource::Lua;
    return ConsoleEntrySource::Build;
}

ConsoleEntrySource ToSource(Debug::ELogSource source)
{
    switch (source)
    {
    case Debug::ELogSource::CSharp: return ConsoleEntrySource::CSharp;
    case Debug::ELogSource::Lua: return ConsoleEntrySource::Lua;
    case Debug::ELogSource::Build: return ConsoleEntrySource::Build;
    case Debug::ELogSource::General: return ConsoleEntrySource::General;
    }
    return ConsoleEntrySource::General;
}

void ApplyStackOptions(const ConsoleOptions& options)
{
    Debug::Logger::SetStackTraceMode(Debug::ELogLevel::LOG_DEFAULT, static_cast<Debug::StackTraceMode>(options.messageStack));
    Debug::Logger::SetStackTraceMode(Debug::ELogLevel::LOG_WARNING, static_cast<Debug::StackTraceMode>(options.warningStack));
    Debug::Logger::SetStackTraceMode(Debug::ELogLevel::LOG_ERROR, static_cast<Debug::StackTraceMode>(options.errorStack));
}

const char* SourceName(ConsoleEntrySource source)
{
    switch (source)
    {
    case ConsoleEntrySource::CSharp: return "C#";
    case ConsoleEntrySource::Lua: return "Lua";
    case ConsoleEntrySource::Build: return "Build";
    case ConsoleEntrySource::General: return "Editor";
    }
    return "Editor";
}

std::string ClockText(const std::string& date)
{
    const auto separator = date.find('_');
    if (separator == std::string::npos) return date;
    auto result = date.substr(separator + 1);
    std::replace(result.begin(), result.end(), '-', ':');
    return result;
}

std::string CurrentClock()
{
    const auto now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

std::string ScriptMessage(const Scripting::ScriptError& error)
{
    std::string location = error.sourcePath;
    if (error.line > 0) location += ":" + std::to_string(error.line);
    if (error.column > 0) location += ":" + std::to_string(error.column);
    return "[" + std::string(Scripting::ToString(error.language)) + "] " +
        (location.empty() ? error.message : location + " - " + error.message);
}

std::string EntryText(const ConsoleLogEntry& entry, bool timestamp)
{
    return (timestamp && !entry.timestamp.empty() ? "[" + entry.timestamp + "]  " : "") + entry.message;
}

std::string FullText(const ConsoleLogEntry& entry)
{
    std::string result = EntryText(entry, true);
    if (!entry.sourcePath.empty())
    {
        result += "\n" + entry.sourcePath;
        if (entry.line > 0) result += ":" + std::to_string(entry.line);
        if (entry.column > 0) result += ":" + std::to_string(entry.column);
    }
    if (!entry.stackTrace.empty()) result += "\n" + entry.stackTrace;
    return result;
}

ImU32 SeverityColor(Debug::ELogLevel level)
{
    if (level == Debug::ELogLevel::LOG_ERROR) return IM_COL32(255, 112, 71, 255);
    if (level == Debug::ELogLevel::LOG_WARNING) return IM_COL32(255, 204, 49, 255);
    return IM_COL32(170, 178, 189, 255);
}

void DrawSeverityIcon(ImDrawList* draw, ImVec2 center, Debug::ELogLevel level, float radius)
{
    const auto color = SeverityColor(level);
    if (level == Debug::ELogLevel::LOG_WARNING)
    {
        draw->AddTriangleFilled(ImVec2(center.x, center.y - radius),
            ImVec2(center.x - radius, center.y + radius * .85f),
            ImVec2(center.x + radius, center.y + radius * .85f), color);
    }
    else if (level == Debug::ELogLevel::LOG_ERROR)
    {
        const float corner = radius * .42f;
        draw->PathLineTo(ImVec2(center.x - corner, center.y - radius));
        draw->PathLineTo(ImVec2(center.x + corner, center.y - radius));
        draw->PathLineTo(ImVec2(center.x + radius, center.y - corner));
        draw->PathLineTo(ImVec2(center.x + radius, center.y + corner));
        draw->PathLineTo(ImVec2(center.x + corner, center.y + radius));
        draw->PathLineTo(ImVec2(center.x - corner, center.y + radius));
        draw->PathLineTo(ImVec2(center.x - radius, center.y + corner));
        draw->PathLineTo(ImVec2(center.x - radius, center.y - corner));
        draw->PathFillConvex(color);
    }
    else
    {
        draw->AddCircleFilled(ImVec2(center.x, center.y - radius * .08f), radius * .86f, color, 16);
        draw->AddTriangleFilled(ImVec2(center.x - radius * .72f, center.y + radius * .42f),
            ImVec2(center.x - radius * .83f, center.y + radius),
            ImVec2(center.x - radius * .15f, center.y + radius * .66f), color);
    }
    const auto mark = IM_COL32(47, 48, 51, 255);
    const float thickness = std::max(1.0f, radius * .22f);
    if (level == Debug::ELogLevel::LOG_WARNING || level == Debug::ELogLevel::LOG_ERROR)
    {
        draw->AddLine(ImVec2(center.x, center.y - radius * .45f),
            ImVec2(center.x, center.y + radius * .2f), mark, thickness);
        draw->AddCircleFilled(ImVec2(center.x, center.y + radius * .50f), thickness * .55f, mark, 8);
    }
    else
    {
        draw->AddCircleFilled(ImVec2(center.x, center.y - radius * .42f), thickness * .55f, mark, 8);
        draw->AddLine(ImVec2(center.x, center.y - radius * .13f),
            ImVec2(center.x, center.y + radius * .42f), mark, thickness);
    }
}

void RequestGlyphs(const std::string& text)
{
    if (Core::ServiceLocator::Contains<UI::UIManager>())
        Core::ServiceLocator::Get<UI::UIManager>().RequestFontGlyphs(text);
}

float SeverityButtonWidth(const std::string& count)
{
    return ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFontSize() +
        ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(count.c_str()).x;
}

bool SeverityButton(const char* id, const char* tooltip, Debug::ELogLevel level, const std::string& count, bool& enabled)
{
    if (enabled) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    const bool clicked = ImGui::Button(id, ImVec2(SeverityButtonWidth(count), ImGui::GetFrameHeight()));
    if (enabled) ImGui::PopStyleColor();
    if (clicked) enabled = !enabled;
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    const float iconSize = ImGui::GetFontSize();
    const float centerY = (min.y + max.y) * .5f;
    auto* draw = ImGui::GetWindowDrawList();
    DrawSeverityIcon(draw, ImVec2(min.x + ImGui::GetStyle().FramePadding.x + iconSize * .5f, centerY), level, iconSize * .42f);
    draw->AddText(ImVec2(min.x + ImGui::GetStyle().FramePadding.x + iconSize + ImGui::GetStyle().ItemInnerSpacing.x,
        centerY - ImGui::GetTextLineHeight() * .5f), ImGui::GetColorU32(ImGuiCol_Text), count.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

bool ToggleButton(const char* label, bool& value)
{
    if (value) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    const bool clicked = ImGui::Button(label);
    if (value) ImGui::PopStyleColor();
    if (clicked) value = !value;
    return clicked;
}

void DrawWrappedRowText(ImDrawList* draw, ImVec2 position, float width, int maxLines,
    const std::string& text, const std::string& search)
{
    if (width <= 1.0f) return;
    const ConsoleTextSearch query(search);
    const auto match = query.Empty() ? ConsoleTextMatch{} : query.Find(text);
    const char* begin = text.data();
    const char* end = begin + text.size();
    const float lineHeight = ImGui::GetTextLineHeight();
    auto* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    const float scale = fontSize / font->FontSize;
    for (int line = 0; line < maxLines && begin < end; ++line)
    {
        const char* newline = std::find(begin, end, '\n');
        const char* lineEnd = font->CalcWordWrapPositionA(scale, begin, newline, width);
        if (lineEnd <= begin && begin < newline)
        {
            lineEnd = begin + 1;
            while (lineEnd < newline && (static_cast<unsigned char>(*lineEnd) & 0xc0) == 0x80) ++lineEnd;
        }
        if (match)
        {
            const char* matchBegin = text.data() + match.begin;
            const char* matchEnd = text.data() + match.end;
            const char* visibleBegin = std::max(begin, matchBegin);
            const char* visibleEnd = std::min(lineEnd, matchEnd);
            if (visibleBegin < visibleEnd)
            {
                const float left = ImGui::CalcTextSize(begin, visibleBegin).x;
                const float right = ImGui::CalcTextSize(begin, visibleEnd).x;
                draw->AddRectFilled(ImVec2(position.x + left, position.y),
                    ImVec2(position.x + right, position.y + lineHeight), IM_COL32(135, 108, 36, 180), 2.0f);
            }
        }
        draw->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), begin, lineEnd);
        position.y += lineHeight;
        begin = lineEnd;
        if (begin == newline && begin < end) ++begin;
        else while (begin < newline && *begin == ' ') ++begin;
    }
}
}

Console::Console(const std::string& title, bool opened, const UI::PanelWindowSettings& settings)
    : PanelWindow(title, opened, settings)
{
    panelSettings.scrollable = false;
    SetOptions(m_options);
    m_logListener = Debug::Logger::LogEvent += [this](const Debug::LogData& data) { OnLogIntercepted(data); };
}

Console::~Console()
{
    Debug::Logger::LogEvent -= m_logListener;
}

void Console::_Draw_Impl()
{
    if (IsOpened() && !(ImGui::GetCurrentContext()->NextWindowData.Flags & ImGuiNextWindowDataFlags_HasSize))
    {
        const float fontSize = ImGui::GetFontSize();
        ImGui::SetNextWindowSize(ImVec2(fontSize * 60.0f, fontSize * 30.0f), ImGuiCond_FirstUseEver);
    }
    UI::PanelWindow::_Draw_Impl();
}

void Console::OnLogIntercepted(const Debug::LogData& data)
{
    std::lock_guard lock(m_pendingLogsMutex);
    if (m_pendingLogs.size() >= 10000)
    {
        m_pendingDroppedError |= m_pendingLogs.front().data.logLevel == Debug::ELogLevel::LOG_ERROR;
        m_pendingLogs.pop_front();
        ++m_pendingDropped;
    }
    m_pendingLogs.push_back({data, std::chrono::steady_clock::now()});
}

void Console::FlushPendingLogs()
{
    std::deque<PendingLog> pending;
    uint64_t dropped = 0;
    bool droppedError = false;
    {
        std::lock_guard lock(m_pendingLogsMutex);
        pending.swap(m_pendingLogs);
        dropped = std::exchange(m_pendingDropped, 0);
        droppedError = std::exchange(m_pendingDroppedError, false);
    }
    m_model.RecordDropped(dropped);
    if (droppedError && m_options.errorPause) m_errorPauseRequested = true;
    for (const auto& log : pending) AddLog(log.data, log.received);
}

void Console::UpdateStatus(Debug::ELogLevel level, const std::string& message,
    std::chrono::steady_clock::time_point received)
{
    if (m_options.errorPause && level == Debug::ELogLevel::LOG_ERROR) m_errorPauseRequested = true;
    const auto sequence = ++m_nextConsoleSequence;
    if (received >= m_lastStatusReceipt)
    {
        m_lastStatusReceipt = received;
        m_statusSnapshot = {true, level, message, sequence};
    }
}

void Console::AddLog(const Debug::LogData& data, std::chrono::steady_clock::time_point received)
{
    RequestGlyphs(data.message);
    RequestGlyphs(data.sourceFile);
    RequestGlyphs(data.scriptStack);
    UpdateStatus(data.logLevel, data.message, received);
    ConsoleLogEntry entry;
    entry.received = received;
    entry.level = data.logLevel;
    entry.source = ToSource(data.source);
    entry.timestamp = ClockText(data.date);
    entry.message = data.message;
    entry.sourcePath = data.sourceFile;
    entry.line = data.sourceLine;
    entry.scriptStack = data.scriptStack;
    entry.stackTrace = data.scriptStack;
    if (data.nativeStack)
    {
        entry.nativeFrames = data.nativeStack->frames;
        if (!entry.stackTrace.empty()) entry.stackTrace += '\n';
        entry.stackTrace += Debug::StackTrace::RawText(entry.nativeFrames);
    }
    const auto id = m_model.Add(std::move(entry));
    if (data.nativeStack) m_pendingNativeResolutions.push_back({id, data.nativeStack});
}

void Console::UpdateLogState(const Scripting::ScriptErrorConsole* scriptConsole)
{
    FlushPendingLogs();
    if (!scriptConsole) scriptConsole = m_scriptConsole;
    if (scriptConsole) FlushScriptDiagnostics(*scriptConsole);
    std::erase_if(m_pendingNativeResolutions, [this](const PendingNativeResolution& pending)
    {
        if (!pending.trace->ready.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(pending.trace->mutex);
        RequestGlyphs(pending.trace->resolved);
        m_model.SetResolvedStack(pending.entryId, pending.trace->resolved);
        return true;
    });
    if (m_selectedId && !m_model.Find(m_selectedId)) m_selectedId = 0;
}

void Console::SetScriptConsole(Scripting::ScriptErrorConsole* scriptConsole,
    std::function<void(const Scripting::ScriptError&)> openDiagnostic)
{
    if (m_scriptConsole != scriptConsole) m_forwardedScriptSequence = 0;
    m_scriptConsole = scriptConsole;
    m_openDiagnostic = std::move(openDiagnostic);
}

void Console::FlushScriptDiagnostics(const Scripting::ScriptErrorConsole& scriptConsole)
{
    const auto& errors = scriptConsole.GetErrors();
    const auto next = scriptConsole.GetNextSequence();
    const auto first = next - errors.size();
    auto sequence = std::max(m_forwardedScriptSequence, first);
    for (; sequence < next; ++sequence)
        AddScriptDiagnostic(errors[static_cast<size_t>(sequence - first)], scriptConsole.GetReceiptTime(sequence));
    m_forwardedScriptSequence = next;
}

void Console::AddScriptDiagnostic(const Scripting::ScriptError& error)
{
    AddScriptDiagnostic(error, std::chrono::steady_clock::now());
}

void Console::AddScriptDiagnostic(const Scripting::ScriptError& error, std::chrono::steady_clock::time_point received)
{
    const auto message = ScriptMessage(error);
    RequestGlyphs(message);
    RequestGlyphs(error.stackTrace);
    const auto level = ToLogLevel(error.severity);
    UpdateStatus(level, "[Script] " + message, received);
    ConsoleLogEntry entry;
    entry.received = received;
    entry.level = level;
    entry.source = ToSource(error.language);
    entry.timestamp = error.logDate.empty() ? CurrentClock() : ClockText(error.logDate);
    entry.message = "[Script] " + message;
    entry.stackTrace = entry.scriptStack = error.stackTrace;
    if (error.nativeStack)
    {
        entry.nativeFrames = error.nativeStack->frames;
        if (!entry.stackTrace.empty()) entry.stackTrace += '\n';
        entry.stackTrace += Debug::StackTrace::RawText(entry.nativeFrames);
    }
    entry.sourcePath = error.sourcePath;
    entry.line = error.line;
    entry.column = error.column;
    entry.diagnostic = error;
    const auto id = m_model.Add(std::move(entry));
    if (error.nativeStack) m_pendingNativeResolutions.push_back({id, error.nativeStack});
}

void Console::SetOptions(const ConsoleOptions& options)
{
    m_options = options;
    m_options.lineCount = std::clamp(m_options.lineCount, 1, 10);
    m_options.detailRatio = std::clamp(m_options.detailRatio, .15f, .75f);
    m_options.messageStack = std::clamp(m_options.messageStack, 0, 2);
    m_options.warningStack = std::clamp(m_options.warningStack, 0, 2);
    m_options.errorStack = std::clamp(m_options.errorStack, 0, 2);
    if (!m_options.errorPause) m_errorPauseRequested = false;
    ApplyStackOptions(m_options);
    FilterLogs();
}

void Console::OptionsChanged()
{
    ApplyStackOptions(m_options);
    FilterLogs();
    if (m_optionsChanged) m_optionsChanged(m_options);
}

void Console::ClearOnPlay() { if (m_options.clearOnPlay) Clear(); }
void Console::ClearOnBuild() { if (m_options.clearOnBuild) Clear(); }
void Console::ClearOnRecompile() { if (m_options.clearOnRecompile) Clear(); }

void Console::Clear()
{
    {
        std::lock_guard lock(m_pendingLogsMutex);
        m_pendingLogs.clear();
        m_pendingDropped = 0;
        m_pendingDroppedError = false;
    }
    m_model.Clear();
    m_pendingNativeResolutions.clear();
    if (m_scriptConsole)
    {
        m_scriptConsole->Clear();
        m_forwardedScriptSequence = m_scriptConsole->GetNextSequence();
    }
    if (m_selectedId && !m_model.Find(m_selectedId)) m_selectedId = 0;
    m_errorPauseRequested = false;
    m_statusSnapshot = {};
    m_lastStatusReceipt = {};
    m_followBottom = true;
    m_haveListScroll = false;
    m_rowsDirty = true;
}

void Console::ReplaceCompilerDiagnostics(std::vector<ConsoleLogEntry> diagnostics)
{
    for (auto& diagnostic : diagnostics)
    {
        RequestGlyphs(diagnostic.message);
        RequestGlyphs(diagnostic.sourcePath);
        RequestGlyphs(diagnostic.stackTrace);
        if (diagnostic.timestamp.empty()) diagnostic.timestamp = CurrentClock();
    }
    m_model.ReplaceCompilerDiagnostics(std::move(diagnostics));
    if (m_selectedId && !m_model.Find(m_selectedId)) m_selectedId = 0;
    m_rowsDirty = true;
}

void Console::FilterLogs()
{
    m_filter.messages = m_options.messages;
    m_filter.warnings = m_options.warnings;
    m_filter.errors = m_options.errors;
    m_filter.csharp = m_options.csharp;
    m_filter.lua = m_options.lua;
    m_filter.build = m_options.build;
    m_filter.collapse = m_options.collapse;
    m_filter.search = m_search.data();
    m_rowsDirty = true;
}

bool Console::IsAllowedByFilter(Debug::ELogLevel level)
{
    switch (level)
    {
    case Debug::ELogLevel::LOG_DEFAULT:
    case Debug::ELogLevel::LOG_INFO: return m_options.messages;
    case Debug::ELogLevel::LOG_WARNING: return m_options.warnings;
    case Debug::ELogLevel::LOG_ERROR: return m_options.errors;
    }
    return false;
}

void Console::RefreshRows()
{
    if (!m_rowsDirty && m_rowsRevision == m_model.Revision()) return;
    m_rows = m_model.BuildRows(m_filter, m_selectedId);
    m_rowsRevision = m_model.Revision();
    m_rowsDirty = false;
}

void Console::DrawStackMenu()
{
    const auto drawMode = [this](const char* label, int& mode)
    {
        if (!ImGui::BeginMenu(label)) return;
        const char* names[] = {"None", "Script Only",
#if defined(_WIN32)
            "Full"
#else
            "Full (native unavailable)"
#endif
        };
        for (int i = 0; i < 3; ++i)
            if (ImGui::MenuItem(names[i], nullptr, mode == i)) { mode = i; OptionsChanged(); }
        ImGui::EndMenu();
    };
    if (!ImGui::BeginMenu("Stack Trace Logging")) return;
    if (ImGui::BeginMenu("All"))
    {
        const char* names[] = {"None", "Script Only",
#if defined(_WIN32)
            "Full"
#else
            "Full (native unavailable)"
#endif
        };
        for (int i = 0; i < 3; ++i)
            if (ImGui::MenuItem(names[i], nullptr, m_options.messageStack == i &&
                m_options.warningStack == i && m_options.errorStack == i))
            {
                m_options.messageStack = m_options.warningStack = m_options.errorStack = i;
                OptionsChanged();
            }
        ImGui::EndMenu();
    }
    drawMode("Messages", m_options.messageStack);
    drawMode("Warnings", m_options.warningStack);
    drawMode("Errors", m_options.errorStack);
    ImGui::EndMenu();
}

void Console::DrawOptionsMenu()
{
    const auto counts = m_model.Counts();
    const auto messageLabel = "Messages (" + std::to_string(counts.messages) + ")";
    const auto warningLabel = "Warnings (" + std::to_string(counts.warnings) + ")";
    const auto errorLabel = "Errors (" + std::to_string(counts.errors) + ")";
    if (ImGui::MenuItem(messageLabel.c_str(), nullptr, &m_options.messages)) OptionsChanged();
    if (ImGui::MenuItem(warningLabel.c_str(), nullptr, &m_options.warnings)) OptionsChanged();
    if (ImGui::MenuItem(errorLabel.c_str(), nullptr, &m_options.errors)) OptionsChanged();
    ImGui::Separator();
    if (ImGui::MenuItem("Collapse", nullptr, &m_options.collapse)) OptionsChanged();
    if (ImGui::MenuItem("Error Pause", nullptr, &m_options.errorPause)) { m_errorPauseRequested = false; OptionsChanged(); }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear on Play", nullptr, &m_options.clearOnPlay)) OptionsChanged();
    if (ImGui::MenuItem("Clear on Build", nullptr, &m_options.clearOnBuild)) OptionsChanged();
    if (ImGui::MenuItem("Clear on Recompile", nullptr, &m_options.clearOnRecompile)) OptionsChanged();
    ImGui::Separator();
    if (ImGui::MenuItem("Show Timestamps", nullptr, &m_options.showTimestamp)) OptionsChanged();
    if (ImGui::BeginMenu("Log Entry Lines"))
    {
        for (int i = 1; i <= 10; ++i)
        {
            const auto label = std::to_string(i) + (i == 1 ? " Line" : " Lines");
            if (ImGui::MenuItem(label.c_str(), nullptr, m_options.lineCount == i))
            {
                m_options.lineCount = i;
                OptionsChanged();
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Sources"))
    {
        if (ImGui::MenuItem("C#", nullptr, &m_options.csharp)) OptionsChanged();
        if (ImGui::MenuItem("Lua", nullptr, &m_options.lua)) OptionsChanged();
        if (ImGui::MenuItem("Build", nullptr, &m_options.build)) OptionsChanged();
        ImGui::EndMenu();
    }
    DrawStackMenu();
    ImGui::Separator();
    if (ImGui::MenuItem("Open Editor Log", nullptr, false, static_cast<bool>(m_openLogFile)) && m_openLogFile) m_openLogFile();
}

void Console::DrawToolbar()
{
    const float width = ImGui::GetContentRegionAvail().x;
    const auto& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float frame = ImGui::GetFrameHeight();
    const auto buttonWidth = [&](const char* label) { return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f; };
    const auto counts = m_model.Counts();
    const auto messages = std::to_string(counts.messages);
    const auto warnings = std::to_string(counts.warnings);
    const auto errors = std::to_string(counts.errors);
    const float countWidth = SeverityButtonWidth(messages) + SeverityButtonWidth(warnings) + SeverityButtonWidth(errors);
    const float menuWidth = buttonWidth("...");
    const float searchMinimum = ImGui::GetFontSize() * 4.0f;
    const float baseWidth = buttonWidth("Clear") + 1.0f + frame + spacing + searchMinimum + spacing + menuWidth;
    const bool showCounts = width >= baseWidth + countWidth + spacing * 3.0f;
    const float neededWithCounts = baseWidth + (showCounts ? countWidth + spacing * 3.0f : 0.0f);
    const float extraWidth = buttonWidth("Collapse") + buttonWidth("Error Pause") + spacing * 2.0f;
    const bool showActions = width >= neededWithCounts + extraWidth;
    const bool showEditor = showActions && width >= neededWithCounts + extraWidth + ImGui::CalcTextSize("Editor").x + spacing;
    if (ImGui::Button("Clear")) Clear();
    ImGui::SameLine(0, 1);
    if (ImGui::ArrowButton("##clear-options", ImGuiDir_Down)) ImGui::OpenPopup("##clear-options-popup");
    if (ImGui::BeginPopup("##clear-options-popup"))
    {
        if (ImGui::MenuItem("Clear on Play", nullptr, &m_options.clearOnPlay)) OptionsChanged();
        if (ImGui::MenuItem("Clear on Build", nullptr, &m_options.clearOnBuild)) OptionsChanged();
        if (ImGui::MenuItem("Clear on Recompile", nullptr, &m_options.clearOnRecompile)) OptionsChanged();
        ImGui::EndPopup();
    }
    if (showActions)
    {
        ImGui::SameLine();
        if (ToggleButton("Collapse", m_options.collapse)) OptionsChanged();
        ImGui::SameLine();
        if (ToggleButton("Error Pause", m_options.errorPause)) { m_errorPauseRequested = false; OptionsChanged(); }
        if (showEditor) { ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Editor"); }
    }
    ImGui::SameLine();
    const float reserve = menuWidth + spacing + (showCounts ? countWidth + spacing * 3.0f : 0.0f);
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - reserve));
    if (ImGui::InputTextWithHint("##console-search", "Search", m_search.data(), m_search.size())) FilterLogs();
    if (showCounts)
    {
        ImGui::SameLine();
        if (SeverityButton("###console-messages", "Messages", Debug::ELogLevel::LOG_INFO, messages, m_options.messages)) OptionsChanged();
        ImGui::SameLine();
        if (SeverityButton("###console-warnings", "Warnings", Debug::ELogLevel::LOG_WARNING, warnings, m_options.warnings)) OptionsChanged();
        ImGui::SameLine();
        if (SeverityButton("###console-errors", "Errors", Debug::ELogLevel::LOG_ERROR, errors, m_options.errors)) OptionsChanged();
    }
    ImGui::SameLine();
    if (ImGui::Button("...##console-menu")) ImGui::OpenPopup("##console-menu-popup");
    if (ImGui::BeginPopup("##console-menu-popup")) { DrawOptionsMenu(); ImGui::EndPopup(); }
    if (m_model.DroppedCount() > 0)
        ImGui::TextDisabled("%llu older messages discarded", static_cast<unsigned long long>(m_model.DroppedCount()));
    ImGui::Separator();
}

void Console::DrawList(float height)
{
    ImGui::BeginChild("##console-list", ImVec2(0, height), true, ImGuiWindowFlags_HorizontalScrollbar);
    RefreshRows();
    const float scrollY = ImGui::GetScrollY();
    const bool atBottom = scrollY >= ImGui::GetScrollMaxY() - 1.0f;
    if ((ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0) ||
        (m_haveListScroll && scrollY < m_lastListScrollY - 1.0f))
        m_followBottom = false;
    else if (atBottom)
        m_followBottom = true;

    if (m_rows.empty())
        ImGui::TextDisabled(m_model.Entries().empty() ? "No messages." : "No matching messages.");

    const float textHeight = ImGui::GetTextLineHeight();
    const float rowHeight = textHeight * static_cast<float>(m_options.lineCount) + ImGui::GetStyle().FramePadding.y * 2.0f;
    const float rowStride = rowHeight + ImGui::GetStyle().ItemSpacing.y;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_rows.size()), rowStride);
    while (clipper.Step())
    {
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
        {
            const auto& row = m_rows[static_cast<size_t>(index)];
            const auto& entry = *row.entry;
            const auto id = std::to_string(entry.id);
            ImGui::PushID(id.c_str());
            const auto start = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", row.containsSelection, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight)))
            {
                m_selectedId = entry.id;
                m_rowsDirty = true;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenEntry(entry);
            }
            const auto end = ImGui::GetItemRectMax();
            if (ImGui::BeginPopupContextItem("##row-context"))
            {
                if (ImGui::MenuItem("Copy Message")) ImGui::SetClipboardText(entry.message.c_str());
                if (ImGui::MenuItem("Copy Full Record")) { const auto text = FullText(entry); ImGui::SetClipboardText(text.c_str()); }
                if (ImGui::MenuItem("Open Source", nullptr, false, !entry.sourcePath.empty())) OpenEntry(entry);
                ImGui::EndPopup();
            }
            auto* draw = ImGui::GetWindowDrawList();
            draw->PushClipRect(start, end, true);
            const float iconSpace = textHeight * 2.1f;
            DrawSeverityIcon(draw, ImVec2(start.x + iconSpace * .5f, start.y + rowHeight * .5f),
                entry.level, std::max(3.0f, std::min(rowHeight * .38f, textHeight * .75f)));
            const auto text = EntryText(entry, m_options.showTimestamp);
            const auto count = row.count > 1 ? std::to_string(row.count) : std::string{};
            const float badgeWidth = count.empty() ? 0.0f : ImGui::CalcTextSize(count.c_str()).x + 12.0f;
            DrawWrappedRowText(draw, ImVec2(start.x + iconSpace, start.y + ImGui::GetStyle().FramePadding.y),
                end.x - start.x - iconSpace - badgeWidth - 6.0f, m_options.lineCount, text, m_filter.search);
            if (!count.empty())
            {
                const ImVec2 badgeMin(end.x - badgeWidth - 4, start.y + ImGui::GetStyle().FramePadding.y);
                const ImVec2 badgeMax(end.x - 4, badgeMin.y + textHeight);
                draw->AddRectFilled(badgeMin, badgeMax, ImGui::GetColorU32(ImGuiCol_FrameBgActive), textHeight * .45f);
                draw->AddText(ImVec2(badgeMin.x + 6, badgeMin.y), ImGui::GetColorU32(ImGuiCol_Text), count.c_str());
            }
            draw->PopClipRect();
            ImGui::PopID();
        }
    }
    clipper.End();

    if (m_scrollToSelection)
    {
        for (size_t i = 0; i < m_rows.size(); ++i) if (m_rows[i].containsSelection)
            ImGui::SetScrollY(std::max(0.0f, i * rowStride - ImGui::GetWindowHeight() * .5f));
        m_scrollToSelection = false;
    }
    if (m_followBottom && (!atBottom || m_lastDrawnRevision != m_model.Revision()))
        ImGui::SetScrollY(ImGui::GetCursorPosY());
    m_lastDrawnRevision = m_model.Revision();
    m_lastListScrollY = scrollY;
    m_haveListScroll = true;
    ImGui::EndChild();
}

bool Console::ExecuteCommand(const std::string& id)
{
    if (id == "end") { m_followBottom = true; return true; }
    if (id == "previous" || id == "next")
    {
        if (m_rows.empty()) return false;
        int selected = -1;
        for (size_t i = 0; i < m_rows.size(); ++i) if (m_rows[i].containsSelection) { selected = static_cast<int>(i); break; }
        selected = std::clamp(selected < 0 ? 0 : selected + (id == "previous" ? -1 : 1), 0, static_cast<int>(m_rows.size()) - 1);
        m_selectedId = m_rows[static_cast<size_t>(selected)].entry->id;
        m_rowsDirty = true; m_scrollToSelection = true; m_followBottom = false;
        return true;
    }
    const auto* entry = m_model.Find(m_selectedId);
    if (!entry) return false;
    if (id == "open") { OpenEntry(*entry); return true; }
    if (id == "copy") { const auto text = FullText(*entry); ImGui::SetClipboardText(text.c_str()); return true; }
    return false;
}

void Console::OpenSource(const std::string& path, int line, int column)
{
    if (!m_openDiagnostic || !CanOpenSource(path)) return;
    Scripting::ScriptError error;
    error.sourcePath = path;
    error.line = line;
    error.column = column;
    m_openDiagnostic(error);
}

bool Console::CanOpenSource(const std::string& path) const
{
    return !path.empty() && (!m_sourceAvailable || m_sourceAvailable(path));
}

void Console::OpenEntry(const ConsoleLogEntry& entry)
{
    if (entry.diagnostic && m_openDiagnostic && CanOpenSource(entry.diagnostic->sourcePath))
    {
        m_openDiagnostic(*entry.diagnostic);
        return;
    }
    if (CanOpenSource(entry.sourcePath)) { OpenSource(entry.sourcePath, entry.line, entry.column); return; }
    size_t start = 0;
    while (start < entry.stackTrace.size())
    {
        const auto end = entry.stackTrace.find('\n', start);
        const auto location = ParseConsoleStackLocation(entry.stackTrace.substr(start, end - start));
        if (CanOpenSource(location.path)) { OpenSource(location.path, location.line, 0); return; }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void Console::DrawDetails(float height)
{
    ImGui::BeginChild("##console-details", ImVec2(0, height), true, ImGuiWindowFlags_HorizontalScrollbar);
    const auto* entry = m_model.Find(m_selectedId);
    if (!entry) ImGui::TextDisabled("Select a message to see details.");
    else
    {
        ImGui::TextDisabled("%s%s%s", SourceName(entry->source), entry->timestamp.empty() ? "" : "  ",
            entry->timestamp.c_str());
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(entry->message.c_str());
        ImGui::PopTextWrapPos();
        if (!entry->sourcePath.empty())
        {
            std::string location = entry->sourcePath;
            if (entry->line > 0) location += ":" + std::to_string(entry->line);
            if (entry->column > 0) location += ":" + std::to_string(entry->column);
            if (ImGui::SmallButton(location.c_str())) OpenEntry(*entry);
        }
        if (!entry->stackTrace.empty())
        {
            ImGui::Separator();
            size_t start = 0;
            int lineIndex = 0;
            while (start < entry->stackTrace.size())
            {
                const auto end = entry->stackTrace.find('\n', start);
                const auto line = entry->stackTrace.substr(start, end - start);
                const auto location = ParseConsoleStackLocation(line);
                ImGui::PushID(lineIndex++);
                if (!location.path.empty())
                {
                    if (ImGui::Selectable(line.c_str(), false)) OpenSource(location.path, location.line, 0);
                }
                else ImGui::TextUnformatted(line.c_str());
                ImGui::PopID();
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        if (ImGui::BeginPopupContextWindow("##details-context", ImGuiPopupFlags_MouseButtonRight))
        {
            if (ImGui::MenuItem("Copy Message")) ImGui::SetClipboardText(entry->message.c_str());
            if (ImGui::MenuItem("Copy Full Record")) { const auto text = FullText(*entry); ImGui::SetClipboardText(text.c_str()); }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
}

void Console::OnBeforeDrawWidgets()
{
    UpdateLogState();
    DrawToolbar();
    const float available = std::max(0.0f, ImGui::GetContentRegionAvail().y);
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float divider = 5.0f * scale;
    const float minimum = std::min(50.0f * scale, available * .3f);
    const float detailHeight = std::clamp(available * m_options.detailRatio, minimum, std::max(minimum, available - minimum - divider));
    DrawList(std::max(1.0f, available - detailHeight - divider));
    const auto splitter = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##console-splitter", ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), divider));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive() && available > 1.0f)
    {
        m_options.detailRatio = std::clamp(m_options.detailRatio - ImGui::GetIO().MouseDelta.y / available, .15f, .75f);
    }
    if (ImGui::IsItemDeactivated()) OptionsChanged();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(splitter.x, splitter.y + divider * .5f),
        ImVec2(splitter.x + ImGui::GetContentRegionAvail().x, splitter.y + divider * .5f), ImGui::GetColorU32(ImGuiCol_Separator));
    DrawDetails(std::max(1.0f, detailHeight));
}
