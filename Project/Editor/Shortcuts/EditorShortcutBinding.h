#pragma once

#include <cstdint>
#include <string>
#include <tuple>

#include <Windowing/Inputs/EKey.h>

namespace NLS::Editor::Shortcuts
{
    enum class EShortcutModifier : uint8_t
    {
        Ctrl = 1 << 0,
        Shift = 1 << 1,
        Alt = 1 << 2,
        Super = 1 << 3
    };

    using ShortcutModifiers = uint8_t;

    struct ShortcutBinding
    {
        bool assigned = false;
        Windowing::Inputs::EKey primaryKey = Windowing::Inputs::EKey::KEY_UNKNOWN;
        ShortcutModifiers modifiers = 0;

        static ShortcutBinding Unassigned();
        static ShortcutBinding FromKey(Windowing::Inputs::EKey p_key, ShortcutModifiers p_modifiers = 0);
        static ShortcutBinding FromKey(Windowing::Inputs::EKey p_key, EShortcutModifier p_modifier);

        bool IsValid() const;
        bool operator==(const ShortcutBinding& p_other) const;
        bool operator!=(const ShortcutBinding& p_other) const;
        bool operator<(const ShortcutBinding& p_other) const;
    };

    ShortcutModifiers operator|(EShortcutModifier p_left, EShortcutModifier p_right);
    ShortcutModifiers operator|(ShortcutModifiers p_left, EShortcutModifier p_right);
    bool HasModifier(ShortcutModifiers p_modifiers, EShortcutModifier p_modifier);
    std::string ToDisplayString(const ShortcutBinding& p_binding);

    inline ShortcutModifiers ToModifierMask(const EShortcutModifier p_modifier)
    {
        return static_cast<ShortcutModifiers>(p_modifier);
    }

    inline ShortcutModifiers operator|(const EShortcutModifier p_left, const EShortcutModifier p_right)
    {
        return ToModifierMask(p_left) | ToModifierMask(p_right);
    }

    inline ShortcutModifiers operator|(const ShortcutModifiers p_left, const EShortcutModifier p_right)
    {
        return p_left | ToModifierMask(p_right);
    }

    inline bool HasModifier(const ShortcutModifiers p_modifiers, const EShortcutModifier p_modifier)
    {
        return (p_modifiers & ToModifierMask(p_modifier)) != 0;
    }

    inline ShortcutBinding ShortcutBinding::Unassigned()
    {
        return {};
    }

    inline ShortcutBinding ShortcutBinding::FromKey(
        const Windowing::Inputs::EKey p_key,
        const ShortcutModifiers p_modifiers)
    {
        ShortcutBinding result;
        result.assigned = true;
        result.primaryKey = p_key;
        result.modifiers = p_modifiers;
        return result;
    }

    inline ShortcutBinding ShortcutBinding::FromKey(
        const Windowing::Inputs::EKey p_key,
        const EShortcutModifier p_modifier)
    {
        return FromKey(p_key, ToModifierMask(p_modifier));
    }

    inline bool IsModifierKey(const Windowing::Inputs::EKey p_key)
    {
        using Windowing::Inputs::EKey;
        switch (p_key)
        {
        case EKey::KEY_LEFT_CONTROL:
        case EKey::KEY_RIGHT_CONTROL:
        case EKey::KEY_LEFT_SHIFT:
        case EKey::KEY_RIGHT_SHIFT:
        case EKey::KEY_LEFT_ALT:
        case EKey::KEY_RIGHT_ALT:
        case EKey::KEY_LEFT_SUPER:
        case EKey::KEY_RIGHT_SUPER:
            return true;
        default:
            return false;
        }
    }

    inline bool ShortcutBinding::IsValid() const
    {
        return assigned &&
            primaryKey != Windowing::Inputs::EKey::KEY_UNKNOWN &&
            !IsModifierKey(primaryKey);
    }

    inline bool ShortcutBinding::operator==(const ShortcutBinding& p_other) const
    {
        return assigned == p_other.assigned &&
            primaryKey == p_other.primaryKey &&
            modifiers == p_other.modifiers;
    }

    inline bool ShortcutBinding::operator!=(const ShortcutBinding& p_other) const
    {
        return !(*this == p_other);
    }

    inline bool ShortcutBinding::operator<(const ShortcutBinding& p_other) const
    {
        return std::tie(assigned, primaryKey, modifiers) <
            std::tie(p_other.assigned, p_other.primaryKey, p_other.modifiers);
    }

    inline std::string KeyToDisplayString(const Windowing::Inputs::EKey p_key)
    {
        using Windowing::Inputs::EKey;
        const auto value = static_cast<int>(p_key);
        if (value >= static_cast<int>(EKey::KEY_A) && value <= static_cast<int>(EKey::KEY_Z))
            return std::string(1, static_cast<char>('A' + value - static_cast<int>(EKey::KEY_A)));
        if (value >= static_cast<int>(EKey::KEY_0) && value <= static_cast<int>(EKey::KEY_9))
            return std::string(1, static_cast<char>('0' + value - static_cast<int>(EKey::KEY_0)));

        switch (p_key)
        {
        case EKey::KEY_SPACE: return "Space";
        case EKey::KEY_ESCAPE: return "Esc";
        case EKey::KEY_ENTER: return "Enter";
        case EKey::KEY_TAB: return "Tab";
        case EKey::KEY_BACKSPACE: return "Backspace";
        case EKey::KEY_DELETE: return "Delete";
        case EKey::KEY_F5: return "F5";
        case EKey::KEY_F11: return "F11";
        default: return "Unknown";
        }
    }

    inline std::string ToDisplayString(const ShortcutBinding& p_binding)
    {
        if (!p_binding.assigned)
            return "Unassigned";

        std::string result;
        const auto append = [&result](const std::string& p_part)
        {
            if (!result.empty())
                result += " + ";
            result += p_part;
        };

        if (HasModifier(p_binding.modifiers, EShortcutModifier::Ctrl))
            append("Ctrl");
        if (HasModifier(p_binding.modifiers, EShortcutModifier::Shift))
            append("Shift");
        if (HasModifier(p_binding.modifiers, EShortcutModifier::Alt))
            append("Alt");
        if (HasModifier(p_binding.modifiers, EShortcutModifier::Super))
            append("Super");

        append(KeyToDisplayString(p_binding.primaryKey));
        return result;
    }
}
