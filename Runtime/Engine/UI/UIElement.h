#pragma once

#include "Object/Object.h"
#include "EngineDef.h"
#include "Rendering/UI/UIDrawSnapshot.h"
#include "UICommon.h"
#include "UIEvent.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace NLS::Engine::UI
{
    struct UIEventListenerHandle
    {
        uint64_t value = 0u;
        [[nodiscard]] bool IsValid() const { return value != 0u; }
    };

    class NLS_ENGINE_API UIElement final : public NLS::Object
    {
    public:
        using EventHandler = std::function<void(UIEvent&)>;

        UIElement(
            UIElementType type = UIElementType::Panel,
            std::string name = {},
            UINodeGuid nodeGuid = UINodeGuid::Empty());
        ~UIElement() override = default;

        [[nodiscard]] UIElementType GetElementType() const { return m_type; }
        [[nodiscard]] const std::string& GetName() const { return m_name; }
        void SetName(std::string name);
        [[nodiscard]] const std::string& GetClassName() const { return m_className; }
        void SetClassName(std::string className);
        [[nodiscard]] const UINodeGuid& GetNodeGuid() const { return m_nodeGuid; }
        [[nodiscard]] uint32_t GetGeneration() const { return m_generation; }

        [[nodiscard]] UIElement* GetParent() const { return m_parent; }
        [[nodiscard]] const std::vector<std::unique_ptr<UIElement>>& GetChildren() const { return m_children; }
        UIElement& AddChild(std::unique_ptr<UIElement> child);
        std::unique_ptr<UIElement> RemoveChild(UIElement& child);
        void ClearChildren();

        UIElement* FindByGuid(const UINodeGuid& guid);
        const UIElement* FindByGuid(const UINodeGuid& guid) const;
        UIElement* FindByName(const std::string& name);
        const UIElement* FindByName(const std::string& name) const;

        [[nodiscard]] const UILayoutParams& GetLayout() const { return m_layout; }
        void SetLayout(const UILayoutParams& layout);
        [[nodiscard]] const UIStyle& GetStyle() const { return m_style; }
        void SetStyle(const UIStyle& style);
        [[nodiscard]] const UIRect& GetLayoutRect() const { return m_layoutRect; }
        [[nodiscard]] UIVector2 GetMeasuredSize() const { return m_measuredSize; }

        void SetText(std::string text);
        [[nodiscard]] const std::string& GetText() const { return m_text; }
        void SetTextSelection(size_t anchor, size_t cursor);
        [[nodiscard]] size_t GetTextCursor() const { return m_textCursor; }
        [[nodiscard]] size_t GetTextSelectionAnchor() const { return m_textSelectionAnchor; }
        [[nodiscard]] const std::string& GetImeComposition() const { return m_imeComposition; }
        void SetValue(float value);
        [[nodiscard]] float GetValue() const { return m_value; }
        void SetChecked(bool checked);
        [[nodiscard]] bool IsChecked() const { return m_checked; }
        void SetSelected(bool selected);
        [[nodiscard]] bool IsSelected() const { return m_selected; }
        void SetVisible(bool visible);
        void SetEnabled(bool enabled);
        void SetInteractive(bool interactive);
        void SetOpacity(float opacity);
        void SetColor(UIColor color);
        void SetBackgroundColor(UIColor color);
        void SetInvalid(bool invalid);
        [[nodiscard]] bool IsInvalid() const { return m_invalid; }
        void SetHovered(bool hovered);
        void SetPressed(bool pressed);
        void SetFocused(bool focused);
        [[nodiscard]] bool IsHovered() const { return m_hovered; }
        [[nodiscard]] bool IsPressed() const { return m_pressed; }
        [[nodiscard]] bool IsFocused() const { return m_focused; }
        [[nodiscard]] uint32_t GetStateMask() const;
        void SetTexture(uint64_t textureId, uint32_t generation = 1u);
        [[nodiscard]] uint64_t GetTextureId() const { return m_textureId; }
        [[nodiscard]] uint32_t GetTextureGeneration() const { return m_textureGeneration; }
        void SetFont(uint64_t fontAssetId, uint16_t pixelSize = 16u);
        [[nodiscard]] uint64_t GetFontAssetId() const { return m_fontAssetId; }
        [[nodiscard]] uint16_t GetFontPixelSize() const { return m_fontPixelSize; }
        void SetPasswordMode(bool enabled) { m_passwordMode = enabled; MarkPaintDirty(); }
        [[nodiscard]] bool IsPasswordMode() const { return m_passwordMode; }
        void SetScrollOffset(UIVector2 offset);
        [[nodiscard]] UIVector2 GetScrollOffset() const { return m_scrollOffset; }

        [[nodiscard]] bool IsLayoutDirty() const { return m_layoutDirty; }
        [[nodiscard]] bool IsPaintDirty() const { return m_paintDirty; }
        void ClearDirtyFlags();
        // External resources (the font atlas, for example) can make cached paint
        // geometry stale, so hosts need to request a rebuild explicitly.
        void MarkPaintDirty();

        UIVector2 Measure(UIVector2 availableSize);
        void Arrange(UIRect finalRect);
        UIElement* HitTest(UIVector2 point);
        const UIElement* HitTest(UIVector2 point) const;

        UIEventListenerHandle AddEventListener(UIEventType type, EventHandler handler, bool capture = false);
        bool RemoveEventListener(UIEventListenerHandle handle);
        void InvokeEventListeners(UIEvent& event, bool capture);
        void PerformDefaultAction(UIEvent& event);

        void BuildDrawSnapshot(NLS::Render::UI::UIDrawSnapshot& snapshot) const;

    private:
        void BuildDrawSnapshotInternal(
            NLS::Render::UI::UIDrawSnapshot& snapshot,
            const NLS::Render::UI::UIClipRect* inheritedClip,
            uint32_t depth) const;
        void MarkLayoutDirty();
        void ReplaceTextRange(size_t begin, size_t end, std::string_view replacement);
        static size_t EventIndex(UIEventType type);

        struct Listener
        {
            UIEventListenerHandle handle;
            bool capture = false;
            EventHandler handler;
        };

        UIElementType m_type = UIElementType::Panel;
        std::string m_name;
        std::string m_className;
        UINodeGuid m_nodeGuid = UINodeGuid::Empty();
        uint32_t m_generation = 1u;
        UIElement* m_parent = nullptr;
        std::vector<std::unique_ptr<UIElement>> m_children;
        UILayoutParams m_layout;
        UIStyle m_style;
        UIRect m_layoutRect;
        UIVector2 m_measuredSize;
        std::string m_text;
        size_t m_textCursor = 0u;
        size_t m_textSelectionAnchor = 0u;
        std::string m_imeComposition;
        std::vector<std::string> m_textUndo;
        bool m_layoutDirty = true;
        bool m_paintDirty = true;
        uint64_t m_nextListenerId = 1u;
        float m_value = 0.0f;
        bool m_checked = false;
        bool m_selected = false;
        bool m_invalid = false;
        bool m_hovered = false;
        bool m_pressed = false;
        bool m_focused = false;
        uint64_t m_textureId = 0u;
        uint32_t m_textureGeneration = 0u;
        uint64_t m_fontAssetId = 0u;
        uint16_t m_fontPixelSize = 16u;
        bool m_passwordMode = false;
        UIVector2 m_scrollOffset;
        std::vector<Listener> m_listeners[static_cast<size_t>(UIEventType::Count)];
    };
}
