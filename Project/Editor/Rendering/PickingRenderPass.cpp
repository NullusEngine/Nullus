#include "Rendering/DebugModelRenderFeature.h"
#include "Rendering/PickingRenderPass.h"
#include "Core/EditorActions.h"
#include "Settings/EditorSettings.h"
#include "Rendering/DebugSceneRenderer.h"
#include <Components/TransformComponent.h>
#include <Components/MaterialRenderer.h>
#include <Rendering/EngineDrawableDescriptor.h>
using namespace NLS;
Editor::Rendering::PickingRenderPass::PickingRenderPass(NLS::Render::Core::CompositeRenderer& p_renderer) :
	NLS::Render::Core::ARenderPass(p_renderer)
{
	/* Light Material */
	m_lightMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Billboard"));
	m_lightMaterial.SetDepthTest(true);

	/* Gizmo Pickable Material */
	m_gizmoPickingMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Gizmo"));
	m_gizmoPickingMaterial.SetGPUInstances(3);
	m_gizmoPickingMaterial.Set("u_IsBall", false);
	m_gizmoPickingMaterial.Set("u_IsPickable", true);

	/* Picking Material */
	m_actorPickingMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders\\Unlit.glsl"]);
	m_actorPickingMaterial.Set("u_Diffuse", Maths::Vector4(1.f, 1.f, 1.f, 1.0f));
	m_actorPickingMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", nullptr);
	m_actorPickingMaterial.SetFrontfaceCulling(false);
	m_actorPickingMaterial.SetBackfaceCulling(false);
}

Editor::Rendering::PickingRenderPass::PickingResult Editor::Rendering::PickingRenderPass::ReadbackPickingResult(
	const Engine::SceneSystem::Scene& p_scene,
	uint32_t p_x,
	uint32_t p_y
)
{
	uint8_t pixel[3];

	m_actorPickingFramebuffer.Bind();

	auto pso = m_renderer.CreatePipelineState();

	m_renderer.ReadPixels(
		p_x, p_y, 1, 1,
		NLS::Render::Settings::EPixelDataFormat::RGB,
		NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
		pixel
	);

	m_actorPickingFramebuffer.Unbind();

	uint32_t actorID = (0 << 24) | (pixel[2] << 16) | (pixel[1] << 8) | (pixel[0] << 0);
	auto actorUnderMouse = p_scene.FindActorByID(actorID);

	if (actorUnderMouse)
	{
        return actorUnderMouse;
	}
	else if (
		pixel[0] == 255 &&
		pixel[1] == 255 &&
		pixel[2] >= 252 &&
		pixel[2] <= 254
		)
	{
		return static_cast<Editor::Core::GizmoBehaviour::EDirection>(pixel[2] - 252);
	}

	return std::nullopt;
}

