#pragma once
#include "ScriptingDef.h"
#include <Math/Vector2.h>
#include <array>
#include <cstdint>
#include <string>
#include <span>
#include <Input/InputTypes.h>

namespace NLS::Windowing::Inputs { class InputManager; }
namespace NLS::Scripting
{
struct NLS_SCRIPTING_API GameInputFrame
{
    uint64_t sessionId = 0;
    uint64_t frameId = 0;
    uint32_t width = 0, height = 0;
    bool focused = false;
    bool pointerAvailable = true;
    std::array<bool, 512> keys {}, pressed {}, released {};
    std::array<bool, 8> mouse {}, mousePressed {}, mouseReleased {};
    Maths::Vector2 position; // output pixels, top-left origin
    Maths::Vector2 delta;
    Maths::Vector2 scroll;
    std::string text;
    std::string composition;
    uint64_t compositionSequence = 0;
    bool composing = false;
    // Borrowed until the host's EndFrame; UI consumes the same ordered stream.
    std::span<const Input::InputEvent> orderedEvents;
    bool hasOrderedEvents = false;
    uint64_t sourceInputFrame = 0;
    Maths::Vector2 pointerOrigin;
    Maths::Vector2 pointerScale{1, 1};
    static GameInputFrame Capture(const Windowing::Inputs::InputManager& input);
    Maths::Vector2 ScriptMousePosition() const { return { position.x, static_cast<float>(height) - position.y }; }
};
}
