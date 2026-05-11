
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <Debug/Logger.h>
#include <Profiling/Profiler.h>
#include <Profiling/TracyProfiler.h>
#include <Reflection/ReflectionDiagnostics.h>
#include <ServiceLocator.h>
#include <imgui.h>

#include "Core/Editor.h"
#include "UI/Settings/PanelWindowSettings.h"
#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"

#include "Panels/EditorTopBar.h"
#include "Panels/EditorStatusBar.h"
#include "Panels/AssetBrowser.h"
#include "Panels/FrameInfo.h"
#include "Panels/Console.h"
#include "Panels/Inspector.h"
#include "Panels/Hierarchy.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Panels/AssetView.h"
#include "Panels/MaterialEditor.h"
#include "Panels/ProjectSettings.h"
#include "Panels/AssetProperties.h"
#include "Panels/ProfilerPanel.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Shortcuts/EditorShortcutBinding.h"
#include "Shortcuts/EditorShortcutContext.h"
#include <Windowing/Inputs/EMouseButton.h>
#include <Windowing/Inputs/EMouseButtonState.h>
using namespace NLS::Core::ResourceManagement;
using namespace NLS::Editor::Panels;
using namespace NLS::Render::Resources::Loaders;
using namespace NLS::Render::Resources::Parsers;
namespace NLS
{
namespace
{
NLS::Base::Profiling::TracyProfiler g_tracyProfiler;
std::size_t g_publishedReflectionDiagnosticCount = 0;

enum class ValidationFocusTarget
{
    None,
    SceneView,
    GameView
};

std::string NormalizeValidationToken(std::string_view value)
{
    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
    return normalized;
}

ValidationFocusTarget ResolveValidationFocusTarget(std::string_view value)
{
    const std::string normalized = NormalizeValidationToken(value);
    if (normalized.empty())
        return ValidationFocusTarget::None;
    if (normalized == "scene" || normalized == "sceneview" || normalized == "scene-view")
        return ValidationFocusTarget::SceneView;
    if (normalized == "game" || normalized == "gameview" || normalized == "game-view")
        return ValidationFocusTarget::GameView;
    return ValidationFocusTarget::None;
}

void RenameFileReplacingDestination(const std::filesystem::path& source, const std::filesystem::path& destination)
{
    std::error_code error;
    if (std::filesystem::exists(destination))
        std::filesystem::remove(destination, error);

    std::filesystem::rename(source, destination, error);
    if (error)
    {
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error);
        if (!error)
            std::filesystem::remove(source, error);
    }
}

void MigrateLegacyMaterialAssets(Editor::Core::EditorActions& editorActions, const std::string& projectAssetsPath)
{
    for (const auto& entry : std::filesystem::recursive_directory_iterator(projectAssetsPath))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".ovmat")
            continue;

        const auto sourcePath = entry.path();
        const auto targetPath = sourcePath.parent_path() / (sourcePath.stem().string() + ".mat");

        RenameFileReplacingDestination(sourcePath, targetPath);

        const auto sourceMeta = sourcePath.string() + ".meta";
        const auto targetMeta = targetPath.string() + ".meta";
        if (std::filesystem::exists(sourceMeta))
            RenameFileReplacingDestination(sourceMeta, targetMeta);

        editorActions.PropagateFileRename(sourcePath.string(), targetPath.string());
        NLS_LOG_INFO("Migrated legacy material asset: " + sourcePath.string() + " -> " + targetPath.string());
    }
}

void PublishReflectionDiagnosticsToLog()
{
    const auto diagnostics = NLS::meta::ReflectionDiagnostics::Snapshot();
    if (g_publishedReflectionDiagnosticCount > diagnostics.size())
        g_publishedReflectionDiagnosticCount = 0;

    for (std::size_t index = g_publishedReflectionDiagnosticCount; index < diagnostics.size(); ++index)
    {
        const auto& diagnostic = diagnostics[index];
        const std::string message = "Reflection: " + NLS::meta::ReflectionDiagnostics::Format(diagnostic);
        if (diagnostic.severity == NLS::meta::ReflectionDiagnosticSeverity::Error)
            NLS_LOG_ERROR(message);
        else
            NLS_LOG_WARNING(message);
    }

    g_publishedReflectionDiagnosticCount = diagnostics.size();
}
}

