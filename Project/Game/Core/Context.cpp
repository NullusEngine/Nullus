#include <filesystem>

#include "Core/Context.h"

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocEnvironment.h"
#include "Utils/PathParser.h"
using namespace NLS;
using namespace NLS::Core;
using namespace NLS::Core::ResourceManagement;

namespace
{
	struct ResolvedGameProjectPaths
	{
		std::string settingsPath;
		std::string assetsPath;
		std::string scriptsPath;
	};

	const ResolvedGameProjectPaths& ResolveGameProjectPaths()
	{
		static const ResolvedGameProjectPaths resolved = []()
		{
			ResolvedGameProjectPaths result;

			if (const char* explicitProject = std::getenv("NLS_PROJECT_FILE"); explicitProject != nullptr)
			{
				const std::filesystem::path explicitProjectPath = std::filesystem::weakly_canonical(std::filesystem::path(explicitProject));
				if (std::filesystem::exists(explicitProjectPath))
				{
					result.settingsPath = explicitProjectPath.string();
					result.assetsPath = (explicitProjectPath.parent_path() / "Assets").string() + Utils::PathParser::Separator();
					result.scriptsPath = (explicitProjectPath.parent_path() / "Scripts").string() + Utils::PathParser::Separator();
					return result;
				}
			}

			const auto cwd = std::filesystem::current_path();
			const auto packagedSettings = cwd / "Data" / "User" / "Game.ini";
			if (std::filesystem::exists(packagedSettings))
			{
				result.settingsPath = packagedSettings.string();
				result.assetsPath = (cwd / "Data" / "User" / "Assets").string() + Utils::PathParser::Separator();
				result.scriptsPath = (cwd / "Data" / "User" / "Scripts").string() + Utils::PathParser::Separator();
				return result;
			}

			for (auto probe = cwd; !probe.empty(); probe = probe.parent_path())
			{
				const auto testProject = probe / "TestProject" / "TestProject.nullus";
				if (std::filesystem::exists(testProject))
				{
					result.settingsPath = testProject.string();
					result.assetsPath = (testProject.parent_path() / "Assets").string() + Utils::PathParser::Separator();
					result.scriptsPath = (testProject.parent_path() / "Scripts").string() + Utils::PathParser::Separator();
					return result;
				}

				std::error_code error;
				for (const auto& child : std::filesystem::directory_iterator(probe, error))
				{
					if (error)
						break;

					if (!child.is_directory())
						continue;

					for (const auto& candidate : std::filesystem::directory_iterator(child.path(), error))
					{
						if (error)
							break;

						if (candidate.is_regular_file() && candidate.path().extension() == ".nullus")
						{
							result.settingsPath = candidate.path().string();
							result.assetsPath = (candidate.path().parent_path() / "Assets").string() + Utils::PathParser::Separator();
							result.scriptsPath = (candidate.path().parent_path() / "Scripts").string() + Utils::PathParser::Separator();
							return result;
						}
					}
				}

				if (probe == probe.root_path())
					break;
			}

			return result;
		}();

		return resolved;
	}

	NLS::Render::Settings::EGraphicsBackend ResolveGraphicsBackend(NLS::Filesystem::IniFile& projectSettings)
	{
		if (const auto backend = NLS::Render::Settings::TryReadGraphicsBackendFromEnvironment("NLS_GRAPHICS_BACKEND"); backend.has_value())
			return backend.value();

		if (projectSettings.IsKeyExisting("graphics_backend"))
			return NLS::Render::Settings::ParseGraphicsBackendOrDefault(projectSettings.Get<std::string>("graphics_backend"));

		return NLS::Render::Settings::GetPlatformDefaultGraphicsBackend();
	}

	Windowing::Settings::WindowClientAPI ToWindowClientAPI(NLS::Render::Settings::EGraphicsBackend backend)
	{
		return backend == NLS::Render::Settings::EGraphicsBackend::OPENGL
			? Windowing::Settings::WindowClientAPI::OpenGL
			: Windowing::Settings::WindowClientAPI::NoAPI;
	}
}

