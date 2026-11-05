#include "InputDisplay.h"
#include <array>
namespace NLS::Windowing::Inputs
{
std::string GetControlDisplayName(const Input::ControlPath& path)
{
    using namespace Input;
    if (path.kind == ControlKind::PointerDelta) return "Pointer Delta";
    if (path.kind == ControlKind::PointerPosition) return "Pointer Position";
    if (path.kind == ControlKind::Wheel) return "Mouse Wheel";
    if (path.device == DeviceKind::Mouse) return "Mouse " + std::to_string(path.control + 1);
    if (path.device == DeviceKind::Gamepad) return std::string(path.kind == ControlKind::Axis ? "Gamepad Axis " : "Gamepad Button ") + std::to_string(path.control);
    if (path.kind == ControlKind::ScanCode) return "Scan " + std::to_string(path.code);
    if (path.kind == ControlKind::LogicalKey)
        return path.code >= 32 && path.code < 127 ? "Logical " + std::string(1, static_cast<char>(path.code)) : "Unicode " + std::to_string(path.code);
    const auto key = path.control;
    if (key >= 33 && key <= 96) return std::string(1, static_cast<char>(key));
    if (key >= 290 && key <= 314) return "F" + std::to_string(key - 289);
    if (key >= 320 && key <= 329) return "Keypad " + std::to_string(key - 320);
    switch (key)
    {
    case 32: return "Space"; case 256: return "Esc"; case 257: return "Enter"; case 258: return "Tab";
    case 259: return "Backspace"; case 260: return "Insert"; case 261: return "Delete";
    case 262: return "Right"; case 263: return "Left"; case 264: return "Down"; case 265: return "Up";
    case 266: return "Page Up"; case 267: return "Page Down"; case 268: return "Home"; case 269: return "End";
    case 340: case 344: return "Shift"; case 341: case 345: return "Ctrl"; case 342: case 346: return "Alt"; case 343: case 347: return "Super";
    default: return "Key " + std::to_string(key);
    }
}
std::string GetBindingDisplayName(const Input::InputUser& user, Input::BindingId binding)
{
    using namespace Input;
    const auto parts = user.GetEffectiveBinding(binding);
    if (parts.empty()) return "Unassigned";
    auto chord = user.GetEffectiveChord(binding);
    auto modifiers = static_cast<uint8_t>(chord.modifiers);
    if (modifiers & 16u)
    {
#ifdef __APPLE__
        modifiers |= 8u;
#else
        modifiers |= 2u;
#endif
    }
    std::string result;
    auto append = [&](const std::string& value) { if (!result.empty()) result += " + "; result += value; };
#ifdef __APPLE__
    if (modifiers & 8u) append("Cmd");
    if (modifiers & 1u) append("Shift");
    if (modifiers & 4u) append("Alt");
    if (modifiers & 2u) append("Ctrl");
#else
    if (modifiers & 2u) append("Ctrl");
    if (modifiers & 1u) append("Shift");
    if (modifiers & 4u) append("Alt");
    if (modifiers & 8u) append("Super");
#endif
    for (const auto& control : chord.controls) append(GetControlDisplayName(control));
    for (const auto& part : parts) append(GetControlDisplayName(part.path));
    return result;
}
}
