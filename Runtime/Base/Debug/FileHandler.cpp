#include <Time/Date.h>

#include "Debug/FileHandler.h"
#include <fstream>
#include <iostream>

std::string const NLS::Debug::FileHandler::__DEFAULT_HEADER;
std::string const NLS::Debug::FileHandler::__INFO_HEADER = "[INFO] ";
std::string const NLS::Debug::FileHandler::__WARNING_HEADER = "[WARNING] ";
std::string const NLS::Debug::FileHandler::__ERROR_HEADER = "[ERROR] ";
std::string NLS::Debug::FileHandler::__APP_LAUNCH_DATE = NLS::Time::Date::GetDateAsString();
std::string const NLS::Debug::FileHandler::__LOG_EXTENSION = ".log";

std::ofstream NLS::Debug::FileHandler::OUTPUT_FILE;
std::string NLS::Debug::FileHandler::LOG_FILE_PATH = __APP_LAUNCH_DATE + __LOG_EXTENSION;

void NLS::Debug::FileHandler::Log(const LogData& p_logData)
{
    if (!OUTPUT_FILE.is_open())
    {
        OUTPUT_FILE.open(LOG_FILE_PATH, std::ios_base::app);
    }

    if (OUTPUT_FILE.is_open())
        OUTPUT_FILE << GetLogHeader(p_logData.logLevel) << p_logData.date << " " << p_logData.message << std::endl;
    else
    {
        std::cerr << "Error opening file: " << LOG_FILE_PATH << std::endl;
        std::cerr << "Error state: " << OUTPUT_FILE.rdstate() << std::endl;
    }
}

std::string& NLS::Debug::FileHandler::GetLogFilePath()
{
    return LOG_FILE_PATH;
}

void NLS::Debug::FileHandler::SetLogFilePath(const std::string& p_path)
{
    LOG_FILE_PATH = p_path + "/" + __APP_LAUNCH_DATE + __LOG_EXTENSION;
}

std::string NLS::Debug::FileHandler::GetLogHeader(ELogLevel p_logLevel)
{
    switch (p_logLevel)
    {
        case ELogLevel::LOG_DEFAULT:
            return __DEFAULT_HEADER;
        case ELogLevel::LOG_INFO:
            return __INFO_HEADER;
        case ELogLevel::LOG_WARNING:
            return __WARNING_HEADER;
        case ELogLevel::LOG_ERROR:
            return __ERROR_HEADER;
    }

    return "";
}