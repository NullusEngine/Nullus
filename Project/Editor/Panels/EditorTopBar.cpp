#include "Panels/EditorTopBar.h"

#include <algorithm>

#include <ServiceLocator.h>
#include <UI/Internal/Converter.h>

#include "Core/Editor.h"
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

Maths::Color MakeToolbarHighlight()
{
    return { 0.23f, 0.49f, 0.82f, 1.0f };
}

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
}

EditorTopBar::EditorTopBar()
    : PanelViewportBar("EditorTopBar", UI::PanelViewportBar::EAnchor::TOP, kTopBarHeight, true),
      m_menuBar(),
      m_toolbar("EmbeddedToolbar", true, UI::PanelWindowSettings{})
{
}

void EditorTopBar::InitializeSettingsMenu()
{
    m_menuBar.InitializeSettingsMenu();
}

void EditorTopBar::HandleShortcuts(const float p_deltaTime)
{
    m_menuBar.HandleShortcuts(p_deltaTime);
}

void EditorTopBar::RegisterWindowPanel(const std::string& p_name, UI::PanelWindow& p_panel)
{
    m_menuBar.RegisterPanel(p_name, p_panel);
}

void EditorTopBar::DrawBarContent()
{
    if (ImGui::BeginMenuBar())
    {
        m_menuBar.DrawMenuEntries();
        ImGui::EndMenuBar();
    }

    DrawToolbarRow();
}

void EditorTopBar::DrawToolbarRow()
{
    const float availableWidth = GetContentWidth();
    const float rowY = kToolbarRowOffsetY;
    DrawSceneToolRow(rowY, availableWidth);

    const bool compactLayout = availableWidth < 700.0f;
    const bool comboLayout = availableWidth < 520.0f;
    const float toolAreaWidth = comboLayout
        ? kSceneToolComboWidth
        : (compactLayout ? kCompactSceneToolAreaWidth : kSceneToolAreaWidth);
    const float minimumToolbarX = kToolbarLeftPadding + toolAreaWidth + kToolbarSpacing;
    const float centeredToolbarX = std::max((availableWidth - kToolbarApproxWidth) * 0.5f, kToolbarLeftPadding);
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
    const bool compactLayout = p_availableWidth < 700.0f;
    const bool comboLayout = p_availableWidth < 520.0f;
    const bool showReferencePlaceholders = p_availableWidth >= 900.0f;
    const float toolAreaWidth = comboLayout
        ? kSceneToolComboWidth
        : (compactLayout ? kCompactSceneToolAreaWidth : kSceneToolAreaWidth);

    (void)toolAreaWidth;
    ImGui::SetCursorPos(ImVec2(kToolbarLeftPadding, p_rowY - 1.0f));

    if (comboLayout)
    {
        ImGui::SetNextItemWidth(kSceneToolComboWidth);
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
        compactLayout ? "W" : "Move",
        Editor::Core::EGizmoOperation::TRANSLATE,
        currentOperation == Editor::Core::EGizmoOperation::TRANSLATE);
    ImGui::SameLine(0.0f, 4.0f);
    DrawSceneToolButton(
        compactLayout ? "E" : "Rotate",
        Editor::Core::EGizmoOperation::ROTATE,
        currentOperation == Editor::Core::EGizmoOperation::ROTATE);
    ImGui::SameLine(0.0f, 4.0f);
    DrawSceneToolButton(
        compactLayout ? "R" : "Scale",
        Editor::Core::EGizmoOperation::SCALE,
        currentOperation == Editor::Core::EGizmoOperation::SCALE);

    if (!showReferencePlaceholders)
        return;

    ImGui::SameLine(0.0f, 10.0f);
    if (ImGui::Button(ToPivotLabel(currentPivot)))
        sceneView.ToggleCurrentGizmoPivot();
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button(ToSpaceLabel(currentSpace)))
        sceneView.ToggleCurrentGizmoSpace();
}

void EditorTopBar::DrawSceneToolButton(
    const char* p_label,
    const Editor::Core::EGizmoOperation p_operation,
    const bool p_active)
{
    const ImVec4 previousButton = ImGui::GetStyle().Colors[ImGuiCol_Button];
    const ImVec4 previousHovered = ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered];
    const ImVec4 previousActive = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];

    if (p_active)
    {
        const ImVec4 highlight = UI::Internal::Converter::ToImVec4(MakeToolbarHighlight());
        ImGui::GetStyle().Colors[ImGuiCol_Button] = highlight;
        ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] = highlight;
        ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] = highlight;
    }

    if (ImGui::Button(p_label))
        EDITOR_PANEL(Panels::SceneView, "Scene View").SetCurrentGizmoOperation(p_operation);

    ImGui::GetStyle().Colors[ImGuiCol_Button] = previousButton;
    ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] = previousHovered;
    ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] = previousActive;
}
}
