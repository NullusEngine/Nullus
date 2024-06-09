#pragma once

#include <string>
#include <queue>

#include "Debug/ILogHandler.h"

namespace NLS::Debug
{
	/*
	* 处理文件中的日志
	*/
	class FileHandler : public ILogHandler
	{
	public:

		/**
		* 输出日志到文件
		*/
		void Log(const LogData& p_logData);
	
		/**
		* 返回日志文件路径
		*/
		static std::string& GetLogFilePath();

		/**
		* 设置日志文件路径
		* @param p_path
		*/
		static void SetLogFilePath(const std::string& p_path);

	private:
		static std::string GetLogHeader(ELogLevel p_logLevel);

		static const std::string __DEFAULT_HEADER;
		static const std::string __INFO_HEADER;
		static const std::string __WARNING_HEADER;
		static const std::string __ERROR_HEADER;
		static std::string __APP_LAUNCH_DATE;
		static const std::string __LOG_EXTENSION;

		static std::string LOG_FILE_PATH;
		static std::ofstream OUTPUT_FILE;
	};
}