Editor::Core::Editor::Editor(Context& p_context)
    : m_context(p_context), m_panelsManager(m_canvas),
    m_editorActions(m_context, m_panelsManager)
{
    NLS::Base::Profiling::Profiler::SetEnabled(false);
    NLS::Base::Profiling::Profiler::RegisterDestination(g_tracyProfiler);

    NLS::Core::ServiceLocator::Provide<NLS::Editor::Core::Context>(m_context);
    NLS::Core::ServiceLocator::Provide<NLS::Editor::Core::Editor>(*this);
    NLS::Core::ServiceLocator::Provide<NLS::Editor::Shortcuts::EditorShortcutService>(m_shortcutService);
    Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<Engine::AssemblyEngine>();
	
    SetupUI();
    PublishReflectionDiagnosticsToLog();
    RegisterShortcutContexts();
    RegisterDefaultShortcuts();
    MigrateLegacyMaterialAssets(m_editorActions, m_context.projectAssetsPath);

    m_sceneSourcePathChangedListener = m_context.sceneManager.CurrentSceneSourcePathChangedEvent += [this](const std::string& p_scenePath)
    {
        RememberLastOpenedScene(p_scenePath);
    };

    RestoreStartupScene();

    ApplyStartupValidationDirectives();
}

Editor::Core::Editor::~Editor()
{
    m_shortcutService.SaveProfile(std::filesystem::path(m_context.projectPath) / "UserSettings" / "shortcuts.json");
    NLS::Base::Profiling::Profiler::UnregisterDestination(
        m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler").GetTimelineSink());
    m_editorActions.PromptSaveCurrentSceneIfDirty();
    m_panelsManager.DestroyPanels();
    m_context.sceneManager.CurrentSceneSourcePathChangedEvent -= m_sceneSourcePathChangedListener;
    m_context.sceneManager.UnloadCurrentScene();
}

void Editor::Core::Editor::SetupUI()
{
    NLS::UI::PanelWindowSettings settings;
    settings.closable = true;
    settings.collapsable = true;
    settings.dockable = true;

    m_panelsManager.CreatePanel<Panels::EditorTopBar>("Editor Top Bar");
    m_panelsManager.CreatePanel<Panels::EditorStatusBar>("Editor Status Bar");
    m_panelsManager.CreatePanel<Panels::AssetBrowser>("Asset Browser", true, settings, m_context.engineAssetsPath, m_context.projectAssetsPath);
    m_panelsManager.CreatePanel<Panels::FrameInfo>("Frame Info", false, settings);
    m_panelsManager.CreatePanel<Panels::ProfilerPanel>("Profiler", false, settings);
    auto& profilerPanel = m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler");
    NLS::Base::Profiling::Profiler::RegisterDestination(profilerPanel.GetTimelineSink());
    m_panelsManager.CreatePanel<Panels::Console>("Console", false, settings);
    m_panelsManager.CreatePanel<Panels::AssetView>("Asset View", false, settings);
    m_panelsManager.CreatePanel<Panels::Hierarchy>("Hierarchy", true, settings);
    m_panelsManager.CreatePanel<Panels::Inspector>("Inspector", true, settings);
    m_panelsManager.CreatePanel<Panels::SceneView>("Scene View", true, settings);
    m_panelsManager.CreatePanel<Panels::GameView>("Game View", true, settings);
    m_panelsManager.CreatePanel<Panels::MaterialEditor>("Material Editor", false, settings);
    m_panelsManager.CreatePanel<Panels::ProjectSettings>("Project Settings", false, settings);
    m_panelsManager.GetPanelAs<Panels::ProjectSettings>("Project Settings").enabled = false;
    m_panelsManager.CreatePanel<Panels::AssetProperties>("Asset Properties", false, settings);
    auto& topBar = m_panelsManager.GetPanelAs<Panels::EditorTopBar>("Editor Top Bar");
    topBar.RegisterProjectSettingsPanel(m_panelsManager.GetPanelAs<Panels::ProjectSettings>("Project Settings"));
    topBar.RegisterWindowPanel("Asset Browser", m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser"));
    topBar.RegisterWindowPanel("Frame Info", m_panelsManager.GetPanelAs<Panels::FrameInfo>("Frame Info"));
    topBar.RegisterWindowPanel("Profiler", profilerPanel);
    topBar.RegisterWindowPanel("Console", m_panelsManager.GetPanelAs<Panels::Console>("Console"));
    topBar.RegisterWindowPanel("Asset View", m_panelsManager.GetPanelAs<Panels::AssetView>("Asset View"));
    topBar.RegisterWindowPanel("Hierarchy", m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy"));
    topBar.RegisterWindowPanel("Inspector", m_panelsManager.GetPanelAs<Panels::Inspector>("Inspector"));
    topBar.RegisterWindowPanel("Scene View", m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View"));
    topBar.RegisterWindowPanel("Game View", m_panelsManager.GetPanelAs<Panels::GameView>("Game View"));
    topBar.RegisterWindowPanel("Material Editor", m_panelsManager.GetPanelAs<Panels::MaterialEditor>("Material Editor"));
    topBar.RegisterWindowPanel("Asset Properties", m_panelsManager.GetPanelAs<Panels::AssetProperties>("Asset Properties"));
    // Needs to be called after all panels got created, because some settings in this menu depend on other panels
    topBar.InitializeSettingsMenu();
    m_canvas.MakeDockspace(true);
    m_context.uiManager->SetCanvas(m_canvas);
    m_context.uiManager->ResetLayout(m_context.projectPath + "/UserSettings/layout.ini");
}

void Editor::Core::Editor::PreUpdate()
{
    RefreshProfilerRecordingState();
    m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler").BeginProfilerFrame();
    NLS_PROFILE_SCOPE();
    m_context.device->PollEvents();
}

void Editor::Core::Editor::Update(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();

    m_currentDeltaTime = p_deltaTime;
    if (p_deltaTime > std::numeric_limits<float>::epsilon())
    {
        m_frameRateAccumulatedTime += p_deltaTime;
        ++m_frameRateSampleCount;

        if (m_frameRateAccumulatedTime >= 1.0f)
        {
            m_currentFrameRate = static_cast<float>(m_frameRateSampleCount) / m_frameRateAccumulatedTime;
            if (m_context.GetDiagnosticsSettings().logEditorFps)
                NLS_LOG_INFO("Editor FPS: " + std::to_string(m_currentFrameRate));

            m_frameRateAccumulatedTime = 0.0f;
            m_frameRateSampleCount = 0;
        }
    }

    {
        NLS_PROFILE_NAMED_SCOPE("Editor::HandleGlobalShortcuts");
        HandleGlobalShortcuts();
    }
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateCurrentEditorMode");
        UpdateCurrentEditorMode(p_deltaTime);
    }
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateViews");
        UpdateViews(p_deltaTime);
    }
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateEditorPanels");
        UpdateEditorPanels(p_deltaTime);
    }
    RenderEditorUI(p_deltaTime);
    {
        NLS_PROFILE_NAMED_SCOPE("EditorActions::ExecuteDelayedActions");
        m_editorActions.ExecuteDelayedActions();
    }
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::PublishReflectionDiagnosticsToLog");
        PublishReflectionDiagnosticsToLog();
    }
}

