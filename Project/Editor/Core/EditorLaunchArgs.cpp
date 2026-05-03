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
        std::printf("  --threaded-rendering         Threaded rendering is the default DX12 mainline\n");
        std::printf("  --editor-performance-mode    Disable expensive RHI debug validation for FPS profiling\n");
        std::printf("  --no-rhi-debug-validation    Disable RHI debug validation while keeping other diagnostics unchanged\n");
        std::printf("  --log-render-draw-path      Enable render draw path logging\n");
        std::printf("  --diag-skip-skybox          Skip skybox draw for diagnostics\n");
        std::printf("  --log-material-bindings      Enable material bindings logging\n");
        std::printf("  --dx12-log-messages          Enable DX12 message logging\n");
        std::printf("  --dx12-log-frame-flow       Enable DX12 frame flow logging\n");
        std::printf("  --editor-disable-grid-pass   Disable editor grid pass\n");
        std::printf("  --editor-disable-debug-cameras-pass  Disable debug cameras pass\n");
        std::printf("  --editor-disable-debug-lights-pass  Disable debug lights pass\n");
        std::printf("  --editor-disable-debug-actor-pass   Disable debug actor pass\n");
        std::printf("  --editor-disable-debug-draw-pass    Disable debug draw pass\n");
        std::printf("  --editor-disable-picking-pass     Disable picking pass\n");
        std::printf("  --editor-validation-focus-view <scene|game>  Focus a validation target view after startup\n");
        std::printf("  --editor-validation-exclusive-view <scene|game>  Close the other main viewport for isolated validation\n");
        std::printf("  --editor-validation-select-actor <name>      Select an actor by name after startup\n");
        std::printf("  --editor-grid-skip-plane    Skip editor grid plane mesh\n");
        std::printf("  --editor-grid-skip-axes      Skip editor grid axes lines\n");
        std::printf("  --help, -h                 Show this help message\n");
        std::printf("\nArguments:\n");
        std::printf("  project_path   Path to .nullus project file or project directory (required)\n");
        std::printf("\nExamples:\n");
        std::printf("  %s MyProject.nullus                       # Open project with default backend\n", executableName);
        std::printf("  %s -b dx12 MyProject.nullus                # Open with DX12 backend\n", executableName);
        std::printf("  %s --editor-performance-mode MyProject.nullus  # Profile FPS without DX12 GPU validation\n", executableName);
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
            }
            else if (arg == "--no-renderdoc")
            {
                parsed.renderDocSettings.enabled = false;
            }
            else if (arg == "--capture-after-frames" && i + 1 < argc)
            {
                try
                {
                    const uint32_t frames = static_cast<uint32_t>(std::stoul(argv[++i]));
                    parsed.renderDocSettings.startupCaptureAfterFrames = frames;
                    parsed.renderDocSettings.enabled = true;
                }
                catch (...)
                {
                    std::fprintf(stderr, "[main] Invalid value for --capture-after-frames: %s\n", argv[i]);
                    parsed.hasError = true;
                    return parsed;
                }
            }
            else if (arg == "--threaded-rendering")
            {
                parsed.enableThreadedRendering = true;
            }
            else if (arg == "--editor-performance-mode")
            {
                parsed.enablePerformanceMode = true;
                parsed.enableRhiDebugValidation = false;
            }
            else if (arg == "--no-rhi-debug-validation")
            {
                parsed.enableRhiDebugValidation = false;
            }
            else if (arg == "--log-render-draw-path")
            {
                parsed.diagnosticsSettings.logRenderDrawPath = true;
            }
            else if (arg == "--diag-skip-skybox")
            {
                parsed.diagnosticsSettings.diagSkipSkyboxDraw = true;
            }
            else if (arg == "--log-material-bindings")
            {
                parsed.diagnosticsSettings.logMaterialBindings = true;
            }
            else if (arg == "--dx12-log-messages")
            {
                parsed.diagnosticsSettings.dx12LogMessages = true;
            }
            else if (arg == "--dx12-log-frame-flow")
            {
                parsed.diagnosticsSettings.dx12LogFrameFlow = true;
            }
            else if (arg == "--editor-disable-grid-pass")
            {
                parsed.diagnosticsSettings.editorDisableGridPass = true;
            }
            else if (arg == "--editor-disable-debug-cameras-pass")
            {
                parsed.diagnosticsSettings.editorDisableDebugCamerasPass = true;
            }
            else if (arg == "--editor-disable-debug-lights-pass")
            {
                parsed.diagnosticsSettings.editorDisableDebugLightsPass = true;
            }
            else if (arg == "--editor-disable-debug-actor-pass")
            {
                parsed.diagnosticsSettings.editorDisableDebugActorPass = true;
            }
            else if (arg == "--editor-disable-debug-draw-pass")
            {
                parsed.diagnosticsSettings.editorDisableDebugDrawPass = true;
            }
            else if (arg == "--editor-disable-picking-pass")
            {
                parsed.diagnosticsSettings.editorDisablePickingPass = true;
            }
            else if (arg == "--editor-validation-focus-view" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationFocusView = argv[++i];
            }
            else if (arg == "--editor-validation-exclusive-view" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationExclusiveView = argv[++i];
            }
            else if (arg == "--editor-validation-select-actor" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationSelectActor = argv[++i];
            }
            else if (arg == "--editor-grid-skip-plane")
            {
                parsed.diagnosticsSettings.editorGridSkipPlane = true;
            }
            else if (arg == "--editor-grid-skip-axes")
            {
                parsed.diagnosticsSettings.editorGridSkipAxes = true;
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
        }
        return parsed;
    }
}
