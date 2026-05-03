
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <Debug/Logger.h>
#include <ServiceLocator.h>

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
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
using namespace NLS::Core::ResourceManagement;
using namespace NLS::Editor::Panels;
using namespace NLS::Render::Resources::Loaders;
using namespace NLS::Render::Resources::Parsers;
namespace NLS
{
namespace
{
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
}

Editor::Core::Editor::Editor(Context& p_context)
    : m_context(p_context), m_panelsManager(m_canvas),
    m_editorActions(m_context, m_panelsManager)
{
    NLS::Core::ServiceLocator::Provide<NLS::Editor::Core::Editor>(*this);
    Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<Engine::AssemblyEngine>();
	
    SetupUI();
    MigrateLegacyMaterialAssets(m_editorActions, m_context.projectAssetsPath);

    const auto startScene = m_context.projectSettings.Get<std::string>("start_scene");
    const auto startScenePath = m_context.projectAssetsPath + startScene;
	if (!startScene.empty() && std::filesystem::exists(startScenePath))
	{
		m_context.sceneManager.LoadScene(startScenePath, true);
	}
	else
	{
		m_context.sceneManager.LoadEmptyLightedScene();
	}

    ApplyStartupValidationDirectives();
}

Editor::Core::Editor::~Editor()
{
    m_panelsManager.DestroyPanels();
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
    m_panelsManager.CreatePanel<Panels::Console>("Console", true, settings);
    m_panelsManager.CreatePanel<Panels::AssetView>("Asset View", false, settings);
    m_panelsManager.CreatePanel<Panels::Hierarchy>("Hierarchy", true, settings);
    m_panelsManager.CreatePanel<Panels::Inspector>("Inspector", true, settings);
    m_panelsManager.CreatePanel<Panels::SceneView>("Scene View", true, settings);
    m_panelsManager.CreatePanel<Panels::GameView>("Game View", true, settings);
    m_panelsManager.CreatePanel<Panels::MaterialEditor>("Material Editor", false, settings);
    m_panelsManager.CreatePanel<Panels::ProjectSettings>("Project Settings", false, settings);
    m_panelsManager.CreatePanel<Panels::AssetProperties>("Asset Properties", false, settings);

    auto& topBar = m_panelsManager.GetPanelAs<Panels::EditorTopBar>("Editor Top Bar");
    topBar.RegisterWindowPanel("Asset Browser", m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser"));
    topBar.RegisterWindowPanel("Frame Info", m_panelsManager.GetPanelAs<Panels::FrameInfo>("Frame Info"));
    topBar.RegisterWindowPanel("Console", m_panelsManager.GetPanelAs<Panels::Console>("Console"));
    topBar.RegisterWindowPanel("Asset View", m_panelsManager.GetPanelAs<Panels::AssetView>("Asset View"));
    topBar.RegisterWindowPanel("Hierarchy", m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy"));
    topBar.RegisterWindowPanel("Inspector", m_panelsManager.GetPanelAs<Panels::Inspector>("Inspector"));
    topBar.RegisterWindowPanel("Scene View", m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View"));
    topBar.RegisterWindowPanel("Game View", m_panelsManager.GetPanelAs<Panels::GameView>("Game View"));
    topBar.RegisterWindowPanel("Material Editor", m_panelsManager.GetPanelAs<Panels::MaterialEditor>("Material Editor"));
    topBar.RegisterWindowPanel("Project Settings", m_panelsManager.GetPanelAs<Panels::ProjectSettings>("Project Settings"));
    topBar.RegisterWindowPanel("Asset Properties", m_panelsManager.GetPanelAs<Panels::AssetProperties>("Asset Properties"));

    // Needs to be called after all panels got created, because some settings in this menu depend on other panels
    topBar.InitializeSettingsMenu();
    m_canvas.MakeDockspace(true);
    m_context.uiManager->SetCanvas(m_canvas);
    m_context.uiManager->ResetLayout(m_context.projectPath + "/UserSettings/layout.ini");
}

void Editor::Core::Editor::PreUpdate()
{
    m_context.device->PollEvents();
}

void Editor::Core::Editor::Update(float p_deltaTime)
{
    m_currentDeltaTime = p_deltaTime;
    if (p_deltaTime > std::numeric_limits<float>::epsilon())
    {
        m_frameRateAccumulatedTime += p_deltaTime;
        ++m_frameRateSampleCount;

        if (m_frameRateAccumulatedTime >= 1.0f)
        {
            m_currentFrameRate = static_cast<float>(m_frameRateSampleCount) / m_frameRateAccumulatedTime;
            m_frameRateAccumulatedTime = 0.0f;
            m_frameRateSampleCount = 0;
        }
    }

    HandleGlobalShortcuts();
    UpdateCurrentEditorMode(p_deltaTime);
    UpdateViews(p_deltaTime);
    UpdateEditorPanels(p_deltaTime);
    RenderEditorUI(p_deltaTime);
    m_editorActions.ExecuteDelayedActions();
}

void Editor::Core::Editor::HandleGlobalShortcuts()
{
    if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_F11))
    {
        if (m_context.inputManager->GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_CONTROL) == Windowing::Inputs::EKeyState::KEY_DOWN)
            Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*m_context.driver);
        else if (Render::Context::DriverUIAccess::IsRenderDocAvailable(*m_context.driver))
            Render::Context::DriverUIAccess::QueueRenderDocCapture(*m_context.driver, "Editor");
    }

