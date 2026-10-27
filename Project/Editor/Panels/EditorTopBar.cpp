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
#include "Panels/SceneView.h"

namespace NLS::Editor::Panels
{
namespace
{
constexpr float kTopBarHeight = 54.0f;
constexpr float kToolbarRowOffsetY = 24.0f;
constexpr float kToolbarApproxWidth = 150.0f;
constexpr float kSceneToolAreaWidth = 300.0f;
constexpr float kCompactSceneToolAreaWidth = 108.0f;
constexpr float kSceneToolComboWidth = 110.0f;
constexpr float kToolbarLeftPadding = 8.0f;
constexpr float kToolbarSpacing = 16.0f;

const char* ToToolLabel(const Editor::Core::EGizmoOperation p_operation)
{
    switch (p_operation)
    {
    case Editor::Core::EGizmoOperation::TRANSLATE:
        return "Move";
    case Editor::Core::EGizmoOperation::ROTATE:
        return "Rotate";
    case Editor::Core::EGizmoOperation::SCALE:
        return "Scale";
    default:
        return "Tool";
    }
}

const char* ToPivotLabel(const Editor::Core::SceneViewGizmoPivot p_pivot)
{
    return p_pivot == Editor::Core::SceneViewGizmoPivot::Center ? "Center" : "Pivot";
}

const char* ToSpaceLabel(const Editor::Core::SceneViewGizmoSpace p_space)
{
    return p_space == Editor::Core::SceneViewGizmoSpace::Local ? "Local" : "Global";
}

Icons::IconId ToSpaceIconId(const Editor::Core::SceneViewGizmoSpace p_space)
{
    return p_space == Editor::Core::SceneViewGizmoSpace::Local
        ? Icons::EditorIcons::ToolbarLocal
        : Icons::EditorIcons::ToolbarGlobal;
}

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

    const float availableWidth = GetContentWidth();
    const float rowY = Scaled(kToolbarRowOffsetY);
    DrawSceneToolRow(rowY, availableWidth);

    const bool compactLayout = availableWidth < Scaled(700.0f);
    const bool comboLayout = availableWidth < Scaled(520.0f);
    const float toolAreaWidth = comboLayout
        ? Scaled(kSceneToolComboWidth)
        : (compactLayout ? Scaled(kCompactSceneToolAreaWidth) : Scaled(kSceneToolAreaWidth));
    const float minimumToolbarX = Scaled(kToolbarLeftPadding) + toolAreaWidth + Scaled(kToolbarSpacing);
    const float centeredToolbarX = std::max((availableWidth - Scaled(kToolbarApproxWidth)) * 0.5f, Scaled(kToolbarLeftPadding));
    const float toolbarX = compactLayout
        ? minimumToolbarX
        : std::max(centeredToolbarX, minimumToolbarX);

    ImGui::SetCursorPos(ImVec2(toolbarX, rowY));
    m_toolbar.DrawToolbarContent();
}

void EditorTopBar::DrawSceneToolRow(const float p_rowY, const float p_availableWidth)
{
    auto& sceneView = EDITOR_PANEL(Panels::SceneView, "Scene View");
    const auto currentOperation = sceneView.GetCurrentGizmoOperation();
    const auto currentPivot = sceneView.GetCurrentGizmoPivot();
    const auto currentSpace = sceneView.GetCurrentGizmoSpace();
    const bool compactLayout = p_availableWidth < Scaled(700.0f);
    const bool comboLayout = p_availableWidth < Scaled(520.0f);
    const bool showReferencePlaceholders = p_availableWidth >= Scaled(900.0f);
    const float toolAreaWidth = comboLayout
        ? Scaled(kSceneToolComboWidth)
        : (compactLayout ? Scaled(kCompactSceneToolAreaWidth) : Scaled(kSceneToolAreaWidth));

    (void)toolAreaWidth;
    ImGui::SetCursorPos(ImVec2(Scaled(kToolbarLeftPadding), p_rowY - Scaled(1.0f)));

    if (comboLayout)
    {
        ImGui::SetNextItemWidth(Scaled(kSceneToolComboWidth));
        if (ImGui::BeginCombo("##SceneToolMode", ToToolLabel(currentOperation)))
        {
            const auto drawSelectable = [&](const char* label, const Editor::Core::EGizmoOperation operation)
            {
                const bool selected = currentOperation == operation;
                if (ImGui::Selectable(label, selected))
                    sceneView.SetCurrentGizmoOperation(operation);
                if (selected)
                    ImGui::SetItemDefaultFocus();
            };

            drawSelectable("Move", Editor::Core::EGizmoOperation::TRANSLATE);
            drawSelectable("Rotate", Editor::Core::EGizmoOperation::ROTATE);
            drawSelectable("Scale", Editor::Core::EGizmoOperation::SCALE);
            ImGui::EndCombo();
        }

        return;
    }

    DrawSceneToolButton(
        "##SceneToolMove",
        Icons::EditorIcons::ToolbarMove,
        "Move",
        Editor::Core::EGizmoOperation::TRANSLATE,
        currentOperation == Editor::Core::EGizmoOperation::TRANSLATE);
    ImGui::SameLine(0.0f, Scaled(4.0f));
    DrawSceneToolButton(
        "##SceneToolRotate",
        Icons::EditorIcons::ToolbarRotate,
        "Rotate",
        Editor::Core::EGizmoOperation::ROTATE,
        currentOperation == Editor::Core::EGizmoOperation::ROTATE);
    ImGui::SameLine(0.0f, Scaled(4.0f));
    DrawSceneToolButton(
        "##SceneToolScale",
        Icons::EditorIcons::ToolbarScale,
        "Scale",
        Editor::Core::EGizmoOperation::SCALE,
        currentOperation == Editor::Core::EGizmoOperation::SCALE);

    if (!showReferencePlaceholders)
        return;

    ImGui::SameLine(0.0f, Scaled(10.0f));
    if (DrawIconTextButton(
            "##SceneToolPivot",
            GetToolbarPivotIconId(currentPivot),
            ToPivotLabel(currentPivot),
            "Toggle Pivot Position"))
    {
        sceneView.ToggleCurrentGizmoPivot();
    }
    ImGui::SameLine(0.0f, Scaled(4.0f));
    if (DrawIconTextButton(
            "##SceneToolSpace",
            ToSpaceIconId(currentSpace),
            ToSpaceLabel(currentSpace),
            "Toggle Pivot Orientation"))
    {
        sceneView.ToggleCurrentGizmoSpace();
    }
}

void EditorTopBar::DrawSceneToolButton(
    const char* p_widgetId,
    Icons::IconId p_icon,
    const char* p_tooltip,
    const Editor::Core::EGizmoOperation p_operation,
    const bool p_active)
{
    Icons::IconButtonOptions options;
    options.tooltip = p_tooltip;
    options.selected = p_active;

    const bool clicked = EDITOR_CONTEXT(editorResources)->GetIconPainter().Button(
        p_widgetId,
        p_icon,
        Icons::IconToken::Small,
        options);

    if (clicked)
        EDITOR_PANEL(Panels::SceneView, "Scene View").SetCurrentGizmoOperation(p_operation);
}

bool EditorTopBar::DrawIconTextButton(
    const char* p_widgetId,
    Icons::IconId p_icon,
    const char* p_label,
    const char* p_tooltip)
{
    Icons::IconButtonOptions options;
    options.tooltip = p_tooltip;

    return EDITOR_CONTEXT(editorResources)->GetIconPainter().TextButton(
        p_widgetId,
        p_icon,
        p_label,
        Icons::IconToken::Small,
        options);
}
}
