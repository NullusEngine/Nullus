#include "Windowing/Window.h"
#include <filesystem>
#include "Utils/PathParser.h"
#include "Core/Application.h"
#include "Assembly.h"
#include "AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"
#include "Debug/Logger.h"
using namespace NLS;
using namespace Engine;

int main(int argc, char** argv);
static void TryRun(const std::string& projectPath, const std::string& projectName);

int main(int argc, char** argv)
{
    std::filesystem::current_path(NLS::Utils::PathParser::GetContainingFolder(argv[0]));

    Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<AssemblyEngine>();

	std::string projectPath;
	std::string projectName;

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
	catch (...) {}

	if (app)
		app->Run();
}