#include "Core/Application.h"
#include "LaunchArgs.h"

#include <cstdio>
#include <filesystem>

using namespace NLS;

int main(int argc, char** argv)
{
	std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

	const NLS::Game::Launch::ParsedGameLaunchArgs launchArgs = NLS::Game::Launch::ParseGameArgs(argc, argv);
	if (launchArgs.showHelp)
	{
		NLS::Game::Launch::PrintUsage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (launchArgs.hasError)
		return EXIT_FAILURE;

	try
	{
		Game::Core::Application app(
			launchArgs.renderDocSettings,
			launchArgs.backendOverride,
			launchArgs.projectPathOverride);
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
