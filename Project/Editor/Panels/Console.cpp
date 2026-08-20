#include <algorithm>

#include <utility>

#include "Panels/Console.h"
#include "Core/EditorActions.h"
#include "Core/Context.h"


#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Widgets/Texts/TextClickable.h>
using namespace NLS;
using namespace NLS::UI;

namespace
{
std::string BuildScriptDiagnosticText(const NLS::Scripting::ScriptError& error)
{
    const auto language = NLS::Scripting::ToString(error.language);
    std::string location = error.sourcePath;
    if (error.line > 0)
        location += ":" + std::to_string(error.line);
    if (error.column > 0)
        location += ":" + std::to_string(error.column);
    return "[" + std::string(language) + "] " +
        (location.empty() ? error.message : location + " - " + error.message);
}
}

std::pair<Maths::Color, std::string> GetWidgetSettingsFromLogData(const Debug::LogData& p_logData)
{
    Maths::Color logColor;
	std::string logHeader;
	std::string logDateFormated = "[";
	bool isSecondPart = false;
	std::for_each(p_logData.date.begin(), p_logData.date.end(), [&logDateFormated, &isSecondPart](char c)
	{ 
		if (isSecondPart)
			logDateFormated.push_back(c == '-' ? ':' : c);

		if (c == '_')
			isSecondPart = true;
	});

	logDateFormated += "] ";

	switch (p_logData.logLevel)
	{
	default:
	case Debug::ELogLevel::LOG_DEFAULT:	return { { 1.f, 1.f, 1.f, 1.f }, logDateFormated };
	case Debug::ELogLevel::LOG_INFO:		return { { 0.f, 1.f, 1.f, 1.f }, logDateFormated };
	case Debug::ELogLevel::LOG_WARNING:	return { { 1.f, 1.f, 0.f, 1.f }, logDateFormated };
	case Debug::ELogLevel::LOG_ERROR:		return { { 1.f, 0.f, 0.f, 1.f }, logDateFormated };
	}
}

