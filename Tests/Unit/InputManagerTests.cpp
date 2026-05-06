#include <gtest/gtest.h>

#include <cstdlib>

#include "Windowing/Context/Device.h"
#include "Windowing/Inputs/InputManager.h"
#include "Windowing/Settings/WindowSettings.h"
#include "Windowing/Window.h"

using namespace NLS;

namespace
{
bool CanCreateHeadlessGlfwWindow()
{
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return std::getenv("DISPLAY") != nullptr || std::getenv("WAYLAND_DISPLAY") != nullptr;
#endif
}
}

TEST(InputManagerTests, ClearEventsClearsWheelMovement)
{
    if (!CanCreateHeadlessGlfwWindow())
        GTEST_SKIP() << "GLFW display is not available in this environment.";

    Windowing::Settings::DeviceSettings deviceSettings;
    Context::Device device(deviceSettings);

    Windowing::Settings::WindowSettings windowSettings;
    windowSettings.title = "InputManagerTests";
    windowSettings.width = 64;
    windowSettings.height = 64;
    windowSettings.visible = false;
    windowSettings.clientAPI = Windowing::Settings::WindowClientAPI::NoAPI;

    Windowing::Window window(device, windowSettings);
    Windowing::Inputs::InputManager inputManager(window);

    window.MouseScrollEvent.Invoke(0.0, -1.0);
    ASSERT_FLOAT_EQ(inputManager.GetWheelMovement().y, -1.0f);

    inputManager.ClearEvents();

    EXPECT_FLOAT_EQ(inputManager.GetWheelMovement().x, 0.0f);
    EXPECT_FLOAT_EQ(inputManager.GetWheelMovement().y, 0.0f);
}

TEST(InputManagerTests, MousePressAndReleaseInSameFrameAreBothVisible)
{
    if (!CanCreateHeadlessGlfwWindow())
        GTEST_SKIP() << "GLFW display is not available in this environment.";

    Windowing::Settings::DeviceSettings deviceSettings;
    Context::Device device(deviceSettings);

    Windowing::Settings::WindowSettings windowSettings;
    windowSettings.title = "InputManagerSameFrameMouseClickTests";
    windowSettings.width = 64;
    windowSettings.height = 64;
    windowSettings.visible = false;
    windowSettings.clientAPI = Windowing::Settings::WindowClientAPI::NoAPI;

    Windowing::Window window(device, windowSettings);
    Windowing::Inputs::InputManager inputManager(window);

    window.MouseButtonPressedEvent.Invoke(static_cast<int>(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT));
    window.MouseButtonReleasedEvent.Invoke(static_cast<int>(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT));

    EXPECT_TRUE(inputManager.IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT));
    EXPECT_TRUE(inputManager.IsMouseButtonReleased(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT));

    inputManager.ClearEvents();

    EXPECT_FALSE(inputManager.IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT));
    EXPECT_FALSE(inputManager.IsMouseButtonReleased(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT));
}
