#pragma once

#include "InputDef.h"
#include <Math/Vector2.h>
#include <array>
#include <cstdint>
#include <string>

namespace NLS::Input
{
struct DeviceId
{
    uint64_t value = 0;
    explicit operator bool() const { return value != 0; }
    friend bool operator==(DeviceId, DeviceId) = default;
};

struct WindowId
{
    uint64_t value = 0;
    explicit operator bool() const { return value != 0; }
    friend bool operator==(WindowId, WindowId) = default;
};

enum class DeviceKind : uint8_t { Keyboard, Mouse, Gamepad };
enum class IngressKind : uint8_t { Virtual, Glfw, Win32 };
enum class InputEventType : uint8_t
{
    DeviceAdded, DeviceRemoved, ButtonDown, ButtonUp, ButtonRepeat,
    Axis, PointerMove, PointerDelta, Wheel, FocusGained, FocusLost,
    CaptureLost, DeviceReset
};
enum class InputEventFlags : uint8_t
{
    None = 0, Resynchronize = 1, PointerWarp = 2, ScreenPosition = 4, FocusReset = 8
};

struct InputEvent
{
    uint64_t sequence = 0;
    double timestamp = -1.0;
    DeviceId device;
    WindowId window;
    InputEventType type = InputEventType::ButtonDown;
    IngressKind ingress = IngressKind::Virtual;
    InputEventFlags flags = InputEventFlags::None;
    uint16_t control = 0;
    int32_t scanCode = 0;
    uint32_t modifiers = 0;
    uint32_t logicalKey = 0;
    double x = 0.0;
    double y = 0.0;
    double screenX = 0.0;
    double screenY = 0.0;
};

struct DeviceDescriptor
{
    DeviceKind kind = DeviceKind::Keyboard;
    std::string name;
    std::string layout;
    std::string hardwareGuid;
    bool rawPointerMotion = false;
};

struct DeviceState
{
    DeviceId id;
    DeviceDescriptor descriptor;
    bool connected = false;
    bool synchronized = true;
    bool hasPointerPosition = false;
    bool hasScreenPosition = false;
    std::array<bool, 512> buttons{};
    std::array<bool, 512> pressed{};
    std::array<bool, 512> released{};
    std::array<float, 16> axes{};
    Maths::Vector2 position{};
    Maths::Vector2 screenPosition{};
    Maths::Vector2 delta{};
    Maths::Vector2 wheel{};
};

struct GamepadState
{
    DeviceId id;
    bool connected = false;
    bool mapped = false;
    std::array<bool, 15> buttons{};
    std::array<float, 6> axes{};
};

struct WindowState
{
    WindowId id;
    uintptr_t nativeHandle = 0;
    IngressKind ingress = IngressKind::Virtual;
    bool registered = false;
    bool focused = false;
};
}
