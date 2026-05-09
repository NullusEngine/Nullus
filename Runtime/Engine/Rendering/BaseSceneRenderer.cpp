#include <Rendering/Data/LightingDescriptor.h>
#include <filesystem>
#include <fstream>

#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/LightGridPrepass.h"
#include "Rendering/SceneLightingProvider.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Profiling/Profiler.h"
#include "Components/MeshRenderer.h"
#include "Components/MaterialRenderer.h"
#include "Components/TransformComponent.h"

namespace NLS::Engine::Rendering
{
namespace
{
    class PreparedPassBindingPlaceholder final : public NLS::Render::RHI::RHIBindingSet
    {
    public:
        std::string_view GetDebugName() const override { return "PreparedPassBindingPlaceholder"; }
        const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingSetDesc m_desc{};
    };

    const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetPreparedPassBindingPlaceholderInstance()
    {
        static const std::shared_ptr<NLS::Render::RHI::RHIBindingSet> kPlaceholder =
            std::make_shared<PreparedPassBindingPlaceholder>();
        return kPlaceholder;
    }

    bool IsPreparedPassBindingPlaceholder(
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet)
    {
        return bindingSet == GetPreparedPassBindingPlaceholderInstance();
    }

    void ResolvePreparedPassBindingPlaceholders(
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& drawCommands,
        const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet)
    {
        for (auto& drawCommand : drawCommands)
        {
            if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
                drawCommand.passBindingSet = resolvedBindingSet;
        }
    }

}

using namespace Components;
using RenderMaterial = Render::Resources::Material;
using RenderModel = Render::Resources::Model;
using RenderMesh = Render::Resources::Mesh;
using LightingDescriptor = Render::Data::LightingDescriptor;

BaseSceneRenderer::BaseSceneRenderer(Render::Context::Driver& p_driver)
	: Render::Core::CompositeRenderer(p_driver)
{
	SetFrameObjectBindingProvider(std::make_unique<EngineFrameObjectBindingProvider>(*this));
	m_lightGridPrepass = std::make_shared<LightGridPrepass>(p_driver);
	m_sceneLightingProvider = std::make_unique<SceneLightingProvider>();
}

BaseSceneRenderer::~BaseSceneRenderer() = default;

void BaseSceneRenderer::BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor)
{
	NLS_PROFILE_SCOPE();
	NLS_ASSERT(HasDescriptor<SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");

	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	RefreshSceneLightingDescriptor(sceneDescriptor.scene);

	Render::Core::CompositeRenderer::BeginFrame(p_frameDescriptor);

	if (const auto snapshot = BuildFrameSnapshot(p_frameDescriptor); snapshot.has_value())
		SetPendingFrameSnapshot(snapshot.value());
}

std::optional<Render::Context::FrameSnapshot> BaseSceneRenderer::BuildFrameSnapshot(
	const Render::Data::FrameDescriptor& frameDescriptor) const
{
	NLS_PROFILE_SCOPE();
	auto snapshot = Render::Core::ABaseRenderer::BuildFrameSnapshot(frameDescriptor);
	if (!snapshot.has_value())
		return snapshot;

	if (!HasDescriptor<SceneDescriptor>())
		return snapshot;

	const auto& scene = GetDescriptor<SceneDescriptor>().scene;
	const auto& fastAccess = scene.GetFastAccessComponents();
	snapshot->hasSceneInput = true;
	snapshot->sceneActorCount = static_cast<uint64_t>(scene.GetActors().size());
	snapshot->sceneModelRendererCount = static_cast<uint64_t>(fastAccess.modelRenderers.size());
	snapshot->sceneLightCount = static_cast<uint64_t>(fastAccess.lights.size());
	snapshot->sceneSkyboxCount = static_cast<uint64_t>(fastAccess.skyboxs.size());
	snapshot->visibleOpaqueDrawCount = 0u;
	snapshot->visibleTransparentDrawCount = 0u;
	snapshot->visibleSkyboxDrawCount = 0u;
	snapshot->visibleHelperDrawCount = 0u;
	return snapshot;
}

void BaseSceneRenderer::RefreshFrameSnapshotVisibility(
	Render::Context::FrameSnapshot& snapshot,
	const AllDrawables& drawables)
{
	snapshot.visibleOpaqueDrawCount = static_cast<uint64_t>(drawables.opaques.size());
	snapshot.visibleTransparentDrawCount = static_cast<uint64_t>(drawables.transparents.size());
	snapshot.visibleSkyboxDrawCount = static_cast<uint64_t>(drawables.skyboxes.size());
}

const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& BaseSceneRenderer::GetLightGridGraphicsPassBindingSet() const
{
	static const std::shared_ptr<NLS::Render::RHI::RHIBindingSet> kNullBindingSet{};
	return m_lightGridPrepass != nullptr ? m_lightGridPrepass->GetGraphicsPassBindingSet() : kNullBindingSet;
}

NLS::Render::FrameGraph::LightGridCompileContext BaseSceneRenderer::BuildLightGridCompileContext(
	const bool hasSkyboxTexture) const
{
	NLS_PROFILE_SCOPE();
	const auto frameSnapshot =
		NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(GetFrameDescriptor());
	const auto preparedComputeRequest = LightGridPrepass::BuildPreparedComputeRequest(
		frameSnapshot,
		GetLightGridPrepass(),
		BuildLightGridFrameInputs(hasSkyboxTexture));
	auto preparedComputeSource = LightGridPrepass::BuildPreparedComputeDispatchSource(preparedComputeRequest);
	auto graphicsPassBindingSet = GetLightGridGraphicsPassBindingSet();
	return NLS::Render::FrameGraph::BuildLightGridCompileContext(
		frameSnapshot,
		std::move(preparedComputeSource),
		std::move(graphicsPassBindingSet));
}

std::optional<LightGridPrepass::PreparedFrameInputs> BaseSceneRenderer::BuildLightGridFrameInputs(
	const bool hasSkyboxTexture) const
{
	NLS_PROFILE_SCOPE();
	if (m_lightGridPrepass == nullptr || !HasDescriptor<LightingDescriptor>())
		return std::nullopt;

	return LightGridPrepass::CaptureFrameInputs(
		GetDescriptor<LightingDescriptor>(),
		hasSkyboxTexture);
}

const std::shared_ptr<LightGridPrepass>& BaseSceneRenderer::GetLightGridPrepass() const
{
	return m_lightGridPrepass;
}

const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder()
{
	return GetPreparedPassBindingPlaceholderInstance();
}

void BaseSceneRenderer::ResolvePreparedPassBindingSetPlaceholders(
	Render::Context::RenderScenePackage& package,
	const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet)
{
	ResolvePreparedPassBindingPlaceholders(package.recordedDrawCommands, resolvedBindingSet);
	for (auto& passInput : package.passCommandInputs)
		ResolvePreparedPassBindingPlaceholders(passInput.recordedDrawCommands, resolvedBindingSet);
}

bool BaseSceneRenderer::CaptureThreadedPreparedDraw(
	PipelineState pso,
	const Drawable& drawable,
	PreparedRecordedDraw& outDraw)
{
	NLS_PROFILE_SCOPE();
	auto* bindingProvider = GetFrameObjectBindingProvider();
	if (bindingProvider != nullptr)
		bindingProvider->PrepareDraw(pso, drawable);

	if (!PrepareRecordedDraw(pso, drawable, outDraw))
		return false;

	if (outDraw.commandBuffer == nullptr && bindingProvider != nullptr)
	{
		Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindingSets;
		if (bindingProvider->CapturePreparedBindingSets(pso, drawable, bindingSets))
		{
			outDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
			outDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
		}
	}

	outDraw.commandBuffer.reset();
	return outDraw.pipeline != nullptr &&
		outDraw.materialBindingSet != nullptr &&
		outDraw.mesh != nullptr &&
		outDraw.instanceCount > 0u;
}

bool BaseSceneRenderer::CaptureThreadedPreparedDraw(
	const Drawable& drawable,
	Render::Resources::MaterialPipelineStateOverrides pipelineOverrides,
	Render::Settings::EComparaisonAlgorithm depthCompareOverride,
	PreparedRecordedDraw& outDraw)
{
	NLS_PROFILE_SCOPE();
	auto effectivePso = CreatePipelineState();
	auto* bindingProvider = GetFrameObjectBindingProvider();
	if (bindingProvider != nullptr)
		bindingProvider->PrepareDraw(effectivePso, drawable);

	if (!PrepareRecordedDraw(drawable, pipelineOverrides, depthCompareOverride, outDraw))
		return false;

	if (outDraw.commandBuffer == nullptr && bindingProvider != nullptr)
	{
		Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindingSets;
		if (bindingProvider->CapturePreparedBindingSets(effectivePso, drawable, bindingSets))
		{
			outDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
			outDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
		}
	}

	outDraw.commandBuffer.reset();
	return outDraw.pipeline != nullptr &&
		outDraw.materialBindingSet != nullptr &&
		outDraw.mesh != nullptr &&
		outDraw.instanceCount > 0u;
}

Render::Context::RenderScenePackage BaseSceneRenderer::BuildSnapshotOwnedRenderScenePackage(
	const Render::Context::FrameSnapshot& snapshot,
	const SnapshotRenderScenePackageBuildMode buildMode)
{
	return Render::Context::BuildSnapshotOwnedRenderScenePackage(snapshot, buildMode);
}

Render::Context::RenderScenePackage BaseSceneRenderer::BuildRenderScenePackage(
	const Render::Context::FrameSnapshot& snapshot) const
{
	NLS_PROFILE_SCOPE();
	return BuildSnapshotOwnedRenderScenePackage(snapshot);
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
	NLS_PROFILE_SCOPE();
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
