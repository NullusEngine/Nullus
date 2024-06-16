#pragma once

#include <string>
#include <map>
#include "UDRefl/config.hpp"
#include <Eventing/Event.h>

#include "Debug/ILogHandler.h"
#include "Debug/ConsoleHandler.h"
#include "Debug/FileHandler.h"
#include "Debug/HistoryHandler.h"

//#define NLS_LOG(message)			NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::LOG_DEFAULT,	NLS::Debug::ELogMode::CONSOLE)
//#define NLS_LOG_INFO(message)		NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_INFO,		NLS::Debug::ELogMode::CONSOLE)
//#define NLS_LOG_WARNING(message)	NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_WARNING,	NLS::Debug::ELogMode::CONSOLE)
//#define NLS_LOG_ERROR(message)	NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_ERROR,	NLS::Debug::ELogMode::CONSOLE)

#define NLS_LOG(message)			NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::LOG_DEFAULT, 	NLS::Debug::ELogMode::CONSOLE); NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::LOG_DEFAULT, NLS::Debug::ELogMode::FILE)
#define NLS_LOG_INFO(message)	NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_INFO, 	NLS::Debug::ELogMode::CONSOLE); NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_INFO, 	NLS::Debug::ELogMode::FILE)
#define NLS_LOG_WARNING(message)	NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_WARNING, 	NLS::Debug::ELogMode::CONSOLE); NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_WARNING, NLS::Debug::ELogMode::FILE)
#define NLS_LOG_ERROR(message)	NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_ERROR, 	NLS::Debug::ELogMode::CONSOLE); NLS::Debug::Logger::Log(message, NLS::Debug::ELogLevel::NLS_LOG_ERROR, 	NLS::Debug::ELogMode::FILE)

namespace NLS::Debug
{
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
		static NLS::Event<const LogData&> LogEvent;

	private:
		static std::map<std::string, ConsoleHandler>	CONSOLE_HANDLER_MAP;
		static std::map<std::string, FileHandler>		FILE_HANDLER_MAP;
		static std::map<std::string, HistoryHandler>	HISTORY_HANDLER_MAP;
	};
}

#include "Debug/Logger.inl"