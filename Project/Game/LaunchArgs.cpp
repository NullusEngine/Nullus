#include "LaunchArgs.h"

#include <cstdio>
#include <string>

#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace NLS::Game::Launch
{
	void PrintUsage(const char* executableName)
	{
		std::printf("Usage: %s [options] [project_path]\n", executableName);
		std::printf("\nOptions:\n");
		std::printf("  --backend <name>, -b <name>  Specify graphics backend (dx12, vulkan, opengl, dx11, metal)\n");
		std::printf("  --renderdoc                  Enable RenderDoc debugging\n");
		std::printf("  --no-renderdoc               Disable RenderDoc debugging\n");
		std::printf("  --capture-after-frames <N>  Automatically capture frame after N presents\n");
		std::printf("  --help, -h                  Show this help message\n");
		std::printf("\nArguments:\n");
		std::printf("  project_path   Path to .nullus project file or project directory\n");
		std::printf("\nExamples:\n");
		std::printf("  %s TestProject.nullus\n", executableName);
		std::printf("  %s --backend vulkan TestProject.nullus\n", executableName);
		std::printf("  %s --backend dx12 TestProject\n", executableName);
	}

	ParsedGameLaunchArgs ParseGameArgs(int argc, char** argv)
	{
		ParsedGameLaunchArgs parsed;
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			if ((arg == "--backend" || arg == "-b") && i + 1 < argc)
			{
				const std::string backendName = argv[++i];
				parsed.backendOverride = Render::Settings::TryParseGraphicsBackend(backendName);
				if (!parsed.backendOverride.has_value())
				{
					std::fprintf(stderr, "[Game main] Unknown graphics backend: %s\n", backendName.c_str());
					std::fprintf(stderr, "[Game main] Supported backends: dx12, vulkan, opengl, dx11, metal\n");
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
					std::fprintf(stderr, "[Game main] Invalid value for --capture-after-frames: %s\n", argv[i]);
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
				parsed.projectPathOverride = arg;
			}
			else
			{
				std::fprintf(stderr, "[Game main] Unknown option: %s\n", arg.c_str());
				parsed.hasError = true;
				return parsed;
			}
		}

		return parsed;
	}
}
