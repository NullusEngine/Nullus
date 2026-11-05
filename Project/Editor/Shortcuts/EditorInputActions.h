#pragma once
#include <Input/InputAction.h>
#include <array>
#include <string_view>

namespace NLS::Editor::Shortcuts
{
// Editor operations share the existing InputUser/Profile. Physical defaults
// live in the asset; discrete operations can also invoke scoped Commands.
enum class EditorInputAction : uint8_t
{
    CameraFly, CameraPan, CameraOrbit, CameraBoost, CameraLook, CameraZoom,
    SceneSnap, SceneCancel, SceneSuspendSelection, CanvasPan, CanvasZoom, CanvasScrollX, CanvasScrollY,
    CanvasSnapBypass, CanvasConstrain, SceneToggleSelection, SceneAddSelection,
    CanvasToggleSelection, CanvasAddSelection, VfxTimelineZoom, Count
};
inline constexpr std::array<std::string_view, static_cast<size_t>(EditorInputAction::Count)> EditorInputActionIds {
    "camera-navigation.fly", "camera-navigation.pan", "camera-navigation.orbit",
    "camera-navigation.boost", "camera-navigation.look", "camera-navigation.zoom",
    "scene-view.snap", "scene-view.cancel-gesture", "scene-view.suspend-selection", "ui-designer.pan",
    "ui-designer.wheel-zoom", "ui-designer.scroll-x", "ui-designer.scroll-y",
    "ui-designer.snap-bypass", "ui-designer.constrain", "scene-view.toggle-selection",
    "scene-view.add-selection", "ui-designer.toggle-selection", "ui-designer.add-selection", "vfx-editor.timeline-zoom"
};

Input::ActionSnapshot GetEditorInputState(EditorInputAction action);
}
