#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ProjectDebugWorkspace.h"

namespace NLS::Editor::Debug
{
struct ExternalCodeEditorResult
{
    bool success = false;
    bool usedVisualStudioCode = false;
    bool usedVisualStudio = false;
    std::string errorMessage;
};

// Small editor-only adapter for source locations reported by script backends.
// The executable path is supplied by project settings; no PATH lookup is done.
class ExternalCodeEditor final
{
public:
    ExternalCodeEditor() = delete;

    static std::vector<std::string> BuildVisualStudioCodeArguments(
        const std::filesystem::path& sourcePath,
        int line,
        int column);

    static ExternalCodeEditorResult Open(
        const std::filesystem::path& sourcePath,
        int line = 0,
        int column = 0,
        const std::filesystem::path& visualStudioCodePath = {});

    // Opens the generated managed project in the configured IDE. VS Code is
    // preferred when configured; otherwise the installed VS 2022 devenv.exe
    // is selected explicitly and reused when an instance is already running.
    static ExternalCodeEditorResult OpenProject(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& projectFile,
        const std::filesystem::path& visualStudioCodePath = {},
        const std::filesystem::path& sourcePath = {});

    static ExternalCodeEditorResult OpenWorkspace(
        const std::filesystem::path& workspaceRoot,
        const std::filesystem::path& visualStudioCodePath = {});

    // Opens the deterministic project workspace generated under
    // <Project>/Library/IDE and optionally opens the source in that same IDE.
    static ExternalCodeEditorResult OpenDebugWorkspace(
        const ProjectDebugManifest& manifest,
        const std::filesystem::path& sourcePath = {},
        const std::filesystem::path& visualStudioCodePath = {});
};
}
