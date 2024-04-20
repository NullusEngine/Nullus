#include "Debug/Logger.h"
#include "Time/Date.h"

NLS::Event<const NLS::Debug::LogData&> NLS::Debug::Logger::LogEvent;

std::map<std::string, NLS::Debug::ConsoleHandler>	NLS::Debug::Logger::CONSOLE_HANDLER_MAP;
std::map<std::string, NLS::Debug::FileHandler>		NLS::Debug::Logger::FILE_HANDLER_MAP;
std::map<std::string, NLS::Debug::HistoryHandler>	NLS::Debug::Logger::HISTORY_HANDLER_MAP;

void NLS::Debug::Logger::Log(const std::string& p_message, ELogLevel p_logLevel, ELogMode p_logMode, std::string p_handlerId)
{
	LogData logData{ p_message, p_logLevel, NLS::Time::Date::GetDateAsString() };

	switch (p_logMode)
	{
	case ELogMode::DEFAULT:
	case ELogMode::CONSOLE: LogToHandlerMap<ConsoleHandler>(CONSOLE_HANDLER_MAP, logData, p_handlerId); break;
	case ELogMode::FILE:	LogToHandlerMap<FileHandler>(FILE_HANDLER_MAP, logData, p_handlerId);		break;
	case ELogMode::HISTORY: LogToHandlerMap<HistoryHandler>(HISTORY_HANDLER_MAP, logData, p_handlerId);	break;
	case ELogMode::ALL:
		LogToHandlerMap<ConsoleHandler>(CONSOLE_HANDLER_MAP, logData, p_handlerId);
		LogToHandlerMap<FileHandler>(FILE_HANDLER_MAP, logData, p_handlerId);
		LogToHandlerMap<HistoryHandler>(HISTORY_HANDLER_MAP, logData, p_handlerId);
		break;
	}

	LogEvent.Invoke(logData);
}

NLS::Debug::ConsoleHandler& NLS::Debug::Logger::CreateConsoleHandler(std::string p_id)
{
	CONSOLE_HANDLER_MAP.emplace(p_id, NLS::Debug::ConsoleHandler());
	return CONSOLE_HANDLER_MAP[p_id];
}

NLS::Debug::FileHandler& NLS::Debug::Logger::CreateFileHandler(std::string p_id)
{
	FILE_HANDLER_MAP.emplace(p_id, NLS::Debug::FileHandler());
	return FILE_HANDLER_MAP[p_id];
}

NLS::Debug::HistoryHandler& NLS::Debug::Logger::CreateHistoryHandler(std::string p_id)
{
	HISTORY_HANDLER_MAP.emplace(p_id, NLS::Debug::HistoryHandler());
	return HISTORY_HANDLER_MAP[p_id];
}

NLS::Debug::ConsoleHandler& NLS::Debug::Logger::GetConsoleHandler(std::string p_id)
{
	return CONSOLE_HANDLER_MAP[p_id];
}

NLS::Debug::FileHandler& NLS::Debug::Logger::GetFileHandler(std::string p_id)
{
	return FILE_HANDLER_MAP[p_id];
}

NLS::Debug::HistoryHandler& NLS::Debug::Logger::GetHistoryHandler(std::string p_id)
{
	return HISTORY_HANDLER_MAP[p_id];
}
