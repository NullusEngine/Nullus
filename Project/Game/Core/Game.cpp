#include "Core/Game.h"

#include <filesystem>

#include <Debug/Logger.h>

#ifdef _DEBUG
#include <Rendering/Features/FrameInfoRenderFeature.h>
#endif
#include <Rendering/Context/DriverAccess.h>

#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"

using namespace NLS;

Game::Core::Game::Game(Context& p_context) :
	m_context(p_context),
	m_sceneRenderer(Engine::Rendering::CreateSceneRenderer(*p_context.driver))
{
	Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<Engine::AssemblyEngine>();

	const auto startScene = m_context.projectSettings.Get<std::string>("start_scene");
	const auto startScenePath = m_context.projectAssetsPath + startScene;
	if (!startScene.empty() && std::filesystem::exists(startScenePath))
	{
		m_context.sceneManager.LoadScene(startScenePath, true);
	}
	else
	{
		if (!startScene.empty())
			NLS_LOG_WARNING("Start scene not found, loading fallback scene: " + startScenePath);
		m_context.sceneManager.LoadEmptyLightedScene();
	}

	if (auto* scene = m_context.sceneManager.GetCurrentScene())
	{
		scene->Play();
	}
}

Game::Core::Game::~Game()
{
	m_context.sceneManager.UnloadCurrentScene();
}

void Game::Core::Game::PreUpdate()
{
	m_context.device->PollEvents();
}

namespace
{
	void RenderCurrentScene(
		Engine::Rendering::BaseSceneRenderer& p_renderer,
		const Game::Context& p_context
	)
	{
		if (auto currentScene = p_context.sceneManager.GetCurrentScene())
		{
			if (auto camera = currentScene->FindMainCamera())
			{
				auto windowSize = p_context.window->GetSize();

				p_renderer.AddDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
					*currentScene,
					std::nullopt,
					nullptr,
					nullptr,
				});

				NLS::Render::Data::FrameDescriptor frameDescriptor;
				frameDescriptor.renderWidth = windowSize.x;
				frameDescriptor.renderHeight = windowSize.y;
				frameDescriptor.camera = camera->GetCamera();

				p_renderer.BeginFrame(frameDescriptor);
				p_renderer.DrawFrame();
				p_renderer.EndFrame();
			}
			else
			{
				NLS_LOG_ERROR("RenderCurrentScene: no camera found");
			}
		}
		else
		{
			NLS_LOG_ERROR("RenderCurrentScene: no current scene");
		}
	}
}

void Game::Core::Game::Update(float p_deltaTime)
{
	if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_F11))
	{
		if (m_context.inputManager->GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_CONTROL) == Windowing::Inputs::EKeyState::KEY_DOWN)
			Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*m_context.driver);
		else if (Render::Context::DriverUIAccess::IsRenderDocAvailable(*m_context.driver))
			Render::Context::DriverUIAccess::QueueRenderDocCapture(*m_context.driver, "Game");
	}

	if (auto currentScene = m_context.sceneManager.GetCurrentScene())
	{
		currentScene->Update(p_deltaTime);
		currentScene->LateUpdate(p_deltaTime);
		RenderCurrentScene(*m_sceneRenderer, m_context);
	}

	m_context.sceneManager.Update();

#ifdef _DEBUG
	if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_R))
		NLS::Render::Resources::Loaders::ShaderLoader::Recompile(*m_context.shaderManager[":Shaders/Standard.hlsl"], "Data/Engine/Shaders/Standard.hlsl");
#endif
}

void Game::Core::Game::PostUpdate()
{
	Render::Context::DriverUIAccess::PresentSwapchain(*m_context.driver);
	m_context.inputManager->ClearEvents();
}
