#include "Debug/ConsoleHandler.h"
#include "Debug/ConsoleColor.h"

#include <iostream>

std::string const NLS::Debug::ConsoleHandler::__DEFAULT_HEADER;
std::string const NLS::Debug::ConsoleHandler::__INFO_HEADER = "[INFO] ";
std::string const NLS::Debug::ConsoleHandler::__WARNING_HEADER = "[WARNING] ";
std::string const NLS::Debug::ConsoleHandler::__ERROR_HEADER = "[ERROR] ";

void NLS::Debug::ConsoleHandler::Log(const LogData& p_logData)
{
	switch (p_logData.logLevel)
	{
	case ELogLevel::LOG_DEFAULT:
		std::cout << COLOR_WHITE;
		break;
	case ELogLevel::NLS_LOG_INFO:
		std::cout << COLOR_BLUE;
		break;
	case ELogLevel::NLS_LOG_WARNING:
		std::cout << COLOR_YELLOW;
		break;
	case ELogLevel::NLS_LOG_ERROR:
		std::cout << COLOR_RED;
		break;
	}

	std::ostream& output = p_logData.logLevel == ELogLevel::NLS_LOG_ERROR ? std::cerr : std::cout;

	output << GetLogHeader(p_logData.logLevel) << p_logData.date << " " << p_logData.message << std::endl;

	std::cout << COLOR_DEFAULT;
}

std::string NLS::Debug::ConsoleHandler::GetLogHeader(ELogLevel p_logLevel)
{
	switch (p_logLevel)
	{
	case ELogLevel::LOG_DEFAULT:	return __DEFAULT_HEADER;
	case ELogLevel::NLS_LOG_INFO:		return __INFO_HEADER;
	case ELogLevel::NLS_LOG_WARNING:	return __WARNING_HEADER;
	case ELogLevel::NLS_LOG_ERROR:		return __ERROR_HEADER;
	}

	return "";
}
