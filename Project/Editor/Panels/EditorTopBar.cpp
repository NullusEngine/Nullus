#include "Panels/EditorTopBar.h"
#include "ImGui/imgui.h"

#include <algorithm>
#include <memory>
#include <string>

#include <ServiceLocator.h>
#include <UI/UIManager.h>

#include "Core/Editor.h"
#include "Core/EditorResources.h"
#include "Icons/EditorTheme.h"
#include "Icons/IconPainter.h"

namespace NLS::Editor::Panels
{
namespace
{
constexpr float kTopBarHeight = 54.0f;
constexpr float kToolbarRowOffsetY = 24.0f;
constexpr float kToolbarApproxWidth = 150.0f;
constexpr float kToolbarLeftPadding = 8.0f;

float UiScale()
{
    return NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>()
        ? NLS_SERVICE(NLS::UI::UIManager).GetScale()
        : 1.0f;
}

float Scaled(const float p_value)
{
    return p_value * UiScale();
}

ImVec2 Scaled(const float p_x, const float p_y)
{
    return ImVec2(Scaled(p_x), Scaled(p_y));
}
}

EditorTopBar::EditorTopBar()
    : PanelViewportBar("EditorTopBar", UI::PanelViewportBar::EAnchor::TOP, kTopBarHeight, true),
      m_menuBar(),
      m_toolbar("EmbeddedToolbar", true, UI::PanelWindowSettings{})
{
}

void EditorTopBar::RegisterWindowPanel(const std::string& p_name, UI::PanelWindow& p_panel)
{
    m_menuBar.RegisterPanel(p_name, p_panel);
}

void EditorTopBar::RegisterProjectSettingsPanel(ProjectSettings& p_panel)
{
    m_menuBar.RegisterProjectSettingsPanel(p_panel);
}

bool EditorTopBar::RegisterCommand(const std::string& p_menuPath, std::function<void()> p_execute)
{
    return m_menuBar.RegisterCommand(p_menuPath, std::move(p_execute));
}
void EditorTopBar::UnregisterCommand(const std::string& path) { m_menuBar.UnregisterCommand(path); }

void EditorTopBar::DrawDialogs()
{
    m_menuBar.DrawDialogs();
}

void EditorTopBar::DrawBarContent()
{
    if (ImGui::BeginMenuBar())
    {
        m_menuBar.DrawMenuEntries();
        ImGui::EndMenuBar();
    }

    m_menuBar.DrawDialogs();
    DrawToolbarRow();
}

void EditorTopBar::DrawToolbarRow()
{
    if (!m_iconsPreloaded)
    {
        // First-screen set only; everything else loads lazily on first use.
        EDITOR_CONTEXT(editorResources)->GetIconService().Preload(
            {
                Icons::IconId{ Icons::EditorIcons::ToolbarMove },
                Icons::IconId{ Icons::EditorIcons::ToolbarRotate },
                Icons::IconId{ Icons::EditorIcons::ToolbarScale },
                Icons::IconId{ Icons::EditorIcons::ToolbarPivot },
                Icons::IconId{ Icons::EditorIcons::ToolbarCenter },
                Icons::IconId{ Icons::EditorIcons::ToolbarGlobal },
                Icons::IconId{ Icons::EditorIcons::ToolbarLocal },
                Icons::IconId{ Icons::EditorIcons::AssetDefault },
            },
            16);
        m_iconsPreloaded = true;
    }

    if (!m_idlePreloadComplete)
    {
        // Spread the remaining first-use decode/upload over a few frames
        // instead of hitching the first time a panel opens.
        m_idlePreloadComplete = !EDITOR_CONTEXT(editorResources)->GetIconService()
            .PreloadIdleStep(2, 32);
    }

    const float toolbarX = std::max((GetContentWidth() - Scaled(kToolbarApproxWidth)) * 0.5f, Scaled(kToolbarLeftPadding));
    ImGui::SetCursorPos(ImVec2(toolbarX, Scaled(kToolbarRowOffsetY)));
    m_toolbar.DrawToolbarContent();
}
}
