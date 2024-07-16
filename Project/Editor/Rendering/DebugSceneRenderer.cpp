#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>
#include <Components/TransformComponent.h>
#include <Rendering/EngineDrawableDescriptor.h>

#include <Rendering/Features/DebugShapeRenderFeature.h>
#include <Rendering/Features/FrameInfoRenderFeature.h>

#include <Debug/Assertion.h>

#include "Rendering/DebugModelRenderFeature.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/GridRenderPass.h"
#include "Rendering/OutlineRenderFeature.h"
#include "Rendering/GizmoRenderFeature.h"
#include "Rendering/PickingRenderPass.h"
#include "Core/EditorResources.h"
//#include "Panels/AView.h"
//#include "Panels/GameView.h"
#include "Settings/EditorSettings.h"

#include "Core/EditorActions.h"
using namespace NLS;
using namespace Maths;
using namespace NLS::Render::Resources;

const Maths::Vector3 kDebugBoundsColor		= { 1.0f, 0.0f, 0.0f };
const Maths::Vector3 kLightVolumeColor		= { 1.0f, 1.0f, 0.0f };
const Maths::Vector3 kColliderColor			= { 0.0f, 1.0f, 0.0f };
const Maths::Vector3 kFrustumColor			= { 1.0f, 1.0f, 1.0f };

const Maths::Vector4 kDefaultOutlineColor{ 1.0f, 0.7f, 0.0f, 1.0f };
const Maths::Vector4 kSelectedOutlineColor{ 1.0f, 1.0f, 0.0f, 1.0f };

constexpr float kDefaultOutlineWidth = 2.5f;
constexpr float kSelectedOutlineWidth = 5.0f;

Maths::Matrix4 CalculateCameraModelMatrix(Engine::GameObject& p_actor)
{
	auto translation = Matrix4::Translation(p_actor.GetTransform()->GetWorldPosition());
    auto rotation = Quaternion::ToMatrix4(p_actor.GetTransform()->GetWorldRotation());
	return translation * rotation;
}

std::optional<std::string> GetLightTypeTextureName(Render::Settings::ELightType type)
{
	using namespace Render::Settings;

	switch (type)
	{
	case ELightType::POINT: return "Bill_Point_Light";
	case ELightType::SPOT: return "Bill_Spot_Light";
	case ELightType::DIRECTIONAL: return "Bill_Directional_Light";
	case ELightType::AMBIENT_BOX: return "Bill_Ambient_Box_Light";
	case ELightType::AMBIENT_SPHERE: return "Bill_Ambient_Sphere_Light";
	}

	return std::nullopt;
}

class DebugCamerasRenderPass : public Render::Core::ARenderPass
{
public:
    DebugCamerasRenderPass(Render::Core::CompositeRenderer& p_renderer)
        : Render::Core::ARenderPass(p_renderer)
	{
		m_cameraMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders/Lambert.glsl"]);
		m_cameraMaterial.Set("u_Diffuse", Vector4(0.0f, 0.3f, 0.7f, 1.0f));
		m_cameraMaterial.Set<Render::Resources::Texture*>("u_DiffuseMap", nullptr);
	}

protected:
	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
        auto& sceneDescriptor = m_renderer.GetDescriptor<Engine::Rendering::SceneRenderer::SceneDescriptor>();

		for (auto camera : sceneDescriptor.scene.GetFastAccessComponents().cameras)
		{
            auto actor = camera->gameobject();

			if (actor->IsActive())
			{
				auto& model = *EDITOR_CONTEXT(editorResources)->GetModel("Camera");
				auto modelMatrix = CalculateCameraModelMatrix(*actor);

				m_renderer.GetFeature<Editor::Rendering::DebugModelRenderFeature>()
					.DrawModelWithSingleMaterial(p_pso, model, m_cameraMaterial, modelMatrix);
			}
		}
	}

private:
    NLS::Render::Resources::Material m_cameraMaterial;
};

class DebugLightsRenderPass : public Render::Core::ARenderPass
{
public:
	DebugLightsRenderPass(Render::Core::CompositeRenderer& p_renderer) : Render::Core::ARenderPass(p_renderer)
	{
		m_lightMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Billboard"));
		m_lightMaterial.Set("u_Diffuse", Vector4(1.f, 1.f, 0.5f, 0.5f));
		m_lightMaterial.SetBackfaceCulling(false);
		m_lightMaterial.SetBlendable(true);
		m_lightMaterial.SetDepthTest(false);
	}

protected:
	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
		auto& sceneDescriptor = m_renderer.GetDescriptor<Engine::Rendering::SceneRenderer::SceneDescriptor>();

