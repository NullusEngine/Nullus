#pragma once

#include <EngineDef.h>

#include <filesystem>
#include <string>

namespace NLS::Engine::Settings
{
// Launch options live in the project file (.nullus) because they are read while
// the editor and the game are starting and cannot change afterwards. The
// launcher owns the file and edits these keys; the runtimes only read them.
namespace ProjectLaunchOptionKeys
{
// Shared with the game runtime, which reads the same key for its swapchain.
constexpr const char* VerticalSynchronization = "vsync";
// Shared with the game runtime, which reads the same key for its driver.
constexpr const char* ThreadedRendering = "enable_threaded_rendering";
constexpr const char* RenderDocStartupCaptureFrames = "renderdoc_startup_capture_frames";
}

struct ProjectLaunchOptions
{
    bool verticalSynchronization = true;
    bool threadedRendering = true;
    int renderDocStartupCaptureFrames = 0;

    // False when the project file does not carry the key yet, so callers can
    // distinguish an explicit value from a default and migrate legacy settings.
    bool hasVerticalSynchronization = false;
    bool hasThreadedRendering = false;
    bool hasRenderDocStartupCaptureFrames = false;
};

// Reads the launch options from a project file. Missing keys keep their default
// value and leave the matching has* flag false. Parsing problems are reported
// through p_outError when it is not null.
NLS_ENGINE_API ProjectLaunchOptions ReadProjectLaunchOptions(
    const std::filesystem::path& p_projectFilePath,
    std::string* p_outError = nullptr);

// Writes every launch option into the project file, creating the file when it
// does not exist yet.
NLS_ENGINE_API bool WriteProjectLaunchOptions(
    const std::filesystem::path& p_projectFilePath,
    const ProjectLaunchOptions& p_options,
    std::string* p_outError = nullptr);
}
