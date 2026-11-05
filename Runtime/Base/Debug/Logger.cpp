#include "Debug/Logger.h"
#include "Debug/StackTrace.h"
#include "Time/Date.h"

#include <atomic>
#include <vector>

namespace NLS::Debug
{
LogEventDispatcher Logger::LogEvent;

namespace
{
std::recursive_mutex& HandlerMutex()
{
    static auto* mutex = new std::recursive_mutex;
    return *mutex;
}
std::atomic<uint64_t> g_nextLogId{0};
std::atomic<int> g_stackModes[3]{{1}, {1}, {1}};
size_t SeverityIndex(ELogLevel level)
{
    if (level == ELogLevel::LOG_ERROR) return 2;
    if (level == ELogLevel::LOG_WARNING) return 1;
    return 0;
}
}

ListenerID LogEventDispatcher::AddListener(Callback callback)
{
    std::lock_guard lock(m_mutex);
    while (m_next == InvalidListenerID || m_callbacks.contains(m_next)) ++m_next;
    const auto id = m_next++;
    m_callbacks.emplace(id, std::move(callback));
    return id;
}

bool LogEventDispatcher::RemoveListener(ListenerID id)
{
    std::lock_guard lock(m_mutex);
    return m_callbacks.erase(id) != 0;
}

void LogEventDispatcher::RemoveAllListeners()
{
    std::lock_guard lock(m_mutex);
    m_callbacks.clear();
}

uint64_t LogEventDispatcher::GetListenerCount()
{
    std::lock_guard lock(m_mutex);
    return m_callbacks.size();
}

void LogEventDispatcher::Invoke(const LogData& data)
{
    std::lock_guard lock(m_mutex);
    // A callback may remove itself or add a listener while it is running.
    const auto callbacks = m_callbacks;
    for (const auto& [id, callback] : callbacks)
        if (m_callbacks.contains(id)) callback(data);
}

std::map<std::string, ConsoleHandler> Logger::CONSOLE_HANDLER_MAP;
std::map<std::string, FileHandler> Logger::FILE_HANDLER_MAP;
std::map<std::string, HistoryHandler> Logger::HISTORY_HANDLER_MAP;

void Logger::Log(const std::string& message, ELogLevel logLevel, ELogMode logMode, std::string handlerId)
{
	LogWithContext(message, logLevel, logMode, nullptr, 0, nullptr, ELogSource::General, {}, std::move(handlerId));
}

void Logger::LogWithContext(const std::string& message, ELogLevel logLevel, ELogMode logMode,
    const char* sourceFile, int sourceLine, const char* sourceFunction, ELogSource source,
    const std::string& scriptStack, std::string handlerId)
{
	(void)WriteWithContext(message, logLevel, logMode, sourceFile, sourceLine, sourceFunction,
		source, scriptStack, std::move(handlerId));
}

LogData Logger::WriteWithContext(const std::string& message, ELogLevel logLevel, ELogMode logMode,
    const char* sourceFile, int sourceLine, const char* sourceFunction, ELogSource source,
    const std::string& scriptStack, std::string handlerId)
{
	LogData logData;
	logData.message = message;
	logData.logLevel = logLevel;
	{
		std::lock_guard lock(HandlerMutex());
		logData.date = Time::Date::GetDateAsString();
	}
	logData.id = ++g_nextLogId;
	logData.source = source;
	if (sourceFile) logData.sourceFile = sourceFile;
	logData.sourceLine = sourceLine;
	if (sourceFunction) logData.sourceFunction = sourceFunction;
	const auto traceMode = GetStackTraceMode(logLevel);
	if (traceMode != StackTraceMode::None) logData.scriptStack = scriptStack;
	if (traceMode == StackTraceMode::Full) logData.nativeStack = StackTrace::Capture();
	bool broadcastLogEvent = false;

	{
	std::lock_guard lock(HandlerMutex());
	switch (logMode)
	{
	case ELogMode::DEFAULT:
	case ELogMode::CONSOLE:
		LogToHandlerMap<ConsoleHandler>(CONSOLE_HANDLER_MAP, logData, handlerId);
		broadcastLogEvent = true;
		break;
	case ELogMode::FILE: LogToHandlerMap<FileHandler>(FILE_HANDLER_MAP, logData, handlerId); break;
	case ELogMode::HISTORY: LogToHandlerMap<HistoryHandler>(HISTORY_HANDLER_MAP, logData, handlerId); break;
	case ELogMode::CONSOLE_AND_FILE:
		LogToHandlerMap<ConsoleHandler>(CONSOLE_HANDLER_MAP, logData, handlerId);
		LogToHandlerMap<FileHandler>(FILE_HANDLER_MAP, logData, handlerId);
		broadcastLogEvent = true;
		break;
	case ELogMode::ALL:
		LogToHandlerMap<ConsoleHandler>(CONSOLE_HANDLER_MAP, logData, handlerId);
		LogToHandlerMap<FileHandler>(FILE_HANDLER_MAP, logData, handlerId);
		LogToHandlerMap<HistoryHandler>(HISTORY_HANDLER_MAP, logData, handlerId);
		broadcastLogEvent = true;
		break;
	}
	}

	if (broadcastLogEvent)
		LogEvent.Invoke(logData);
	return logData;
}

void Logger::SetStackTraceMode(ELogLevel level, StackTraceMode mode)
{
    g_stackModes[SeverityIndex(level)].store(static_cast<int>(mode), std::memory_order_relaxed);
}

StackTraceMode Logger::GetStackTraceMode(ELogLevel level)
{
    return static_cast<StackTraceMode>(g_stackModes[SeverityIndex(level)].load(std::memory_order_relaxed));
}

void Logger::Flush() { FileHandler::Flush(); }
void Logger::Shutdown() { StackTrace::Shutdown(); FileHandler::Flush(); }

ConsoleHandler& Logger::CreateConsoleHandler(std::string id)
{
	std::lock_guard lock(HandlerMutex());
	CONSOLE_HANDLER_MAP.emplace(id, ConsoleHandler());
	return CONSOLE_HANDLER_MAP[id];
}

FileHandler& Logger::CreateFileHandler(std::string id)
{
	std::lock_guard lock(HandlerMutex());
	FILE_HANDLER_MAP.emplace(id, FileHandler());
	return FILE_HANDLER_MAP[id];
}

HistoryHandler& Logger::CreateHistoryHandler(std::string id)
{
	std::lock_guard lock(HandlerMutex());
	HISTORY_HANDLER_MAP.emplace(id, HistoryHandler());
	return HISTORY_HANDLER_MAP[id];
}

ConsoleHandler& Logger::GetConsoleHandler(std::string id)
{
	std::lock_guard lock(HandlerMutex());
	return CONSOLE_HANDLER_MAP[id];
}

FileHandler& Logger::GetFileHandler(std::string id)
{
	std::lock_guard lock(HandlerMutex());
	return FILE_HANDLER_MAP[id];
}

HistoryHandler& Logger::GetHistoryHandler(std::string id)
{
	std::lock_guard lock(HandlerMutex());
	return HISTORY_HANDLER_MAP[id];
}
}
