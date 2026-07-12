#include "Windowing/Window.h"
#include <filesystem>
#include "Utils/PathParser.h"
#include "Core/Utils/String.h"
#include "Core/Application.h"
#include "Core/EditorLaunchArgs.h"
#include "Debug/Logger.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
using namespace NLS;

int main(int argc, char** argv);
static bool TryRun(const std::string& projectPath, const std::string& projectName,
                  std::optional<Render::Settings::EGraphicsBackend> backendOverride,
                  const std::optional<Render::Settings::RenderDocSettings>& renderDocOverride,
                  const std::optional<Render::Settings::EngineDiagnosticsSettings>& diagnosticsOverride,
                  std::chrono::steady_clock::time_point launchBegin);

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
	const auto launchBegin = std::chrono::steady_clock::now();
	const auto launchWorkingDirectory = std::filesystem::current_path();
	std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

	bool ready = false;
	std::string projectPath;
	std::string projectName;

	const auto launchArgs = Editor::Launch::ParseEditorArgs(argc, argv);
	if (launchArgs.hasError)
		return EXIT_FAILURE;
	if (launchArgs.showHelp)
	{
		Editor::Launch::PrintUsage(argv[0]);
		return EXIT_SUCCESS;
	}

	if (launchArgs.projectPathArgument.has_value())
	{
		std::filesystem::path projPath = ResolveLaunchPathArgument(launchWorkingDirectory, launchArgs.projectPathArgument.value());
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
				projFile = projPath.string();
		}
		else if (projPath.extension() == ".nullus")
		{
			projFile = projPath.string();
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

	if (!ready)
	{
		std::fprintf(stderr, "No project specified. Launch Editor through Launcher.exe or provide a project path as argument.\n");
		return EXIT_FAILURE;
	}

	const bool ran = TryRun(
		projectPath,
		projectName,
		launchArgs.backendOverride,
		launchArgs.hasRenderDocOverride
			? std::optional<Render::Settings::RenderDocSettings>(launchArgs.renderDocSettings)
			: std::nullopt,
		launchArgs.hasDiagnosticsOverride
			? std::optional<Render::Settings::EngineDiagnosticsSettings>(launchArgs.diagnosticsSettings)
			: std::nullopt,
		launchBegin);
	return ran ? EXIT_SUCCESS : EXIT_FAILURE;
}

static bool TryRun(const std::string& projectPath, const std::string& projectName,
                   std::optional<Render::Settings::EGraphicsBackend> backendOverride,
                   const std::optional<Render::Settings::RenderDocSettings>& renderDocOverride,
                   const std::optional<Render::Settings::EngineDiagnosticsSettings>& diagnosticsOverride,
                   const std::chrono::steady_clock::time_point launchBegin)
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
		app = std::make_unique<Editor::Core::Application>(
			projectPath,
			projectName,
			backendOverride,
			renderDocOverride,
			diagnosticsOverride);
		if (app->DidShowEditorWindow())
		{
			NLS_LOG_INFO(
				"[Startup] EditorWindowShown elapsedMs=" +
				std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - launchBegin).count()));
		}
		NLS::Context::Device::ErrorEvent -= listenerId;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[Editor::TryRun] std::exception: %s\n", e.what());
		return false;
	}
	catch (...)
	{
		std::fprintf(stderr, "[Editor::TryRun] unknown exception\n");
		return false;
	}

	if (app)
	{
		app->Run();
		return true;
	}
	return false;
}
