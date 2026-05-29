#include "Rendering/DeferredSceneRenderer.h"

#include <fg/Blackboard.hpp>

#include <cstdint>
#include <filesystem>
#include <span>

#include <Debug/Logger.h>
#include <Math/Matrix4.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Data/LightingDescriptor.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/FrameGraph/FrameGraphExecutionPlan.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Rendering/Geometry/Vertex.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/MaterialVariantKey.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/TextureCube.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>
#include <Rendering/Settings/EPrimitiveMode.h>
#include <ResourceManagement/ShaderManager.h>

#include <Profiling/Profiler.h>

#include "Rendering/ScenePipelineStatePresets.h"

namespace
{
	using LightingDescriptor = NLS::Render::Data::LightingDescriptor;

	struct DeferredPreparedFrameDescriptorSnapshot
	{
		NLS::Render::Data::FrameDescriptor descriptor{};
		std::shared_ptr<NLS::Maths::Transform> cameraTransform;
		std::shared_ptr<NLS::Render::Entities::Camera> camera;
	};

	std::span<const NLS::Render::RHI::TextureFormat> GetDeferredGBufferColorFormats()
	{
		return NLS::Render::FrameGraph::kDeferredGBufferColorFormats;
	}

	bool TryReservePreparedFrameResourcesForThreadedCapture(
		NLS::Render::Core::CompositeRenderer& renderer)
	{
		auto* provider = renderer.GetFrameObjectBindingProvider();
		return provider == nullptr || provider->TryReservePreparedFrameResources();
	}

	NLS::Render::Geometry::Vertex MakeFullscreenVertex(float x, float y, float u, float v)
	{
		NLS::Render::Geometry::Vertex vertex{};
		vertex.position[0] = x;
		vertex.position[1] = y;
		vertex.position[2] = 0.0f;
		vertex.texCoords[0] = u;
		vertex.texCoords[1] = v;
		vertex.normals[2] = 1.0f;
		vertex.tangent[0] = 1.0f;
		vertex.bitangent[1] = 1.0f;
		return vertex;
	}

	DeferredPreparedFrameDescriptorSnapshot FreezeDeferredPreparedFrameDescriptor(
		const NLS::Render::Data::FrameDescriptor& source)
	{
		DeferredPreparedFrameDescriptorSnapshot snapshot;
		snapshot.descriptor =
			NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(source);

		if (source.camera == nullptr)
			return snapshot;

		snapshot.cameraTransform = std::make_shared<NLS::Maths::Transform>();
		snapshot.camera = std::make_shared<NLS::Render::Entities::Camera>(snapshot.cameraTransform.get());
		snapshot.cameraTransform->SetWorldPosition(source.camera->GetPosition());
		snapshot.cameraTransform->SetWorldRotation(source.camera->GetRotation());
		snapshot.camera->SetProjectionMode(source.camera->GetProjectionMode());
		snapshot.camera->SetFov(source.camera->GetFov());
		snapshot.camera->SetSize(source.camera->GetSize());
		snapshot.camera->SetNear(source.camera->GetNear());
		snapshot.camera->SetFar(source.camera->GetFar());
		snapshot.camera->SetClearColor(source.camera->GetClearColor());
		snapshot.camera->SetClearColorBuffer(source.camera->GetClearColorBuffer());
		snapshot.camera->SetClearDepthBuffer(source.camera->GetClearDepthBuffer());
		snapshot.camera->SetClearStencilBuffer(source.camera->GetClearStencilBuffer());
		snapshot.camera->SetFrustumGeometryCulling(source.camera->HasFrustumGeometryCulling());
		snapshot.camera->SetFrustumLightCulling(source.camera->HasFrustumLightCulling());
		snapshot.camera->CacheMatrices(source.renderWidth, source.renderHeight);
		snapshot.descriptor.camera = snapshot.camera.get();
		return snapshot;
	}

	std::string ResolveEngineShaderPath(const std::string& fileName)
	{
		const auto relativeShaderPath = std::filesystem::path("App") / "Assets" / "Engine" / "Shaders" / fileName;
		const auto appRelativeShaderPath = std::filesystem::path("..") / "Assets" / "Engine" / "Shaders" / fileName;
		for (auto probe = std::filesystem::current_path(); !probe.empty(); probe = probe.parent_path())
		{
			const auto repoCandidate = probe / relativeShaderPath;
			if (std::filesystem::exists(repoCandidate))
				return std::filesystem::weakly_canonical(repoCandidate).string();

			const auto appCandidate = probe / appRelativeShaderPath;
			if (std::filesystem::exists(appCandidate))
				return std::filesystem::weakly_canonical(appCandidate).string();

			if (probe == probe.root_path())
				break;
		}

		return relativeShaderPath.string();
	}

	const std::any* FindMaterialParameter(
		const NLS::Render::Resources::Material& material,
		const char* name)
	{
		return material.GetParameterBlock().TryGet(name);
	}

	void SetMaterialParameter(
		NLS::Render::Resources::Material& target,
		const char* targetName,
		const std::any& value)
	{
		target.SetRawParameter(targetName, value);
	}

	void CopyMaterialParameterIfPresent(
		NLS::Render::Resources::Material& target,
		const char* targetName,
		const NLS::Render::Resources::Material& source,
		const char* sourceName)
	{
		if (const auto* value = FindMaterialParameter(source, sourceName); value != nullptr)
			SetMaterialParameter(target, targetName, *value);
	}

