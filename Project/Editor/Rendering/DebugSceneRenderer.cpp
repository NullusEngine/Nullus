#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>
#include <Components/TransformComponent.h>
#include <Rendering/Debug/DebugDrawPass.h>
#include <Rendering/Debug/DebugDrawGeometry.h>
#include <Rendering/Debug/DebugDrawService.h>
#include <Rendering/EngineDrawableDescriptor.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/FrameGraph/FrameGraphExecutionPlan.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <Rendering/Settings/GraphicsBackendUtils.h>

#include <Debug/Assertion.h>

#include "Rendering/DebugModelRenderer.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/EditorDefaultResources.h"
#include "Rendering/EditorPipelineStatePresets.h"
#include "Rendering/GridRenderPass.h"
#include "Rendering/OutlineRenderer.h"
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

namespace
{
	std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> BuildDebugDeferredThreadedPassMetadata(
		const uint64_t helperVisibleCount,
        const bool includeGridPass,
        const bool includeSelectionPass,
        const bool includePickingPass)
	{
		std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> metadata;
		metadata.reserve(
            (includeGridPass ? 1u : 0u) +
            (includeSelectionPass ? 1u : 0u) +
            1u +
            (includePickingPass ? 1u : 0u));

        if (includeGridPass)
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
            gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
            gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            gridMetadata.visibleDrawCountContribution = 1u;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
                gridMetadata,
                "EditorGridPass");
            metadata.push_back(std::move(gridMetadata));
        }

        if (includeSelectionPass)
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata selectionMetadata;
            selectionMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            selectionMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
            selectionMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            selectionMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            selectionMetadata.visibleDrawCountContribution = 2u;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
                selectionMetadata,
                "EditorSelectionPass");
            metadata.push_back(std::move(selectionMetadata));
        }

		NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
		helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
		helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
		helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
		helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
		helperMetadata.visibleDrawCountContribution = helperVisibleCount;
		NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
			helperMetadata,
			"EditorHelperPass");
		metadata.push_back(std::move(helperMetadata));

        if (includePickingPass)
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata pickingMetadata;
            pickingMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            pickingMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary;
            pickingMetadata.executionMode = NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded;
            pickingMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            pickingMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            pickingMetadata.visibleDrawCountContribution = 0u;
            pickingMetadata.propagatesColorOutput = false;
            pickingMetadata.propagatesDepthOutput = false;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
                pickingMetadata,
                "EditorPickingPass");
            metadata.push_back(std::move(pickingMetadata));
        }

		return metadata;
	}

	std::vector<NLS::Render::Context::RenderPassCommandInput> BuildDebugDeferredAppendedPassInputs(
        const std::optional<NLS::Render::Context::RenderPassCommandInput>& gridPassInput,
        const std::optional<NLS::Render::Context::RenderPassCommandInput>& selectionPassInput,
        const std::optional<NLS::Render::Context::RenderPassCommandInput>& pickingPassInput)
	{
		std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
		passInputs.reserve(
            (gridPassInput.has_value() ? 1u : 0u) +
            (selectionPassInput.has_value() ? 1u : 0u) +
            (pickingPassInput.has_value() ? 1u : 0u));

        if (gridPassInput.has_value())
            passInputs.push_back(*gridPassInput);

        if (selectionPassInput.has_value())
            passInputs.push_back(*selectionPassInput);

        if (pickingPassInput.has_value())
            passInputs.push_back(*pickingPassInput);

		return passInputs;
	}

	bool IsEditorDebugPassEnabled(const char* /*name*/)
	{
		// Debug passes are controlled via EditorSettings, not command-line diagnostics
		// This function is kept for potential future use
		return true;
	}

	bool ShouldDisableEditorGridPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableGridPass;
	}

	bool ShouldDisableDebugCamerasPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableDebugCamerasPass;
	}

	bool ShouldDisableDebugLightsPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableDebugLightsPass;
	}

	bool ShouldDisableDebugActorPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableDebugActorPass;
	}

	bool ShouldDisableDebugDrawPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableDebugDrawPass;
	}

	bool ShouldDisablePickingPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisablePickingPass;
	}
}

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
        , m_debugModelRenderer(p_renderer)
	{
		m_cameraMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("DebugLitColor"));
		m_cameraMaterial.Set("u_Diffuse", Vector4(0.0f, 0.3f, 0.7f, 1.0f));
	}

