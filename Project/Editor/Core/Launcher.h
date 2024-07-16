
#pragma once

#include <Windowing/Context/Device.h>
#include <Windowing/Window.h>
#include <Rendering/Context/Driver.h>
#include <UI/UIManager.h>
#include <UI/Panels/PanelWindow.h>

namespace NLS
{
/**
 * A simple panel that allow the user to select the project to launch
 */
class Launcher
{
public:
    /**
     * Constructor
     */
    Launcher();

    /**
     * Run the project hub logic
     */
    std::tuple<bool, std::string, std::string> Run();

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
    std::unique_ptr<Context::Device> m_device;
    std::unique_ptr<Windowing::Window> m_window;
    std::unique_ptr<Render::Context::Driver> m_driver;
    std::unique_ptr<UI::UIManager> m_uiManager;

    UI::Modules::Canvas m_canvas;
    std::unique_ptr<UI::Panels::PanelWindow> m_mainPanel;

    std::string m_projectPath = "";
    std::string m_projectName = "";
    bool m_readyToGo = false;
};
} // namespace NLS