	void EnsureDeferredLightingSkyParameters(
		NLS::Render::Resources::Material& lightingMaterial,
		const NLS::Render::Resources::Material* skyboxMaterial)
	{
		if (skyboxMaterial != nullptr)
		{
			for (const char* parameterName : {
				"u_UseProceduralSky",
				"u_SkyTint",
				"u_GroundColor",
				"u_SunDirection",
				"u_Exposure",
				"u_AtmosphereThickness",
				"u_SunSize",
				"u_SunSizeConvergence"
			})
			{
				CopyMaterialParameterIfPresent(lightingMaterial, parameterName, *skyboxMaterial, parameterName);
			}
			return;
		}

		lightingMaterial.Set<bool>("u_UseProceduralSky", true);
		lightingMaterial.Set<NLS::Maths::Vector3>("u_SkyTint", NLS::Maths::Vector3(0.50f, 0.62f, 0.82f));
		lightingMaterial.Set<NLS::Maths::Vector3>("u_GroundColor", NLS::Maths::Vector3(0.46f, 0.44f, 0.42f));
		lightingMaterial.Set<NLS::Maths::Vector3>("u_SunDirection", NLS::Maths::Vector3(-0.35f, 0.78f, -0.18f));
		lightingMaterial.Set<float>("u_Exposure", 1.02f);
		lightingMaterial.Set<float>("u_AtmosphereThickness", 0.85f);
		lightingMaterial.Set<float>("u_SunSize", 0.0f);
		lightingMaterial.Set<float>("u_SunSizeConvergence", 1.0f);
	}

	bool IsZeroColor(const NLS::Maths::Vector4& color)
	{
		return color.x == 0.0f &&
			color.y == 0.0f &&
			color.z == 0.0f &&
			color.w == 0.0f;
	}

	void EnsureDeferredGBufferFallbackParameters(NLS::Render::Resources::Material& target)
	{
		const auto& parameters = target.GetParameterBlock();
		auto* albedo = parameters.TryGet("u_Albedo");
		if (albedo == nullptr ||
			(albedo->type() == typeid(NLS::Maths::Vector4) &&
				IsZeroColor(std::any_cast<const NLS::Maths::Vector4&>(*albedo))))
		{
			target.SetRawParameter("u_Albedo", NLS::Maths::Vector4(1.0f, 1.0f, 1.0f, 1.0f));
		}

		auto* roughness = parameters.TryGet("u_Roughness");
		if (roughness == nullptr ||
			(roughness->type() == typeid(float) &&
				std::any_cast<float>(*roughness) == 0.0f))
		{
			target.SetRawParameter("u_Roughness", 1.0f);
		}

		auto* ambientOcclusion = parameters.TryGet("u_AmbientOcclusion");
		if (ambientOcclusion == nullptr ||
			(ambientOcclusion->type() == typeid(float) &&
				std::any_cast<float>(*ambientOcclusion) == 0.0f))
		{
			target.SetRawParameter("u_AmbientOcclusion", 1.0f);
		}

		for (const char* textureName : {
			"u_AlbedoMap",
			"u_MetallicMap",
			"u_RoughnessMap",
			"u_AmbientOcclusionMap",
			"u_NormalMap",
			"u_OpacityMap",
			"u_EmissiveMap",
			"u_SpecularMap"
		})
		{
			if (!parameters.Contains(textureName))
				target.SetRawParameter(textureName, static_cast<NLS::Render::Resources::Texture2D*>(nullptr));
		}
	}

	void SubmitMeshDraw(
		const std::shared_ptr<NLS::Render::RHI::RHICommandBuffer>& commandBuffer,
		const std::shared_ptr<NLS::Render::RHI::RHIMesh>& rhiMesh,
		const uint32_t instanceCount)
	{
		if (commandBuffer == nullptr || rhiMesh == nullptr || rhiMesh->GetVertexBuffer() == nullptr)
			return;

		commandBuffer->BindVertexBuffer(0, { rhiMesh->GetVertexBuffer(), 0, rhiMesh->GetVertexStride() });

		const auto indexBuffer = rhiMesh->GetIndexBuffer();
		const auto indexCount = rhiMesh->GetIndexCount();
		if (indexBuffer != nullptr && indexCount > 0u)
		{
			commandBuffer->BindIndexBuffer({ indexBuffer, 0, rhiMesh->GetIndexType() });
			commandBuffer->DrawIndexed(indexCount, instanceCount, 0, 0, 0);
			return;
		}

		const auto vertexCount = rhiMesh->GetVertexCount();
		if (vertexCount > 0u)
			commandBuffer->Draw(vertexCount, instanceCount, 0, 0);
	}

	NLS::Render::Resources::MaterialPipelineStateOverrides BuildGBufferMaterialOverrides(
		const NLS::Render::Resources::Material& sourceMaterial)
	{
		NLS::Render::Resources::MaterialPipelineStateOverrides overrides;
		overrides.depthTest = sourceMaterial.HasDepthTest();
		overrides.depthWrite = sourceMaterial.HasDepthWriting();
		overrides.colorWrite = true;
		overrides.SetColorFormats(GetDeferredGBufferColorFormats());
		overrides.culling = sourceMaterial.HasBackfaceCulling() || sourceMaterial.HasFrontfaceCulling();
		overrides.cullFace = sourceMaterial.HasBackfaceCulling() && sourceMaterial.HasFrontfaceCulling()
			? NLS::Render::Settings::ECullFace::FRONT_AND_BACK
			: sourceMaterial.HasFrontfaceCulling()
				? NLS::Render::Settings::ECullFace::FRONT
				: NLS::Render::Settings::ECullFace::BACK;
		return overrides;
	}

	NLS::Engine::Rendering::DeferredSceneRenderer::GBufferMaterialSyncStamp BuildGBufferMaterialSyncStamp(
		const NLS::Render::Resources::Material& sourceMaterial)
	{
		return {
			sourceMaterial.GetInstanceId(),
			sourceMaterial.GetParameterRevision(),
			sourceMaterial.GetRenderStateRevision()
		};
	}

	bool operator==(
		const NLS::Engine::Rendering::DeferredSceneRenderer::GBufferMaterialSyncStamp& lhs,
		const NLS::Engine::Rendering::DeferredSceneRenderer::GBufferMaterialSyncStamp& rhs)
	{
		return lhs.sourceMaterialInstanceId == rhs.sourceMaterialInstanceId &&
			lhs.parameterRevision == rhs.parameterRevision &&
			lhs.renderStateRevision == rhs.renderStateRevision;
	}

