#include <Rendering/Data/LightingDescriptor.h>
#include <filesystem>
#include <fstream>

#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/SceneLightingProvider.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Components/TransformComponent.h"

namespace NLS::Engine::Rendering
{
using namespace Components;
using RenderMaterial = Render::Resources::Material;
using RenderModel = Render::Resources::Model;
using RenderMesh = Render::Resources::Mesh;
using LightingDescriptor = Render::Data::LightingDescriptor;

BaseSceneRenderer::BaseSceneRenderer(Render::Context::Driver& p_driver)
	: Render::Core::CompositeRenderer(p_driver)
{
	SetFrameObjectBindingProvider(std::make_unique<EngineFrameObjectBindingProvider>(*this));
	m_sceneLightingProvider = std::make_unique<SceneLightingProvider>();
}

BaseSceneRenderer::~BaseSceneRenderer() = default;

void BaseSceneRenderer::BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor)
{
	NLS_ASSERT(HasDescriptor<SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");

	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	RefreshSceneLightingDescriptor(sceneDescriptor.scene);

	Render::Core::CompositeRenderer::BeginFrame(p_frameDescriptor);
}

SceneLightingProvider& BaseSceneRenderer::GetSceneLightingProvider()
{
	return *m_sceneLightingProvider;
}

const SceneLightingProvider& BaseSceneRenderer::GetSceneLightingProvider() const
{
	return *m_sceneLightingProvider;
}

void BaseSceneRenderer::RefreshSceneLightingDescriptor(SceneSystem::Scene& scene)
{
	m_sceneLightingProvider->Collect(scene);
	AddDescriptor<LightingDescriptor>(LightingDescriptor{ GetSceneLightingProvider().GetLightingDescriptor().lights });
}

void BaseSceneRenderer::DrawModelWithSingleMaterial(
	Render::Data::PipelineState p_pso,
	RenderModel& p_model,
	RenderMaterial& p_material,
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
		Render::Entities::Drawable element;
		element.mesh = mesh;
		element.material = &p_material;
		element.stateMask = stateMask;
		element.AddDescriptor(engineDrawableDescriptor);

		DrawEntity(element);
	}
}

BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()
{
	OpaqueDrawables opaques;
	TransparentDrawables transparents;
	SkyboxDrawables skyboxes;

	auto& camera = *m_frameDescriptor.camera;
	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	auto& scene = sceneDescriptor.scene;
	auto overrideMaterial = sceneDescriptor.overrideMaterial;
	auto fallbackMaterial = sceneDescriptor.fallbackMaterial;
	std::optional<Render::Data::Frustum> frustum = std::nullopt;

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
		auto cullingOptions = Render::Settings::ECullingOptions::NONE;

		if (modelRenderer->GetFrustumBehaviour() != MeshRenderer::EFrustumBehaviour::DISABLED)
			cullingOptions |= Render::Settings::ECullingOptions::FRUSTUM_PER_MODEL;
		if (modelRenderer->GetFrustumBehaviour() == MeshRenderer::EFrustumBehaviour::CULL_MESHES)
			cullingOptions |= Render::Settings::ECullingOptions::FRUSTUM_PER_MESH;

		auto modelBoundingSphere = modelRenderer->GetFrustumBehaviour() == MeshRenderer::EFrustumBehaviour::CULL_CUSTOM
			? modelRenderer->GetCustomBoundingSphere()
			: model->GetBoundingSphere();

		std::vector<RenderMesh*> meshes;
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
			RenderMaterial* material = nullptr;
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
				Render::Entities::Drawable drawable;
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
				Render::Entities::Drawable drawable;
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
}
