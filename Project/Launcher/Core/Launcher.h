
#pragma once

#include <Windowing/Context/Device.h>
#include <Windowing/Window.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Settings/EGraphicsBackend.h>
#include <Rendering/Settings/DriverSettings.h>
#include <UI/UIManager.h>
#include <UI/Panels/PanelWindow.h>

#include "Core/TemplateManager.h"

namespace NLS::Render::Resources
{
    class Texture2D;
}

namespace NLS::Render::RHI
{
    class RHITextureView;
}

namespace NLS
{
struct LauncherRunResult
{
    bool ready = false;
    std::string projectPath;
    std::string projectName;
    std::string editorExecutablePath;
};

/**
 * A simple panel that allow the user to select the project to launch
 */
class Launcher
{
public:
    /**
     * Constructor
     * @param backendOverride Optional backend override from command line
     * @param renderDocSettings Optional RenderDoc settings from command line
     */
    Launcher(
        std::optional<Render::Settings::EGraphicsBackend> backendOverride = std::nullopt,
        const Render::Settings::RenderDocSettings& renderDocSettings = {});

    ~Launcher();

    /**
     * Run the project hub logic
     */
    LauncherRunResult Run();

    /**
     * Setup the project hub specific context (minimalist context)
     */
    void SetupContext();

    /**
     * Register the project (identified from the given path) into the project hub
     * @param p_path
     */
    void RegisterProject(const std::string& p_path);

private:
    std::shared_ptr<NLS::Render::RHI::RHITextureView> m_brandTextureView;
    NLS::Render::Resources::Texture2D* m_brandTextureResource = nullptr;

    std::unique_ptr<Context::Device> m_device;
    std::unique_ptr<Windowing::Window> m_window;
    std::unique_ptr<Render::Context::Driver> m_driver;
    std::unique_ptr<UI::UIManager> m_uiManager;

    UI::Canvas m_canvas;
    std::unique_ptr<UI::PanelWindow> m_mainPanel;

    std::string m_projectPath = "";
    std::string m_projectName = "";
    std::string m_editorExecutablePath = "";
    bool m_readyToGo = false;
    Render::Settings::EGraphicsBackend m_graphicsBackend = Render::Settings::EGraphicsBackend::NONE;
    std::optional<Render::Settings::EGraphicsBackend> m_backendOverride = std::nullopt;
    Render::Settings::RenderDocSettings m_renderDocSettings;

    TemplateManager m_templateManager;
};
} // namespace NLS
