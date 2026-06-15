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
        std::printf("  --editor-validation-focus-view <scene|game>  Focus a view during startup validation\n");
        std::printf("  --editor-validation-exclusive-view <scene|game>  Close the other view during startup validation\n");
        std::printf("  --editor-validation-open-frame-info  Open Frame Info during startup validation\n");
        std::printf("  --editor-validation-open-profiler  Open Profiler during startup validation\n");
        std::printf("  --editor-validation-trace-frames <N>  Export TimelineProfiler trace for N validation frames\n");
        std::printf("  --editor-validation-select-gameobject <name>  Select a GameObject during startup validation\n");
        std::printf("  --editor-validation-create-asset <path>  Create an asset instance during startup validation\n");
        std::printf("  --editor-validation-disable-hzb-occlusion  Disable HZB occlusion for A/B validation\n");
        std::printf("  --editor-validation-occlusion-stack <N>  Create N overlapping cubes for occlusion validation\n");
        std::printf("  --editor-validation-scene-readback-output <path>  Write Scene View render target PNG during validation\n");
        std::printf("  --editor-validation-scene-readback-summary <path>  Write Scene View readback summary during validation\n");
        std::printf("  --editor-validation-scene-camera <pos;rot>  Force Scene View camera, e.g. 1,2,3;10,20,30\n");
        std::printf("  --editor-log-render-draw-path  Log renderer draw/package diagnostics\n");
        std::printf("  --editor-log-scene-camera-input  Log Scene View input and camera movement diagnostics\n");
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
            else if (arg == "--editor-validation-scene-camera" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationSceneCamera = argv[++i];
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-focus-view" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationFocusView = argv[++i];
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-exclusive-view" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationExclusiveView = argv[++i];
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-open-frame-info")
            {
                parsed.diagnosticsSettings.editorValidationOpenFrameInfo = true;
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-open-profiler")
            {
                parsed.diagnosticsSettings.editorValidationOpenProfiler = true;
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-trace-frames" && i + 1 < argc)
            {
                try
                {
                    parsed.diagnosticsSettings.editorValidationTimelineTraceFrames =
                        static_cast<uint32_t>(std::stoul(argv[++i]));
                    parsed.diagnosticsSettings.editorValidationOpenProfiler = true;
                    parsed.hasDiagnosticsOverride = true;
                }
                catch (...)
                {
                    std::fprintf(stderr, "[main] Invalid value for --editor-validation-trace-frames: %s\n", argv[i]);
                    parsed.hasError = true;
                    return parsed;
                }
            }
            else if (arg == "--editor-validation-select-gameobject" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationSelectGameObject = argv[++i];
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-create-asset" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationCreateAsset = argv[++i];
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-disable-hzb-occlusion")
            {
                parsed.diagnosticsSettings.editorValidationDisableHZBOcclusion = true;
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-occlusion-stack" && i + 1 < argc)
            {
                try
                {
                    parsed.diagnosticsSettings.editorValidationOcclusionStackCount =
                        static_cast<uint32_t>(std::stoul(argv[++i]));
                    parsed.hasDiagnosticsOverride = true;
                }
                catch (...)
                {
                    std::fprintf(stderr, "[main] Invalid value for --editor-validation-occlusion-stack: %s\n", argv[i]);
                    parsed.hasError = true;
                    return parsed;
                }
            }
            else if (arg == "--editor-validation-scene-readback-output" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationSceneReadbackOutput = argv[++i];
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-validation-scene-readback-summary" && i + 1 < argc)
            {
                parsed.diagnosticsSettings.editorValidationSceneReadbackSummary = argv[++i];
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-log-scene-camera-input")
            {
                parsed.diagnosticsSettings.editorLogSceneCameraInput = true;
                parsed.hasDiagnosticsOverride = true;
            }
            else if (arg == "--editor-log-render-draw-path")
            {
                parsed.diagnosticsSettings.logRenderDrawPath = true;
                parsed.diagnosticsSettings.dx12LogFrameFlow = true;
                parsed.hasDiagnosticsOverride = true;
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
