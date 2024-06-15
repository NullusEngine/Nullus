#include "UI/Widgets/Layout/GroupCollapsable.h"
#include "ImGui/imgui_internal.h"

namespace NLS
{
UI::Widgets::Layout::GroupCollapsable::GroupCollapsable(const std::string& p_name)
    : name(p_name)
{
}

void UI::Widgets::Layout::GroupCollapsable::_Draw_Impl()
{
    bool previouslyOpened = opened;

    if (ImGui::CollapsingHeader(name.c_str(), closable ? &opened : nullptr))
        Group::_Draw_Impl();

    if (opened != previouslyOpened)
    {
        if (opened)
            OpenEvent.Invoke();
        else
            CloseEvent.Invoke();
    }
}
} // namespace NLS
