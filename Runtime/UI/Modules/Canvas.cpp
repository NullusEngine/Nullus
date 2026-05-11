#include "UI/Modules/Canvas.h"
#include "UI/Panels/PanelViewportBar.h"

#include "Profiling/Profiler.h"

namespace NLS::UI
{
void Canvas::Draw()
{
    NLS_PROFILE_SCOPE();
    if (!m_panels.empty())
    {
        if (m_isDockspace)
        {
            {
                NLS_PROFILE_NAMED_SCOPE("Canvas::RefreshViewportBars");
                PanelViewportBar::ResetReservedHeights();
                for (auto& panel : m_panels)
                {
                    if (auto* viewportBar = dynamic_cast<PanelViewportBar*>(&panel.get());
                        viewportBar != nullptr && viewportBar->enabled)
                    {
                        viewportBar->RefreshReservedLayout();
                    }
                }
            }

            {
                NLS_PROFILE_NAMED_SCOPE("Canvas::DrawDockspace");
                ImGuiViewport* viewport = ImGui::GetMainViewport();
                const float topInset = PanelViewportBar::GetReservedTopHeight();
                const float bottomInset = PanelViewportBar::GetReservedBottomHeight();
                const float dockspaceHeight = std::max(viewport->Size.y - topInset - bottomInset, 1.0f);
                ImDrawList* backgroundDrawList = ImGui::GetBackgroundDrawList(viewport);
                backgroundDrawList->AddRectFilled(
                    viewport->Pos,
                    ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
                    ImGui::GetColorU32(ImGuiCol_DockingEmptyBg));
                ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + topInset));
                ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, dockspaceHeight));
                ImGui::SetNextWindowViewport(viewport->ID);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

                ImGui::Begin(
                    "##dockspace",
                    nullptr,
                    ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoBringToFrontOnFocus |
                    ImGuiWindowFlags_NoDocking |
                    ImGuiWindowFlags_NoSavedSettings);
                ImGuiID dockspace_id = ImGui::GetID("MyDockspace");
                ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
                ImGui::End();

                ImGui::PopStyleVar(3);
            }
        }

        {
            NLS_PROFILE_NAMED_SCOPE("Canvas::DrawPanels");
            for (auto& panel : m_panels)
            {
                NLS_PROFILE_NAMED_SCOPE("Canvas::DrawPanel");
                panel.get().Draw();
            }
        }
    }
}

void Canvas::AddPanel(APanel& p_panel)
{
    m_panels.push_back(std::ref(p_panel));
}

void Canvas::RemovePanel(APanel& p_panel)
{
    m_panels.erase(std::remove_if(m_panels.begin(), m_panels.end(), [&p_panel](std::reference_wrapper<APanel>& p_item)
                                  { return &p_panel == &p_item.get(); }));
}

void Canvas::RemoveAllPanels()
{
    m_panels.clear();
}

void Canvas::MakeDockspace(bool p_state)
{
    m_isDockspace = p_state;
}

bool Canvas::IsDockspace() const
{
    return m_isDockspace;
}
} // namespace NLS::UI
