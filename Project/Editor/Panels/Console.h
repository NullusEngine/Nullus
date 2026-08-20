#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Debug/Logger.h>
#include <Scripting/ScriptTypes.h>

#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Texts/TextClickable.h>
#include <UI/Widgets/Texts/TextColored.h>

namespace NLS::Editor::Panels
{
	class Console : public UI::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		Console
		(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);
		~Console();

		/**
		* Method called when a log event occured
		* @param p_logData
		*/
		void OnLogIntercepted(const Debug::LogData& p_logData);

		/**
		* Flush pending log entries on the UI thread.
		*/
		void FlushPendingLogs();

		/**
		* Called when the scene plays. It will clear the console if the "Clear on play" settings is on
		*/
		void ClearOnPlay();

		/**
		* Clear the console
		*/
		void Clear();

		/**
		* Filter logs using defined filters
		*/
		void FilterLogs();

		/**
		* Verify if a given log level is allowed by the current filter
		* @param p_logLevel
		*/
		bool IsAllowedByFilter(Debug::ELogLevel p_logLevel);
		void AddScriptDiagnostic(const NLS::Scripting::ScriptError& error);

	private:
		enum class EntrySource
		{
			General,
			CSharp,
			Lua,
			Build
		};
		struct EntryInfo
		{
			Debug::ELogLevel level;
			EntrySource source;
		};
		struct ScriptEntryInfo
		{
			Debug::ELogLevel level;
			EntrySource source;
			NLS::Scripting::ScriptError error;
			std::string header;
			bool expanded = false;
		};
		void SetShowDefaultLogs(bool p_value);
		void SetShowInfoLogs(bool p_value);
		void SetShowWarningLogs(bool p_value);
		void SetShowErrorLogs(bool p_value);
		void SetShowCSharpLogs(bool p_value);
		void SetShowLuaLogs(bool p_value);
		void SetShowBuildLogs(bool p_value);
		void AddLogWidget(const Debug::LogData& p_logData);
		void FlushScriptDiagnostics();
		bool IsAllowedBySource(EntrySource source) const;
		void OnBeforeDrawWidgets() override;

	private:
		UI::Widgets::Group* m_logGroup;
		std::unordered_map<UI::Widgets::TextColored*, EntryInfo> m_logTextWidgets;
		std::unordered_map<UI::Widgets::TextClickable*, ScriptEntryInfo> m_scriptTextWidgets;
		std::mutex m_pendingLogsMutex;
		std::vector<Debug::LogData> m_pendingLogs;

		bool m_clearOnPlay = true;
		bool m_showDefaultLog = true;
		bool m_showInfoLog = true;
		bool m_showWarningLog = true;
		bool m_showErrorLog = true;
		bool m_showCSharpLog = true;
		bool m_showLuaLog = true;
		bool m_showBuildLog = true;
		size_t m_forwardedScriptErrors = 0;
		uint64_t m_playListener = 0;
		uint64_t m_logListener = 0;
	};
}
