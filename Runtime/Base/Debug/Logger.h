#pragma once

#include <string>
#include <map>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include "BaseDef.h"
#include <Eventing/Event.h>

#include "Debug/ILogHandler.h"
#include "Debug/ConsoleHandler.h"
#include "Debug/FileHandler.h"
#include "Debug/HistoryHandler.h"

//#define NLS_LOG(message)			NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::LOG_DEFAULT,	NLS::Debug::ELogMode::CONSOLE)
//#define NLS_LOG_INFO(message)		NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::LOG_INFO,		NLS::Debug::ELogMode::CONSOLE)
//#define NLS_LOG_WARNING(message)	NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::LOG_WARNING,	NLS::Debug::ELogMode::CONSOLE)
//#define NLS_LOG_ERROR(message)	NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::LOG_ERROR,	NLS::Debug::ELogMode::CONSOLE)

#define NLS_LOG(message) do { NLS::Debug::Logger::LogWithContext((message), NLS::Debug::ELogLevel::LOG_DEFAULT, NLS::Debug::ELogMode::CONSOLE_AND_FILE, __FILE__, __LINE__, __func__); } while (0)
#define NLS_LOG_INFO(message) do { NLS::Debug::Logger::LogWithContext((message), NLS::Debug::ELogLevel::LOG_INFO, NLS::Debug::ELogMode::CONSOLE_AND_FILE, __FILE__, __LINE__, __func__); } while (0)
#define NLS_LOG_WARNING(message) do { NLS::Debug::Logger::LogWithContext((message), NLS::Debug::ELogLevel::LOG_WARNING, NLS::Debug::ELogMode::CONSOLE_AND_FILE, __FILE__, __LINE__, __func__); } while (0)
#define NLS_LOG_ERROR(message) do { NLS::Debug::Logger::LogWithContext((message), NLS::Debug::ELogLevel::LOG_ERROR, NLS::Debug::ELogMode::CONSOLE_AND_FILE, __FILE__, __LINE__, __func__); } while (0)

namespace NLS::Debug
{
	// Logger callbacks can be registered, removed, and invoked on different
	// threads. Removal waits for in-flight callbacks, including at shutdown.
	class NLS_BASE_API LogEventDispatcher
	{
	public:
		using Callback = std::function<void(const LogData&)>;
		ListenerID AddListener(Callback callback);
		ListenerID operator+=(Callback callback) { return AddListener(std::move(callback)); }
		bool RemoveListener(ListenerID id);
		bool operator-=(ListenerID id) { return RemoveListener(id); }
		void RemoveAllListeners();
		uint64_t GetListenerCount();
		void Invoke(const LogData& data);
	private:
		std::recursive_mutex m_mutex;
		std::unordered_map<ListenerID, Callback> m_callbacks;
		ListenerID m_next = InvalidListenerID + 1u;
	};
	/*
	* 用于在控制台或文件上显示错误消息的静态类
	*/
	class NLS_BASE_API Logger
	{
	public:

		/**
		* 禁用构造函数
		*/
		Logger() = delete;

		/**
		* 输出日志
		* @param p_message
		* @param p_logLevel
		* @param p_logMode
		* @param p_handlerID
		*/
		static void Log(const std::string& p_message, ELogLevel p_logLevel = ELogLevel::LOG_DEFAULT, ELogMode p_logMode = ELogMode::DEFAULT, std::string p_handlerId = "default");
		static void LogWithContext(const std::string& message, ELogLevel level, ELogMode mode,
			const char* sourceFile, int sourceLine, const char* sourceFunction,
			ELogSource source = ELogSource::General, const std::string& scriptStack = {},
			std::string handlerId = "default");
		static LogData WriteWithContext(const std::string& message, ELogLevel level, ELogMode mode,
			const char* sourceFile, int sourceLine, const char* sourceFunction,
			ELogSource source = ELogSource::General, const std::string& scriptStack = {},
			std::string handlerId = "default");
		static void SetStackTraceMode(ELogLevel level, StackTraceMode mode);
		static StackTraceMode GetStackTraceMode(ELogLevel level);
		static void Flush();
		static void Shutdown();

		/**
		* Create console handler
		* @param p_id
		*/
		static ConsoleHandler& CreateConsoleHandler(std::string p_id);

		/**
		* Create console handler
		* @param p_id
		*/
		static FileHandler& CreateFileHandler(std::string p_id);

		/**
		* Create console handler
		* @param p_id
		*/
		static HistoryHandler& CreateHistoryHandler(std::string p_id);

		/**
		* Return target console handler
		* @param p_id
		*/
		static ConsoleHandler& GetConsoleHandler(std::string p_id);

		/**
		* Return target file handler
		* @param p_id
		*/
		static FileHandler& GetFileHandler(std::string p_id);

		/**
		* Return target history handler
		* @param p_id
		*/
		static HistoryHandler& GetHistoryHandler(std::string p_id);

	private:
		template<typename T>
		static void LogToHandlerMap(std::map<std::string, T>& p_map, const LogData& p_data, std::string p_id);

	public:
		static LogEventDispatcher LogEvent;

	private:
		static std::map<std::string, ConsoleHandler>	CONSOLE_HANDLER_MAP;
		static std::map<std::string, FileHandler>		FILE_HANDLER_MAP;
		static std::map<std::string, HistoryHandler>	HISTORY_HANDLER_MAP;
	};
}

#include "Debug/Logger.inl"
