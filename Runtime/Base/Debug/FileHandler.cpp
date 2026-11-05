#include <Time/Date.h>

#include "Debug/FileHandler.h"
#include "Debug/StackTrace.h"

#include <fstream>
#include <iostream>
#include <mutex>

namespace NLS::Debug
{
namespace
{
std::mutex& FileMutex()
{
    static auto* mutex = new std::mutex;
    return *mutex;
}
}
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
	std::lock_guard lock(FileMutex());
	if (!OUTPUT_FILE.is_open())
	{
		OUTPUT_FILE.open(LOG_FILE_PATH, std::ios_base::app);
	}

	if (OUTPUT_FILE.is_open())
	{
		OUTPUT_FILE << GetLogHeader(logData.logLevel) << logData.date << " " << logData.message << '\n';
		if (!logData.sourceFile.empty())
		{
			OUTPUT_FILE << logData.sourceFile;
			if (logData.sourceLine > 0) OUTPUT_FILE << ':' << logData.sourceLine;
			OUTPUT_FILE << '\n';
		}
		if (!logData.scriptStack.empty()) OUTPUT_FILE << logData.scriptStack << '\n';
		if (logData.nativeStack) OUTPUT_FILE << StackTrace::RawText(logData.nativeStack->frames);
		OUTPUT_FILE.flush();
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
	std::lock_guard lock(FileMutex());
	const auto next = path + "/" + __APP_LAUNCH_DATE + __LOG_EXTENSION;
	if (next != LOG_FILE_PATH && OUTPUT_FILE.is_open()) { OUTPUT_FILE.flush(); OUTPUT_FILE.close(); }
	LOG_FILE_PATH = next;
}

void FileHandler::Flush()
{
	std::lock_guard lock(FileMutex());
	if (OUTPUT_FILE.is_open()) OUTPUT_FILE.flush();
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
