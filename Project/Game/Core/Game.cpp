#include "Core/Game.h"

#include <Debug/Logger.h>

#ifdef _DEBUG
#include <Rendering/Features/FrameInfoRenderFeature.h>
#endif
#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"

#include "Components/MaterialRenderer.h"
using namespace NLS;
Game::Core::Game::Game(Context & p_context) :
	m_context(p_context),
	m_sceneRenderer(*p_context.driver)
{
	Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<Engine::AssemblyEngine>();
	//m_context.sceneManager.LoadScene(m_context.projectSettings.Get<std::string>("start_scene"));
	m_context.sceneManager.LoadEmptyLightedScene();

    {
        auto& instance = m_context.sceneManager.GetCurrentScene()->CreateGameObject("Cube");

        auto modelRenderer = instance.AddComponent<MeshRenderer>();

        const auto model = m_context.modelManager[":Models\\Cube.fbx"];
        if (model)
            modelRenderer->SetModel(model);
        auto materialRenderer = instance.AddComponent<MaterialRenderer>();
        auto material = new NLS::Rendering::Data::Material(m_context.shaderManager[":Shaders\\Standard.glsl"]);
        if (material)
            materialRenderer->FillWithMaterial(*material);
    }

	m_context.sceneManager.GetCurrentScene()->Play();
}

Game::Core::Game::~Game()
{
	m_context.sceneManager.UnloadCurrentScene();
}

void Game::Core::Game::PreUpdate()
{
	m_context.device->PollEvents();
}

void RenderCurrentScene(
	Engine::Rendering::SceneRenderer& p_renderer,
	const Game::Context& p_context
)
{

	if (auto currentScene = p_context.sceneManager.GetCurrentScene())
	{
		if (auto camera = currentScene->FindMainCamera())
		{
			auto windowSize = p_context.window->GetSize();

			p_renderer.AddDescriptor<Engine::Rendering::SceneRenderer::SceneDescriptor>({
				*currentScene,
			});

			NLS::Rendering::Data::FrameDescriptor frameDescriptor;
			frameDescriptor.renderWidth = windowSize.x;
			frameDescriptor.renderHeight = windowSize.y;
			frameDescriptor.camera = camera->GetCamera();

			p_renderer.BeginFrame(frameDescriptor);
			p_renderer.DrawFrame();
			p_renderer.EndFrame();
		}
	}
}

void Game::Core::Game::Update(float p_deltaTime)
{
	if (auto currentScene = m_context.sceneManager.GetCurrentScene())
	{
		{
			currentScene->Update(p_deltaTime);
			currentScene->LateUpdate(p_deltaTime);
		}
		RenderCurrentScene(m_sceneRenderer, m_context);
	}

	m_context.sceneManager.Update();


	#ifdef _DEBUG
	if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_R))
		NLS::Rendering::Resources::Loaders::ShaderLoader::Recompile(*m_context.shaderManager[":Shaders\\Standard.glsl"], "Data\\Engine\\Shaders\\Standard.glsl");
	#endif
}

void Game::Core::Game::PostUpdate()
{
	m_context.window->SwapBuffers();
	m_context.inputManager->ClearEvents();
}