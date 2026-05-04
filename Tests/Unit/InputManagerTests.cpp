#include <gtest/gtest.h>

#include "Windowing/Context/Device.h"
#include "Windowing/Inputs/InputManager.h"
#include "Windowing/Settings/WindowSettings.h"
#include "Windowing/Window.h"

using namespace NLS;

TEST(InputManagerTests, ClearEventsClearsWheelMovement)
{
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
