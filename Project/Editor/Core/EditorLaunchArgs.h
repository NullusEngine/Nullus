#pragma once

#include <optional>
#include <string>

#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"

namespace NLS::Editor::Launch
{
    enum class StartupResourcePipeline
    {
        Async,
        Legacy,
        Pipelined
    };

    constexpr StartupResourcePipeline DefaultStartupResourcePipeline() noexcept
    {
#if defined(_WIN32)
        // Bounded artifact parsing plus synchronous resource creation keeps the
        // startup ready gate intact without per-frame DX12 upload overhead.
        return StartupResourcePipeline::Pipelined;
#else
        return StartupResourcePipeline::Async;
#endif
    }

    struct ParsedEditorLaunchArgs
    {
        Render::Settings::RenderDocSettings renderDocSettings;
        Render::Settings::EngineDiagnosticsSettings diagnosticsSettings;
        std::optional<Render::Settings::EGraphicsBackend> backendOverride;
        std::optional<std::string> projectPathArgument;
        StartupResourcePipeline startupResourcePipeline = DefaultStartupResourcePipeline();
        bool prewarmAssetsOnly = false;
        bool dx12DebugLayer = false;
        bool hasRenderDocOverride = false;
        bool hasDiagnosticsOverride = false;
        bool showHelp = false;
        bool hasError = false;
    };

    void PrintUsage(const char* executableName);
    ParsedEditorLaunchArgs ParseEditorArgs(int argc, char** argv);
}
