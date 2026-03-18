#include <filesystem>

#include <Core/ServiceLocator.h>
#include "Windowing/Settings/DeviceSettings.h"
#include "Core/Context.h"
#include "Debug/FileHandler.h"
#include "Utils/PathParser.h"
#include "Resource/Actor/ActorManager.h"

using namespace NLS::Core::ResourceManagement;
namespace NLS
{
Editor::Core::Context::Context(const std::string& p_projectPath, const std::string& p_projectName)
    : projectPath(p_projectPath), 
    projectName(p_projectName), 
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

    NLS::Debug::FileHandler::SetLogFilePath(p_projectPath + Utils::PathParser::Separator() + "Logs");
    /* Settings */
    NLS::Windowing::Settings::DeviceSettings deviceSettings;
    deviceSettings.contextMajorVersion = 4;
    deviceSettings.contextMinorVersion = 3;
    windowSettings.title = "Nullus Editor";
    windowSettings.width = 1600;
    windowSettings.height = 900;

    /* Window creation */
    device = std::make_unique<NLS::Context::Device>(deviceSettings);
    window = std::make_unique<NLS::Windowing::Window>(*device, windowSettings);
    window->SetIcon(engineAssetsPath + "Brand" + Utils::PathParser::Separator() + "NullusLogoMark.png");
    inputManager = std::make_unique<NLS::Windowing::Inputs::InputManager>(*window);
    window->MakeCurrentContext();

    /* Center Window */
    auto monSize = device->GetMonitorSize();
    auto winSize = window->GetSize();
    window->SetPosition(monSize.x / 2 - winSize.x / 2, monSize.y / 2 - winSize.y / 2);

    device->SetVsync(true);

    /* Graphics context creation */
    driver = std::make_unique<NLS::Render::Context::Driver>(NLS::Render::Settings::DriverSettings{true});

    uiManager = std::make_unique<NLS::UI::UIManager>(window->GetGlfwWindow(), UI::EStyle::ALTERNATIVE_DARK);
    uiManager->LoadFont("Ruda_Big", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 16);
    uiManager->LoadFont("Ruda_Small", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 12);
    uiManager->LoadFont("Ruda_Medium", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 14);
    uiManager->UseFont("Ruda_Medium");
    uiManager->SetEditorLayoutSaveFilename(p_projectPath + "/UserSettings/layout.ini");
    uiManager->SetEditorLayoutAutosaveFrequency(60.0f);
    uiManager->EnableEditorLayoutSave(true);
    uiManager->EnableDocking(true);

    if (!std::filesystem::exists(p_projectPath + "/UserSettings/layout.ini"))
        uiManager->ResetLayout(editorAssetsPath + "/Settings/layout.ini");

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
    projectSettings.Add<std::string>("start_scene", "Scene.ovscene");
    projectSettings.Add<bool>("vsync", true);
    projectSettings.Add<bool>("multi_sampling", true);
    projectSettings.Add<int>("samples", 4);
    projectSettings.Add<int>("opengl_major", 4);
    projectSettings.Add<int>("opengl_minor", 3);
    projectSettings.Add<bool>("dev_build", true);
}

bool Editor::Core::Context::IsProjectSettingsIntegrityVerified()
{
    return projectSettings.IsKeyExisting("gravity") && projectSettings.IsKeyExisting("x_resolution") && projectSettings.IsKeyExisting("y_resolution") && projectSettings.IsKeyExisting("fullscreen") && projectSettings.IsKeyExisting("executable_name") && projectSettings.IsKeyExisting("start_scene") && projectSettings.IsKeyExisting("vsync") && projectSettings.IsKeyExisting("multi_sampling") && projectSettings.IsKeyExisting("samples") && projectSettings.IsKeyExisting("opengl_major") && projectSettings.IsKeyExisting("opengl_minor") && projectSettings.IsKeyExisting("dev_build");
}

void Editor::Core::Context::ApplyProjectSettings()
{
}
} // namespace NLS
