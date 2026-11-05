#pragma once
#include "PlatformDef.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace NLS::Platform::Process
{
struct ProcessRunOptions
{
    std::filesystem::path executable, workingDirectory, stdoutFile;
    std::vector<std::string> arguments;
    // This is the complete environment, not an overlay on inherited Git/curl
    // settings. Callers explicitly select trusted runtime variables.
    std::map<std::string,std::string> environment;
    uint64_t maxOutputBytes = 8 * 1024 * 1024;
    // Sampling bounds staging disk use; a child can overshoot one interval.
    std::filesystem::path monitoredDirectory;
    uint64_t maxDirectoryBytes = 0;
    std::chrono::milliseconds timeout = std::chrono::minutes(10);
    const std::atomic_bool* cancel = nullptr;
};
struct ProcessRunResult
{
    int exitCode = -1;
    bool cancelled = false, timedOut = false, outputLimitExceeded = false;
    std::string output, error;
    bool Succeeded() const { return exitCode == 0 && !cancelled && !timedOut && !outputLimitExceeded; }
};
NLS_PLATFORM_API ProcessRunResult Run(const ProcessRunOptions& options);
}
