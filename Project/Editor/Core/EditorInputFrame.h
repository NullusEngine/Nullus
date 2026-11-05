#pragma once
#include <Scripting/GameInputFrame.h>
#include <Windowing/Inputs/InputManager.h>

namespace NLS::Editor::Core
{
inline NLS::Scripting::GameInputFrame CaptureEditorInputFrame(const Windowing::Inputs::InputManager& fallback)
{
    auto frame = NLS::Scripting::GameInputFrame::Capture(fallback);
    if (const auto* mouse = fallback.GetInputSystem().FindDevice(fallback.GetInputSystem().GetMouse()); mouse && mouse->hasScreenPosition)
        frame.position = mouse->screenPosition;
    frame.pointerOrigin = {};
    return frame;
}
}
