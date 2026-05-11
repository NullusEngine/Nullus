#pragma once

#include <optional>
#include <string>

#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"

namespace NLS::Editor::Launch
{
    struct ParsedEditorLaunchArgs
    {
        Render::Settings::RenderDocSettings renderDocSettings;
        std::optional<Render::Settings::EGraphicsBackend> backendOverride;
        std::optional<std::string> projectPathArgument;
        bool hasRenderDocOverride = false;
        bool showHelp = false;
        bool hasError = false;
    };

    void PrintUsage(const char* executableName);
    ParsedEditorLaunchArgs ParseEditorArgs(int argc, char** argv);
}
