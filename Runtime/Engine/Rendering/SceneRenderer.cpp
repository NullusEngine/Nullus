
#include <Rendering/Data/Frustum.h>
#include <Rendering/Features/LightingRenderFeature.h>

#include "Rendering/SceneRenderer.h"
#include "Rendering/EngineBufferRenderFeature.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Components/TransformComponent.h"
struct SceneRenderPassDescriptor
{
	Engine::Rendering::SceneRenderer::AllDrawables drawables;
};

class OpaqueRenderPass : public NLS::Rendering::Core::ARenderPass
{
public:
	OpaqueRenderPass(NLS::Rendering::Core::CompositeRenderer& p_renderer) : NLS::Rendering::Core::ARenderPass(p_renderer) {}

protected:
	virtual void Draw(NLS::Rendering::Data::PipelineState p_pso) override
	{
		auto& sceneContent = m_renderer.GetDescriptor<SceneRenderPassDescriptor>();

		for (const auto& [distance, drawable] : sceneContent.drawables.opaques)
		{
			m_renderer.DrawEntity(p_pso, drawable);
		}
	}
};

class TransparentRenderPass : public NLS::Rendering::Core::ARenderPass
{
public:
	TransparentRenderPass(NLS::Rendering::Core::CompositeRenderer& p_renderer) : NLS::Rendering::Core::ARenderPass(p_renderer) {}

protected:
	virtual void Draw(NLS::Rendering::Data::PipelineState p_pso) override
	{
		auto& sceneContent = m_renderer.GetDescriptor<SceneRenderPassDescriptor>();

		for (const auto& [distance, drawable] : sceneContent.drawables.transparents)
		{
			m_renderer.DrawEntity(p_pso, drawable);
		}
	}
};

Engine::Rendering::SceneRenderer::SceneRenderer(NLS::Rendering::Context::Driver& p_driver)
	: NLS::Rendering::Core::CompositeRenderer(p_driver)
{
	AddFeature<EngineBufferRenderFeature>();
	AddFeature<NLS::Rendering::Features::LightingRenderFeature>();

	AddPass<OpaqueRenderPass>("Opaques", NLS::Rendering::Settings::ERenderPassOrder::Opaque);
	AddPass<TransparentRenderPass>("Transparents", NLS::Rendering::Settings::ERenderPassOrder::Transparent);
}

NLS::Rendering::Features::LightingRenderFeature::LightSet FindActiveLights(const Engine::SceneSystem::Scene& p_scene)
{
	NLS::Rendering::Features::LightingRenderFeature::LightSet lights;

	const auto& facs = p_scene.GetFastAccessComponents();

	for (auto light : facs.lights)
	{
		if (light->gameobject()->IsActive())
		{
			lights.push_back(std::ref(*light->GetData()));
		}
	}

	return lights;
}

