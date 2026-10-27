#include "UIElement.h"

#include "Rendering/UI/UIDrawSnapshot.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace NLS::Engine::UI
{
    namespace
    {
        float ClampDimension(float value, float minimum, float maximum)
        {
            value = std::max(0.0f, value);
            if (minimum > 0.0f)
                value = std::max(value, minimum);
            if (maximum > 0.0f)
                value = std::min(value, maximum);
            return value;
        }

        NLS::Render::UI::UIClipRect IntersectClip(
            const NLS::Render::UI::UIClipRect& lhs,
            const NLS::Render::UI::UIClipRect& rhs)
        {
            return {
                std::max(lhs.left, rhs.left),
                std::max(lhs.top, rhs.top),
                std::min(lhs.right, rhs.right),
                std::min(lhs.bottom, rhs.bottom) };
        }

        NLS::Render::UI::UIClipRect RectToClip(const UIRect& rect)
        {
            return { rect.x, rect.y, rect.x + rect.width, rect.y + rect.height };
        }

        size_t CountUtf8Codepoints(const std::string& text)
        {
            size_t count = 0u;
            for (size_t index = 0u; index < text.size(); ++index)
            {
                const auto byte = static_cast<unsigned char>(text[index]);
                if ((byte & 0xc0u) != 0x80u)
                    ++count;
            }
            return count;
        }
    }

    UIElement::UIElement(
        const UIElementType type,
        std::string name,
        UINodeGuid nodeGuid)
        : m_type(type),
          m_name(std::move(name)),
          m_nodeGuid(nodeGuid.IsValid() ? nodeGuid : UINodeGuid::New()),
          m_style()
    {
        m_style.interactive =
            type == UIElementType::Button ||
            type == UIElementType::Toggle ||
            type == UIElementType::Radio ||
            type == UIElementType::Slider ||
            type == UIElementType::TextField ||
            type == UIElementType::Dropdown ||
            type == UIElementType::ListView;
    }

    void UIElement::SetName(std::string name)
    {
        if (m_name == name)
            return;
        m_name = std::move(name);
        MarkPaintDirty();
    }

    void UIElement::SetClassName(std::string className)
    {
        if (m_className == className)
            return;
        m_className = std::move(className);
        MarkPaintDirty();
    }

    UIElement& UIElement::AddChild(std::unique_ptr<UIElement> child)
    {
        if (!child)
            throw std::invalid_argument("UIElement::AddChild requires a non-null child");
        for (auto* ancestor = this; ancestor != nullptr; ancestor = ancestor->m_parent)
        {
            if (ancestor == child.get())
                throw std::invalid_argument("UIElement::AddChild would create a cycle");
        }

        child->m_parent = this;
        m_children.push_back(std::move(child));
        MarkLayoutDirty();
        return *m_children.back();
    }

    std::unique_ptr<UIElement> UIElement::RemoveChild(UIElement& child)
    {
        const auto iterator = std::find_if(
            m_children.begin(),
            m_children.end(),
            [&child](const auto& candidate) { return candidate.get() == &child; });
        if (iterator == m_children.end())
            return nullptr;

        auto result = std::move(*iterator);
        m_children.erase(iterator);
        result->m_parent = nullptr;
        ++result->m_generation;
        MarkLayoutDirty();
        return result;
    }

    void UIElement::ClearChildren()
    {
        for (auto& child : m_children)
        {
            if (child)
            {
                child->m_parent = nullptr;
                ++child->m_generation;
            }
        }
        m_children.clear();
        MarkLayoutDirty();
    }

    UIElement* UIElement::FindByGuid(const UINodeGuid& guid)
    {
        if (m_nodeGuid == guid)
            return this;
        for (auto& child : m_children)
        {
            if (auto* result = child->FindByGuid(guid))
                return result;
        }
        return nullptr;
    }

    const UIElement* UIElement::FindByGuid(const UINodeGuid& guid) const
    {
        if (m_nodeGuid == guid)
            return this;
        for (const auto& child : m_children)
        {
            if (const auto* result = child->FindByGuid(guid))
                return result;
        }
        return nullptr;
    }

    UIElement* UIElement::FindByName(const std::string& name)
    {
        if (m_name == name)
            return this;
        for (auto& child : m_children)
        {
            if (auto* result = child->FindByName(name))
                return result;
        }
        return nullptr;
    }

    const UIElement* UIElement::FindByName(const std::string& name) const
    {
        if (m_name == name)
            return this;
        for (const auto& child : m_children)
        {
            if (const auto* result = child->FindByName(name))
                return result;
        }
        return nullptr;
    }

    void UIElement::SetLayout(const UILayoutParams& layout)
    {
        m_layout = layout;
        MarkLayoutDirty();
    }

    void UIElement::SetStyle(const UIStyle& style)
    {
        m_style = style;
        MarkLayoutDirty();
        MarkPaintDirty();
    }

    void UIElement::SetText(std::string text)
    {
        if (m_text == text)
            return;
        if (m_textUndo.empty() || m_textUndo.back() != m_text)
            m_textUndo.push_back(m_text);
        if (m_textUndo.size() > 32u)
            m_textUndo.erase(m_textUndo.begin());
        m_text = std::move(text);
        m_textCursor = m_text.size();
        m_textSelectionAnchor = m_textCursor;
        MarkPaintDirty();
        if (m_type == UIElementType::Text || m_type == UIElementType::RichText)
            MarkLayoutDirty();
    }

    void UIElement::SetTextSelection(const size_t anchor, const size_t cursor)
    {
        m_textSelectionAnchor = std::min(anchor, m_text.size());
        m_textCursor = std::min(cursor, m_text.size());
        MarkPaintDirty();
    }

    void UIElement::ReplaceTextRange(
        const size_t begin,
        const size_t end,
        const std::string_view replacement)
    {
        const size_t clampedBegin = std::min(begin, m_text.size());
        const size_t clampedEnd = std::max(clampedBegin, std::min(end, m_text.size()));
        std::string next = m_text;
        next.replace(clampedBegin, clampedEnd - clampedBegin, replacement);
        SetText(std::move(next));
        m_textCursor = clampedBegin + replacement.size();
        m_textSelectionAnchor = m_textCursor;
    }

    void UIElement::SetValue(const float value)
    {
        const float clamped = std::clamp(value, 0.0f, 1.0f);
        if (std::abs(m_value - clamped) <= 0.00001f)
            return;
        m_value = clamped;
        MarkPaintDirty();
    }

    void UIElement::SetChecked(const bool checked)
    {
        if (m_checked == checked)
            return;
        m_checked = checked;
        MarkPaintDirty();
    }

    void UIElement::SetSelected(const bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        MarkPaintDirty();
    }

    void UIElement::SetVisible(const bool visible)
    {
        if (m_style.visible == visible)
            return;
        m_style.visible = visible;
        MarkPaintDirty();
        MarkLayoutDirty();
    }

    void UIElement::SetEnabled(const bool enabled)
    {
        if (m_style.enabled == enabled)
            return;
        m_style.enabled = enabled;
        MarkPaintDirty();
    }

    void UIElement::SetInteractive(const bool interactive)
    {
        if (m_style.interactive == interactive)
            return;
        m_style.interactive = interactive;
        MarkPaintDirty();
    }

    void UIElement::SetOpacity(const float opacity)
    {
        const float clamped = std::clamp(opacity, 0.0f, 1.0f);
        if (std::abs(m_style.opacity - clamped) <= 0.00001f)
            return;
        m_style.opacity = clamped;
        MarkPaintDirty();
    }

    void UIElement::SetColor(const UIColor color)
    {
        if (m_style.color == color)
            return;
        m_style.color = color;
        MarkPaintDirty();
    }

    void UIElement::SetBackgroundColor(const UIColor color)
    {
        if (m_style.backgroundColor.r == color.r && m_style.backgroundColor.g == color.g &&
            m_style.backgroundColor.b == color.b && m_style.backgroundColor.a == color.a)
            return;
        m_style.backgroundColor = color;
        MarkPaintDirty();
    }

    void UIElement::SetInvalid(const bool invalid)
    {
        if (m_invalid == invalid)
            return;
        m_invalid = invalid;
        MarkPaintDirty();
    }

    void UIElement::SetHovered(const bool hovered)
    {
        if (m_hovered == hovered)
            return;
        m_hovered = hovered;
        MarkPaintDirty();
    }

    void UIElement::SetPressed(const bool pressed)
    {
        if (m_pressed == pressed)
            return;
        m_pressed = pressed;
        MarkPaintDirty();
    }

    void UIElement::SetFocused(const bool focused)
    {
        if (m_focused == focused)
            return;
        m_focused = focused;
        MarkPaintDirty();
    }

    uint32_t UIElement::GetStateMask() const
    {
        uint32_t mask = 0u;
        if (m_hovered) mask |= 1u << 0u;
        if (m_pressed) mask |= 1u << 1u;
        if (m_focused) mask |= 1u << 2u;
        if (!m_style.enabled) mask |= 1u << 3u;
        if (m_checked) mask |= 1u << 4u;
        if (m_selected) mask |= 1u << 5u;
        if (m_invalid) mask |= 1u << 6u;
        return mask;
    }

    void UIElement::SetTexture(
        const uint64_t textureId,
        const uint32_t generation,
        const uint32_t width,
        const uint32_t height)
    {
        const uint32_t normalizedGeneration = textureId == 0u
            ? 0u
            : (generation == 0u ? 1u : generation);
        if (m_textureId == textureId &&
            m_textureGeneration == normalizedGeneration &&
            m_textureWidth == width &&
            m_textureHeight == height)
            return;
        m_textureId = textureId;
        m_textureGeneration = normalizedGeneration;
        m_textureWidth = textureId == 0u ? 0u : width;
        m_textureHeight = textureId == 0u ? 0u : height;
        MarkPaintDirty();
    }

    void UIElement::SetNineSlice(const UIInsets& insets)
    {
        if (m_nineSlice == insets)
            return;
        m_nineSlice = insets;
        MarkPaintDirty();
    }

    void UIElement::SetFont(const uint64_t fontAssetId, const uint16_t pixelSize)
    {
        const auto normalizedSize = std::max<uint16_t>(1u, pixelSize);
        if (m_fontAssetId == fontAssetId && m_fontPixelSize == normalizedSize)
            return;
        m_fontAssetId = fontAssetId;
        m_fontPixelSize = normalizedSize;
        MarkPaintDirty();
        if (m_type == UIElementType::Text || m_type == UIElementType::RichText)
            MarkLayoutDirty();
    }

    void UIElement::SetScrollOffset(const UIVector2 offset)
    {
        const UIVector2 clamped { std::max(0.0f, offset.x), std::max(0.0f, offset.y) };
        if (m_scrollOffset == clamped)
            return;
        m_scrollOffset = clamped;
        MarkLayoutDirty();
        MarkPaintDirty();
    }

    void UIElement::ClearDirtyFlags()
    {
        m_layoutDirty = false;
        m_paintDirty = false;
        for (auto& child : m_children)
            child->ClearDirtyFlags();
    }

    UIVector2 UIElement::Measure(const UIVector2 availableSize)
    {
        if (!m_style.visible)
        {
            m_measuredSize = {};
            return m_measuredSize;
        }

        const UIVector2 innerAvailable {
            std::max(0.0f, availableSize.x - m_layout.padding.x * 2.0f),
            std::max(0.0f, availableSize.y - m_layout.padding.y * 2.0f) };
        float width = m_layout.size.x > 0.0f ? m_layout.size.x : 0.0f;
        float height = m_layout.size.y > 0.0f ? m_layout.size.y : 0.0f;

        if (!m_children.empty())
        {
            float contentWidth = 0.0f;
            float contentHeight = 0.0f;
            if (m_layout.mode == UILayoutMode::Stack || m_layout.mode == UILayoutMode::Flex ||
                m_layout.mode == UILayoutMode::Scroll)
            {
                for (const auto& child : m_children)
                {
                    if (!child)
                        continue;
                    const auto childSize = child->Measure(innerAvailable);
                    const auto marginX = child->m_layout.margin.x * 2.0f;
                    const auto marginY = child->m_layout.margin.y * 2.0f;
                    if (m_layout.direction == UIStackDirection::Horizontal)
                    {
                        contentWidth += childSize.x + marginX;
                        contentHeight = std::max(contentHeight, childSize.y + marginY);
                    }
                    else
                    {
                        contentHeight += childSize.y + marginY;
                        contentWidth = std::max(contentWidth, childSize.x + marginX);
                    }
                }
                const auto childCount = m_children.size();
                if (childCount > 1u)
                {
                    if (m_layout.direction == UIStackDirection::Horizontal)
                        contentWidth += m_layout.spacing * static_cast<float>(childCount - 1u);
                    else
                        contentHeight += m_layout.spacing * static_cast<float>(childCount - 1u);
                }
            }
            else if (m_layout.mode == UILayoutMode::Grid)
            {
                const auto columns = std::max(1u, m_layout.gridColumns);
                const auto rows = (m_children.size() + columns - 1u) / columns;
                float maxChildWidth = 0.0f;
                float maxChildHeight = 0.0f;
                for (const auto& child : m_children)
                {
                    if (child)
                    {
                        const auto childSize = child->Measure(innerAvailable);
                        maxChildWidth = std::max(maxChildWidth, childSize.x);
                        maxChildHeight = std::max(maxChildHeight, childSize.y);
                    }
                }
                contentWidth = maxChildWidth * static_cast<float>(columns);
                contentHeight = maxChildHeight * static_cast<float>(rows);
            }
            else
            {
                for (const auto& child : m_children)
                {
                    if (child)
                    {
                        const auto childSize = child->Measure(innerAvailable);
                        contentWidth = std::max(contentWidth, childSize.x);
                        contentHeight = std::max(contentHeight, childSize.y);
                    }
                }
            }

            if (width <= 0.0f)
                width = contentWidth + m_layout.padding.x * 2.0f;
            if (height <= 0.0f)
                height = contentHeight + m_layout.padding.y * 2.0f;
        }

        const bool textLike = m_type == UIElementType::Text ||
            m_type == UIElementType::RichText || m_type == UIElementType::Button ||
            m_type == UIElementType::TextField || m_type == UIElementType::Dropdown;
        if (textLike && !m_text.empty())
        {
            const float lineHeight = static_cast<float>(m_fontPixelSize) * 1.25f;
            const float intrinsicWidth = static_cast<float>(CountUtf8Codepoints(m_text)) *
                static_cast<float>(m_fontPixelSize) * 0.6f;
            if (width <= 0.0f)
                width = intrinsicWidth + m_layout.padding.x * 2.0f;
            if (height <= 0.0f)
                height = lineHeight + m_layout.padding.y * 2.0f;
        }
        if (width <= 0.0f)
            width = innerAvailable.x;
        if (height <= 0.0f)
            height = innerAvailable.y;
        if (m_layout.aspectRatio > 0.0f)
        {
            if (m_layout.size.x > 0.0f && m_layout.size.y <= 0.0f)
                height = width / m_layout.aspectRatio;
            else if (m_layout.size.y > 0.0f && m_layout.size.x <= 0.0f)
                width = height * m_layout.aspectRatio;
        }
        m_measuredSize = {
            ClampDimension(width, m_layout.minSize.x, m_layout.maxSize.x),
            ClampDimension(height, m_layout.minSize.y, m_layout.maxSize.y) };
        m_layoutDirty = false;
        return m_measuredSize;
    }

    void UIElement::Arrange(const UIRect finalRect)
    {
        m_layoutRect = finalRect;
        if (!m_style.visible)
            return;

        const auto content = finalRect.Inset(
            m_layout.padding.x,
            m_layout.padding.y,
            m_layout.padding.x,
            m_layout.padding.y);

        if (m_children.empty())
        {
            m_layoutDirty = false;
            return;
        }

        if (m_layout.mode == UILayoutMode::Stack || m_layout.mode == UILayoutMode::Flex ||
            m_layout.mode == UILayoutMode::Scroll)
        {
            float cursor = m_layout.direction == UIStackDirection::Horizontal
                ? content.x - (m_layout.mode == UILayoutMode::Scroll ? m_scrollOffset.x : 0.0f)
                : content.y - (m_layout.mode == UILayoutMode::Scroll ? m_scrollOffset.y : 0.0f);
            const float availableMain = m_layout.direction == UIStackDirection::Horizontal
                ? content.width
                : content.height;
            float fixedMain = 0.0f;
            float growTotal = 0.0f;
            for (const auto& child : m_children)
            {
                if (!child)
                    continue;
                const auto size = child->Measure({ content.width, content.height });
                fixedMain += (m_layout.direction == UIStackDirection::Horizontal ? size.x : size.y) +
                    (m_layout.direction == UIStackDirection::Horizontal ? child->m_layout.margin.x : child->m_layout.margin.y) * 2.0f;
                growTotal += std::max(0.0f, child->m_layout.flexGrow);
            }
            if (m_children.size() > 1u)
                fixedMain += m_layout.spacing * static_cast<float>(m_children.size() - 1u);
            const float remaining = std::max(0.0f, availableMain - fixedMain);
            for (const auto& child : m_children)
            {
                if (!child)
                    continue;
                auto childSize = child->m_measuredSize;
                if (growTotal > 0.0f && child->m_layout.flexGrow > 0.0f)
                {
                    const float grow = remaining * child->m_layout.flexGrow / growTotal;
                    if (m_layout.direction == UIStackDirection::Horizontal)
                        childSize.x += grow;
                    else
                        childSize.y += grow;
                }
                const float crossSize = m_layout.direction == UIStackDirection::Horizontal ? childSize.y : childSize.x;
                const float crossAvailable = m_layout.direction == UIStackDirection::Horizontal ? content.height : content.width;
                float cross = m_layout.direction == UIStackDirection::Horizontal ? content.y : content.x;
                if (m_layout.align == UIAlign::Center)
                    cross += (crossAvailable - crossSize) * 0.5f;
                else if (m_layout.align == UIAlign::End)
                    cross += crossAvailable - crossSize;
                else if (m_layout.align == UIAlign::Stretch)
                {
                    if (m_layout.direction == UIStackDirection::Horizontal)
                        childSize.y = crossAvailable;
                    else
                        childSize.x = crossAvailable;
                }
                if (m_layout.direction == UIStackDirection::Horizontal)
                {
                    child->Arrange({ cursor + child->m_layout.margin.x, cross,
                        std::max(0.0f, childSize.x - child->m_layout.margin.x * 2.0f),
                        std::max(0.0f, childSize.y) });
                    cursor += childSize.x + child->m_layout.margin.x * 2.0f + m_layout.spacing;
                }
                else
                {
                    child->Arrange({ cross, cursor + child->m_layout.margin.y,
                        std::max(0.0f, childSize.x),
                        std::max(0.0f, childSize.y - child->m_layout.margin.y * 2.0f) });
                    cursor += childSize.y + child->m_layout.margin.y * 2.0f + m_layout.spacing;
                }
            }
        }
        else if (m_layout.mode == UILayoutMode::Grid)
        {
            const auto columns = std::max(1u, m_layout.gridColumns);
            const auto cellWidth = content.width / static_cast<float>(columns);
            const auto rows = (m_children.size() + columns - 1u) / columns;
            const auto cellHeight = rows == 0u ? content.height : content.height / static_cast<float>(rows);
            for (size_t index = 0u; index < m_children.size(); ++index)
            {
                auto& child = m_children[index];
                if (!child)
                    continue;
                child->Arrange({
                    content.x + static_cast<float>(index % columns) * cellWidth -
                        (m_layout.mode == UILayoutMode::Scroll ? m_scrollOffset.x : 0.0f),
                    content.y + static_cast<float>(index / columns) * cellHeight -
                        (m_layout.mode == UILayoutMode::Scroll ? m_scrollOffset.y : 0.0f),
                    cellWidth,
                    cellHeight });
            }
        }
        else
        {
            for (auto& child : m_children)
            {
                if (!child)
                    continue;
                const auto& layout = child->m_layout;
                const float anchorLeft = content.x + content.width * layout.anchorMin.x;
                const float anchorTop = content.y + content.height * layout.anchorMin.y;
                const float anchorRight = content.x + content.width * layout.anchorMax.x;
                const float anchorBottom = content.y + content.height * layout.anchorMax.y;
                const bool stretchedX = std::abs(layout.anchorMax.x - layout.anchorMin.x) > 0.0001f;
                const bool stretchedY = std::abs(layout.anchorMax.y - layout.anchorMin.y) > 0.0001f;
                const float width = stretchedX
                    ? std::max(0.0f, anchorRight - anchorLeft + layout.offsetMax.x - layout.offsetMin.x)
                    : (layout.size.x > 0.0f ? layout.size.x : child->m_measuredSize.x);
                const float height = stretchedY
                    ? std::max(0.0f, anchorBottom - anchorTop + layout.offsetMax.y - layout.offsetMin.y)
                    : (layout.size.y > 0.0f ? layout.size.y : child->m_measuredSize.y);
                const float baseX = anchorLeft + layout.offsetMin.x -
                    (m_layout.mode == UILayoutMode::Scroll ? m_scrollOffset.x : 0.0f);
                const float baseY = anchorTop + layout.offsetMin.y -
                    (m_layout.mode == UILayoutMode::Scroll ? m_scrollOffset.y : 0.0f);
                const float adjustedHeight = layout.aspectRatio > 0.0f &&
                    layout.size.y <= 0.0f && layout.size.x > 0.0f
                    ? width / layout.aspectRatio : height;
                const float adjustedWidth = layout.aspectRatio > 0.0f &&
                    layout.size.x <= 0.0f && layout.size.y > 0.0f
                    ? height * layout.aspectRatio : width;
                child->Arrange({ baseX - adjustedWidth * layout.pivot.x,
                    baseY - adjustedHeight * layout.pivot.y,
                    adjustedWidth,
                    adjustedHeight });
            }
        }
        m_layoutDirty = false;
    }

    UIElement* UIElement::HitTest(const UIVector2 point)
    {
        if (!m_style.visible || !m_style.enabled || !m_layoutRect.Contains(point))
            return nullptr;
        for (auto iterator = m_children.rbegin(); iterator != m_children.rend(); ++iterator)
        {
            if (*iterator)
            {
                if (auto* hit = (*iterator)->HitTest(point))
                    return hit;
            }
        }
        return m_style.interactive ? this : nullptr;
    }

    const UIElement* UIElement::HitTest(const UIVector2 point) const
    {
        return const_cast<UIElement*>(this)->HitTest(point);
    }

    UIEventListenerHandle UIElement::AddEventListener(
        const UIEventType type,
        EventHandler handler,
        const bool capture)
    {
        if (!handler)
            return {};
        UIEventListenerHandle handle { m_nextListenerId++ };
        m_listeners[EventIndex(type)].push_back({ handle, capture, std::move(handler) });
        return handle;
    }

    bool UIElement::RemoveEventListener(const UIEventListenerHandle handle)
    {
        if (!handle.IsValid())
            return false;
        for (auto& listeners : m_listeners)
        {
            const auto iterator = std::find_if(
                listeners.begin(), listeners.end(),
                [handle](const Listener& listener) { return listener.handle.value == handle.value; });
            if (iterator != listeners.end())
            {
                listeners.erase(iterator);
                return true;
            }
        }
        return false;
    }

    void UIElement::InvokeEventListeners(UIEvent& event, const bool capture)
    {
        auto& listeners = m_listeners[EventIndex(event.type)];
        const auto copy = listeners;
        for (const auto& listener : copy)
        {
            if (listener.capture == capture && listener.handler && !event.propagationStopped)
            {
                event.currentTarget = this;
                listener.handler(event);
            }
        }
    }

    void UIElement::PerformDefaultAction(UIEvent& event)
    {
        if (!m_style.enabled || !m_style.visible)
            return;

        const auto notifyValueChanged = [this, &event]()
        {
            UIEvent changed = event;
            changed.type = UIEventType::ValueChanged;
            changed.target = this;
            changed.currentTarget = this;
            changed.phase = UIEventPhase::Target;
            changed.consumed = false;
            changed.defaultPrevented = false;
            changed.propagationStopped = false;
            InvokeEventListeners(changed, true);
            InvokeEventListeners(changed, false);
        };

        if (event.type == UIEventType::Click)
        {
            if (m_type == UIElementType::Toggle)
            {
                const auto oldValue = m_checked;
                SetChecked(!m_checked);
                if (oldValue != m_checked)
                    notifyValueChanged();
            }
            else if (m_type == UIElementType::Radio)
            {
                const auto oldValue = m_checked;
                SetChecked(true);
                if (oldValue != m_checked)
                    notifyValueChanged();
            }
            return;
        }

        if (m_type == UIElementType::Slider &&
            (event.type == UIEventType::PointerDown || event.type == UIEventType::PointerMove))
        {
            if (m_layoutRect.width > 0.0f)
            {
                const auto oldValue = m_value;
                SetValue((event.position.x - m_layoutRect.x) / m_layoutRect.width);
                if (std::abs(oldValue - m_value) > 0.00001f)
                    notifyValueChanged();
            }
            return;
        }

        if (m_type == UIElementType::TextField && event.type == UIEventType::TextInput)
        {
            const auto begin = std::min(m_textCursor, m_textSelectionAnchor);
            const auto end = std::max(m_textCursor, m_textSelectionAnchor);
            ReplaceTextRange(begin, end, event.text);
            notifyValueChanged();
            return;
        }

        if (m_type == UIElementType::TextField && event.type == UIEventType::ImeCompositionUpdate)
        {
            m_imeComposition = event.text;
            MarkPaintDirty();
            return;
        }

        if (m_type == UIElementType::TextField && event.type == UIEventType::ImeCompositionCommit)
        {
            const auto begin = std::min(m_textCursor, m_textSelectionAnchor);
            const auto end = std::max(m_textCursor, m_textSelectionAnchor);
            ReplaceTextRange(begin, end, event.text);
            m_imeComposition.clear();
            notifyValueChanged();
            return;
        }

        if (m_type == UIElementType::TextField && event.type == UIEventType::KeyDown)
        {
            if ((event.modifiers & UIInputModifierControl) != 0u && event.keyCode == 90u && !m_textUndo.empty())
            {
                const auto previous = m_textUndo.back();
                m_textUndo.pop_back();
                m_text = previous;
                m_textCursor = m_text.size();
                m_textSelectionAnchor = m_textCursor;
                MarkPaintDirty();
                MarkLayoutDirty();
            }
            else if (event.keyCode == 8u && m_textCursor > 0u)
            {
                const auto begin = std::min(m_textCursor - 1u, m_textSelectionAnchor);
                const auto end = std::max(m_textCursor, m_textSelectionAnchor);
                ReplaceTextRange(begin, end, {});
            }
            else if (event.keyCode == 127u && m_textCursor < m_text.size())
            {
                ReplaceTextRange(m_textCursor, std::max(m_textCursor + 1u, m_textSelectionAnchor), {});
            }
            else if (event.keyCode == 37u)
            {
                m_textCursor = m_textCursor > 0u ? m_textCursor - 1u : 0u;
                if ((event.modifiers & UIInputModifierShift) == 0u)
                    m_textSelectionAnchor = m_textCursor;
            }
            else if (event.keyCode == 39u)
            {
                m_textCursor = std::min(m_text.size(), m_textCursor + 1u);
                if ((event.modifiers & UIInputModifierShift) == 0u)
                    m_textSelectionAnchor = m_textCursor;
            }
            else if (event.keyCode == 13u)
            {
                UIEvent submit = event;
                submit.type = UIEventType::Submit;
                submit.target = this;
                submit.currentTarget = this;
                InvokeEventListeners(submit, true);
                InvokeEventListeners(submit, false);
            }
        }
    }

    void UIElement::BuildDrawSnapshot(NLS::Render::UI::UIDrawSnapshot& snapshot) const
    {
        BuildDrawSnapshotInternal(snapshot, nullptr, 0u);
    }

    void UIElement::BuildDrawSnapshotInternal(
        NLS::Render::UI::UIDrawSnapshot& snapshot,
        const NLS::Render::UI::UIClipRect* inheritedClip,
        const uint32_t depth) const
    {
        ++snapshot.diagnostics.nodeCount;
        if (!m_style.visible || m_style.opacity <= 0.0f)
            return;
        ++snapshot.diagnostics.visibleNodeCount;

        NLS::Render::UI::UIClipRect clip;
        const bool hasLocalClip = m_layout.clipChildren;
        if (hasLocalClip)
        {
            clip = RectToClip(m_layoutRect);
            if (inheritedClip)
                clip = IntersectClip(*inheritedClip, clip);
        }
        else if (inheritedClip)
        {
            clip = *inheritedClip;
        }
        const auto clipIndex = (hasLocalClip || inheritedClip)
            ? snapshot.AddClip(clip)
            : NLS::Render::UI::UIClipIndexNone;

        const auto color = m_style.backgroundColor;
        const auto alpha = std::clamp(color.a * m_style.opacity, 0.0f, 1.0f);
        const bool textured = m_textureId != 0u &&
            (m_type == UIElementType::Image || m_type == UIElementType::NineSlice);
        if ((alpha > 0.0f || textured) &&
            m_layoutRect.width > 0.0f && m_layoutRect.height > 0.0f)
        {
            const std::array<float, 4> drawColor {
                color.r, color.g, color.b, textured ? std::max(alpha, 1.0f) : alpha };
            const bool nineSlice = m_type == UIElementType::NineSlice;
            if (textured && nineSlice && !m_nineSlice.IsZero() &&
                m_textureWidth > 0u && m_textureHeight > 0u)
            {
                const float width = m_layoutRect.width;
                const float height = m_layoutRect.height;
                const float halfWidth = width * 0.5f;
                const float halfHeight = height * 0.5f;
                float left = std::clamp(m_nineSlice.left, 0.0f, halfWidth);
                float right = std::clamp(m_nineSlice.right, 0.0f, halfWidth);
                float top = std::clamp(m_nineSlice.top, 0.0f, halfHeight);
                float bottom = std::clamp(m_nineSlice.bottom, 0.0f, halfHeight);
                if (left + right > width)
                {
                    const float scale = width / (left + right);
                    left *= scale;
                    right *= scale;
                }
                if (top + bottom > height)
                {
                    const float scale = height / (top + bottom);
                    top *= scale;
                    bottom *= scale;
                }

                const float textureWidth = static_cast<float>(m_textureWidth);
                const float textureHeight = static_cast<float>(m_textureHeight);
                const float sourceLeft = std::clamp(
                    m_nineSlice.left, 0.0f, textureWidth * 0.5f) / textureWidth;
                const float sourceRight = std::clamp(
                    m_nineSlice.right, 0.0f, textureWidth * 0.5f) / textureWidth;
                const float sourceTop = std::clamp(
                    m_nineSlice.top, 0.0f, textureHeight * 0.5f) / textureHeight;
                const float sourceBottom = std::clamp(
                    m_nineSlice.bottom, 0.0f, textureHeight * 0.5f) / textureHeight;

                const float x[4] = {
                    m_layoutRect.x,
                    m_layoutRect.x + left,
                    m_layoutRect.x + width - right,
                    m_layoutRect.x + width };
                const float y[4] = {
                    m_layoutRect.y,
                    m_layoutRect.y + top,
                    m_layoutRect.y + height - bottom,
                    m_layoutRect.y + height };
                const float u[4] = { 0.0f, sourceLeft, 1.0f - sourceRight, 1.0f };
                const float v[4] = { 0.0f, sourceTop, 1.0f - sourceBottom, 1.0f };

                uint32_t quadCount = 0u;
                for (uint32_t row = 0u; row < 3u; ++row)
                {
                    if (y[row + 1u] <= y[row] || v[row + 1u] <= v[row])
                        continue;
                    for (uint32_t column = 0u; column < 3u; ++column)
                    {
                        if (x[column + 1u] <= x[column] || u[column + 1u] <= u[column])
                            continue;
                        snapshot.AddTexturedQuad(
                            x[column],
                            y[row],
                            x[column + 1u],
                            y[row + 1u],
                            { u[column], v[row] },
                            { u[column + 1u], v[row + 1u] },
                            drawColor,
                            clipIndex,
                            m_textureId,
                            static_cast<uint64_t>(depth),
                            m_textureGeneration);
                        ++quadCount;
                    }
                }
                if (quadCount == 0u)
                {
                    snapshot.AddQuad(
                        m_layoutRect.x,
                        m_layoutRect.y,
                        m_layoutRect.x + width,
                        m_layoutRect.y + height,
                        drawColor,
                        clipIndex,
                        m_textureId,
                        static_cast<uint64_t>(depth),
                        m_textureGeneration);
                }
            }
            else
            {
                snapshot.AddQuad(
                    m_layoutRect.x,
                    m_layoutRect.y,
                    m_layoutRect.x + m_layoutRect.width,
                    m_layoutRect.y + m_layoutRect.height,
                    drawColor,
                    clipIndex,
                    textured ? m_textureId : 0u,
                    static_cast<uint64_t>(depth),
                    textured ? m_textureGeneration : 0u);
            }
        }

        if (!m_text.empty() &&
            (m_type == UIElementType::Text || m_type == UIElementType::RichText ||
             m_type == UIElementType::Button || m_type == UIElementType::TextField ||
             m_type == UIElementType::Dropdown))
        {
            const auto textColor = m_style.color;
            std::string displayText = m_text;
            if (m_passwordMode)
                displayText.assign(CountUtf8Codepoints(m_text), '*');
            snapshot.textRuns.push_back({
                std::move(displayText),
                m_layoutRect.x,
                m_layoutRect.y,
                m_layoutRect.width,
                m_layoutRect.height,
                static_cast<float>(m_fontPixelSize),
                { textColor.r, textColor.g, textColor.b,
                    std::clamp(textColor.a * m_style.opacity, 0.0f, 1.0f) },
                clipIndex,
                0u,
                static_cast<uint64_t>(depth),
                m_fontPixelSize });
            snapshot.diagnostics.textRunCount =
                static_cast<uint32_t>(snapshot.textRuns.size());
        }

        for (const auto& child : m_children)
        {
            if (child)
                child->BuildDrawSnapshotInternal(snapshot, (hasLocalClip || inheritedClip) ? &clip : nullptr, depth + 1u);
        }
    }

    void UIElement::MarkLayoutDirty()
    {
        if (m_layoutDirty)
            return;
        m_layoutDirty = true;
        if (m_parent)
            m_parent->MarkLayoutDirty();
    }

    void UIElement::MarkPaintDirty()
    {
        m_paintDirty = true;
        if (m_parent)
            m_parent->MarkPaintDirty();
    }

    size_t UIElement::EventIndex(const UIEventType type)
    {
        const auto index = static_cast<size_t>(type);
        return index < static_cast<size_t>(UIEventType::Count)
            ? index
            : static_cast<size_t>(UIEventType::Cancel);
    }
}
