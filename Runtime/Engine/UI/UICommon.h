#pragma once

#include "Guid.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace NLS::Engine::UI
{
    using UINodeGuid = NLS::Guid;

    struct UIVector2
    {
        float x = 0.0f;
        float y = 0.0f;

        constexpr UIVector2() = default;
        constexpr UIVector2(float xValue, float yValue) : x(xValue), y(yValue) {}
        constexpr UIVector2 operator+(const UIVector2& rhs) const { return { x + rhs.x, y + rhs.y }; }
        constexpr UIVector2 operator-(const UIVector2& rhs) const { return { x - rhs.x, y - rhs.y }; }
        constexpr UIVector2 operator*(float scalar) const { return { x * scalar, y * scalar }; }
        constexpr bool operator==(const UIVector2&) const = default;
    };

    struct UIRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        [[nodiscard]] constexpr bool Contains(UIVector2 point) const
        {
            return point.x >= x && point.y >= y && point.x <= x + width && point.y <= y + height;
        }

        [[nodiscard]] constexpr UIRect Inset(float left, float top, float right, float bottom) const
        {
            return {
                x + left,
                y + top,
                std::max(0.0f, width - left - right),
                std::max(0.0f, height - top - bottom) };
        }
    };

    struct UIColor
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;

        constexpr std::array<float, 4> ToArray() const { return { r, g, b, a }; }
        constexpr bool operator==(const UIColor&) const = default;
    };

    enum class UIElementType : uint8_t
    {
        Panel,
        Image,
        NineSlice,
        Text,
        RichText,
        Button,
        Toggle,
        Radio,
        Slider,
        ProgressBar,
        TextField,
        Dropdown,
        ScrollView,
        Scrollbar,
        ListView,
        Unknown
    };

    enum class UILayoutMode : uint8_t
    {
        Absolute,
        Stack,
        Flex,
        Grid,
        Overlay,
        Scroll
    };

    enum class UIStackDirection : uint8_t
    {
        Horizontal,
        Vertical
    };

    enum class UIAlign : uint8_t
    {
        Start,
        Center,
        End,
        Stretch
    };

    struct UILayoutParams
    {
        UILayoutMode mode = UILayoutMode::Absolute;
        UIStackDirection direction = UIStackDirection::Vertical;
        UIVector2 anchorMin { 0.0f, 0.0f };
        UIVector2 anchorMax { 0.0f, 0.0f };
        UIVector2 pivot { 0.0f, 0.0f };
        UIVector2 offsetMin { 0.0f, 0.0f };
        UIVector2 offsetMax { 0.0f, 0.0f };
        UIVector2 size { 0.0f, 0.0f };
        UIVector2 minSize { 0.0f, 0.0f };
        UIVector2 maxSize { 0.0f, 0.0f };
        UIVector2 margin { 0.0f, 0.0f };
        UIVector2 padding { 0.0f, 0.0f };
        // A value of zero disables aspect-ratio enforcement.  Keeping this
        // on the shared layout record lets containers and leaf controls apply
        // the same constraint during both measure and arrange.
        float aspectRatio = 0.0f;
        float spacing = 0.0f;
        float flexGrow = 0.0f;
        uint32_t gridColumns = 1u;
        UIAlign align = UIAlign::Start;
        bool clipChildren = false;
    };

    struct UIStyle
    {
        UIColor backgroundColor { 0.0f, 0.0f, 0.0f, 0.0f };
        UIColor color { 1.0f, 1.0f, 1.0f, 1.0f };
        float opacity = 1.0f;
        bool visible = true;
        bool enabled = true;
        bool interactive = false;
        // Keeps the distinction between an omitted JSON property and an
        // explicit false override.  Element defaults are applied only when
        // this flag is false.
        bool interactiveSpecified = false;
    };

    [[nodiscard]] inline const char* ToString(const UIElementType type)
    {
        switch (type)
        {
        case UIElementType::Panel: return "Panel";
        case UIElementType::Image: return "Image";
        case UIElementType::NineSlice: return "NineSlice";
        case UIElementType::Text: return "Text";
        case UIElementType::RichText: return "RichText";
        case UIElementType::Button: return "Button";
        case UIElementType::Toggle: return "Toggle";
        case UIElementType::Radio: return "Radio";
        case UIElementType::Slider: return "Slider";
        case UIElementType::ProgressBar: return "ProgressBar";
        case UIElementType::TextField: return "TextField";
        case UIElementType::Dropdown: return "Dropdown";
        case UIElementType::ScrollView: return "ScrollView";
        case UIElementType::Scrollbar: return "Scrollbar";
        case UIElementType::ListView: return "ListView";
        case UIElementType::Unknown: break;
        }
        return "Unknown";
    }

    [[nodiscard]] inline UIElementType ElementTypeFromString(std::string_view text)
    {
        const auto equal = [text](std::string_view candidate)
        {
            return text == candidate;
        };
        for (const auto type : {
            UIElementType::Panel, UIElementType::Image, UIElementType::NineSlice,
            UIElementType::Text, UIElementType::RichText, UIElementType::Button,
            UIElementType::Toggle, UIElementType::Radio, UIElementType::Slider,
            UIElementType::ProgressBar, UIElementType::TextField, UIElementType::Dropdown,
            UIElementType::ScrollView, UIElementType::Scrollbar, UIElementType::ListView })
        {
            if (equal(ToString(type)))
                return type;
        }
        return UIElementType::Unknown;
    }

    [[nodiscard]] inline UILayoutMode LayoutModeFromString(std::string_view text)
    {
        if (text == "Stack") return UILayoutMode::Stack;
        if (text == "Flex") return UILayoutMode::Flex;
        if (text == "Grid") return UILayoutMode::Grid;
        if (text == "Overlay") return UILayoutMode::Overlay;
        if (text == "Scroll") return UILayoutMode::Scroll;
        return UILayoutMode::Absolute;
    }
}
