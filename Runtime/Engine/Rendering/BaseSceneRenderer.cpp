#include <Rendering/Data/LightingDescriptor.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>

#include "Math/Vector2.h"
#include "Math/Vector4.h"
#include "Debug/Logger.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/LightGridPrepass.h"
#include "Rendering/SceneLightingProvider.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Core/ServiceLocator.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/Data/ObjectDataLimits.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Profiling/Profiler.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine::Rendering
{
namespace
{
	template <typename Compare>
	void SortSceneDrawables(BaseSceneRenderer::SceneDrawables& drawables, Compare compare)
	{
		std::stable_sort(
			drawables.begin(),
			drawables.end(),
			[compare](const auto& lhs, const auto& rhs)
			{
				return compare(lhs.first, rhs.first);
			});
	}

	struct LoadedSceneFallbackShader
	{
		NLS::Render::Resources::Shader* shader = nullptr;
		std::string resourcePath;
	};

	constexpr const char* kSceneFallbackShaderResourcePaths[] = {
		":Shaders\\Lambert.hlsl",
		":Shaders/Lambert.hlsl",
		":Shaders\\Standard.hlsl",
		":Shaders/Standard.hlsl"
	};

	LoadedSceneFallbackShader ResolveLoadedSceneFallbackShader()
	{
		if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
			return {};

		auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
		for (const char* resourcePath : kSceneFallbackShaderResourcePaths)
		{
			if (auto* shader = shaderManager.GetResource(resourcePath, false))
				return { shader, resourcePath };
		}

		return {};
	}

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

	void ResolvePreparedPassBindingPlaceholders(
		std::vector<NLS::Render::Context::RecordedDrawCommandInput>& drawCommands,
		const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet,
		const size_t firstDrawIndex,
        const size_t lastDrawIndex)
    {
        const auto resolvedLastDrawIndex = std::min(lastDrawIndex, drawCommands.size());
        if (firstDrawIndex >= resolvedLastDrawIndex)
            return;

        for (size_t drawIndex = firstDrawIndex; drawIndex < resolvedLastDrawIndex; ++drawIndex)
        {
            auto& drawCommand = drawCommands[drawIndex];
            if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
                drawCommand.passBindingSet = resolvedBindingSet;
        }
    }

    void ClearPreparedPassBindingPlaceholders(
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& drawCommands)
    {
        for (auto& drawCommand : drawCommands)
        {
			if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
				drawCommand.passBindingSet.reset();
		}
	}

	bool TryLoadOneMissingMaterialTexture(NLS::Render::Resources::Material& material)
	{
		if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
			return false;

		for (const auto& [uniformName, texturePath] : material.GetTextureResourcePaths())
		{
			if (texturePath.empty())
				continue;

			const auto* parameter = material.GetParameterBlock().TryGet(uniformName);
			if (parameter != nullptr &&
				parameter->type() == typeid(NLS::Render::Resources::Texture2D*) &&
				std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter) != nullptr)
			{
				continue;
			}

			auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
			if (auto* texture = textureManager.GetResource(texturePath, false))
			{
				material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture);
				return true;
			}

			if (auto* texture = textureManager.RequestAsyncArtifact(texturePath))
			{
				material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture);
				return true;
			}

			return false;
		}

		return false;
	}

	bool PumpOneVisibleMaterialTexture(BaseSceneRenderer::SceneDrawables& drawables)
	{
		for (auto& [_, drawable] : drawables)
		{
			if (drawable.material != nullptr && TryLoadOneMissingMaterialTexture(*drawable.material))
				return true;
		}
		return false;
	}

}

using namespace Components;
using RenderMaterial = Render::Resources::Material;
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

void BaseSceneRenderer::PreloadSceneFallbackShader(NLS::Core::ResourceManagement::ShaderManager& shaderManager)
{
	for (const char* resourcePath : kSceneFallbackShaderResourcePaths)
	{
		if (shaderManager.GetResource(resourcePath, false) != nullptr)
			return;
	}

	for (const char* resourcePath : kSceneFallbackShaderResourcePaths)
	{
		if (shaderManager.GetResource(resourcePath, true) != nullptr)
			return;
	}

	NLS_LOG_WARNING("BaseSceneRenderer failed to preload a scene fallback shader; scene objects without explicit materials may be skipped until a default material or fallback shader is loaded.");
}

