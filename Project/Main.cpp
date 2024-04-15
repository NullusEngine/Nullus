#include "Windowing/Window.h"
#include "Windowing/Inputs/InputManager.h"
#include "Time/Clock.h"
#include "AI/StateMachine.h"
#include "AI/StateTransition.h"
#include "AI/State.h"

#include "Pathfinding/NavigationGrid.h"

#include "TutorialGame.h"
#include "Assembly.h"
#include "AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"

using namespace NLS;
using namespace Engine;

/*

The main function should look pretty familar to you!
We make a window, and then go into a while loop that repeatedly
runs our 'game' until we press escape. Instead of making a 'renderer'
and updating it, we instead make a whole game, and repeatedly update that,
instead.

This time, we've added some extra functionality to the window class - we can
hide or show the

*/


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

    TutorialGame* g = new TutorialGame();
    Time::Clock clock;
    while (!window->ShouldClose() && !inputManager->IsKeyPressed(Inputs::EKey::KEY_ESCAPE))
    {
        device->PollEvents();
        float dt = clock.GetDeltaTime();
        if (dt > 0.1f)
        {
            std::cout << "Skipping large time delta" << std::endl;
            continue; // must have hit a breakpoint or something to have a 1 second frame time!
        }
        if (inputManager->IsKeyPressed(Inputs::EKey::KEY_F1))
        {
            window->ShowConsole(true);
        }
        if (inputManager->IsKeyPressed(Inputs::EKey::KEY_F2))
        {
            window->ShowConsole(false);
        }

        if (inputManager->IsKeyPressed(Inputs::EKey::KEY_T))
        {
            window->SetPosition(0, 0);
        }

        window->SetTitle("Gametech frame time:" + std::to_string(1000.0f * dt));

        g->UpdateGame(dt);
        inputManager->Update();
        clock.Update();
    }

}