#include "Panels/SceneView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include "Core/EditorResources.h"
#include "Icons/IconPainter.h"
#include "Icons/Generated/IconIds.generated.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"
#include "Shortcuts/EditorCommandInvocation.h"
#include <Debug/Logger.h>
#include <UI/UIManager.h>
#include <GameObject.h>
#include <SceneSystem/Scene.h>
#include <ImGui/imgui.h>

namespace NLS::Editor::Panels
{
namespace
{
using DrawMode = Core::SceneViewDrawMode;
const char* DrawModeLabel(DrawMode mode)
{
    return Core::GetSceneViewDrawModeInfo(mode).label;
}
constexpr std::array<const char*, 7> kGroupNames{
    "Transform Tools", "Handle Position & Space", "Grid & Snap", "Display", "Lighting & Effects", "Visibility & Camera", "Gizmos"};

void UnavailableItem(const char* label, const char* reason)
{
    ImGui::BeginDisabled();
    ImGui::MenuItem(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", reason);
}

void CollectObjectIds(const Engine::GameObject& object, std::vector<int32_t>& ids, bool children = true)
{
    ids.push_back(object.GetInstanceID());
    if (children)
        for (auto* child : object.GetChildren())
            if (child) CollectObjectIds(*child, ids);
}

}

Core::SceneViewToolbarState SceneView::GetToolbarState() const
{
    auto mode = Core::DecodeSceneViewDrawMode(Settings::EditorSettings::GetSceneViewSettingsObject().drawMode);
    if (m_distanceFieldDebug.visualization == Render::DistanceField::DistanceFieldVisualization::Mesh)
        mode = DrawMode::MeshDistanceField;
    else if (m_distanceFieldDebug.visualization == Render::DistanceField::DistanceFieldVisualization::Global)
        mode = DrawMode::GlobalDistanceField;
    return {m_currentOperation, m_currentPivot, m_currentSpace, mode, m_is2D};
}

void SceneView::SetDrawMode(const DrawMode mode)
{
    namespace df = Render::DistanceField;
    const auto previous = GetToolbarState().drawMode;
    if (mode == DrawMode::MeshDistanceField || mode == DrawMode::GlobalDistanceField)
        m_distanceFieldDebug.visualization = mode == DrawMode::MeshDistanceField ?
            df::DistanceFieldVisualization::Mesh : df::DistanceFieldVisualization::Global;
    else
    {
        m_distanceFieldDebug.visualization = df::DistanceFieldVisualization::Off;
        Settings::EditorSettings::GetSceneViewSettingsObject().drawMode = Core::GetSceneViewDrawModeInfo(mode).persistedValue;
        m_toolbarSettingsDirty = true;
    }
    if (previous != GetToolbarState().drawMode)
    {
        ++m_distanceFieldDebugRevision;
        InvalidateStaticFrameCache();
    }
}

void SceneView::Set2D(const bool enabled)
{
    if (m_is2D == enabled) return;
    CancelGizmoEditSession();
    m_cameraController.ResetMouseInteractionState();
    EnsureCameraFocus();
    if (enabled)
    {
        m_saved3DCamera = Core::CaptureSceneViewCamera(m_camera, m_cameraFocus);
        Core::EnterSceneView2D(m_camera, m_cameraFocus);
    }
    else if (m_saved3DCamera)
    {
        Core::RestoreSceneViewCamera(m_camera, m_cameraFocus, *m_saved3DCamera);
        m_saved3DCamera.reset();
    }
    m_is2D = enabled;
    m_cameraController.SetPlanarNavigation(enabled);
    m_cameraMovedForPresentation = true;
    m_pendingClickPickRenderPos.reset();
    m_pendingClickPickingSignature.reset();
    InvalidateStaticFrameCache();
}

bool SceneView::IsToolbarInteractionActive() const
{
    return m_toolbarPopupOpen;
}

void SceneView::OnBeforeDrawWidgets()
{
    m_distanceFieldDebug.forceRebuild = false;
    DrawSceneToolbar();
    // Toolbar edits must reach this frame before its camera and descriptor freeze.
    ApplyViewSettings();
    if (auto* renderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(m_renderer.get()))
        renderer->SetDistanceFieldDebugSettings(m_distanceFieldDebug);
    AView::OnBeforeDrawWidgets();
}

void SceneView::DrawSceneToolbar()
{
    const float scale = NLS_SERVICE(UI::UIManager).GetScale();
    const float innerSpacing = 2.0f * scale;
    const float groupSpacing = 12.0f * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * scale, 3.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(innerSpacing, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * scale);
    const float buttonHeight = std::max(ImGui::GetFrameHeight(), 22.0f * scale);
    const float height = buttonHeight + 8.0f * scale;
    ImGui::BeginChild("##SceneToolbar", ImVec2(0, height), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorPos(ImVec2(6.0f * scale, 4.0f * scale));
    const float iconWidth = 24.0f * scale;
    const float arrowWidth = 16.0f * scale;
    const float splitWidth = iconWidth + arrowWidth;
    const std::array<float, 7> widths{
        4 * iconWidth + 3 * innerSpacing, 2 * iconWidth + innerSpacing,
        2 * splitWidth + innerSpacing, splitWidth + iconWidth + innerSpacing,
        iconWidth + splitWidth + innerSpacing, splitWidth + iconWidth + innerSpacing, splitWidth};
    const float available = std::max(0.0f, ImGui::GetContentRegionAvail().x - 6.0f * scale);
    const auto visible = Core::GetVisibleSceneToolbarGroupCount(widths, available, iconWidth, groupSpacing);
#if defined(NLS_ENABLE_TEST_HOOKS)
    const auto& diagnostics = EDITOR_EXEC(GetContext()).GetDiagnosticsSettings();
    const bool validationCapture = !diagnostics.editorValidationSceneReadbackOutput.empty();
    static bool validationActionApplied = false;
    if (validationCapture && !validationActionApplied)
    {
        bool applied = true;
        if (const auto* action = std::getenv("NLS_EDITOR_TOOLBAR_VALIDATION_ACTION"))
        {
            if (std::string_view(action) == "2d") Set2D(true);
            else if (std::string_view(action) == "build-distance-fields")
            {
                SetDrawMode(DrawMode::MeshDistanceField);
                applied = BuildSceneDistanceFields();
            }
            else if (std::string_view(action) == "mesh-distance-fields") SetDrawMode(DrawMode::MeshDistanceField);
            else if (std::string_view(action) == "global-distance-fields") SetDrawMode(DrawMode::GlobalDistanceField);
            else if (std::string_view(action) == "hide-selected")
            {
                if (EDITOR_EXEC(IsAnyGameObjectSelected()))
                    for (auto* selected : EDITOR_EXEC(GetSelectedGameObjects())) SetObjectVisibility(*selected, false);
                else applied = false;
            }
            else if (std::string_view(action) == "isolate-selected")
            {
                if (EDITOR_EXEC(IsAnyGameObjectSelected())) IsolateSelection();
                else applied = false;
            }
        }
        validationActionApplied = applied;
    }
    std::ostringstream layout;
    if (validationCapture) layout << "scale=" << scale << "\navailable=" << available << "\nvisibleGroups=" << visible
        << "\nvalidationActionApplied=" << validationActionApplied << "\nis2D=" << m_is2D
        << "\nhasHiddenObjects=" << m_sceneVisibility.HasHiddenObjects() << "\n";
#endif
    float total = 0;
    for (size_t group = 0; group < visible; ++group) total += widths[group] + (group ? groupSpacing : 0);
    if (visible < widths.size()) total += (visible ? groupSpacing : 0) + iconWidth;
    // Editing tools stay left; view options form a separate block at the right.
    const float stretch = visible == widths.size() ? std::max(0.0f, available - total) : 0.0f;
    for (size_t group = 0; group < visible; ++group)
    {
        if (group) ImGui::SameLine(0, groupSpacing + (group == 3 ? stretch : 0));
        auto start = ImGui::GetCursorScreenPos();
        const auto background = ImGui::GetColorU32(ImGuiCol_FrameBg, 0.48f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start.x - 2 * scale, start.y - scale),
            ImVec2(start.x + widths[group] + 2 * scale, start.y + buttonHeight + scale), background, 4 * scale);
        ImGui::BeginGroup();
        DrawSceneToolbarGroup(group, false);
        ImGui::EndGroup();
#if defined(NLS_ENABLE_TEST_HOOKS)
        if (validationCapture)
        {
            const auto end = ImGui::GetItemRectMax();
            layout << "group" << group << "=" << start.x << "," << start.y << "," << end.x << "," << end.y << "\n";
        }
#endif
        if (group && group != 3)
            ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x - 6 * scale, start.y + 4 * scale),
                ImVec2(start.x - 6 * scale, start.y + buttonHeight - 4 * scale), ImGui::GetColorU32(ImGuiCol_Separator));
    }
    if (visible < widths.size())
    {
        if (visible) ImGui::SameLine(0, groupSpacing);
        Icons::IconButtonOptions options;
        options.tooltip = "More Scene tools";
        if (EDITOR_CONTEXT(editorResources)->GetIconPainter().Button("##SceneToolbarMore",
            Icons::EditorIcons::GlyphMore, Icons::IconToken::Small, options))
            ImGui::OpenPopup("##SceneToolbarOverflow");
        if (ImGui::BeginPopup("##SceneToolbarOverflow"))
        {
            for (size_t group = visible; group < widths.size(); ++group)
            {
                ImGui::PushID(static_cast<int>(group));
                if (ImGui::BeginMenu(kGroupNames[group]))
                {
                    DrawSceneToolbarGroup(group, true);
                    ImGui::EndMenu();
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }
    m_toolbarPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (validationCapture)
    {
        layout << "popupBlocksSceneInput=" << m_toolbarPopupOpen << "\n";
        std::ofstream(diagnostics.editorValidationSceneReadbackOutput + ".toolbar.txt") << layout.str();
    }
#endif
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    if (m_toolbarSettingsDirty)
    {
        m_hasPickingSample = false;
        m_pendingClickPickRenderPos.reset();
        m_pendingClickPickingSignature.reset();
        InvalidateStaticFrameCache();
    }
    if (m_toolbarSettingsDirty && !ImGui::IsAnyItemActive()) SaveSceneToolbarSettings();
}

void SceneView::DrawSceneToolbarGroup(const size_t group, const bool menu)
{
    auto& painter = EDITOR_CONTEXT(editorResources)->GetIconPainter();
    auto& settings = Settings::EditorSettings::GetSceneViewSettingsObject();
    using namespace Icons::EditorIcons;
    const auto iconButton = [&](const char* id, Icons::IconId icon, const char* tooltip, bool selected = false)
    {
        Icons::IconButtonOptions options; options.tooltip = tooltip; options.selected = selected;
        return painter.Button(id, icon, Icons::IconToken::Small, options);
    };
    const auto dropdown = [&](const char* id, const char* tooltip, const char* popup)
    {
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetStyle().FramePadding.y));
        const bool clicked = iconButton(id, GlyphCaretDown, tooltip);
        ImGui::PopStyleVar();
        if (clicked) ImGui::OpenPopup(popup);
        const float scale = NLS_SERVICE(UI::UIManager).GetScale();
        ImGui::SetNextWindowSizeConstraints(ImVec2(300 * scale, 0), ImVec2(420 * scale, 620 * scale));
        return ImGui::BeginPopup(popup);
    };
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (!menu && !EDITOR_EXEC(GetContext()).GetDiagnosticsSettings().editorValidationSceneReadbackOutput.empty())
        if (const auto* requested = std::getenv("NLS_EDITOR_TOOLBAR_VALIDATION_POPUP"))
        {
            const std::string_view name(requested);
            const char* popup = nullptr;
            if (group == 3 && name == "draw") popup = "##SceneDrawMode";
            if (group == 4 && name == "effects") popup = "##SceneEffectsMenu";
            if (group == 5 && name == "visibility") popup = "##SceneVisibilityMenu";
            if (group == 5 && name == "camera") popup = "##SceneCameraMenu";
            if (group == 6 && name == "gizmos") popup = "##SceneGizmosMenu";
            if (popup && !ImGui::IsPopupOpen(popup)) ImGui::OpenPopup(popup);
        }
#endif
    switch (group)
    {
    case 0:
    {
        const std::array operations{Core::EGizmoOperation::TRANSLATE, Core::EGizmoOperation::ROTATE,
            Core::EGizmoOperation::SCALE, Core::EGizmoOperation::RECT};
        const std::array icons{ToolbarMove, ToolbarRotate, ToolbarScale, ToolbarRect};
        constexpr std::array labels{"Move", "Rotate", "Scale", "Rect"};
        constexpr std::array commands{"scene-view.move-tool", "scene-view.rotate-tool", "scene-view.scale-tool", "scene-view.rect-tool"};
        for (size_t i = 0; i < operations.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            const auto binding = NLS_SERVICE(Shortcuts::EditorShortcutService).GetBindingDisplayText(commands[i]);
            const auto tooltip = std::string(labels[i]) + " (" + binding + ")";
            if (menu ? ImGui::MenuItem(labels[i], binding.c_str(), m_currentOperation == operations[i]) :
                iconButton("##SceneTool", icons[i], tooltip.c_str(), m_currentOperation == operations[i]))
                Shortcuts::InvokeEditorCommand(commands[i], menu ? Shortcuts::CommandSource::Menu : Shortcuts::CommandSource::Toolbar, "scene-view");
            ImGui::PopID();
            if (!menu && i + 1 < operations.size()) ImGui::SameLine();
        }
        break;
    }
    case 1:
    {
        const bool center = m_currentPivot == Core::SceneViewGizmoPivot::Center;
        const bool local = m_currentSpace == Core::SceneViewGizmoSpace::Local;
        const auto pivotBinding = NLS_SERVICE(Shortcuts::EditorShortcutService).GetBindingDisplayText("scene-view.toggle-pivot-position");
        const auto spaceBinding = NLS_SERVICE(Shortcuts::EditorShortcutService).GetBindingDisplayText("scene-view.toggle-pivot-orientation");
        const auto pivotTip = std::string(center ? "Center" : "Pivot") + " (" + pivotBinding + ")";
        const auto spaceTip = std::string(local ? "Local" : "Global") + " (" + spaceBinding + ")";
        if (menu ? ImGui::MenuItem(center ? "Center" : "Pivot", pivotBinding.c_str()) :
            iconButton("##ScenePivot", center ? ToolbarCenter : ToolbarPivot,
                pivotTip.c_str())) Shortcuts::InvokeEditorCommand("scene-view.toggle-pivot-position", menu ? Shortcuts::CommandSource::Menu : Shortcuts::CommandSource::Toolbar, "scene-view");
        if (!menu) ImGui::SameLine();
        ImGui::BeginDisabled(m_currentOperation == Core::EGizmoOperation::RECT);
        if (menu ? ImGui::MenuItem(local ? "Local" : "Global", spaceBinding.c_str()) :
            iconButton("##SceneSpace", local ? ToolbarLocal : ToolbarGlobal,
                spaceTip.c_str())) Shortcuts::InvokeEditorCommand("scene-view.toggle-pivot-orientation", menu ? Shortcuts::CommandSource::Menu : Shortcuts::CommandSource::Toolbar, "scene-view");
        ImGui::EndDisabled();
        if (m_currentOperation == Core::EGizmoOperation::RECT && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Rect uses local object axes. Your previous space is restored when switching tools.");
        break;
    }
    case 2:
        if (menu) DrawSceneGridSnapMenu();
        else
        {
            if (iconButton("##SceneGrid", ToolbarGrid, "Show grid", settings.showGrid))
            { settings.showGrid = !settings.showGrid; m_toolbarSettingsDirty = true; }
            if (dropdown("##SceneGridArrow", "Grid settings", "##SceneGridSettings")) { DrawSceneGridMenu(); ImGui::EndPopup(); }
            ImGui::SameLine();
            if (iconButton("##SceneSnap", ToolbarSnap, "Always snap. Hold Ctrl for temporary snapping.", settings.snapEnabled))
            { settings.snapEnabled = !settings.snapEnabled; m_toolbarSettingsDirty = true; }
            if (dropdown("##SceneSnapArrow", "Snap settings", "##SceneSnapSettings")) { DrawSceneSnapMenu(); ImGui::EndPopup(); }
        }
        break;
    case 3:
        if (menu)
        {
            if (ImGui::BeginMenu("Draw Mode")) { DrawSceneDrawModeMenu(); ImGui::EndMenu(); }
            if (ImGui::MenuItem("2D", nullptr, m_is2D)) Set2D(!m_is2D);
        }
        else
        {
            const auto mode = GetToolbarState().drawMode;
            const auto icon = mode == DrawMode::Shaded ? ToolbarShaded : mode == DrawMode::Wireframe ? ToolbarWireframe :
                mode == DrawMode::ShadedWireframe ? ToolbarShadedWireframe : ToolbarBuffer;
            const std::string tooltip = std::string("Draw Mode: ") + DrawModeLabel(mode);
            if (iconButton("##SceneDrawModeButton", icon, tooltip.c_str(), mode != DrawMode::Shaded)) ImGui::OpenPopup("##SceneDrawMode");
            if (dropdown("##SceneDrawModeArrow", "Draw mode menu", "##SceneDrawMode")) { DrawSceneDrawModeMenu(); ImGui::EndPopup(); }
            ImGui::SameLine();
            if (iconButton("##Scene2D", Toolbar2d, "2D: edit on the XY plane", m_is2D)) Set2D(!m_is2D);
        }
        break;
    case 4:
        if (menu)
        {
            if (ImGui::MenuItem("Scene Lighting", nullptr, settings.sceneLighting)) { settings.sceneLighting = !settings.sceneLighting; m_toolbarSettingsDirty = true; }
            if (ImGui::BeginMenu("Effects")) { DrawSceneEffectsMenu(); ImGui::EndMenu(); }
        }
        else
        {
            if (iconButton("##SceneLighting", SceneLightDirectional, "Scene Lighting / Editor Preview Lighting", settings.sceneLighting))
            { settings.sceneLighting = !settings.sceneLighting; m_toolbarSettingsDirty = true; }
            ImGui::SameLine();
            if (iconButton("##SceneEffects", ToolbarEffects, "Toggle particles and post processing", settings.effectsEnabled))
            { settings.effectsEnabled = !settings.effectsEnabled; m_toolbarSettingsDirty = true; }
            if (dropdown("##SceneEffectsArrow", "Effects menu", "##SceneEffectsMenu")) { DrawSceneEffectsMenu(); ImGui::EndPopup(); }
        }
        break;
    case 5:
        if (menu)
        {
            if (ImGui::BeginMenu("Scene Visibility")) { DrawSceneVisibilityMenu(); ImGui::EndMenu(); }
            if (ImGui::BeginMenu("Camera Settings")) { DrawSceneCameraMenu(); ImGui::EndMenu(); }
        }
        else
        {
            const bool hidden = settings.sceneVisibility && m_sceneVisibility.HasHiddenObjects();
            if (iconButton("##SceneVisibility", hidden ? ToolbarVisibilityOff : ToolbarVisibility,
                "Scene Visibility: apply or ignore hidden objects", settings.sceneVisibility))
            { settings.sceneVisibility = !settings.sceneVisibility; m_toolbarSettingsDirty = true; }
            if (dropdown("##SceneVisibilityArrow", "Scene visibility menu", "##SceneVisibilityMenu")) { DrawSceneVisibilityMenu(); ImGui::EndPopup(); }
            ImGui::SameLine();
            if (iconButton("##SceneCamera", ToolbarCamera, "Scene camera settings")) ImGui::OpenPopup("##SceneCameraMenu");
            const float scale = NLS_SERVICE(UI::UIManager).GetScale();
            ImGui::SetNextWindowSizeConstraints(ImVec2(330 * scale, 0), ImVec2(430 * scale, 640 * scale));
            if (ImGui::BeginPopup("##SceneCameraMenu")) { DrawSceneCameraMenu(); ImGui::EndPopup(); }
        }
        break;
    case 6:
        if (menu) DrawSceneGizmosMenu();
        else
        {
            if (iconButton("##SceneGizmos", ToolbarGizmos, "Toggle component gizmos and icons", settings.showGizmos))
            { settings.showGizmos = !settings.showGizmos; m_toolbarSettingsDirty = true; }
            if (dropdown("##SceneGizmosArrow", "Gizmos menu", "##SceneGizmosMenu")) { DrawSceneGizmosMenu(); ImGui::EndPopup(); }
        }
        break;
    }
}

void SceneView::DrawSceneGridMenu()
{
    auto& scene = Settings::EditorSettings::GetSceneViewSettingsObject();
    auto& view = Settings::EditorSettings::GetViewSettingsObject();
    ImGui::SeparatorText("Grid");
    bool changed = ImGui::Checkbox("Show Grid", &scene.showGrid);
    changed = ImGui::ColorEdit3("Grid Color", &view.sceneViewGrid.r) || changed;
    ImGui::TextDisabled("Plane: %s", m_is2D ? "XY" : "XZ");
    m_toolbarSettingsDirty |= changed;
}

void SceneView::DrawSceneSnapMenu()
{
    auto& scene = Settings::EditorSettings::GetSceneViewSettingsObject();
    auto& snap = Settings::EditorSettings::GetSceneToolSettingsObject();
    ImGui::SeparatorText("Increment Snapping");
    bool changed = ImGui::Checkbox("Always Snap", &scene.snapEnabled);
    ImGui::TextDisabled("Ctrl temporarily enables snapping.");
    changed = ImGui::DragFloat("Move", &snap.translationSnapUnit, 0.05f, 0.001f, 10000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp) || changed;
    changed = ImGui::DragFloat("Rotate", &snap.rotationSnapUnit, 1.0f, 0.001f, 360.0f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp) || changed;
    changed = ImGui::DragFloat("Scale", &snap.scalingSnapUnit, 0.01f, 0.001f, 1000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp) || changed;
    m_toolbarSettingsDirty |= changed;
}

void SceneView::DrawSceneGridSnapMenu() { DrawSceneGridMenu(); DrawSceneSnapMenu(); }

void SceneView::DrawSceneDrawModeMenu()
{
    ImGui::SeparatorText("Draw Mode");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    if (ImGui::InputTextWithHint("##DrawModeSearch", "Search draw modes...", m_drawModeFilter.InputBuf,
        IM_ARRAYSIZE(m_drawModeFilter.InputBuf))) m_drawModeFilter.Build();
    const char* previousCategory = nullptr;
    for (const auto& entry : Core::GetSceneViewDrawModes())
    {
        const auto searchText = std::string(entry.category) + " " + entry.label;
        if (!m_drawModeFilter.PassFilter(searchText.c_str())) continue;
        if (!m_drawModeFilter.IsActive() && (!previousCategory || std::string_view(previousCategory) != entry.category))
            ImGui::SeparatorText(entry.category);
        previousCategory = entry.category;
        if (ImGui::MenuItem(entry.label, nullptr, GetToolbarState().drawMode == entry.mode)) SetDrawMode(entry.mode);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry.legend);
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", Core::GetSceneViewDrawModeInfo(GetToolbarState().drawMode).legend);
    bool changed = false;
    if (GetToolbarState().drawMode == DrawMode::Overdraw)
        changed = ImGui::SliderFloat("Maximum layers", &m_drawModeParameters.overdrawMax, 1, 64, "%.0f");
    if (GetToolbarState().drawMode == DrawMode::ValidateAlbedo || GetToolbarState().drawMode == DrawMode::ValidateMetalSpecular)
    {
        auto& range = GetToolbarState().drawMode == DrawMode::ValidateAlbedo ? m_drawModeParameters.albedoRange : m_drawModeParameters.specularRange;
        changed |= ImGui::DragFloatRange2("Dielectric sRGB", &range.x, &range.y, 1, 0, 255, "%.0f", "%.0f");
        changed |= ImGui::DragFloatRange2("Metal sRGB", &range.z, &range.w, 1, 0, 255, "%.0f", "%.0f");
    }
    if (GetToolbarState().drawMode == DrawMode::Overdraw || GetToolbarState().drawMode == DrawMode::ValidateAlbedo || GetToolbarState().drawMode == DrawMode::ValidateMetalSpecular)
        if (ImGui::Button("Reset diagnostic settings")) { m_drawModeParameters = {}; changed = true; }
    if (changed) InvalidateStaticFrameCache();
    if (!m_drawModeFilter.IsActive() && ImGui::BeginMenu("Distance Field Settings")) { DrawDistanceFieldControls(); ImGui::EndMenu(); }
    if (!m_drawModeFilter.IsActive() && ImGui::BeginMenu("Pipeline Capabilities"))
    {
        ImGui::TextDisabled("Unavailable in the current render pipeline:");
        UnavailableItem("Shadow Cascades", "The renderer does not yet provide cascaded shadow debug data.");
        UnavailableItem("Texture Streaming", "This pipeline has no per-texture mip residency system.");
        UnavailableItem("Sprite Mask", "This pipeline has no Sprite Mask component.");
        UnavailableItem("Global Illumination / Lightmaps", "This pipeline does not provide GI or baked-lightmap buffers.");
        ImGui::EndMenu();
    }
}

void SceneView::DrawSceneCameraMenu()
{
    auto& scene = Settings::EditorSettings::GetSceneViewSettingsObject();
    auto& view = Settings::EditorSettings::GetViewSettingsObject();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Scene Camera");
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 24 * NLS_SERVICE(UI::UIManager).GetScale()));
    Icons::IconButtonOptions resetOptions; resetOptions.tooltip = "Camera settings";
    if (EDITOR_CONTEXT(editorResources)->GetIconPainter().Button("##CameraSettings",
        Icons::EditorIcons::GlyphMore, Icons::IconToken::Small, resetOptions)) ImGui::OpenPopup("##CameraResetMenu");
    if (ImGui::BeginPopup("##CameraResetMenu"))
    {
        if (ImGui::MenuItem("Reset"))
        {
            const Settings::EditorSceneViewSettingsObject defaults;
            scene.cameraFov = defaults.cameraFov; scene.cameraNear = defaults.cameraNear; scene.cameraFar = defaults.cameraFar;
            scene.cameraDynamicClipping = defaults.cameraDynamicClipping; scene.cameraOcclusionCulling = defaults.cameraOcclusionCulling;
            scene.cameraEasing = defaults.cameraEasing; scene.cameraAcceleration = defaults.cameraAcceleration;
            scene.cameraMinSpeed = defaults.cameraMinSpeed; scene.cameraMaxSpeed = defaults.cameraMaxSpeed;
            view.sceneViewCameraSpeed = Settings::EditorViewSettingsObject{}.sceneViewCameraSpeed;
            m_toolbarSettingsDirty = true;
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::BeginDisabled(m_is2D);
    int projection = m_camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC ? 1 : 0;
    if (ImGui::Combo("Projection", &projection, "Perspective\0Orthographic\0"))
    {
        m_cameraController.ResetMouseInteractionState();
        m_camera.SetProjectionMode(projection ? Render::Settings::EProjectionMode::ORTHOGRAPHIC : Render::Settings::EProjectionMode::PERSPECTIVE);
        InvalidateStaticFrameCache();
    }
    ImGui::EndDisabled();
    bool changed = false;
    if (projection == 0)
        changed |= ImGui::SliderFloat("Field of View", &scene.cameraFov, 1.0f, 179.0f, "%.1f deg");
    else
    {
        float size = m_camera.GetSize();
        if (ImGui::DragFloat("Orthographic Size", &size, 0.05f, 0.01f, 100000.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
        { m_camera.SetSize(size); InvalidateStaticFrameCache(); }
    }
    changed |= ImGui::Checkbox("Dynamic Clipping", &scene.cameraDynamicClipping);
    ImGui::BeginDisabled(scene.cameraDynamicClipping);
    changed |= ImGui::DragFloat("Near", &scene.cameraNear, 0.01f, 0.001f, std::max(0.001f, scene.cameraFar - 0.01f), "%.3f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::DragFloat("Far", &scene.cameraFar, 1.0f, scene.cameraNear + 0.01f, 1000000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    if (scene.cameraDynamicClipping) ImGui::TextDisabled("Effective planes: %.3f / %.1f", m_camera.GetNear(), m_camera.GetFar());
    changed |= ImGui::Checkbox("Occlusion Culling", &scene.cameraOcclusionCulling);
    ImGui::SeparatorText("Navigation");
    changed |= ImGui::Checkbox("Camera Easing", &scene.cameraEasing);
    changed |= ImGui::Checkbox("Camera Acceleration", &scene.cameraAcceleration);
    changed |= ImGui::SliderFloat("Camera Speed", &view.sceneViewCameraSpeed, scene.cameraMinSpeed, scene.cameraMaxSpeed, "%.2f");
    changed |= ImGui::DragFloat("Min", &scene.cameraMinSpeed, 0.01f, 0.01f, std::min(98.0f, scene.cameraMaxSpeed - 0.01f), "%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::DragFloat("Max", &scene.cameraMaxSpeed, 0.01f, scene.cameraMinSpeed + 0.01f, 99.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SeparatorText("Background");
    changed |= ImGui::ColorEdit3("Color", &view.sceneViewBackground.r);
    if (ImGui::BeginMenu("Advanced"))
    {
        auto& rendering = Settings::EditorSettings::GetRenderingSettingsObject();
        changed |= ImGui::Checkbox("Static Frame Cache", &rendering.enableSceneViewStaticFrameCache);
        if(auto* renderer=dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(m_renderer.get()))
        {
            ImGui::SeparatorText("Sprite Rendering");
            bool cpu=renderer->GetSpriteCpuBatching();
            if(ImGui::Checkbox("CPU Sprite Batches",&cpu)){renderer->SetSpriteCpuBatching(cpu);InvalidateStaticFrameCache();}
            const auto stats=renderer->GetSpriteStatistics();
            ImGui::Text("Submitted: %u  Visible: %u  Culled: %u",stats.submitted,stats.visible,stats.culled);
            ImGui::Text("Batches: %u  Draw Calls: %u",stats.batches,stats.drawCalls);
            ImGui::Text("Material/shader breaks: %u",stats.materialBreaks);
            ImGui::Text("Texture breaks: %u  State breaks: %u",stats.textureBreaks,stats.stateBreaks);
            ImGui::Text("Ordering breaks: %u  Capacity breaks: %u",stats.orderingBreaks,stats.capacityBreaks);
        }
        ImGui::EndMenu();
    }
    m_toolbarSettingsDirty |= changed;
}

void SceneView::DrawSceneGizmosMenu()
{
    auto& scene = Settings::EditorSettings::GetSceneViewSettingsObject();
    ImGui::SeparatorText("Gizmos");
    bool changed = ImGui::Checkbox("Show Gizmos", &scene.showGizmos);
    changed |= ImGui::Checkbox("3D Icons", &scene.icons3D);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * NLS_SERVICE(UI::UIManager).GetScale());
    changed |= ImGui::SliderFloat("##IconSize", &scene.lightBillboardScale, 0.01f, 5.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Component icon size");
    changed |= ImGui::Checkbox("Fade Gizmos", &scene.fadeGizmos);
    changed |= ImGui::Checkbox("Selection Outline", &scene.showSelectionOutline);
    changed |= ImGui::Checkbox("Selection Wire", &scene.showSelectionWire);
    UnavailableItem("Light Probe Visualization", "This pipeline has no light-probe data to visualize.");
    ImGui::SeparatorText("Built-in Components");
    m_gizmosFilter.Draw("Search##Gizmos", -1);
    if (ImGui::BeginTable("##ComponentGizmos", 3, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Component", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Gizmo", ImGuiTableColumnFlags_WidthFixed, 48 * NLS_SERVICE(UI::UIManager).GetScale());
        ImGui::TableSetupColumn("Icon", ImGuiTableColumnFlags_WidthFixed, 40 * NLS_SERVICE(UI::UIManager).GetScale());
        ImGui::TableHeadersRow();
        const auto row = [&](const char* name, bool& gizmo, bool* icon)
        {
            if (!m_gizmosFilter.PassFilter(name)) return;
            ImGui::PushID(name); ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
            ImGui::TableNextColumn(); changed |= ImGui::Checkbox("##Gizmo", &gizmo);
            ImGui::TableNextColumn(); if (icon) changed |= ImGui::Checkbox("##Icon", icon); else ImGui::TextDisabled("-");
            ImGui::PopID();
        };
        row("Camera", scene.showCameras, &scene.showCameraIcons);
        row("Light", scene.showLights, &scene.showLightIcons);
        row("Mesh Bounds", scene.showBounds, nullptr);
        ImGui::EndTable();
    }
    if (ImGui::Button("All"))
    { scene.showCameras = scene.showLights = scene.showBounds = scene.showCameraIcons = scene.showLightIcons = true; changed = true; }
    ImGui::SameLine();
    if (ImGui::Button("None"))
    { scene.showCameras = scene.showLights = scene.showBounds = scene.showCameraIcons = scene.showLightIcons = false; changed = true; }
    m_toolbarSettingsDirty |= changed;
}

void SceneView::DrawSceneEffectsMenu()
{
    auto& scene = Settings::EditorSettings::GetSceneViewSettingsObject();
    ImGui::SeparatorText("Background");
    bool changed = ImGui::Checkbox("Skybox", &scene.showSkybox);
    ImGui::SeparatorText("Effects");
    changed |= ImGui::Checkbox("Enable Selected Effects", &scene.effectsEnabled);
    ImGui::Separator();
    UnavailableItem("Clouds", "This render pipeline has no volumetric cloud renderer.");
    UnavailableItem("Fog", "This render pipeline has no scene fog renderer.");
    UnavailableItem("Flares", "This render pipeline does not support lens flare components.");
    changed |= ImGui::Checkbox("Always Refresh", &scene.alwaysRefresh);
    changed |= ImGui::Checkbox("Post Processing", &scene.postProcessing);
    changed |= ImGui::Checkbox("Particle Systems", &scene.showParticles);
    m_toolbarSettingsDirty |= changed;
}

void SceneView::SetObjectVisibility(Engine::GameObject& object, bool visible, bool includeChildren)
{
    m_sceneVisibility.BindScene(GetScene(), GetScene() ? GetScene()->GetInstanceID() : 0);
    std::vector<int32_t> ids;
    CollectObjectIds(object, ids, includeChildren);
    m_sceneVisibility.SetHidden(ids, !visible);
    m_hasPickingSample = false;
    m_pendingClickPickRenderPos.reset(); m_pendingClickPickingSignature.reset();
    InvalidateStaticFrameCache();
}

bool SceneView::IsObjectHidden(const Engine::GameObject& object) const
{
    return m_sceneVisibility.IsHidden(object.GetInstanceID());
}

void SceneView::IsolateSelection()
{
    const auto selected = EDITOR_EXEC(GetSelectedGameObjects());
    if (!selected.empty())
    {
        m_sceneVisibility.BindScene(GetScene(), GetScene() ? GetScene()->GetInstanceID() : 0);
        std::vector<int32_t> ids;
        for (auto* object : selected) CollectObjectIds(*object, ids);
        std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        m_sceneVisibility.Isolate(ids);
        InvalidateStaticFrameCache();
    }
}

void SceneView::ShowAllObjects()
{
    m_sceneVisibility.ShowAll();
    m_hasPickingSample = false;
    InvalidateStaticFrameCache();
}

void SceneView::DrawSceneVisibilityMenu()
{
    auto& scene = Settings::EditorSettings::GetSceneViewSettingsObject();
    ImGui::SeparatorText("Scene Visibility");
    m_toolbarSettingsDirty |= ImGui::Checkbox("Apply Scene Visibility", &scene.sceneVisibility);
    ImGui::TextDisabled("Hidden objects remain active in Game view.");
    const auto selected = EDITOR_EXEC(GetSelectedGameObjects());
    ImGui::BeginDisabled(selected.empty());
    if (ImGui::MenuItem("Hide Selected", "H")) for (auto* object : selected) SetObjectVisibility(*object, false);
    if (ImGui::MenuItem("Show Selected", "H")) for (auto* object : selected) SetObjectVisibility(*object, true);
    if (ImGui::MenuItem("Isolate Selected", nullptr, m_sceneVisibility.IsIsolating())) IsolateSelection();
    ImGui::EndDisabled();
    if (ImGui::MenuItem("Exit Isolation", nullptr, false, m_sceneVisibility.IsIsolating()))
    { m_sceneVisibility.ExitIsolation(); InvalidateStaticFrameCache(); }
    ImGui::Separator();
    if (ImGui::MenuItem("Show All", nullptr, false, m_sceneVisibility.HasHiddenObjects())) ShowAllObjects();
}

void SceneView::SaveSceneToolbarSettings()
{
    Settings::EditorSettingsRegistry registry;
    Settings::EditorSettings::RegisterSettingObjects(registry);
    const auto path = std::filesystem::path(EDITOR_CONTEXT(projectPath)) / "UserSettings" / "editor-settings.json";
    if (Settings::EditorSettingsPersistence::Save(path, registry))
        m_toolbarSettingsDirty = false;
    else
        NLS_LOG_ERROR("Could not save Scene View preferences: " + path.string());
}
}
