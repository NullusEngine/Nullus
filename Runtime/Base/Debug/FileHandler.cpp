#include <Time/Date.h>

#include "Debug/FileHandler.h"

#include <fstream>
#include <iostream>

namespace NLS::Debug
{
std::string const FileHandler::__DEFAULT_HEADER;
std::string const FileHandler::__INFO_HEADER = "[INFO] ";
std::string const FileHandler::__WARNING_HEADER = "[WARNING] ";
std::string const FileHandler::__ERROR_HEADER = "[ERROR] ";
std::string FileHandler::__APP_LAUNCH_DATE = Time::Date::GetDateAsString();
std::string const FileHandler::__LOG_EXTENSION = ".log";

std::ofstream FileHandler::OUTPUT_FILE;
std::string FileHandler::LOG_FILE_PATH = __APP_LAUNCH_DATE + __LOG_EXTENSION;

void FileHandler::Log(const LogData& logData)
{
	if (!OUTPUT_FILE.is_open())
	{
		OUTPUT_FILE.open(LOG_FILE_PATH, std::ios_base::app);
	}

	if (OUTPUT_FILE.is_open())
	{
		OUTPUT_FILE << GetLogHeader(logData.logLevel) << logData.date << " " << logData.message << std::endl;
	}
	else
	{
		std::cerr << "Error opening file: " << LOG_FILE_PATH << std::endl;
		std::cerr << "Error state: " << OUTPUT_FILE.rdstate() << std::endl;
	}
}

std::string& FileHandler::GetLogFilePath()
{
	return LOG_FILE_PATH;
}

void FileHandler::SetLogFilePath(const std::string& path)
{
	LOG_FILE_PATH = path + "/" + __APP_LAUNCH_DATE + __LOG_EXTENSION;
}

std::string FileHandler::GetLogHeader(ELogLevel logLevel)
{
	switch (logLevel)
	{
	case ELogLevel::LOG_DEFAULT: return __DEFAULT_HEADER;
	case ELogLevel::LOG_INFO: return __INFO_HEADER;
	case ELogLevel::LOG_WARNING: return __WARNING_HEADER;
	case ELogLevel::LOG_ERROR: return __ERROR_HEADER;
	}

	return "";
}
}
