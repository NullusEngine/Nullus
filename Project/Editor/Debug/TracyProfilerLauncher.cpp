#include "Debug/TracyProfilerLauncher.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace NLS::Editor::Debug
{
namespace
{
#if defined(_WIN32)
constexpr const char* kProfilerExecutableName = "tracy-profiler.exe";
#else
constexpr const char* kProfilerExecutableName = "tracy-profiler";
#endif

std::filesystem::path NormalizePath(const std::filesystem::path& p_path)
{
    if (p_path.empty())
        return {};

    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(p_path, error);
    if (!error)
        return canonical;

    return p_path.lexically_normal();
}

void AppendUniqueCandidate(
    std::vector<std::filesystem::path>& p_candidates,
    const std::filesystem::path& p_candidate)
{
    if (p_candidate.empty())
        return;

    const auto normalized = NormalizePath(p_candidate);
    if (std::find(p_candidates.begin(), p_candidates.end(), normalized) == p_candidates.end())
        p_candidates.push_back(normalized);
}

// Accepts both a tools directory that directly contains the profiler and the
// versioned `Tools/Tracy/<version>` layout produced by
// Prepare-NullusTracy.ps1. Versions are listed newest first so a freshly
// prepared toolchain wins over an older copy.
void AppendToolsRootCandidates(
    std::vector<std::filesystem::path>& p_candidates,
    const std::filesystem::path& p_toolsRoot)
{
    if (p_toolsRoot.empty())
        return;

    AppendUniqueCandidate(p_candidates, p_toolsRoot / kProfilerExecutableName);

    std::error_code error;
    if (!std::filesystem::is_directory(p_toolsRoot, error) || error)
        return;

    std::vector<std::pair<std::string, std::filesystem::path>> versionedCandidates;
    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(p_toolsRoot))
        {
            std::error_code entryError;
            if (!entry.is_directory(entryError) || entryError)
                continue;

            const auto executable = entry.path() / kProfilerExecutableName;
            if (!std::filesystem::is_regular_file(executable, entryError) || entryError)
                continue;

            versionedCandidates.emplace_back(entry.path().filename().string(), executable);
        }
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return;
    }

    std::sort(
        versionedCandidates.begin(),
        versionedCandidates.end(),
        [](const auto& p_left, const auto& p_right) { return p_left.first > p_right.first; });

    for (const auto& candidate : versionedCandidates)
        AppendUniqueCandidate(p_candidates, candidate.second);
}

std::filesystem::path ResolveAbsolutePath(const std::filesystem::path& p_path)
{
    if (p_path.empty())
        return {};

    std::error_code error;
    const auto absolute = std::filesystem::absolute(p_path, error);
    if (error)
        return NormalizePath(p_path);

    return NormalizePath(absolute);
}

std::filesystem::path FindRepositoryRoot(
    const std::filesystem::path& p_startDirectory,
    std::size_t p_maxDepth)
{
    std::filesystem::path directory = NormalizePath(p_startDirectory);
    for (std::size_t depth = 0; !directory.empty() && depth <= p_maxDepth; ++depth)
    {
        std::error_code error;
        const bool hasVendoredTracySource = std::filesystem::is_directory(directory / "ThirdParty" / "Tracy", error);
        error.clear();
        const bool hasTracyTools = std::filesystem::is_directory(directory / "Tools" / "Tracy", error);
        if (hasVendoredTracySource || hasTracyTools)
            return directory;

        const auto parent = directory.parent_path();
        if (parent.empty() || parent == directory)
            break;

        directory = parent;
    }

    return {};
}
}

std::optional<std::filesystem::path> ReadTracyProfilerOverrideFromEnvironment()
{
    if (const char* overrideValue = std::getenv("NLS_TRACY_PROFILER");
        overrideValue != nullptr && overrideValue[0] != '\0')
    {
        return std::filesystem::path(overrideValue);
    }

    return std::nullopt;
}

