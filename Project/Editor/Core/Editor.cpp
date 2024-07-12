
#include "Core/Editor.h"
#include "UI/Settings/PanelWindowSettings.h"
#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"
namespace NLS
{
Editor::Core::Editor::Editor(Context& p_context)
    : m_context(p_context)
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
    NLS::UI::Settings::PanelWindowSettings settings;
    settings.closable = true;
    settings.collapsable = true;
    settings.dockable = true;
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
}

void Editor::Core::Editor::HandleGlobalShortcuts()
{

}

void Editor::Core::Editor::UpdateCurrentEditorMode(float p_deltaTime)
{
    UpdatePlayMode(p_deltaTime);

    {
        m_context.sceneManager.GetCurrentScene()->CollectGarbages();
        m_context.sceneManager.Update();
    }
}

void Editor::Core::Editor::UpdatePlayMode(float p_deltaTime)
{
    auto currentScene = m_context.sceneManager.GetCurrentScene();
    bool simulationApplied = false;

    {
        //simulationApplied = m_context.physicsEngine->Update(p_deltaTime);
    }

    if (simulationApplied)
    {
        currentScene->FixedUpdate(p_deltaTime);
    }

    {
        currentScene->Update(p_deltaTime);
    }

    {
        currentScene->LateUpdate(p_deltaTime);
    }
}

void Editor::Core::Editor::UpdateEditMode(float p_deltaTime)
{
    if (m_context.inputManager->IsKeyPressed(NLS::Windowing::Inputs::EKey::KEY_F5))
    {

    }
}

void Editor::Core::Editor::UpdateEditorPanels(float p_deltaTime)
{

}

void Editor::Core::Editor::RenderViews(float p_deltaTime)
{

}

void Editor::Core::Editor::RenderEditorUI(float p_deltaTime)
{

}

void Editor::Core::Editor::PostUpdate()
{
    m_context.window->SwapBuffers();
    m_context.inputManager->ClearEvents();
    ++m_elapsedFrames;
}
} // namespace NLS
