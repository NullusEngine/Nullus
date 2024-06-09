#include "Debug/HistoryHandler.h"

std::queue<NLS::Debug::LogData> NLS::Debug::HistoryHandler::LOG_QUEUE;

void NLS::Debug::HistoryHandler::Log(const LogData& p_logData)
{
	LOG_QUEUE.push(p_logData);
}

std::queue<NLS::Debug::LogData>& NLS::Debug::HistoryHandler::GetLogQueue()
{
	return LOG_QUEUE;
}