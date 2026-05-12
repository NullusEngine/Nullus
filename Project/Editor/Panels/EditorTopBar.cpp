#include "Panels/EditorTopBar.h"
#include "ImGui/imgui.h"

#include <algorithm>
#include <memory>
#include <string>

#include <ServiceLocator.h>
#include <UI/Internal/Converter.h>
#include <UI/UIManager.h>

#include "Core/Editor.h"
#include "Core/EditorResources.h"
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
constexpr float kSceneToolIconSize = 16.0f;
constexpr float kSceneToolButtonSize = 25.0f;

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

const char* ToSpaceIconId(const Editor::Core::SceneViewGizmoSpace p_space)
{
    return p_space == Editor::Core::SceneViewGizmoSpace::Local ? "Toolbar_Local" : "Toolbar_Global";
}

std::shared_ptr<NLS::Render::RHI::RHITextureView> GetToolbarIconView(const char* p_iconId)
{
    auto* texture = EDITOR_CONTEXT(editorResources)->GetTexture(p_iconId);
    return texture != nullptr
        ? texture->GetOrCreateExplicitTextureView(std::string("EditorTopBar.") + p_iconId)
        : nullptr;
}

void* ResolveTextureId(const std::shared_ptr<NLS::Render::RHI::RHITextureView>& p_textureView)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
        return nullptr;

    const auto nativeHandle = NLS_SERVICE(NLS::UI::UIManager).ResolveTextureView(p_textureView);
    return nativeHandle.IsValid() ? nativeHandle.handle : nullptr;
}

void DrawToolbarTooltip(const char* p_text)
{
    if (p_text != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", p_text);
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

void EditorTopBar::RegisterProjectSettingsPanel(ProjectSettings& p_panel)
{
    m_menuBar.RegisterProjectSettingsPanel(p_panel);
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
        "Toolbar_Move",
        "Move",
        Editor::Core::EGizmoOperation::TRANSLATE,
        currentOperation == Editor::Core::EGizmoOperation::TRANSLATE);
    ImGui::SameLine(0.0f, Scaled(4.0f));
    DrawSceneToolButton(
        "Toolbar_Rotate",
        "Rotate",
        Editor::Core::EGizmoOperation::ROTATE,
        currentOperation == Editor::Core::EGizmoOperation::ROTATE);
    ImGui::SameLine(0.0f, Scaled(4.0f));
    DrawSceneToolButton(
        "Toolbar_Scale",
        "Scale",
        Editor::Core::EGizmoOperation::SCALE,
        currentOperation == Editor::Core::EGizmoOperation::SCALE);

    if (!showReferencePlaceholders)
        return;

    ImGui::SameLine(0.0f, Scaled(10.0f));
    if (DrawIconTextButton(GetToolbarPivotIconId(currentPivot), ToPivotLabel(currentPivot), "Toggle Pivot Position"))
        sceneView.ToggleCurrentGizmoPivot();
    ImGui::SameLine(0.0f, Scaled(4.0f));
    if (DrawIconTextButton(ToSpaceIconId(currentSpace), ToSpaceLabel(currentSpace), "Toggle Pivot Orientation"))
        sceneView.ToggleCurrentGizmoSpace();
}

void EditorTopBar::DrawSceneToolButton(
    const char* p_iconId,
    const char* p_tooltip,
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

    const auto textureView = GetToolbarIconView(p_iconId);
    ImGui::PushID(p_iconId);
    const bool clicked = ImGui::ImageButton(
        ResolveTextureId(textureView),
        Scaled(kSceneToolIconSize, kSceneToolIconSize),
        ImVec2(0.0f, 1.0f),
        ImVec2(1.0f, 0.0f),
        -1,
        ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
        ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PopID();
    DrawToolbarTooltip(p_tooltip);
    if (clicked)
        EDITOR_PANEL(Panels::SceneView, "Scene View").SetCurrentGizmoOperation(p_operation);

    ImGui::GetStyle().Colors[ImGuiCol_Button] = previousButton;
    ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] = previousHovered;
    ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] = previousActive;
}

bool EditorTopBar::DrawIconTextButton(const char* p_iconId, const char* p_label, const char* p_tooltip)
{
    const ImVec2 labelSize = ImGui::CalcTextSize(p_label);
    const ImVec2 framePadding = ImGui::GetStyle().FramePadding;
    const ImVec2 buttonSize(
        Scaled(kSceneToolIconSize) + labelSize.x + framePadding.x * 3.0f,
        Scaled(kSceneToolButtonSize));

    const bool clicked = ImGui::Button(("##" + std::string(p_label)).c_str(), buttonSize);
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    DrawToolbarTooltip(p_tooltip);

    const auto textureView = GetToolbarIconView(p_iconId);
    const ImVec2 iconPos(
        buttonMin.x + framePadding.x,
        buttonMin.y + (buttonMax.y - buttonMin.y - Scaled(kSceneToolIconSize)) * 0.5f);
    ImGui::GetWindowDrawList()->AddImage(
        ResolveTextureId(textureView),
        iconPos,
        ImVec2(iconPos.x + Scaled(kSceneToolIconSize), iconPos.y + Scaled(kSceneToolIconSize)),
        ImVec2(0.0f, 1.0f),
        ImVec2(1.0f, 0.0f));

    const ImVec2 textPos(
        iconPos.x + Scaled(kSceneToolIconSize) + framePadding.x,
        buttonMin.y + (buttonMax.y - buttonMin.y - labelSize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(textPos, ImGui::GetColorU32(ImGuiCol_Text), p_label);

    return clicked;
}
}
