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
                  const Render::Settings::RenderDocSettings& renderDocSettings,
                  bool enableThreadedRendering,
                  const Render::Settings::EngineDiagnosticsSettings& diagnosticsSettings);

namespace
{
	std::filesystem::path ResolveLaunchPathArgument(
		const std::filesystem::path& launchWorkingDirectory,
		const std::string& argument)
	{
		std::filesystem::path path(argument);
		if (path.is_relative())
			path = launchWorkingDirectory / path;

		std::error_code error;
		const auto canonicalPath = std::filesystem::weakly_canonical(path, error);
		return error ? path : canonicalPath;
	}
}

int main(int argc, char** argv)
{
	const auto launchWorkingDirectory = std::filesystem::current_path();
	std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

	bool ready = false;
	std::string projectPath;
	std::string projectName;
	std::optional<Render::Settings::EGraphicsBackend> backendOverride;
	Render::Settings::RenderDocSettings renderDocSettings;
	bool enableThreadedRendering = true;
	Render::Settings::EngineDiagnosticsSettings diagnosticsSettings;

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
				std::fprintf(stderr, "[main] Supported backends during phase 1: dx12\n");
				return EXIT_FAILURE;
			}

			if (const auto restriction =
				Render::Settings::GetPhase1BackendRestrictionMessage(backendOverride.value(), "Editor CLI");
				restriction.has_value())
			{
				std::fprintf(stderr, "[main] %s\n", restriction->c_str());
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
		else if (arg == "--threaded-rendering")
		{
			enableThreadedRendering = true;
		}
		else if (arg == "--log-render-draw-path")
		{
			diagnosticsSettings.logRenderDrawPath = true;
		}
		else if (arg == "--diag-skip-skybox")
		{
			diagnosticsSettings.diagSkipSkyboxDraw = true;
		}
		else if (arg == "--log-material-bindings")
		{
			diagnosticsSettings.logMaterialBindings = true;
		}
		else if (arg == "--dx12-log-messages")
		{
			diagnosticsSettings.dx12LogMessages = true;
		}
		else if (arg == "--dx12-log-frame-flow")
		{
			diagnosticsSettings.dx12LogFrameFlow = true;
		}
		else if (arg == "--editor-disable-grid-pass")
		{
			diagnosticsSettings.editorDisableGridPass = true;
		}
		else if (arg == "--editor-disable-debug-cameras-pass")
		{
			diagnosticsSettings.editorDisableDebugCamerasPass = true;
		}
		else if (arg == "--editor-disable-debug-lights-pass")
		{
			diagnosticsSettings.editorDisableDebugLightsPass = true;
		}
		else if (arg == "--editor-disable-debug-actor-pass")
		{
			diagnosticsSettings.editorDisableDebugActorPass = true;
		}
		else if (arg == "--editor-disable-debug-draw-pass")
		{
			diagnosticsSettings.editorDisableDebugDrawPass = true;
		}
		else if (arg == "--editor-disable-picking-pass")
		{
			diagnosticsSettings.editorDisablePickingPass = true;
		}
		else if (arg == "--editor-validation-focus-view" && i + 1 < argc)
		{
			diagnosticsSettings.editorValidationFocusView = argv[++i];
		}
		else if (arg == "--editor-validation-exclusive-view" && i + 1 < argc)
		{
			diagnosticsSettings.editorValidationExclusiveView = argv[++i];
		}
		else if (arg == "--editor-validation-select-actor" && i + 1 < argc)
		{
			diagnosticsSettings.editorValidationSelectActor = argv[++i];
		}
		else if (arg == "--editor-grid-skip-plane")
		{
			diagnosticsSettings.editorGridSkipPlane = true;
		}
		else if (arg == "--editor-grid-skip-axes")
		{
			diagnosticsSettings.editorGridSkipAxes = true;
		}
		else if (arg == "--help" || arg == "-h")
		{
			std::printf("Usage: %s [options] [project_path]\n", argv[0]);
			std::printf("\nOptions:\n");
			std::printf("  --backend <name>, -b <name>  Specify graphics backend (dx12 only during phase 1)\n");
			std::printf("  --renderdoc                  Enable RenderDoc debugging\n");
			std::printf("  --no-renderdoc               Disable RenderDoc debugging\n");
			std::printf("  --capture-after-frames <N>   Automatically capture frame after N presents\n");
			std::printf("  --threaded-rendering         Threaded rendering is the default DX12 mainline\n");
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
			std::printf("  %s MyProject.nullus                       # Open project with default backend\n", argv[0]);
			std::printf("  %s -b dx12 MyProject.nullus                # Open with DX12 backend\n", argv[0]);
			std::printf("  %s --renderdoc MyProject.nullus            # Enable RenderDoc and open project\n", argv[0]);
			std::printf("  %s --capture-after-frames 60 MyProject.nullus  # Auto-capture after 60 frames\n", argv[0]);
			return EXIT_SUCCESS;
		}
		else if (arg.rfind("-", 0) != 0)
		{
			// Not an option, treat as project file or directory
			std::filesystem::path projPath = ResolveLaunchPathArgument(launchWorkingDirectory, arg);
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
				projFile = projPath.string();
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
		TryRun(projectPath, projectName, backendOverride, renderDocSettings, enableThreadedRendering, diagnosticsSettings);
	return EXIT_SUCCESS;
}

static void TryRun(const std::string& projectPath, const std::string& projectName,
                   std::optional<Render::Settings::EGraphicsBackend> backendOverride,
                   const Render::Settings::RenderDocSettings& renderDocSettings,
                   bool enableThreadedRendering,
                   const Render::Settings::EngineDiagnosticsSettings& diagnosticsSettings)
{
	auto errorEvent =
		[](NLS::EDeviceError, std::string errMsg)
		{
			errMsg = "Nullus phase-1 editor runtime requires a working DX12 path.\r\n" + errMsg;
			NLS_LOG_ERROR(errMsg);
		};

	std::unique_ptr<Editor::Core::Application> app;

	try
	{
		auto listenerId = NLS::Context::Device::ErrorEvent += errorEvent;
		app = std::make_unique<Editor::Core::Application>(projectPath, projectName, backendOverride, renderDocSettings, enableThreadedRendering, diagnosticsSettings);
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