Editor::Panels::Console::Console
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) :
	PanelWindow(p_title, p_opened, p_windowSettings)
{
	panelSettings.allowHorizontalScrollbar = true;

	auto& clearButton = CreateWidget<Widgets::Button>("Clear");
	clearButton.size = { 50.f, 0.f };
	clearButton.idleBackgroundColor = { 0.5f, 0.f, 0.f };
	clearButton.ClickedEvent += std::bind(&Console::Clear, this);
	clearButton.lineBreak = false;

	auto& clearOnPlay = CreateWidget<Widgets::CheckBox>(m_clearOnPlay, "Auto clear on play");

	CreateWidget<Widgets::Spacing>(5).lineBreak = false;

	auto& enableDefault = CreateWidget<Widgets::CheckBox>(true, "Default");
	auto& enableInfo = CreateWidget<Widgets::CheckBox>(true, "Info");
	auto& enableWarning = CreateWidget<Widgets::CheckBox>(true, "Warning");
	auto& enableError = CreateWidget<Widgets::CheckBox>(true, "Error");
	auto& enableCSharp = CreateWidget<Widgets::CheckBox>(true, "C#");
	auto& enableLua = CreateWidget<Widgets::CheckBox>(true, "Lua");
	auto& enableBuild = CreateWidget<Widgets::CheckBox>(true, "Build");

	clearOnPlay.lineBreak = false;
	enableDefault.lineBreak = false;
	enableInfo.lineBreak = false;
	enableWarning.lineBreak = false;
	enableError.lineBreak = true;
	enableCSharp.lineBreak = false;
	enableLua.lineBreak = false;
	enableBuild.lineBreak = true;

	clearOnPlay.ValueChangedEvent += [this](bool p_value) { m_clearOnPlay = p_value; };
	enableDefault.ValueChangedEvent += std::bind(&Console::SetShowDefaultLogs, this, std::placeholders::_1);
	enableInfo.ValueChangedEvent += std::bind(&Console::SetShowInfoLogs, this, std::placeholders::_1);
	enableWarning.ValueChangedEvent += std::bind(&Console::SetShowWarningLogs, this, std::placeholders::_1);
	enableError.ValueChangedEvent += std::bind(&Console::SetShowErrorLogs, this, std::placeholders::_1);
	enableCSharp.ValueChangedEvent += std::bind(&Console::SetShowCSharpLogs, this, std::placeholders::_1);
	enableLua.ValueChangedEvent += std::bind(&Console::SetShowLuaLogs, this, std::placeholders::_1);
	enableBuild.ValueChangedEvent += std::bind(&Console::SetShowBuildLogs, this, std::placeholders::_1);

	CreateWidget<Widgets::Separator>();

	m_logGroup = &CreateWidget<Widgets::Group>();
    m_logGroup->ReverseDrawOrder();

    if (NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        m_playListener = EDITOR_EVENT(PlayEvent) += std::bind(&Console::ClearOnPlay, this);

	m_logListener = Debug::Logger::LogEvent += std::bind(&Console::OnLogIntercepted, this, std::placeholders::_1);
}

Editor::Panels::Console::~Console()
{
    if (m_playListener != 0 && NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        EDITOR_EVENT(PlayEvent) -= m_playListener;
	Debug::Logger::LogEvent -= m_logListener;
}

void Editor::Panels::Console::OnLogIntercepted(const Debug::LogData & p_logData)
{
    std::lock_guard lock(m_pendingLogsMutex);
    m_pendingLogs.push_back(p_logData);
}

void Editor::Panels::Console::FlushPendingLogs()
{
    std::vector<Debug::LogData> pendingLogs;
    {
        std::lock_guard lock(m_pendingLogsMutex);
        pendingLogs.swap(m_pendingLogs);
    }

    for (const auto& logData : pendingLogs)
        AddLogWidget(logData);
}

void Editor::Panels::Console::AddLogWidget(const Debug::LogData& p_logData)
{
    auto [logColor, logDate] = GetWidgetSettingsFromLogData(p_logData);

    auto& consoleItem1 = m_logGroup->CreateWidget<Widgets::TextColored>(logDate + "\t" + p_logData.message, logColor);

    consoleItem1.enabled = IsAllowedByFilter(p_logData.logLevel);

    m_logTextWidgets[&consoleItem1] = {p_logData.logLevel, EntrySource::General};
}

void Editor::Panels::Console::OnBeforeDrawWidgets()
{
    FlushPendingLogs();
    FlushScriptDiagnostics();
}

void Editor::Panels::Console::FlushScriptDiagnostics()
{
    if (!NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        return;
    const auto* debugService = EDITOR_CONTEXT(scriptDebugService).get();
    if (debugService == nullptr)
        return;

    const auto& errors = debugService->GetConsole().GetErrors();
    if (m_forwardedScriptErrors > errors.size())
        m_forwardedScriptErrors = 0;
    while (m_forwardedScriptErrors < errors.size())
        AddScriptDiagnostic(errors[m_forwardedScriptErrors++]);
}

void Editor::Panels::Console::AddScriptDiagnostic(const NLS::Scripting::ScriptError& error)
{
    const std::string message = BuildScriptDiagnosticText(error);
    auto& widget = m_logGroup->CreateWidget<Widgets::TextClickable>("[Script] " + message);
    widget.selected = false;
    const auto source = error.language == NLS::Scripting::ScriptLanguage::CSharp
        ? EntrySource::CSharp
        : error.language == NLS::Scripting::ScriptLanguage::Lua
            ? EntrySource::Lua
            : EntrySource::Build;
    widget.enabled = IsAllowedBySource(source);
    m_scriptTextWidgets[&widget] = {
        Debug::ELogLevel::LOG_ERROR,
        source,
        error,
        "[Script] " + message,
        false};
    widget.ClickedEvent += [this, &widget]
    {
        const auto it = m_scriptTextWidgets.find(&widget);
        if (it == m_scriptTextWidgets.end())
            return;
        it->second.expanded = !it->second.expanded;
        widget.content = it->second.header;
        if (it->second.expanded && !it->second.error.stackTrace.empty())
            widget.content += "\n" + it->second.error.stackTrace;
    };
    widget.DoubleClickedEvent += [this, &widget]
    {
        const auto it = m_scriptTextWidgets.find(&widget);
        if (it == m_scriptTextWidgets.end())
            return;
        if (NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>() &&
            EDITOR_CONTEXT(scriptDebugService))
            EDITOR_CONTEXT(scriptDebugService)->OpenDiagnostic(it->second.error);
    };
}

void Editor::Panels::Console::ClearOnPlay()
{
	if (m_clearOnPlay)
		Clear();
}

void Editor::Panels::Console::Clear()
{
    {
        std::lock_guard lock(m_pendingLogsMutex);
        m_pendingLogs.clear();
    }
	m_logTextWidgets.clear();
	m_scriptTextWidgets.clear();
	m_forwardedScriptErrors = 0;
	if (NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>() && EDITOR_CONTEXT(scriptDebugService))
		EDITOR_CONTEXT(scriptDebugService)->GetConsole().Clear();
	m_logGroup->RemoveAllWidgets();
}

void Editor::Panels::Console::FilterLogs()
{
	for (const auto&[widget, entry] : m_logTextWidgets)
		widget->enabled = IsAllowedByFilter(entry.level) && IsAllowedBySource(entry.source);
	for (const auto&[widget, entry] : m_scriptTextWidgets)
		widget->enabled = IsAllowedByFilter(entry.level) && IsAllowedBySource(entry.source);
}

bool Editor::Panels::Console::IsAllowedByFilter(Debug::ELogLevel p_logLevel)
{
	switch (p_logLevel)
	{
	case Debug::ELogLevel::LOG_DEFAULT:	return m_showDefaultLog;
	case Debug::ELogLevel::LOG_INFO:		return m_showInfoLog;
	case Debug::ELogLevel::LOG_WARNING:	return m_showWarningLog;
	case Debug::ELogLevel::LOG_ERROR:		return m_showErrorLog;
	}

	return false;
}

void Editor::Panels::Console::SetShowDefaultLogs(bool p_value)
{
	m_showDefaultLog = p_value;
	FilterLogs();
}

void Editor::Panels::Console::SetShowInfoLogs(bool p_value)
{
	m_showInfoLog = p_value;
	FilterLogs();
}

void Editor::Panels::Console::SetShowWarningLogs(bool p_value)
{
	m_showWarningLog = p_value;
	FilterLogs();
}

void Editor::Panels::Console::SetShowErrorLogs(bool p_value)
{
	m_showErrorLog = p_value;
	FilterLogs();
}

void Editor::Panels::Console::SetShowCSharpLogs(bool p_value)
{
	m_showCSharpLog = p_value;
	FilterLogs();
}

void Editor::Panels::Console::SetShowLuaLogs(bool p_value)
{
	m_showLuaLog = p_value;
	FilterLogs();
}

void Editor::Panels::Console::SetShowBuildLogs(bool p_value)
{
	m_showBuildLog = p_value;
	FilterLogs();
}

bool Editor::Panels::Console::IsAllowedBySource(EntrySource source) const
{
	switch (source)
	{
	case EntrySource::CSharp: return m_showCSharpLog;
	case EntrySource::Lua: return m_showLuaLog;
	case EntrySource::Build: return m_showBuildLog;
	case EntrySource::General: return true;
	}
	return false;
}
