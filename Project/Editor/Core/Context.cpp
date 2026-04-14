#include <filesystem>

#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include "Windowing/Settings/DeviceSettings.h"
#include "Core/Context.h"
#include "Debug/FileHandler.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocEnvironment.h"
#include "Utils/PathParser.h"
#include "Resource/Actor/ActorManager.h"

using namespace NLS::Core::ResourceManagement;
namespace NLS
{
namespace
{
	bool IsEditorLayoutFileHealthy(const std::string& content)
	{
		return content.find("[Docking][Data]") != std::string::npos &&
			content.find("[Window][Scene View##Scene View]") != std::string::npos &&
			content.find("[Window][Game View##Game View]") != std::string::npos &&
			content.find("[Window][Hierarchy##Hierarchy]") != std::string::npos &&
			content.find("[Window][Inspector##Inspector]") != std::string::npos;
	}

	Render::Settings::EGraphicsBackend ResolveGraphicsBackend(NLS::Filesystem::IniFile& projectSettings)
	{
		if (const auto backend = Render::Settings::TryReadGraphicsBackendFromEnvironment("NLS_GRAPHICS_BACKEND"); backend.has_value())
			return backend.value();

		if (projectSettings.IsKeyExisting("graphics_backend"))
			return Render::Settings::ParseGraphicsBackendOrDefault(projectSettings.Get<std::string>("graphics_backend"));

		return Render::Settings::GetPlatformDefaultGraphicsBackend();
	}

	Render::Settings::EGraphicsBackend ResolveEditorGraphicsBackend(
		NLS::Filesystem::IniFile& projectSettings,
		std::optional<Render::Settings::EGraphicsBackend> backendOverride)
	{
		if (backendOverride.has_value())
		{
			NLS_LOG_INFO("Using command-line backend override: " + std::string(Render::Settings::ToString(backendOverride.value())));
			return backendOverride.value();
		}

		return ResolveGraphicsBackend(projectSettings);
	}

	Windowing::Settings::WindowClientAPI ToWindowClientAPI(Render::Settings::EGraphicsBackend backend)
	{
		return backend == Render::Settings::EGraphicsBackend::OPENGL
			? Windowing::Settings::WindowClientAPI::OpenGL
			: Windowing::Settings::WindowClientAPI::NoAPI;
	}

    void MigrateLegacyEditorLayoutFile(const std::filesystem::path& layoutPath)
    {
        if (!std::filesystem::exists(layoutPath))
            return;

        std::ifstream input(layoutPath, std::ios::binary);
        if (!input)
            return;

        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const std::regex legacyPanelWindowPattern(R"(\[Window\]\[([^\[#].*?)##\d+\])");
        const std::string normalizedContent = std::regex_replace(content, legacyPanelWindowPattern, "[Window][$1##$1]");

        std::istringstream stream(normalizedContent);
        std::ostringstream deduplicated;
        std::string line;
        std::string currentHeader;
        std::ostringstream currentSection;
        std::unordered_set<std::string> seenWindowSections;

        auto flushSection = [&]()
        {
            if (currentHeader.empty())
                return;

            const bool isWindowSection = currentHeader.rfind("[Window][", 0) == 0;
            if (!isWindowSection || seenWindowSections.insert(currentHeader).second)
                deduplicated << currentSection.str();

            currentHeader.clear();
            currentSection.str({});
            currentSection.clear();
        };

        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (!line.empty() && line.front() == '[')
            {
                flushSection();
                currentHeader = line;
            }

            currentSection << line << "\n";
        }
        flushSection();

        const std::string migratedContent = deduplicated.str();
        if (migratedContent == content)
            return;

        std::ofstream output(layoutPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return;

        output.write(migratedContent.data(), static_cast<std::streamsize>(migratedContent.size()));
        NLS_LOG_INFO("Migrated legacy editor layout IDs in " + layoutPath.string());
    }