    // If the [Del] key is pressed while an actor is selected and the Scene View or Hierarchy is focused
    if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_DELETE) && EDITOR_EXEC(IsAnyActorSelected()) && (EDITOR_PANEL(SceneView, "Scene View").IsFocused() || EDITOR_PANEL(Hierarchy, "Hierarchy").IsFocused()))
    {
        EDITOR_EXEC(DestroyActor(*EDITOR_EXEC(GetSelectedActor())));
    }
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
    if (auto editorMode = m_editorActions.GetCurrentEditorMode(); editorMode == EditorActions::EEditorMode::PLAY || editorMode == EditorActions::EEditorMode::FRAME_BY_FRAME)
        UpdatePlayMode(p_deltaTime);
    else
        UpdateEditMode(p_deltaTime);

    {
        m_context.sceneManager.GetCurrentScene()->CollectGarbages();
        m_context.sceneManager.Update();
    }
}

void Editor::Core::Editor::UpdatePlayMode(float p_deltaTime)
{
    auto currentScene = m_context.sceneManager.GetCurrentScene();

    {
        currentScene->Update(p_deltaTime);
    }

    {
        currentScene->LateUpdate(p_deltaTime);
    }


    if (m_editorActions.GetCurrentEditorMode() == EditorActions::EEditorMode::FRAME_BY_FRAME)
        m_editorActions.PauseGame();

    if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_ESCAPE))
        m_editorActions.StopPlaying();
}

void Editor::Core::Editor::UpdateEditMode(float p_deltaTime)
{
    if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_F5))
        m_editorActions.StartPlaying();
}

void Editor::Core::Editor::UpdateEditorPanels(float p_deltaTime)
{
    auto& topBar = m_panelsManager.GetPanelAs<NLS::Editor::Panels::EditorTopBar>("Editor Top Bar");
    auto& frameInfo = m_panelsManager.GetPanelAs<NLS::Editor::Panels::FrameInfo>("Frame Info");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");

    topBar.HandleShortcuts(p_deltaTime);

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
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");

    {
        assetView.Update(p_deltaTime);
        gameView.Update(p_deltaTime);
        sceneView.Update(p_deltaTime);
    }
}

void Editor::Core::Editor::RenderEditorUI(float p_deltaTime)
{
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::RenderEditorUI: begin");

    // Set up UI synchronization semaphores for all backends
    // Get the semaphore that game rendering will signal (UI should wait on this)
    void* renderFinishedSemaphore = Render::Context::DriverUIAccess::GetRenderFinishedSemaphore(*m_context.driver);
    if (renderFinishedSemaphore != nullptr)
    {
        m_context.uiManager->SetWaitSemaphore(renderFinishedSemaphore);
    }

    // Get the UI's signal semaphore (Driver will wait on this during Present)
    NLS::Render::RHI::NativeHandle uiSignalSemaphore = m_context.uiManager->ResolveUISignalSemaphore();

    EDITOR_CONTEXT(uiManager)->Render();
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::RenderEditorUI: UIManager::Render returned");

    if (uiSignalSemaphore.handle != nullptr)
    {
        Render::Context::DriverUIAccess::SetUISignalSemaphore(
            *m_context.driver,
            uiSignalSemaphore.handle,
            m_context.uiManager->ResolveUISignalValue());
    }

    m_context.uiManager->SubmitUIRendering();
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::RenderEditorUI: SubmitUIRendering returned");
}

void Editor::Core::Editor::PostUpdate()
{
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: begin");

    Render::Context::DriverUIAccess::PresentSwapchain(*m_context.driver);
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: PresentSwapchain returned");

    m_context.inputManager->ClearEvents();
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
} // namespace NLS