protected:
	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
        const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
        if (!debugSettings.debugDrawEnabled || !debugSettings.debugDrawCamera)
            return;

        auto& sceneDescriptor = m_renderer.GetDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>();

		for (auto camera : sceneDescriptor.scene.GetFastAccessComponents().cameras)
		{
            auto actor = camera->gameobject();

			if (actor->IsActive())
			{
				auto& model = *EDITOR_CONTEXT(editorResources)->GetModel("Camera");
				auto modelMatrix = CalculateCameraModelMatrix(*actor);

				m_debugModelRenderer.DrawModelWithSingleMaterial(p_pso, model, m_cameraMaterial, modelMatrix);
			}
		}
	}

private:
    Editor::Rendering::DebugModelRenderer m_debugModelRenderer;
    NLS::Render::Resources::Material m_cameraMaterial;
};

class DebugLightsRenderPass : public Render::Core::ARenderPass
{
public:
	DebugLightsRenderPass(Render::Core::CompositeRenderer& p_renderer)
        : Render::Core::ARenderPass(p_renderer)
        , m_debugModelRenderer(p_renderer)
	{
		m_lightMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Billboard"));
		m_lightMaterial.Set("u_Diffuse", Vector4(1.f, 1.f, 0.5f, 0.5f));
		m_lightMaterial.Set("u_TextureTiling", Vector2(1.0f, 1.0f));
		m_lightMaterial.Set("u_TextureOffset", Vector2(0.0f, 0.0f));
		m_lightMaterial.SetBlendable(true);
	}

protected:
	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
        const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
        if (!debugSettings.debugDrawEnabled || !debugSettings.debugDrawLighting)
            return;

		auto& sceneDescriptor = m_renderer.GetDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>();
		p_pso = Editor::Rendering::CreateEditorTransparentOverlayPipelineState(p_pso);

		m_lightMaterial.Set<float>("u_Scale", debugSettings.lightBillboardScale * 0.1f);

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
				m_lightMaterial.Set<Render::Resources::Texture2D*>("u_DiffuseMap", lightTexture);
				m_lightMaterial.Set<Maths::Vector4>("u_Diffuse", Maths::Vector4(lightColor.x, lightColor.y, lightColor.z, 0.75f));

				m_debugModelRenderer.DrawModelWithSingleMaterial(p_pso, model, m_lightMaterial, modelMatrix);
			}
		}
	}

private:
    Editor::Rendering::DebugModelRenderer m_debugModelRenderer;
    NLS::Render::Resources::Material m_lightMaterial;
};

class DebugActorRenderPass : public Render::Core::ARenderPass
{
public:
	DebugActorRenderPass(Render::Core::CompositeRenderer& p_renderer)
        : Render::Core::ARenderPass(p_renderer)
        , m_debugModelRenderer(p_renderer)
        , m_outlineRenderer(p_renderer, m_debugModelRenderer)
	{
		
	}

    std::optional<NLS::Render::Context::RenderPassCommandInput> GetPreparedThreadedPassInput() const
    {
        return m_preparedThreadedPassInput;
    }

protected:
    void OnBeginFrame(const NLS::Render::Data::FrameDescriptor&) override
    {
        m_preparedThreadedPassInput.reset();
    }

	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
		auto& debugSceneDescriptor = m_renderer.GetDescriptor<Editor::Rendering::DebugSceneRenderer::DebugSceneDescriptor>();
        auto* selectedActor = debugSceneDescriptor.selectedActor;

		if (selectedActor == nullptr)
            return;

        if (Editor::Rendering::OutlineRenderer::ShouldIncludeInThreadedFrame(true, selectedActor))
		{
			DrawActorDebugElements(*selectedActor);

            if (NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver()))
            {
                m_preparedThreadedPassInput = BuildThreadedPassInput(*selectedActor, debugSceneDescriptor, p_pso);
                return;
            }

