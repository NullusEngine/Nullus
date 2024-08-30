
#include "Core/Editor.h"
#include "UI/Settings/PanelWindowSettings.h"
#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"

#include "Panels/MenuBar.h"
#include "Panels/AssetBrowser.h"
#include "Panels/FrameInfo.h"
#include "Panels/Console.h"
#include "Panels/Inspector.h"
#include "Panels/Hierarchy.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Panels/AssetView.h"
#include "Panels/Toolbar.h"
#include "Panels/MaterialEditor.h"
#include "Panels/ProjectSettings.h"
#include "Panels/AssetProperties.h"
using namespace NLS::Core::ResourceManagement;
using namespace NLS::Editor::Panels;
using namespace NLS::Render::Resources::Loaders;
using namespace NLS::Render::Resources::Parsers;
namespace NLS
{
Editor::Core::Editor::Editor(Context& p_context)
    : m_context(p_context), m_panelsManager(m_canvas),
    m_editorActions(m_context, m_panelsManager)
{
    Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<Engine::AssemblyEngine>();
	
    SetupUI();

    m_context.sceneManager.LoadEmptyLightedScene();
}

Editor::Core::Editor::~Editor()
{
    m_context.sceneManager.UnloadCurrentScene();
}

void Editor::Core::Editor::SetupUI()
{
    NLS::UI::PanelWindowSettings settings;
    settings.closable = true;
    settings.collapsable = true;
    settings.dockable = true;

    m_panelsManager.CreatePanel<Panels::MenuBar>("Menu Bar");
    m_panelsManager.CreatePanel<Panels::AssetBrowser>("Asset Browser", true, settings, m_context.engineAssetsPath, m_context.projectAssetsPath);
    m_panelsManager.CreatePanel<Panels::FrameInfo>("Frame Info", true, settings);
    m_panelsManager.CreatePanel<Panels::Console>("Console", true, settings);
    m_panelsManager.CreatePanel<Panels::AssetView>("Asset View", false, settings);
    m_panelsManager.CreatePanel<Panels::Hierarchy>("Hierarchy", true, settings);
    m_panelsManager.CreatePanel<Panels::Inspector>("Inspector", true, settings);
    m_panelsManager.CreatePanel<Panels::SceneView>("Scene View", true, settings);
    m_panelsManager.CreatePanel<Panels::GameView>("Game View", true, settings);
    m_panelsManager.CreatePanel<Panels::Toolbar>("Toolbar", true, settings);
    m_panelsManager.CreatePanel<Panels::MaterialEditor>("Material Editor", false, settings);
    m_panelsManager.CreatePanel<Panels::ProjectSettings>("Project Settings", false, settings);
    m_panelsManager.CreatePanel<Panels::AssetProperties>("Asset Properties", false, settings);

    // Needs to be called after all panels got created, because some settings in this menu depend on other panels
    m_panelsManager.GetPanelAs<Panels::MenuBar>("Menu Bar").InitializeSettingsMenu();
    m_canvas.MakeDockspace(true);
    m_context.uiManager->SetCanvas(m_canvas);
}

void Editor::Core::Editor::PreUpdate()
{
    m_context.device->PollEvents();
}

void Editor::Core::Editor::Update(float p_deltaTime)
{
    HandleGlobalShortcuts();
    UpdateCurrentEditorMode(p_deltaTime);
    RenderViews(p_deltaTime);
    UpdateEditorPanels(p_deltaTime);
    RenderEditorUI(p_deltaTime);
    m_editorActions.ExecuteDelayedActions();
}

void Editor::Core::Editor::HandleGlobalShortcuts()
{
    // If the [Del] key is pressed while an actor is selected and the Scene View or Hierarchy is focused
    if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_DELETE) && EDITOR_EXEC(IsAnyActorSelected()) && (EDITOR_PANEL(SceneView, "Scene View").IsFocused() || EDITOR_PANEL(Hierarchy, "Hierarchy").IsFocused()))
    {
        EDITOR_EXEC(DestroyActor(*EDITOR_EXEC(GetSelectedActor())));
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
    auto& menuBar = m_panelsManager.GetPanelAs<NLS::Editor::Panels::MenuBar>("Menu Bar");
    auto& frameInfo = m_panelsManager.GetPanelAs<NLS::Editor::Panels::FrameInfo>("Frame Info");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");

    menuBar.HandleShortcuts(p_deltaTime);

    if (m_elapsedFrames == 1) // Let the first frame happen and then make the scene view the first seen view
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

void Editor::Core::Editor::RenderViews(float p_deltaTime)
{
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");

    {
        assetView.Update(p_deltaTime);
        gameView.Update(p_deltaTime);
        sceneView.Update(p_deltaTime);
    }

    if (assetView.IsOpened())
    {
        assetView.Render();
    }

    if (gameView.IsOpened())
    {
        gameView.Render();
    }

    if (sceneView.IsOpened())
    {
        sceneView.Render();
    }
}

void Editor::Core::Editor::RenderEditorUI(float p_deltaTime)
{
    EDITOR_CONTEXT(uiManager)->Render();
}

void Editor::Core::Editor::PostUpdate()
{
    m_context.window->SwapBuffers();
    m_context.inputManager->ClearEvents();
    ++m_elapsedFrames;
}
} // namespace NLS
