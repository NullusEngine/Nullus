#include "Windowing/Window.h"
#include <filesystem>
#include "Utils/PathParser.h"
#include "Core/Utils/String.h"
#include "Core/Application.h"
#include "Debug/Logger.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include <cstdio>
#include <exception>
#include <optional>
using namespace NLS;

int main(int argc, char** argv);
static void TryRun(const std::string& projectPath, const std::string& projectName,
                  std::optional<Render::Settings::EGraphicsBackend> backendOverride,
                  const Render::Settings::RenderDocSettings& renderDocSettings);

int main(int argc, char** argv)
{
	std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

	bool ready = false;
	std::string projectPath;
	std::string projectName;
	std::optional<Render::Settings::EGraphicsBackend> backendOverride;
	Render::Settings::RenderDocSettings renderDocSettings;

	// Parse command-line arguments
	for (int i = 1; i < argc; ++i)
	{
		std::string arg = argv[i];

		// Check for --backend <name> or -b <name>
		if ((arg == "--backend" || arg == "-b") && i + 1 < argc)
		{
			std::string backendName = argv[++i];
			backendOverride = Render::Settings::TryParseGraphicsBackend(backendName);
			if (!backendOverride.has_value())
			{
				std::fprintf(stderr, "[main] Unknown graphics backend: %s\n", backendName.c_str());
				std::fprintf(stderr, "[main] Supported backends: dx12, vulkan, opengl, dx11, metal\n");
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
				std::fprintf(stderr, "[main] Invalid value for --capture-after-frames: %s\n", argv[i]);
				return EXIT_FAILURE;
			}
		}
		else if (arg == "--help" || arg == "-h")
		{
			std::printf("Usage: %s [options] [project_path]\n", argv[0]);
			std::printf("\nOptions:\n");
			std::printf("  --backend <name>, -b <name>  Specify graphics backend (dx12, vulkan, opengl, dx11, metal)\n");
			std::printf("  --renderdoc                  Enable RenderDoc debugging\n");
			std::printf("  --no-renderdoc               Disable RenderDoc debugging\n");
			std::printf("  --capture-after-frames <N>   Automatically capture frame after N presents\n");
			std::printf("  --help, -h                  Show this help message\n");
			std::printf("\nArguments:\n");
			std::printf("  project_path   Path to .nullus project file or project directory (required)\n");
			std::printf("\nExamples:\n");
			std::printf("  %s MyProject.nullus                       # Open project with default backend\n", argv[0]);
			std::printf("  %s --backend vulkan MyProject.nullus       # Open project with Vulkan backend\n", argv[0]);
			std::printf("  %s -b dx12 MyProject.nullus                # Open with DX12 backend\n", argv[0]);
			std::printf("  %s --renderdoc MyProject.nullus            # Enable RenderDoc and open project\n", argv[0]);
			std::printf("  %s --capture-after-frames 60 MyProject.nullus  # Auto-capture after 60 frames\n", argv[0]);
			return EXIT_SUCCESS;
		}
		else if (arg.rfind("-", 0) != 0)
		{
			// Not an option, treat as project file or directory
			std::filesystem::path projPath(arg);
			std::string projFile;

			if (std::filesystem::is_directory(projPath))
			{
				// Directory provided - look for .nullus file inside
				for (const auto& entry : std::filesystem::directory_iterator(projPath))
				{
					if (entry.is_regular_file() && entry.path().extension() == ".nullus")
					{
						projFile = entry.path().string();
						break;
					}
				}
				if (projFile.empty())
				{
					// No .nullus file found, use directory as project path
					projFile = projPath.string();
				}
			}
			else if (projPath.extension() == ".nullus")
			{
				projFile = arg;
			}
			else
			{
				// Not a .nullus file and not a directory - skip
				continue;
			}

			if (!projFile.empty())
			{
				ready = true;
				projectPath = std::filesystem::path(projFile).parent_path().string();
				projectName = Utils::PathParser::GetElementName(projFile);
				if (projectName.ends_with(".nullus"))
					projectName.erase(projectName.size() - 7);
			}
		}
	}

	if (!ready)
	{
		std::fprintf(stderr, "No project specified. Launch Editor through Launcher.exe or provide a project path as argument.\n");
		return EXIT_FAILURE;
	}

	if (ready)
		TryRun(projectPath, projectName, backendOverride, renderDocSettings);
	return EXIT_SUCCESS;
}

static void TryRun(const std::string& projectPath, const std::string& projectName,
                   std::optional<Render::Settings::EGraphicsBackend> backendOverride,
                   const Render::Settings::RenderDocSettings& renderDocSettings)
{
	auto errorEvent =
		[](NLS::EDeviceError, std::string errMsg)
		{
			errMsg = "Nullus requires OpenGL 4.3 or newer.\r\n" + errMsg;
			NLS_LOG_ERROR(errMsg);
		};

	std::unique_ptr<Editor::Core::Application> app;

	try
	{
		auto listenerId = NLS::Context::Device::ErrorEvent += errorEvent;
		app = std::make_unique<Editor::Core::Application>(projectPath, projectName, backendOverride, renderDocSettings);
		NLS::Context::Device::ErrorEvent -= listenerId;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[Editor::TryRun] std::exception: %s\n", e.what());
	}
	catch (...)
	{
		std::fprintf(stderr, "[Editor::TryRun] unknown exception\n");
	}

	if (app)
		app->Run();
}