            m_outlineRenderer.DrawOutline(*selectedActor, kSelectedOutlineColor, kSelectedOutlineWidth);
		}
	}

	void DrawActorDebugElements(Engine::GameObject& p_actor)
	{
		if (p_actor.IsActive())
		{
			ApplyDebugDrawSettings();

			/* Render static mesh outline and bounding spheres */
			if (Editor::Settings::EditorSettings::GetDebugDrawSettingsObject().debugDrawBounds)
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
				DrawLightVolume(*plight);
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
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = Render::Debug::DebugDrawCategory::Camera;
		options.style.color = kFrustumColor;
		options.style.lineWidth = 1.0f;
		options.style.depthMode = Render::Debug::DebugDrawDepthMode::AlwaysOnTop;

		const std::array<Vector3, 8u> corners = {
			pos + forward * near + a,
			pos + forward * near + b,
			pos + forward * near + c,
			pos + forward * near + d,
			pos + forward * far + e,
			pos + forward * far + f,
			pos + forward * far + g,
			pos + forward * far + h
		};

		Render::Debug::SubmitFrustum(GetDebugDrawService(), corners, options);
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
		const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
		std::pair<uint16_t, uint16_t> viewSize = {
			frameDescriptor.renderWidth != 0 ? frameDescriptor.renderWidth : 16,
			frameDescriptor.renderHeight != 0 ? frameDescriptor.renderHeight : 9
		};

		switch (p_camera.GetProjectionMode())
		{
		case Render::Settings::EProjectionMode::ORTHOGRAPHIC:
			DrawCameraOrthographicFrustum(viewSize, p_camera);
			break;

		case Render::Settings::EProjectionMode::PERSPECTIVE:
			DrawCameraPerspectiveFrustum(viewSize, p_camera);
			break;
		}
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
// 			SubmitBox(
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
// 			SubmitSphere(
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
// 			SubmitCapsule(
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

	void DrawLightVolume(Engine::Components::LightComponent& p_light)
	{
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = Render::Debug::DebugDrawCategory::Lighting;
		options.style.color = kLightVolumeColor;
		options.style.lineWidth = 1.0f;
		options.style.depthMode = Render::Debug::DebugDrawDepthMode::AlwaysOnTop;

		Render::Debug::SubmitLightVolume(GetDebugDrawService(), *p_light.GetData(), options);
	}

	void DrawBoundingSpheres(Engine::Components::MeshRenderer& p_modelRenderer)
	{
		using namespace Engine::Components;

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

			SubmitSphere(
				actorPosition + sphereOffset,
				actorRotation,
				scaledRadius,
				kDebugBoundsColor,
				1.0f,
				Render::Debug::DebugDrawCategory::Bounds
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

						SubmitSphere(
							actorPosition + sphereOffset,
							actorRotation,
							scaledRadius,
							kDebugBoundsColor,
							1.0f,
							Render::Debug::DebugDrawCategory::Bounds
						);
					}
				}
			}
		}
	}

private:
	void ApplyDebugDrawSettings()
	{
		auto& debugDrawService = GetDebugDrawService();
        const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
		debugDrawService.SetEnabled(debugSettings.debugDrawEnabled);
		debugDrawService.SetCategoryEnabled(Render::Debug::DebugDrawCategory::Grid, debugSettings.debugDrawGrid);
		debugDrawService.SetCategoryEnabled(Render::Debug::DebugDrawCategory::Bounds, debugSettings.debugDrawBounds);
		debugDrawService.SetCategoryEnabled(Render::Debug::DebugDrawCategory::Camera, debugSettings.debugDrawCamera);
		debugDrawService.SetCategoryEnabled(Render::Debug::DebugDrawCategory::Lighting, debugSettings.debugDrawLighting);
	}

    Render::Debug::DebugDrawService& GetDebugDrawService()
    {
        auto* debugDrawService = m_renderer.GetDebugDrawService();
        NLS_ASSERT(debugDrawService != nullptr, "Cannot find DebugDrawService attached to this renderer");
        return *debugDrawService;
    }

    void SubmitLine(
        const Maths::Vector3& start,
        const Maths::Vector3& end,
        const Maths::Vector3& color,
        const float lineWidth = 1.0f,
		const Render::Debug::DebugDrawCategory category = Render::Debug::DebugDrawCategory::General)
    {
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = category;
		options.style.color = color;
		options.style.lineWidth = lineWidth;
        GetDebugDrawService().SubmitLine(start, end, options);
    }

    void SubmitBox(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        const Maths::Vector3& size,
        const Maths::Vector3& color,
        const float lineWidth = 1.0f,
		const Render::Debug::DebugDrawCategory category = Render::Debug::DebugDrawCategory::General)
    {
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = category;
		options.style.color = color;
		options.style.lineWidth = lineWidth;
        Render::Debug::SubmitBox(GetDebugDrawService(), position, rotation, size, options);
    }

    void SubmitSphere(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        const float radius,
        const Maths::Vector3& color,
        const float lineWidth = 1.0f,
		const Render::Debug::DebugDrawCategory category = Render::Debug::DebugDrawCategory::General)
    {
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = category;
		options.style.color = color;
		options.style.lineWidth = lineWidth;
        Render::Debug::SubmitSphere(GetDebugDrawService(), position, rotation, radius, options);
    }

    void SubmitCapsule(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        const float radius,
        const float height,
        const Maths::Vector3& color,
        const float lineWidth = 1.0f,
		const Render::Debug::DebugDrawCategory category = Render::Debug::DebugDrawCategory::General)
    {
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = category;
		options.style.color = color;
		options.style.lineWidth = lineWidth;
        Render::Debug::SubmitCapsule(GetDebugDrawService(), position, rotation, radius, height, options);
    }

    std::optional<NLS::Render::Context::RenderPassCommandInput> BuildThreadedPassInput(
        Engine::GameObject& selectedActor,
        const Editor::Rendering::DebugSceneRenderer::DebugSceneDescriptor& debugSceneDescriptor,
        NLS::Render::Data::PipelineState p_pso)
    {
        const auto& frameDescriptor = m_renderer.GetFrameDescriptor();

        NLS::Render::Context::RenderPassCommandInput passInput;
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
        passInput.debugName = "EditorSelectionPass";
        passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
        passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
        passInput.requiresFrameData = true;
        passInput.requiresObjectData = true;
        passInput.targetsSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor);
        passInput.renderWidth = frameDescriptor.renderWidth;
        passInput.renderHeight = frameDescriptor.renderHeight;
        passInput.usesColorAttachment = true;
        passInput.usesDepthStencilAttachment = true;

        m_outlineRenderer.CaptureOutlineDrawCommands(
            selectedActor,
            kSelectedOutlineColor,
            kSelectedOutlineWidth,
            passInput.recordedDrawCommands);

        passInput.drawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
        if (passInput.drawCount == 0u)
            return std::nullopt;

        return passInput;
    }