	void EnsureEditorLayoutFileReady(const std::filesystem::path& layoutPath, const std::filesystem::path& defaultLayoutPath)
	{
		std::error_code error;
		std::filesystem::create_directories(layoutPath.parent_path(), error);

		bool restoredDefaultLayout = false;
		if (!std::filesystem::exists(layoutPath))
		{
			if (std::filesystem::exists(defaultLayoutPath))
			{
				std::filesystem::copy_file(defaultLayoutPath, layoutPath, std::filesystem::copy_options::overwrite_existing, error);
				restoredDefaultLayout = !error;
			}
		}

		if (!std::filesystem::exists(layoutPath))
			return;

		MigrateLegacyEditorLayoutFile(layoutPath);

		std::ifstream input(layoutPath, std::ios::binary);
		if (!input)
			return;

		const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if (IsEditorLayoutFileHealthy(content))
			return;

		if (!std::filesystem::exists(defaultLayoutPath))
			return;

		std::filesystem::copy_file(defaultLayoutPath, layoutPath, std::filesystem::copy_options::overwrite_existing, error);
		if (error)
			return;

		MigrateLegacyEditorLayoutFile(layoutPath);
		NLS_LOG_WARNING(
			(restoredDefaultLayout
				? "Initialized missing editor layout from default template: "
				: "Reset invalid editor layout to default template: ") +
			layoutPath.string());
	}
}

Editor::Core::Context::Context(
	const std::string& p_projectPath,
	const std::string& p_projectName,
	std::optional<Render::Settings::EGraphicsBackend> p_backendOverride,
	const Render::Settings::RenderDocSettings& p_renderDocSettings)
    : projectPath(p_projectPath), 
    projectName(p_projectName), 
    m_backendOverride(p_backendOverride),
    m_renderDocSettings(p_renderDocSettings),
    projectFilePath(p_projectPath + Utils::PathParser::Separator() + p_projectName + ".nullus"), 
    engineAssetsPath(std::filesystem::canonical(std::filesystem::path("../Assets/Engine")).string() + Utils::PathParser::Separator()), 
    projectAssetsPath(p_projectPath + Utils::PathParser::Separator() + "Assets" + Utils::PathParser::Separator()), 
    editorAssetsPath(std::filesystem::canonical(std::filesystem::path("../Assets/Editor")).string() + Utils::PathParser::Separator()), 
    sceneManager(projectAssetsPath), projectSettings(projectFilePath)
{
    ModelManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    TextureManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    ShaderManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    MaterialManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    NLS::Engine::ActorManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);

	if (!IsProjectSettingsIntegrityVerified())
	{
		NLS_LOG_WARNING("Project settings file is missing keys or empty. Restoring editor defaults in " + projectFilePath);
		ResetProjectSettings();
		projectSettings.Rewrite();
	}

    NLS::Debug::FileHandler::SetLogFilePath(p_projectPath + Utils::PathParser::Separator() + "Logs");
    /* Settings */
    NLS::Windowing::Settings::DeviceSettings deviceSettings;
    deviceSettings.contextMajorVersion = 4;
    deviceSettings.contextMinorVersion = 3;
    windowSettings.title = "Nullus Editor";
    windowSettings.width = 1600;
    windowSettings.height = 900;
    const auto graphicsBackend = ResolveEditorGraphicsBackend(projectSettings, m_backendOverride);
    windowSettings.clientAPI = ToWindowClientAPI(graphicsBackend);

    /* Graphics context creation */
    NLS::Render::Settings::DriverSettings driverSettings;
    driverSettings.graphicsBackend = graphicsBackend;
    driverSettings.debugMode = true;

	if (m_renderDocSettings.enabled || m_renderDocSettings.startupCaptureAfterFrames > 0)
	{
		driverSettings.renderDoc = m_renderDocSettings;
		NLS_LOG_INFO("RenderDoc: applied command-line settings (enabled=" +
			std::string(m_renderDocSettings.enabled ? "true" : "false") +
			", captureAfterFrames=" + std::to_string(m_renderDocSettings.startupCaptureAfterFrames) + ")");
	}