void BaseSceneRenderer::BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor)
{
	NLS_PROFILE_SCOPE();
	NLS_ASSERT(HasDescriptor<SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");
	InvalidateLightGridCompileContextCache();

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
	snapshot->sceneGameObjectCount = static_cast<uint64_t>(scene.GetGameObjects().size());
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
	return m_lightGridPrepass != nullptr
		? m_lightGridPrepass->GetGraphicsPassBindingSet()
		: kNullBindingSet;
}

NLS::Render::FrameGraph::LightGridCompileContext BaseSceneRenderer::BuildLightGridCompileContext(
	const bool hasSkyboxTexture) const
{
	NLS_PROFILE_SCOPE();
	const auto frameSnapshot =
		NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(GetFrameDescriptor());

	if (!NLS::Render::Context::DriverRendererAccess::IsLightGridEnabled(m_driver))
	{
		if (m_lightGridPrepass != nullptr)
			m_lightGridPrepass->EnsureFallbackGraphicsPassBindingSet(frameSnapshot, hasSkyboxTexture);
		return NLS::Render::FrameGraph::BuildLightGridCompileContext(
			frameSnapshot,
			{},
			GetLightGridGraphicsPassBindingSet());
	}

	std::lock_guard lock(m_lightGridCompileContextCacheMutex);
	if (IsLightGridCompileContextCacheHit(frameSnapshot, hasSkyboxTexture))
		return m_lightGridCompileContextCache.context;

	const auto preparedComputeRequest = LightGridPrepass::BuildPreparedComputeRequest(
		frameSnapshot,
		GetLightGridPrepass(),
		BuildLightGridFrameInputs(hasSkyboxTexture));
	auto preparedComputeSource = LightGridPrepass::BuildPreparedComputeDispatchSource(preparedComputeRequest);
	if (GetLightGridGraphicsPassBindingSet() == nullptr && m_lightGridPrepass != nullptr)
		m_lightGridPrepass->EnsureFallbackGraphicsPassBindingSet(frameSnapshot, hasSkyboxTexture);
	auto graphicsPassBindingSet = GetLightGridGraphicsPassBindingSet();
	auto context = NLS::Render::FrameGraph::BuildLightGridCompileContext(
		frameSnapshot,
		std::move(preparedComputeSource),
		std::move(graphicsPassBindingSet));

	m_lightGridCompileContextCache.valid = true;
	m_lightGridCompileContextCache.hasSkyboxTexture = hasSkyboxTexture;
	m_lightGridCompileContextCache.frameDescriptor = frameSnapshot;
	if (frameSnapshot.camera != nullptr)
	{
		m_lightGridCompileContextCache.cameraPosition = frameSnapshot.camera->GetPosition();
		m_lightGridCompileContextCache.cameraRotation = frameSnapshot.camera->GetRotation();
	}
	else
	{
		m_lightGridCompileContextCache.cameraPosition = {};
		m_lightGridCompileContextCache.cameraRotation = {};
	}
	m_lightGridCompileContextCache.context = context;
	return context;
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

BaseSceneRenderer::Material* BaseSceneRenderer::ResolveDefaultSceneMaterial()
{
	if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
	{
		auto* defaultMaterial = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager)
			.GetResource(":Materials\\Default.mat", false);
		if (defaultMaterial != nullptr && defaultMaterial->IsValid())
			return defaultMaterial;
	}

	const auto fallbackShader = ResolveLoadedSceneFallbackShader();
	if (fallbackShader.shader == nullptr)
		return nullptr;

	const auto shaderGeneration = fallbackShader.shader->GetGeneration();
	if (!m_sceneFallbackMaterial ||
		m_sceneFallbackShader != fallbackShader.shader ||
		m_sceneFallbackShaderGeneration != shaderGeneration ||
		m_sceneFallbackShaderResourcePath != fallbackShader.resourcePath)
	{
		m_sceneFallbackMaterial = std::make_unique<Render::Resources::Material>();
		m_sceneFallbackMaterial->SetShader(fallbackShader.shader);
		const_cast<std::string&>(m_sceneFallbackMaterial->path) = ":Generated/SceneFallbackMaterial";
		m_sceneFallbackMaterial->Set<Maths::Vector4>("u_Diffuse", Maths::Vector4(1.0f, 1.0f, 1.0f, 1.0f));
		m_sceneFallbackMaterial->Set<Maths::Vector2>("u_TextureTiling", Maths::Vector2(1.0f, 1.0f));
		m_sceneFallbackMaterial->Set<Maths::Vector2>("u_TextureOffset", Maths::Vector2::Zero);
		m_sceneFallbackMaterial->SetBlendable(false);
		m_sceneFallbackMaterial->SetBackfaceCulling(false);
		m_sceneFallbackMaterial->SetFrontfaceCulling(false);
		m_sceneFallbackMaterial->SetDepthTest(true);
		m_sceneFallbackMaterial->SetDepthWriting(true);
		m_sceneFallbackMaterial->SetColorWriting(true);
		m_sceneFallbackShader = fallbackShader.shader;
		m_sceneFallbackShaderGeneration = shaderGeneration;
		m_sceneFallbackShaderResourcePath = fallbackShader.resourcePath;
	}

	return m_sceneFallbackMaterial->IsValid() ? m_sceneFallbackMaterial.get() : nullptr;
}

void BaseSceneRenderer::InvalidateLightGridCompileContextCache() const
{
	std::lock_guard lock(m_lightGridCompileContextCacheMutex);
	m_lightGridCompileContextCache.valid = false;
	m_lightGridCompileContextCache.context = {};
}

bool BaseSceneRenderer::IsLightGridCompileContextCacheHit(
	const NLS::Render::Data::FrameDescriptor& frameDescriptor,
	const bool hasSkyboxTexture) const
{
	return m_lightGridCompileContextCache.valid &&
		m_lightGridCompileContextCache.hasSkyboxTexture == hasSkyboxTexture &&
		AreSameLightGridFrameInputs(m_lightGridCompileContextCache, frameDescriptor);
}

bool BaseSceneRenderer::AreSameLightGridFrameInputs(
	const LightGridCompileContextCache& cached,
	const NLS::Render::Data::FrameDescriptor& current) const
{
	const auto& cachedFrame = cached.frameDescriptor;
	const bool sameCameraTransform =
		current.camera == nullptr ||
		(Maths::Vector3::Distance(cached.cameraPosition, current.camera->GetPosition()) <= 1e-5f &&
			std::fabs(cached.cameraRotation.x - current.camera->GetRotation().x) <= 1e-5f &&
			std::fabs(cached.cameraRotation.y - current.camera->GetRotation().y) <= 1e-5f &&
			std::fabs(cached.cameraRotation.z - current.camera->GetRotation().z) <= 1e-5f &&
			std::fabs(cached.cameraRotation.w - current.camera->GetRotation().w) <= 1e-5f);

	return cachedFrame.renderWidth == current.renderWidth &&
		cachedFrame.renderHeight == current.renderHeight &&
		cachedFrame.camera == current.camera &&
		sameCameraTransform &&
		cachedFrame.outputBuffer == current.outputBuffer &&
		cachedFrame.outputColorTexture == current.outputColorTexture &&
		cachedFrame.outputDepthStencilTexture == current.outputDepthStencilTexture &&
		cachedFrame.outputColorView == current.outputColorView &&
		cachedFrame.outputDepthStencilView == current.outputDepthStencilView;
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

void BaseSceneRenderer::ResolvePreparedScenePassBindingSetPlaceholders(
	Render::Context::RenderScenePackage& package,
	const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet,
	const uint64_t sceneDrawCount)
{
	const auto sceneDrawEnd = static_cast<size_t>(
		std::min<uint64_t>(
			sceneDrawCount,
			static_cast<uint64_t>(package.recordedDrawCommands.size())));
	ResolvePreparedPassBindingPlaceholders(package.recordedDrawCommands, resolvedBindingSet, 0u, sceneDrawEnd);
	for (size_t drawIndex = sceneDrawEnd; drawIndex < package.recordedDrawCommands.size(); ++drawIndex)
	{
		auto& drawCommand = package.recordedDrawCommands[drawIndex];
		if (IsPreparedPassBindingPlaceholder(drawCommand.passBindingSet))
			drawCommand.passBindingSet.reset();
	}

	for (auto& passInput : package.passCommandInputs)
	{
		switch (passInput.kind)
		{
		case NLS::Render::Context::RenderPassCommandKind::Opaque:
		case NLS::Render::Context::RenderPassCommandKind::Skybox:
		case NLS::Render::Context::RenderPassCommandKind::Transparent:
		case NLS::Render::Context::RenderPassCommandKind::GBuffer:
		case NLS::Render::Context::RenderPassCommandKind::Lighting:
			ResolvePreparedPassBindingPlaceholders(passInput.recordedDrawCommands, resolvedBindingSet);
			break;
		default:
			ClearPreparedPassBindingPlaceholders(passInput.recordedDrawCommands);
			break;
		}
	}
}

bool BaseSceneRenderer::CaptureThreadedPreparedDraw(
	PipelineState pso,
	const Drawable& drawable,
	PreparedRecordedDraw& outDraw)
{
	NLS_PROFILE_SCOPE();
	auto* bindingProvider = GetFrameObjectBindingProvider();
	if (bindingProvider != nullptr && !bindingProvider->PrepareDraw(pso, drawable))
		return false;

	if (!PrepareRecordedDraw(pso, drawable, outDraw))
		return false;

	if (outDraw.commandBuffer == nullptr && bindingProvider != nullptr)
	{
		Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindingSets;
		if (bindingProvider->CapturePreparedBindingSets(pso, drawable, bindingSets))
		{
			outDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
			outDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
			outDraw.objectIndex = bindingSets.objectIndex;
			outDraw.usesObjectIndex = bindingSets.usesObjectIndex;
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
	if (bindingProvider != nullptr && !bindingProvider->PrepareDraw(effectivePso, drawable))
		return false;

	if (!PrepareRecordedDraw(drawable, pipelineOverrides, depthCompareOverride, outDraw))
		return false;

	if (outDraw.commandBuffer == nullptr && bindingProvider != nullptr)
	{
		Render::Core::FrameObjectBindingProvider::PreparedBindingSets bindingSets;
		if (bindingProvider->CapturePreparedBindingSets(effectivePso, drawable, bindingSets))
		{
			outDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
			outDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
			outDraw.objectIndex = bindingSets.objectIndex;
			outDraw.usesObjectIndex = bindingSets.usesObjectIndex;
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
	RenderMesh& p_mesh,
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

	Render::Entities::Drawable element;
	element.mesh = &p_mesh;
	element.material = &p_material;
	element.stateMask = stateMask;
	element.AddDescriptor(engineDrawableDescriptor);

	DrawEntity(element);
}

BaseSceneRenderer::AllDrawables BaseSceneRenderer::ParseScene()
{
	if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
		NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager).PumpAsyncLoads(1u);

	OpaqueDrawables opaques;
	TransparentDrawables transparents;
	SkyboxDrawables skyboxes;

	auto& camera = *m_frameDescriptor.camera;
	auto& sceneDescriptor = GetDescriptor<SceneDescriptor>();
	auto overrideMaterial = sceneDescriptor.overrideMaterial;
	std::optional<Render::Data::Frustum> frustum = std::nullopt;

	if (camera.HasFrustumGeometryCulling())
	{
		auto& frustumOverride = sceneDescriptor.frustumOverride;
		frustum = frustumOverride ? frustumOverride : camera.GetFrustum();
	}

	uint64_t rebuiltCachedCommandCount = 0u;
	uint64_t rawVisibleObjectCount = 0u;
	uint64_t submittedSceneDrawCount = 0u;

	auto appendSceneDrawables = [&](
		SceneSystem::Scene& scene,
		RenderScene& renderScene,
		const bool includeSkyboxes,
		const bool requireExplicitMaterialTextures)
	{
		const auto syncStats = renderScene.Synchronize(scene, {
			ResolveDefaultSceneMaterial(),
			overrideMaterial,
			requireExplicitMaterialTextures
		});
		rebuiltCachedCommandCount += syncStats.rebuiltCachedCommandCount;
		auto retainedDrawables = renderScene.GatherVisibleCommands({
			frustum ? &frustum.value() : nullptr,
			camera.GetPosition()
		});
		const auto sceneStats = renderScene.GetLastDrawCallOptimizationStats();

		rawVisibleObjectCount += sceneStats.rawVisibleObjectCount;
		submittedSceneDrawCount += sceneStats.submittedSceneDrawCount;
		opaques.insert(
			opaques.end(),
			std::make_move_iterator(retainedDrawables.opaques.begin()),
			std::make_move_iterator(retainedDrawables.opaques.end()));
		transparents.insert(
			transparents.end(),
			std::make_move_iterator(retainedDrawables.transparents.begin()),
			std::make_move_iterator(retainedDrawables.transparents.end()));

		if (!includeSkyboxes)
			return;

		const auto& fastAccess = scene.GetFastAccessComponents();
		skyboxes.reserve(skyboxes.size() + fastAccess.skyboxs.size());

		for (auto* skybox : fastAccess.skyboxs)
		{
			if (!skybox)
				continue;
			auto* owner = skybox->gameobject();
			if (!owner || !owner->IsActive())
				continue;

			if (auto mesh = skybox->GetMesh())
			{
				if (auto material = skybox->GetMaterial())
				{
					auto& transform = owner->GetTransform()->GetTransform();
					Render::Entities::Drawable drawable;
					drawable.mesh = mesh;
					drawable.material = material;
					drawable.stateMask = material->GenerateStateMask();
					drawable.AddDescriptor<EngineDrawableDescriptor>({ transform.GetWorldMatrix() });
					skyboxes.emplace_back(0.0f, std::move(drawable));
				}
			}
		}
	};

	appendSceneDrawables(sceneDescriptor.scene, m_renderScene, true, false);
	for (auto it = m_additiveRenderScenes.begin(); it != m_additiveRenderScenes.end();)
	{
		const auto* cachedScene = it->first;
		const auto isStillActive = std::find(
			sceneDescriptor.additiveScenes.begin(),
			sceneDescriptor.additiveScenes.end(),
			cachedScene) != sceneDescriptor.additiveScenes.end();
		if (isStillActive)
			++it;
		else
			it = m_additiveRenderScenes.erase(it);
	}
	for (auto* additiveScene : sceneDescriptor.additiveScenes)
	{
		if (!additiveScene)
			continue;
		auto& additiveRenderScene = m_additiveRenderScenes[additiveScene];
		appendSceneDrawables(*additiveScene, additiveRenderScene, false, true);
	}

	SortSceneDrawables(transparents, std::greater<float>{});

	uint32_t nextObjectIndex = 0u;
	auto reassignObjectIndices = [&nextObjectIndex](auto& queue)
	{
		for (auto& entry : queue)
		{
			EngineDrawableDescriptor descriptor;
			if (!entry.second.template TryGetDescriptor<EngineDrawableDescriptor>(descriptor))
				continue;

			const uint32_t objectCount = std::max<uint32_t>(1u, descriptor.objectCount);
			uint32_t lastObjectIndex = 0u;
			if (!NLS::Render::Data::TryResolveObjectDataRangeEnd(
				nextObjectIndex,
				objectCount,
				lastObjectIndex))
			{
				descriptor.objectIndex = EngineDrawableDescriptor::kInvalidObjectIndex;
			}
			else
			{
				descriptor.objectIndex = nextObjectIndex;
				nextObjectIndex = lastObjectIndex + 1u;
			}
			entry.second.template RemoveDescriptor<EngineDrawableDescriptor>();
			entry.second.template AddDescriptor<EngineDrawableDescriptor>(std::move(descriptor));
		}
	};
	reassignObjectIndices(opaques);
	reassignObjectIndices(skyboxes);
	reassignObjectIndices(transparents);

	if (!PumpOneVisibleMaterialTexture(opaques))
		PumpOneVisibleMaterialTexture(transparents);

	SortSceneDrawables(skyboxes, std::less<float>{});
	m_rendererStats.RecordSceneParse(
		static_cast<uint64_t>(opaques.size()),
		static_cast<uint64_t>(transparents.size()),
		static_cast<uint64_t>(skyboxes.size()));
	auto optimizationStats = m_renderScene.GetLastDrawCallOptimizationStats();
	optimizationStats.cachedCommandRebuildCount = rebuiltCachedCommandCount;
	optimizationStats.rawVisibleObjectCount = rawVisibleObjectCount + static_cast<uint64_t>(skyboxes.size());
	optimizationStats.submittedSceneDrawCount = submittedSceneDrawCount + static_cast<uint64_t>(skyboxes.size());
	m_rendererStats.RecordDrawCallOptimizationStats(optimizationStats);
	return { opaques, transparents, skyboxes };
}
}
