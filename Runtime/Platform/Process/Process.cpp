#include "Process.h"

#include <cstdlib>
#include <sstream>
#include <system_error>

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

namespace
{
std::filesystem::path NormalizePath(std::filesystem::path path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error)
        normalized = path.lexically_normal();
    return normalized;
}

std::filesystem::path StripTrailingEmptyFilename(std::filesystem::path path)
{
    path = path.lexically_normal();
    while (!path.empty() && !path.has_filename())
        path = path.parent_path();
    return path;
}
}

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
    const auto exeDir = GetCurrentExecutablePath().parent_path();

    auto candidate = exeDir / name;
    if (std::filesystem::exists(candidate))
        return std::filesystem::canonical(candidate);

    return std::nullopt;
}

std::filesystem::path GetCurrentExecutablePath()
{
#ifdef _WIN32
    wchar_t modulePath[MAX_PATH] {};
    const DWORD size = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (size > 0u)
        return NormalizePath(std::filesystem::path(modulePath));
#elif __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return NormalizePath(std::filesystem::path(buffer.data()));
#else
    char buffer[4096] {};
    const auto size = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1u);
    if (size > 0)
        return NormalizePath(std::filesystem::path(std::string(buffer, static_cast<size_t>(size))));
#endif
    return NormalizePath(std::filesystem::current_path());
}

InstallResourceRoots ResolveInstallResourceRoots(const std::filesystem::path& executablePath)
{
    auto executable = StripTrailingEmptyFilename(executablePath.empty() ? GetCurrentExecutablePath() : executablePath);
    const auto executableDirectory = executable.has_filename()
        ? executable.parent_path()
        : executable;

    auto installRoot = executableDirectory;
    std::error_code error;
    for (auto candidate = executableDirectory; !candidate.empty(); candidate = candidate.parent_path())
    {
        if (std::filesystem::is_directory(candidate / "Assets" / "Editor", error) ||
            std::filesystem::is_directory(candidate / "App" / "Assets" / "Editor", error))
        {
            installRoot = candidate;
            break;
        }
        if (candidate == candidate.parent_path())
            break;
    }

    auto assetsRoot = installRoot / "Assets";
    if (!std::filesystem::is_directory(assetsRoot / "Editor", error) &&
        std::filesystem::is_directory(installRoot / "App" / "Assets" / "Editor", error))
    {
        assetsRoot = installRoot / "App" / "Assets";
    }

    InstallResourceRoots roots;
    roots.installRoot = NormalizePath(installRoot);
    roots.assetsRoot = NormalizePath(assetsRoot);
    roots.editorAssetsRoot = NormalizePath(assetsRoot / "Editor");
    roots.engineAssetsRoot = NormalizePath(assetsRoot / "Engine");
    return roots;
}

} // namespace NLS::Platform::Process