	bool operator!=(
		const NLS::Engine::Rendering::DeferredSceneRenderer::GBufferMaterialSyncStamp& lhs,
		const NLS::Engine::Rendering::DeferredSceneRenderer::GBufferMaterialSyncStamp& rhs)
	{
		return !(lhs == rhs);
	}

	bool WrappedTextureMatches(
		const std::unique_ptr<NLS::Render::Resources::Texture2D>& wrapper,
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		const uint16_t width,
		const uint16_t height)
	{
		return wrapper != nullptr &&
			texture != nullptr &&
			wrapper->width == width &&
			wrapper->height == height &&
			wrapper->GetExplicitRHITextureHandle() == texture;
	}

	bool TextureResourceMatchesSize(
		const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
		const uint16_t width,
		const uint16_t height)
	{
		if (texture == nullptr)
			return false;

		const auto& desc = texture->GetDesc();
		return desc.extent.width == width && desc.extent.height == height;
	}

	std::string BuildGBufferMaterialCacheKey(
		const NLS::Render::Resources::Material& sourceMaterial,
		const NLS::Render::Data::PipelineState& pipelineState)
	{
		auto key = NLS::Render::Resources::BuildMaterialPassVariantKey(
			sourceMaterial,
			"DeferredGBuffer",
			pipelineState,
			BuildGBufferMaterialOverrides(sourceMaterial)).stableKey;
		if (sourceMaterial.path.empty())
		{
			key += "|source:";
			key += std::to_string(sourceMaterial.GetInstanceId());
		}
		return key;
	}

}

namespace NLS::Engine::Rendering
{
	DeferredSceneRenderer::DeferredSceneRenderer(NLS::Render::Context::Driver& p_driver)
		: DeferredSceneRenderer(p_driver, {})
	{
	}

	DeferredSceneRenderer::DeferredSceneRenderer(
		NLS::Render::Context::Driver& p_driver,
		ConstructionOptions options)
		: BaseSceneRenderer(p_driver)
	{
		if (options.loadPipelineResources)
			LoadPipelineResources();
	}

	void DeferredSceneRenderer::SynchronizeThreadedDeferredSnapshot(
		NLS::Render::Context::FrameSnapshot& snapshot,
		const uint64_t queuedGBufferDrawCount)
	{
		const auto totalRecordedDrawCount =
			static_cast<uint64_t>(snapshot.recordedDrawCommands.size());
		snapshot.visibleOpaqueDrawCount = (std::min)(queuedGBufferDrawCount, totalRecordedDrawCount);
	}

	bool ShouldSkipThreadedDeferredFramePublish(
		const NLS::Render::Context::FrameSnapshot& snapshot,
		const uint64_t queuedGBufferDrawCount)
	{
		return snapshot.visibleOpaqueDrawCount > 0u && queuedGBufferDrawCount == 0u;
	}

	DeferredSceneRenderer::~DeferredSceneRenderer()
	{
		m_gBufferMaterialCache.clear();
		m_lightingMaterial.reset();
		m_fullscreenQuad.reset();
		NLS::Render::Resources::Loaders::ShaderLoader::Destroy(m_lightingShader);
		NLS::Render::Resources::Loaders::ShaderLoader::Destroy(m_gBufferShader);
	}

	NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest DeferredSceneRenderer::BuildDeferredPreparedSceneResourceRequest() const
	{
		return {
			&m_gBuffer,
			m_gBufferAlbedoTexture.get(),
			m_gBufferNormalTexture.get(),
			m_gBufferMaterialTexture.get(),
			m_gBufferDepthTexture.get()
		};
	}

