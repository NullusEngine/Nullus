#include "UI/Widgets/Layout/TreeNode.h"

namespace NLS::UI::Widgets
{
TreeNode::TreeNode(const std::string& p_name, bool p_arrowClickToOpen)
    : DataWidget(name), name(p_name), m_arrowClickToOpen(p_arrowClickToOpen)
{
    m_autoExecutePlugins = false;
}

void TreeNode::Open()
{
    m_shouldOpen = true;
    m_shouldClose = false;
}

void TreeNode::Close()
{
    m_shouldClose = true;
    m_shouldOpen = false;
}

bool TreeNode::IsOpened() const
{
    return m_opened;
}

void TreeNode::_Draw_Impl()
{
    bool prevOpened = m_opened;

    if (m_shouldOpen)
    {
        ImGui::SetNextItemOpen(true);
        m_shouldOpen = false;
    }
    else if (m_shouldClose)
    {
        ImGui::SetNextItemOpen(false);
        m_shouldClose = false;
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding;
    if (m_arrowClickToOpen)
        flags |= ImGuiTreeNodeFlags_OpenOnArrow;
    if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (leaf)
        flags |= ImGuiTreeNodeFlags_Leaf;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.29f, 0.40f, 0.68f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.24f, 0.36f, 0.52f, 0.82f));
    bool opened = ImGui::TreeNodeEx((name + m_widgetID).c_str(), flags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();

    if (ImGui::IsItemClicked() && (ImGui::GetMousePos().x - ImGui::GetItemRectMin().x) > ImGui::GetTreeNodeToLabelSpacing())
    {
        ClickedEvent.Invoke();

        if (ImGui::IsMouseDoubleClicked(0))
        {
            DoubleClickedEvent.Invoke();
        }
    }

    if (opened)
    {
        if (!prevOpened)
            OpenedEvent.Invoke();

        m_opened = true;

        ExecutePlugins(); // Manually execute plugins to make plugins considering the TreeNode and no childs

        DrawWidgets();

        ImGui::TreePop();
    }
    else
    {
        if (prevOpened)
            ClosedEvent.Invoke();

        m_opened = false;

        ExecutePlugins(); // Manually execute plugins to make plugins considering the TreeNode and no childs
    }
}

} // namespace NLS::UI::Widgets