void Editor::Rendering::PickingRenderPass::Draw(NLS::Render::Data::PipelineState p_pso)
{
	// TODO: Make sure we only renderer when the view is hovered and not being resized

	using namespace Engine::Rendering;

	NLS_ASSERT(m_renderer.HasDescriptor<SceneRenderer::SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");
	NLS_ASSERT(m_renderer.HasDescriptor<DebugSceneRenderer::DebugSceneDescriptor>(), "Cannot find DebugSceneDescriptor attached to this renderer");

	auto& sceneDescriptor = m_renderer.GetDescriptor<SceneRenderer::SceneDescriptor>();
	auto& debugSceneDescriptor = m_renderer.GetDescriptor<DebugSceneRenderer::DebugSceneDescriptor>();
	auto& frameDescriptor = m_renderer.GetFrameDescriptor();
	auto& scene = sceneDescriptor.scene;

	m_actorPickingFramebuffer.Resize(frameDescriptor.renderWidth, frameDescriptor.renderHeight);

	m_actorPickingFramebuffer.Bind();
	
	auto pso = m_renderer.CreatePipelineState();

	m_renderer.Clear(true, true, true);

	DrawPickableModels(pso, scene);
	DrawPickableCameras(pso, scene);
	DrawPickableLights(pso, scene);

	if (debugSceneDescriptor.selectedActor)
	{
		auto& selectedActor = *debugSceneDescriptor.selectedActor;

		DrawPickableGizmo(
			pso,
            selectedActor.GetTransform()->GetWorldPosition(),
            selectedActor.GetTransform()->GetWorldRotation(),
			debugSceneDescriptor.gizmoOperation
		);
	}

	m_actorPickingFramebuffer.Unbind();

	if (auto output = frameDescriptor.outputBuffer)
	{
		output->Bind();
	}
}

void PreparePickingMaterial(Engine::GameObject& p_actor, NLS::Render::Resources::Material& p_material)
{
	uint32_t actorID = static_cast<uint32_t>(p_actor.GetWorldID());

	auto bytes = reinterpret_cast<uint8_t*>(&actorID);
	auto color = Maths::Vector4{ bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, 1.0f };

	p_material.Set("u_Diffuse", color);
}

void Editor::Rendering::PickingRenderPass::DrawPickableModels(
	NLS::Render::Data::PipelineState p_pso,
	Engine::SceneSystem::Scene& p_scene
)
{
	for (auto modelRenderer : p_scene.GetFastAccessComponents().modelRenderers)
	{
		auto& actor = *modelRenderer->gameobject();

		if (actor.IsActive())
		{
			if (auto model = modelRenderer->GetModel())
			{
                if (auto materialRenderer = modelRenderer->gameobject()->GetComponent<Engine::Components::MaterialRenderer>())
				{
					const auto& materials = materialRenderer->GetMaterials();
					const auto& modelMatrix = actor.GetTransform()->GetWorldMatrix();

					PreparePickingMaterial(actor, m_actorPickingMaterial);

					for (auto mesh : model->GetMeshes())
					{
						auto stateMask = m_actorPickingMaterial.GenerateStateMask();

						// Override the state mask to use the material state mask (if this one is valid)
						if (auto material = materials.at(mesh->GetMaterialIndex()); material && material->IsValid())
						{
							stateMask = material->GenerateStateMask();
						}

						NLS::Render::Entities::Drawable drawable;
						drawable.mesh = mesh;
						drawable.material = &m_actorPickingMaterial;
						drawable.stateMask = stateMask;

						drawable.AddDescriptor<Engine::Rendering::EngineDrawableDescriptor>({
							modelMatrix
						});

						m_renderer.DrawEntity(p_pso, drawable);
					}
				}
			}
		}
	}
}

void Editor::Rendering::PickingRenderPass::DrawPickableCameras(
	NLS::Render::Data::PipelineState p_pso,
	Engine::SceneSystem::Scene& p_scene
)
{
	for (auto camera : p_scene.GetFastAccessComponents().cameras)
	{
		auto& actor = *camera->gameobject();

		if (actor.IsActive())
		{
			PreparePickingMaterial(actor, m_actorPickingMaterial);
			auto& cameraModel = *EDITOR_CONTEXT(editorResources)->GetModel("Camera");
            auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
            auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
			auto modelMatrix = translation * rotation;

			m_renderer.GetFeature<DebugModelRenderFeature>()
				.DrawModelWithSingleMaterial(p_pso, cameraModel, m_actorPickingMaterial, modelMatrix);
		}
	}
}

void Editor::Rendering::PickingRenderPass::DrawPickableLights(
	NLS::Render::Data::PipelineState p_pso,
	Engine::SceneSystem::Scene& p_scene
)
{
	if (Settings::EditorSettings::LightBillboardScale > 0.001f)
	{
		m_renderer.Clear(false, true, false);

		m_lightMaterial.Set<float>("u_Scale", Settings::EditorSettings::LightBillboardScale * 0.1f);

		for (auto light : p_scene.GetFastAccessComponents().lights)
		{
			auto& actor = *light->gameobject();

			if (actor.IsActive())
			{
				PreparePickingMaterial(actor, m_lightMaterial);
				auto& lightModel = *EDITOR_CONTEXT(editorResources)->GetModel("Vertical_Plane");
                auto modelMatrix = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());

				m_renderer.GetFeature<DebugModelRenderFeature>()
					.DrawModelWithSingleMaterial(p_pso, lightModel, m_lightMaterial, modelMatrix);
			}
		}
	}
}

void Editor::Rendering::PickingRenderPass::DrawPickableGizmo(
	NLS::Render::Data::PipelineState p_pso,
	const Maths::Vector3& p_position,
	const Maths::Quaternion& p_rotation,
	Editor::Core::EGizmoOperation p_operation
)
{
	auto modelMatrix =
		Maths::Matrix4::Translation(p_position) *
		Maths::Quaternion::ToMatrix4(Maths::Quaternion::Normalize(p_rotation));

	auto arrowModel = EDITOR_CONTEXT(editorResources)->GetModel("Arrow_Picking");

	m_renderer.GetFeature<DebugModelRenderFeature>()
		.DrawModelWithSingleMaterial(p_pso, *arrowModel, m_gizmoPickingMaterial, modelMatrix);
}