private:
    Editor::Rendering::DebugModelRenderer m_debugModelRenderer;
    Editor::Rendering::OutlineRenderer m_outlineRenderer;
    std::optional<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInput;
};

Editor::Rendering::DebugSceneRenderer::DebugSceneRenderer(NLS::Render::Context::Driver& p_driver) :
	Engine::Rendering::DeferredSceneRenderer(p_driver)
{
    SetDebugDrawService(std::make_unique<NLS::Render::Debug::DebugDrawService>());

    auto& gridPass = AddPass<GridRenderPass>("Grid", NLS::Render::Settings::ERenderPassOrder::Transparent + 1);
    gridPass.SetEnabled(!ShouldDisableEditorGridPass());

    auto& debugCamerasPass = AddPass<DebugCamerasRenderPass>("Debug Cameras", NLS::Render::Settings::ERenderPassOrder::Transparent + 1);
    debugCamerasPass.SetEnabled(!ShouldDisableDebugCamerasPass());

    auto& debugLightsPass = AddPass<DebugLightsRenderPass>("Debug Lights", NLS::Render::Settings::ERenderPassOrder::Transparent + 2);
    debugLightsPass.SetEnabled(!ShouldDisableDebugLightsPass());

    auto& debugActorPass = AddPass<DebugActorRenderPass>("Debug Actor", NLS::Render::Settings::ERenderPassOrder::Transparent + 3);
    debugActorPass.SetEnabled(!ShouldDisableDebugActorPass());
    auto& debugDrawPass = AddPass<NLS::Render::Debug::DebugDrawPass>("Debug Draw", NLS::Render::Settings::ERenderPassOrder::Transparent + 4);
    debugDrawPass.SetEnabled(!ShouldDisableDebugDrawPass());
    auto& pickingPass = AddPass<PickingRenderPass>("Picking", NLS::Render::Settings::ERenderPassOrder::PostProcessing + 1);
    pickingPass.SetEnabled(!ShouldDisablePickingPass());
}

