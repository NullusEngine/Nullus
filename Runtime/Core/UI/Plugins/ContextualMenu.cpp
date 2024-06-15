#include "UI/Plugins/ContextualMenu.h"

namespace NLS
{
void UI::Plugins::ContextualMenu::Execute()
{
    if (ImGui::BeginPopupContextItem())
    {
        DrawWidgets();
        ImGui::EndPopup();
    }
}

void UI::Plugins::ContextualMenu::Close()
{
    ImGui::CloseCurrentPopup();
}

} // namespace NLS
