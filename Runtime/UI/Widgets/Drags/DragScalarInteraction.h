#pragma once

#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"

namespace NLS::UI::Widgets::DragScalarInternal
{
    inline bool IsDragScalarTextInputActive(const ImGuiID p_itemId)
    {
        return ImGui::TempInputIsActive(p_itemId);
    }
}