void Editor::Core::Editor::RefreshProfilerRecordingState()
{
    auto& profilerPanel = m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler");
    profilerPanel.GetTimelineSink().SetRecordingEnabled(profilerPanel.IsRecordingEnabled());

    const bool tracyConnected = NLS::Base::Profiling::TracyProfiler::IsConnected();
    NLS::Base::Profiling::Profiler::SetEnabled(tracyConnected || profilerPanel.IsRecordingEnabled());
}

void Editor::Core::Editor::HandleGlobalShortcuts()
{
    m_shortcutService.ExecutePressedShortcut(
        [this](Windowing::Inputs::EKey p_key)
        {
            return m_context.inputManager->IsKeyPressed(p_key);
        },
        [this](Windowing::Inputs::EKey p_key)
        {
            return m_context.inputManager->GetKeyState(p_key);
        });
}

void Editor::Core::Editor::RegisterShortcutContexts()
{
    using namespace NLS::Editor::Shortcuts;
    using namespace Windowing::Inputs;

    const auto isSceneViewFocused = [this]
    {
        return m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").IsFocused();
    };

    m_shortcutService.RegisterContext({
        ShortcutContexts::SceneView,
        "Scene View",
        20,
        "focused-panel",
        isSceneViewFocused });

    m_shortcutService.RegisterContext({
        ShortcutContexts::SceneViewFlyMode,
        "Scene View/Fly Mode",
        40,
        "scene-navigation-mode",
        [this, isSceneViewFocused]
        {
            return isSceneViewFocused() &&
                m_context.inputManager->GetMouseButtonState(EMouseButton::MOUSE_BUTTON_RIGHT) == EMouseButtonState::MOUSE_DOWN;
        }});

    m_shortcutService.RegisterContext({
        ShortcutContexts::Hierarchy,
        "Hierarchy",
        20,
        "focused-panel",
        [this]
        {
            return m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy").IsFocused();
        }});

    m_shortcutService.RegisterContext({
        ShortcutContexts::TextInput,
        "Text Input",
        100,
        "",
        []
        {
            return ImGui::GetIO().WantTextInput;
        }});
}