void Engine::Rendering::SceneRenderer::BeginFrame(const NLS::Rendering::Data::FrameDescriptor& p_frameDescriptor)
{
	NLS_ASSERT(HasDescriptor<SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");

	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();

	AddDescriptor<NLS::Rendering::Features::LightingRenderFeature::LightingDescriptor>({
		FindActiveLights(sceneDescriptor.scene),
	});

	NLS::Rendering::Core::CompositeRenderer::BeginFrame(p_frameDescriptor);

	AddDescriptor<SceneRenderPassDescriptor>({
		ParseScene()
	});
}

void Engine::Rendering::SceneRenderer::DrawModelWithSingleMaterial(NLS::Rendering::Data::PipelineState p_pso, NLS::Rendering::Resources::Model& p_model, NLS::Rendering::Data::Material& p_material, const Maths::Matrix4& p_modelMatrix)
{
	auto stateMask = p_material.GenerateStateMask();
	auto userMatrix = Maths::Matrix4::Identity;

	auto engineDrawableDescriptor = EngineDrawableDescriptor{
		p_modelMatrix,
		userMatrix
	};

	for (auto mesh : p_model.GetMeshes())
	{
		NLS::Rendering::Entities::Drawable element;
		element.mesh = mesh;
		element.material = &p_material;
		element.stateMask = stateMask;
		element.AddDescriptor(engineDrawableDescriptor);

		DrawEntity(p_pso, element);
	}
}

Engine::Rendering::SceneRenderer::AllDrawables Engine::Rendering::SceneRenderer::ParseScene()
{
	using namespace NLS::Engine::Components;

	OpaqueDrawables opaques;
	TransparentDrawables transparents;

	auto& camera = *m_frameDescriptor.camera;

	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	auto& scene = sceneDescriptor.scene;
	auto overrideMaterial = sceneDescriptor.overrideMaterial;
	auto fallbackMaterial = sceneDescriptor.fallbackMaterial;
	std::optional<NLS::Rendering::Data::Frustum> frustum = std::nullopt;

	if (camera.HasFrustumGeometryCulling())
	{
		auto& frustumOverride = sceneDescriptor.frustumOverride;
		frustum = frustumOverride ? frustumOverride : camera.GetFrustum();
	}

	for (MeshRenderer* modelRenderer : scene.GetFastAccessComponents().modelRenderers)
	{
		auto owner = modelRenderer->gameobject();

		if (owner->IsActive())
		{
			if (auto model = modelRenderer->GetModel())
			{
				if (auto materialRenderer = modelRenderer->gameobject()->GetComponent<MaterialRenderer>())
				{
					auto& transform = owner->GetTransform()->GetTransform();

					auto cullingOptions = NLS::Rendering::Settings::ECullingOptions::NONE;

					if (modelRenderer->GetFrustumBehaviour() != MeshRenderer::EFrustumBehaviour::DISABLED)
					{
						cullingOptions |= NLS::Rendering::Settings::ECullingOptions::FRUSTUM_PER_MODEL;
					}

					if (modelRenderer->GetFrustumBehaviour() == MeshRenderer::EFrustumBehaviour::CULL_MESHES)
					{
						cullingOptions |= NLS::Rendering::Settings::ECullingOptions::FRUSTUM_PER_MESH;
					}

					auto modelBoundingSphere = modelRenderer->GetFrustumBehaviour() == MeshRenderer::EFrustumBehaviour::CULL_CUSTOM ? modelRenderer->GetCustomBoundingSphere() : model->GetBoundingSphere();

					std::vector<NLS::Rendering::Resources::Mesh*> meshes;

					if (frustum)
					{
						meshes = frustum.value().GetMeshesInFrustum(*model, modelBoundingSphere, transform, cullingOptions);
					}
					else
					{
						meshes = model->GetMeshes();
					}

					if (!meshes.empty())
					{
						float distanceToActor = Maths::Vector3::Distance(transform.GetWorldPosition(), camera.GetPosition());
						const MaterialRenderer::MaterialList& materials = materialRenderer->GetMaterials();

						for (const auto mesh : meshes)
						{
                            NLS::Rendering::Data::Material* material = nullptr;

							if (mesh->GetMaterialIndex() < kMaxMaterialCount)
							{
                                if (overrideMaterial && overrideMaterial->IsValid())
								{
									material = overrideMaterial;
								}
								else
								{
									material = materials.at(mesh->GetMaterialIndex());
								}

								bool isMaterialValid = false;
                                if (material)
								{
                                    isMaterialValid = material->IsValid();
								}
                                bool hasValidFallbackMaterial = false;
                                if (fallbackMaterial)
								{
                                    hasValidFallbackMaterial = fallbackMaterial->IsValid();
								}

								if (!isMaterialValid && hasValidFallbackMaterial)
								{
									material = fallbackMaterial;
								}
							}

							if (material && material->IsValid())
							{
								NLS::Rendering::Entities::Drawable drawable;
								drawable.mesh = mesh;
								drawable.material = material;
								drawable.stateMask = material->GenerateStateMask();

								drawable.AddDescriptor<EngineDrawableDescriptor>({
									transform.GetWorldMatrix(),
									materialRenderer->GetUserMatrix()
								});

								if (material->IsBlendable())
								{
									transparents.emplace(distanceToActor, drawable);
								}
								else
								{
									opaques.emplace(distanceToActor, drawable);
								}
							}
						}
					}
				}
			}
		}
	}

	return { opaques, transparents };
}
