#include "Debug/HistoryHandler.h"

namespace NLS::Debug
{
std::queue<LogData> HistoryHandler::LOG_QUEUE;

void HistoryHandler::Log(const LogData& logData)
{
	LOG_QUEUE.push(logData);
}

std::queue<LogData>& HistoryHandler::GetLogQueue()
{
	return LOG_QUEUE;
}
}
