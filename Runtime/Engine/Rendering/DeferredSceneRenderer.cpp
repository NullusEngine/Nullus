#include "Rendering/DeferredSceneRenderer.h"

#include <fg/Blackboard.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include <Debug/Logger.h>
#include <Math/Matrix4.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Data/SceneOcclusionPacketLayout.h>
#include <Rendering/Data/LightingDescriptor.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/FrameGraph/FrameGraphExecutionPlan.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Rendering/Geometry/Vertex.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/RHI/Core/RHIPipeline.h>
#include <Rendering/RHI/Core/RHIResource.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/ComputeShaderUtils.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/MaterialVariantKey.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/ShaderParameterStruct.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/TextureCube.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>
#include <Rendering/Settings/EPrimitiveMode.h>
#include <Rendering/SceneOcclusion.h>
#include <Rendering/Shaders/HZBShaders.h>
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

	uint32_t CalculateHZBMipCount(const uint32_t width, const uint32_t height)
	{
		uint32_t mipCount = 1u;
		uint32_t mipWidth = (std::max)(width, 1u);
		uint32_t mipHeight = (std::max)(height, 1u);
		while (mipWidth > 1u || mipHeight > 1u)
		{
			mipWidth = (std::max)(1u, (mipWidth + 1u) / 2u);
			mipHeight = (std::max)(1u, (mipHeight + 1u) / 2u);
			++mipCount;
		}
		return mipCount;
	}

	std::array<uint32_t, 3u> CalculateHZBBuildDispatchGroupsForMip(
		const uint32_t width,
		const uint32_t height,
		const uint32_t mip)
	{
		const uint32_t mipScale = 1u << mip;
		const uint32_t mipWidth = (std::max)(1u, (width + mipScale - 1u) / mipScale);
		const uint32_t mipHeight = (std::max)(1u, (height + mipScale - 1u) / mipScale);
		return {
			(mipWidth + 7u) / 8u,
			(mipHeight + 7u) / 8u,
			1u
		};
	}

	struct HZBOcclusionConstants
	{
		uint32_t primitiveCount = 0u;
		uint32_t padding[3] = {};
	};

	static_assert(sizeof(HZBOcclusionConstants) == 16u);

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

		lightingMaterial.Set<bool>("u_UseProceduralSky", false);
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

	bool IsDeviceLostReadbackFailure(const NLS::Render::RHI::RHIReadbackResult& result)
	{
		if (result.Succeeded())
			return false;

		return result.code == NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost ||
			result.message.find("device removed") != std::string::npos ||
			result.message.find("device removed/lost") != std::string::npos ||
			result.message.find("device is lost") != std::string::npos;
	}

	bool IsTransientHZBReadbackBusyFailure(const std::string& message)
	{
		return message.find("previous async readback has not been completed") != std::string::npos;
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

	NLS::Render::Resources::MaterialPipelineStateOverrides BuildDeferredDecalMaterialOverrides(
		const NLS::Render::Resources::Material& sourceMaterial)
	{
		auto overrides = BuildGBufferMaterialOverrides(sourceMaterial);
		NLS::Render::RHI::RHIRenderTargetBlendStateDesc blendedTarget;
		blendedTarget.blendEnable = true;
		blendedTarget.colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::All;
		NLS::Render::RHI::RHIRenderTargetBlendStateDesc suppressedTarget;
		suppressedTarget.blendEnable = false;
		suppressedTarget.colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::None;
		std::array<
			NLS::Render::RHI::RHIRenderTargetBlendStateDesc,
			NLS::Render::FrameGraph::kDeferredGBufferColorAttachmentCount> decalTargets {};
		decalTargets.fill(suppressedTarget);
		decalTargets[NLS::Render::FrameGraph::kDeferredGBufferAlbedoColorAttachmentIndex] = blendedTarget;
		overrides.depthTest = true;
		overrides.depthWrite = false;
		overrides.blending = true;
		overrides.hasDepthAttachment = true;
		overrides.stencilTest = false;
		overrides.stencilWriteMask = 0u;
		overrides.SetRenderTargetBlendStates(decalTargets);
		return overrides;
	}

	NLS::Render::Settings::EComparaisonAlgorithm GetDeferredDecalDepthCompare()
	{
		return NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
	}

	NLS::Engine::Rendering::DeferredSceneRenderer::GBufferMaterialSyncStamp BuildGBufferMaterialSyncStamp(
		const NLS::Render::Resources::Material& sourceMaterial)
	{
		const auto* shader = sourceMaterial.GetShader();
		return {
			sourceMaterial.GetInstanceId(),
			sourceMaterial.GetParameterRevision(),
			sourceMaterial.GetRenderStateRevision(),
			sourceMaterial.GetBindingRevision(),
			shader != nullptr ? shader->GetInstanceId() : 0u,
			shader != nullptr ? shader->GetGeneration() : 0u
		};
	}

	bool operator==(
		const NLS::Engine::Rendering::DeferredSceneRenderer::GBufferMaterialSyncStamp& lhs,
		const NLS::Engine::Rendering::DeferredSceneRenderer::GBufferMaterialSyncStamp& rhs)
	{
		return lhs.sourceMaterialInstanceId == rhs.sourceMaterialInstanceId &&
			lhs.parameterRevision == rhs.parameterRevision &&
			lhs.renderStateRevision == rhs.renderStateRevision &&
			lhs.bindingRevision == rhs.bindingRevision &&
			lhs.shaderInstanceId == rhs.shaderInstanceId &&
			lhs.shaderGeneration == rhs.shaderGeneration;
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
		const auto& parameters = sourceMaterial.GetParameterBlock().Data();
		const bool hasRuntimeTextureBinding = std::any_of(
			parameters.begin(),
			parameters.end(),
			[](const auto& entry)
			{
				return entry.second.type() == typeid(NLS::Render::Resources::Texture2D*) &&
					std::any_cast<NLS::Render::Resources::Texture2D*>(entry.second) != nullptr;
			});
		if (sourceMaterial.path.empty() || hasRuntimeTextureBinding)
		{
			key += "|source:";
			key += std::to_string(sourceMaterial.GetInstanceId());
		}
		return key;
	}

	const char* ToHZBFallbackReasonName(
		const NLS::Engine::Rendering::SceneOcclusionFallbackReason reason)
	{
		using NLS::Engine::Rendering::SceneOcclusionFallbackReason;
		switch (reason)
		{
		case SceneOcclusionFallbackReason::None: return "None";
		case SceneOcclusionFallbackReason::Disabled: return "Disabled";
		case SceneOcclusionFallbackReason::BackendUnsupported: return "BackendUnsupported";
		case SceneOcclusionFallbackReason::HZBUnsupported: return "HZBUnsupported";
		case SceneOcclusionFallbackReason::OcclusionUnsupported: return "OcclusionUnsupported";
		case SceneOcclusionFallbackReason::AsyncReadbackUnsupported: return "AsyncReadbackUnsupported";
		case SceneOcclusionFallbackReason::OpaqueDepthTextureUnsupported: return "OpaqueDepthTextureUnsupported";
		case SceneOcclusionFallbackReason::HZBTextureUnsupported: return "HZBTextureUnsupported";
		case SceneOcclusionFallbackReason::HistoryTextureInvalid: return "HistoryTextureInvalid";
		case SceneOcclusionFallbackReason::MissingHistory: return "MissingHistory";
		case SceneOcclusionFallbackReason::HistoryTooOld: return "HistoryTooOld";
		case SceneOcclusionFallbackReason::ViewChanged: return "ViewChanged";
		case SceneOcclusionFallbackReason::PrimitiveChanged: return "PrimitiveChanged";
		case SceneOcclusionFallbackReason::RepresentationChanged: return "RepresentationChanged";
		case SceneOcclusionFallbackReason::DepthWriteIneligible: return "DepthWriteIneligible";
		default: return "Unknown";
		}
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
		const uint64_t queuedGBufferDrawCount,
		const uint64_t queuedDecalDrawCount,
		const uint64_t queuedLightingDrawCount,
		const uint64_t queuedTransparentDrawCount)
	{
		const auto totalRecordedDrawCount =
			static_cast<uint64_t>(snapshot.recordedDrawCommands.size());
		snapshot.visibleOpaqueDrawCount =
			(std::min)(queuedGBufferDrawCount, totalRecordedDrawCount);
		const auto recordedDrawsAfterOpaque =
			totalRecordedDrawCount > snapshot.visibleOpaqueDrawCount
				? totalRecordedDrawCount - snapshot.visibleOpaqueDrawCount
				: 0u;
		snapshot.visibleDecalDrawCount =
			(std::min)(queuedDecalDrawCount, recordedDrawsAfterOpaque);
		const auto recordedDrawsAfterDecal =
			recordedDrawsAfterOpaque > snapshot.visibleDecalDrawCount
				? recordedDrawsAfterOpaque - snapshot.visibleDecalDrawCount
				: 0u;
		const auto recordedLightingDrawCount =
			(std::min)(queuedLightingDrawCount, recordedDrawsAfterDecal);
		const auto recordedDrawsAfterLighting =
			recordedDrawsAfterDecal > recordedLightingDrawCount
				? recordedDrawsAfterDecal - recordedLightingDrawCount
				: 0u;
		snapshot.visibleTransparentDrawCount =
			(std::min)(queuedTransparentDrawCount, recordedDrawsAfterLighting);
	}

	bool ShouldSkipThreadedDeferredFramePublish(
		const NLS::Render::Context::FrameSnapshot& snapshot,
		const uint64_t queuedGBufferDrawCount,
		const uint64_t queuedDecalDrawCount,
		const uint64_t queuedLightingDrawCount,
		const uint64_t queuedTransparentDrawCount)
	{
		const bool requiresDeferredLighting =
			snapshot.visibleOpaqueDrawCount > 0u ||
			snapshot.visibleDecalDrawCount > 0u ||
			snapshot.visibleTransparentDrawCount > 0u ||
			snapshot.visibleSkyboxDrawCount > 0u;
		return queuedGBufferDrawCount != snapshot.visibleOpaqueDrawCount ||
			queuedDecalDrawCount != snapshot.visibleDecalDrawCount ||
			(requiresDeferredLighting && queuedLightingDrawCount != 1u) ||
			(!requiresDeferredLighting && queuedLightingDrawCount > 1u) ||
			queuedTransparentDrawCount != snapshot.visibleTransparentDrawCount;
	}

	DeferredSceneRenderer::~DeferredSceneRenderer()
	{
		m_gBufferMaterialCache.clear();
		m_hzbOcclusionBindingSet.reset();
		m_hzbBuildBindingSets.clear();
		m_hzbOcclusionPipeline.reset();
		m_hzbBuildPipeline.reset();
		m_hzbOcclusionPipelineLayout.reset();
		m_hzbBuildPipelineLayout.reset();
		m_hzbOcclusionBindingLayout.reset();
		m_hzbBuildBindingLayout.reset();
		m_hzbReadView.reset();
		m_hzbDepthReadView.reset();
		m_hzbMipReadViews.clear();
		m_hzbMipWriteViews.clear();
		m_hzbPreparedHZBTexture.reset();
		m_hzbPreparedDepthTexture.reset();
		m_hzbTexture.reset();
		m_lightingMaterial.reset();
		m_fullscreenQuad.reset();
		NLS::Render::Resources::Loaders::ShaderLoader::Destroy(m_hzbOcclusionShader);
		NLS::Render::Resources::Loaders::ShaderLoader::Destroy(m_hzbBuildShader);
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
		const uint64_t preferredReadbackTextureGeneration,
		const uint64_t additionalRenderTargetUseCount,
		std::optional<NLS::Render::Context::PostSubmitBufferReadbackRequest> hzbPostSubmitReadback) const
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
		const auto hzbSource = NLS::Render::FrameGraph::BuildHZBPreparedComputeDispatchSource(
			BuildHZBFrameResourceRequest());
		if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
		{
			NLS_LOG_INFO(
				"[DeferredSceneRenderer][HZBReadback] prepared builder hzbDispatches=" +
				std::to_string(hzbSource.dispatchInputs.size()) +
				" hasReadback=" +
				std::to_string(hzbPostSubmitReadback.has_value() ? 1 : 0));
		}
		return [snapshot = std::move(snapshot),
				frameDescriptorForBuilder,
				externalSceneOutputAttachments = std::move(externalSceneOutputAttachments),
				lightGridContext = std::move(lightGridContext),
				deferredResources = std::move(deferredResources),
				hzbSource,
				hzbPostSubmitReadback = std::move(hzbPostSubmitReadback),
				appendedPassInputs = std::move(appendedPassInputs),
				appendedPassMetadata = std::move(appendedPassMetadata),
				preferredReadbackTexture = std::move(preferredReadbackTexture),
				preferredReadbackTextureGeneration,
				additionalRenderTargetUseCount,
				queuedDrawCounts = [this]()
				{
					NLS::Render::FrameGraph::DeferredPreparedQueuedDrawCounts counts;
					counts.lightingDrawCount = m_threadedQueuedLightingDrawCount;
					counts.decalDrawCount = m_threadedQueuedDecalDrawCount;
					counts.transparentDrawCount = m_threadedQueuedTransparentDrawCount;
					return counts;
				}(),
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
				queuedDrawCounts,
				hzbSource);
			if (preferredReadbackTexture != nullptr)
				NLS::Render::FrameGraph::RegisterPreferredReadbackTexture(
					package,
					preferredReadbackTexture,
					preferredReadbackTextureGeneration);
			if (hzbPostSubmitReadback.has_value())
				package.postSubmitBufferReadbacks.push_back(hzbPostSubmitReadback.value());
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

		const bool published = BaseSceneRenderer::TryPublishThreadedFrame();
		if (!published && m_threadedHZBPostSubmitReadback.has_value())
		{
			DiscardPendingHZBOcclusionObservationFrame();
			m_threadedHZBPostSubmitReadback.reset();
		}
		else if (!published)
		{
			DiscardHZBObservationIfNoReadbackWasPublished();
		}
		else if (published && m_threadedHZBPostSubmitReadback.has_value())
		{
			AdoptHZBPostSubmitReadbackRequest(m_threadedHZBPostSubmitReadback.value());
			m_threadedHZBPostSubmitReadback.reset();
		}
		return published;
	}

	void DeferredSceneRenderer::OnThreadedFramePublishFailed()
	{
		BaseSceneRenderer::OnThreadedFramePublishFailed();
		if (m_hzbOcclusionResultReadbackState != nullptr)
			return;
		if (!m_threadedHZBPostSubmitReadback.has_value())
			return;

		if (m_hzbOcclusionResultReadbackState == nullptr)
		{
			DiscardPendingHZBOcclusionObservationFrame();
			m_threadedHZBPostSubmitReadback.reset();
		}
	}

	bool DeferredSceneRenderer::IsThreadedFramePublishSkippedForCurrentFrame() const
	{
		return m_skipThreadedFramePublish;
	}

	std::optional<NLS::Render::Context::PostSubmitBufferReadbackRequest>
	DeferredSceneRenderer::GetThreadedHZBPostSubmitReadbackForPreparedBuilder() const
	{
		return m_threadedHZBPostSubmitReadback;
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
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::PollHZBOcclusionResultReadback");
			PollHZBOcclusionResultReadback();
		}
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::BaseSceneRendererBeginFrame");
			BaseSceneRenderer::BeginFrame(p_frameDescriptor);
		}

		const bool usesThreadedRendering = NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);
		uint64_t queuedGBufferDrawCount = 0u;
		uint64_t queuedDecalDrawCount = 0u;
		uint64_t queuedLightingDrawCount = 0u;
		uint64_t queuedTransparentDrawCount = 0u;
		m_threadedQueuedGBufferDrawCount = 0u;
		m_threadedQueuedDecalDrawCount = 0u;
		m_threadedQueuedLightingDrawCount = 0u;
		m_threadedQueuedTransparentDrawCount = 0u;
		m_frameGBufferMaterialSyncCount = 0u;
		m_skipThreadedFramePublish = false;
		m_threadedHZBPostSubmitReadback.reset();
		ClearFrameGBufferMaterialResolveCache();

		auto drawables = [&]()
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::ParseScene");
			return ParseScene();
		}();
		const auto& hzbPacketBuild = GetLastHZBOcclusionPrimitivePacketBuildResult();
		const auto& hzbOcclusionFrameInput = GetLastHZBOcclusionFrameInput();
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::PrepareHZBOcclusionObservation");
			if (hzbOcclusionFrameInput.enabled &&
				hzbOcclusionFrameInput.backendSupported &&
				hzbOcclusionFrameInput.historyTextureValid &&
				!HasPendingHZBOcclusionObservationFrame() &&
				m_hzbOcclusionResultReadbackCompletion == nullptr &&
				m_hzbOcclusionResultReadbackState == nullptr &&
				!hzbPacketBuild.primitiveInputs.empty() &&
				PrepareHZBOcclusionPrimitiveBuffers(hzbPacketBuild.primitivePackets))
			{
				BeginHZBOcclusionObservationFrame(
					hzbOcclusionFrameInput,
					hzbPacketBuild.primitiveInputs);
			}
		}
		const auto& frameDescriptor = GetFrameDescriptor();
		NLS::Render::Resources::TextureCube* skyboxTexture = nullptr;
		NLS::Render::Resources::Material* skyboxMaterial = nullptr;
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::ResolveSkyboxTexture");
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
		}
		const bool hasSkyboxTexture = skyboxTexture != nullptr;
		if (usesThreadedRendering)
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::ThreadedCapture");
			const bool hasPreparedSceneDrawables =
				!drawables.opaques.empty() ||
				!drawables.decals.empty() ||
				!drawables.transparents.empty();
			const bool preparedFrameResourcesAvailable =
				!hasPreparedSceneDrawables ||
				TryReservePreparedFrameResourcesForThreadedCapture(*this);
			if (!preparedFrameResourcesAvailable && queuedGBufferDrawCount == 0u)
			{
				m_skipThreadedFramePublish = true;
				DiscardHZBObservationIfNoReadbackWasPublished();
				if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
				{
					NLS_LOG_INFO(
						"[DeferredSceneRenderer] Skipping threaded deferred capture: prepared frame resources unavailable sceneOpaqueDrawables=" +
						std::to_string(drawables.opaques.size()) +
						" sceneDecalDrawables=" + std::to_string(drawables.decals.size()) +
						" sceneTransparentDrawables=" + std::to_string(drawables.transparents.size()));
				}
			}
			else
			{
				{
					NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::EnsureGBufferTargets");
					EnsureGBufferTargets(frameDescriptor.renderWidth, frameDescriptor.renderHeight);
				}
				if (!HasDeferredThreadedPipelineResources())
				{
					m_skipThreadedFramePublish = true;
					DiscardHZBObservationIfNoReadbackWasPublished();
					if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
					{
						NLS_LOG_INFO(
							"[DeferredSceneRenderer] Skipping threaded deferred capture: pipeline resources unavailable"
							" gBufferShader=" + std::to_string(m_gBufferShader != nullptr ? 1 : 0) +
							" lightingMaterial=" + std::to_string(m_lightingMaterial != nullptr ? 1 : 0) +
							" fullscreenQuad=" + std::to_string(m_fullscreenQuad != nullptr ? 1 : 0) +
							" gBufferAlbedo=" + std::to_string(m_gBufferAlbedoTexture != nullptr ? 1 : 0) +
							" gBufferNormal=" + std::to_string(m_gBufferNormalTexture != nullptr ? 1 : 0) +
							" gBufferMaterial=" + std::to_string(m_gBufferMaterialTexture != nullptr ? 1 : 0) +
							" gBufferDepth=" + std::to_string(m_gBufferDepthTexture != nullptr ? 1 : 0));
					}
				}
				else
				{
					{
						NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::PrepareHZBFrameResources");
						PrepareHZBFrameResources(BuildDeferredPreparedSceneResourceRequest());
						m_threadedHZBPostSubmitReadback = BuildHZBPostSubmitReadbackRequest(true);
					}
					SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder());

					auto gbufferPso = CreateSceneDefaultPipelineState(*this);
					{
						NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::CaptureGBufferOpaques");
						for (const auto& entry : drawables.opaques)
						{
							const auto& drawable = entry.second;
							if (!drawable.material)
								continue;
							auto gbufferDrawable = drawable;
							gbufferDrawable.material = &ResolveFrameGBufferMaterial(*drawable.material);

							const auto gBufferOverrides = BuildGBufferMaterialOverrides(*drawable.material);

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
					}

					{
						NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::CaptureDecals");
						for (const auto& entry : drawables.decals)
						{
							const auto& drawable = entry.second;
							if (!drawable.material)
								continue;

							auto gbufferDrawable = drawable;
							gbufferDrawable.material = &ResolveFrameGBufferMaterial(*drawable.material);

							const auto decalOverrides = BuildDeferredDecalMaterialOverrides(*drawable.material);

							PreparedRecordedDraw preparedDraw;
							const bool captured = CaptureThreadedPreparedDraw(
								gbufferDrawable,
								decalOverrides,
								GetDeferredDecalDepthCompare(),
								preparedDraw);
							bool queued = false;
							if (captured)
							{
								queued = QueueThreadedRecordedDraw(preparedDraw);
								if (queued)
									++queuedDecalDrawCount;
							}
							LogPreparedDrawResult("Decal", captured, queued, preparedDraw);
						}
					}

					{
						NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::CaptureLighting");
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
					}

					{
						NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::CaptureTransparents");
						NLS::Render::Resources::MaterialPipelineStateOverrides transparentOverrides;
						transparentOverrides.depthWrite = false;
						auto transparentPso = CreateSceneDefaultPipelineState(*this);
						for (const auto& entry : drawables.transparents)
						{
							const auto& drawable = entry.second;
							if (!drawable.material || !drawable.mesh)
								continue;

							PreparedRecordedDraw preparedDraw;
							const bool captured = CaptureThreadedPreparedDraw(
								drawable,
								transparentOverrides,
								transparentPso.depthFunc,
								preparedDraw);
							bool queued = false;
							if (captured)
							{
								queued = QueueThreadedRecordedDraw(preparedDraw);
								if (queued)
									++queuedTransparentDrawCount;
							}
							LogPreparedDrawResult("Transparent", captured, queued, preparedDraw);
						}
						SetActivePreparedPassBindingSet(nullptr);
					}
				}
			}

			m_threadedQueuedGBufferDrawCount = queuedGBufferDrawCount;
			m_threadedQueuedDecalDrawCount = queuedDecalDrawCount;
			m_threadedQueuedLightingDrawCount = queuedLightingDrawCount;
			m_threadedQueuedTransparentDrawCount = queuedTransparentDrawCount;

		}

		auto pendingFrameSnapshot = [&]()
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::BuildFrameSnapshot");
			return BuildFrameSnapshot(p_frameDescriptor);
		}();
		if (pendingFrameSnapshot.has_value())
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::PublishFrameSnapshot");
			RefreshFrameSnapshotVisibility(pendingFrameSnapshot.value(), drawables);
			if (usesThreadedRendering &&
				ShouldSkipThreadedDeferredFramePublish(
					pendingFrameSnapshot.value(),
					queuedGBufferDrawCount,
					queuedDecalDrawCount,
					queuedLightingDrawCount,
					queuedTransparentDrawCount))
			{
				m_skipThreadedFramePublish = true;
				if (m_threadedHZBPostSubmitReadback.has_value())
					DiscardPendingHZBOcclusionObservationFrame();
				m_threadedHZBPostSubmitReadback.reset();
			}
			if (usesThreadedRendering)
				SynchronizeThreadedDeferredSnapshot(
					pendingFrameSnapshot.value(),
					queuedGBufferDrawCount,
					queuedDecalDrawCount,
					queuedLightingDrawCount,
					queuedTransparentDrawCount);
			if (usesThreadedRendering)
			{
				SetPendingPreparedRenderSceneBuilder(
					BuildDeferredPreparedRenderSceneBuilder(
						pendingFrameSnapshot.value(),
						hasSkyboxTexture,
						{},
						{},
						nullptr,
						0u,
						0u,
						GetThreadedHZBPostSubmitReadbackForPreparedBuilder()));
			}
			if (usesThreadedRendering &&
				NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
			{
				NLS_LOG_INFO(
					"[DeferredSceneRenderer] Snapshot recordedDraws=" +
					std::to_string(pendingFrameSnapshot->recordedDrawCommands.size()) +
					" queuedGBufferDraws=" + std::to_string(queuedGBufferDrawCount) +
					" queuedDecalDraws=" + std::to_string(queuedDecalDrawCount) +
					" queuedLightingDraws=" + std::to_string(queuedLightingDrawCount) +
					" queuedTransparentDraws=" + std::to_string(queuedTransparentDrawCount) +
					" visibleOpaqueDraws=" + std::to_string(pendingFrameSnapshot->visibleOpaqueDrawCount) +
					" visibleDecalDraws=" + std::to_string(pendingFrameSnapshot->visibleDecalDrawCount) +
					" visibleTransparentDraws=" + std::to_string(pendingFrameSnapshot->visibleTransparentDrawCount) +
					" sceneOpaqueDrawables=" + std::to_string(drawables.opaques.size()));
			}
			SetPendingFrameSnapshot(pendingFrameSnapshot.value());
		}

		NLS::Render::Context::RenderScenePackage scenePackage;
		if (!usesThreadedRendering && pendingFrameSnapshot.has_value())
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::BuildRenderScenePackage");
			scenePackage = BuildRenderScenePackage(pendingFrameSnapshot.value());
		}

		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::AddDeferredSceneDescriptor");
			AddDescriptor<DeferredSceneDescriptor>({
				std::move(drawables),
				std::move(scenePackage),
				hasSkyboxTexture });
		}

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
				if (PrepareHZBFrameResources(deferredResourceRequest))
					resourceRequest.hzbResources = BuildHZBFrameResourceRequest();
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
					lightGridContext,
					!scene.drawables.transparents.empty(),
					!scene.drawables.decals.empty());
			}
			{
				NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::ExecutePreparedDeferredSceneGraph");
				const auto transparentDepthView = !scene.drawables.transparents.empty()
					? NLS::Render::FrameGraph::CaptureDeferredPreparedSceneResources(
						deferredResourceRequest).gbufferDepthView
					: std::shared_ptr<NLS::Render::RHI::RHITextureView>{};
				SetActivePreparedPassBindingSet(lightGridContext.graphicsPassBindingSet);
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
						},
						[this, transparentDepthView](const auto& beginDesc) -> bool
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginTransparentPass");
							return BeginOutputRenderPass(
								beginDesc.renderWidth,
								beginDesc.renderHeight,
								beginDesc.clearColor,
								beginDesc.clearDepth,
								beginDesc.clearStencil,
								transparentDepthView,
								false,
								beginDesc.clearValue);
						},
						[this]()
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::DrawTransparentsCallback");
							DrawTransparents(CreateSceneDefaultPipelineState(*this));
						},
						[this](bool startedRenderPass, const auto& endDesc)
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::EndTransparentPass");
							(void)endDesc;
							EndOutputRenderPass(startedRenderPass);
						},
						[this](const auto& beginDesc) -> bool
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginDecalPass");
							return BeginRecordedRenderPass(
								&m_gBuffer,
								beginDesc.renderWidth,
								beginDesc.renderHeight,
								beginDesc.clearColor,
								beginDesc.clearDepth,
								beginDesc.clearStencil,
								false,
								beginDesc.clearValue);
						},
						[this]()
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::DrawDecalsCallback");
							DrawDecals(CreateSceneDefaultPipelineState(*this));
						},
						[this]()
						{
							NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::EndDecalPass");
							EndRecordedRenderPass();
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
			SetActivePreparedPassBindingSet(nullptr);
			if (auto hzbPostSubmitReadback = BuildHZBPostSubmitReadbackRequest(false); hzbPostSubmitReadback.has_value())
			{
				const auto queuedHZBReadbackRequest = hzbPostSubmitReadback.value();
				const bool queuedHZBReadback =
					NLS::Render::Context::DriverRendererAccess::QueueStandalonePostSubmitBufferReadback(
					m_driver,
					std::move(hzbPostSubmitReadback.value()));
				if (queuedHZBReadback)
					AdoptHZBPostSubmitReadbackRequest(queuedHZBReadbackRequest);
				if (!queuedHZBReadback)
				{
					DiscardPendingHZBOcclusionObservationFrame();
					ClearHZBPendingResultReadback();
				}
			}
		}

		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::DrawFrame::DrawRegisteredPasses");
			DrawRegisteredPasses();
		}

		if (usesThreadedRendering)
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::DrawFrame::ThreadedSnapshotRefresh");
			const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
			auto pendingFrameSnapshot = BuildFrameSnapshot(m_frameDescriptor);
			if (pendingFrameSnapshot.has_value())
			{
				RefreshFrameSnapshotVisibility(pendingFrameSnapshot.value(), scene.drawables);
				SynchronizeThreadedDeferredSnapshot(
					pendingFrameSnapshot.value(),
					m_threadedQueuedGBufferDrawCount,
					m_threadedQueuedDecalDrawCount,
					m_threadedQueuedLightingDrawCount,
					m_threadedQueuedTransparentDrawCount);
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
		m_hzbBuildShader = ShaderLoader::Create(ResolveEngineShaderPath("HZBBuild.hlsl"), projectAssetsRoot);
		m_hzbOcclusionShader = ShaderLoader::Create(ResolveEngineShaderPath("HZBOcclusion.hlsl"), projectAssetsRoot);

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

	bool DeferredSceneRenderer::HasDeferredThreadedPipelineResources() const
	{
		return m_gBufferShader != nullptr &&
			m_lightingMaterial != nullptr &&
			m_fullscreenQuad != nullptr &&
			m_gBufferAlbedoTexture != nullptr &&
			m_gBufferNormalTexture != nullptr &&
			m_gBufferMaterialTexture != nullptr &&
			m_gBufferDepthTexture != nullptr &&
			m_gBufferAlbedoTexture->GetExplicitRHITextureHandle() != nullptr &&
			m_gBufferNormalTexture->GetExplicitRHITextureHandle() != nullptr &&
			m_gBufferMaterialTexture->GetExplicitRHITextureHandle() != nullptr &&
			m_gBufferDepthTexture->GetExplicitRHITextureHandle() != nullptr;
	}

	bool DeferredSceneRenderer::EnsureHZBTargets(uint16_t width, uint16_t height)
	{
		NLS_PROFILE_SCOPE();
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
		if (device == nullptr || width == 0u || height == 0u)
			return false;

		const auto expectedHZBMipCount = CalculateHZBMipCount(width, height);
		const auto matchesSize = [width, height, expectedHZBMipCount](const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
		{
			return TextureResourceMatchesSize(texture, width, height) &&
				texture->GetDesc().mipLevels == expectedHZBMipCount;
		};

		if (!matchesSize(m_hzbTexture))
		{
			NLS::Render::RHI::RHITextureDesc desc;
			desc.extent = { width, height, 1u };
			desc.format = NLS::Render::RHI::TextureFormat::R32F;
			desc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled |
				NLS::Render::RHI::TextureUsageFlags::Storage;
			desc.mipLevels = expectedHZBMipCount;
			desc.arrayLayers = 1u;
			desc.debugName = "SceneHZB";
			m_hzbTexture = device->CreateTexture(desc);
			m_hzbReadView.reset();
			m_hzbMipReadViews.clear();
			m_hzbMipWriteViews.clear();
			m_hzbBuildBindingSets.clear();
			m_hzbOcclusionBindingSet.reset();
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedHZBTexture.reset();
		}

		if (m_hzbOcclusionPrimitiveInputBuffer == nullptr)
		{
			const SceneOcclusionPrimitivePacket emptyPrimitiveInput{};

			NLS::Render::RHI::RHIBufferDesc desc;
			desc.size = NLS::Render::Data::kSceneOcclusionPrimitivePacketStride;
			desc.usage = NLS::Render::RHI::BufferUsageFlags::ShaderRead;
			desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
			desc.debugName = "SceneHZBOcclusionPrimitiveInputs";

			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = &emptyPrimitiveInput;
			uploadDesc.dataSize = sizeof(emptyPrimitiveInput);
			uploadDesc.debugName = "SceneHZBOcclusionPrimitiveInputsInitialUpload";
			m_hzbOcclusionPrimitiveInputBuffer = device->CreateBuffer(desc, uploadDesc);
			m_hzbOcclusionBindingSet.reset();
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedOcclusionPrimitiveInputBuffer.reset();
		}

		if (m_hzbOcclusionPrimitiveResultBuffer == nullptr)
		{
			const uint32_t emptyPrimitiveResult = 0u;

			NLS::Render::RHI::RHIBufferDesc desc;
			desc.size = sizeof(uint32_t);
			desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage |
				NLS::Render::RHI::BufferUsageFlags::CopySrc;
			desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
			desc.debugName = "SceneHZBOcclusionPrimitiveResults";

			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = &emptyPrimitiveResult;
			uploadDesc.dataSize = sizeof(emptyPrimitiveResult);
			uploadDesc.debugName = "SceneHZBOcclusionPrimitiveResultsInitialUpload";
			m_hzbOcclusionPrimitiveResultBuffer = device->CreateBuffer(desc, uploadDesc);
			m_hzbOcclusionBindingSet.reset();
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedOcclusionPrimitiveResultBuffer.reset();
		}

		if (m_hzbOcclusionConstantsBuffer == nullptr)
		{
			const HZBOcclusionConstants constants{};

			NLS::Render::RHI::RHIBufferDesc desc;
			desc.size = sizeof(HZBOcclusionConstants);
			desc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
			desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
			desc.debugName = "SceneHZBOcclusionConstants";

			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = &constants;
			uploadDesc.dataSize = sizeof(constants);
			uploadDesc.debugName = "SceneHZBOcclusionConstantsInitialUpload";
			m_hzbOcclusionConstantsBuffer = device->CreateBuffer(desc, uploadDesc);
			m_hzbOcclusionBindingSet.reset();
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedOcclusionConstantsBuffer.reset();
		}

		if (m_hzbTexture == nullptr ||
			m_hzbOcclusionPrimitiveInputBuffer == nullptr ||
			m_hzbOcclusionPrimitiveResultBuffer == nullptr ||
			m_hzbOcclusionConstantsBuffer == nullptr)
		{
			return false;
		}

		auto makeViewDesc = [](std::string debugName, const NLS::Render::RHI::TextureFormat format)
		{
			NLS::Render::RHI::RHITextureViewDesc viewDesc;
			viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
			viewDesc.format = format;
			viewDesc.subresourceRange.baseMipLevel = 0u;
			viewDesc.subresourceRange.mipLevelCount = 1u;
			viewDesc.subresourceRange.baseArrayLayer = 0u;
			viewDesc.subresourceRange.arrayLayerCount = 1u;
			viewDesc.debugName = std::move(debugName);
			return viewDesc;
		};

		const auto hzbMipCount = m_hzbTexture->GetDesc().mipLevels;
		if (m_hzbMipWriteViews.size() != hzbMipCount || m_hzbMipReadViews.size() != hzbMipCount)
		{
			m_hzbMipWriteViews.clear();
			m_hzbMipReadViews.clear();
			m_hzbMipWriteViews.reserve(hzbMipCount);
			m_hzbMipReadViews.reserve(hzbMipCount);
			for (uint32_t mip = 0u; mip < hzbMipCount; ++mip)
			{
				auto writeDesc = makeViewDesc(
					"SceneHZBMip" + std::to_string(mip) + "UAV",
					NLS::Render::RHI::TextureFormat::R32F);
				writeDesc.subresourceRange.baseMipLevel = mip;
				auto readDesc = makeViewDesc(
					"SceneHZBMip" + std::to_string(mip) + "SRV",
					NLS::Render::RHI::TextureFormat::R32F);
				readDesc.subresourceRange.baseMipLevel = mip;
				m_hzbMipWriteViews.push_back(device->CreateTextureView(m_hzbTexture, writeDesc));
				m_hzbMipReadViews.push_back(device->CreateTextureView(m_hzbTexture, readDesc));
			}
			m_hzbBuildBindingSets.clear();
			m_hzbOcclusionBindingSet.reset();
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedHZBTexture.reset();
		}
		if (m_hzbReadView == nullptr)
		{
			auto readAllDesc = makeViewDesc(
				"SceneHZBAllMipsSRV",
				NLS::Render::RHI::TextureFormat::R32F);
			readAllDesc.subresourceRange.mipLevelCount = hzbMipCount;
			m_hzbReadView = device->CreateTextureView(
				m_hzbTexture,
				readAllDesc);
		}

		const auto allMipViewsValid =
			std::all_of(m_hzbMipWriteViews.begin(), m_hzbMipWriteViews.end(), [](const auto& view) { return view != nullptr; }) &&
			std::all_of(m_hzbMipReadViews.begin(), m_hzbMipReadViews.end(), [](const auto& view) { return view != nullptr; });
		return allMipViewsValid && m_hzbReadView != nullptr;
	}

	bool DeferredSceneRenderer::PrepareHZBOcclusionPrimitiveBuffers(
		std::span<const SceneOcclusionPrimitivePacket> packets)
	{
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
		if (device == nullptr)
			return false;

		static const SceneOcclusionPrimitivePacket emptyPacket{};
		const auto activePacketCount = static_cast<uint32_t>(packets.size());
		const auto storagePacketCount = static_cast<uint32_t>((std::max<size_t>)(packets.size(), 1u));
		const auto inputSize = static_cast<size_t>(storagePacketCount) * sizeof(SceneOcclusionPrimitivePacket);
		const auto activeInputSize = static_cast<size_t>((std::max<size_t>)(packets.size(), 1u)) * sizeof(SceneOcclusionPrimitivePacket);
		const auto resultSize = static_cast<size_t>(storagePacketCount) * sizeof(uint32_t);
		const void* inputData = packets.empty() ? static_cast<const void*>(&emptyPacket) : packets.data();
		std::vector<uint32_t> primitiveResultClear(storagePacketCount, 0u);
		const void* resultData = primitiveResultClear.data();

		const auto canReuseCapacityBuffer = [](const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& buffer, const size_t requiredSize)
		{
			return buffer != nullptr && buffer->GetDesc().size >= requiredSize;
		};
		const auto updateBuffer = [](const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& buffer,
			const void* data,
			const size_t dataSize,
			std::string debugName)
		{
			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = data;
			uploadDesc.dataSize = dataSize;
			uploadDesc.debugName = std::move(debugName);
			return buffer != nullptr && buffer->UpdateData(uploadDesc).Succeeded();
		};

		if (!canReuseCapacityBuffer(m_hzbOcclusionPrimitiveInputBuffer, inputSize))
		{
			NLS::Render::RHI::RHIBufferDesc desc;
			desc.size = inputSize;
			desc.usage = NLS::Render::RHI::BufferUsageFlags::ShaderRead;
			desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
			desc.debugName = "SceneHZBOcclusionPrimitiveInputs";

			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = inputData;
			uploadDesc.dataSize = inputSize;
			uploadDesc.debugName = packets.empty()
				? "SceneHZBOcclusionPrimitiveInputsInitialUpload"
				: "SceneHZBOcclusionPrimitiveInputsFrameUpload";
			m_hzbOcclusionPrimitiveInputBuffer = device->CreateBuffer(desc, uploadDesc);
			m_hzbOcclusionBindingSet.reset();
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedOcclusionPrimitiveInputBuffer.reset();
		}
		else if (!updateBuffer(
			m_hzbOcclusionPrimitiveInputBuffer,
			inputData,
			activeInputSize,
			packets.empty()
				? "SceneHZBOcclusionPrimitiveInputsInitialUpload"
				: "SceneHZBOcclusionPrimitiveInputsFrameUpload"))
		{
			return false;
		}

		if (!canReuseCapacityBuffer(m_hzbOcclusionPrimitiveResultBuffer, resultSize))
		{
			NLS::Render::RHI::RHIBufferDesc desc;
			desc.size = resultSize;
			desc.usage = NLS::Render::RHI::BufferUsageFlags::Storage |
				NLS::Render::RHI::BufferUsageFlags::CopySrc;
			desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
			desc.debugName = "SceneHZBOcclusionPrimitiveResults";

			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = resultData;
			uploadDesc.dataSize = resultSize;
			uploadDesc.debugName = packets.empty()
				? "SceneHZBOcclusionPrimitiveResultsInitialUpload"
				: "SceneHZBOcclusionPrimitiveResultsFrameClear";
			m_hzbOcclusionPrimitiveResultBuffer = device->CreateBuffer(desc, uploadDesc);
			m_hzbOcclusionBindingSet.reset();
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedOcclusionPrimitiveResultBuffer.reset();
		}

		const HZBOcclusionConstants constants{ activePacketCount, {} };
		if (m_hzbOcclusionConstantsBuffer == nullptr)
		{
			NLS::Render::RHI::RHIBufferDesc desc;
			desc.size = sizeof(HZBOcclusionConstants);
			desc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;
			desc.memoryUsage = NLS::Render::RHI::MemoryUsage::CPUToGPU;
			desc.debugName = "SceneHZBOcclusionConstants";

			NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
			uploadDesc.data = &constants;
			uploadDesc.dataSize = sizeof(constants);
			uploadDesc.debugName = "SceneHZBOcclusionConstantsInitialUpload";
			m_hzbOcclusionConstantsBuffer = device->CreateBuffer(desc, uploadDesc);
			m_hzbOcclusionBindingSet.reset();
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedOcclusionConstantsBuffer.reset();
		}
		else if (!updateBuffer(
			m_hzbOcclusionConstantsBuffer,
			&constants,
			sizeof(constants),
			"SceneHZBOcclusionConstantsFrameUpload"))
		{
			return false;
		}

		m_hzbOcclusionPrimitiveCount = activePacketCount;
		return m_hzbOcclusionPrimitiveInputBuffer != nullptr &&
			m_hzbOcclusionPrimitiveResultBuffer != nullptr &&
			m_hzbOcclusionConstantsBuffer != nullptr;
	}

	bool DeferredSceneRenderer::PollHZBOcclusionResultReadback()
	{
		const bool logRenderDrawPath =
			NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath;
		if (m_hzbOcclusionResultReadbackCompletion == nullptr &&
			m_hzbOcclusionResultReadbackState != nullptr)
		{
			bool discardReadback = false;
			bool retryReadback = false;
			const auto readbackState = m_hzbOcclusionResultReadbackState;
			{
				std::lock_guard lock(readbackState->mutex);
				if (!readbackState->beginAttempted)
					return false;
				if (readbackState->beginInProgress)
					return false;

				if (readbackState->beginSucceeded &&
					readbackState->completion != nullptr)
				{
					m_hzbOcclusionResultReadbackCompletion =
						readbackState->completion;
				}
				else
				{
					retryReadback =
						IsTransientHZBReadbackBusyFailure(readbackState->resultMessage);
					discardReadback = !retryReadback;
				}
			}
			if (retryReadback)
			{
				if (logRenderDrawPath)
				{
					NLS_LOG_INFO(
						"[DeferredSceneRenderer][HZBReadback] retrying after transient begin failure code=" +
						std::to_string(static_cast<int>(readbackState->resultCode)) +
						" message=\"" +
						readbackState->resultMessage +
						"\"");
				}
				ClearHZBPendingResultReadback(false);
				return false;
			}
			if (discardReadback)
			{
				if (logRenderDrawPath)
				{
					NLS_LOG_INFO(
						"[DeferredSceneRenderer][HZBReadback] discarded after begin failure code=" +
						std::to_string(static_cast<int>(readbackState->resultCode)) +
						" message=\"" +
						readbackState->resultMessage +
						"\"");
				}
				DiscardPendingHZBOcclusionObservationFrame();
				ClearHZBPendingResultReadback();
				return false;
			}
		}

		if (m_hzbOcclusionResultReadbackCompletion == nullptr)
			return false;

		const auto status = m_hzbOcclusionResultReadbackCompletion->Poll();
		if (!status.IsComplete())
			return false;

		if (status.Succeeded() &&
			m_hzbOcclusionResultReadbackFlags != nullptr &&
			!m_hzbOcclusionResultReadbackFlags->empty())
		{
			if (logRenderDrawPath)
				NLS_LOG_INFO(
					"[DeferredSceneRenderer][HZBReadback] completed flags=" +
					std::to_string(m_hzbOcclusionResultReadbackFlags->size()));
			CompleteHZBOcclusionObservationFrame(*m_hzbOcclusionResultReadbackFlags);
		}
		else
		{
			if (logRenderDrawPath)
				NLS_LOG_INFO(
					"[DeferredSceneRenderer][HZBReadback] discarded after completion success=" +
					std::to_string(status.Succeeded() ? 1 : 0));
			DiscardPendingHZBOcclusionObservationFrame();
		}

		m_hzbOcclusionResultReadbackCompletion.reset();
		ClearHZBPendingResultReadback();
		return status.Succeeded();
	}

	bool DeferredSceneRenderer::BeginHZBOcclusionResultReadback()
	{
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
		if (device == nullptr ||
			!IsHZBOcclusionSupported() ||
			m_hzbOcclusionPrimitiveResultBuffer == nullptr ||
			m_hzbOcclusionObservationPrimitiveCount == 0u ||
			m_hzbOcclusionResultReadbackCompletion != nullptr ||
			m_hzbOcclusionResultReadbackState != nullptr)
		{
			return false;
		}

		auto readbackRequest = BuildHZBPostSubmitReadbackRequest(false);
		if (!readbackRequest.has_value())
			return false;

		AdoptHZBPostSubmitReadbackRequest(*readbackRequest);
		const auto result = device->BeginReadBuffer(readbackRequest->desc);
		if (!result.Succeeded() || result.completion == nullptr)
		{
			if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
				NLS_LOG_INFO("[DeferredSceneRenderer][HZBReadback] BeginReadBuffer failed: " + result.message);
			if (IsDeviceLostReadbackFailure(result))
			{
				NLS::Render::Context::DriverUIAccess::MarkDeviceLost(
					m_driver,
					result.message.empty()
						? "DeferredSceneRenderer::BeginHZBOcclusionResultReadback failed because the RHI device is lost"
						: result.message);
			}
			if (IsTransientHZBReadbackBusyFailure(result.message))
			{
				ClearHZBPendingResultReadback(false);
				return false;
			}
			DiscardPendingHZBOcclusionObservationFrame();
			ClearHZBPendingResultReadback();
			return false;
		}

		m_hzbOcclusionResultReadbackCompletion = result.completion;
		if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
			NLS_LOG_INFO("[DeferredSceneRenderer][HZBReadback] BeginReadBuffer succeeded");
		if (m_hzbOcclusionResultReadbackState != nullptr)
		{
			std::lock_guard lock(m_hzbOcclusionResultReadbackState->mutex);
			m_hzbOcclusionResultReadbackState->beginAttempted = true;
			m_hzbOcclusionResultReadbackState->beginSucceeded = true;
			m_hzbOcclusionResultReadbackState->resultCode = result.code;
			m_hzbOcclusionResultReadbackState->resultMessage = result.message;
			m_hzbOcclusionResultReadbackState->completion = result.completion;
		}
		return true;
	}

	std::optional<NLS::Render::Context::PostSubmitBufferReadbackRequest>
	DeferredSceneRenderer::BuildHZBPostSubmitReadbackRequest(
		const bool waitForLastComputeQueueCompletion)
	{
		const bool supported = IsHZBOcclusionSupported();
		if (!supported ||
		m_hzbOcclusionPrimitiveResultBuffer == nullptr ||
		m_hzbOcclusionObservationPrimitiveCount == 0u ||
		m_hzbOcclusionResultReadbackCompletion != nullptr ||
		m_hzbOcclusionResultReadbackState != nullptr)
		{
			if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
			{
				NLS_LOG_INFO(
					"[DeferredSceneRenderer][HZBReadback] request skipped supported=" +
					std::to_string(supported ? 1 : 0) +
					" resultBuffer=" +
					std::to_string(m_hzbOcclusionPrimitiveResultBuffer != nullptr ? 1 : 0) +
					" observationCount=" +
					std::to_string(m_hzbOcclusionObservationPrimitiveCount) +
					" hasCompletion=" +
					std::to_string(m_hzbOcclusionResultReadbackCompletion != nullptr ? 1 : 0) +
					" hasState=" +
					std::to_string(m_hzbOcclusionResultReadbackState != nullptr ? 1 : 0));
			}
			return std::nullopt;
		}

		auto readbackFlags = std::make_shared<std::vector<uint32_t>>(
			m_hzbOcclusionObservationPrimitiveCount,
			0u);
		auto readbackState = std::make_shared<NLS::Render::Context::PostSubmitBufferReadbackState>();

		NLS::Render::Context::PostSubmitBufferReadbackRequest request;
		request.desc.source = m_hzbOcclusionPrimitiveResultBuffer;
		request.desc.sourceState = NLS::Render::RHI::ResourceState::ShaderWrite;
		request.desc.sourceOffset = 0u;
		request.desc.size =
			static_cast<uint64_t>(readbackFlags->size()) *
			sizeof(uint32_t);
		request.desc.data = readbackFlags->data();
		request.desc.debugName = "SceneHZBOcclusionPrimitiveResultsReadback";
		request.state = std::move(readbackState);
		request.destinationKeepAlive = std::move(readbackFlags);
		request.waitForLastComputeQueueCompletion = waitForLastComputeQueueCompletion;
		if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
		{
			NLS_LOG_INFO(
				"[DeferredSceneRenderer][HZBReadback] request built flags=" +
				std::to_string(m_hzbOcclusionObservationPrimitiveCount) +
				" waitForLastCompute=" +
				std::to_string(waitForLastComputeQueueCompletion ? 1 : 0));
		}
		return request;
	}

	void DeferredSceneRenderer::AdoptHZBPostSubmitReadbackRequest(
		const NLS::Render::Context::PostSubmitBufferReadbackRequest& request)
	{
		if (request.state == nullptr || request.destinationKeepAlive == nullptr)
			return;

		m_hzbOcclusionResultReadbackState = request.state;
		m_hzbOcclusionResultReadbackFlags =
			std::static_pointer_cast<std::vector<uint32_t>>(request.destinationKeepAlive);
		if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
		{
			NLS_LOG_INFO(
				"[DeferredSceneRenderer][HZBReadback] request adopted flags=" +
				std::to_string(m_hzbOcclusionResultReadbackFlags != nullptr ? m_hzbOcclusionResultReadbackFlags->size() : 0u));
		}
	}

	void DeferredSceneRenderer::ClearHZBPendingResultReadback(const bool clearObservationPrimitiveCount)
	{
		m_hzbOcclusionResultReadbackState.reset();
		m_hzbOcclusionResultReadbackFlags.reset();
		m_threadedHZBPostSubmitReadback.reset();
		if (clearObservationPrimitiveCount)
			m_hzbOcclusionObservationPrimitiveCount = 0u;
	}

	void DeferredSceneRenderer::DiscardHZBObservationIfNoReadbackWasPublished()
	{
		if (m_hzbOcclusionResultReadbackState != nullptr ||
			m_hzbOcclusionResultReadbackCompletion != nullptr ||
			m_threadedHZBPostSubmitReadback.has_value())
		{
			return;
		}

		if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath &&
			HasPendingHZBOcclusionObservationFrame())
		{
			NLS_LOG_INFO("[DeferredSceneRenderer][HZBReadback] discarded pending observation because no readback was published");
		}
		DiscardPendingHZBOcclusionObservationFrame();
		m_hzbOcclusionObservationPrimitiveCount = 0u;
	}

	bool DeferredSceneRenderer::IsHZBOcclusionSupported() const
	{
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
		if (device == nullptr)
			return false;

		SceneOcclusionCapabilityRequest request;
		request.opaqueDepthFormat = NLS::Render::FrameGraph::kDeferredGBufferDepthFormat;
		return SceneOcclusionSystem::ResolveCapabilities(*device, request).backendSupported;
	}

	void DeferredSceneRenderer::BeginHZBOcclusionObservationFrame(
		const SceneOcclusionFrameInput& frame,
		std::span<const SceneOcclusionPrimitiveInput> primitiveInputs)
	{
		m_hzbOcclusionObservationPrimitiveCount = static_cast<uint32_t>(primitiveInputs.size());
		BaseSceneRenderer::BeginHZBOcclusionObservationFrame(frame, primitiveInputs);
	}

	SceneOcclusionObservationStats DeferredSceneRenderer::CompleteHZBOcclusionObservationFrame(
		std::span<const uint32_t> primitiveResultFlags)
	{
		return BaseSceneRenderer::CompleteHZBOcclusionObservationFrame(primitiveResultFlags);
	}

	bool DeferredSceneRenderer::EnsureHZBPipelines()
	{
		NLS_PROFILE_SCOPE();
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
		auto pipelineCache = NLS::Render::Context::DriverRendererAccess::GetPipelineCache(m_driver);
		if (device == nullptr || pipelineCache == nullptr || m_hzbBuildShader == nullptr || m_hzbOcclusionShader == nullptr)
			return false;

		NLS::Render::Resources::GlobalShader hzbBuildGlobalShader{
			"HZBBuildCS",
			NLS::Render::ShaderCompiler::ShaderStage::Compute,
			m_hzbBuildShader,
			NLS::Render::Engine::Shaders::HZBBuildCS::GetStaticShaderType().GetRootParameterStructs().front()
		};
		NLS::Render::Resources::GlobalShader hzbOcclusionGlobalShader{
			"HZBOcclusionCS",
			NLS::Render::ShaderCompiler::ShaderStage::Compute,
			m_hzbOcclusionShader,
			NLS::Render::Engine::Shaders::HZBOcclusionCS::GetStaticShaderType().GetRootParameterStructs().front()
		};

		if (m_hzbBuildBindingLayout == nullptr)
			m_hzbBuildBindingLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePassBindingLayout(
				device,
				hzbBuildGlobalShader.parameters);
		if (m_hzbOcclusionBindingLayout == nullptr)
			m_hzbOcclusionBindingLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePassBindingLayout(
				device,
				hzbOcclusionGlobalShader.parameters);
		if (m_hzbBuildBindingLayout == nullptr || m_hzbOcclusionBindingLayout == nullptr)
			return false;

		if (m_hzbBuildPipelineLayout == nullptr)
			m_hzbBuildPipelineLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePipelineLayout(
				device,
				m_hzbBuildBindingLayout,
				"HZBBuildPipelineLayout");
		if (m_hzbOcclusionPipelineLayout == nullptr)
			m_hzbOcclusionPipelineLayout = NLS::Render::Resources::ComputeShaderUtils::CreatePipelineLayout(
				device,
				m_hzbOcclusionBindingLayout,
				"HZBOcclusionPipelineLayout");
		if (m_hzbBuildPipelineLayout == nullptr || m_hzbOcclusionPipelineLayout == nullptr)
			return false;

		m_hzbBuildPipeline = NLS::Render::Resources::ComputeShaderUtils::CreateComputePipeline(
			device,
			pipelineCache,
			hzbBuildGlobalShader,
			m_hzbBuildPipelineLayout,
			m_hzbBuildPipeline,
			m_hzbBuildPipelineKey,
			"HZBBuildPipeline");
		m_hzbOcclusionPipeline = NLS::Render::Resources::ComputeShaderUtils::CreateComputePipeline(
			device,
			pipelineCache,
			hzbOcclusionGlobalShader,
			m_hzbOcclusionPipelineLayout,
			m_hzbOcclusionPipeline,
			m_hzbOcclusionPipelineKey,
			"HZBOcclusionPipeline");

		return m_hzbBuildPipeline != nullptr && m_hzbOcclusionPipeline != nullptr;
	}

	bool DeferredSceneRenderer::PrepareHZBFrameResources(
		const NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest& deferredResources)
	{
		NLS_PROFILE_SCOPE();
		const auto logSkip = [this](
			const char* reason,
			SceneOcclusionFallbackReason fallbackReason = SceneOcclusionFallbackReason::None,
			const std::string& diagnosticReason = {})
		{
			if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
			{
				std::string message = std::string("[DeferredSceneRenderer][HZB] Prepare skipped: ") + reason;
				if (fallbackReason != SceneOcclusionFallbackReason::None)
				{
					message += " fallback=";
					message += ToHZBFallbackReasonName(fallbackReason);
				}
				if (!diagnosticReason.empty())
				{
					message += " diagnostic=\"";
					message += diagnosticReason;
					message += "\"";
				}
				NLS_LOG_INFO(message);
			}
		};
		auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
		if (device == nullptr)
		{
			logSkip("explicit RHI device missing");
			return false;
		}
		if (NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).
			editorValidationDisableHZBOcclusion)
		{
			logSkip("disabled by editor validation override");
			return false;
		}
		if (deferredResources.gBuffer == nullptr || deferredResources.gbufferDepthTexture == nullptr)
		{
			logSkip("deferred prepared resources missing GBuffer depth");
			return false;
		}

		const auto depthTexture = deferredResources.gbufferDepthTexture->GetExplicitRHITextureHandle();
		if (depthTexture == nullptr)
		{
			logSkip("GBuffer depth has no explicit RHI texture");
			return false;
		}

		const auto& depthDesc = depthTexture->GetDesc();
		SceneOcclusionCapabilityRequest capabilityRequest;
		capabilityRequest.opaqueDepthFormat = depthDesc.format;
		const auto support = SceneOcclusionSystem::ResolveCapabilities(*device, capabilityRequest);
		if (!support.backendSupported)
		{
			logSkip(
				"capability gate rejected HZB occlusion",
				support.fallbackReason,
				support.diagnosticReason);
			return false;
		}
		if (!NLS::Render::RHI::HasTextureUsage(depthDesc.usage, NLS::Render::RHI::TextureUsageFlags::Sampled))
		{
			logSkip("GBuffer depth texture is not sampleable");
			return false;
		}

		if (!EnsureHZBTargets(
				static_cast<uint16_t>(depthDesc.extent.width),
				static_cast<uint16_t>(depthDesc.extent.height)) ||
			!EnsureHZBPipelines())
		{
			logSkip("target, pipeline, or shader setup failed");
			return false;
		}

		const auto hzbMipCount = m_hzbTexture != nullptr ? m_hzbTexture->GetDesc().mipLevels : 0u;
		m_hzbBuildDispatchGroupsByMip.clear();
		m_hzbBuildDispatchGroupsByMip.reserve(hzbMipCount);
		for (uint32_t mip = 0u; mip < hzbMipCount; ++mip)
			m_hzbBuildDispatchGroupsByMip.push_back(CalculateHZBBuildDispatchGroupsForMip(
				depthDesc.extent.width,
				depthDesc.extent.height,
				mip));
		m_hzbOcclusionDispatchGroups = {
			(std::max<uint32_t>(m_hzbOcclusionPrimitiveCount, 1u) + 7u) / 8u,
			1u,
			1u
		};

		const bool hzbBindingCacheValid =
			m_hzbPreparedDepthTexture == depthTexture &&
			m_hzbPreparedHZBTexture == m_hzbTexture &&
			m_hzbPreparedOcclusionPrimitiveInputBuffer == m_hzbOcclusionPrimitiveInputBuffer &&
			m_hzbPreparedOcclusionPrimitiveResultBuffer == m_hzbOcclusionPrimitiveResultBuffer &&
			m_hzbPreparedOcclusionConstantsBuffer == m_hzbOcclusionConstantsBuffer &&
			m_hzbDepthReadView != nullptr &&
			m_hzbBuildBindingSets.size() == hzbMipCount &&
			std::all_of(m_hzbBuildBindingSets.begin(), m_hzbBuildBindingSets.end(), [](const auto& bindingSet) { return bindingSet != nullptr; }) &&
			m_hzbOcclusionBindingSet != nullptr;
		if (hzbBindingCacheValid)
			return true;

		if (device == nullptr)
			return false;

		NLS::Render::RHI::RHITextureViewDesc depthViewDesc;
		depthViewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
		depthViewDesc.format = depthDesc.format;
		depthViewDesc.subresourceRange.baseMipLevel = 0u;
		depthViewDesc.subresourceRange.mipLevelCount = 1u;
		depthViewDesc.subresourceRange.baseArrayLayer = 0u;
		depthViewDesc.subresourceRange.arrayLayerCount = 1u;
		depthViewDesc.debugName = "HZBOpaqueDepthSRV";
		m_hzbDepthReadView = device->CreateTextureView(depthTexture, depthViewDesc);
		if (m_hzbDepthReadView == nullptr)
		{
			logSkip("depth SRV creation failed");
			return false;
		}

		const auto descriptorLifetime = NLS::Render::RHI::DescriptorAllocationLifetime::Persistent;
		const auto hzbBuildParameters =
			NLS::Render::Engine::Shaders::HZBBuildCS::GetStaticShaderType().GetRootParameterStructs().front();
		m_hzbBuildBindingSets.clear();
		m_hzbBuildBindingSets.reserve(hzbMipCount);
		for (uint32_t mip = 0u; mip < hzbMipCount; ++mip)
		{
			const auto& previousMipView = mip == 0u ? m_hzbDepthReadView : m_hzbMipReadViews[mip - 1u];
			auto hzbBuildSetDesc = NLS::Render::Resources::BuildBindingSetDescFromShaderParameters(
				hzbBuildParameters,
				m_hzbBuildBindingLayout,
				{
					NLS::Render::Resources::ShaderParameterBindingValue::Texture("u_HZBPreviousMip", previousMipView),
					NLS::Render::Resources::ShaderParameterBindingValue::RWTexture("u_HZBOutputMip", m_hzbMipWriteViews[mip])
				},
				"HZBBuildMip" + std::to_string(mip) + "BindingSet");
			m_hzbBuildBindingSets.push_back(NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
				m_driver,
				hzbBuildSetDesc,
				descriptorLifetime));
		}

		const auto hzbOcclusionParameters =
			NLS::Render::Engine::Shaders::HZBOcclusionCS::GetStaticShaderType().GetRootParameterStructs().front();
		const auto primitiveInputBufferRange = static_cast<uint64_t>(
			m_hzbOcclusionPrimitiveInputBuffer->GetDesc().size);
		const auto primitiveResultBufferRange = static_cast<uint64_t>(
			m_hzbOcclusionPrimitiveResultBuffer->GetDesc().size);
		auto hzbOcclusionSetDesc = NLS::Render::Resources::BuildBindingSetDescFromShaderParameters(
			hzbOcclusionParameters,
			m_hzbOcclusionBindingLayout,
			{
				NLS::Render::Resources::ShaderParameterBindingValue::Texture("u_HZB", m_hzbReadView),
				NLS::Render::Resources::ShaderParameterBindingValue::UniformBuffer(
					"HZBOcclusionConstants",
					m_hzbOcclusionConstantsBuffer,
					sizeof(HZBOcclusionConstants)),
				NLS::Render::Resources::ShaderParameterBindingValue::StructuredBuffer(
					"u_OcclusionPrimitiveInputs",
					m_hzbOcclusionPrimitiveInputBuffer,
					primitiveInputBufferRange,
					0u,
					static_cast<uint32_t>(NLS::Render::Data::kSceneOcclusionPrimitivePacketStride)),
				NLS::Render::Resources::ShaderParameterBindingValue::StorageBuffer(
					"u_OcclusionPrimitiveResults",
					m_hzbOcclusionPrimitiveResultBuffer,
					primitiveResultBufferRange,
					0u,
					sizeof(uint32_t))
			},
			"HZBOcclusionBindingSet");
		m_hzbOcclusionBindingSet = NLS::Render::Context::DriverRendererAccess::CreateExplicitBindingSet(
			m_driver,
			hzbOcclusionSetDesc,
			descriptorLifetime);

		const auto hzbBuildBindingSetsValid = m_hzbBuildBindingSets.size() == hzbMipCount &&
			std::all_of(m_hzbBuildBindingSets.begin(), m_hzbBuildBindingSets.end(), [](const auto& bindingSet) { return bindingSet != nullptr; });
		if (!hzbBuildBindingSetsValid || m_hzbOcclusionBindingSet == nullptr)
		{
			logSkip("binding set creation failed");
			m_hzbPreparedDepthTexture.reset();
			m_hzbPreparedHZBTexture.reset();
			m_hzbPreparedOcclusionPrimitiveInputBuffer.reset();
			m_hzbPreparedOcclusionPrimitiveResultBuffer.reset();
			m_hzbPreparedOcclusionConstantsBuffer.reset();
			return false;
		}

		m_hzbPreparedDepthTexture = depthTexture;
		m_hzbPreparedHZBTexture = m_hzbTexture;
		m_hzbPreparedOcclusionPrimitiveInputBuffer = m_hzbOcclusionPrimitiveInputBuffer;
		m_hzbPreparedOcclusionPrimitiveResultBuffer = m_hzbOcclusionPrimitiveResultBuffer;
		m_hzbPreparedOcclusionConstantsBuffer = m_hzbOcclusionConstantsBuffer;
		return true;
	}

	NLS::Render::FrameGraph::HZBFrameResourceRequest DeferredSceneRenderer::BuildHZBFrameResourceRequest() const
	{
		NLS::Render::FrameGraph::HZBFrameResourceRequest request;
		if (!IsHZBOcclusionSupported())
			return request;
		request.opaqueDepthTexture = m_gBuffer.GetExplicitDepthTextureHandle();
		request.hzbTexture = m_hzbTexture;
		request.occlusionPrimitiveInputBuffer = m_hzbOcclusionPrimitiveInputBuffer;
		request.occlusionPrimitiveResultBuffer = m_hzbOcclusionPrimitiveResultBuffer;
		request.occlusionConstantsBuffer = m_hzbOcclusionConstantsBuffer;
		request.hzbBuildPipeline = m_hzbBuildPipeline;
		request.hzbBuildBindingSets = m_hzbBuildBindingSets;
		request.hzbBuildGroupCountsByMip = m_hzbBuildDispatchGroupsByMip;
		request.occlusionPipeline = m_hzbOcclusionPipeline;
		request.occlusionBindingSet = m_hzbOcclusionBindingSet;
		request.occlusionGroupCounts = m_hzbOcclusionDispatchGroups;
		request.opaqueDepthEligible = request.opaqueDepthTexture != nullptr &&
			m_hzbBuildPipeline != nullptr &&
			!m_hzbBuildBindingSets.empty() &&
			std::all_of(m_hzbBuildBindingSets.begin(), m_hzbBuildBindingSets.end(), [](const auto& bindingSet) { return bindingSet != nullptr; }) &&
			m_hzbOcclusionPipeline != nullptr &&
			m_hzbOcclusionBindingSet != nullptr &&
			m_hzbOcclusionPrimitiveInputBuffer != nullptr &&
			m_hzbOcclusionPrimitiveResultBuffer != nullptr &&
			m_hzbOcclusionConstantsBuffer != nullptr;
		request.hzbMipCount = m_hzbTexture != nullptr ? m_hzbTexture->GetDesc().mipLevels : 0u;
		return request;
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

	NLS::Render::Resources::Material& DeferredSceneRenderer::ResolveFrameGBufferMaterial(
		NLS::Render::Resources::Material& sourceMaterial)
	{
		NLS_PROFILE_SCOPE();
		const auto sourceStamp = BuildGBufferMaterialSyncStamp(sourceMaterial);
		auto found = m_frameGBufferMaterialResolveCache.find(sourceStamp.sourceMaterialInstanceId);
		if (found != m_frameGBufferMaterialResolveCache.end() &&
			found->second.material != nullptr &&
			found->second.sourceStamp == sourceStamp)
		{
			++m_frameGBufferMaterialResolveHitCount;
			m_rendererStats.RecordGBufferMaterialResolve(true);
			return *found->second.material;
		}

		++m_frameGBufferMaterialResolveMissCount;
		m_rendererStats.RecordGBufferMaterialResolve(false);
		auto& material = GetOrCreateGBufferMaterial(sourceMaterial);
		m_frameGBufferMaterialResolveCache[sourceStamp.sourceMaterialInstanceId] = {
			sourceStamp,
			&material
		};
		return material;
	}

	void DeferredSceneRenderer::ClearFrameGBufferMaterialResolveCache()
	{
		m_frameGBufferMaterialResolveCache.clear();
		m_frameGBufferMaterialResolveHitCount = 0u;
		m_frameGBufferMaterialResolveMissCount = 0u;
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
			gbufferDrawable.material = &ResolveFrameGBufferMaterial(*drawable.material);

			const auto gBufferOverrides = BuildGBufferMaterialOverrides(*drawable.material);

			DrawEntity(gbufferDrawable, gBufferOverrides, pso.depthFunc);
		}
	}

	void DeferredSceneRenderer::DrawDecals(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		if (!m_gBufferShader)
			return;

		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();

		for (const auto& entry : scene.drawables.decals)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;

			auto gbufferDrawable = drawable;
			gbufferDrawable.material = &ResolveFrameGBufferMaterial(*drawable.material);

			const auto decalOverrides = BuildDeferredDecalMaterialOverrides(*drawable.material);

			DrawEntity(gbufferDrawable, decalOverrides, GetDeferredDecalDepthCompare());
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

	void DeferredSceneRenderer::DrawTransparents(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();

		NLS::Render::Resources::MaterialPipelineStateOverrides transparentOverrides;
		transparentOverrides.depthWrite = false;

		for (const auto& entry : scene.drawables.transparents)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;
			DrawEntity(drawable, transparentOverrides, pso.depthFunc);
		}
	}

	NLS::Render::Resources::Material& DeferredSceneRendererTestAccess::GetOrCreateGBufferMaterial(
		DeferredSceneRenderer& renderer,
		NLS::Render::Resources::Material& sourceMaterial)
	{
		return renderer.GetOrCreateGBufferMaterial(sourceMaterial);
	}

	NLS::Render::Resources::Material& DeferredSceneRendererTestAccess::ResolveFrameGBufferMaterial(
		DeferredSceneRenderer& renderer,
		NLS::Render::Resources::Material& sourceMaterial)
	{
		return renderer.ResolveFrameGBufferMaterial(sourceMaterial);
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

	void DeferredSceneRendererTestAccess::ClearFrameGBufferMaterialResolveCache(
		DeferredSceneRenderer& renderer)
	{
		renderer.ClearFrameGBufferMaterialResolveCache();
	}

	uint64_t DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveCacheSize(
		const DeferredSceneRenderer& renderer)
	{
		return static_cast<uint64_t>(renderer.m_frameGBufferMaterialResolveCache.size());
	}

	uint64_t DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveHitCount(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_frameGBufferMaterialResolveHitCount;
	}

	uint64_t DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveMissCount(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_frameGBufferMaterialResolveMissCount;
	}

	NLS::Render::Resources::MaterialPipelineStateOverrides DeferredSceneRendererTestAccess::BuildDeferredDecalMaterialOverridesForTesting(
		const NLS::Render::Resources::Material& sourceMaterial)
	{
		return BuildDeferredDecalMaterialOverrides(sourceMaterial);
	}

	NLS::Render::Settings::EComparaisonAlgorithm DeferredSceneRendererTestAccess::GetDeferredDecalDepthCompareForTesting()
	{
		return GetDeferredDecalDepthCompare();
	}

	bool DeferredSceneRendererTestAccess::ShouldSkipThreadedDeferredFramePublishForTesting(
		const NLS::Render::Context::FrameSnapshot& snapshot,
		const uint64_t queuedGBufferDrawCount,
		const uint64_t queuedDecalDrawCount,
		const uint64_t queuedLightingDrawCount,
		const uint64_t queuedTransparentDrawCount)
	{
		return ShouldSkipThreadedDeferredFramePublish(
			snapshot,
			queuedGBufferDrawCount,
			queuedDecalDrawCount,
			queuedLightingDrawCount,
			queuedTransparentDrawCount);
	}

	void DeferredSceneRendererTestAccess::SynchronizeThreadedDeferredSnapshotForTesting(
		NLS::Render::Context::FrameSnapshot& snapshot,
		const uint64_t queuedGBufferDrawCount,
		const uint64_t queuedDecalDrawCount,
		const uint64_t queuedLightingDrawCount,
		const uint64_t queuedTransparentDrawCount)
	{
		DeferredSceneRenderer::SynchronizeThreadedDeferredSnapshot(
			snapshot,
			queuedGBufferDrawCount,
			queuedDecalDrawCount,
			queuedLightingDrawCount,
			queuedTransparentDrawCount);
	}

	void DeferredSceneRendererTestAccess::EnsureGBufferTargets(
		DeferredSceneRenderer& renderer,
		const uint16_t width,
		const uint16_t height)
	{
		renderer.EnsureGBufferTargets(width, height);
	}

	bool DeferredSceneRendererTestAccess::EnsureHZBTargets(
		DeferredSceneRenderer& renderer,
		const uint16_t width,
		const uint16_t height)
	{
		return renderer.EnsureHZBTargets(width, height);
	}

	bool DeferredSceneRendererTestAccess::PrepareHZBOcclusionPrimitiveBuffers(
		DeferredSceneRenderer& renderer,
		std::span<const SceneOcclusionPrimitivePacket> packets)
	{
		return renderer.PrepareHZBOcclusionPrimitiveBuffers(packets);
	}

	bool DeferredSceneRendererTestAccess::PollHZBOcclusionResultReadback(DeferredSceneRenderer& renderer)
	{
		return renderer.PollHZBOcclusionResultReadback();
	}

	bool DeferredSceneRendererTestAccess::BeginHZBOcclusionResultReadback(DeferredSceneRenderer& renderer)
	{
		return renderer.BeginHZBOcclusionResultReadback();
	}

	void DeferredSceneRendererTestAccess::BeginHZBOcclusionObservationFrame(
		DeferredSceneRenderer& renderer,
		const SceneOcclusionFrameInput& frame,
		std::span<const SceneOcclusionPrimitiveInput> primitiveInputs)
	{
		renderer.BeginHZBOcclusionObservationFrame(frame, primitiveInputs);
	}

	bool DeferredSceneRendererTestAccess::HasPendingHZBOcclusionObservationFrame(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.HasPendingHZBOcclusionObservationFrame();
	}

	void DeferredSceneRendererTestAccess::DiscardPendingHZBOcclusionObservationFrame(
		DeferredSceneRenderer& renderer)
	{
		renderer.DiscardPendingHZBOcclusionObservationFrame();
	}

	SceneOcclusionObservationStats DeferredSceneRendererTestAccess::CompleteHZBOcclusionObservationFrame(
		DeferredSceneRenderer& renderer,
		std::span<const uint32_t> primitiveResultFlags)
	{
		return renderer.CompleteHZBOcclusionObservationFrame(primitiveResultFlags);
	}

	const SceneOcclusionHistory& DeferredSceneRendererTestAccess::GetHZBOcclusionHistory(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.GetHZBOcclusionHistoryForTesting();
	}

	NLS::Render::FrameGraph::HZBFrameResourceRequest DeferredSceneRendererTestAccess::BuildHZBFrameResourceRequest(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.BuildHZBFrameResourceRequest();
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
