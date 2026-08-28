#pragma once

#include "EngineDef.h"
#include "UIEvent.h"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace NLS::Engine::UI
{
    // Game and editor input producers publish this value-type queue. The UI
    // tree is still mutated only by the main thread; render threads never see
    // queued events or element pointers.
    struct NLS_ENGINE_API UIInputEvent
    {
        UIEventType type = UIEventType::PointerMove;
        UIInputDevice device = UIInputDevice::Unknown;
        uint64_t timestamp = 0u;
        uint32_t pointerId = 0u;
        uint32_t viewportId = 0u;
        uint32_t keyCode = 0u;
        uint32_t modifiers = 0u;
        UIVector2 position {};
        UIVector2 delta {};
        float wheel = 0.0f;
        std::string text;
        bool consumed = false;
        uint32_t deviceId = 0u;
        uint32_t repeatCount = 0u;
        bool isRepeat = false;
        UIFocusDirection focusDirection = UIFocusDirection::None;
        float dpiScale = 1.0f;
        UIVector2 viewportSize {};
    };

    class NLS_ENGINE_API InputEventQueue final
    {
    public:
        void Push(UIInputEvent event) { m_events.push_back(std::move(event)); }

        [[nodiscard]] bool Empty() const { return m_events.empty(); }
        [[nodiscard]] size_t Size() const { return m_events.size(); }

        bool TryPop(UIInputEvent& output)
        {
            if (m_events.empty())
                return false;
            output = std::move(m_events.front());
            m_events.pop_front();
            return true;
        }

        std::vector<UIInputEvent> Drain()
        {
            std::vector<UIInputEvent> result;
            result.reserve(m_events.size());
            while (!m_events.empty())
            {
                result.push_back(std::move(m_events.front()));
                m_events.pop_front();
            }
            return result;
        }

        void Clear() { m_events.clear(); }

    private:
        std::deque<UIInputEvent> m_events;
    };
}