Game::Context::Context()
    : engineAssetsPath(std::filesystem::canonical(std::filesystem::path("../Assets/Engine")).string() + Utils::PathParser::Separator()), projectAssetsPath(ResolveGameProjectPaths().assetsPath),
	projectScriptsPath(ResolveGameProjectPaths().scriptsPath),
	projectSettings(ResolveGameProjectPaths().settingsPath),
	sceneManager(projectAssetsPath)
{
	ModelManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
	TextureManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
	ShaderManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    MaterialManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);

	if (!projectSettings.IsKeyExisting("start_scene"))
		projectSettings.Add<std::string>("start_scene", "");
	if (!projectSettings.IsKeyExisting("graphics_backend"))
		projectSettings.Add<std::string>("graphics_backend", NLS::Render::Settings::ToString(NLS::Render::Settings::GetPlatformDefaultGraphicsBackend()));
	if (!projectSettings.IsKeyExisting("vsync"))
		projectSettings.Add<bool>("vsync", true);
	if (!projectSettings.IsKeyExisting("multi_sampling"))
		projectSettings.Add<bool>("multi_sampling", true);

	if (!ResolveGameProjectPaths().settingsPath.empty())
		NLS_LOG_INFO("Game runtime using project settings: " + ResolveGameProjectPaths().settingsPath);
	else
		NLS_LOG_WARNING("Game runtime could not resolve a project settings file. Falling back to in-memory defaults.");

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
	auto graphicsBackend = ResolveGraphicsBackend(projectSettings);
	windowSettings.clientAPI = ToWindowClientAPI(graphicsBackend);

	NLS::Render::Data::PipelineState basePSO;
	basePSO.multisample = projectSettings.GetOrDefault<bool>("multi_sampling", true);

	NLS::Render::Settings::DriverSettings driverSettings;
	driverSettings.graphicsBackend = graphicsBackend;
#ifdef _DEBUG
	driverSettings.debugMode = true;
#else
	driverSettings.debugMode = false;
#endif
	driverSettings.defaultPipelineState = basePSO;
	Render::Tooling::ApplyRenderDocEnvironmentOverrides(
		driverSettings.renderDoc,
		(std::filesystem::current_path() / "Logs" / "RenderDoc" / "Game").string(),
		"Game");
	Render::Tooling::PreloadRenderDocIfAvailable(driverSettings.renderDoc);

	/* Window creation */
	device = std::make_unique<NLS::Context::Device>(deviceSettings);
	window = std::make_unique<NLS::Windowing::Window>(*device, windowSettings);
	window->SetIcon(engineAssetsPath + "Brand" + Utils::PathParser::Separator() + "NullusLogoMark.png");
	inputManager = std::make_unique<NLS::Windowing::Inputs::InputManager>(*window);
	if (graphicsBackend == NLS::Render::Settings::EGraphicsBackend::OPENGL)
		window->MakeCurrentContext();

	device->SetVsync(projectSettings.GetOrDefault<bool>("vsync", true));

	/* Graphics context creation */
	driver = std::make_unique<NLS::Render::Context::Driver>(driverSettings);

	if ((!driver->IsBackendReady() || !driver->SupportsCurrentSceneRenderer()) &&
		graphicsBackend != NLS::Render::Settings::EGraphicsBackend::OPENGL)
	{
		if (!driver->IsBackendReady())
		{
			NLS_LOG_WARNING(
				"Requested game backend " +
				std::string(NLS::Render::Settings::ToString(graphicsBackend)) +
				" is not ready. Falling back to OpenGL.");
		}
		else
		{
			NLS_LOG_WARNING(
				"Game scene rendering is still OpenGL-native. Falling back from " +
				std::string(NLS::Render::Settings::ToString(graphicsBackend)) +
				" to OpenGL for the current runtime.");
			NLS_LOG_WARNING(NLS::Render::Settings::SceneRendererSupportDescription(graphicsBackend));
		}

		graphicsBackend = NLS::Render::Settings::EGraphicsBackend::OPENGL;
		windowSettings.clientAPI = ToWindowClientAPI(graphicsBackend);
		window = std::make_unique<NLS::Windowing::Window>(*device, windowSettings);
		window->SetIcon(engineAssetsPath + "Brand" + Utils::PathParser::Separator() + "NullusLogoMark.png");
		inputManager = std::make_unique<NLS::Windowing::Inputs::InputManager>(*window);
		window->MakeCurrentContext();
		driverSettings.graphicsBackend = graphicsBackend;
		driver = std::make_unique<NLS::Render::Context::Driver>(driverSettings);
	}

	NLS::Render::RHI::SwapchainDesc swapchainDesc;
	swapchainDesc.platformWindow = window->GetGlfwWindow();
	swapchainDesc.nativeWindowHandle = window->GetNativeWindowHandle();
	swapchainDesc.width = static_cast<uint32_t>(windowSettings.width);
	swapchainDesc.height = static_cast<uint32_t>(windowSettings.height);
	swapchainDesc.vsync = projectSettings.GetOrDefault<bool>("vsync", true);
	driver->CreateSwapchain(swapchainDesc);
	window->FramebufferResizeEvent.AddListener([this](uint16_t width, uint16_t height)
	{
		if (driver != nullptr && width > 0u && height > 0u)
		{
			driver->ResizeSwapchain(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		}
	});


	/* Service Locator providing */
	ServiceLocator::Provide<NLS::Render::Context::Driver>(*driver);
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
