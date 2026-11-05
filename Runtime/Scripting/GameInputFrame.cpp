#include "GameInputFrame.h"
#include <Windowing/Inputs/InputManager.h>
namespace NLS::Scripting
{
GameInputFrame GameInputFrame::Capture(const Windowing::Inputs::InputManager& input)
{
    using namespace Windowing::Inputs;
    GameInputFrame frame;
    frame.focused = true;
    for (int i = 32; i <= 348; ++i)
    {
        const auto key = static_cast<EKey>(i);
        frame.keys[i] = input.GetKeyState(key) == EKeyState::KEY_DOWN;
        frame.pressed[i] = input.IsKeyPressed(key);
        frame.released[i] = input.IsKeyReleased(key);
    }
    for (int i = 0; i < 8; ++i)
    {
        const auto button = static_cast<EMouseButton>(i);
        frame.mouse[i] = input.GetMouseButtonState(button) == EMouseButtonState::MOUSE_DOWN;
        frame.mousePressed[i] = input.IsMouseButtonPressed(button);
        frame.mouseReleased[i] = input.IsMouseButtonReleased(button);
    }
    frame.position = input.GetMousePosition();
    frame.delta = input.GetMouseDelta();
    frame.orderedEvents = input.GetInputSystem().GetEvents();
    frame.hasOrderedEvents = true;
    frame.sourceInputFrame = input.GetInputSystem().GetFrameIndex();
    const auto* mouse = input.GetInputSystem().FindDevice(input.GetInputSystem().GetMouse());
    if (mouse && mouse->hasScreenPosition) frame.pointerOrigin = mouse->screenPosition - frame.position;
    frame.scroll = input.GetWheelMovement();
    frame.text = input.GetTextInput();
    const auto& text = input.GetPlatformBackend().GetTextInput();
    frame.composition = text.GetCompositionText(); frame.compositionSequence = text.GetComposition().sequence; frame.composing = text.GetComposition().active;
    return frame;
}
}
