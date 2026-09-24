#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Debug
{
// Search inputs for the Tracy profiler executable. The editor sets its working
// directory to the folder that contains Editor.exe before this runs, so an empty
// start directory means "search from the running executable directory".
struct TracyProfilerSearchOptions
{
    std::filesystem::path startDirectory;
    // Explicit profiler executable or tools directory, normally NLS_TRACY_PROFILER.
    std::optional<std::filesystem::path> toolsOverride;
    // Number of parent directories inspected for a repository checkout.
    std::size_t ancestorDepth = 6;
};

struct TracyProfilerResolution
{
    std::filesystem::path executablePath;
    std::filesystem::path toolsRoot;
    std::filesystem::path repositoryRoot;

    [[nodiscard]] bool IsValid() const { return !executablePath.empty(); }
};

// Reads NLS_TRACY_PROFILER. The value may point at the profiler executable itself
// or at the directory that contains it.
std::optional<std::filesystem::path> ReadTracyProfilerOverrideFromEnvironment();

// Ordered candidate list. The override wins, then a profiler copied next to the
// editor, then the versioned `Tools/Tracy/<version>` and `Tracy/<version>`
// layouts of every ancestor directory. The editor build deploys the latter
// next to Editor.exe in App/<platform>_<config>_Runtime_*.
std::vector<std::filesystem::path> BuildTracyProfilerCandidates(const TracyProfilerSearchOptions& p_options);

// Returns the first candidate that exists on disk together with its roots.
TracyProfilerResolution ResolveTracyProfiler(const TracyProfilerSearchOptions& p_options);

// Convenience wrapper for the editor UI: current working directory plus
// NLS_TRACY_PROFILER.
TracyProfilerResolution ResolveTracyProfilerFromEditorProcess();

// Capture output directory used by the Tracy helper scripts. Empty when the
// repository root could not be located.
std::filesystem::path ResolveTracyCaptureDirectory(const TracyProfilerResolution& p_resolution);

// Launches the profiler as a detached process so it outlives the editor call.
// Fills p_outError when the executable is missing or the platform refuses to
// start it.
bool LaunchTracyProfiler(const std::filesystem::path& p_executablePath, std::string& p_outError);
}