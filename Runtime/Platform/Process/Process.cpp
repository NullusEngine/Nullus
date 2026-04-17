#include "Process.h"

#include <cstdlib>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace NLS::Platform::Process
{

#ifdef _WIN32

ProcessLaunchResult Launch(
    const std::filesystem::path& executablePath,
    const std::vector<std::string>& arguments)
{
    ProcessLaunchResult result;

    if (!std::filesystem::exists(executablePath))
    {
        result.success = false;
        result.errorMessage = "Executable not found: " + executablePath.string();
        return result;
    }

    // Build command line: "executable" arg1 arg2 ...
    // All arguments are quoted to handle spaces
    auto quote = [](const std::string& s) -> std::string
    {
        return "\"" + s + "\"";
    };

    std::string cmdLine = quote(executablePath.string());
    for (const auto& arg : arguments)
    {
        cmdLine += " ";
        cmdLine += quote(arg);
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Create the command line buffer (CreateProcessA needs mutable buffer)
    std::vector<char> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        DETACHED_PROCESS | CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    if (!ok)
    {
        DWORD err = GetLastError();
        result.success = false;
        result.errorMessage = "Failed to create process (error " + std::to_string(err) + "): " + executablePath.string();
        return result;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    result.success = true;
    return result;
}

#else // Linux / macOS

ProcessLaunchResult Launch(
    const std::filesystem::path& executablePath,
    const std::vector<std::string>& arguments)
{
    ProcessLaunchResult result;

    if (!std::filesystem::exists(executablePath))
    {
        result.success = false;
        result.errorMessage = "Executable not found: " + executablePath.string();
        return result;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        result.success = false;
        result.errorMessage = "fork() failed: " + std::string(std::strerror(errno));
        return result;
    }

    if (pid == 0)
    {
        // Child process
        setsid(); // Create new session so parent exit doesn't kill child

        // Build argv array
        std::vector<char*> argv;
        std::string exeName = executablePath.filename().string();
        std::vector<std::string> storedArgs;
        storedArgs.push_back(executablePath.string());
        for (const auto& arg : arguments)
            storedArgs.push_back(arg);

        for (auto& arg : storedArgs)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        execvp(executablePath.c_str(), argv.data());

        // If execvp returns, it failed
        _exit(127);
    }

    // Parent process - child is now detached
    result.success = true;
    return result;
}

#endif

std::optional<std::filesystem::path> FindExecutable(const std::string& name)
{
    std::filesystem::path exeDir;

#ifdef _WIN32
    {
        char modulePath[MAX_PATH];
        GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        exeDir = std::filesystem::path(modulePath).parent_path();
    }
#elif __APPLE__
    {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> buffer(size);
        _NSGetExecutablePath(buffer.data(), &size);
        exeDir = std::filesystem::path(buffer.data()).parent_path();
    }
#else
    {
        exeDir = std::filesystem::canonical("/proc/self/exe").parent_path();
    }
#endif

    auto candidate = exeDir / name;
    if (std::filesystem::exists(candidate))
        return std::filesystem::canonical(candidate);

    return std::nullopt;
}

} // namespace NLS::Platform::Process