	void DeferredSceneRenderer::LogPreparedDrawResult(
		const char* stage,
		const bool captured,
		const bool queued,
		const PreparedRecordedDraw& preparedDraw) const
	{
		if (!NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
			return;

		NLS_LOG_INFO(
			std::string("[DeferredSceneRenderer] ") + stage +
			" captured=" + std::to_string(captured ? 1 : 0) +
			" queued=" + std::to_string(queued ? 1 : 0) +
			" pipeline=" + std::to_string(preparedDraw.pipeline != nullptr ? 1 : 0) +
			" materialBindingSet=" + std::to_string(preparedDraw.materialBindingSet != nullptr ? 1 : 0) +
			" passBindingSet=" + std::to_string(preparedDraw.passBindingSet != nullptr ? 1 : 0) +
			" frameBindingSet=" + std::to_string(preparedDraw.frameBindingSet != nullptr ? 1 : 0) +
			" objectBindingSet=" + std::to_string(preparedDraw.objectBindingSet != nullptr ? 1 : 0) +
			" mesh=" + std::to_string(preparedDraw.mesh != nullptr ? 1 : 0) +
			" vertexBuffer=" + std::to_string(preparedDraw.mesh != nullptr && preparedDraw.mesh->GetVertexBuffer() != nullptr ? 1 : 0) +
			" indexBuffer=" + std::to_string(preparedDraw.mesh != nullptr && preparedDraw.mesh->GetIndexBuffer() != nullptr ? 1 : 0) +
			" vertices=" + std::to_string(preparedDraw.mesh != nullptr ? preparedDraw.mesh->GetVertexCount() : 0u) +
			" indices=" + std::to_string(preparedDraw.mesh != nullptr ? preparedDraw.mesh->GetIndexCount() : 0u) +
			" instances=" + std::to_string(preparedDraw.instanceCount));
	}

	NLS::Render::Context::PreparedRenderSceneBuilder DeferredSceneRenderer::BuildDeferredPreparedRenderSceneBuilder(
		NLS::Render::Context::FrameSnapshot snapshot,
		const bool hasSkyboxTexture,
		std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs,
		std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedPassMetadata,
		std::shared_ptr<NLS::Render::RHI::RHITexture> preferredReadbackTexture,
		const uint64_t additionalRenderTargetUseCount) const
	{
		auto lightGridContext = BuildLightGridCompileContext(hasSkyboxTexture);
		auto frozenFrameDescriptor = FreezeDeferredPreparedFrameDescriptor(GetFrameDescriptor());
		auto frameDescriptorForBuilder = frozenFrameDescriptor.descriptor;
		lightGridContext.frameDescriptor = frozenFrameDescriptor.descriptor;
		auto externalSceneOutputAttachments =
			NLS::Render::FrameGraph::ResolveExternalSceneOutputAttachments(
				frameDescriptorForBuilder,
				"DeferredEditorOverlayColorView",
				"DeferredEditorOverlayDepthView");
		auto deferredResources = NLS::Render::FrameGraph::CaptureDeferredPreparedSceneResources(
			BuildDeferredPreparedSceneResourceRequest());

		return [snapshot = std::move(snapshot),
				frameDescriptorForBuilder,
				externalSceneOutputAttachments = std::move(externalSceneOutputAttachments),
				lightGridContext = std::move(lightGridContext),
				deferredResources = std::move(deferredResources),
				appendedPassInputs = std::move(appendedPassInputs),
				appendedPassMetadata = std::move(appendedPassMetadata),
				preferredReadbackTexture = std::move(preferredReadbackTexture),
				additionalRenderTargetUseCount,
				queuedLightingDrawCount = m_threadedQueuedLightingDrawCount,
				frozenFrameDescriptor = std::move(frozenFrameDescriptor)]() mutable
		{
			frameDescriptorForBuilder.camera = frozenFrameDescriptor.camera.get();
			lightGridContext.frameDescriptor.camera = frozenFrameDescriptor.camera.get();
			auto package = BuildSnapshotOwnedRenderScenePackage(
				snapshot,
				SnapshotRenderScenePackageBuildMode::SkipDefaultPassInputs);
			if (!package.targetsSwapchain)
			{
				NLS::Render::FrameGraph::ApplyExternalSceneOutputAttachments(
					appendedPassInputs,
					externalSceneOutputAttachments,
					{
						NLS::Render::Context::RenderPassCommandKind::Helper
					});
			}
			NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
				package,
				lightGridContext,
				deferredResources,
				std::move(appendedPassInputs),
				appendedPassMetadata,
				queuedLightingDrawCount);
			if (preferredReadbackTexture != nullptr)
				NLS::Render::FrameGraph::RegisterPreferredReadbackTexture(
					package,
					preferredReadbackTexture);
			package.renderTargetUseCount += additionalRenderTargetUseCount;
			NLS::Render::FrameGraph::FinalizePreparedDeferredScenePackage(package, frameDescriptorForBuilder);
			return package;
		};
	}

	NLS::Render::Context::PreparedRenderSceneBuilder DeferredSceneRenderer::BuildPreparedRenderSceneBuilder(
		const NLS::Render::Context::FrameSnapshot& snapshot) const
	{
		if (m_pendingPreparedRenderSceneBuilder)
			return m_pendingPreparedRenderSceneBuilder;

		bool hasSkyboxTexture = false;
		if (HasDescriptor<DeferredSceneDescriptor>())
			hasSkyboxTexture = GetDescriptor<DeferredSceneDescriptor>().hasSkyboxTexture;

		return BuildDeferredPreparedRenderSceneBuilder(
			snapshot,
			hasSkyboxTexture);
	}

	bool DeferredSceneRenderer::TryPublishThreadedFrame()
	{
		if (m_skipThreadedFramePublish)
			return false;

		return BaseSceneRenderer::TryPublishThreadedFrame();
	}

	bool DeferredSceneRenderer::IsThreadedFramePublishSkippedForCurrentFrame() const
	{
		return m_skipThreadedFramePublish;
	}

	std::shared_ptr<NLS::Render::RHI::RHITextureView>
	DeferredSceneRenderer::GetDeferredPreparedSceneDepthViewForEditorHelpers() const
	{
		return NLS::Render::FrameGraph::CaptureDeferredPreparedSceneResources(
			BuildDeferredPreparedSceneResourceRequest()).gbufferDepthView;
	}

	void DeferredSceneRenderer::BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
	{
		NLS_PROFILE_SCOPE();
		NLS_ASSERT(HasFrameObjectBindingProvider(), "DeferredSceneRenderer requires a renderer-owned frame/object binding provider.");
		BaseSceneRenderer::BeginFrame(p_frameDescriptor);

		const bool usesThreadedRendering = NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);
		uint64_t queuedGBufferDrawCount = 0u;
		uint64_t queuedLightingDrawCount = 0u;
		m_threadedQueuedGBufferDrawCount = 0u;
		m_threadedQueuedLightingDrawCount = 0u;
		m_frameGBufferMaterialSyncCount = 0u;
		m_skipThreadedFramePublish = false;

