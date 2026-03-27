#include "Windowing/Window.h"
#include <filesystem>
#include "Utils/PathParser.h"
#include "Core/Utils/String.h"
#include "Core/Application.h"
#include "Debug/Logger.h"
#include "Core//Launcher.h"
#include <cstdio>
#include <exception>
using namespace NLS;

int main(int argc, char** argv);
static void TryRun(const std::string& projectPath, const std::string& projectName);

int main(int argc, char** argv)
{
	std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path());

	bool ready = false;
	std::string projectPath;
	std::string projectName;

	if (argc < 2)
	{
		Launcher launcher;
		std::tie(ready, projectPath, projectName) = launcher.Run();
	}
	else
	{
		// Project file given as argument ==> Open the project directly.
		std::string projectFile = argv[1];

		if (Utils::PathParser::GetExtension(projectFile) == "nullus")
		{
			ready = true;
			projectPath = std::filesystem::path(projectFile).parent_path().string();
			projectName = Utils::PathParser::GetElementName(projectFile);
			Utils::String::Replace(projectName, ".nullus", "");
		}
	}

	if (ready)
		TryRun(projectPath, projectName);
	return EXIT_SUCCESS;
}

static void TryRun(const std::string& projectPath, const std::string& projectName)
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
		app = std::make_unique<Editor::Core::Application>(projectPath, projectName);
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
