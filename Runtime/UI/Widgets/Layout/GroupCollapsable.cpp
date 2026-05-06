#include "UI/Widgets/Layout/GroupCollapsable.h"
#include "Core/ServiceLocator.h"
#include "UI/UIManager.h"
#include "ImGui/imgui_internal.h"

namespace NLS::UI::Widgets
{
namespace
{
float UiScale()
{
    return NLS::Core::ServiceLocator::Contains<UIManager>()
        ? NLS_SERVICE(UIManager).GetScale()
        : 1.0f;
}
}

GroupCollapsable::GroupCollapsable(const std::string& p_name)
    : name(p_name)
{
}

void GroupCollapsable::_Draw_Impl()
{
    bool previouslyOpened = opened;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float uiScale = UiScale();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * uiScale, 6.0f * uiScale));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.19f, 0.22f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.21f, 0.24f, 0.29f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.24f, 0.30f, 0.38f, 1.0f));

    if (ImGui::CollapsingHeader(name.c_str(), closable ? &opened : nullptr, ImGuiTreeNodeFlags_SpanFullWidth))
    {
        const ImVec2 bodyMin = ImGui::GetCursorScreenPos();
        const float bodyWidth = ImGui::GetContentRegionAvail().x;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * uiScale, 7.0f * uiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * uiScale, 5.0f * uiScale));
        ImGui::Indent(4.0f * uiScale);
        Group::_Draw_Impl();
        ImGui::Unindent(4.0f * uiScale);
        ImGui::PopStyleVar(2);

        const ImVec2 bodyMax = ImGui::GetCursorScreenPos();
        if (bodyMax.y > bodyMin.y)
        {
            drawList->AddRect(
                ImVec2(bodyMin.x - 8.0f * uiScale, bodyMin.y - 4.0f * uiScale),
                ImVec2(bodyMin.x + bodyWidth + 4.0f * uiScale, bodyMax.y + 6.0f * uiScale),
                IM_COL32(58, 62, 70, 220),
                4.0f * uiScale);
        }

        ImGui::Spacing();
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    if (opened != previouslyOpened)
    {
        if (opened)
            OpenEvent.Invoke();
        else
            CloseEvent.Invoke();
    }
}
} // namespace NLS::UI::Widgets
