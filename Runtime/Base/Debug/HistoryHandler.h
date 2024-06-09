#pragma once

#include <string>
#include <queue>

#include "ILogHandler.h"

namespace NLS::Debug
{
	/*
	* 处理历史队列中的日志
	*/
	class HistoryHandler : public ILogHandler
	{
	public:

		/**
		* 打印日志到到历史记录
		*/
		void Log(const LogData& p_logData);

		/**
		* 返回日志队列
		*/
		static std::queue<LogData>& GetLogQueue();

	private:

		static std::queue<LogData> LOG_QUEUE;
	};
}