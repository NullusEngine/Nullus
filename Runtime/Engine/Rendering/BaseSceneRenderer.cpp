#include <Rendering/Features/LightingRenderFeature.h>

#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/EngineBufferRenderFeature.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Components/TransformComponent.h"

using namespace NLS;

namespace
{
	NLS::Render::Features::LightingRenderFeature::LightSet FindActiveLights(const NLS::Engine::SceneSystem::Scene& p_scene)
	{
		NLS::Render::Features::LightingRenderFeature::LightSet lights;

		for (auto* light : p_scene.GetFastAccessComponents().lights)
		{
			if (!light)
				continue;

			auto* owner = light->gameobject();
			if (owner && owner->IsActive())
				lights.push_back(std::ref(*light->GetData()));
		}

		return lights;
	}
}

Engine::Rendering::BaseSceneRenderer::BaseSceneRenderer(NLS::Render::Context::Driver& p_driver)
	: NLS::Render::Core::CompositeRenderer(p_driver)
{
	AddFeature<EngineBufferRenderFeature>();
	AddFeature<NLS::Render::Features::LightingRenderFeature>();
}

void Engine::Rendering::BaseSceneRenderer::BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
{
	NLS_ASSERT(HasDescriptor<SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");

	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	AddDescriptor<NLS::Render::Features::LightingRenderFeature::LightingDescriptor>({
		FindActiveLights(sceneDescriptor.scene),
	});

	NLS::Render::Core::CompositeRenderer::BeginFrame(p_frameDescriptor);
}

void Engine::Rendering::BaseSceneRenderer::DrawModelWithSingleMaterial(
	NLS::Render::Data::PipelineState p_pso,
	NLS::Render::Resources::Model& p_model,
	NLS::Render::Resources::Material& p_material,
	const Maths::Matrix4& p_modelMatrix
)
{
	auto stateMask = p_material.GenerateStateMask();
	auto userMatrix = Maths::Matrix4::Identity;

	auto engineDrawableDescriptor = EngineDrawableDescriptor{
		p_modelMatrix,
		userMatrix
	};

	for (auto mesh : p_model.GetMeshes())
	{
		NLS::Render::Entities::Drawable element;
		element.mesh = mesh;
		element.material = &p_material;
		element.stateMask = stateMask;
		element.AddDescriptor(engineDrawableDescriptor);

		DrawEntity(p_pso, element);
	}
}

Engine::Rendering::BaseSceneRenderer::AllDrawables Engine::Rendering::BaseSceneRenderer::ParseScene()
{
	using namespace NLS::Engine::Components;

	OpaqueDrawables opaques;
	TransparentDrawables transparents;
	SkyboxDrawables skyboxes;

	auto& camera = *m_frameDescriptor.camera;
	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	auto& scene = sceneDescriptor.scene;
	auto overrideMaterial = sceneDescriptor.overrideMaterial;
	auto fallbackMaterial = sceneDescriptor.fallbackMaterial;
	std::optional<NLS::Render::Data::Frustum> frustum = std::nullopt;

	if (camera.HasFrustumGeometryCulling())
	{
		auto& frustumOverride = sceneDescriptor.frustumOverride;
		frustum = frustumOverride ? frustumOverride : camera.GetFrustum();
	}

	for (auto* modelRenderer : scene.GetFastAccessComponents().modelRenderers)
	{
		if (!modelRenderer)
			continue;

		auto* owner = modelRenderer->gameobject();
		if (!owner || !owner->IsActive())
			continue;

		auto model = modelRenderer->GetModel();
		auto materialRenderer = owner->GetComponent<MaterialRenderer>();
		if (!model || !materialRenderer)
			continue;

		auto& transform = owner->GetTransform()->GetTransform();
		auto cullingOptions = NLS::Render::Settings::ECullingOptions::NONE;

		if (modelRenderer->GetFrustumBehaviour() != MeshRenderer::EFrustumBehaviour::DISABLED)
			cullingOptions |= NLS::Render::Settings::ECullingOptions::FRUSTUM_PER_MODEL;
		if (modelRenderer->GetFrustumBehaviour() == MeshRenderer::EFrustumBehaviour::CULL_MESHES)
			cullingOptions |= NLS::Render::Settings::ECullingOptions::FRUSTUM_PER_MESH;

		auto modelBoundingSphere = modelRenderer->GetFrustumBehaviour() == MeshRenderer::EFrustumBehaviour::CULL_CUSTOM
			? modelRenderer->GetCustomBoundingSphere()
			: model->GetBoundingSphere();

		std::vector<NLS::Render::Resources::Mesh*> meshes;
		if (frustum)
			meshes = frustum.value().GetMeshesInFrustum(*model, modelBoundingSphere, transform, cullingOptions);
		else
			meshes = model->GetMeshes();

		if (meshes.empty())
			continue;

		float distanceToActor = Maths::Vector3::Distance(transform.GetWorldPosition(), camera.GetPosition());
		const MaterialRenderer::MaterialList& materials = materialRenderer->GetMaterials();

		for (const auto mesh : meshes)
		{
			NLS::Render::Resources::Material* material = nullptr;
			if (mesh->GetMaterialIndex() < kMaxMaterialCount)
			{
				if (overrideMaterial && overrideMaterial->IsValid())
					material = overrideMaterial;
				else
					material = materials.at(mesh->GetMaterialIndex());

				const bool isMaterialValid = material && material->IsValid();
				const bool hasValidFallbackMaterial = fallbackMaterial && fallbackMaterial->IsValid();
				if (!isMaterialValid && hasValidFallbackMaterial)
					material = fallbackMaterial;
			}

			if (material && material->IsValid())
			{
				NLS::Render::Entities::Drawable drawable;
				drawable.mesh = mesh;
				drawable.material = material;
				drawable.stateMask = material->GenerateStateMask();
				drawable.AddDescriptor<EngineDrawableDescriptor>({ transform.GetWorldMatrix(), materialRenderer->GetUserMatrix() });

				if (material->IsBlendable())
					transparents.emplace(distanceToActor, drawable);
				else
					opaques.emplace(distanceToActor, drawable);
			}
		}
	}

	for (auto* skybox : scene.GetFastAccessComponents().skyboxs)
	{
		if (!skybox)
			continue;
		auto* owner = skybox->gameobject();
		if (!owner || !owner->IsActive())
			continue;

		if (auto model = skybox->GetModel())
		{
			if (auto material = skybox->GetMaterial())
			{
				auto& transform = owner->GetTransform()->GetTransform();
				NLS::Render::Entities::Drawable drawable;
				drawable.mesh = model->GetMeshes()[0];
				drawable.material = material;
				drawable.stateMask = material->GenerateStateMask();
				drawable.AddDescriptor<EngineDrawableDescriptor>({ transform.GetWorldMatrix() });
				skyboxes.emplace(0.0f, drawable);
			}
		}
	}

	return { opaques, transparents, skyboxes };
}