		m_lightMaterial.Set<float>("u_Scale", Editor::Settings::EditorSettings::LightBillboardScale * 0.1f);

		for (auto light : sceneDescriptor.scene.GetFastAccessComponents().lights)
		{
			auto actor = light->gameobject();

			if (actor->IsActive())
			{
				auto& model = *EDITOR_CONTEXT(editorResources)->GetModel("Vertical_Plane");
                auto modelMatrix = Maths::Matrix4::Translation(actor->GetTransform()->GetWorldPosition());

				auto lightTypeTextureName = GetLightTypeTextureName(light->GetData()->type);

				auto lightTexture =
					lightTypeTextureName ?
					EDITOR_CONTEXT(editorResources)->GetTexture(lightTypeTextureName.value()) :
					nullptr;

				const auto& lightColor = light->GetColor();
				m_lightMaterial.Set<Render::Resources::Texture*>("u_DiffuseMap", lightTexture);
				m_lightMaterial.Set<Maths::Vector4>("u_Diffuse", Maths::Vector4(lightColor.x, lightColor.y, lightColor.z, 0.75f));

				m_renderer.GetFeature<Editor::Rendering::DebugModelRenderFeature>()
					.DrawModelWithSingleMaterial(p_pso, model, m_lightMaterial, modelMatrix);
			}
		}
	}

private:
    NLS::Render::Resources::Material m_lightMaterial;
};

class DebugActorRenderPass : public Render::Core::ARenderPass
{
public:
	DebugActorRenderPass(Render::Core::CompositeRenderer& p_renderer) : Render::Core::ARenderPass(p_renderer),
		m_debugShapeFeature(m_renderer.GetFeature<Render::Features::DebugShapeRenderFeature>())
	{
		
	}

protected:
	Render::Features::DebugShapeRenderFeature& m_debugShapeFeature;

	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
		auto& debugSceneDescriptor = m_renderer.GetDescriptor<Editor::Rendering::DebugSceneRenderer::DebugSceneDescriptor>();

