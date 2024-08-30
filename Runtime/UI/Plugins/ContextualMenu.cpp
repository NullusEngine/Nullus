#include "UI/Plugins/ContextualMenu.h"

namespace NLS::UI
{
void ContextualMenu::Execute()
{
    if (ImGui::BeginPopupContextItem())
    {
        DrawWidgets();
        ImGui::EndPopup();
    }
}

void ContextualMenu::Close()
{
    ImGui::CloseCurrentPopup();
}

} // namespace NLS
