#include "Core/Editor.h"
#include "Panels/AssetBrowser.h"
#include "Panels/SceneView.h"
#include "Panels/Console.h"
#include "Panels/UIDocumentDesigner.h"
#include "Panels/VfxEditor.h"
#include "Panels/VfxLibraryEditor.h"
#include "Panels/InputAssetEditor.h"
#include "Panels/SpriteEditor.h"
#include "Core/CameraController.h"
#include "Panels/Inspector.h"

namespace NLS::Editor::Core
{
void Editor::RegisterPanelCommands()
{
    using namespace Shortcuts;
    auto scope = [this](std::string id, std::function<bool()> active)
    { m_shortcutService.RegisterContext({id, id, 50, "focused-panel", std::move(active)}); };
    auto add = [this](std::string area, std::string local, std::function<bool()> invoke, std::function<bool()> canExecute = {})
    {
        ShortcutCommand command;
        command.id = area + "." + local; command.context = area; command.category = area; command.displayName = local;
        command.availability = std::move(canExecute);
        command.invoke = [invoke = std::move(invoke)](const InvocationContext&)
        { return invoke() ? CommandResult::Completed : CommandResult::Disabled; };
        m_shortcutService.RegisterCommand(std::move(command));
    };
    // Mappings, alternate gestures and repeat interactions live exclusively in
    // EditorShortcuts.inputactions. Registration only describes business calls.
    auto& assets = m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser");
    auto& scene = m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View");
    m_shortcutService.RegisterContext({"scene-input-gesture", "Scene Gesture", 160, "", [&scene] { return scene.HasActiveInputGesture(); }});
    ShortcutCommand cancelScene;
    cancelScene.id = "scene-view.cancel-gesture"; cancelScene.displayName = "Cancel Scene Gesture";
    cancelScene.category = "Scene View"; cancelScene.context = "scene-input-gesture";
    cancelScene.availability = [&scene] { return scene.HasActiveInputGesture(); };
    cancelScene.execute = [&scene] { scene.CancelInputGesture(); };
    m_shortcutService.RegisterCommand(std::move(cancelScene));
    auto& inspector = m_panelsManager.GetPanelAs<Panels::Inspector>("Inspector");
    scope("inspector", [&inspector] { return inspector.HasCurrentKeyboardFocus(); });
    for (const std::string id : {"copy", "paste", "remove", "reset", "up", "down"})
    {
        ShortcutCommand command; command.id = "inspector.component-" + id; command.context = "inspector";
        command.category = "Inspector"; command.displayName = "Component " + id;
        command.canExecute = [&inspector, id](const InvocationContext& context) { return inspector.CanExecuteComponentCommand(id, context.targetInstance); };
        command.invoke = [&inspector, id](const InvocationContext& context) { return inspector.ExecuteComponentCommand(id, context.targetInstance) ? CommandResult::Completed : CommandResult::Failed; };
        m_shortcutService.RegisterCommand(std::move(command));
    }
    m_shortcutService.RegisterContext({"camera-angle", "Viewport Camera", 60, "", [] { return CameraController::GetKeyboardInputTarget() != nullptr; }});
    scope("camera-preview", [] { auto* camera = CameraController::GetKeyboardInputTarget(); return camera && !camera->IsSceneViewCamera(); });
    add("camera-preview", "frame", [] { auto* camera = CameraController::GetKeyboardInputTarget(); return camera && camera->FocusInputTarget(); });
    for (const auto& [id, angle] : std::vector<std::pair<std::string, Maths::Vector3>>{
        {"top", Maths::Vector3::Up}, {"bottom", -Maths::Vector3::Up},
        {"right", Maths::Vector3::Right}, {"left", -Maths::Vector3::Right},
        {"front", Maths::Vector3::Forward}, {"back", -Maths::Vector3::Forward}})
        add("camera-angle", id, [angle] { auto* camera = CameraController::GetKeyboardInputTarget(); return camera && camera->FocusInputTarget(angle); });
    auto& inputEditor = m_panelsManager.GetPanelAs<Panels::InputAssetEditor>("Input Editor");
    scope("input-editor", [&inputEditor] { return inputEditor.HasCurrentKeyboardFocus(); });
    add("input-editor", "save", [&inputEditor] { return inputEditor.Save(); });
    auto& spriteEditor = m_panelsManager.GetPanelAs<Panels::SpriteEditor>("Sprite Editor");
    scope("sprite-editor", [&spriteEditor] { return spriteEditor.OwnsShortcuts(); });
    for (const std::string id : {"save", "undo", "redo", "delete", "revert", "cancel", "frame-all"})
        add("sprite-editor", id, [&spriteEditor,id] { return spriteEditor.ExecuteCommand(id); });
    scope("asset-browser", [&assets] { return assets.HasCurrentKeyboardFocus(); });
    for (const std::string id : {"rename", "copy", "paste", "duplicate", "delete"})
    {
        add("asset-browser", id, [&assets, id] { return assets.ExecuteCommand(id); });
        m_shortcutService.GetRegistry().FindCommand("asset-browser." + id)->invoke = [&assets, id](const InvocationContext& context)
        { return assets.ExecuteCommand(id, context.targetPath) ? CommandResult::Completed : CommandResult::TargetExpired; };
    }

    auto& designer = m_panelsManager.GetPanelAs<Panels::UIDocumentDesigner>("UI Designer");
    scope("ui-designer", [&designer] { return designer.HasCurrentKeyboardFocus(); });
    for (const std::string id : {"save", "undo", "redo", "duplicate", "copy", "paste", "delete", "rename", "cancel",
        "frame-all", "frame-selection", "zoom-in", "zoom-out", "zoom-100", "revert"})
        add("ui-designer", id, [&designer, id] { return designer.ExecuteCommand(id); }, [&designer, id] { return designer.IsCommandEnabled(id); });
    for (const auto* direction : {"left", "right", "up", "down"})
        for (bool fast : {false, true})
        {
            const auto local = std::string("nudge-") + direction + (fast ? "-fast" : "");
            add("ui-designer", local, [&designer, local] { return designer.ExecuteCommand(local); },
                [&designer, local] { return designer.IsCommandEnabled(local); });
        }

    if (m_panelsManager.HasPanel("Particle Editor"))
    {
        auto& vfx = m_panelsManager.GetPanelAs<Panels::VfxEditor>("Particle Editor");
        scope("vfx-editor", [&vfx] { return vfx.CanUseShortcuts(); });
        for (const std::string id : {"File.Save", "Edit.Undo", "Edit.Redo", "Edit.Copy", "Edit.Cut", "Edit.Paste",
            "Stack.CollapseToHeaders", "Stack.CollapseDisabled", "Find", "Simulation.Reset", "Cancel", "Select.Isolate",
            "Select.DisableEmitters", "Select.EnableEmitters", "Focus", "Overview.ZoomToFitAll", "Emitter.Add", "Module.Delete",
            "File.New", "File.Open", "Compile", "Compile.AutoCompile", "Simulation.PlayPause", "Simulation.Step", "Simulation.StepBack",
            "Preview.Grid", "Preview.Bounds", "Preview.OriginAxis", "Preview.ParticleCounts", "Preview.ToggleProjection", "Preview.FocusSelection", "Preview.FocusAll",
            "Overview.ZoomToFit", "View.ResetLayout"})
        {
            ShortcutCommand command; command.id = "vfx-editor." + id; command.context = "vfx-editor";
            command.category = "vfx-editor"; command.displayName = id;
            command.canExecute = [&vfx, id](const InvocationContext& context) { return vfx.IsCommandEnabled(id, context.targetPath); };
            command.invoke = [&vfx, id](const InvocationContext& context) { return vfx.ExecuteCommand(id, context.targetPath) ? CommandResult::Completed : CommandResult::TargetExpired; };
            m_shortcutService.RegisterCommand(std::move(command));
        }
    }
    if (m_panelsManager.HasPanel("Particle Library Editor"))
    {
        auto& library = m_panelsManager.GetPanelAs<Panels::VfxLibraryEditor>("Particle Library Editor");
        scope("vfx-library", [&library] { return library.HasCurrentKeyboardFocus(); });
        for (const std::string id : {"save", "undo", "redo", "cancel"})
            add("vfx-library", id, [&library, id] { return library.ExecuteCommand(id); });
    }
    auto& console = m_panelsManager.GetPanelAs<Panels::Console>("Console");
    scope("console", [&console] { return console.HasCurrentKeyboardFocus(); });
    for (const std::string id : {"previous", "next", "end", "open", "copy"})
        add("console", id, [&console, id] { return console.ExecuteCommand(id); });
}
}