void Editor::Core::Editor::RegisterDefaultShortcuts()
{
    using namespace NLS::Editor::Shortcuts;
    using Windowing::Inputs::EKey;

    const auto registerCommand = [this](ShortcutCommand p_command)
    {
        m_shortcutService.RegisterCommand(std::move(p_command));
    };

    auto makeCommand = [](std::string p_id,
                          std::string p_displayName,
                          std::string p_category,
                          ShortcutContextId p_context,
                          ShortcutBinding p_binding,
                          std::function<void()> p_execute)
    {
        ShortcutCommand command;
        command.id = std::move(p_id);
        command.displayName = std::move(p_displayName);
        command.category = std::move(p_category);
        command.context = std::move(p_context);
        command.defaultBinding = p_binding;
        command.execute = std::move(p_execute);
        return command;
    };

    registerCommand(makeCommand(
        "file.new-scene",
        "New Scene",
        "File",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl),
        [this] { m_editorActions.LoadEmptyScene(); }));

    registerCommand(makeCommand(
        "file.save-scene",
        "Save Scene",
        "File",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl),
        [this] { m_editorActions.SaveSceneChanges(); }));

    registerCommand(makeCommand(
        "file.save-scene-as",
        "Save Scene As",
        "File",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl | EShortcutModifier::Shift),
        [this] { m_editorActions.SaveAs(); }));

    registerCommand(makeCommand(
        "editor.play",
        "Play",
        "Editor",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_F5),
        [this] { m_editorActions.StartPlaying(); }));

    auto stopCommand = makeCommand(
        "editor.stop",
        "Stop",
        "Editor",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_ESCAPE),
        [this] { m_editorActions.StopPlaying(); });
    stopCommand.availability = [this]
    {
        return m_editorActions.GetCurrentEditorMode() != EditorActions::EEditorMode::EDIT;
    };
    registerCommand(std::move(stopCommand));

    auto captureCommand = makeCommand(
        "debug.renderdoc.capture-next-frame",
        "Capture Next Frame",
        "Debugging",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_F11),
        [this]
        {
            if (Render::Context::DriverUIAccess::IsRenderDocAvailable(*m_context.driver))
                Render::Context::DriverUIAccess::QueueRenderDocCapture(*m_context.driver, "Editor");
        });
    captureCommand.allowDuringTextInput = true;
    registerCommand(std::move(captureCommand));

    auto openCaptureCommand = makeCommand(
        "debug.renderdoc.open-latest-capture",
        "Open Latest RenderDoc Capture",
        "Debugging",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_F11, EShortcutModifier::Ctrl),
        [this]
        {
            Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*m_context.driver);
        });
    openCaptureCommand.allowDuringTextInput = true;
    registerCommand(std::move(openCaptureCommand));

    const auto registerSceneToolCommand = [&](std::string p_id,
                                              std::string p_displayName,
                                              const EKey p_key,
                                              std::function<void(Panels::SceneView&)> p_execute)
    {
        registerCommand(makeCommand(
            std::move(p_id),
            std::move(p_displayName),
            "Scene View",
            ShortcutContexts::SceneView,
            ShortcutBinding::FromKey(p_key),
            [this, execute = std::move(p_execute)]
            {
                execute(m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View"));
            }));
    };

    registerCommand(makeCommand(
        "scene-view.fly-forward",
        "Fly Mode Forward",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_W),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-backward",
        "Fly Mode Backward",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_S),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-left",
        "Fly Mode Left",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_A),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-right",
        "Fly Mode Right",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_D),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-up",
        "Fly Mode Up",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_E),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-down",
        "Fly Mode Down",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_Q),
        [] {}));

    registerSceneToolCommand(
        "scene-view.view-tool",
        "View",
        EKey::KEY_Q,
        [](Panels::SceneView&) {});
    registerSceneToolCommand(
        "scene-view.move-tool",
        "Move",
        EKey::KEY_W,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::TRANSLATE); });
    registerSceneToolCommand(
        "scene-view.rotate-tool",
        "Rotate",
        EKey::KEY_E,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::ROTATE); });
    registerSceneToolCommand(
        "scene-view.scale-tool",
        "Scale",
        EKey::KEY_R,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::SCALE); });
    registerSceneToolCommand(
        "scene-view.rect-tool",
        "Rect",
        EKey::KEY_T,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::TRANSLATE); });
    registerSceneToolCommand(
        "scene-view.transform-tool",
        "Transform",
        EKey::KEY_Y,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::TRANSLATE); });
    registerSceneToolCommand(
        "scene-view.toggle-pivot-position",
        "Toggle Pivot Position",
        EKey::KEY_Z,
        [](Panels::SceneView& p_sceneView) { p_sceneView.ToggleCurrentGizmoPivot(); });
    registerSceneToolCommand(
        "scene-view.toggle-pivot-orientation",
        "Toggle Pivot Orientation",
        EKey::KEY_X,
        [](Panels::SceneView& p_sceneView) { p_sceneView.ToggleCurrentGizmoSpace(); });

    registerCommand(makeCommand(
        "edit.delete-selected-actor",
        "Delete Selected Actor",
        "Edit",
        ShortcutContexts::SceneView,
        ShortcutBinding::FromKey(EKey::KEY_DELETE),
        [this]
        {
            if (m_editorActions.IsAnyActorSelected())
                m_editorActions.DestroyActor(*m_editorActions.GetSelectedActor());
        }));

    registerCommand(makeCommand(
        "edit.delete-selected-actor-hierarchy",
        "Delete Selected Actor",
        "Edit",
        ShortcutContexts::Hierarchy,
        ShortcutBinding::FromKey(EKey::KEY_DELETE),
        [this]
        {
            if (m_editorActions.IsAnyActorSelected())
                m_editorActions.DestroyActor(*m_editorActions.GetSelectedActor());
        }));

    m_shortcutService.LoadProfile(std::filesystem::path(m_context.projectPath) / "UserSettings" / "shortcuts.json");
}

