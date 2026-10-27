#include "Icons/EditorTheme.h"

#include "ImGui/imgui.h"

namespace NLS::Editor::Icons
{
namespace
{
    const EditorIconTheme kDarkTheme {
        IM_COL32(255, 255, 255, 255), // primary
        IM_COL32(206, 206, 206, 255), // secondary
        IM_COL32(150, 150, 150, 255), // muted
        IM_COL32(150, 198, 255, 255), // accent
        IM_COL32(112, 214, 124, 255), // success
        IM_COL32(240, 191, 82, 255),  // warning
        IM_COL32(235, 96, 96, 255),   // danger
        IM_COL32(255, 255, 255, 90)   // disabled
    };

    const EditorIconTheme kLightTheme {
        IM_COL32(42, 46, 52, 255),    // primary
        IM_COL32(84, 88, 94, 255),    // secondary
        IM_COL32(128, 132, 138, 255), // muted
        IM_COL32(32, 112, 200, 255),  // accent
        IM_COL32(32, 140, 64, 255),   // success
        IM_COL32(170, 118, 16, 255),  // warning
        IM_COL32(190, 48, 48, 255),   // danger
        IM_COL32(42, 46, 52, 90)      // disabled
    };

    EditorIconThemeVariant g_variant = EditorIconThemeVariant::Dark;
}

const EditorIconTheme& GetEditorIconTheme()
{
    return GetEditorIconTheme(g_variant);
}

const EditorIconTheme& GetEditorIconTheme(const EditorIconThemeVariant p_variant)
{
    return p_variant == EditorIconThemeVariant::Light ? kLightTheme : kDarkTheme;
}

void SetEditorIconThemeVariant(const EditorIconThemeVariant p_variant)
{
    g_variant = p_variant;
}

EditorIconThemeVariant GetEditorIconThemeVariant()
{
    return g_variant;
}

std::uint32_t IconStateColor(const IconState p_state, const bool p_tintable)
{
    return IconStateColor(p_state, p_tintable, g_variant);
}

std::uint32_t IconStateColor(
    const IconState p_state,
    const bool p_tintable,
    const EditorIconThemeVariant p_variant)
{
    const auto& theme = GetEditorIconTheme(p_variant);
    if (!p_tintable)
    {
        return p_state == IconState::Disabled
            ? IM_COL32(255, 255, 255, 90)
            : IM_COL32(255, 255, 255, 255);
    }

    switch (p_state)
    {
    case IconState::Normal:   return theme.primary;
    case IconState::Hovered:  return theme.primary;
    case IconState::Pressed:  return theme.secondary;
    case IconState::Selected: return theme.accent;
    case IconState::Disabled: return theme.disabled;
    case IconState::Danger:   return theme.danger;
    }
    return theme.primary;
}
}
