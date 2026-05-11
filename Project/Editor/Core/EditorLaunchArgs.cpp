#include "Core/EditorLaunchArgs.h"

#include <cstdio>
#include <string>

#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace NLS::Editor::Launch
{
    void PrintUsage(const char* executableName)
    {
        std::printf("Usage: %s [options] [project_path]\n", executableName);
        std::printf("\nOptions:\n");
        std::printf("  --backend <name>, -b <name>  Specify graphics backend (dx12 only during phase 1)\n");
        std::printf("  --renderdoc                  Enable RenderDoc debugging\n");
        std::printf("  --no-renderdoc               Disable RenderDoc debugging\n");
        std::printf("  --capture-after-frames <N>   Automatically capture frame after N presents\n");
        std::printf("  --help, -h                  Show this help message\n");
        std::printf("\nArguments:\n");
        std::printf("  project_path   Path to .nullus project file or project directory (required)\n");
        std::printf("\nExamples:\n");
        std::printf("  %s MyProject.nullus                       # Open project with default backend\n", executableName);
        std::printf("  %s -b dx12 MyProject.nullus                # Open with DX12 backend\n", executableName);
        std::printf("  %s --renderdoc MyProject.nullus            # Enable RenderDoc and open project\n", executableName);
        std::printf("  %s --capture-after-frames 60 MyProject.nullus  # Auto-capture after 60 frames\n", executableName);
    }

    ParsedEditorLaunchArgs ParseEditorArgs(int argc, char** argv)
    {
        ParsedEditorLaunchArgs parsed;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if ((arg == "--backend" || arg == "-b") && i + 1 < argc)
            {
                const std::string backendName = argv[++i];
                parsed.backendOverride = Render::Settings::TryParseGraphicsBackend(backendName);
                if (!parsed.backendOverride.has_value())
                {
                    std::fprintf(stderr, "[main] Unknown graphics backend: %s\n", backendName.c_str());
                    std::fprintf(stderr, "[main] Supported backends during phase 1: dx12\n");
                    parsed.hasError = true;
                    return parsed;
                }

                if (const auto restriction =
                    Render::Settings::GetPhase1BackendRestrictionMessage(parsed.backendOverride.value(), "Editor CLI");
                    restriction.has_value())
                {
                    std::fprintf(stderr, "[main] %s\n", restriction->c_str());
                    parsed.hasError = true;
                    return parsed;
                }
            }
            else if (arg == "--renderdoc")
            {
                parsed.renderDocSettings.enabled = true;
                parsed.hasRenderDocOverride = true;
            }
            else if (arg == "--no-renderdoc")
            {
                parsed.renderDocSettings.enabled = false;
                parsed.hasRenderDocOverride = true;
            }
            else if (arg == "--capture-after-frames" && i + 1 < argc)
            {
                try
                {
                    const uint32_t frames = static_cast<uint32_t>(std::stoul(argv[++i]));
                    parsed.renderDocSettings.startupCaptureAfterFrames = frames;
                    parsed.renderDocSettings.enabled = true;
                    parsed.hasRenderDocOverride = true;
                }
                catch (...)
                {
                    std::fprintf(stderr, "[main] Invalid value for --capture-after-frames: %s\n", argv[i]);
                    parsed.hasError = true;
                    return parsed;
                }
            }
            else if (arg == "--help" || arg == "-h")
            {
                parsed.showHelp = true;
                return parsed;
            }
            else if (arg.rfind("-", 0) != 0)
            {
                parsed.projectPathArgument = arg;
            }
            else
            {
                std::fprintf(stderr, "[main] Unknown option: %s\n", arg.c_str());
                parsed.hasError = true;
                return parsed;
            }
        }
        return parsed;
    }
}