void Editor::Core::Editor::RestoreStartupScene()
{
    const auto loadRememberedScene = [this](const std::string& scenePath) -> bool
    {
        if (scenePath.empty() || scenePath == "NULL")
            return false;

        const std::filesystem::path configuredPath(scenePath);
        const auto resolvedPath = configuredPath.is_absolute()
            ? configuredPath
            : std::filesystem::path(m_context.projectAssetsPath) / configuredPath;

        if (!std::filesystem::exists(resolvedPath))
            return false;

        return m_context.sceneManager.LoadScene(resolvedPath.string(), true);
    };

    const auto lastOpenedScene = m_context.projectSettings.GetOrDefault<std::string>("last_opened_scene", "");
    if (loadRememberedScene(lastOpenedScene))
        return;

    const auto startScene = m_context.projectSettings.Get<std::string>("start_scene");
    const auto startScenePath = m_context.projectAssetsPath + startScene;
    if (!startScene.empty() && startScene != "NULL" && std::filesystem::exists(startScenePath))
    {
        m_context.sceneManager.LoadScene(startScenePath, true);
        return;
    }

    m_context.sceneManager.LoadEmptyLightedScene();
    m_context.sceneManager.MarkCurrentSceneDirty();
}

void Editor::Core::Editor::RememberLastOpenedScene(const std::string& p_scenePath)
{
    std::string storedScenePath = p_scenePath;
    if (!storedScenePath.empty())
    {
        std::error_code scenePathError;
        std::error_code assetsPathError;
        const auto absoluteScenePath = std::filesystem::absolute(storedScenePath, scenePathError);
        const auto absoluteAssetsPath = std::filesystem::absolute(m_context.projectAssetsPath, assetsPathError);
        if (!scenePathError && !assetsPathError)
        {
            const auto relativeScenePath = absoluteScenePath.lexically_relative(absoluteAssetsPath);
            const auto relativeScenePathText = relativeScenePath.generic_string();
            if (!relativeScenePathText.empty() && relativeScenePathText != ".." && relativeScenePathText.rfind("../", 0) != 0)
                storedScenePath = relativeScenePathText;
        }
    }

    if (!m_context.projectSettings.IsKeyExisting("last_opened_scene"))
        m_context.projectSettings.Add<std::string>("last_opened_scene", storedScenePath);
    else
        m_context.projectSettings.Set<std::string>("last_opened_scene", storedScenePath);

    m_context.projectSettings.Rewrite();
}