std::vector<std::filesystem::path> BuildTracyProfilerCandidates(const TracyProfilerSearchOptions& p_options)
{
    std::vector<std::filesystem::path> candidates;

    if (p_options.toolsOverride.has_value() && !p_options.toolsOverride->empty())
    {
        // The override may be the executable itself or the directory holding it.
        const auto overridePath = ResolveAbsolutePath(*p_options.toolsOverride);
        AppendUniqueCandidate(candidates, overridePath);
        AppendToolsRootCandidates(candidates, overridePath);
    }

    const std::filesystem::path startDirectory = p_options.startDirectory.empty()
        ? ResolveAbsolutePath(std::filesystem::current_path())
        : ResolveAbsolutePath(p_options.startDirectory);

    std::filesystem::path directory = startDirectory;
    for (std::size_t depth = 0; !directory.empty() && depth <= p_options.ancestorDepth; ++depth)
    {
        AppendUniqueCandidate(candidates, directory / kProfilerExecutableName);
        // Versioned layouts: the repository checkout, and the
        // App/<platform>_<config>_Runtime_*/Tracy copy deployed by the editor
        // build so packaged output directories carry the matching tools.
        AppendToolsRootCandidates(candidates, directory / "Tools" / "Tracy");
        AppendToolsRootCandidates(candidates, directory / "Tracy");

        const auto parent = directory.parent_path();
        if (parent.empty() || parent == directory)
            break;

        directory = parent;
    }

    return candidates;
}

TracyProfilerResolution ResolveTracyProfiler(const TracyProfilerSearchOptions& p_options)
{
    TracyProfilerResolution resolution;
    for (const auto& candidate : BuildTracyProfilerCandidates(p_options))
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error)
            continue;

        resolution.executablePath = candidate;
        resolution.toolsRoot = candidate.parent_path();
        resolution.repositoryRoot = FindRepositoryRoot(resolution.toolsRoot, p_options.ancestorDepth + 2);
        break;
    }

    return resolution;
}

TracyProfilerResolution ResolveTracyProfilerFromEditorProcess()
{
    std::error_code error;
    const auto currentDirectory = std::filesystem::current_path(error);

    TracyProfilerSearchOptions options;
    options.startDirectory = error ? std::filesystem::path {} : currentDirectory;
    options.toolsOverride = ReadTracyProfilerOverrideFromEnvironment();
    return ResolveTracyProfiler(options);
}

std::filesystem::path ResolveTracyCaptureDirectory(const TracyProfilerResolution& p_resolution)
{
    if (p_resolution.repositoryRoot.empty())
        return {};

    // Prepare-NullusTracy.ps1 and Invoke-NullusTracyCapture.ps1 default to
    // Build\Tracy next to the repository root.
    std::error_code error;
    const auto lowerCaseDirectory = p_resolution.repositoryRoot / "build" / "Tracy";
    if (std::filesystem::is_directory(lowerCaseDirectory, error) && !error)
        return lowerCaseDirectory;

    error.clear();
    const auto upperCaseDirectory = p_resolution.repositoryRoot / "Build" / "Tracy";
    if (std::filesystem::is_directory(upperCaseDirectory, error) && !error)
        return upperCaseDirectory;

    return upperCaseDirectory;
}

bool LaunchTracyProfiler(const std::filesystem::path& p_executablePath, std::string& p_outError)
{
    p_outError.clear();

    if (p_executablePath.empty())
    {
        p_outError = "No Tracy profiler executable was resolved.";
        return false;
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(p_executablePath, error) || error)
    {
        p_outError = "Tracy profiler executable not found: " + p_executablePath.string();
        return false;
    }

#if defined(_WIN32)
    const auto workingDirectory = p_executablePath.parent_path().wstring();
    std::wstring commandLine = L"\"" + p_executablePath.wstring() + L"\"";
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (::CreateProcessW(
            nullptr,
            buffer.data(),
            nullptr,
            nullptr,
            FALSE,
            0u,
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup,
            &process) == FALSE)
    {
        p_outError = "CreateProcessW failed with error " + std::to_string(::GetLastError()) + ".";
        return false;
    }

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
#else
    const pid_t processId = ::fork();
    if (processId < 0)
    {
        p_outError = "fork() failed while launching the Tracy profiler.";
        return false;
    }

    if (processId == 0)
    {
        ::setsid();
        ::execl(p_executablePath.c_str(), p_executablePath.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    return true;
#endif
}
}