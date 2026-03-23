#include "Debug/ConsoleHandler.h"
#include "Debug/ConsoleColor.h"

#include <iostream>

namespace NLS::Debug
{
std::string const ConsoleHandler::__DEFAULT_HEADER;
std::string const ConsoleHandler::__INFO_HEADER = "[INFO] ";
std::string const ConsoleHandler::__WARNING_HEADER = "[WARNING] ";
std::string const ConsoleHandler::__ERROR_HEADER = "[ERROR] ";

void ConsoleHandler::Log(const LogData& logData)
{
	switch (logData.logLevel)
	{
	case ELogLevel::LOG_DEFAULT:
		std::cout << COLOR_WHITE;
		break;
	case ELogLevel::LOG_INFO:
		std::cout << COLOR_BLUE;
		break;
	case ELogLevel::LOG_WARNING:
		std::cout << COLOR_YELLOW;
		break;
	case ELogLevel::LOG_ERROR:
		std::cout << COLOR_RED;
		break;
	}

	std::ostream& output = logData.logLevel == ELogLevel::LOG_ERROR ? std::cerr : std::cout;
	output << GetLogHeader(logData.logLevel) << logData.date << " " << logData.message << std::endl;

	std::cout << COLOR_DEFAULT;
}

std::string ConsoleHandler::GetLogHeader(ELogLevel logLevel)
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
