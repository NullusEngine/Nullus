#pragma once

#include <cstdint>

namespace NLS::Editor::Icons
{
    enum class IconState
    {
        Normal,
        Hovered,
        Pressed,
        Selected,
        Disabled,
        Danger
    };

    enum class EditorIconThemeVariant
    {
        Dark,
        Light
    };

    /**
    * Editor icon color tokens. Values use ImGui's IM_COL32 byte order so they
    * can be consumed by IconPainter without Runtime/UI depending on this header.
    */
    struct EditorIconTheme
    {
        std::uint32_t primary = 0;
        std::uint32_t secondary = 0;
        std::uint32_t muted = 0;
        std::uint32_t accent = 0;
        std::uint32_t success = 0;
        std::uint32_t warning = 0;
        std::uint32_t danger = 0;
        std::uint32_t disabled = 0;
    };

    const EditorIconTheme& GetEditorIconTheme();
    const EditorIconTheme& GetEditorIconTheme(EditorIconThemeVariant p_variant);

    void SetEditorIconThemeVariant(EditorIconThemeVariant p_variant);
    EditorIconThemeVariant GetEditorIconThemeVariant();

    // Tint for a monochrome icon. Color icons ignore the theme and only honor
    // the disabled alpha contract.
    std::uint32_t IconStateColor(IconState p_state, bool p_tintable);
    std::uint32_t IconStateColor(IconState p_state, bool p_tintable, EditorIconThemeVariant p_variant);
}
