#pragma once

#include "Panels/ConsoleLogModel.h"

#include <Debug/Logger.h>
#include <UI/Panels/PanelWindow.h>

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace NLS::Scripting { class ScriptErrorConsole; }

namespace NLS::Editor::Panels
{
struct ConsoleStatusSnapshot
{
    bool hasMessage = false;
    ::NLS::Debug::ELogLevel level = ::NLS::Debug::ELogLevel::LOG_DEFAULT;
    std::string message;
    uint64_t sequence = 0;
};

// Values are supplied by the Editor's user-settings registry. Keeping this
// independent of reflection also allows the headless console tests to use it.
struct ConsoleOptions
{
    bool clearOnPlay = true;
    bool clearOnBuild = false;
    bool clearOnRecompile = false;
    bool collapse = false;
    bool errorPause = false;
    bool showTimestamp = true;
    bool messages = true;
    bool warnings = true;
    bool errors = true;
    bool csharp = true;
    bool lua = true;
    bool build = true;
    int lineCount = 2;
    float detailRatio = 0.30f;
    // 0 = None, 1 = ScriptOnly, 2 = Full.
    int messageStack = 1;
    int warningStack = 1;
    int errorStack = 1;
};

class Console : public UI::PanelWindow
{
public:
    Console(const std::string& title, bool opened, const UI::PanelWindowSettings& windowSettings);
    ~Console();
    bool ExecuteCommand(const std::string& id);

    void OnLogIntercepted(const ::NLS::Debug::LogData& logData);
    void FlushPendingLogs();
    void UpdateLogState(const NLS::Scripting::ScriptErrorConsole* scriptConsole = nullptr);
    void SetScriptConsole(NLS::Scripting::ScriptErrorConsole* scriptConsole,
        std::function<void(const NLS::Scripting::ScriptError&)> openDiagnostic = {});
    void SetSourceAvailability(std::function<bool(const std::string&)> available) { m_sourceAvailable = std::move(available); }
    const ConsoleStatusSnapshot& GetStatusSnapshot() const { return m_statusSnapshot; }
    bool ConsumeErrorPauseRequest() { const bool result = m_errorPauseRequested; m_errorPauseRequested = false; return result; }

    void SetOptions(const ConsoleOptions& options);
    const ConsoleOptions& GetOptions() const { return m_options; }
    void SetOptionsChanged(std::function<void(const ConsoleOptions&)> changed) { m_optionsChanged = std::move(changed); }
    void SetOpenLogFile(std::function<void()> open) { m_openLogFile = std::move(open); }
    void ClearOnPlay();
    void ClearOnBuild();
    void ClearOnRecompile();
    void Clear();
    void ReplaceCompilerDiagnostics(std::vector<ConsoleLogEntry> diagnostics);
    void FilterLogs();
    bool IsAllowedByFilter(::NLS::Debug::ELogLevel level);
    void AddScriptDiagnostic(const NLS::Scripting::ScriptError& error);
    const ConsoleLogModel& GetLogModel() const { return m_model; }
    const ConsoleLogFilter& GetFilter() const { return m_filter; }

private:
    bool m_scrollToSelection = false;
    struct PendingLog
    {
        ::NLS::Debug::LogData data;
        std::chrono::steady_clock::time_point received;
    };
    struct PendingNativeResolution
    {
        uint64_t entryId = 0;
        std::shared_ptr<::NLS::Debug::NativeStackTrace> trace;
    };

    void AddLog(const ::NLS::Debug::LogData& data, std::chrono::steady_clock::time_point received);
    void AddScriptDiagnostic(const NLS::Scripting::ScriptError& error, std::chrono::steady_clock::time_point received);
    void FlushScriptDiagnostics(const NLS::Scripting::ScriptErrorConsole& scriptConsole);
    void UpdateStatus(::NLS::Debug::ELogLevel level, const std::string& message, std::chrono::steady_clock::time_point received);
    void DrawToolbar();
    void DrawList(float height);
    void DrawDetails(float height);
    void DrawOptionsMenu();
    void DrawStackMenu();
    void OpenEntry(const ConsoleLogEntry& entry);
    void OpenSource(const std::string& path, int line, int column);
    bool CanOpenSource(const std::string& path) const;
    void OptionsChanged();
    void RefreshRows();
    void _Draw_Impl() override;
    void OnBeforeDrawWidgets() override;

    ConsoleLogModel m_model;
    ConsoleOptions m_options;
    ConsoleLogFilter m_filter;
    std::vector<ConsoleLogRow> m_rows;
    uint64_t m_rowsRevision = ~uint64_t{0};
    bool m_rowsDirty = true;
    uint64_t m_selectedId = 0;
    uint64_t m_lastDrawnRevision = 0;
    bool m_followBottom = true;
    bool m_haveListScroll = false;
    float m_lastListScrollY = 0.0f;
    std::array<char, 512> m_search{};

    std::mutex m_pendingLogsMutex;
    std::deque<PendingLog> m_pendingLogs;
    std::vector<PendingNativeResolution> m_pendingNativeResolutions;
    uint64_t m_pendingDropped = 0;
    bool m_pendingDroppedError = false;
    bool m_errorPauseRequested = false;
    uint64_t m_forwardedScriptSequence = 0;
    uint64_t m_nextConsoleSequence = 0;
    ConsoleStatusSnapshot m_statusSnapshot;
    std::chrono::steady_clock::time_point m_lastStatusReceipt;
    NLS::Scripting::ScriptErrorConsole* m_scriptConsole = nullptr;
    std::function<void(const NLS::Scripting::ScriptError&)> m_openDiagnostic;
    std::function<bool(const std::string&)> m_sourceAvailable;
    std::function<void(const ConsoleOptions&)> m_optionsChanged;
    std::function<void()> m_openLogFile;
    uint64_t m_logListener = 0;
};
}
