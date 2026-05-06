#include "UI/Panels/PanelViewportBar.h"

#include <algorithm>

#include "Core/ServiceLocator.h"
#include "UI/UIManager.h"

namespace NLS::UI
{
namespace
{
float ScaleValue(const float p_value)
{
    return NLS::Core::ServiceLocator::Contains<UIManager>()
        ? NLS_SERVICE(UIManager).Scale(p_value)
        : p_value;
}
}

PanelViewportBar::PanelViewportBar(
    const std::string& p_windowName,
    const EAnchor p_anchor,
    const float p_height,
    const bool p_includeMenuBar)
    : m_windowName(p_windowName),
      m_anchor(p_anchor),
      m_height(p_height),
      m_includeMenuBar(p_includeMenuBar)
{
    RefreshReservedHeight(m_anchor, m_height);
}

float PanelViewportBar::GetReservedTopHeight()
{
    return s_reservedTopHeight;
}

float PanelViewportBar::GetReservedBottomHeight()
{
    return s_reservedBottomHeight;
}

void PanelViewportBar::ResetReservedHeights()
{
    s_reservedTopHeight = 0.0f;
    s_reservedBottomHeight = 0.0f;
}

void PanelViewportBar::SetHeight(const float p_height)
{
    m_height = p_height;
    RefreshReservedHeight(m_anchor, m_height);
}

float PanelViewportBar::GetHeight() const
{
    return ScaleValue(m_height);
}

void PanelViewportBar::RefreshReservedLayout() const
{
    RefreshReservedHeight(m_anchor, GetHeight());
}

void PanelViewportBar::_Draw_Impl()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scaledHeight = GetHeight();
    const float posY = m_anchor == EAnchor::TOP
        ? viewport->Pos.y
        : viewport->Pos.y + viewport->Size.y - scaledHeight;

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, posY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, scaledHeight), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ScaleValue(8.0f), ScaleValue(4.0f)));

    int flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (m_includeMenuBar)
        flags |= ImGuiWindowFlags_MenuBar;

    if (ImGui::Begin((m_windowName + m_panelID).c_str(), nullptr, flags))
        DrawBarContent();

    ImGui::End();
    ImGui::PopStyleVar(3);
}

float PanelViewportBar::GetContentWidth() const
{
    return ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
}

void PanelViewportBar::RefreshReservedHeight(const EAnchor p_anchor, const float p_height)
{
    if (p_anchor == EAnchor::TOP)
        s_reservedTopHeight = std::max(s_reservedTopHeight, p_height);
    else
        s_reservedBottomHeight = std::max(s_reservedBottomHeight, p_height);
}
}
