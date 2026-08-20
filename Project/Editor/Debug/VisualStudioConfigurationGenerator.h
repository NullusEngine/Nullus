#pragma once

#include <filesystem>
#include <string>

namespace NLS::Editor::Debug
{
struct ProjectDebugManifest;

struct VisualStudioConfigurationResult
{
    bool success = false;
    std::filesystem::path launchSettingsPath;
    std::filesystem::path projectUserPath;
    std::filesystem::path guidePath;
    std::filesystem::path attachScriptPath;
    std::filesystem::path projectWorkspacePath;
    std::filesystem::path launchProfilePath;
    std::string errorMessage;
};

// Generates the same project-scoped F5 metadata as the manifest overload. The
// editor executable is accepted for legacy callers, but all outputs still
// live under <Project>/Library/IDE/VisualStudio.
VisualStudioConfigurationResult GenerateVisualStudioConfiguration(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& editorExecutable = {});

// Generates the project-scoped Visual Studio metadata used by the bundled
// Nullus debug provider. This overload never writes the shared
// Managed/Nullus.GameScripts.csproj.user or an attach helper.
VisualStudioConfigurationResult GenerateVisualStudioConfiguration(
    const ProjectDebugManifest& manifest);
}
