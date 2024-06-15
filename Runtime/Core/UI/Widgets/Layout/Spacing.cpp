#include "UI/Widgets/Layout/Spacing.h"

namespace NLS
{
UI::Widgets::Layout::Spacing::Spacing(uint16_t p_spaces)
    : spaces(p_spaces)
{
}

void UI::Widgets::Layout::Spacing::_Draw_Impl()
{
    for (uint16_t i = 0; i < spaces; ++i)
    {
        ImGui::Spacing();

        if (i + 1 < spaces)
            ImGui::SameLine();
    }
}
} // namespace NLS
