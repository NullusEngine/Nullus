#pragma once

#include <Windowing/Context/Device.h>
#include <Windowing/Inputs/InputManager.h>
#include <Windowing/Window.h>
#include "UI/UIManager.h"
#include "Context/Driver.h"
#include <memory>
#include <optional>
#include "SceneSystem/SceneManager.h"
#include "Filesystem/IniFile.h"
#include "ResourceManagement/ModelManager.h"
#include "ResourceManagement/TextureManager.h"
#include "ResourceManagement/ShaderManager.h"
#include "ResourceManagement/MaterialManager.h"
#include "Resource/Actor/ActorManager.h"
#include "EditorResources.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"
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
     * @param p_backendOverride optional backend override from command line, if not provided uses project settings
     * @param p_renderDocSettings RenderDoc settings from command line
     */
    Context(const std::string& p_projectPath, const std::string& p_projectName,
            std::optional<Render::Settings::EGraphicsBackend> p_backendOverride = std::nullopt,
            const Render::Settings::RenderDocSettings& p_renderDocSettings = {});

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
    NLS::Engine::ActorManager actorManager;

    NLS::Windowing::Settings::WindowSettings windowSettings;

    NLS::Filesystem::IniFile projectSettings;

private:
    std::optional<Render::Settings::EGraphicsBackend> m_backendOverride;
    Render::Settings::RenderDocSettings m_renderDocSettings;
};
} // namespace Editor::Core
} // namespace NLS
