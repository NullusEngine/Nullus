#pragma once

#include "UICommon.h"

#include <cstdint>
#include <string>

namespace NLS::Engine::UI
{
    class UIElement;

    enum class UIEventType : uint8_t
    {
        PointerMove,
        PointerDown,
        PointerUp,
        PointerCancel,
        Click,
        HoverEnter,
        HoverLeave,
        Scroll,
        KeyDown,
        KeyUp,
        TextInput,
        ImeCompositionStart,
        ImeCompositionUpdate,
        ImeCompositionCommit,
        Navigation,
        WindowDpiChanged,
        WindowSafeAreaChanged,
        FocusGained,
        FocusLost,
        ValueChanged,
        Submit,
        Cancel,
        Count
    };

    enum class UIInputDevice : uint8_t
    {
        Unknown,
        Mouse,
        Keyboard,
        Gamepad,
        Text,
        Ime,
        Window
    };

    enum UIInputModifier : uint32_t
    {
        UIInputModifierNone = 0u,
        UIInputModifierShift = 1u << 0u,
        UIInputModifierControl = 1u << 1u,
        UIInputModifierAlt = 1u << 2u,
        UIInputModifierSuper = 1u << 3u
    };

    enum class UIFocusDirection : uint8_t
    {
        None,
        Up,
        Down,
        Left,
        Right,
        Next,
        Previous
    };

    enum class UIEventPhase : uint8_t
    {
        Capture,
        Target,
        Bubble,
        DefaultAction
    };

    struct UIEvent
    {
        UIEventType type = UIEventType::PointerMove;
        UIEventPhase phase = UIEventPhase::Target;
        UIElement* target = nullptr;
        UIElement* currentTarget = nullptr;
        UIInputDevice device = UIInputDevice::Unknown;
        UIVector2 position {};
        UIVector2 delta {};
        float wheel = 0.0f;
        uint32_t pointerId = 0u;
        uint32_t viewportId = 0u;
        uint32_t keyCode = 0u;
        uint32_t modifiers = 0u;
        uint64_t timestamp = 0u;
        std::string text;
        uint32_t deviceId = 0u;
        uint32_t repeatCount = 0u;
        bool isRepeat = false;
        UIFocusDirection focusDirection = UIFocusDirection::None;
        float dpiScale = 1.0f;
        UIVector2 viewportSize {};
        bool consumed = false;
        bool defaultPrevented = false;
        bool propagationStopped = false;

        void StopPropagation()
        {
            propagationStopped = true;
            consumed = true;
        }

        void PreventDefault()
        {
            defaultPrevented = true;
            consumed = true;
        }
    };
}
