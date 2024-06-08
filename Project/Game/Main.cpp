#include "Windowing/Window.h"
#include "Windowing/Inputs/InputManager.h"
#include "Time/Clock.h"
#include "Assembly.h"
#include "AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"
#include "Game.h"
using namespace NLS;
using namespace Engine;

int main()
{
    Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<AssemblyEngine>();

    /* Settings */
    NLS::Settings::DeviceSettings deviceSettings;
    NLS::Settings::WindowSettings windowSettings;
    windowSettings.title = "Nullus";
    windowSettings.width = 1280;
    windowSettings.height = 720;
    /* Window creation */
    auto device = std::make_unique<NLS::Context::Device>(deviceSettings);
    auto window = std::make_unique<NLS::Window>(*device, windowSettings);
    auto inputManager = std::make_unique<NLS::Inputs::InputManager>(*window);
    window->MakeCurrentContext();

    if (!window->GetGlfwWindow())
    {
        return -1;
    }
    srand(time(0));
    window->SetCursorMode(Cursor::ECursorMode::DISABLED);

    Game* g = new Game();
    Time::Clock clock;
    while (!window->ShouldClose() && !inputManager->IsKeyPressed(Inputs::EKey::KEY_ESCAPE))
    {
        device->PollEvents();
        float dt = clock.GetDeltaTime();

        if (inputManager->IsKeyPressed(Inputs::EKey::KEY_F1))
        {
            window->ShowConsole(true);
        }
        if (inputManager->IsKeyPressed(Inputs::EKey::KEY_F2))
        {
            window->ShowConsole(false);
        }
        if (inputManager->IsKeyPressed(Inputs::EKey::KEY_F11))
        {
            window->ToggleFullscreen();
        }

        window->SetTitle("FPS:" + std::to_string(clock.GetFramerate()));
        g->UpdateGame(dt);
        inputManager->Update();
        clock.Update();
        inputManager->ClearEvents();
    }

}