#include "Core/Application.h"
#include "LaunchArgs.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "RuntimeAssetManifestStartup.h"
#include "Platform/Process/Process.h"

#include "Assets/NpakArchive.h"
#include "Engine/Assets/PlayerDescriptor.h"
#include "Version.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace NLS;

namespace
{
	int RunPackageSelfTest()
	{
		const auto installRoots = NLS::Platform::Process::ResolveInstallResourceRoots();
		const auto packaged = NLS::Game::RuntimeAssets::DetectPackagedRuntime(installRoots.installRoot);
		if (!packaged.has_value())
		{
			std::fprintf(stderr, "[package-self-test] No packaged runtime found at '%s'.\n",
				installRoots.installRoot.string().c_str());
			return 2;
		}

		std::string error;
		if (!NLS::Game::RuntimeAssets::ValidatePackagedRuntimeCompatibility(*packaged, &error))
		{
			std::fprintf(stderr, "[package-self-test] Player/package compatibility failed: %s\n", error.c_str());
			return 3;
		}

		for (const auto& packagePath : packaged->packages)
		{
			auto archive = NLS::Core::Assets::NpakArchive::Open(packagePath, &error);
			if (!archive.has_value())
			{
				std::fprintf(stderr, "[package-self-test] %s\n", error.c_str());
				return 4;
			}
			if (archive->GetInfo().formatVersion != NLS::Core::Assets::NpakFormatVersion)
			{
				std::fprintf(stderr, "[package-self-test] Unsupported container version in '%s'.\n",
					packagePath.string().c_str());
				return 5;
			}
			if (archive->GetEntries().empty())
			{
				std::fprintf(stderr, "[package-self-test] Package '%s' contains no entries.\n",
					packagePath.string().c_str());
				return 6;
			}
		}

		const auto settingsPath = packaged->contentRoot / "Game.ini";
		auto database = NLS::Game::RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(
			settingsPath, &*packaged);
		if (!database.has_value())
		{
			std::fprintf(stderr, "[package-self-test] The runtime asset manifest could not be loaded.\n");
			return 7;
		}

		const auto startScene = packaged->contentRoot / "Assets" /
			std::filesystem::path(packaged->descriptor.startScene);
		if (!std::filesystem::is_regular_file(startScene))
		{
			std::fprintf(stderr, "[package-self-test] The start scene is missing: %s\n",
				startScene.string().c_str());
			return 8;
		}

		const auto& scripts = packaged->descriptor.scripts;
		for (const auto& scriptPath : {
			scripts.managedRuntimeConfig,
			scripts.managedAssembly,
			scripts.scriptApiDirectory})
		{
			if (!scriptPath.empty() &&
				!std::filesystem::exists(packaged->installRoot / std::filesystem::path(scriptPath)))
			{
				std::fprintf(stderr, "[package-self-test] Script payload is missing: %s\n",
					scriptPath.c_str());
				return 9;
			}
		}

		std::printf(
			"[package-self-test] OK: %zu package(s), %zu manifest entries, start scene '%s'.\n",
			packaged->packages.size(),
			database->GetManifest().entries.size(),
			packaged->descriptor.startScene.c_str());
		return 0;
	}

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

	void ApplyPackagedWorkingDirectory()
	{
		// Installed players keep their working directory at the executable's
		// install root. This makes relative runtime paths stable regardless of
		// where the shell launched the process from, and it keeps content
		// resolution independent from the caller's cwd.
		const auto installRoots = NLS::Platform::Process::ResolveInstallResourceRoots();
		std::error_code error;
		if (!installRoots.installRoot.empty() &&
			std::filesystem::is_regular_file(installRoots.installRoot / "Player.info", error))
		{
			std::filesystem::current_path(installRoots.installRoot, error);
			if (error)
			{
				std::fprintf(
					stderr,
					"[Game main] failed to enter install root '%s': %s\n",
					installRoots.installRoot.string().c_str(),
					error.message().c_str());
			}
		}
	}
}

int main(int argc, char** argv)
{
		const Render::Context::ScopedGameThread gameThread;
	const auto launchWorkingDirectory = std::filesystem::current_path();
	std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());
	ApplyPackagedWorkingDirectory();

	for (int index = 1; index < argc; ++index)
	{
		if (std::strcmp(argv[index], "--package-self-test") == 0)
			return RunPackageSelfTest();
	}

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
			launchArgs.hasRenderDocOverride
				? std::optional<Render::Settings::RenderDocSettings>(launchArgs.renderDocSettings)
				: std::nullopt,
			launchArgs.backendOverride,
			launchArgs.projectPathOverride,
			launchArgs.materialValidation);
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
