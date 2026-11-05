#pragma once

#include <string>
#include <cstdint>
#include <memory>


namespace NLS::Debug
{
	struct NativeStackTrace;
	enum class ELogSource : uint8_t { General, CSharp, Lua, Build };
	enum class StackTraceMode : uint8_t { None, ScriptOnly, Full };
	enum class ELogLevel
	{
		LOG_DEFAULT,
		LOG_INFO,
		LOG_WARNING,
		LOG_ERROR
	};

	enum class ELogMode
	{
		DEFAULT,
		CONSOLE,
		FILE,
		HISTORY,
		ALL,
		CONSOLE_AND_FILE
	};

	/**
	* 存储日志信息
	*/
	struct LogData
	{
		std::string message;
		ELogLevel logLevel;
		std::string date;
		uint64_t id = 0;
		ELogSource source = ELogSource::General;
		std::string sourceFile;
		int sourceLine = 0;
		std::string sourceFunction;
		std::string scriptStack;
		std::shared_ptr<NativeStackTrace> nativeStack;
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
