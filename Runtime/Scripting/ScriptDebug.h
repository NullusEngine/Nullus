#pragma once

#include "ScriptErrorConsole.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace NLS::Scripting
{
struct NLS_SCRIPTING_API ScriptDebugSettings
{
    bool enableCSharpDebugging = false;
    bool enableLuaPanda = false;
    std::string luaPandaHost = "127.0.0.1";
    uint16_t luaPandaPort = 8818;
    bool stopOnEntry = false;

    // LuaPanda is intentionally local-only in the first debugger release.
    bool IsValid() const;
};

// Context-owned bridge used by Editor Play. It owns only the bounded
// structured-error console; debugger protocols remain backend-owned and are
// enabled explicitly through the settings passed to this service.
class NLS_SCRIPTING_API ScriptDebugService final
{
public:
    using DiagnosticOpener = std::function<bool(const ScriptError&)>;

    explicit ScriptDebugService(ScriptRuntime& runtime, ScriptDebugSettings settings = {});
    ~ScriptDebugService();

    ScriptDebugService(const ScriptDebugService&) = delete;
    ScriptDebugService& operator=(const ScriptDebugService&) = delete;

    bool Attach();
    void Detach();
    bool IsAttached() const { return m_attached; }
    bool IsValid() const { return m_settings.IsValid(); }

    const ScriptDebugSettings& GetSettings() const { return m_settings; }
    // Returns the backend status so the Editor can surface a failed debugger
    // start immediately. Existing callers may ignore the return value.
    ScriptStatus SetSettings(ScriptDebugSettings settings);
    const ScriptStatus& GetLastStatus() const { return m_lastStatus; }
    void SetDiagnosticOpener(DiagnosticOpener opener) { m_diagnosticOpener = std::move(opener); }
    bool OpenDiagnostic(const ScriptError& error) const;
    ScriptErrorConsole& GetConsole() { return m_console; }
    const ScriptErrorConsole& GetConsole() const { return m_console; }

private:
    ScriptStatus ApplySettings();
    void PushSettingsDiagnostic(const ScriptStatus& status);

    ScriptRuntime* m_runtime = nullptr;
    ScriptDebugSettings m_settings;
    ScriptErrorConsole m_console;
    DiagnosticOpener m_diagnosticOpener;
    ScriptStatus m_lastStatus;
    bool m_attached = false;
};
}
