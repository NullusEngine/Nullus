#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "PlatformDef.h"

namespace NLS::Platform::Process
{

/**
 * Result of a process launch attempt
 */
struct NLS_PLATFORM_API ProcessLaunchResult
{
    bool success = false;
    std::string errorMessage;
};

/**
 * Launch an executable as a detached child process.
 * The parent process does not wait for the child to complete.
 *
 * @param executablePath  Absolute path to the executable
 * @param arguments       Command-line arguments (not including the executable name)
 * @return                Result indicating success or failure with error message
 */
NLS_PLATFORM_API ProcessLaunchResult Launch(
    const std::filesystem::path& executablePath,
    const std::vector<std::string>& arguments);

/**
 * Find an executable by name in the same directory as the current executable.
 *
 * @param name  Executable file name (e.g., "Editor.exe")
 * @return      Absolute path if found, std::nullopt otherwise
 */
NLS_PLATFORM_API std::optional<std::filesystem::path> FindExecutable(const std::string& name);

} // namespace NLS::Platform::Process
