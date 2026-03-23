#include "Debug/Logger.h"
#include "Time/Date.h"

namespace NLS::Debug
{
Event<const LogData&> Logger::LogEvent;

std::map<std::string, ConsoleHandler> Logger::CONSOLE_HANDLER_MAP;
std::map<std::string, FileHandler> Logger::FILE_HANDLER_MAP;
std::map<std::string, HistoryHandler> Logger::HISTORY_HANDLER_MAP;

void Logger::Log(const std::string& message, ELogLevel logLevel, ELogMode logMode, std::string handlerId)
{
	LogData logData{ message, logLevel, Time::Date::GetDateAsString() };

	switch (logMode)
	{
	case ELogMode::DEFAULT:
	case ELogMode::CONSOLE: LogToHandlerMap<ConsoleHandler>(CONSOLE_HANDLER_MAP, logData, handlerId); break;
	case ELogMode::FILE: LogToHandlerMap<FileHandler>(FILE_HANDLER_MAP, logData, handlerId); break;
	case ELogMode::HISTORY: LogToHandlerMap<HistoryHandler>(HISTORY_HANDLER_MAP, logData, handlerId); break;
	case ELogMode::ALL:
		LogToHandlerMap<ConsoleHandler>(CONSOLE_HANDLER_MAP, logData, handlerId);
		LogToHandlerMap<FileHandler>(FILE_HANDLER_MAP, logData, handlerId);
		LogToHandlerMap<HistoryHandler>(HISTORY_HANDLER_MAP, logData, handlerId);
		break;
	}

	LogEvent.Invoke(logData);
}

ConsoleHandler& Logger::CreateConsoleHandler(std::string id)
{
	CONSOLE_HANDLER_MAP.emplace(id, ConsoleHandler());
	return CONSOLE_HANDLER_MAP[id];
}

FileHandler& Logger::CreateFileHandler(std::string id)
{
	FILE_HANDLER_MAP.emplace(id, FileHandler());
	return FILE_HANDLER_MAP[id];
}

HistoryHandler& Logger::CreateHistoryHandler(std::string id)
{
	HISTORY_HANDLER_MAP.emplace(id, HistoryHandler());
	return HISTORY_HANDLER_MAP[id];
}

ConsoleHandler& Logger::GetConsoleHandler(std::string id)
{
	return CONSOLE_HANDLER_MAP[id];
}

FileHandler& Logger::GetFileHandler(std::string id)
{
	return FILE_HANDLER_MAP[id];
}

HistoryHandler& Logger::GetHistoryHandler(std::string id)
{
	return HISTORY_HANDLER_MAP[id];
}
}
