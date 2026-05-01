#include <gtest/gtest.h>

#include "LaunchArgs.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace
{
	char** MutableArgv(const std::initializer_list<const char*> args, std::vector<std::string>& storage)
	{
		storage.assign(args.begin(), args.end());
		static std::vector<char*> argv;
		argv.clear();
		for (std::string& value : storage)
			argv.push_back(value.data());
		return argv.data();
	}
}

TEST(GameLaunchArgsTests, RejectsNonDx12BackendOverrideDuringPhase1)
{
	std::vector<std::string> storage;
	char** argv = MutableArgv({"Game.exe", "--backend", "vulkan", "TestProject.nullus"}, storage);

	const auto parsed = NLS::Game::Launch::ParseGameArgs(static_cast<int>(storage.size()), argv);

	EXPECT_TRUE(parsed.hasError);
	EXPECT_FALSE(parsed.showHelp);
}

TEST(GameLaunchArgsTests, ParsesShortBackendFlagAndRenderDocCaptureOptions)
{
	std::vector<std::string> storage;
	char** argv = MutableArgv({"Game.exe", "-b", "dx12", "--capture-after-frames", "42", "TestProject"}, storage);

	const auto parsed = NLS::Game::Launch::ParseGameArgs(static_cast<int>(storage.size()), argv);

	if (NLS::Render::Settings::IsBackendSelectableForPhase1(NLS::Render::Settings::EGraphicsBackend::DX12))
		EXPECT_FALSE(parsed.hasError);
	else
		EXPECT_TRUE(parsed.hasError);
	ASSERT_TRUE(parsed.backendOverride.has_value());
	EXPECT_EQ(parsed.backendOverride.value(), NLS::Render::Settings::EGraphicsBackend::DX12);

	if (NLS::Render::Settings::IsBackendSelectableForPhase1(NLS::Render::Settings::EGraphicsBackend::DX12))
	{
		EXPECT_TRUE(parsed.renderDocSettings.enabled);
		EXPECT_EQ(parsed.renderDocSettings.startupCaptureAfterFrames, 42u);
		ASSERT_TRUE(parsed.projectPathOverride.has_value());
		EXPECT_EQ(parsed.projectPathOverride.value(), "TestProject");
	}
	else
	{
		EXPECT_FALSE(parsed.renderDocSettings.enabled);
		EXPECT_EQ(parsed.renderDocSettings.startupCaptureAfterFrames, 0u);
		EXPECT_FALSE(parsed.projectPathOverride.has_value());
	}
}

TEST(GameLaunchArgsTests, DefaultsToThreadedRenderingMainlineWithoutOptInFlag)
{
	std::vector<std::string> storage;
	char** argv = MutableArgv({"Game.exe", "TestProject"}, storage);

	const auto parsed = NLS::Game::Launch::ParseGameArgs(static_cast<int>(storage.size()), argv);

	EXPECT_FALSE(parsed.hasError);
	EXPECT_TRUE(parsed.enableThreadedRendering);
}

TEST(GameLaunchArgsTests, RejectsUnknownBackendName)
{
	std::vector<std::string> storage;
	char** argv = MutableArgv({"Game.exe", "--backend", "mystery"}, storage);

	const auto parsed = NLS::Game::Launch::ParseGameArgs(static_cast<int>(storage.size()), argv);

	EXPECT_TRUE(parsed.hasError);
	EXPECT_FALSE(parsed.showHelp);
	EXPECT_FALSE(parsed.backendOverride.has_value());
}

TEST(GameLaunchArgsTests, ReturnsHelpWithoutTreatingItAsAnError)
{
	std::vector<std::string> storage;
	char** argv = MutableArgv({"Game.exe", "--help"}, storage);

	const auto parsed = NLS::Game::Launch::ParseGameArgs(static_cast<int>(storage.size()), argv);

	EXPECT_FALSE(parsed.hasError);
	EXPECT_TRUE(parsed.showHelp);
	EXPECT_FALSE(parsed.backendOverride.has_value());
	EXPECT_FALSE(parsed.projectPathOverride.has_value());
}
