#include "Core/Game.h"

#include <Debug/Logger.h>
#include <Image.h>
#include <vector>
#include <Math/Matrix4.h>

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
#include "Components/TransformComponent.h"
using namespace NLS;

namespace
{
    NLS::Render::Resources::Model* g_debugModel = nullptr;
    NLS::Render::Resources::Material* g_debugMaterial = nullptr;
}

Game::Core::Game::Game(Context & p_context) :
	m_context(p_context),
	m_sceneRenderer(*p_context.driver)
{
	Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<Engine::AssemblyEngine>();
	//m_context.sceneManager.LoadScene(m_context.projectSettings.Get<std::string>("start_scene"));
	m_context.sceneManager.LoadEmptyLightedScene();

    {
        auto& instance = m_context.sceneManager.GetCurrentScene()->CreateGameObject("Cube");

        auto meshObj = instance.AddComponent(UDRefl::Type_of<Engine::Components::MeshRenderer>);
        auto modelRenderer = meshObj.StaticCast(UDRefl::Type_of<Engine::Components::MeshRenderer>).AsPtr<Engine::Components::MeshRenderer>();

        if (!modelRenderer)
        {
            NLS_LOG_ERROR("Failed to resolve MeshRenderer from reflection component");
        }

        const auto model = m_context.modelManager[":Models/Cube.fbx"];
        if (model && modelRenderer)
        {
            modelRenderer->SetModel(model);
            modelRenderer->SetFrustumBehaviour(Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            g_debugModel = model;
            NLS_LOG_INFO("Cube MeshRenderer ptr=" + std::to_string(reinterpret_cast<uintptr_t>(modelRenderer)));
            NLS_LOG_INFO("Cube model loaded: meshes=" + std::to_string(model->GetMeshes().size()) +
                ", boundRadius=" + std::to_string(model->GetBoundingSphere().radius));
            for (const auto* mesh : model->GetMeshes())
            {
                NLS_LOG_INFO("Cube mesh materialIndex=" + std::to_string(mesh->GetMaterialIndex()));
            }
        }
        else
        {
            NLS_LOG_ERROR("Failed to load model :Models/Cube.fbx");
        }

        if (auto tr = instance.GetTransform())
        {
            tr->SetLocalPosition({0.0f, 0.0f, 5.0f});
            tr->SetLocalScale({2.0f, 2.0f, 2.0f});
        }

        auto matObj = instance.AddComponent(UDRefl::Type_of<Engine::Components::MaterialRenderer>);
        auto materialRenderer = matObj.StaticCast(UDRefl::Type_of<Engine::Components::MaterialRenderer>).AsPtr<Engine::Components::MaterialRenderer>();
        if (!materialRenderer)
        {
            NLS_LOG_ERROR("Failed to resolve MaterialRenderer from reflection component");
        }

        auto* standardShader = m_context.shaderManager[":Shaders/Unlit.glsl"];
        if (!standardShader)
        {
            NLS_LOG_ERROR("Failed to load shader :Shaders/Unlit.glsl");
        }
        else
        {
            NLS_LOG_INFO("Unlit shader uniforms: " + std::to_string(standardShader->uniforms.size()));
        }

        auto material = new NLS::Render::Resources::Material(standardShader);
        if (material && materialRenderer)
        {
            material->Set("u_Diffuse", NLS::Maths::Vector4(0.95f, 0.35f, 0.2f, 1.0f));
            // Unlit.glsl always samples u_DiffuseMap; bind nullptr so the engine uses its fallback white texture.
            material->Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", nullptr);
            material->SetBackfaceCulling(false);
            material->SetFrontfaceCulling(false);
            materialRenderer->FillWithMaterial(*material);
            g_debugMaterial = material;
            NLS_LOG_INFO("Cube material valid=" + std::string(material->IsValid() ? "true" : "false"));
        }
    }

	auto* scene = m_context.sceneManager.GetCurrentScene();
	if (scene)
	{
		const auto& fac = scene->GetFastAccessComponents();
		NLS_LOG_INFO("Scene FAC - cameras=" + std::to_string(fac.cameras.size()) +
			", lights=" + std::to_string(fac.lights.size()) +
			", meshRenderers=" + std::to_string(fac.modelRenderers.size()) +
			", skyboxes=" + std::to_string(fac.skyboxs.size()));
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

void RenderCurrentScene(
	Engine::Rendering::SceneRenderer& p_renderer,
	const Game::Context& p_context,
	bool& p_captureFrame
)
{
	if (auto currentScene = p_context.sceneManager.GetCurrentScene())
	{
		if (auto camera = currentScene->FindMainCamera())
		{
			auto windowSize = p_context.window->GetSize();

			p_renderer.AddDescriptor<Engine::Rendering::SceneRenderer::SceneDescriptor>({
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

            // Debug path: force one direct model draw to isolate Scene parsing issues.
            if (g_debugModel && g_debugMaterial)
            {
                p_renderer.DrawModelWithSingleMaterial({}, *g_debugModel, *g_debugMaterial, Maths::Matrix4::Translation({0.0f, 0.0f, 5.0f}));
            }

			if (!p_captureFrame)
			{
				std::vector<unsigned char> pixels(windowSize.x * windowSize.y * 3);
				p_renderer.ReadPixels(0, 0, windowSize.x, windowSize.y,
					NLS::Render::Settings::EPixelDataFormat::RGB,
					NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
					pixels.data());
				NLS::Image image(static_cast<int>(windowSize.x), static_cast<int>(windowSize.y), 3);
				image.SetData(pixels.data());
				image.Save("frame_capture.png");
				p_captureFrame = true;
			}

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
		RenderCurrentScene(m_sceneRenderer, m_context, m_frameCaptured);
	}

	m_context.sceneManager.Update();


	#ifdef _DEBUG
	if (m_context.inputManager->IsKeyPressed(Windowing::Inputs::EKey::KEY_R))
		NLS::Render::Resources::Loaders::ShaderLoader::Recompile(*m_context.shaderManager[":Shaders/Standard.glsl"], "Data/Engine/Shaders/Standard.glsl");
	#endif
}

void Game::Core::Game::PostUpdate()
{
	m_context.window->SwapBuffers();
	m_context.inputManager->ClearEvents();
}