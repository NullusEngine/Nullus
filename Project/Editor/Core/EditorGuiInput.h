#pragma once
#include <ImGui/imgui_internal.h>

namespace NLS::Editor::Core
{
inline bool IsEditorTextInputActive()
{
    // WantTextInput is computed by NewFrame. A widget can relinquish focus
    // while drawing, before this frame's input routing runs.
    const auto* context = ImGui::GetCurrentContext();
    return context && context->ActiveId != 0 && context->InputTextState.ID == context->ActiveId;
}

inline bool TryFocusViewportForNavigation(ImGuiWindow* viewport, const ImVec2 mousePosition,
    bool freshNavigationPress, bool pointerOwnerHeld)
{
    auto* context = ImGui::GetCurrentContext();
    if (!context || !viewport || !freshNavigationPress || pointerOwnerHeld || context->DragDropActive ||
        !ImGui::IsWindowContentHoverable(viewport)) return false;
    ImGuiWindow* hovered = nullptr;
    if (context->WithinFrameScope && mousePosition.x == context->IO.MousePos.x && mousePosition.y == context->IO.MousePos.y)
        hovered = context->HoveredWindow;
    else ImGui::FindHoveredWindowEx(mousePosition, false, &hovered, nullptr);
    if (hovered != viewport) return false;
    if (context->ActiveId != 0 && context->ActiveIdWindow != viewport)
    {
        if (context->ActiveIdSource != ImGuiInputSource_Mouse && !IsEditorTextInputActive()) return false;
        ImGui::ClearActiveID();
    }
    ImGui::FocusWindow(viewport);
    return true;
}
}
