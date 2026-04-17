#include "Core/Launcher.h"
#include "Core/LauncherProjectMetadata.h"

#include <filesystem>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <Platform/Process/Process.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>
#include <Utils/PathParser.h>

using namespace NLS;

static std::optional<Render::Settings::EGraphicsBackend> backendOverride;
static Render::Settings::RenderDocSettings renderDocSettings;

static void LaunchEditor(const std::string& projectPath, const std::string& preferredEditorExecutablePath)
{
    std::string editorPath = preferredEditorExecutablePath;
    if (!editorPath.empty() && !std::filesystem::exists(editorPath))
        editorPath.clear();

    std::string editorName =
#ifdef _WIN32
        "Editor.exe";
#else
        "Editor";
#endif
    if (editorPath.empty())
    {
        auto resolvedEditorPath = Platform::Process::FindExecutable(editorName);
        if (resolvedEditorPath.has_value())
            editorPath = resolvedEditorPath->string();
    }

    if (editorPath.empty())
    {
        std::fprintf(stderr, "[Launcher] Editor executable not found: %s\n", editorName.c_str());
        return;
    }

    WriteProjectLastEditorExecutable(projectPath, editorPath);

    // Build command line arguments per launcher-editor-cli.md contract
    std::vector<std::string> args;

    if (backendOverride.has_value())
    {
        args.push_back("--backend");
        args.push_back(Render::Settings::ToString(backendOverride.value()));
    }

    if (renderDocSettings.enabled)
        args.push_back("--renderdoc");
    else
        args.push_back("--no-renderdoc");

    if (renderDocSettings.startupCaptureAfterFrames > 0)
    {
        args.push_back("--capture-after-frames");
        args.push_back(std::to_string(renderDocSettings.startupCaptureAfterFrames));
    }

    // Project path as positional argument (use .nullus file if available)
    std::string targetPath = projectPath;
    if (const auto settingsFilePath = ResolveProjectSettingsFilePath(projectPath); !settingsFilePath.empty())
        targetPath = settingsFilePath.string();
    args.push_back(targetPath);

    auto result = Platform::Process::Launch(editorPath, args);
    if (!result.success)
    {
        std::fprintf(stderr, "[Launcher] Failed to launch Editor: %s\n", result.errorMessage.c_str());
    }
}

int main(int argc, char** argv)
{
    std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

    // Parse command-line arguments (same flags as Editor, minus project_path)
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if ((arg == "--backend" || arg == "-b") && i + 1 < argc)
        {
            std::string backendName = argv[++i];
            backendOverride = Render::Settings::TryParseGraphicsBackend(backendName);
            if (!backendOverride.has_value())
            {
                std::fprintf(stderr, "[Launcher] Unknown graphics backend: %s\n", backendName.c_str());
                std::fprintf(stderr, "[Launcher] Supported backends: dx12, vulkan, opengl, dx11, metal\n");
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--renderdoc")
        {
            renderDocSettings.enabled = true;
        }
        else if (arg == "--no-renderdoc")
        {
            renderDocSettings.enabled = false;
        }
        else if (arg == "--capture-after-frames" && i + 1 < argc)
        {
            try
            {
                uint32_t frames = static_cast<uint32_t>(std::stoul(argv[++i]));
                renderDocSettings.startupCaptureAfterFrames = frames;
                renderDocSettings.enabled = true;
            }
            catch (...)
            {
                std::fprintf(stderr, "[Launcher] Invalid value for --capture-after-frames: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::printf("Usage: %s [options]\n", argv[0]);
            std::printf("\nOptions:\n");
            std::printf("  --backend <name>, -b <name>  Default graphics backend for launched Editor\n");
            std::printf("  --renderdoc                  Enable RenderDoc debugging\n");
            std::printf("  --no-renderdoc               Disable RenderDoc debugging\n");
            std::printf("  --capture-after-frames <N>   Auto capture after N presents\n");
            std::printf("  --help, -h                  Show this help message\n");
            return EXIT_SUCCESS;
        }
    }

    // Run Launcher UI
    Launcher launcher(backendOverride, renderDocSettings);
    const auto result = launcher.Run();

    if (result.ready)
    {
        LaunchEditor(result.projectPath, result.editorExecutablePath);
    }

    return EXIT_SUCCESS;
}
