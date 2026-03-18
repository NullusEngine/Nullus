#include <filesystem>

#include "Core/Context.h"

#include <Core/ServiceLocator.h>
#include "Utils/PathParser.h"
using namespace NLS;
using namespace NLS::Core;
using namespace NLS::Core::ResourceManagement;

Game::Context::Context()
    : engineAssetsPath(std::filesystem::canonical(std::filesystem::path("../Assets/Engine")).string() + Utils::PathParser::Separator()), projectAssetsPath(""),
	projectScriptsPath(""),
	projectSettings(""),
	sceneManager(projectAssetsPath)
{
	ModelManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
	TextureManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
	ShaderManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    MaterialManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);

	/* Settings */
	Windowing::Settings::DeviceSettings deviceSettings;
	deviceSettings.contextMajorVersion = 4;
	deviceSettings.contextMinorVersion = 3;
	deviceSettings.samples = 4;

	Windowing::Settings::WindowSettings windowSettings;
	windowSettings.title = "Nullus";
	windowSettings.width = 1600;
	windowSettings.height = 900;
	windowSettings.maximized = false;
	windowSettings.resizable = false;
	windowSettings.fullscreen = false;
	windowSettings.samples = 4;

	/* Window creation */
	device = std::make_unique<NLS::Context::Device>(deviceSettings);
	window = std::make_unique<NLS::Windowing::Window>(*device, windowSettings);
	window->SetIcon(engineAssetsPath + "Brand" + Utils::PathParser::Separator() + "NullusLogoMark.png");
	inputManager = std::make_unique<NLS::Windowing::Inputs::InputManager>(*window);
	window->MakeCurrentContext();

	device->SetVsync(projectSettings.Get<bool>("vsync"));

	NLS::Render::Data::PipelineState basePSO;
	basePSO.multisample = projectSettings.Get<bool>("multisampling");

	/* Graphics context creation */
	driver = std::make_unique<NLS::Render::Context::Driver>(NLS::Render::Settings::DriverSettings{
#ifdef _DEBUG
		true,
#else
		false,
#endif
		basePSO
	});


	/* Service Locator providing */
	ServiceLocator::Provide<ModelManager>(modelManager);
	ServiceLocator::Provide<TextureManager>(textureManager);
	ServiceLocator::Provide<ShaderManager>(shaderManager);
}

Game::Context::~Context()
{
	modelManager.UnloadResources();
	textureManager.UnloadResources();
	shaderManager.UnloadResources();
}