		auto drawables = ParseScene();
		const auto& frameDescriptor = GetFrameDescriptor();
		NLS::Render::Resources::TextureCube* skyboxTexture = nullptr;
		NLS::Render::Resources::Material* skyboxMaterial = nullptr;
		for (const auto& entry : drawables.skyboxes)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr)
				continue;
			if (skyboxMaterial == nullptr)
				skyboxMaterial = drawable.material;
			const auto* skyboxParameter = drawable.material->GetParameterBlock().TryGet("cubeTex");
			if (skyboxParameter != nullptr && skyboxParameter->type() == typeid(NLS::Render::Resources::TextureCube*))
			{
				skyboxTexture = std::any_cast<NLS::Render::Resources::TextureCube*>(*skyboxParameter);
				break;
			}
		}
		const bool hasSkyboxTexture = skyboxTexture != nullptr;

		if (usesThreadedRendering)
		{
			const bool preparedFrameResourcesAvailable =
				drawables.opaques.empty() ||
				TryReservePreparedFrameResourcesForThreadedCapture(*this);
			if (!preparedFrameResourcesAvailable && queuedGBufferDrawCount == 0u)
			{
				m_skipThreadedFramePublish = true;
				if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
				{
					NLS_LOG_INFO(
						"[DeferredSceneRenderer] Skipping threaded deferred capture: prepared frame resources unavailable sceneOpaqueDrawables=" +
						std::to_string(drawables.opaques.size()));
				}
			}
			else
			{
				EnsureGBufferTargets(frameDescriptor.renderWidth, frameDescriptor.renderHeight);
				SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder());

				auto gbufferPso = CreateSceneDefaultPipelineState(*this);
				for (const auto& entry : drawables.opaques)
				{
					const auto& drawable = entry.second;
					if (!drawable.material)
						continue;
					auto gbufferDrawable = drawable;
					gbufferDrawable.material = &GetOrCreateGBufferMaterial(*drawable.material);

					NLS::Render::Resources::MaterialPipelineStateOverrides gBufferOverrides;
					gBufferOverrides.depthTest = drawable.material->HasDepthTest();
					gBufferOverrides.depthWrite = drawable.material->HasDepthWriting();
					gBufferOverrides.colorWrite = true;
					gBufferOverrides.SetColorFormats(GetDeferredGBufferColorFormats());
					gBufferOverrides.culling = drawable.material->HasBackfaceCulling() || drawable.material->HasFrontfaceCulling();
					gBufferOverrides.cullFace = drawable.material->HasBackfaceCulling() && drawable.material->HasFrontfaceCulling()
						? NLS::Render::Settings::ECullFace::FRONT_AND_BACK
						: drawable.material->HasFrontfaceCulling()
							? NLS::Render::Settings::ECullFace::FRONT
							: NLS::Render::Settings::ECullFace::BACK;

					PreparedRecordedDraw preparedDraw;
					const bool captured = CaptureThreadedPreparedDraw(
						gbufferDrawable,
						gBufferOverrides,
						gbufferPso.depthFunc,
						preparedDraw);
					bool queued = false;
					if (captured)
					{
						queued = QueueThreadedRecordedDraw(preparedDraw);
						if (queued)
							++queuedGBufferDrawCount;
					}
					LogPreparedDrawResult("GBuffer", captured, queued, preparedDraw);
				}

				{
					m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>("u_GBufferAlbedo", m_gBufferAlbedoTexture.get());
					m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>("u_GBufferNormal", m_gBufferNormalTexture.get());
					m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>("u_GBufferMaterial", m_gBufferMaterialTexture.get());
					if (m_gBufferDepthTexture)
						m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>("u_GBufferDepth", m_gBufferDepthTexture.get());
					if (m_lightingMaterial->GetParameterBlock().Contains("u_SkyboxCube"))
						m_lightingMaterial->Set<NLS::Render::Resources::TextureCube*>("u_SkyboxCube", skyboxTexture);
					EnsureDeferredLightingSkyParameters(*m_lightingMaterial, skyboxMaterial);

					NLS::Render::Entities::Drawable lightingDrawable;
					lightingDrawable.mesh = m_fullscreenQuad.get();
					lightingDrawable.material = m_lightingMaterial.get();
					lightingDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;

					NLS::Render::Resources::MaterialPipelineStateOverrides compositeOverrides;
					compositeOverrides.depthTest = false;
					compositeOverrides.depthWrite = false;
					compositeOverrides.culling = false;
					compositeOverrides.colorWrite = true;

					PreparedRecordedDraw preparedDraw;
					const bool captured = CaptureThreadedPreparedDraw(
						lightingDrawable,
						compositeOverrides,
						gbufferPso.depthFunc,
						preparedDraw);
					const bool queued = captured && QueueThreadedRecordedDraw(preparedDraw);
					if (queued)
						++queuedLightingDrawCount;
					LogPreparedDrawResult("Lighting", captured, queued, preparedDraw);
					SetActivePreparedPassBindingSet(nullptr);
				}
			}

			m_threadedQueuedGBufferDrawCount = queuedGBufferDrawCount;
			m_threadedQueuedLightingDrawCount = queuedLightingDrawCount;

		}

		auto pendingFrameSnapshot = BuildFrameSnapshot(p_frameDescriptor);
		if (pendingFrameSnapshot.has_value())
		{
			RefreshFrameSnapshotVisibility(pendingFrameSnapshot.value(), drawables);
			if (usesThreadedRendering &&
				ShouldSkipThreadedDeferredFramePublish(pendingFrameSnapshot.value(), queuedGBufferDrawCount))
			{
				m_skipThreadedFramePublish = true;
			}
			if (usesThreadedRendering)
				SynchronizeThreadedDeferredSnapshot(pendingFrameSnapshot.value(), queuedGBufferDrawCount);
			if (usesThreadedRendering &&
				NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
			{
				NLS_LOG_INFO(
					"[DeferredSceneRenderer] Snapshot recordedDraws=" +
					std::to_string(pendingFrameSnapshot->recordedDrawCommands.size()) +
					" queuedGBufferDraws=" + std::to_string(queuedGBufferDrawCount) +
					" queuedLightingDraws=" + std::to_string(queuedLightingDrawCount) +
					" visibleOpaqueDraws=" + std::to_string(pendingFrameSnapshot->visibleOpaqueDrawCount) +
					" sceneOpaqueDrawables=" + std::to_string(drawables.opaques.size()));
			}
			SetPendingFrameSnapshot(pendingFrameSnapshot.value());
		}

		NLS::Render::Context::RenderScenePackage scenePackage;
		if (!usesThreadedRendering && pendingFrameSnapshot.has_value())
			scenePackage = BuildRenderScenePackage(pendingFrameSnapshot.value());

		AddDescriptor<DeferredSceneDescriptor>({
			std::move(drawables),
			std::move(scenePackage),
			hasSkyboxTexture });

	}

	void DeferredSceneRenderer::DrawFrame()
	{
		NLS_PROFILE_SCOPE();
		const bool usesThreadedRendering = NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

		// NOTE: Deferred rendering with threaded RHI requires proper GBuffer-to-Lighting
		// texture barrier handling in SubmitThreadedRhiFrame, which is not yet implemented.
		// For now, always use FrameGraph path for Deferred to ensure correct rendering.
		if (!usesThreadedRendering)
		{
			const auto& frame = GetFrameDescriptor();
			EnsureGBufferTargets(frame.renderWidth, frame.renderHeight);

			if (!m_gBufferShader || !m_lightingMaterial || !m_fullscreenQuad)
			{
				NLS_LOG_WARNING("DeferredSceneRenderer is missing shader or mesh resources; skipping deferred frame.");
				return;
			}

			FrameGraph frameGraph;
			FrameGraphBlackboard blackboard;
			const auto deferredResourceRequest = BuildDeferredPreparedSceneResourceRequest();
			const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
			const auto lightGridContext = BuildLightGridCompileContext(scene.hasSkyboxTexture);
			NLS::Render::FrameGraph::DeferredGraphSceneResourceRequest resourceRequest;
			{
				NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BuildGraphResourceRequest");
				resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
					frameGraph,
					blackboard,
					frame,
					deferredResourceRequest);
			}
			{
				NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::ReserveDeferredSceneGraph");
				NLS::Render::FrameGraph::ReserveDeferredSceneGraph(frameGraph, resourceRequest);
			}
			NLS::Render::FrameGraph::PreparedDeferredSceneGraph preparedGraph;
			{
				NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::PrepareDeferredSceneGraph");
				preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
					resourceRequest,
					lightGridContext);
			}
			{
				NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::ExecutePreparedDeferredSceneGraph");
				NLS::Render::FrameGraph::ExecutePreparedDeferredSceneGraph(
					frameGraph,
					preparedGraph,
					{
						[this](const auto& beginDesc) -> bool
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginGBufferPass");
							return BeginRecordedRenderPass(
								&m_gBuffer,
								beginDesc.renderWidth,
								beginDesc.renderHeight,
								beginDesc.clearColor,
								beginDesc.clearDepth,
								beginDesc.clearStencil,
								beginDesc.clearValue);
						},
						[this]()
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::DrawGBufferOpaquesCallback");
							DrawGBufferOpaques(CreateSceneDefaultPipelineState(*this));
						},
						[this]()
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::EndGBufferPass");
							EndRecordedRenderPass();
						},
						[this](const auto& beginDesc) -> bool
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginLightingPass");
							return BeginOutputRenderPass(
								beginDesc.renderWidth,
								beginDesc.renderHeight,
								beginDesc.clearColor,
								beginDesc.clearDepth,
								beginDesc.clearStencil,
								beginDesc.clearValue);
						},
						[this]()
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::DrawLightingPassCallback");
							DrawLightingPass(CreateSceneFullscreenCompositePipelineState(*this));
						},
						[this](bool startedRenderPass, const auto& endDesc)
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::EndLightingPass");
							(void)endDesc;
							EndOutputRenderPass(startedRenderPass);
						}
					});
			}

			{
				NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::FrameGraphCompile");
				frameGraph.compile();
			}
			auto executionContext = CreateFrameGraphExecutionContext();
			{
				NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::FrameGraphExecute");
				frameGraph.execute(&executionContext, &executionContext);
			}
		}

		DrawRegisteredPasses();

		if (usesThreadedRendering)
		{
			const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
			auto pendingFrameSnapshot = BuildFrameSnapshot(m_frameDescriptor);
			if (pendingFrameSnapshot.has_value())
			{
				RefreshFrameSnapshotVisibility(pendingFrameSnapshot.value(), scene.drawables);
				SynchronizeThreadedDeferredSnapshot(pendingFrameSnapshot.value(), m_threadedQueuedGBufferDrawCount);
				SetPendingFrameSnapshot(pendingFrameSnapshot.value());
			}
		}
	}

	void DeferredSceneRenderer::LoadPipelineResources()
	{
		NLS_PROFILE_SCOPE();
		using ShaderLoader = NLS::Render::Resources::Loaders::ShaderLoader;
		const auto& projectAssetsRoot = NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();

		m_gBufferShader = ShaderLoader::Create(ResolveEngineShaderPath("DeferredGBuffer.hlsl"), projectAssetsRoot);
		m_lightingShader = ShaderLoader::Create(ResolveEngineShaderPath("DeferredLighting.hlsl"), projectAssetsRoot);

		if (m_lightingShader)
		{
			m_lightingMaterial = std::make_unique<NLS::Render::Resources::Material>(m_lightingShader);
		}

		std::vector<NLS::Render::Geometry::Vertex> vertices{
			MakeFullscreenVertex(-1.0f, -1.0f, 0.0f, 1.0f),
			MakeFullscreenVertex(-1.0f,  1.0f, 0.0f, 0.0f),
			MakeFullscreenVertex( 1.0f,  1.0f, 1.0f, 0.0f),
			MakeFullscreenVertex( 1.0f, -1.0f, 1.0f, 1.0f)
		};
		std::vector<uint32_t> indices{ 0, 1, 2, 0, 2, 3 };
		m_fullscreenQuad = std::make_unique<NLS::Render::Resources::Mesh>(vertices, indices, 0);
	}

	void DeferredSceneRenderer::EnsureGBufferTargets(uint16_t width, uint16_t height)
	{
		NLS_PROFILE_SCOPE();
		static const auto kAttachments = []()
		{
			std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> attachments;
			attachments.reserve(NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount);
			for (const auto& slot : NLS::Render::FrameGraph::kDeferredGBufferColorSlots)
				attachments.push_back({ slot.format, slot.usage });
			return attachments;
		}();
		static constexpr NLS::Render::Buffers::MultiFramebuffer::DepthAttachmentDesc kDepthAttachment{
			NLS::Render::FrameGraph::kDeferredGBufferDepthFormat,
			NLS::Render::FrameGraph::kDeferredGBufferDepthUsage
		};

		if (!m_gBuffer.IsInitialized())
			m_gBuffer.Init(width, height, kAttachments, true, kDepthAttachment);
		else
			m_gBuffer.Resize(width, height);

		const auto& colorResources = m_gBuffer.GetExplicitColorTextureHandles();
		const auto& depthResource = m_gBuffer.GetExplicitDepthTextureHandle();
		bool targetsValid =
			colorResources.size() >= NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount &&
			TextureResourceMatchesSize(depthResource, width, height);
		for (size_t i = 0u; targetsValid && i < NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount; ++i)
			targetsValid = TextureResourceMatchesSize(colorResources[i], width, height);
		if (!targetsValid)
		{
			m_gBufferAlbedoTexture.reset();
			m_gBufferNormalTexture.reset();
			m_gBufferMaterialTexture.reset();
			m_gBufferDepthTexture.reset();
			return;
		}

		std::array<std::unique_ptr<NLS::Render::Resources::Texture2D>*, NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount> colorWrappers = {
			&m_gBufferAlbedoTexture,
			&m_gBufferNormalTexture,
			&m_gBufferMaterialTexture
		};
		bool wrappersMatch = WrappedTextureMatches(m_gBufferDepthTexture, depthResource, width, height);
		for (size_t i = 0u; wrappersMatch && i < colorWrappers.size(); ++i)
			wrappersMatch = WrappedTextureMatches(*colorWrappers[i], colorResources[i], width, height);
		if (wrappersMatch)
		{
			return;
		}

		auto wrapTexture = [width, height](const std::shared_ptr<NLS::Render::RHI::RHITexture>& textureResource) -> std::unique_ptr<NLS::Render::Resources::Texture2D>
		{
			if (!textureResource)
				return nullptr;
			return NLS::Render::Resources::Texture2D::WrapExternal(textureResource, width, height);
		};

		for (size_t i = 0u; i < colorWrappers.size(); ++i)
			*colorWrappers[i] = wrapTexture(colorResources[i]);
		m_gBufferDepthTexture = wrapTexture(depthResource);
	}

	std::unique_ptr<NLS::Render::Resources::Material> DeferredSceneRenderer::CreateGBufferMaterial() const
	{
		auto material = std::make_unique<NLS::Render::Resources::Material>(m_gBufferShader);
		material->SetBlendable(false);
		material->SetColorWriting(true);
		return material;
	}

	NLS::Render::Resources::Material& DeferredSceneRenderer::GetOrCreateGBufferMaterial(NLS::Render::Resources::Material& sourceMaterial)
	{
		NLS_PROFILE_SCOPE();
		const NLS::Render::Data::PipelineState pipelineState;
		const auto cacheKey = BuildGBufferMaterialCacheKey(sourceMaterial, pipelineState);
		auto found = m_gBufferMaterialCache.find(cacheKey);
		if (found == m_gBufferMaterialCache.end())
		{
			GBufferMaterialCacheEntry entry;
			entry.material = CreateGBufferMaterial();
			found = m_gBufferMaterialCache.emplace(cacheKey, std::move(entry)).first;
		}

		auto& entry = found->second;
		const auto sourceStamp = BuildGBufferMaterialSyncStamp(sourceMaterial);
		if (entry.material != nullptr && entry.syncedStamp != sourceStamp)
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::SyncGBufferMaterial");
			SyncGBufferMaterial(*entry.material, sourceMaterial);
			entry.syncedStamp = sourceStamp;
			++entry.syncCount;
			++m_frameGBufferMaterialSyncCount;
			m_rendererStats.RecordGBufferMaterialSync();
		}
		return *entry.material;
	}

	void DeferredSceneRenderer::SyncGBufferMaterial(NLS::Render::Resources::Material& target, const NLS::Render::Resources::Material& sourceMaterial) const
	{
		target.SetGPUInstances(sourceMaterial.GetGPUInstances());

		target.FillUniform();
		for (const auto& [name, value] : sourceMaterial.GetParameterBlock().Data())
		{
			if (target.GetParameterBlock().Contains(name))
				SetMaterialParameter(target, name.c_str(), value);
		}

		CopyMaterialParameterIfPresent(target, "u_Albedo", sourceMaterial, "u_Diffuse");
		CopyMaterialParameterIfPresent(target, "u_AlbedoMap", sourceMaterial, "u_DiffuseMap");
		EnsureDeferredGBufferFallbackParameters(target);
	}

	void DeferredSceneRenderer::DrawGBufferOpaques(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		if (!m_gBufferShader)
			return;

		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();

		for (const auto& entry : scene.drawables.opaques)
		{
			const auto& drawable = entry.second;
			if (!drawable.material)
				continue;

			auto gbufferDrawable = drawable;
			gbufferDrawable.material = &GetOrCreateGBufferMaterial(*drawable.material);

			NLS::Render::Resources::MaterialPipelineStateOverrides gBufferOverrides;
			gBufferOverrides.depthTest = drawable.material->HasDepthTest();
			gBufferOverrides.depthWrite = drawable.material->HasDepthWriting();
			gBufferOverrides.colorWrite = true;
			gBufferOverrides.SetColorFormats(GetDeferredGBufferColorFormats());
			gBufferOverrides.culling = drawable.material->HasBackfaceCulling() || drawable.material->HasFrontfaceCulling();
			gBufferOverrides.cullFace = drawable.material->HasBackfaceCulling() && drawable.material->HasFrontfaceCulling()
				? NLS::Render::Settings::ECullFace::FRONT_AND_BACK
				: drawable.material->HasFrontfaceCulling()
					? NLS::Render::Settings::ECullFace::FRONT
					: NLS::Render::Settings::ECullFace::BACK;

			DrawEntity(gbufferDrawable, gBufferOverrides, pso.depthFunc);
		}
	}

	void DeferredSceneRenderer::DrawLightingPass(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		if (!m_lightingMaterial || !m_fullscreenQuad || !m_gBufferAlbedoTexture || !m_gBufferNormalTexture || !m_gBufferMaterialTexture)
			return;

		NLS::Render::Resources::TextureCube* skyboxTexture = nullptr;
		NLS::Render::Resources::Material* skyboxMaterial = nullptr;
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
		for (const auto& entry : scene.drawables.skyboxes)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr)
				continue;
			if (skyboxMaterial == nullptr)
				skyboxMaterial = drawable.material;

			const auto* skyboxParameter = drawable.material->GetParameterBlock().TryGet("cubeTex");
			if (skyboxParameter != nullptr && skyboxParameter->type() == typeid(NLS::Render::Resources::TextureCube*))
			{
				skyboxTexture = std::any_cast<NLS::Render::Resources::TextureCube*>(*skyboxParameter);
				break;
			}
		}

		m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>("u_GBufferAlbedo", m_gBufferAlbedoTexture.get());
		m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>("u_GBufferNormal", m_gBufferNormalTexture.get());
		m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>("u_GBufferMaterial", m_gBufferMaterialTexture.get());
		if (m_gBufferDepthTexture)
			m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>("u_GBufferDepth", m_gBufferDepthTexture.get());
		if (m_lightingMaterial->GetParameterBlock().Contains("u_SkyboxCube"))
			m_lightingMaterial->Set<NLS::Render::Resources::TextureCube*>("u_SkyboxCube", skyboxTexture);
		EnsureDeferredLightingSkyParameters(*m_lightingMaterial, skyboxMaterial);

		auto commandBuffer = GetActiveExplicitCommandBuffer();
		auto device = GetExplicitDevice();
		auto pipelineCache = NLS::Render::Context::DriverRendererAccess::GetPipelineCache(GetDriver());

		NLS::Render::Entities::Drawable lightingDrawable;
		lightingDrawable.mesh = m_fullscreenQuad.get();
		lightingDrawable.material = m_lightingMaterial.get();
		lightingDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;

		NLS::Render::Resources::MaterialPipelineStateOverrides compositeOverrides;
		compositeOverrides.depthTest = false;
		compositeOverrides.depthWrite = false;
		compositeOverrides.culling = false;
		compositeOverrides.colorWrite = true;

		auto material = lightingDrawable.material;
		auto mesh = lightingDrawable.mesh;

		auto pipeline = material->BuildRecordedGraphicsPipeline(
			device, pipelineCache, lightingDrawable.primitiveMode, pso, compositeOverrides);
		auto bindingSet = material->GetRecordedBindingSet(device);
		auto rhiMesh = mesh->GetRHIMesh();
		commandBuffer->BindGraphicsPipeline(pipeline);
		if (GetLightGridGraphicsPassBindingSet() != nullptr)
			commandBuffer->BindBindingSet(NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, GetLightGridGraphicsPassBindingSet());
		commandBuffer->BindBindingSet(NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet, bindingSet);
		SubmitMeshDraw(commandBuffer, rhiMesh, material->GetGPUInstances());
	}

	NLS::Render::Resources::Material& DeferredSceneRendererTestAccess::GetOrCreateGBufferMaterial(
		DeferredSceneRenderer& renderer,
		NLS::Render::Resources::Material& sourceMaterial)
	{
		return renderer.GetOrCreateGBufferMaterial(sourceMaterial);
	}

	DeferredSceneRendererTestAccess::GBufferMaterialCache& DeferredSceneRendererTestAccess::GetGBufferMaterialCache(
		DeferredSceneRenderer& renderer)
	{
		return renderer.m_gBufferMaterialCache;
	}

	const DeferredSceneRendererTestAccess::GBufferMaterialCache& DeferredSceneRendererTestAccess::GetGBufferMaterialCache(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_gBufferMaterialCache;
	}

	void DeferredSceneRendererTestAccess::SetGBufferShader(
		DeferredSceneRenderer& renderer,
		NLS::Render::Resources::Shader* shader)
	{
		renderer.m_gBufferShader = shader;
	}

	NLS::Render::Resources::Shader* DeferredSceneRendererTestAccess::GetGBufferShader(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_gBufferShader;
	}

	void DeferredSceneRendererTestAccess::ResetFrameGBufferMaterialSyncCount(
		DeferredSceneRenderer& renderer)
	{
		renderer.m_frameGBufferMaterialSyncCount = 0u;
	}

	uint64_t DeferredSceneRendererTestAccess::GetFrameGBufferMaterialSyncCount(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_frameGBufferMaterialSyncCount;
	}

	void DeferredSceneRendererTestAccess::EnsureGBufferTargets(
		DeferredSceneRenderer& renderer,
		const uint16_t width,
		const uint16_t height)
	{
		renderer.EnsureGBufferTargets(width, height);
	}

	const NLS::Render::Resources::Texture2D* DeferredSceneRendererTestAccess::GetGBufferAlbedoTexture(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_gBufferAlbedoTexture.get();
	}

	const NLS::Render::Resources::Texture2D* DeferredSceneRendererTestAccess::GetGBufferNormalTexture(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_gBufferNormalTexture.get();
	}

	const NLS::Render::Resources::Texture2D* DeferredSceneRendererTestAccess::GetGBufferMaterialTexture(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_gBufferMaterialTexture.get();
	}

	const NLS::Render::Resources::Texture2D* DeferredSceneRendererTestAccess::GetGBufferDepthTexture(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_gBufferDepthTexture.get();
	}
}
