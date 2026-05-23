#include <Components/CameraComponent.h>
#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/SceneRendererFactory.h>
#include "Panels/GameView.h"
#include "Core/EditorActions.h"
#include "Settings/EditorSettings.h"

using namespace NLS;
Editor::Panels::GameView::GameView
(
	const std::string & p_title,
	bool p_opened,
	const UI::PanelWindowSettings & p_windowSettings
) :
	AView(p_title, p_opened, p_windowSettings),
	m_sceneManager(EDITOR_CONTEXT(sceneManager))
{
    NLS_LOG_INFO("[Startup] Creating Game View renderer");
	m_renderer = Engine::Rendering::CreateSceneRenderer(*EDITOR_CONTEXT(driver));
    SetRequiresRetiredFrameConsumption(true);
	Render::Buffers::UniformBuffer test(1024, 1);
}

void Editor::Panels::GameView::EnsureRenderer()
{
    if (m_renderer != nullptr)
        return;

    NLS_LOG_INFO("[Startup] Creating Game View renderer");
    m_renderer = Engine::Rendering::CreateSceneRenderer(*EDITOR_CONTEXT(driver));
}

Render::Entities::Camera* Editor::Panels::GameView::GetCamera()
{
	if (auto scene = m_sceneManager.GetCurrentScene())
	{
		if (auto camera = scene->FindMainCamera())
		{
			return camera->GetCamera();
		}
	}

	return nullptr;
}

Engine::SceneSystem::Scene* Editor::Panels::GameView::GetScene()
{
	return m_sceneManager.GetCurrentScene();
}