		if (debugSceneDescriptor.selectedActor)
		{
			auto selectedActor = debugSceneDescriptor.selectedActor;
			DrawActorDebugElements(*selectedActor);
			m_renderer.GetFeature<Editor::Rendering::OutlineRenderFeature>().DrawOutline(*selectedActor, kSelectedOutlineColor, kSelectedOutlineWidth);
			m_renderer.Clear(false, true, false, Maths::Vector3::Zero);
			m_renderer.GetFeature<Editor::Rendering::GizmoRenderFeature>().DrawGizmo(
                selectedActor->GetTransform()->GetWorldPosition(),
                selectedActor->GetTransform()->GetWorldRotation(),
				debugSceneDescriptor.gizmoOperation,
				false,
				debugSceneDescriptor.highlightedGizmoDirection
			);
		}
	}

	void DrawActorDebugElements(Engine::GameObject& p_actor)
	{
		if (p_actor.IsActive())
		{
			/* Render static mesh outline and bounding spheres */
			if (Editor::Settings::EditorSettings::ShowGeometryBounds)
			{
				auto modelRenderer = p_actor.GetComponent<Engine::Components::MeshRenderer>();

				if (modelRenderer && modelRenderer->GetModel())
				{
					DrawBoundingSpheres(*modelRenderer);
				}
			}

			/* Render camera component outline */
            if (auto cameraComponent = p_actor.GetComponent<Engine::Components::CameraComponent>(); cameraComponent)
			{
				auto model = CalculateCameraModelMatrix(p_actor);
				DrawCameraFrustum(*cameraComponent);
			}

// 			/* Render the actor collider */
// 			if (p_actor.GetComponent<Engine::Components::CPhysicalObject>())
// 			{
// 				DrawActorCollider(p_actor);
// 			}

			/* Render the actor ambient light */
            auto plight = p_actor.GetComponent<Engine::Components::LightComponent>();
            if (plight)
			{
                auto type = plight->GetLightType();
                switch (type)
                {
                    case NLS::Render::Settings::ELightType::POINT:
                        break;
                    case NLS::Render::Settings::ELightType::DIRECTIONAL:
                        break;
                    case NLS::Render::Settings::ELightType::SPOT:
                        break;
                    case NLS::Render::Settings::ELightType::AMBIENT_BOX:
                        DrawAmbientBoxVolume(*plight);
                        break;
                    case NLS::Render::Settings::ELightType::AMBIENT_SPHERE:
                        DrawAmbientSphereVolume(*plight);
                        break;
                    default:
                        break;
                }
                if (Editor::Settings::EditorSettings::ShowLightBounds)
                {
                    DrawLightBounds(*plight);
                }
			}



			for (auto& child : p_actor.GetChildren())
			{
				DrawActorDebugElements(*child);
			}
		}
	}

	void DrawFrustumLines(
		const Maths::Vector3& pos,
		const Maths::Vector3& forward,
		float near,
		const float far,
		const Maths::Vector3& a,
		const Maths::Vector3& b,
		const Maths::Vector3& c,
		const Maths::Vector3& d,
		const Maths::Vector3& e,
		const Maths::Vector3& f,
		const Maths::Vector3& g,
		const Maths::Vector3& h
	)
	{
		auto pso = m_renderer.CreatePipelineState();

		// Convenient lambda to draw a frustum line
		auto draw = [&](const Vector3& p_start, const Vector3& p_end, const float planeDistance)
			{
				auto offset = pos + forward * planeDistance;
				auto start = offset + p_start;
				auto end = offset + p_end;
				m_debugShapeFeature.DrawLine(pso, start, end, kFrustumColor);
			};

		// Draw near plane
		draw(a, b, near);
		draw(b, d, near);
		draw(d, c, near);
		draw(c, a, near);

		// Draw far plane
		draw(e, f, far);
		draw(f, h, far);
		draw(h, g, far);
		draw(g, e, far);

		// Draw lines between near and far planes
		draw(a + forward * near, e + forward * far, 0);
		draw(b + forward * near, f + forward * far, 0);
		draw(c + forward * near, g + forward * far, 0);
		draw(d + forward * near, h + forward * far, 0);
	}

	void DrawCameraPerspectiveFrustum(std::pair<uint16_t, uint16_t>& p_size, Engine::Components::CameraComponent& p_camera)
	{
		const auto& owner = *p_camera.gameobject();
		auto& camera = *p_camera.GetCamera();

		const auto& cameraPos = owner.GetTransform()->GetWorldPosition();
        const auto& cameraRotation = owner.GetTransform()->GetWorldRotation();
        const auto& cameraForward = owner.GetTransform()->GetWorldForward();

		camera.CacheMatrices(p_size.first, p_size.second); // TODO: We shouldn't cache matrices mid air, we could use another function to get the matrices/calculate
		const auto proj = Matrix4::Transpose(camera.GetProjectionMatrix());
		const auto near = camera.GetNear();
		const auto far = camera.GetFar();

		const auto nLeft = near * (proj.data[2] - 1.0f) / proj.data[0];
		const auto nRight = near * (1.0f + proj.data[2]) / proj.data[0];
		const auto nTop = near * (1.0f + proj.data[6]) / proj.data[5];
		const auto nBottom = near * (proj.data[6] - 1.0f) / proj.data[5];

		const auto fLeft = far * (proj.data[2] - 1.0f) / proj.data[0];
		const auto fRight = far * (1.0f + proj.data[2]) / proj.data[0];
		const auto fTop = far * (1.0f + proj.data[6]) / proj.data[5];
		const auto fBottom = far * (proj.data[6] - 1.0f) / proj.data[5];

		auto a = cameraRotation * Vector3{ nLeft, nTop, 0 };
		auto b = cameraRotation * Vector3{ nRight, nTop, 0 };
		auto c = cameraRotation * Vector3{ nLeft, nBottom, 0 };
		auto d = cameraRotation * Vector3{ nRight, nBottom, 0 };
		auto e = cameraRotation * Vector3{ fLeft, fTop, 0 };
		auto f = cameraRotation * Vector3{ fRight, fTop, 0 };
		auto g = cameraRotation * Vector3{ fLeft, fBottom, 0 };
		auto h = cameraRotation * Vector3{ fRight, fBottom, 0 };

		DrawFrustumLines(cameraPos, cameraForward, near, far, a, b, c, d, e, f, g, h);
	}

	void DrawCameraOrthographicFrustum(std::pair<uint16_t, uint16_t>& p_size, Engine::Components::CameraComponent& p_camera)
	{
		auto& owner = *p_camera.gameobject();
		auto& camera = *p_camera.GetCamera();
		const auto ratio = p_size.first / static_cast<float>(p_size.second);

		const auto& cameraPos = owner.GetTransform()->GetWorldPosition();
        const auto& cameraRotation = owner.GetTransform()->GetWorldRotation();
        const auto& cameraForward = owner.GetTransform()->GetWorldForward();

		const auto near = camera.GetNear();
		const auto far = camera.GetFar();
		const auto size = p_camera.GetSize();

		const auto right = ratio * size;
		const auto left = -right;
		const auto top = size;
		const auto bottom = -top;

		const auto a = cameraRotation * Vector3{ left, top, 0 };
		const auto b = cameraRotation * Vector3{ right, top, 0 };
		const auto c = cameraRotation * Vector3{ left, bottom, 0 };
		const auto d = cameraRotation * Vector3{ right, bottom, 0 };

		DrawFrustumLines(cameraPos, cameraForward, near, far, a, b, c, d, a, b, c, d);
	}

	void DrawCameraFrustum(Engine::Components::CameraComponent& p_camera)
	{
// 		auto& gameView = EDITOR_PANEL(Editor::Panels::GameView, "Game View");
// 		auto gameViewSize = gameView.GetSafeSize();
// 
// 		if (gameViewSize.first == 0 || gameViewSize.second == 0)
// 		{
// 			gameViewSize = { 16, 9 };
// 		}
// 
// 		switch (p_camera.GetProjectionMode())
// 		{
// 		case Render::Settings::EProjectionMode::ORTHOGRAPHIC:
// 			DrawCameraOrthographicFrustum(gameViewSize, p_camera);
// 			break;
// 
// 		case Render::Settings::EProjectionMode::PERSPECTIVE:
// 			DrawCameraPerspectiveFrustum(gameViewSize, p_camera);
// 			break;
// 		}
	}

	void DrawActorCollider(Engine::GameObject& p_actor)
	{
// 		using namespace Engine::Components;
// 		using namespace OvPhysics::Entities;
// 
// 		auto pso = m_renderer.CreatePipelineState();
// 		pso.depthTest = false;
// 
// 		/* Draw the box collider if any */
// 		if (auto boxColliderComponent = p_actor.GetComponent<Engine::Components::CPhysicalBox>(); boxColliderComponent)
// 		{
// 			m_debugShapeFeature.DrawBox(
// 				pso,
// 				p_actor.transform.GetWorldPosition(),
// 				p_actor.transform.GetWorldRotation(),
// 				boxColliderComponent->GetSize() * p_actor.transform.GetWorldScale(),
// 				Maths::Vector3{ 0.f, 1.f, 0.f },
// 				1.0f
// 			);
// 		}
// 
// 		/* Draw the sphere collider if any */
// 		if (auto sphereColliderComponent = p_actor.GetComponent<Engine::Components::CPhysicalSphere>(); sphereColliderComponent)
// 		{
// 			Vector3 actorScale = p_actor.transform.GetWorldScale();
// 			float radius = sphereColliderComponent->GetRadius() * std::max(std::max(std::max(actorScale.x, actorScale.y), actorScale.z), 0.0f);
// 
// 			m_debugShapeFeature.DrawSphere(
// 				pso,
// 				p_actor.transform.GetWorldPosition(),
// 				p_actor.transform.GetWorldRotation(),
// 				radius,
// 				Maths::Vector3{ 0.f, 1.f, 0.f },
// 				1.0f
// 			);
// 		}
// 
// 		/* Draw the capsule collider if any */
// 		if (auto capsuleColliderComponent = p_actor.GetComponent<Engine::Components::CPhysicalCapsule>(); capsuleColliderComponent)
// 		{
// 			Vector3 actorScale = p_actor.transform.GetWorldScale();
// 			float radius = abs(capsuleColliderComponent->GetRadius() * std::max(std::max(actorScale.x, actorScale.z), 0.f));
// 			float height = abs(capsuleColliderComponent->GetHeight() * actorScale.y);
// 
// 			m_debugShapeFeature.DrawCapsule(
// 				pso,
// 				p_actor.transform.GetWorldPosition(),
// 				p_actor.transform.GetWorldRotation(),
// 				radius,
// 				height,
// 				Maths::Vector3{ 0.f, 1.f, 0.f },
// 				1.0f
// 			);
// 		}
	}

	void DrawLightBounds(Engine::Components::LightComponent& p_light)
	{
		auto pso = m_renderer.CreatePipelineState();
		pso.depthTest = false;

		auto& data = *p_light.GetData();

		m_debugShapeFeature.DrawSphere(
			pso,
			data.transform->GetWorldPosition(),
			data.transform->GetWorldRotation(),
			data.GetEffectRange(),
			kDebugBoundsColor,
			1.0f
		);
	}

	void DrawAmbientBoxVolume(Engine::Components::LightComponent& p_ambientBoxLight)
	{
		auto pso = m_renderer.CreatePipelineState();
		pso.depthTest = false;

		auto& data = *p_ambientBoxLight.GetData();

		m_debugShapeFeature.DrawBox(
			pso,
            p_ambientBoxLight.gameobject()->GetTransform()->GetWorldPosition(),
			data.transform->GetWorldRotation(),
			{ data.constant, data.linear, data.quadratic },
			data.GetEffectRange(),
			1.0f
		);
	}

	void DrawAmbientSphereVolume(Engine::Components::LightComponent& p_ambientSphereLight)
	{
		auto pso = m_renderer.CreatePipelineState();
		pso.depthTest = false;

		auto& data = *p_ambientSphereLight.GetData();

		m_debugShapeFeature.DrawSphere(
			pso,
            p_ambientSphereLight.gameobject()->GetTransform()->GetWorldPosition(),
            p_ambientSphereLight.gameobject()->GetTransform()->GetWorldRotation(),
			data.constant,
			kLightVolumeColor,
			1.0f
		);
	}

	void DrawBoundingSpheres(Engine::Components::MeshRenderer& p_modelRenderer)
	{
		using namespace Engine::Components;

		auto pso = m_renderer.CreatePipelineState();
		pso.depthTest = false;

		/* Draw the sphere collider if any */
		if (auto model = p_modelRenderer.GetModel())
		{
			auto& actor = *p_modelRenderer.gameobject();

			Maths::Vector3 actorScale = actor.GetTransform()->GetWorldScale();
            Maths::Quaternion actorRotation = actor.GetTransform()->GetWorldRotation();
            Maths::Vector3 actorPosition = actor.GetTransform()->GetWorldPosition();

			const auto& modelBoundingsphere =
				p_modelRenderer.GetFrustumBehaviour() == Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM ?
				p_modelRenderer.GetCustomBoundingSphere() :
				model->GetBoundingSphere();

			float radiusScale = std::max(std::max(std::max(actorScale.x, actorScale.y), actorScale.z), 0.0f);
			float scaledRadius = modelBoundingsphere.radius * radiusScale;
			auto sphereOffset = Maths::Quaternion::RotatePoint(modelBoundingsphere.position, actorRotation) * radiusScale;

			m_debugShapeFeature.DrawSphere(
				pso,
				actorPosition + sphereOffset,
				actorRotation,
				scaledRadius,
				kDebugBoundsColor,
				1.0f
			);

			if (p_modelRenderer.GetFrustumBehaviour() == Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MESHES)
			{
				const auto& meshes = model->GetMeshes();

				if (meshes.size() > 1) // One mesh would result into the same bounding sphere for mesh and model
				{
					for (auto mesh : meshes)
					{
						auto& meshBoundingSphere = mesh->GetBoundingSphere();
						float scaledRadius = meshBoundingSphere.radius * radiusScale;
						auto sphereOffset = Maths::Quaternion::RotatePoint(meshBoundingSphere.position, actorRotation) * radiusScale;

						m_debugShapeFeature.DrawSphere(
							pso,
							actorPosition + sphereOffset,
							actorRotation,
							scaledRadius,
							kDebugBoundsColor,
							1.0f
						);
					}
				}
			}
		}
	}
};

Editor::Rendering::DebugSceneRenderer::DebugSceneRenderer(NLS::Render::Context::Driver& p_driver) :
	Engine::Rendering::SceneRenderer(p_driver)
{
    AddFeature<NLS::Render::Features::FrameInfoRenderFeature>();
    AddFeature<NLS::Render::Features::DebugShapeRenderFeature>();
	AddFeature<Editor::Rendering::DebugModelRenderFeature>();
	AddFeature<Editor::Rendering::OutlineRenderFeature>();
	AddFeature<Editor::Rendering::GizmoRenderFeature>();

	AddPass<GridRenderPass>("Grid", NLS::Render::Settings::ERenderPassOrder::Opaque - 1);
    AddPass<DebugCamerasRenderPass>("Debug Cameras", NLS::Render::Settings::ERenderPassOrder::Transparent + 1);
    AddPass<DebugLightsRenderPass>("Debug Lights", NLS::Render::Settings::ERenderPassOrder::Transparent + 2);
    AddPass<DebugActorRenderPass>("Debug Actor", NLS::Render::Settings::ERenderPassOrder::Transparent + 3);
    AddPass<PickingRenderPass>("Picking", NLS::Render::Settings::ERenderPassOrder::PostProcessing + 1);
}
