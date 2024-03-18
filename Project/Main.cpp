#ifdef _WIN32
    #include "Win32/Win32Window.h"
#endif

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

    Window* w = Window::CreateGameWindow<Win32Code::Win32Window>("Engine Game technology!", 1280, 720);

    if (!w->HasInitialised())
    {
        return -1;
    }
    srand(time(0));
    w->ShowOSPointer(false);
    w->LockMouseToWindow(true);

    TutorialGame* g = new TutorialGame();
    w->GetTimer()->GetTimeDeltaSeconds(); // Clear the timer so we don't get a larget first dt!
    while (w->UpdateWindow() && !Window::GetKeyboard()->KeyDown(KeyboardKeys::ESCAPE))
    {
        float dt = w->GetTimer()->GetTimeDeltaSeconds();
        if (dt > 0.1f)
        {
            std::cout << "Skipping large time delta" << std::endl;
            continue; // must have hit a breakpoint or something to have a 1 second frame time!
        }
        if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::PRIOR))
        {
            w->ShowConsole(true);
        }
        if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::NEXT))
        {
            w->ShowConsole(false);
        }

        if (Window::GetKeyboard()->KeyPressed(KeyboardKeys::T))
        {
            w->SetWindowPosition(0, 0);
        }

        w->SetTitle("Gametech frame time:" + std::to_string(1000.0f * dt));

        g->UpdateGame(dt);
    }
    Window::DestroyGameWindow();
}