	Render::Tooling::ApplyRenderDocEnvironmentOverrides(
		driverSettings.renderDoc,
		(std::filesystem::path(p_projectPath) / "Logs" / "RenderDoc" / "Editor").string(),
		"Editor");
    Render::Tooling::PreloadRenderDocIfAvailable(driverSettings.renderDoc);

    /* Window creation */
    device = std::make_unique<NLS::Context::Device>(deviceSettings);
    window = std::make_unique<NLS::Windowing::Window>(*device, windowSettings);
    window->SetIcon(engineAssetsPath + "Brand" + Utils::PathParser::Separator() + "NullusLogoMark.png");
    inputManager = std::make_unique<NLS::Windowing::Inputs::InputManager>(*window);
    if (graphicsBackend == Render::Settings::EGraphicsBackend::OPENGL)
        window->MakeCurrentContext();

    /* Center Window */
    auto monSize = device->GetMonitorSize();
    auto winSize = window->GetSize();
    window->SetPosition(monSize.x / 2 - winSize.x / 2, monSize.y / 2 - winSize.y / 2);

    if (windowSettings.clientAPI == Windowing::Settings::WindowClientAPI::OpenGL)
        device->SetVsync(true);
    driver = std::make_unique<NLS::Render::Context::Driver>(driverSettings);
    const auto driverCapabilities = driver != nullptr
        ? driver->GetCapabilities()
        : NLS::Render::RHI::RHIDeviceCapabilities{};
    const auto runtimeFallbackDecision =
        Render::Settings::EvaluateEditorMainRuntimeFallback(graphicsBackend, driverCapabilities);
    if (runtimeFallbackDecision.primaryWarning.has_value())
        NLS_LOG_WARNING(runtimeFallbackDecision.primaryWarning.value());
    if (runtimeFallbackDecision.detailWarning.has_value())
        NLS_LOG_WARNING(runtimeFallbackDecision.detailWarning.value());

    if (driver == nullptr ||
        runtimeFallbackDecision.primaryWarning.has_value() ||
        !Render::Settings::SupportsEditorMainRuntime(driverCapabilities))
    {
        const std::string message =
            "Editor startup failed: could not create a validated runtime for backend " +
            std::string(Render::Settings::ToString(graphicsBackend)) +
            ".";
        NLS_LOG_ERROR(message);
        throw std::runtime_error(message);
    }

    NLS::Render::RHI::SwapchainDesc swapchainDesc;
    swapchainDesc.platformWindow = window->GetGlfwWindow();
    swapchainDesc.nativeWindowHandle = window->GetNativeWindowHandle();
    swapchainDesc.width = static_cast<uint32_t>(windowSettings.width);
    swapchainDesc.height = static_cast<uint32_t>(windowSettings.height);
    swapchainDesc.vsync = true;
    if (!driver->CreateSwapchain(swapchainDesc))
    {
        const std::string message = "Editor startup failed: CreateSwapchain returned false.";
        NLS_LOG_ERROR(message);
        throw std::runtime_error(message);
    }
    NLS::Core::ServiceLocator::Provide<NLS::Render::Context::Driver>(*driver);

    if (const auto pickingReadbackWarning = Render::Settings::GetEditorPickingReadbackWarning(driverCapabilities);
        pickingReadbackWarning.has_value())
    {
        NLS_LOG_WARNING(pickingReadbackWarning.value());
    }