std::optional<NLS::Render::Context::FrameSnapshot> Editor::Rendering::DebugSceneRenderer::BuildFrameSnapshot(
    const NLS::Render::Data::FrameDescriptor& frameDescriptor) const
{
    auto snapshot = Engine::Rendering::DeferredSceneRenderer::BuildFrameSnapshot(frameDescriptor);
    if (!snapshot.has_value() || !HasDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>())
        return snapshot;

    const auto& scene = GetDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>().scene;
    const bool gridPassEnabled = !ShouldDisableEditorGridPass();
    const bool cameraPassEnabled = !ShouldDisableDebugCamerasPass();
    const bool lightPassEnabled = !ShouldDisableDebugLightsPass();
    const bool actorPassEnabled = !ShouldDisableDebugActorPass();
    const bool debugDrawPassEnabled = !ShouldDisableDebugDrawPass();

    ThreadedEditorHelperState helperState;
    helperState.gridPassEnabled = gridPassEnabled;
    helperState.cameraPassEnabled = cameraPassEnabled;
    helperState.lightPassEnabled = lightPassEnabled;
    helperState.actorPassEnabled = actorPassEnabled;
    helperState.debugDrawPassEnabled = debugDrawPassEnabled;
    const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
    helperState.debugDrawEnabled = debugSettings.debugDrawEnabled;
    helperState.debugDrawCamera = debugSettings.debugDrawCamera;
    helperState.debugDrawLighting = debugSettings.debugDrawLighting;
    helperState.gridEnabled = GridRenderPass::ShouldIncludeInThreadedFrame(
        gridPassEnabled,
        HasDescriptor<GridRenderPass::GridDescriptor>(),
        debugSettings.debugDrawEnabled,
        debugSettings.debugDrawGrid);
    helperState.sceneCameraCount = static_cast<uint64_t>(scene.GetFastAccessComponents().cameras.size());
    helperState.sceneLightCount = static_cast<uint64_t>(scene.GetFastAccessComponents().lights.size());
    if (HasDescriptor<DebugSceneDescriptor>())
    {
        const auto* selectedActor = GetDescriptor<DebugSceneDescriptor>().selectedActor;
        helperState.hasSelectedActor =
            OutlineRenderer::ShouldIncludeInThreadedFrame(actorPassEnabled, selectedActor);
    }

    if (const auto* debugDrawService = GetDebugDrawService(); debugDrawService != nullptr)
        helperState.hasVisibleDebugDrawPrimitives = !debugDrawService->CollectVisiblePrimitives().empty();

    snapshot->visibleHelperDrawCount = CountThreadedEditorHelperPasses(helperState);
    return snapshot;
}

NLS::Render::Context::PreparedRenderSceneBuilder Editor::Rendering::DebugSceneRenderer::BuildPreparedRenderSceneBuilder(
    const NLS::Render::Context::FrameSnapshot& snapshot) const
{
    const auto gridPassInput = GetPass<GridRenderPass>("Grid").GetPreparedThreadedPassInput();
    const auto selectionPassInput = GetPass<DebugActorRenderPass>("Debug Actor").GetPreparedThreadedPassInput();
    const auto pickingPassInput = GetPass<PickingRenderPass>("Picking").GetPreparedThreadedPassInput();
    const auto explicitHelperContribution =
        static_cast<uint64_t>(gridPassInput.has_value() ? 1u : 0u) +
        static_cast<uint64_t>(selectionPassInput.has_value() ? 2u : 0u);
    const auto aggregateHelperVisibleCount =
        snapshot.visibleHelperDrawCount >= explicitHelperContribution
            ? snapshot.visibleHelperDrawCount - explicitHelperContribution
            : 0u;
    auto metadata = BuildDebugDeferredThreadedPassMetadata(
        aggregateHelperVisibleCount,
        gridPassInput.has_value(),
        selectionPassInput.has_value(),
        pickingPassInput.has_value());
    const auto appendedPassInputs =
        BuildDebugDeferredAppendedPassInputs(gridPassInput, selectionPassInput, pickingPassInput);

    std::shared_ptr<NLS::Render::RHI::RHITexture> preferredReadbackTexture;
    uint64_t additionalRenderTargetUseCount = 0u;
    if (pickingPassInput.has_value())
    {
        preferredReadbackTexture =
            !pickingPassInput->colorAttachmentViews.empty()
                ? pickingPassInput->colorAttachmentViews.front()->GetTexture()
                : nullptr;
        additionalRenderTargetUseCount = 2u;
    }

    const bool hasSkyboxTexture =
        HasDescriptor<DeferredSceneDescriptor>() &&
        GetDescriptor<DeferredSceneDescriptor>().hasSkyboxTexture;

    return BuildDeferredPreparedRenderSceneBuilder(
        snapshot,
        hasSkyboxTexture,
        appendedPassInputs,
        metadata,
        std::move(preferredReadbackTexture),
        additionalRenderTargetUseCount);
}
