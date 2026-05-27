#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

#include "Core/Context.h"

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include <stdexcept>
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocEnvironment.h"
#include "Utils/PathParser.h"
#include "RuntimeAssetManifestStartup.h"
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

	std::optional<std::filesystem::path> ResolveProjectSettingsPathFromInput(const std::filesystem::path& inputPath)
	{
		std::error_code error;
		const std::filesystem::path normalizedInput = std::filesystem::weakly_canonical(inputPath, error);
		const std::filesystem::path effectiveInput = error ? inputPath : normalizedInput;

		if (std::filesystem::is_regular_file(effectiveInput, error) && effectiveInput.extension() == ".nullus")
			return effectiveInput;

		if (std::filesystem::is_directory(effectiveInput, error))
		{
			for (const auto& entry : std::filesystem::directory_iterator(effectiveInput, error))
			{
				if (error)
					break;
				if (entry.is_regular_file() && entry.path().extension() == ".nullus")
					return entry.path();
			}
		}

		return std::nullopt;
	}

	ResolvedGameProjectPaths ResolveGameProjectPaths(const std::optional<std::string>& projectPathOverride)
	{
		ResolvedGameProjectPaths result;

		if (projectPathOverride.has_value())
		{
			if (const auto resolvedProject = ResolveProjectSettingsPathFromInput(projectPathOverride.value()); resolvedProject.has_value())
			{
				result.settingsPath = resolvedProject->string();
				result.assetsPath = (resolvedProject->parent_path() / "Assets").string() + Utils::PathParser::Separator();
				result.scriptsPath = (resolvedProject->parent_path() / "Scripts").string() + Utils::PathParser::Separator();
				return result;
			}
		}

		if (const char* explicitProject = std::getenv("NLS_PROJECT_FILE"); explicitProject != nullptr)
		{
			if (const auto resolvedProject = ResolveProjectSettingsPathFromInput(explicitProject); resolvedProject.has_value())
			{
				result.settingsPath = resolvedProject->string();
				result.assetsPath = (resolvedProject->parent_path() / "Assets").string() + Utils::PathParser::Separator();
				result.scriptsPath = (resolvedProject->parent_path() / "Scripts").string() + Utils::PathParser::Separator();
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
	}

	NLS::Render::Settings::EGraphicsBackend ResolveGraphicsBackend(NLS::Filesystem::IniFile& projectSettings)
	{
		if (projectSettings.IsKeyExisting("graphics_backend"))
		{
			return NLS::Render::Settings::ParseGraphicsBackendOrDefault(
				projectSettings.Get<std::string>("graphics_backend"),
				NLS::Render::Settings::GetPhase1RequiredRuntimeBackend());
		}

		return NLS::Render::Settings::GetPhase1RequiredRuntimeBackend();
	}

	std::vector<std::pair<std::string, std::string>> ParseRuntimePrewarmAssetPacks(
		const std::string& value)
	{
		std::vector<std::pair<std::string, std::string>> packs;
		std::stringstream stream(value);
		std::string item;
		while (std::getline(stream, item, ';'))
		{
			if (item.empty())
				continue;

			const auto separator = item.find(':');
			if (separator == std::string::npos)
				packs.emplace_back(item, std::string {});
			else
				packs.emplace_back(item.substr(0u, separator), item.substr(separator + 1u));
		}
		return packs;
	}
}

Game::Context::Context(
	std::optional<Render::Settings::RenderDocSettings> p_renderDocOverride,
	std::optional<Render::Settings::EGraphicsBackend> p_backendOverride,
	std::optional<std::string> p_projectPathOverride)
    : engineAssetsPath(std::filesystem::canonical(std::filesystem::path("../Assets/Engine")).string() + Utils::PathParser::Separator()), projectAssetsPath(ResolveGameProjectPaths(p_projectPathOverride).assetsPath),
	projectScriptsPath(ResolveGameProjectPaths(p_projectPathOverride).scriptsPath),
	projectSettings(ResolveGameProjectPaths(p_projectPathOverride).settingsPath),
	sceneManager(projectAssetsPath),
	m_renderDocOverride(std::move(p_renderDocOverride)),
	m_backendOverride(p_backendOverride),
	m_projectPathOverride(p_projectPathOverride)
{
	const auto resolvedProjectPaths = ResolveGameProjectPaths(m_projectPathOverride);

	MeshManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
	TextureManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
	ShaderManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    MaterialManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);

	if (!projectSettings.IsKeyExisting("start_scene"))
		projectSettings.Add<std::string>("start_scene", "");
	if (!projectSettings.IsKeyExisting("graphics_backend"))
	{
		projectSettings.Add<std::string>(
			"graphics_backend",
			NLS::Render::Settings::ToString(NLS::Render::Settings::GetPhase1RequiredRuntimeBackend()));
	}
	if (!projectSettings.IsKeyExisting("vsync"))
		projectSettings.Add<bool>("vsync", true);
	if (!projectSettings.IsKeyExisting("multi_sampling"))
		projectSettings.Add<bool>("multi_sampling", true);
	if (!projectSettings.IsKeyExisting("enable_light_grid"))
		projectSettings.Add<bool>("enable_light_grid", true);
	if (!projectSettings.IsKeyExisting("enable_threaded_rendering"))
		projectSettings.Add<bool>("enable_threaded_rendering", true);
	if (!projectSettings.IsKeyExisting("renderdoc_enabled"))
		projectSettings.Add<bool>("renderdoc_enabled", false);
	if (!projectSettings.IsKeyExisting("renderdoc_capture_after_frames"))
		projectSettings.Add<int>("renderdoc_capture_after_frames", 0);

	if (!resolvedProjectPaths.settingsPath.empty())
		NLS_LOG_INFO("Game runtime using project settings: " + resolvedProjectPaths.settingsPath);
	else
		NLS_LOG_WARNING("Game runtime could not resolve a project settings file. Falling back to in-memory defaults.");

	runtimeAssetDatabase = RuntimeAssets::LoadRuntimeAssetDatabaseForProjectSettings(resolvedProjectPaths.settingsPath);
	if (runtimeAssetDatabase.has_value())
	{
		const auto manifestPath =
			RuntimeAssets::ResolveRuntimeAssetManifestPathForProjectSettings(resolvedProjectPaths.settingsPath);
		NLS_LOG_INFO("Game runtime loaded asset manifest: " + manifestPath.string());
	}

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
	if (m_backendOverride.has_value())
	{
		graphicsBackend = m_backendOverride.value();
		NLS_LOG_INFO("Using command-line backend override: " + std::string(NLS::Render::Settings::ToString(graphicsBackend)));
	}
	if (const auto restriction =
		NLS::Render::Settings::GetPhase1BackendRestrictionMessage(graphicsBackend, "Game runtime");
		restriction.has_value())
	{
		throw std::runtime_error(*restriction);
	}
	windowSettings.clientAPI = Windowing::Settings::WindowClientAPI::NoAPI;

	NLS::Render::Data::PipelineState basePSO;
	basePSO.multisample = projectSettings.GetOrDefault<bool>("multi_sampling", true);

	NLS::Render::Settings::DriverSettings driverSettings;
	driverSettings.graphicsBackend = graphicsBackend;
	driverSettings.enableThreadedRendering = projectSettings.GetOrDefault<bool>("enable_threaded_rendering", true);
	driverSettings.enableLightGrid = projectSettings.GetOrDefault<bool>("enable_light_grid", true);
	driverSettings.threadedFrameSlotCount = driverSettings.framesInFlight;
	driverSettings.defaultPipelineState = basePSO;

	Render::Settings::RenderDocSettings renderDocSettings;
	renderDocSettings.enabled = projectSettings.GetOrDefault<bool>("renderdoc_enabled", false);
	const int captureAfterFrames = projectSettings.GetOrDefault<int>("renderdoc_capture_after_frames", 0);
	renderDocSettings.startupCaptureAfterFrames = captureAfterFrames > 0 ? static_cast<uint32_t>(captureAfterFrames) : 0u;
	if (renderDocSettings.startupCaptureAfterFrames > 0u)
		renderDocSettings.enabled = true;
	if (m_renderDocOverride.has_value())
		renderDocSettings = m_renderDocOverride.value();

	if (renderDocSettings.enabled || renderDocSettings.startupCaptureAfterFrames > 0)
	{
		driverSettings.renderDoc = renderDocSettings;
		NLS_LOG_INFO(
			std::string("RenderDoc: applied ") +
			(m_renderDocOverride.has_value() ? "command-line override" : "project settings") +
			" (enabled=" + std::string(renderDocSettings.enabled ? "true" : "false") +
			", captureAfterFrames=" + std::to_string(renderDocSettings.startupCaptureAfterFrames) + ")");
	}

	Render::Tooling::ApplyRenderDocEnvironmentOverrides(
		driverSettings.renderDoc,
		(std::filesystem::current_path() / "Build" / "RenderDocCaptures" / "Game").string(),
		"Game");
	Render::Tooling::PreloadRenderDocIfAvailable(driverSettings.renderDoc);

	/* Window creation */
	device = std::make_unique<NLS::Context::Device>(deviceSettings);
	window = std::make_unique<NLS::Windowing::Window>(*device, windowSettings);
	window->SetIcon(engineAssetsPath + "Brand" + Utils::PathParser::Separator() + "NullusLogoMark.png");
	inputManager = std::make_unique<NLS::Windowing::Inputs::InputManager>(*window);

	/* Graphics context creation */
	driver = std::make_unique<NLS::Render::Context::Driver>(driverSettings);

	const auto runtimeReadiness = driver->EvaluateGameMainRuntimeReadiness(graphicsBackend);
	if (runtimeReadiness.primaryWarning.has_value())
		NLS_LOG_WARNING(runtimeReadiness.primaryWarning.value());
	if (runtimeReadiness.detailWarning.has_value())
		NLS_LOG_WARNING(runtimeReadiness.detailWarning.value());

	if (runtimeReadiness.primaryWarning.has_value())
	{
		throw std::runtime_error(runtimeReadiness.primaryWarning.value());
	}

	if (driver == nullptr || driver->GetActiveGraphicsBackend() == NLS::Render::Settings::EGraphicsBackend::NONE)
	{
		const std::string message =
			"Game startup failed: could not create a usable RHI device for backend " +
			std::string(NLS::Render::Settings::ToString(graphicsBackend)) + ".";
		NLS_LOG_ERROR(message);
		throw std::runtime_error(message);
	}

	if (!driver->CreatePlatformSwapchain(
		window->GetGlfwWindow(),
		window->GetNativeWindowHandle(),
		static_cast<uint32_t>(windowSettings.width),
		static_cast<uint32_t>(windowSettings.height),
		projectSettings.GetOrDefault<bool>("vsync", true)))
	{
		const std::string message = "Game startup failed: CreatePlatformSwapchain returned false.";
		NLS_LOG_ERROR(message);
		throw std::runtime_error(message);
	}
	window->FramebufferResizeEvent.AddListener([this](uint16_t width, uint16_t height)
	{
		if (driver != nullptr && width > 0u && height > 0u)
		{
			driver->ResizePlatformSwapchain(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		}
	});


	/* Service Locator providing */
	ServiceLocator::Provide<NLS::Render::Context::Driver>(*driver);
	ServiceLocator::Provide<MeshManager>(meshManager);
	ServiceLocator::Provide<TextureManager>(textureManager);
	ServiceLocator::Provide<ShaderManager>(shaderManager);
	ServiceLocator::Provide<MaterialManager>(materialManager);
	NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager);
	if (runtimeAssetDatabase.has_value())
	{
		ServiceLocator::Provide<NLS::Engine::Assets::RuntimeAssetDatabase>(*runtimeAssetDatabase);
		RuntimeAssets::RuntimeMaterialPrewarmOptions prewarmOptions;
		prewarmOptions.assetPacks = ParseRuntimePrewarmAssetPacks(
			projectSettings.GetOrDefault<std::string>("runtime_prewarm_asset_packs", ""));
		RuntimeAssets::PrewarmRuntimeMaterialAssets(*runtimeAssetDatabase, materialManager, prewarmOptions);
	}
}

Game::Context::~Context()
{
	ShutdownThreadedRendering();
	meshManager.UnloadResources();
	textureManager.UnloadResources();
	shaderManager.UnloadResources();
	materialManager.UnloadResources();
}

void Game::Context::ShutdownThreadedRendering()
{
	if (driver != nullptr)
		driver->ShutdownThreadedRendering();
}
