#include "ScriptDebug.h"

#include "LuaScriptBackend.h"

#include <utility>

namespace NLS::Scripting
{
namespace
{
ScriptStatus ApplyBackendSettings(ScriptRuntime* runtime, const ScriptDebugSettings& settings)
{
    if (!runtime)
        return ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Script debug service has no runtime.");
    if (!settings.enableLuaPanda)
    {
        if (auto* lua = dynamic_cast<LuaScriptBackend*>(runtime->GetBackend(ScriptLanguage::Lua)))
            return lua->SetLuaPandaDebugging(false, settings.luaPandaHost, settings.luaPandaPort);
        return ScriptStatus::Ok();
    }

    auto* lua = dynamic_cast<LuaScriptBackend*>(runtime->GetBackend(ScriptLanguage::Lua));
    if (!lua)
        return ScriptStatus::Error(ScriptStatusCode::BackendUnavailable, "LuaPanda debugging requires an initialized Lua backend.");
    return lua->SetLuaPandaDebugging(
            settings.enableLuaPanda,
            settings.luaPandaHost,
            settings.luaPandaPort);
}
}

bool ScriptDebugSettings::IsValid() const
{
    return luaPandaHost == "127.0.0.1"
        && luaPandaPort != 0;
}

ScriptDebugService::ScriptDebugService(ScriptRuntime& runtime, ScriptDebugSettings settings)
    : m_runtime(&runtime)
    , m_settings(std::move(settings))
{
    Attach();
}

ScriptDebugService::~ScriptDebugService()
{
    Detach();
}

bool ScriptDebugService::Attach()
{
    if (!m_runtime)
    {
        m_lastStatus = ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Script debug service has no runtime.");
        return false;
    }
    if (!m_settings.IsValid())
    {
        m_lastStatus = ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Script debug settings are invalid; LuaPanda must use 127.0.0.1 and a non-zero port.");
        return false;
    }
    if (!m_attached)
    {
        m_console.Attach(*m_runtime);
        m_attached = true;
    }
    m_lastStatus = ApplySettings();
    if (!m_lastStatus.Succeeded())
        PushSettingsDiagnostic(m_lastStatus);
    return m_lastStatus.Succeeded();
}

void ScriptDebugService::Detach()
{
    if (!m_attached || !m_runtime)
        return;
    ScriptDebugSettings disabled = m_settings;
    disabled.enableLuaPanda = false;
    (void)ApplyBackendSettings(m_runtime, disabled);
    // ScriptRuntime's sink is intentionally replaceable. Detaching leaves
    // runtime error collection intact and stops forwarding to this context's
    // console before the service is destroyed.
    m_runtime->SetErrorSink({});
    m_attached = false;
}

ScriptStatus ScriptDebugService::SetSettings(ScriptDebugSettings settings)
{
    m_settings = std::move(settings);
    if (!m_settings.IsValid())
    {
        m_lastStatus = ScriptStatus::Error(ScriptStatusCode::InvalidArgument, "Script debug settings are invalid; LuaPanda must use 127.0.0.1 and a non-zero port.");
        Detach();
    }
    else if (!m_attached)
    {
        if (!Attach())
            return m_lastStatus;
    }
    else
    {
        m_lastStatus = ApplySettings();
        if (!m_lastStatus.Succeeded())
            PushSettingsDiagnostic(m_lastStatus);
    }
    return m_lastStatus;
}

ScriptStatus ScriptDebugService::ApplySettings()
{
    return ApplyBackendSettings(m_runtime, m_settings);
}

void ScriptDebugService::PushSettingsDiagnostic(const ScriptStatus& status)
{
    if (status.Succeeded())
        return;
    ScriptError error;
    error.language = m_settings.enableLuaPanda ? ScriptLanguage::Lua : ScriptLanguage::Unknown;
    error.severity = ScriptErrorSeverity::Error;
    error.message = status.message;
    m_console.Push(error);
}

bool ScriptDebugService::OpenDiagnostic(const ScriptError& error) const
{
    if (!m_diagnosticOpener || error.sourcePath.empty())
        return false;
    return m_diagnosticOpener(error);
}
}
