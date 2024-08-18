
#include "UI/Panels/PanelWindow.h"
#include "UI/Internal/Converter.h"

#include "ImGui/imgui_internal.h"

namespace NLS
{
UI::Panels::PanelWindow::PanelWindow(const std::string& p_name, bool p_opened, const Settings::PanelWindowSettings& p_floatingPanelSettings)
    : name(p_name), panelSettings(p_floatingPanelSettings), m_opened(p_opened)
{
    autoSize = p_floatingPanelSettings.autoSize;
}

void UI::Panels::PanelWindow::Open()
{
    if (!m_opened)
    {
        m_opened = true;
        OpenEvent.Invoke();
    }
}

void UI::Panels::PanelWindow::Close()
{
    if (m_opened)
    {
        m_opened = false;
        CloseEvent.Invoke();
    }
}

void UI::Panels::PanelWindow::Focus()
{
    ImGui::SetWindowFocus((name + m_panelID).c_str());
}

void UI::Panels::PanelWindow::SetOpened(bool p_value)
{
    if (p_value != m_opened)
    {
        m_opened = p_value;

        if (m_opened)
            OpenEvent.Invoke();
        else
            CloseEvent.Invoke();
    }
}

bool UI::Panels::PanelWindow::IsOpened() const
{
    return m_opened;
}

bool UI::Panels::PanelWindow::IsHovered() const
{
    return m_hovered;
}

bool UI::Panels::PanelWindow::IsFocused() const
{
    return m_focused;
}

bool UI::Panels::PanelWindow::IsAppearing() const
{
    if (auto window = ImGui::FindWindowByName((name + GetPanelID()).c_str()); window)
        return window->Appearing;
    else
        return false;
}

void UI::Panels::PanelWindow::ScrollToBottom()
{
    m_mustScrollToBottom = true;
}

void UI::Panels::PanelWindow::ScrollToTop()
{
    m_mustScrollToTop = true;
}

bool UI::Panels::PanelWindow::IsScrolledToBottom() const
{
    return m_scrolledToBottom;
}

bool UI::Panels::PanelWindow::IsScrolledToTop() const
{
    return m_scrolledToTop;
}

void UI::Panels::PanelWindow::_Draw_Impl()
{
    if (m_opened)
    {
        int windowFlags = ImGuiWindowFlags_None;

        if (!panelSettings.resizable)
            windowFlags |= ImGuiWindowFlags_NoResize;
        if (!panelSettings.movable)
            windowFlags |= ImGuiWindowFlags_NoMove;
        if (!panelSettings.dockable)
            windowFlags |= ImGuiWindowFlags_NoDocking;
        if (panelSettings.hideBackground)
            windowFlags |= ImGuiWindowFlags_NoBackground;
        if (panelSettings.forceHorizontalScrollbar)
            windowFlags |= ImGuiWindowFlags_AlwaysHorizontalScrollbar;
        if (panelSettings.forceVerticalScrollbar)
            windowFlags |= ImGuiWindowFlags_AlwaysVerticalScrollbar;
        if (panelSettings.allowHorizontalScrollbar)
            windowFlags |= ImGuiWindowFlags_HorizontalScrollbar;
        if (!panelSettings.bringToFrontOnFocus)
            windowFlags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (!panelSettings.collapsable)
            windowFlags |= ImGuiWindowFlags_NoCollapse;
        if (!panelSettings.allowInputs)
            windowFlags |= ImGuiWindowFlags_NoInputs;
        if (!panelSettings.scrollable)
            windowFlags |= ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;
        if (!panelSettings.titleBar)
            windowFlags |= ImGuiWindowFlags_NoTitleBar;

        ImVec2 minSizeConstraint = Internal::Converter::ToImVec2(minSize);
        ImVec2 maxSizeConstraint = Internal::Converter::ToImVec2(maxSize);

        /* Cancel constraint if x or y is <= 0.f */
        if (minSizeConstraint.x <= 0.f || minSizeConstraint.y <= 0.f)
            minSizeConstraint = {0.0f, 0.0f};

        if (maxSizeConstraint.x <= 0.f || maxSizeConstraint.y <= 0.f)
            maxSizeConstraint = {10000.f, 10000.f};

        ImGui::SetNextWindowSizeConstraints(minSizeConstraint, maxSizeConstraint);

        if (ImGui::Begin((name + m_panelID).c_str(), panelSettings.closable ? &m_opened : nullptr, windowFlags))
        {
            m_hovered = ImGui::IsWindowHovered();
            m_focused = ImGui::IsWindowFocused();

            auto scrollY = ImGui::GetScrollY();

            m_scrolledToBottom = scrollY == ImGui::GetScrollMaxY();
            m_scrolledToTop = scrollY == 0.0f;

            if (!m_opened)
                CloseEvent.Invoke();

            Update();

            if (m_mustScrollToBottom)
            {
                ImGui::SetScrollY(ImGui::GetScrollMaxY());
                m_mustScrollToBottom = false;
            }

            if (m_mustScrollToTop)
            {
                ImGui::SetScrollY(0.0f);
                m_mustScrollToTop = false;
            }

            DrawWidgets();
        }

        ImGui::End();
    }
}
} // namespace NLS