void Editor::Core::Editor::ApplyStartupValidationDirectives()
{
    const auto& diagnostics = m_context.GetDiagnosticsSettings();
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");

    switch (ResolveValidationFocusTarget(diagnostics.editorValidationExclusiveView))
    {
    case ValidationFocusTarget::SceneView:
        sceneView.Open();
        gameView.Close();
        NLS_LOG_INFO("Editor validation isolated Scene View.");
        break;
    case ValidationFocusTarget::GameView:
        gameView.Open();
        sceneView.Close();
        NLS_LOG_INFO("Editor validation isolated Game View.");
        break;
    case ValidationFocusTarget::None:
    default:
        break;
    }

    switch (ResolveValidationFocusTarget(diagnostics.editorValidationFocusView))
    {
    case ValidationFocusTarget::SceneView:
        sceneView.Focus();
        NLS_LOG_INFO("Editor validation pre-focused Scene View.");
        break;
    case ValidationFocusTarget::GameView:
        gameView.Focus();
        NLS_LOG_INFO("Editor validation pre-focused Game View.");
        break;
    case ValidationFocusTarget::None:
    default:
        break;
    }

    if (!diagnostics.editorValidationSelectActor.empty())
    {
        if (auto* currentScene = m_context.sceneManager.GetCurrentScene();
            currentScene != nullptr)
        {
            if (auto* actor = currentScene->FindActorByName(diagnostics.editorValidationSelectActor);
                actor != nullptr)
            {
                m_editorActions.SelectActor(*actor);
                NLS_LOG_INFO("Editor validation pre-selected actor: " + diagnostics.editorValidationSelectActor);
            }
            else
            {
                NLS_LOG_WARNING(
                    "Editor validation could not find actor during startup: " +
                    diagnostics.editorValidationSelectActor);
            }
        }
    }
}

void Editor::Core::Editor::UpdateCurrentEditorMode(float p_deltaTime)
{
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateModeState");
        if (auto editorMode = m_editorActions.GetCurrentEditorMode(); editorMode == EditorActions::EEditorMode::PLAY || editorMode == EditorActions::EEditorMode::FRAME_BY_FRAME)
            UpdatePlayMode(p_deltaTime);
        else
            UpdateEditMode(p_deltaTime);
    }

    {
        NLS_PROFILE_NAMED_SCOPE("SceneManager::CollectGarbagesAndUpdate");
        m_context.sceneManager.GetCurrentScene()->CollectGarbages();
        m_context.sceneManager.Update();
    }
}

void Editor::Core::Editor::UpdatePlayMode(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    auto currentScene = m_context.sceneManager.GetCurrentScene();

    {
        NLS_PROFILE_NAMED_SCOPE("Scene::Update");
        currentScene->Update(p_deltaTime);
    }

    {
        NLS_PROFILE_NAMED_SCOPE("Scene::LateUpdate");
        currentScene->LateUpdate(p_deltaTime);
    }


    if (m_editorActions.GetCurrentEditorMode() == EditorActions::EEditorMode::FRAME_BY_FRAME)
        m_editorActions.PauseGame();
}

void Editor::Core::Editor::UpdateEditMode(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    (void)p_deltaTime;
}

