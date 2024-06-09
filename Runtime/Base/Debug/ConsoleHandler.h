#pragma once

#include <string>

#include "Debug/ILogHandler.h"

namespace NLS::Debug
{
	/*
	* 处理控制台中的日志
	*/
	class ConsoleHandler : public ILogHandler
	{
	public:

		/**
		* 输出日志到控制台
		* @param p_logData
		*/
		void Log(const LogData& p_logData);

	private:
		static std::string GetLogHeader(ELogLevel p_logLevel);

		static const std::string __DEFAULT_HEADER;
		static const std::string __INFO_HEADER;
		static const std::string __WARNING_HEADER;
		static const std::string __ERROR_HEADER;
	};
}