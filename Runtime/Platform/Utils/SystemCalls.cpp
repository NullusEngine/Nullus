#include "PathParser.h"
#include "SystemCalls.h"

#include <cstdlib>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace NLS::Platform
{
void SystemCalls::ShowInExplorer(const std::string& p_path)
{
    std::string command;
#ifdef _WIN32
    command = "explorer " + Utils::PathParser::MakeWindowsStyle(p_path);
#elif __APPLE__
    command = "open " + p_path;
#elif __linux__
    command = "xdg-open " + p_path;
#else
    throw std::runtime_error("Unsupported platform");
#endif
    std::system(command.c_str());
}

void SystemCalls::OpenFile(const std::string& p_file, const std::string& p_workingDir)
{
    std::string command;
#ifdef _WIN32
    command = "start " + Utils::PathParser::MakeWindowsStyle(p_file);
    if (!p_workingDir.empty())
    {
        fs::current_path(p_workingDir);
    }
#elif __APPLE__
    command = "open " + p_file;
#elif __linux__
    command = "xdg-open " + p_file;
    if (!p_workingDir.empty())
    {
        fs::current_path(p_workingDir);
    }
#else
    throw std::runtime_error("Unsupported platform");
#endif
    std::system(command.c_str());
}

void SystemCalls::EditFile(const std::string& p_file)
{
    std::string command;
#ifdef _WIN32
    command = "notepad " + Utils::PathParser::MakeWindowsStyle(p_file);
#elif __APPLE__
    command = "open -e " + p_file;
#elif __linux__
    command = "xdg-open " + p_file;
#else
    throw std::runtime_error("Unsupported platform");
#endif
    std::system(command.c_str());
}

void SystemCalls::OpenURL(const std::string& p_url)
{
    std::string command;
#ifdef _WIN32
    command = "start " + p_url;
#elif __APPLE__
    command = "open " + p_url;
#elif __linux__
    command = "xdg-open " + p_url;
#else
    throw std::runtime_error("Unsupported platform");
#endif
    std::system(command.c_str());
}
}

