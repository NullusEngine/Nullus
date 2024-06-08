#pragma once

#include <string>


namespace NLS::Debug
{
	enum class ELogLevel
	{
		LOG_DEFAULT,
		NLS_LOG_INFO,
		NLS_LOG_WARNING,
		NLS_LOG_ERROR
	};

	enum class ELogMode
	{
		DEFAULT,
		CONSOLE,
		FILE,
		HISTORY,
		ALL
	};

	/**
	* 存储日志信息
	*/
	struct LogData
	{
		std::string message;
		ELogLevel logLevel;
		std::string date;
	};

	/*
	* 以某种方式处理日志（由派生类定义）
	*/
	class ILogHandler
	{
		friend class Logger;

	private:
		virtual void Log(const LogData& p_logData) = 0;
	};
}