    uiManager = std::make_unique<NLS::UI::UIManager>(
        window->GetGlfwWindow(),
        driverSettings.graphicsBackend,
        driver->GetNativeDeviceInfo(),
        UI::EStyle::ALTERNATIVE_DARK);
    uiManager->LoadFont("Ruda_Big", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 16);
    uiManager->LoadFont("Ruda_Small", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 12);
    uiManager->LoadFont("Ruda_Medium", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 14);
    uiManager->UseFont("Ruda_Medium");
    const auto layoutPath = std::filesystem::path(p_projectPath) / "UserSettings" / "layout.ini";
    EnsureEditorLayoutFileReady(layoutPath, std::filesystem::path(editorAssetsPath) / "Settings" / "layout.ini");
    window->FramebufferResizeEvent.AddListener([this](uint16_t width, uint16_t height)
    {
        if (driver != nullptr && width > 0u && height > 0u)
        {
            driver->ResizePlatformSwapchain(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }
    });
    uiManager->SetEditorLayoutSaveFilename(layoutPath.string());
    uiManager->SetEditorLayoutAutosaveFrequency(60.0f);
    uiManager->EnableEditorLayoutSave(true);
    uiManager->EnableDocking(true);

    /* Editor resources */
    editorResources = std::make_unique<Editor::Core::EditorResources>(editorAssetsPath);

    /* Service Locator providing */
    NLS::Core::ServiceLocator::Provide<ModelManager>(modelManager);
    NLS::Core::ServiceLocator::Provide<TextureManager>(textureManager);
    NLS::Core::ServiceLocator::Provide<ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<MaterialManager>(materialManager);
    NLS::Core::ServiceLocator::Provide<NLS::UI::UIManager>(*uiManager);
    NLS::Core::ServiceLocator::Provide<NLS::Engine::ActorManager>(actorManager);


    NLS::Core::ServiceLocator::Provide<NLS::Windowing::Inputs::InputManager>(*inputManager);
    NLS::Core::ServiceLocator::Provide<NLS::Windowing::Window>(*window);
    NLS::Core::ServiceLocator::Provide<NLS::Engine::SceneSystem::SceneManager>(sceneManager);


    ApplyProjectSettings();
}

Editor::Core::Context::~Context()
{
    modelManager.UnloadResources();
    textureManager.UnloadResources();
    shaderManager.UnloadResources();
    materialManager.UnloadResources();
}

void Editor::Core::Context::ResetProjectSettings()
{
    projectSettings.RemoveAll();
    projectSettings.Add<float>("gravity", -9.81f);
    projectSettings.Add<int>("x_resolution", 1280);
    projectSettings.Add<int>("y_resolution", 720);
    projectSettings.Add<bool>("fullscreen", false);
    projectSettings.Add<std::string>("executable_name", "Game");
    projectSettings.Add<std::string>("start_scene", "");
    projectSettings.Add<bool>("vsync", true);
    projectSettings.Add<bool>("multi_sampling", true);
    projectSettings.Add<int>("samples", 4);
    projectSettings.Add<std::string>("graphics_backend", Render::Settings::ToString(Render::Settings::GetPlatformDefaultGraphicsBackend()));
    projectSettings.Add<int>("opengl_major", 4);
    projectSettings.Add<int>("opengl_minor", 3);
    projectSettings.Add<bool>("dev_build", true);
}

bool Editor::Core::Context::IsProjectSettingsIntegrityVerified()
{
    return projectSettings.IsKeyExisting("gravity") && projectSettings.IsKeyExisting("x_resolution") && projectSettings.IsKeyExisting("y_resolution") && projectSettings.IsKeyExisting("fullscreen") && projectSettings.IsKeyExisting("executable_name") && projectSettings.IsKeyExisting("start_scene") && projectSettings.IsKeyExisting("vsync") && projectSettings.IsKeyExisting("multi_sampling") && projectSettings.IsKeyExisting("samples") && projectSettings.IsKeyExisting("graphics_backend") && projectSettings.IsKeyExisting("opengl_major") && projectSettings.IsKeyExisting("opengl_minor") && projectSettings.IsKeyExisting("dev_build");
}

void Editor::Core::Context::ApplyProjectSettings()
{
}
} // namespace NLS
