#pragma once

#include "UIElement.h"
#include "InputEventQueue.h"
#include "EngineDef.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace NLS::Engine::UI
{
    class NLS_ENGINE_API UIInputRouter final
    {
    public:
        void SetRoot(UIElement* root) { m_root = root; }
        [[nodiscard]] UIElement* GetRoot() const { return m_root; }
        [[nodiscard]] UIElement* GetHoveredElement() const { return m_hovered; }
        [[nodiscard]] UIElement* GetFocusedElement() const { return m_focused; }

        UIElement* HitTest(UIVector2 position) const;
        bool DispatchPointer(
            UIEventType type,
            UIVector2 position,
            uint32_t pointerId = 0u,
            UIVector2 delta = {},
            uint64_t timestamp = 0u,
            uint32_t modifiers = 0u,
            UIInputDevice device = UIInputDevice::Unknown,
            uint32_t viewportId = 0u);
        bool Dispatch(const UIInputEvent& event);
        bool DispatchScroll(UIVector2 position, float wheel, uint64_t timestamp = 0u);
        bool DispatchKey(UIEventType type, uint32_t keyCode, uint32_t modifiers = 0u, uint64_t timestamp = 0u);
        bool DispatchTextInput(std::string text, uint64_t timestamp = 0u);
        bool Navigate(UIFocusDirection direction, uint64_t timestamp = 0u);

        void CapturePointer(uint32_t pointerId, UIElement& element);
        void ReleasePointer(uint32_t pointerId);
        void Focus(UIElement* element);
        void ClearFocus() { Focus(nullptr); }
        void Clear();

    private:
        bool DispatchToTarget(UIEvent& event, UIElement* target);
        std::vector<UIElement*> BuildPath(UIElement* target) const;
        void CollectFocusable(UIElement& element, std::vector<UIElement*>& output) const;

        UIElement* m_root = nullptr;
        UIElement* m_hovered = nullptr;
        UIElement* m_focused = nullptr;
        std::unordered_map<uint32_t, UIElement*> m_pointerCapture;
        std::unordered_map<uint32_t, UIElement*> m_pointerDownTargets;
    };
}