void Editor::Core::Editor::UpdateEditorPanels(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    auto& topBar = m_panelsManager.GetPanelAs<NLS::Editor::Panels::EditorTopBar>("Editor Top Bar");
    auto& frameInfo = m_panelsManager.GetPanelAs<NLS::Editor::Panels::FrameInfo>("Frame Info");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");

    {
        NLS_PROFILE_NAMED_SCOPE("EditorTopBar::HandleShortcuts");
        topBar.HandleShortcuts(p_deltaTime);
    }

    const bool keepDefaultSceneFocus =
        ResolveValidationFocusTarget(m_context.GetDiagnosticsSettings().editorValidationFocusView) ==
        ValidationFocusTarget::None;
    if (m_elapsedFrames == 1 && keepDefaultSceneFocus) // Let the first frame happen and then make the scene view the first seen view
        sceneView.Focus();

    if (frameInfo.IsOpened())
    {

        if (sceneView.IsFocused())
        {
            frameInfo.Update(&sceneView);
        }
        else if (gameView.IsFocused())
        {
            frameInfo.Update(&gameView);
        }
        else if (assetView.IsFocused())
        {
            frameInfo.Update(&assetView);
        }
        else
        {
            frameInfo.Update(nullptr);
        }
    }
}

void Editor::Core::Editor::UpdateViews(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");

    {
        NLS_PROFILE_NAMED_SCOPE("AssetView::Update");
        assetView.Update(p_deltaTime);
    }
    {
        NLS_PROFILE_NAMED_SCOPE("GameView::Update");
        gameView.Update(p_deltaTime);
    }
    {
        NLS_PROFILE_NAMED_SCOPE("SceneView::Update");
        sceneView.Update(p_deltaTime);
    }
}

void Editor::Core::Editor::RenderEditorUI(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();

    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::RenderEditorUI: begin");

    // Set up the explicit scene -> UI -> present synchronization boundary.
    Render::Context::DriverUIAccess::UICompositionSyncBoundary uiSyncBoundary;
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::BuildUICompositionSyncBoundary");
        uiSyncBoundary = Render::Context::DriverUIAccess::BuildUICompositionSyncBoundary(*m_context.driver);
    }
    if (uiSyncBoundary.sceneToUiWaitSemaphore.IsValid())
    {
        NLS_PROFILE_NAMED_SCOPE("UIManager::SetWaitSemaphore");
        m_context.uiManager->SetWaitSemaphore(uiSyncBoundary.sceneToUiWaitSemaphore);
    }

    NLS::Render::RHI::NativeHandle uiSignalSemaphore;
    {
        NLS_PROFILE_NAMED_SCOPE("UIManager::ResolveUISignalSemaphore");
        uiSignalSemaphore = m_context.uiManager->ResolveUISignalSemaphore();
    }

    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UIManagerRender");
        EDITOR_CONTEXT(uiManager)->Render();
    }
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::RenderEditorUI: UIManager::Render returned");

    if (uiSignalSemaphore.IsValid())
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::SetUICompositionSignal");
        Render::Context::DriverUIAccess::SetUICompositionSignal(
            *m_context.driver,
            uiSignalSemaphore,
            m_context.uiManager->ResolveUISignalValue());
    }

    {
        NLS_PROFILE_NAMED_SCOPE("UIManager::SubmitUIRendering");
        m_context.uiManager->SubmitUIRendering();
    }
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::RenderEditorUI: SubmitUIRendering returned");
}

void Editor::Core::Editor::PostUpdate()
{
    NLS_PROFILE_SCOPE();
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: begin");

    {
        NLS_PROFILE_NAMED_SCOPE("DriverUIAccess::PresentSwapchain");
        Render::Context::DriverUIAccess::PresentSwapchain(*m_context.driver);
    }
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: PresentSwapchain returned");

    {
        NLS_PROFILE_NAMED_SCOPE("InputManager::ClearEvents");
        m_context.inputManager->ClearEvents();
    }
    ++m_elapsedFrames;
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: end");
}

float Editor::Core::Editor::GetCurrentFrameRate() const
{
    return m_currentFrameRate;
}

float Editor::Core::Editor::GetCurrentDeltaTime() const
{
    return m_currentDeltaTime;
}

void Editor::Core::Editor::OpenConsole()
{
    auto& console = m_panelsManager.GetPanelAs<Panels::Console>("Console");
    console.Open();
    console.Focus();
    console.ScrollToBottom();
}
} // namespace NLS
