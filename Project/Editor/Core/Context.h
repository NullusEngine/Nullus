#pragma once

#include <Windowing/Context/Device.h>
#include <Windowing/Inputs/InputManager.h>
#include <Windowing/Window.h>
#include "UI/UIManager.h"
#include "Context/Driver.h"
#include <memory>
#include "SceneSystem/SceneManager.h"
#include "Filesystem/IniFile.h"
#include "ResourceManagement/ModelManager.h"
#include "ResourceManagement/TextureManager.h"
#include "ResourceManagement/ShaderManager.h"
#include "ResourceManagement/MaterialManager.h"
#include "EditorResources.h"
namespace NLS
{
namespace Editor::Core
{
/**
 * The Context handle the engine features setup
 */
class Context
{
public:
    /**
     * Constructor
     * @param p_projectPath
     * @param p_projectName
     */
    Context(const std::string& p_projectPath, const std::string& p_projectName);

    /**
     * Destructor
     */
    ~Context();

    /**
     * Reset project settings ini file
     */
    void ResetProjectSettings();

    /**
     * Verify that project settings are complete (No missing key).
     * Returns true if the integrity is verified
     */
    bool IsProjectSettingsIntegrityVerified();

    /**
     * Apply project settings to the ini file
     */
    void ApplyProjectSettings();

public:
    const std::string projectPath;
    const std::string projectName;
    const std::string projectFilePath;
    const std::string engineAssetsPath;
    const std::string projectAssetsPath;
    const std::string projectScriptsPath;
    const std::string editorAssetsPath;

    std::unique_ptr<NLS::Context::Device> device;
    std::unique_ptr<NLS::Windowing::Window> window;
    std::unique_ptr<NLS::Windowing::Inputs::InputManager> inputManager;
    std::unique_ptr<NLS::Render::Context::Driver> driver;
    std::unique_ptr<NLS::UI::UIManager> uiManager;
    std::unique_ptr<Editor::Core::EditorResources> editorResources;
    NLS::Engine::SceneSystem::SceneManager sceneManager;

    NLS::Core::ResourceManagement::ModelManager modelManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;

    NLS::Windowing::Settings::WindowSettings windowSettings;

    NLS::Filesystem::IniFile projectSettings;
};
} // namespace Editor::Core
} // namespace NLS
