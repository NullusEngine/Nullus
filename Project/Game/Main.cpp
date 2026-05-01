#include "Core/Application.h"
#include "LaunchArgs.h"

#include <cstdio>
#include <filesystem>
#include <string>

using namespace NLS;

namespace
{
	std::string ResolveLaunchPathArgument(
		const std::filesystem::path& launchWorkingDirectory,
		const std::string& argument)
	{
		std::filesystem::path path(argument);
		if (path.is_relative())
			path = launchWorkingDirectory / path;

		std::error_code error;
		const auto canonicalPath = std::filesystem::weakly_canonical(path, error);
		return (error ? path : canonicalPath).string();
	}
}

int main(int argc, char** argv)
{
	const auto launchWorkingDirectory = std::filesystem::current_path();
	std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

	NLS::Game::Launch::ParsedGameLaunchArgs launchArgs = NLS::Game::Launch::ParseGameArgs(argc, argv);
	if (launchArgs.showHelp)
	{
		NLS::Game::Launch::PrintUsage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (launchArgs.hasError)
		return EXIT_FAILURE;
	if (launchArgs.projectPathOverride.has_value())
	{
		launchArgs.projectPathOverride = ResolveLaunchPathArgument(
			launchWorkingDirectory,
			launchArgs.projectPathOverride.value());
	}

	try
	{
		Game::Core::Application app(
			launchArgs.renderDocSettings,
			launchArgs.backendOverride,
			launchArgs.projectPathOverride,
			launchArgs.enableThreadedRendering,
			launchArgs.diagnosticsSettings);
		app.Run();
		return EXIT_SUCCESS;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "[Game main] std::exception: %s\n", e.what());
		return EXIT_FAILURE;
	}
	catch (...)
	{
		std::fprintf(stderr, "[Game main] unknown exception\n");
		return EXIT_FAILURE;
	}
}
