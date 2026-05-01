#pragma once

#include <optional>
#include <string>

#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"

namespace NLS::Game::Launch
{
	struct ParsedGameLaunchArgs
	{
		Render::Settings::RenderDocSettings renderDocSettings;
		Render::Settings::EngineDiagnosticsSettings diagnosticsSettings;
		std::optional<Render::Settings::EGraphicsBackend> backendOverride;
		std::optional<std::string> projectPathOverride;
		bool enableThreadedRendering = true;
		bool showHelp = false;
		bool hasError = false;
	};

	void PrintUsage(const char* executableName);
	ParsedGameLaunchArgs ParseGameArgs(int argc, char** argv);
}
