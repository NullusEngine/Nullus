#include "UIInputRouter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NLS::Engine::UI
{
    UIElement* UIInputRouter::HitTest(const UIVector2 position) const
    {
        return m_root ? m_root->HitTest(position) : nullptr;
    }

    bool UIInputRouter::DispatchPointer(
        const UIEventType type,
        const UIVector2 position,
        const uint32_t pointerId,
        const UIVector2 delta,
        const uint64_t timestamp,
        const uint32_t modifiers,
        const UIInputDevice device,
        const uint32_t viewportId)
    {
        UIElement* target = nullptr;
        const auto capture = m_pointerCapture.find(pointerId);
        if (capture != m_pointerCapture.end())
            target = capture->second;
        if (!target)
            target = HitTest(position);

        if (type == UIEventType::PointerMove)
        {
            if (target != m_hovered)
            {
                if (m_hovered)
                {
                    m_hovered->SetHovered(false);
                    UIEvent leave;
                    leave.type = UIEventType::HoverLeave;
                    leave.target = m_hovered;
                    leave.position = position;
                    leave.pointerId = pointerId;
                    leave.timestamp = timestamp;
                    leave.device = device;
                    leave.viewportId = viewportId;
                    DispatchToTarget(leave, m_hovered);
                }
                m_hovered = target;
                if (m_hovered)
                {
                    m_hovered->SetHovered(true);
                    UIEvent enter;
                    enter.type = UIEventType::HoverEnter;
                    enter.target = m_hovered;
                    enter.position = position;
                    enter.pointerId = pointerId;
                    enter.timestamp = timestamp;
                    enter.device = device;
                    enter.viewportId = viewportId;
                    DispatchToTarget(enter, m_hovered);
                }
            }
        }
        if (!target)
        {
            // A pointer-up/cancel can legitimately land outside the pressed
            // element when capture was not requested. Always clear the press
            // bookkeeping so controls cannot remain visually pressed forever.
            if (type == UIEventType::PointerUp || type == UIEventType::PointerCancel)
            {
                if (const auto downIterator = m_pointerDownTargets.find(pointerId);
                    downIterator != m_pointerDownTargets.end())
                {
                    if (downIterator->second)
                        downIterator->second->SetPressed(false);
                    m_pointerDownTargets.erase(downIterator);
                }
                ReleasePointer(pointerId);
            }
            return false;
        }

        UIEvent event;
        event.type = type;
        event.position = position;
        event.delta = delta;
        event.pointerId = pointerId;
        event.device = device;
        event.viewportId = viewportId;
        event.timestamp = timestamp;
        event.modifiers = modifiers;
        const bool dispatched = DispatchToTarget(event, target);

        if (type == UIEventType::PointerDown)
        {
            // Keep the press target independently from pointer capture. A
            // control may capture later during dispatch, while a normal
            // click still requires down/up on the same element.
            m_pointerDownTargets[pointerId] = target;
            target->SetPressed(true);
            Focus(target);
        }
        if (type == UIEventType::PointerUp)
        {
            const auto downIterator = m_pointerDownTargets.find(pointerId);
            if (downIterator != m_pointerDownTargets.end() && downIterator->second == target)
            {
                UIEvent click = event;
                click.type = UIEventType::Click;
                DispatchToTarget(click, target);
            }
            if (downIterator != m_pointerDownTargets.end() && downIterator->second != nullptr)
                downIterator->second->SetPressed(false);
            m_pointerDownTargets.erase(pointerId);
            ReleasePointer(pointerId);
        }
        else if (type == UIEventType::PointerCancel)
        {
            if (const auto downIterator = m_pointerDownTargets.find(pointerId);
                downIterator != m_pointerDownTargets.end() && downIterator->second != nullptr)
                downIterator->second->SetPressed(false);
            m_pointerDownTargets.erase(pointerId);
            ReleasePointer(pointerId);
        }
        return dispatched || event.consumed;
    }

    bool UIInputRouter::Dispatch(const UIInputEvent& input)
    {
        switch (input.type)
        {
        case UIEventType::PointerMove:
        case UIEventType::PointerDown:
        case UIEventType::PointerUp:
        case UIEventType::PointerCancel:
            return DispatchPointer(
                input.type,
                input.position,
                input.pointerId,
                input.delta,
                input.timestamp,
                input.modifiers,
                input.device,
                input.viewportId);
        case UIEventType::Scroll:
            return DispatchScroll(input.position, input.wheel, input.timestamp);
        case UIEventType::KeyDown:
        case UIEventType::KeyUp:
        case UIEventType::Cancel:
            return DispatchKey(input.type, input.keyCode, input.modifiers, input.timestamp);
        case UIEventType::Navigation:
        {
            UIEvent event;
            event.type = UIEventType::Navigation;
            event.focusDirection = input.focusDirection;
            event.timestamp = input.timestamp;
            event.device = input.device;
            if (m_focused)
            {
                const bool dispatched = DispatchToTarget(event, m_focused);
                if (!event.defaultPrevented && !event.consumed)
                    Navigate(input.focusDirection, input.timestamp);
                return dispatched || event.consumed;
            }
            return Navigate(input.focusDirection, input.timestamp);
        }
        case UIEventType::TextInput:
            return DispatchTextInput(input.text, input.timestamp);
        case UIEventType::ImeCompositionStart:
        case UIEventType::ImeCompositionUpdate:
        case UIEventType::ImeCompositionCommit:
        {
            if (!m_focused)
                return false;
            UIEvent event;
            event.type = input.type;
            event.target = m_focused;
            event.text = input.text;
            event.timestamp = input.timestamp;
            event.device = input.device;
            event.deviceId = input.deviceId;
            event.isRepeat = input.isRepeat;
            return DispatchToTarget(event, m_focused) || event.consumed;
        }
        default:
            return false;
        }
    }

    bool UIInputRouter::DispatchScroll(const UIVector2 position, const float wheel, const uint64_t timestamp)
    {
        UIEvent event;
        event.type = UIEventType::Scroll;
        event.position = position;
        event.wheel = wheel;
        event.timestamp = timestamp;
        auto* target = HitTest(position);
        return target ? (DispatchToTarget(event, target) || event.consumed) : false;
    }

    bool UIInputRouter::DispatchKey(
        const UIEventType type,
        const uint32_t keyCode,
        const uint32_t modifiers,
        const uint64_t timestamp)
    {
        if (!m_focused)
            return false;
        UIEvent event;
        event.type = type;
        event.target = m_focused;
        event.keyCode = keyCode;
        event.modifiers = modifiers;
        event.timestamp = timestamp;
        return DispatchToTarget(event, m_focused) || event.consumed;
    }

    bool UIInputRouter::DispatchTextInput(std::string text, const uint64_t timestamp)
    {
        if (!m_focused)
            return false;
        UIEvent event;
        event.type = UIEventType::TextInput;
        event.target = m_focused;
        event.text = std::move(text);
        event.timestamp = timestamp;
        return DispatchToTarget(event, m_focused) || event.consumed;
    }

    bool UIInputRouter::Navigate(const UIFocusDirection direction, const uint64_t timestamp)
    {
        if (!m_root || direction == UIFocusDirection::None)
            return false;

        std::vector<UIElement*> focusable;
        CollectFocusable(*m_root, focusable);
        if (focusable.empty())
            return false;

        auto current = std::find(focusable.begin(), focusable.end(), m_focused);
        size_t index = current == focusable.end() ? 0u : static_cast<size_t>(current - focusable.begin());
        if (direction == UIFocusDirection::Previous)
            index = current == focusable.end() || index == 0u ? focusable.size() - 1u : index - 1u;
        else if (direction == UIFocusDirection::Next || current == focusable.end())
            index = current == focusable.end() ? 0u : (index + 1u) % focusable.size();
        else
        {
            // Prefer the nearest element in the requested half-plane. This is
            // the same predictable spatial policy used by console UI systems;
            // document order remains the fallback when no candidate exists.
            if (current != focusable.end())
            {
                const auto& currentRect = (*current)->GetLayoutRect();
                const UIVector2 currentCenter {
                    currentRect.x + currentRect.width * 0.5f,
                    currentRect.y + currentRect.height * 0.5f };
                UIElement* best = nullptr;
                float bestPrimary = std::numeric_limits<float>::max();
                float bestSecondary = std::numeric_limits<float>::max();
                for (auto* candidate : focusable)
                {
                    if (!candidate || candidate == *current)
                        continue;
                    const auto& candidateRect = candidate->GetLayoutRect();
                    const UIVector2 candidateCenter {
                        candidateRect.x + candidateRect.width * 0.5f,
                        candidateRect.y + candidateRect.height * 0.5f };
                    const float dx = candidateCenter.x - currentCenter.x;
                    const float dy = candidateCenter.y - currentCenter.y;
                    float primary = 0.0f;
                    float secondary = 0.0f;
                    switch (direction)
                    {
                    case UIFocusDirection::Up:
                        if (dy >= -0.001f) continue;
                        primary = -dy;
                        secondary = std::abs(dx);
                        break;
                    case UIFocusDirection::Down:
                        if (dy <= 0.001f) continue;
                        primary = dy;
                        secondary = std::abs(dx);
                        break;
                    case UIFocusDirection::Left:
                        if (dx >= -0.001f) continue;
                        primary = -dx;
                        secondary = std::abs(dy);
                        break;
                    case UIFocusDirection::Right:
                        if (dx <= 0.001f) continue;
                        primary = dx;
                        secondary = std::abs(dy);
                        break;
                    default:
                        continue;
                    }
                    if (primary < bestPrimary ||
                        (std::abs(primary - bestPrimary) <= 0.001f && secondary < bestSecondary))
                    {
                        best = candidate;
                        bestPrimary = primary;
                        bestSecondary = secondary;
                    }
                }
                if (best)
                    index = static_cast<size_t>(std::find(focusable.begin(), focusable.end(), best) - focusable.begin());
                else
                    index = static_cast<size_t>((current - focusable.begin() + 1u) % focusable.size());
            }
            else
            {
                index = 0u;
            }
        }

        Focus(focusable[index]);
        UIEvent event;
        event.type = UIEventType::Navigation;
        event.focusDirection = direction;
        event.timestamp = timestamp;
        event.target = focusable[index];
        event.currentTarget = focusable[index];
        return DispatchToTarget(event, focusable[index]) || event.consumed;
    }

    void UIInputRouter::CapturePointer(const uint32_t pointerId, UIElement& element)
    {
        m_pointerCapture[pointerId] = &element;
    }

    void UIInputRouter::ReleasePointer(const uint32_t pointerId)
    {
        m_pointerCapture.erase(pointerId);
    }

    void UIInputRouter::Focus(UIElement* element)
    {
        if (m_focused == element)
            return;
        if (m_focused)
        {
            m_focused->SetFocused(false);
            UIEvent lost;
            lost.type = UIEventType::FocusLost;
            lost.target = m_focused;
            DispatchToTarget(lost, m_focused);
        }
        m_focused = element;
        if (m_focused)
        {
            m_focused->SetFocused(true);
            UIEvent gained;
            gained.type = UIEventType::FocusGained;
            gained.target = m_focused;
            DispatchToTarget(gained, m_focused);
        }
    }

    void UIInputRouter::Clear()
    {
        m_pointerCapture.clear();
        for (const auto& [pointerId, element] : m_pointerDownTargets)
            if (element)
                element->SetPressed(false);
        m_pointerDownTargets.clear();
        if (m_hovered)
            m_hovered->SetHovered(false);
        if (m_focused)
            m_focused->SetFocused(false);
        m_hovered = nullptr;
        m_focused = nullptr;
        m_root = nullptr;
    }

    bool UIInputRouter::DispatchToTarget(UIEvent& event, UIElement* target)
    {
        if (!target)
            return false;
        event.target = target;
        const auto path = BuildPath(target);
        for (auto iterator = path.begin(); iterator != path.end() && *iterator != target &&
             !event.propagationStopped; ++iterator)
        {
            event.phase = UIEventPhase::Capture;
            event.currentTarget = *iterator;
            (*iterator)->InvokeEventListeners(event, true);
        }
        if (!event.propagationStopped)
        {
            event.phase = UIEventPhase::Target;
            event.currentTarget = target;
            target->InvokeEventListeners(event, true);
            target->InvokeEventListeners(event, false);
        }
        for (auto iterator = path.rbegin(); iterator != path.rend() && !event.propagationStopped; ++iterator)
        {
            if (*iterator == target)
                continue;
            event.phase = UIEventPhase::Bubble;
            event.currentTarget = *iterator;
            (*iterator)->InvokeEventListeners(event, false);
        }
        if (!event.defaultPrevented)
        {
            event.phase = UIEventPhase::DefaultAction;
            event.currentTarget = target;
            target->PerformDefaultAction(event);
        }
        return true;
    }

    std::vector<UIElement*> UIInputRouter::BuildPath(UIElement* target) const
    {
        std::vector<UIElement*> path;
        for (auto* current = target; current != nullptr; current = current->GetParent())
            path.push_back(current);
        std::reverse(path.begin(), path.end());
        return path;
    }

    void UIInputRouter::CollectFocusable(UIElement& element, std::vector<UIElement*>& output) const
    {
        if (element.GetStyle().visible && element.GetStyle().enabled &&
            element.GetStyle().interactive)
            output.push_back(&element);
        for (const auto& child : element.GetChildren())
            if (child)
                CollectFocusable(*child, output);
    }
}
