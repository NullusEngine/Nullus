#pragma once
#include <cstdint>

namespace NLS::Windowing::Inputs
{
enum class WindowInputEventType : uint8_t
{
    KeyDown, KeyUp, KeyRepeat, MouseDown, MouseUp, PointerMove, Wheel,
    Character, FocusGained, FocusLost, CaptureLost
};
struct WindowInputEvent
{
    WindowInputEventType type = WindowInputEventType::KeyDown;
    int key = 0;
    int scanCode = 0;
    unsigned int modifiers = 0;
    double x = 0.0, y = 0.0;
    unsigned int character = 0;
    bool pointerWarp = false;
};
class PlatformInputBackend;
}
