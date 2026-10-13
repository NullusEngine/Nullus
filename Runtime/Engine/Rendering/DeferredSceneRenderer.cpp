#include "Rendering/DeferredSceneRenderer.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"
#include "Rendering/SceneLightingProvider.h"

#include <fg/Blackboard.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>

#include <Debug/Logger.h>
#include <Math/Matrix4.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/Context/RenderingThreadSystem.h>
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
	constexpr size_t kMaterialResolveCacheMaxEntries = 1024u;

	template <typename Cache>
	void TrimMaterialResolveCacheForInsert(Cache& cache, const bool insertsNewSource)
	{
		if (insertsNewSource && cache.size() >= kMaterialResolveCacheMaxEntries)
			cache.clear();
	}

		struct CaptureFrameSlotScope
		{
			NLS::Render::Core::FrameObjectBindingProvider* provider = nullptr;

			CaptureFrameSlotScope(
				NLS::Render::Core::FrameObjectBindingProvider* p_provider,
					const size_t slotIndex, const std::function<void(size_t)>& bind)
				: provider(p_provider)
			{
				if (provider != nullptr)
						bind(slotIndex);
			}

			~CaptureFrameSlotScope()
			{
				if (provider != nullptr)
					provider->UnbindCaptureFrameSlot();
			}

			CaptureFrameSlotScope(const CaptureFrameSlotScope&) = delete;
			CaptureFrameSlotScope& operator=(const CaptureFrameSlotScope&) = delete;
		};

	std::span<const NLS::Render::RHI::TextureFormat> GetDeferredGBufferColorFormats()
	{
		return NLS::Render::FrameGraph::kDeferredGBufferColorFormats;
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
		uint32_t largestDimension = (std::max)({ width, height, 1u });
		while (largestDimension > 1u)
		{
			largestDimension = (largestDimension + 1u) / 2u;
			++mipCount;
		}
		return mipCount;
	}

	std::array<uint32_t, 3u> CalculateHZBBuildDispatchGroupsForMip(
		const uint32_t width,
		const uint32_t height,
		const uint32_t mip)
	{
		uint32_t mipWidth = (std::max)(1u, static_cast<uint32_t>(width));
		uint32_t mipHeight = (std::max)(1u, static_cast<uint32_t>(height));
		for (uint32_t level = 0u; level < mip; ++level)
		{
			mipWidth = (mipWidth + 1u) / 2u;
			mipHeight = (mipHeight + 1u) / 2u;
		}
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

	std::string ResolveEngineShaderPath(const std::string& fileName)
	{
		const auto trustedPath =
			NLS::Render::Resources::Loaders::ShaderLoader::ResolveTrustedBuiltInEngineShaderPath(fileName);
		if (!trustedPath.empty())
			return trustedPath;

		return (std::filesystem::path("App") / "Assets" / "Engine" / "Shaders" / fileName).string();
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

	void CopyMaterialParameterAndTexturePathIfPresent(
		NLS::Render::Resources::Material& target,
		const char* targetName,
		const NLS::Render::Resources::Material& source,
		const char* sourceName)
	{
		const auto* value = FindMaterialParameter(source, sourceName);
		const auto path = source.GetTextureResourcePath(sourceName);
		if (value == nullptr && path.empty())
			return;

		if (value != nullptr)
			SetMaterialParameter(target, targetName, *value);
		if (path.empty())
			target.ClearTextureResourcePath(targetName);
		else
			target.SetTextureResourcePath(targetName, path);
	}

	void EnsureDeferredLightingSkyParameters(
		NLS::Render::Resources::Material& lightingMaterial,
		const NLS::Render::Resources::Material* skyboxMaterial)
	{
		if (skyboxMaterial != nullptr)
		{
			// A procedural SkyBoxComponent may publish its material before any
			// lighting draw executes. Set the semantic switch explicitly instead
			// of relying on the optional parameter-copy path.
			lightingMaterial.SetRawParameter("u_UseProceduralSky", true);
			for (const char* parameterName : {
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

		for (const char* channelName : {"u_MetallicMapChannel", "u_RoughnessMapChannel"})
		{
			const auto* channel = parameters.TryGet(channelName);
			if (channel == nullptr ||
				(channel->type() == typeid(NLS::Maths::Vector4) &&
					IsZeroColor(std::any_cast<const NLS::Maths::Vector4&>(*channel))))
			{
				target.SetRawParameter(channelName, NLS::Maths::Vector4(1.0f, 0.0f, 0.0f, 0.0f));
			}
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

	void EnsureDeferredDecalFallbackParameters(NLS::Render::Resources::Material& target)
	{
		const auto& parameters = target.GetParameterBlock();
		const auto* albedo = parameters.TryGet("u_Albedo");
		if (albedo == nullptr || albedo->type() != typeid(NLS::Maths::Vector4))
		{
			target.SetRawParameter("u_Albedo", NLS::Maths::Vector4(1.0f, 1.0f, 1.0f, 1.0f));
		}

		for (const char* textureName : {"u_AlbedoMap", "u_OpacityMap"})
		{
			if (!parameters.Contains(textureName))
				target.SetRawParameter(textureName, static_cast<NLS::Render::Resources::Texture2D*>(nullptr));
		}
	}

	bool MaterialHasTextureParameter(
		const NLS::Render::Resources::Material& material,
		const char* textureName)
	{
		const auto* value = material.GetParameterBlock().TryGet(textureName);
		return value != nullptr &&
			value->type() == typeid(NLS::Render::Resources::Texture2D*) &&
			std::any_cast<NLS::Render::Resources::Texture2D*>(*value) != nullptr;
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
		overrides.hasDepthAttachment = true;
		overrides.depthFormat = NLS::Render::FrameGraph::kDeferredGBufferDepthFormat;
		overrides.sampleCount = 1u;
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
		constexpr auto kDeferredDecalColorWriteMask = NLS::Render::RHI::RHIColorWriteMask::Red |
			NLS::Render::RHI::RHIColorWriteMask::Green |
			NLS::Render::RHI::RHIColorWriteMask::Blue;
		NLS::Render::RHI::RHIRenderTargetBlendStateDesc blendedTarget;
		blendedTarget.blendEnable = true;
		blendedTarget.colorWriteMask = kDeferredDecalColorWriteMask;
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

	std::string BuildDeferredDecalMaterialCacheKey(
		const NLS::Render::Resources::Material& sourceMaterial,
		const NLS::Render::Data::PipelineState& pipelineState)
	{
		auto key = NLS::Render::Resources::BuildMaterialPassVariantKey(
			sourceMaterial,
			"DeferredDecal",
			pipelineState,
			BuildDeferredDecalMaterialOverrides(sourceMaterial)).stableKey;
		key += "|source:";
		key += std::to_string(sourceMaterial.GetInstanceId());
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
			m_deferPipelineResourceAssetsUntilFirstFrame =
				options.loadPipelineResources && options.deferPipelineResourcesUntilFirstFrame;
			if (options.loadPipelineResources && !m_deferPipelineResourceAssetsUntilFirstFrame)
			{
				const auto pipelineAssetsBegin = std::chrono::steady_clock::now();
				EnsureDeferredPipelineResourceAssets();
				NLS_LOG_INFO(
					"[Startup] DeferredSceneRenderer pipeline assets elapsedMs=" +
					std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - pipelineAssetsBegin).count()));
			}
		}

	void DeferredSceneRenderer::SynchronizeThreadedDeferredSnapshot(
		NLS::Render::Context::RenderSceneBuildData& snapshot,
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
		const NLS::Render::Context::RenderSceneBuildData& snapshot,
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
		// Runs before the resource releases below. Deferred has its own destructor
		// body, so it is the first one to run for this class and for
		// DebugSceneRenderer; releasing GPU resources first would race the render
		// thread, which may still be inside this frame's prepared builder.
		DrainPublishedFramesBeforeTeardown();
		ReleaseDeferredDecalPipelineResources();
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

DeferredSceneRenderer::DeferredThreadedDrawCounts
DeferredSceneRenderer::EstimateThreadedDeferredDrawCounts(const AllDrawables& drawables)
{
	DeferredThreadedDrawCounts counts;
		for (const auto& entry : drawables.opaques)
		{
			if (entry.second.material != nullptr && entry.second.mesh != nullptr)
				++counts.gBufferDrawCount;
		}
		for (const auto& entry : drawables.decals)
		{
			if (entry.second.material == nullptr || entry.second.mesh == nullptr)
				continue;
			// This check must not create the fallback decal material or touch the
			// resolve cache: the estimate runs before capture and must not perturb the
			// cache state capture then observes.
			const bool hasOwnDecalShader =
				entry.second.material->ResolveShaderForLightMode("DeferredDecal") != nullptr;
			if (!hasOwnDecalShader && m_deferredDecalShader == nullptr)
			{
				++counts.unresolvedDecalDrawCount;
				continue;
			}
			++counts.decalDrawCount;
		}
	for (const auto& entry : drawables.transparents)
	{
		if (entry.second.material != nullptr && entry.second.mesh != nullptr)
			++counts.transparentDrawCount;
	}

	// The deferred lighting composite is captured once whenever the deferred
	// scene itself is captured.
	if (!drawables.opaques.empty() ||
		!drawables.decals.empty() ||
		!drawables.transparents.empty() ||
		!drawables.skyboxes.empty())
	{
		counts.lightingDrawCount = 1u;
	}
	return counts;
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

	DeferredSceneRenderer::DeferredThreadedCaptureResult
	DeferredSceneRenderer::CaptureThreadedDeferredDraws(
		const AllDrawables& drawables,
		NLS::Render::Resources::TextureCube* skyboxTexture,
		NLS::Render::Resources::Material* skyboxMaterial,
		const bool logPreparedDrawResults,
		std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outCommands)
	{
		NLS_PROFILE_SCOPE();
		DeferredThreadedCaptureResult result;
		// Diagnostics: accumulated across every captured drawable, reported once per
		// frame. Per-drawable profiler zones would cost more than the work they
		// measure, so the phase split is kept as plain counters behind the existing
		// draw-path logging switch.
		uint64_t prepareDrawNs = 0u;
		uint64_t prepareRecordedDrawNs = 0u;
		uint64_t captureBindingSetsNs = 0u;
		uint64_t queueDrawNs = 0u;
		uint64_t capturedDrawableCount = 0u;
		if (logPreparedDrawResults)
		NLS_LOG_INFO(
			std::string("[DeferredSceneRenderer] CaptureThreadedDeferredDraws begin opaques=") +
			std::to_string(drawables.opaques.size()) +
			" decals=" + std::to_string(drawables.decals.size()) +
			" transparents=" + std::to_string(drawables.transparents.size()));
		SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder());

		const auto queueCapturedDraw =
			[this, &outCommands, logPreparedDrawResults](
				const char* stage,
				PreparedRecordedDraw& preparedDraw)
			{
				const bool queued = logPreparedDrawResults
					? QueueThreadedRecordedDraw(preparedDraw, outCommands)
					: QueueThreadedRecordedDraw(std::move(preparedDraw), outCommands);
				if (logPreparedDrawResults)
					LogPreparedDrawResult(stage, true, queued, preparedDraw);
				return queued;
			};

		const auto gbufferPso = CreateSceneDefaultPipelineState(*this);
		for (const auto& entry : drawables.opaques)
		{
			const auto& drawable = entry.second;
			if (!drawable.material)
				continue;

			CaptureDrawPhaseTimings phaseTimings;
			CaptureDrawPhaseTimings* phaseTimingsOut = logPreparedDrawResults ? &phaseTimings : nullptr;
			auto& gbufferMaterial = ResolveGBufferDrawableMaterial(*drawable.material);
			const auto gBufferOverrides = BuildGBufferMaterialOverrides(*drawable.material);
			PreparedRecordedDraw preparedDraw;
			const auto captureStart = logPreparedDrawResults
				? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
			const bool captured = CaptureThreadedPreparedDraw(
				drawable,
				gbufferMaterial,
				gBufferOverrides,
				gbufferPso.depthFunc,
				"GBuffer",
				preparedDraw,
				phaseTimingsOut);
			if (logPreparedDrawResults)
			{
				const auto captureTotalNs = static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - captureStart).count());
				capturedDrawableCount++;
				prepareDrawNs += phaseTimings.prepareDrawNs;
				prepareRecordedDrawNs += phaseTimings.prepareRecordedDrawNs;
				captureBindingSetsNs += phaseTimings.captureBindingSetsNs;
				queueDrawNs += captureTotalNs >
					(phaseTimings.prepareDrawNs + phaseTimings.prepareRecordedDrawNs +
						phaseTimings.captureBindingSetsNs)
					? captureTotalNs - (phaseTimings.prepareDrawNs +
						phaseTimings.prepareRecordedDrawNs + phaseTimings.captureBindingSetsNs)
					: 0u;
			}
			bool queued = false;
			if (captured)
				queued = queueCapturedDraw("GBuffer", preparedDraw);
			if (!captured && logPreparedDrawResults)
				LogPreparedDrawResult("GBuffer", false, false, preparedDraw);
			if (queued)
				++result.drawCounts.gBufferDrawCount;
			if (logPreparedDrawResults && (result.drawCounts.gBufferDrawCount % 32u) == 0u && result.drawCounts.gBufferDrawCount != 0u)
				NLS_LOG_INFO("[DeferredSceneRenderer] CaptureThreadedDeferredDraws gbuffer queued=" + std::to_string(result.drawCounts.gBufferDrawCount));
		}
		if (logPreparedDrawResults)
			NLS_LOG_INFO("[DeferredSceneRenderer] CaptureThreadedDeferredDraws gbuffer complete queued=" + std::to_string(result.drawCounts.gBufferDrawCount));
		if (logPreparedDrawResults)
		{
			// One line per frame: the per-drawable split of the capture cost, in
			// microseconds, so the draw preparation can be attributed without
			// paying for a profiler zone per drawable.
			NLS_LOG_INFO(
				"[DeferredSceneRenderer] CapturePhases drawables=" + std::to_string(capturedDrawableCount) +
				" prepareDrawUs=" + std::to_string(prepareDrawNs / 1000u) +
				" prepareRecordedDrawUs=" + std::to_string(prepareRecordedDrawNs / 1000u) +
				" captureBindingSetsUs=" + std::to_string(captureBindingSetsNs / 1000u) +
				" otherUs=" + std::to_string(queueDrawNs / 1000u) +
				" ringHits=" + std::to_string(GetFrameObjectBindingProvider()->GetLegacyObjectConstantsRingHitCount()) +
				" ringFallbacks=" + std::to_string(GetFrameObjectBindingProvider()->GetLegacyObjectConstantsRingFallbackCount()));
		}

		for (const auto& entry : drawables.decals)
		{
			const auto& drawable = entry.second;
			if (!drawable.material)
				continue;

			auto* decalMaterial = ResolveDeferredDecalDrawableMaterial(*drawable.material);
			if (decalMaterial == nullptr)
			{
				++result.unresolvedDecalDrawCount;
				continue;
			}

			const auto materialOverrides = BuildDeferredDecalMaterialOverrides(*drawable.material);
			PreparedRecordedDraw preparedDraw;
			const bool captured = CaptureThreadedPreparedDraw(
				drawable,
				*decalMaterial,
				materialOverrides,
				GetDeferredDecalDepthCompare(),
				"DeferredDecal",
				preparedDraw);
			bool queued = false;
			if (captured)
				queued = queueCapturedDraw("Decal", preparedDraw);
			if (!captured && logPreparedDrawResults)
				LogPreparedDrawResult("Decal", false, false, preparedDraw);
			if (queued)
				++result.drawCounts.decalDrawCount;
		}
		if (logPreparedDrawResults)
			NLS_LOG_INFO("[DeferredSceneRenderer] CaptureThreadedDeferredDraws decal complete queued=" + std::to_string(result.drawCounts.decalDrawCount));

		if (!SelectDeferredLightingVariant() || m_lightingMaterial == nullptr || m_fullscreenQuad == nullptr)
		{
			result.lightingCaptureSucceeded = false;
			SetActivePreparedPassBindingSet(nullptr);
				return result;
			}
		if (logPreparedDrawResults)
			NLS_LOG_INFO("[DeferredSceneRenderer] CaptureThreadedDeferredDraws lighting resources ready");

		m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>(
			"u_GBufferAlbedo",
			m_gBufferAlbedoTexture.get());
		m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>(
			"u_GBufferNormal",
			m_gBufferNormalTexture.get());
		m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>(
			"u_GBufferMaterial",
			m_gBufferMaterialTexture.get());
		if (m_gBufferDepthTexture)
			m_lightingMaterial->Set<NLS::Render::Resources::Texture2D*>(
				"u_GBufferDepth",
				m_gBufferDepthTexture.get());
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
		compositeOverrides.hasDepthAttachment = false;
		compositeOverrides.culling = false;
		compositeOverrides.colorWrite = true;

		PreparedRecordedDraw lightingDraw;
		const bool lightingCaptured = CaptureThreadedPreparedDraw(
			lightingDrawable,
			compositeOverrides,
			gbufferPso.depthFunc,
			"Forward",
			lightingDraw);
		bool lightingQueued = false;
		if (lightingCaptured)
			lightingQueued = queueCapturedDraw("Lighting", lightingDraw);
		if (!lightingCaptured && logPreparedDrawResults)
			LogPreparedDrawResult("Lighting", false, false, lightingDraw);
		if (lightingQueued)
			++result.drawCounts.lightingDrawCount;
		if (logPreparedDrawResults)
			NLS_LOG_INFO("[DeferredSceneRenderer] CaptureThreadedDeferredDraws lighting complete queued=" + std::to_string(result.drawCounts.lightingDrawCount));

		NLS::Render::Resources::MaterialPipelineStateOverrides transparentOverrides;
		transparentOverrides.depthWrite = false;
		const auto transparentPso = CreateSceneDefaultPipelineState(*this);
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
				"Forward",
				preparedDraw);
			bool queued = false;
			if (captured)
				queued = queueCapturedDraw("Transparent", preparedDraw);
			if (!captured && logPreparedDrawResults)
				LogPreparedDrawResult("Transparent", false, false, preparedDraw);
			if (queued)
				++result.drawCounts.transparentDrawCount;
		}
		if (logPreparedDrawResults)
			NLS_LOG_INFO("[DeferredSceneRenderer] CaptureThreadedDeferredDraws transparent complete queued=" + std::to_string(result.drawCounts.transparentDrawCount));

		SetActivePreparedPassBindingSet(nullptr);
		return result;
	}

	void DeferredSceneRenderer::LogPreparedDrawResult(
		const char* stage,
		const bool captured,
		const bool queued,
		const PreparedRecordedDraw& preparedDraw) const
	{
		if (!NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath)
			return;
		if (m_framePreparedDrawDiagnosticLogCount >= 8u)
			return;
		++m_framePreparedDrawDiagnosticLogCount;

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

NLS::Render::Context::RenderSceneBuildCommand DeferredSceneRenderer::BuildDeferredRenderSceneCommand(
    NLS::Render::Context::RenderFramePacket packet,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput> helperDrawCommands,
    const uint64_t helperPassCount,
    const bool hasSkyboxTexture,
    DeferredThreadedCaptureInput captureInput,
    std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs,
    std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedPassMetadata,
    std::shared_ptr<NLS::Render::RHI::RHITexture> preferredReadbackTexture,
    const uint64_t preferredReadbackTextureGeneration,
    const uint64_t additionalRenderTargetUseCount,
    const bool includeDeferredSceneExecution)
{
    auto frozenFrame = packet.view != nullptr
        ? packet.view
        : std::make_shared<const NLS::Render::FrameGraph::FrozenFrameDescriptor>(
            NLS::Render::FrameGraph::FreezeFrameDescriptor(GetFrameDescriptor()));
    return [this, packet = std::move(packet), helperDrawCommands = std::move(helperDrawCommands),
        helperPassCount, frozenFrame = std::move(frozenFrame),
        basePipelineState = CreatePipelineState(),
        bindCaptureFrame = GetFrameObjectBindingProvider()->FreezeCaptureFrame(),
        captureInput = std::move(captureInput), hasSkyboxTexture, includeDeferredSceneExecution,
        appendedPassInputs = std::move(appendedPassInputs),
        appendedPassMetadata = std::move(appendedPassMetadata),
        preferredReadbackTexture = std::move(preferredReadbackTexture),
        preferredReadbackTextureGeneration, additionalRenderTargetUseCount](const size_t lifecycleSlotIndex) mutable
    {
        NLS::Render::Context::RenderSceneBuildData snapshot;
        snapshot.visibleHelperDrawCount = helperPassCount;
        snapshot.recordedDrawCommands = std::move(helperDrawCommands);
        const auto& frameDescriptorForBuilder = frozenFrame->descriptor;
        PreparedFrameScope preparedFrameScope(*this, frameDescriptorForBuilder, basePipelineState);
        std::optional<NLS::Render::Context::ScopedFrameResourceContext> frameResources;
        if (NLS::Render::Context::DriverRendererAccess::GetFrameContextSlotCount(m_driver) != 0u)
            frameResources.emplace(m_driver, lifecycleSlotIndex);
        CaptureFrameSlotScope captureFrameSlotScope(
            GetFrameObjectBindingProvider(), lifecycleSlotIndex, bindCaptureFrame);
        const auto externalSceneOutputAttachments =
            NLS::Render::FrameGraph::ResolveExternalSceneOutputAttachments(
                frameDescriptorForBuilder, "DeferredEditorOverlayColorView", "DeferredEditorOverlayDepthView");
        auto finishPackage = [&](NLS::Render::Context::RenderScenePackage package)
        {
            if (preferredReadbackTexture != nullptr)
                NLS::Render::FrameGraph::RegisterPreferredReadbackTexture(
                    package, preferredReadbackTexture, preferredReadbackTextureGeneration);
            package.renderTargetUseCount += additionalRenderTargetUseCount;
            NLS::Render::FrameGraph::FinalizePreparedDeferredScenePackage(package, frameDescriptorForBuilder);
            return package;
        };
        if (!snapshot.recordedDrawCommands.empty() && std::none_of(
            appendedPassMetadata.begin(), appendedPassMetadata.end(), [](const auto& metadata)
            {
                return metadata.graphPassName != nullptr &&
                    std::string_view(metadata.graphPassName) == "EditorHelperPass";
            }))
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
            helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
            helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            helperMetadata.visibleDrawCountContribution = static_cast<uint64_t>(snapshot.recordedDrawCommands.size());
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(helperMetadata, "EditorHelperPass");
            appendedPassMetadata.push_back(std::move(helperMetadata));
            snapshot.visibleHelperDrawCount = (std::max)(snapshot.visibleHelperDrawCount,
                static_cast<uint64_t>(snapshot.recordedDrawCommands.size()));
        }

        try
        {
            ConsumeThreadedHZBReadbackHandoffs();
            PollHZBOcclusionResultReadback();
            if (captureInput.hzbReadbackHandoff != nullptr)
                m_threadedHZBReadbackHandoffs.push_back(captureInput.hzbReadbackHandoff);
            const bool resolvedScene = ResolveThreadedSceneCapture(snapshot, captureInput, *frozenFrame);
            const bool buildDeferredScene = includeDeferredSceneExecution && resolvedScene;
            if (!buildDeferredScene)
            {
                if (captureInput.hzbReadbackHandoff != nullptr)
                {
                    std::lock_guard lock(captureInput.hzbReadbackHandoff->mutex);
                    captureInput.hzbReadbackHandoff->request.reset();
                    captureInput.hzbReadbackHandoff->ready = true;
                }
                auto package = NLS::Render::Context::BuildRenderScenePackage(
                    packet, std::move(snapshot), NLS::Render::Context::RenderScenePackageBuildMode::SkipDefaultPassInputs);
                if (auto helperInput = NLS::Render::FrameGraph::BuildDeferredAggregateHelperPassInput(package);
                    helperInput.has_value())
                    appendedPassInputs.push_back(std::move(*helperInput));
                if (!package.targetsSwapchain)
                    NLS::Render::FrameGraph::ApplyExternalSceneOutputAttachments(
                        appendedPassInputs, externalSceneOutputAttachments,
                        {NLS::Render::Context::RenderPassCommandKind::Helper});
                NLS::Render::FrameGraph::CompileAndApplyThreadedRenderSceneExecution(
                    package, frameDescriptorForBuilder, -1, -1, appendedPassMetadata,
                    [appendedPassInputs = std::move(appendedPassInputs)](const auto&) mutable
                    {
                        return std::move(appendedPassInputs);
                    });
                return finishPackage(std::move(package));
            }

            if (m_deferPipelineResourceAssetsUntilFirstFrame)
                EnsureDeferredPipelineResourceAssets();
            EnsureGBufferTargets(
                static_cast<uint16_t>(frameDescriptorForBuilder.renderWidth),
                static_cast<uint16_t>(frameDescriptorForBuilder.renderHeight));
            const auto deferredResourceRequest = BuildDeferredPreparedSceneResourceRequest();
            const bool hzbResourcesPrepared =
                captureInput.hzbFrameInputEnabled && PrepareHZBFrameResources(deferredResourceRequest);
            const auto deferredResources = NLS::Render::FrameGraph::CaptureDeferredPreparedSceneResources(
                deferredResourceRequest);
            const auto hzbSource = hzbResourcesPrepared
                ? NLS::Render::FrameGraph::BuildHZBPreparedComputeDispatchSource(BuildHZBFrameResourceRequest())
                : NLS::Render::FrameGraph::PreparedComputeDispatchSource{};
            const bool frameHasSkyboxTexture = captureInput.sceneParseInput != nullptr
                ? captureInput.skyboxTexture != nullptr
                : hasSkyboxTexture;
            auto lightGridContext = BuildLightGridCompileContext(
                *frozenFrame, CaptureLightGridFrameInputs(frameHasSkyboxTexture),
                captureInput.suppressLightGridCompute);

            NLS::Render::FrameGraph::DeferredPreparedQueuedDrawCounts queuedDrawCounts;
            if (captureInput.drawables != nullptr && captureInput.recordedDrawCommands != nullptr)
            {
                auto helperDrawCommands = std::move(snapshot.recordedDrawCommands);
                snapshot.recordedDrawCommands = std::move(*captureInput.recordedDrawCommands);
                captureInput.recordedDrawCommands.reset();
                snapshot.recordedDrawCommands.clear();
                const auto captureResult = CaptureThreadedDeferredDraws(
                    *captureInput.drawables, captureInput.skyboxTexture, captureInput.skyboxMaterial,
                    captureInput.logPreparedDrawResults, snapshot.recordedDrawCommands);
                snapshot.visibleDecalDrawCount -= (std::min)(
                    snapshot.visibleDecalDrawCount, captureResult.unresolvedDecalDrawCount);
                if (ShouldSkipThreadedDeferredFramePublish(snapshot,
                    captureResult.drawCounts.gBufferDrawCount, captureResult.drawCounts.decalDrawCount,
                    captureResult.drawCounts.lightingDrawCount, captureResult.drawCounts.transparentDrawCount))
                    throw std::runtime_error("Deferred Render Command captured an incomplete scene draw queue");
                queuedDrawCounts.lightingDrawCount = captureResult.drawCounts.lightingDrawCount;
                queuedDrawCounts.decalDrawCount = captureResult.drawCounts.decalDrawCount;
                queuedDrawCounts.transparentDrawCount = captureResult.drawCounts.transparentDrawCount;
                SynchronizeThreadedDeferredSnapshot(snapshot, captureResult.drawCounts.gBufferDrawCount,
                    captureResult.drawCounts.decalDrawCount, captureResult.drawCounts.lightingDrawCount,
                    captureResult.drawCounts.transparentDrawCount);
                for (auto& command : helperDrawCommands)
                    snapshot.recordedDrawCommands.push_back(std::move(command));
            }

            std::optional<NLS::Render::Context::PostSubmitBufferReadbackRequest> hzbPostSubmitReadback;
            if (hzbResourcesPrepared)
                hzbPostSubmitReadback = BuildHZBPostSubmitReadbackRequest();
            if (captureInput.hzbReadbackHandoff != nullptr)
            {
                std::lock_guard lock(captureInput.hzbReadbackHandoff->mutex);
                if (!captureInput.hzbReadbackHandoff->cancelled)
                    captureInput.hzbReadbackHandoff->request = hzbPostSubmitReadback;
                else
                    captureInput.hzbReadbackHandoff->request.reset();
                captureInput.hzbReadbackHandoff->ready = true;
            }

            RecordDrawCommandMove(static_cast<uint64_t>(snapshot.recordedDrawCommands.size()));
            auto package = NLS::Render::Context::BuildRenderScenePackage(
                packet, std::move(snapshot), NLS::Render::Context::RenderScenePackageBuildMode::SkipDefaultPassInputs);
            if (!package.targetsSwapchain)
                NLS::Render::FrameGraph::ApplyExternalSceneOutputAttachments(
                    appendedPassInputs, externalSceneOutputAttachments,
                    {NLS::Render::Context::RenderPassCommandKind::Helper});
            NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
                package, lightGridContext, deferredResources, std::move(appendedPassInputs),
                appendedPassMetadata, queuedDrawCounts, hzbSource, &externalSceneOutputAttachments);
            if (hzbPostSubmitReadback.has_value())
                package.postSubmitBufferReadbacks.push_back(hzbPostSubmitReadback.value());
            return finishPackage(std::move(package));
        }
        catch (...)
        {
            if (captureInput.hzbReadbackHandoff != nullptr)
            {
                std::lock_guard lock(captureInput.hzbReadbackHandoff->mutex);
                captureInput.hzbReadbackHandoff->cancelled = true;
                captureInput.hzbReadbackHandoff->ready = true;
                captureInput.hzbReadbackHandoff->request.reset();
            }
            DiscardHZBObservationIfNoReadbackWasPublished();
            throw;
        }
    };
}

	NLS::Render::Context::RenderSceneBuildCommand DeferredSceneRenderer::BuildRenderSceneCommand(
		const NLS::Render::Context::RenderFramePacket& packet)
	{
		if (m_pendingRenderSceneCommand)
			return m_pendingRenderSceneCommand;

		const bool hasSkyboxTexture =
			HasDescriptor<DeferredSceneDescriptor>() &&
			GetDescriptor<DeferredSceneDescriptor>().hasSkyboxTexture;

		return BuildDeferredRenderSceneCommand(
			packet,
			std::move(m_threadedRecordedDrawCommands),
			0u,
			hasSkyboxTexture,
			BuildThreadedCaptureInputForPreparedBuilder());
	}

	DeferredSceneRenderer::DeferredThreadedCaptureInput
	DeferredSceneRenderer::BuildThreadedCaptureInputForPreparedBuilder()
	{
		DeferredThreadedCaptureInput captureInput;
		captureInput.sceneParseInput = m_pendingThreadedSceneParse;
		captureInput.sceneParseDropper = m_pendingThreadedSceneParseDropper;
		captureInput.logPreparedDrawResults =
			NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(m_driver).logRenderDrawPath;
		captureInput.suppressLightGridCompute = HasDescriptor<SceneDescriptor>() &&
			GetDescriptor<SceneDescriptor>().suppressLightGridCompute;
		if (!HasDescriptor<DeferredSceneDescriptor>())
			return captureInput;

		const auto& sceneDescriptor = GetDescriptor<DeferredSceneDescriptor>();
		captureInput.drawables = sceneDescriptor.drawables;
		if (sceneDescriptor.drawables != nullptr)
		{
			for (const auto& entry : sceneDescriptor.drawables->skyboxes)
			{
				const auto& drawable = entry.second;
				if (drawable.material == nullptr)
					continue;

				captureInput.skyboxMaterial = drawable.material;
				const auto* skyboxParameter =
					drawable.material->GetParameterBlock().TryGet("cubeTex");
				if (skyboxParameter != nullptr &&
					skyboxParameter->type() == typeid(NLS::Render::Resources::TextureCube*))
				{
					captureInput.skyboxTexture =
						std::any_cast<NLS::Render::Resources::TextureCube*>(*skyboxParameter);
				}
				break;
			}
		}
		captureInput.recordedDrawCommands =
			std::make_shared<std::vector<NLS::Render::Context::RecordedDrawCommandInput>>();
		captureInput.hzbReadbackHandoff = m_currentThreadedHZBReadbackHandoff;
		return captureInput;
	}

bool DeferredSceneRenderer::ResolveThreadedSceneCapture(
    NLS::Render::Context::RenderSceneBuildData& snapshot,
    DeferredThreadedCaptureInput& captureInput,
    const NLS::Render::FrameGraph::FrozenFrameDescriptor& frame)
{
    NLS_PROFILE_SCOPE();
    m_frameGBufferMaterialSyncCount = 0u;
    m_framePreparedDrawDiagnosticLogCount = 0u;
    ResetFrameGBufferMaterialResolveStats();
    ResetFrameDeferredDecalMaterialResolveStats();
    if (captureInput.sceneParseInput != nullptr)
    {
        captureInput.drawables = std::make_shared<AllDrawables>(
            ResolveSceneParse(*captureInput.sceneParseInput, frame));
        captureInput.sceneParseDropper.MarkResolved();
        RefreshFrameBuildMetadata(snapshot);
        RefreshSceneBuildVisibility(snapshot, *captureInput.drawables);
        captureInput.skyboxMaterial = nullptr;
        captureInput.skyboxTexture = nullptr;
        for (const auto& entry : captureInput.drawables->skyboxes)
        {
            const auto* material = entry.second.material;
            if (material == nullptr)
                continue;
            if (captureInput.skyboxMaterial == nullptr)
                captureInput.skyboxMaterial = entry.second.material;
            const auto* parameter = material->GetParameterBlock().TryGet("cubeTex");
            if (parameter != nullptr && parameter->type() == typeid(NLS::Render::Resources::TextureCube*))
            {
                captureInput.skyboxTexture = std::any_cast<NLS::Render::Resources::TextureCube*>(*parameter);
                break;
            }
        }
        const auto hzbFrameInput = GetLastHZBOcclusionFrameInput();
        captureInput.hzbFrameInputEnabled = hzbFrameInput.enabled;
        const auto packets = GetLastHZBOcclusionPrimitivePacketBuildResult();
        if (hzbFrameInput.enabled && hzbFrameInput.backendSupported && hzbFrameInput.historyTextureValid &&
            !HasPendingHZBOcclusionObservationFrame() && m_hzbOcclusionResultReadbackCompletion == nullptr &&
            m_hzbOcclusionResultReadbackState == nullptr && packets != nullptr &&
            !packets->primitiveInputs.empty() && PrepareHZBOcclusionPrimitiveBuffers(packets->primitivePackets))
        {
            BeginHZBOcclusionObservationFrame(hzbFrameInput, packets->primitiveInputs);
            if (captureInput.hzbReadbackHandoff != nullptr)
            {
                std::lock_guard lock(captureInput.hzbReadbackHandoff->mutex);
                captureInput.hzbReadbackHandoff->observationStarted = true;
            }
        }
    }
    if (captureInput.drawables == nullptr)
        return snapshot.visibleOpaqueDrawCount != 0u || snapshot.visibleDecalDrawCount != 0u ||
            snapshot.visibleTransparentDrawCount != 0u || snapshot.visibleSkyboxDrawCount != 0u;
    return !captureInput.drawables->opaques.empty() || !captureInput.drawables->decals.empty() ||
        !captureInput.drawables->transparents.empty() || !captureInput.drawables->skyboxes.empty();
}

	void DeferredSceneRenderer::ConsumeThreadedHZBReadbackHandoffs()
	{
		for (auto handoffIt = m_threadedHZBReadbackHandoffs.begin();
			handoffIt != m_threadedHZBReadbackHandoffs.end();)
		{
			const auto& handoff = *handoffIt;
			if (handoff == nullptr)
			{
				handoffIt = m_threadedHZBReadbackHandoffs.erase(handoffIt);
				continue;
			}

			std::optional<NLS::Render::Context::PostSubmitBufferReadbackRequest> request;
			bool discardObservation = false;
			bool removeHandoff = false;
			{
				std::lock_guard lock(handoff->mutex);
				if (!handoff->ready)
				{
					++handoffIt;
					continue;
				}

				if (handoff->cancelled || !handoff->request.has_value())
				{
					discardObservation = handoff->observationStarted;
					removeHandoff = true;
				}
				else if (m_hzbOcclusionResultReadbackState == nullptr &&
					m_hzbOcclusionResultReadbackCompletion == nullptr)
				{
					request = std::move(handoff->request);
					removeHandoff = true;
				}
			}

			if (request.has_value())
				AdoptHZBPostSubmitReadbackRequest(*request);
			if (discardObservation)
				DiscardPendingHZBOcclusionObservationFrame();
			if (removeHandoff)
				handoffIt = m_threadedHZBReadbackHandoffs.erase(handoffIt);
			else
				++handoffIt;
		}
	}

	void DeferredSceneRenderer::CancelCurrentThreadedHZBReadbackHandoff()
	{
		if (m_currentThreadedHZBReadbackHandoff == nullptr)
			return;

		std::lock_guard lock(m_currentThreadedHZBReadbackHandoff->mutex);
		m_currentThreadedHZBReadbackHandoff->cancelled = true;
		m_currentThreadedHZBReadbackHandoff->ready = true;
		m_currentThreadedHZBReadbackHandoff->request.reset();
	}

	bool DeferredSceneRenderer::TryPublishThreadedFrame()
	{
		if (m_skipThreadedFramePublish)
			return false;

		const bool published = BaseSceneRenderer::TryPublishThreadedFrame();
		if (!published)
		{
			CancelCurrentThreadedHZBReadbackHandoff();
		}
		return published;
	}

	void DeferredSceneRenderer::OnThreadedFramePublishFailed()
	{
		BaseSceneRenderer::OnThreadedFramePublishFailed();
		CancelCurrentThreadedHZBReadbackHandoff();
	}

	bool DeferredSceneRenderer::IsThreadedFramePublishSkippedForCurrentFrame() const
	{
		return m_skipThreadedFramePublish;
	}

	std::shared_ptr<NLS::Render::RHI::RHITextureView>
	DeferredSceneRenderer::GetDeferredPreparedSceneDepthViewForEditorHelpers() const
	{
			const auto& frame = GetFrameDescriptor();
			const auto width = static_cast<uint16_t>(frame.renderWidth);
			const auto height = static_cast<uint16_t>(frame.renderHeight);
			auto view = m_publishedGBufferDepthView.load(std::memory_order_acquire);
			if (view != nullptr && TextureResourceMatchesSize(view->GetTexture(), width, height))
				return view;

			// First use and resize need a current depth target before helper bindings
			// are captured. Steady frames use the published immutable resource handle.
			auto* renderer = const_cast<DeferredSceneRenderer*>(this);
			if (NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread())
				renderer->EnsureGBufferTargets(width, height);
			else
			{
				const auto completion = NLS::Render::Context::EnqueueRenderCommand<struct PrepareDeferredHelperDepthCommand>(
					[renderer, width, height] { renderer->EnsureGBufferTargets(width, height); });
#if defined(NLS_ENABLE_TEST_HOOKS)
				if (!NLS::Render::Context::RenderingThreadSystem::HasActiveRenderThread())
					(void)NLS::Render::Context::RenderCommandDispatcher::Dispatch();
#endif
				completion.Wait();
				if (!completion.Succeeded())
					throw std::runtime_error(completion.GetDiagnostic());
			}
			return m_publishedGBufferDepthView.load(std::memory_order_acquire);
	}

void DeferredSceneRenderer::BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
{
    NLS_PROFILE_SCOPE();
    NLS_ASSERT(HasFrameObjectBindingProvider(),
        "DeferredSceneRenderer requires a renderer-owned frame/object binding provider.");
    {
        NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::BaseBeginFrame");
        BaseSceneRenderer::BeginFrame(p_frameDescriptor);
    }
    m_skipThreadedFramePublish = false;
    m_pendingThreadedSceneParse.reset();
    m_pendingThreadedSceneParseDropper = {};
    m_currentThreadedHZBReadbackHandoff.reset();
    if (!IsFrameActive())
        return;

    if (!m_deferPipelineResourceAssetsUntilFirstFrame &&
        (m_gBufferShader == nullptr || m_lightingMaterial == nullptr || m_fullscreenQuad == nullptr))
    {
        m_skipThreadedFramePublish = true;
        return;
    }

    std::optional<NLS::Render::Context::RenderFramePacket> snapshot;
    {
        NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::BuildFramePacket");
        snapshot = BuildRenderFramePacket(p_frameDescriptor);
    }
    if (!snapshot.has_value() || snapshot->view == nullptr)
    {
        m_skipThreadedFramePublish = true;
        return;
    }
    std::shared_ptr<SceneParseInput> parseInput;
    {
        // Game-thread scene capture: reads the scene to snapshot what the render
        // thread will build from. Measured because it is the whole cost of the
        // game thread's part of BeginFrame.
        NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::BeginFrame::BeginSceneParse");
        parseInput = std::make_shared<SceneParseInput>(BeginSceneParse(*snapshot->view));
    }
    auto parseDropper = MakeSceneParseCaptureDropper(parseInput);
    if (parseInput->captureRefused)
    {
        m_skipThreadedFramePublish = true;
        return;
    }
    m_pendingThreadedSceneParse = std::move(parseInput);
    m_pendingThreadedSceneParseDropper = std::move(parseDropper);
    m_currentThreadedHZBReadbackHandoff = std::make_shared<DeferredThreadedCaptureInput::HZBReadbackHandoff>();
    AddDescriptor<DeferredSceneDescriptor>({nullptr, {}, false});
    SetPendingRenderFramePacket(snapshot.value());
}

void DeferredSceneRenderer::DrawFrame()
{
    NLS_PROFILE_SCOPE();
    DrawRegisteredPasses();
}

		void DeferredSceneRenderer::LoadPipelineResources()
		{
			NLS_PROFILE_SCOPE();
		using ShaderLoader = NLS::Render::Resources::Loaders::ShaderLoader;
	const auto& projectAssetsRoot = NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();

		const auto loadShader = [&projectAssetsRoot](const char* shaderName)
		{
			const auto shaderBegin = std::chrono::steady_clock::now();
			auto* shader = ShaderLoader::CreateBuiltInHlsl(
				ResolveEngineShaderPath(shaderName), projectAssetsRoot);
			NLS_LOG_INFO(
				std::string("[Startup] DeferredSceneRenderer shader ") + shaderName + " elapsedMs=" +
				std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - shaderBegin).count()));
			return shader;
		};

		m_gBufferShader = loadShader("DeferredGBuffer.hlsl");
		m_deferredDecalShader = loadShader("DeferredDecal.hlsl");
		m_lightingShader = loadShader("DeferredLighting.hlsl");
		m_hzbBuildShader = loadShader("HZBBuild.hlsl");
		m_hzbOcclusionShader = loadShader("HZBOcclusion.hlsl");

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

	bool DeferredSceneRenderer::SelectDeferredLightingVariant()
	{
		if (m_lightingMaterial == nullptr || m_lightingShader == nullptr)
			return false;

		const auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_driver);
		const bool usesVulkan = device != nullptr &&
			device->GetNativeDeviceInfo().backend == NLS::Render::RHI::NativeBackendType::Vulkan;
		if (!usesVulkan)
		{
			if (!m_deferredLightingVariantKeyword.empty())
				m_lightingMaterial->DisableKeyword(m_deferredLightingVariantKeyword);
			m_deferredLightingVariantKeyword.clear();
			return true;
		}

		const auto lighting = GetSceneLightingProvider().ReadPublishedLightingDescriptor();
		const size_t sceneLightCount = lighting != nullptr ? lighting->lights.size() : 0u;
		const uint32_t lightCount = static_cast<uint32_t>((std::min<size_t>)(sceneLightCount, 32u));
		const std::string desiredKeyword = lightCount == 0u
			? std::string{}
			: "NLS_DEFERRED_LIGHT_COUNT_" + std::to_string(lightCount);
		if (desiredKeyword == m_deferredLightingVariantKeyword)
			return true;

		if (!desiredKeyword.empty())
		{
			auto desiredKeywords = m_lightingMaterial->GetShaderLabKeywords();
			if (!m_deferredLightingVariantKeyword.empty())
				desiredKeywords.Disable(m_deferredLightingVariantKeyword);
			desiredKeywords.Enable(desiredKeyword);
			const std::vector<NLS::Render::ShaderCompiler::ShaderMacroDefinition> macros{
				{"NLS_DEFERRED_LIGHT_COUNT", std::to_string(lightCount)}};
			const auto& projectAssetsRoot = NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot();
			if (!NLS::Render::Resources::Loaders::ShaderLoader::EnsureBuiltInHlslVariant(
					*m_lightingShader,
					m_lightingShader->path,
					projectAssetsRoot,
					NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV,
					desiredKeywords.Hash(),
					macros))
			{
				NLS_LOG_ERROR(
					"DeferredSceneRenderer: failed to compile Vulkan deferred lighting variant for " +
					std::to_string(lightCount) + " lights.");
				return false;
			}
		}

		if (!m_deferredLightingVariantKeyword.empty())
			m_lightingMaterial->DisableKeyword(m_deferredLightingVariantKeyword);
		if (!desiredKeyword.empty())
			m_lightingMaterial->EnableKeyword(desiredKeyword);
		m_deferredLightingVariantKeyword = desiredKeyword;
		return true;
	}

		void DeferredSceneRenderer::EnsureDeferredPipelineResourceAssets()
		{
			if (m_pipelineResourceAssetsLoaded)
				return;
			LoadPipelineResources();
			m_pipelineResourceAssetsLoaded = true;
		}

		void DeferredSceneRenderer::ReleaseDeferredDecalPipelineResources()
		{
			ClearFrameDeferredDecalMaterialResolveCache();
			m_deferredDecalMaterialCache.clear();
#if defined(NLS_ENABLE_TEST_HOOKS)
			if (m_beforeDeferredDecalShaderDestroyForTesting)
				m_beforeDeferredDecalShaderDestroyForTesting();
			m_beforeDeferredDecalShaderDestroyForTesting = {};
#endif
			NLS::Render::Resources::Loaders::ShaderLoader::Destroy(m_deferredDecalShader);
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
				m_publishedGBufferDepthView.store(nullptr, std::memory_order_release);
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
			m_publishedGBufferDepthView.store(
				NLS::Render::FrameGraph::CaptureDeferredPreparedSceneResources(
					BuildDeferredPreparedSceneResourceRequest()).gbufferDepthView,
				std::memory_order_release);
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
		if (clearObservationPrimitiveCount)
			m_hzbOcclusionObservationPrimitiveCount = 0u;
	}

	void DeferredSceneRenderer::DiscardHZBObservationIfNoReadbackWasPublished()
	{
		if (m_hzbOcclusionResultReadbackState != nullptr ||
			m_hzbOcclusionResultReadbackCompletion != nullptr)
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
		auto found = m_gBufferMaterialResolveCache.find(sourceStamp.sourceMaterialInstanceId);
		const bool insertsNewSource = found == m_gBufferMaterialResolveCache.end();
		if (found != m_gBufferMaterialResolveCache.end() &&
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
		TrimMaterialResolveCacheForInsert(m_gBufferMaterialResolveCache, insertsNewSource);
		m_gBufferMaterialResolveCache[sourceStamp.sourceMaterialInstanceId] = {
			sourceStamp,
			&material
		};
		return material;
	}

	NLS::Render::Resources::Material& DeferredSceneRenderer::ResolveGBufferDrawableMaterial(
		NLS::Render::Resources::Material& sourceMaterial)
	{
		if (sourceMaterial.ResolveShaderForLightMode("GBuffer") != nullptr)
			return sourceMaterial;
		return ResolveFrameGBufferMaterial(sourceMaterial);
	}

	void DeferredSceneRenderer::ClearFrameGBufferMaterialResolveCache()
	{
		m_gBufferMaterialResolveCache.clear();
		ResetFrameGBufferMaterialResolveStats();
	}

	void DeferredSceneRenderer::ResetFrameGBufferMaterialResolveStats()
	{
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
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_AlbedoMap", sourceMaterial, "u_DiffuseMap");

		CopyMaterialParameterIfPresent(target, "u_Albedo", sourceMaterial, "_BaseColor");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_AlbedoMap", sourceMaterial, "_BaseMap");
		CopyMaterialParameterIfPresent(target, "u_Metallic", sourceMaterial, "_Metallic");
		CopyMaterialParameterIfPresent(target, "u_Roughness", sourceMaterial, "_Roughness");
		CopyMaterialParameterIfPresent(target, "u_MetallicMapChannel", sourceMaterial, "_MetallicMapChannel");
		CopyMaterialParameterIfPresent(target, "u_RoughnessMapChannel", sourceMaterial, "_RoughnessMapChannel");
		CopyMaterialParameterIfPresent(target, "u_AmbientOcclusion", sourceMaterial, "_AmbientOcclusion");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_MetallicMap", sourceMaterial, "_MetallicMap");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_RoughnessMap", sourceMaterial, "_RoughnessMap");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_AmbientOcclusionMap", sourceMaterial, "_OcclusionMap");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_NormalMap", sourceMaterial, "_NormalMap");
		target.SetRawParameter(
			"u_EnableNormalMapping",
			MaterialHasTextureParameter(sourceMaterial, "_NormalMap") ? 1.0f : 0.0f);
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_OpacityMap", sourceMaterial, "_OpacityMap");
		CopyMaterialParameterIfPresent(target, "u_Emissive", sourceMaterial, "_EmissiveColor");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_EmissiveMap", sourceMaterial, "_EmissiveMap");
		CopyMaterialParameterIfPresent(target, "u_Specular", sourceMaterial, "_SpecularColor");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_SpecularMap", sourceMaterial, "_SpecularMap");
		EnsureDeferredGBufferFallbackParameters(target);
	}

	std::unique_ptr<NLS::Render::Resources::Material> DeferredSceneRenderer::CreateDeferredDecalMaterial() const
	{
		if (m_deferredDecalShader == nullptr)
			return nullptr;
		auto material = std::make_unique<NLS::Render::Resources::Material>(m_deferredDecalShader);
		material->SetBlendable(true);
		material->SetColorWriting(true);
		return material;
	}

	NLS::Render::Resources::Material* DeferredSceneRenderer::GetOrCreateDeferredDecalMaterial(
		NLS::Render::Resources::Material& sourceMaterial)
	{
		NLS_PROFILE_SCOPE();
		if (m_deferredDecalShader == nullptr)
			return nullptr;
		const NLS::Render::Data::PipelineState pipelineState;
		const auto cacheKey = BuildDeferredDecalMaterialCacheKey(sourceMaterial, pipelineState);
		auto found = m_deferredDecalMaterialCache.find(cacheKey);
		if (found == m_deferredDecalMaterialCache.end())
		{
			DeferredDecalMaterialCacheEntry entry;
			entry.material = CreateDeferredDecalMaterial();
			if (entry.material == nullptr)
				return nullptr;
			found = m_deferredDecalMaterialCache.emplace(cacheKey, std::move(entry)).first;
		}

		auto& entry = found->second;
		const auto sourceStamp = BuildGBufferMaterialSyncStamp(sourceMaterial);
		if (entry.material != nullptr && entry.syncedStamp != sourceStamp)
		{
			NLS_PROFILE_NAMED_SCOPE("DeferredSceneRenderer::SyncDeferredDecalMaterial");
			SyncDeferredDecalMaterial(*entry.material, sourceMaterial);
			entry.syncedStamp = sourceStamp;
			++entry.syncCount;
		}
		return entry.material.get();
	}

	NLS::Render::Resources::Material* DeferredSceneRenderer::ResolveFrameDeferredDecalMaterial(
		NLS::Render::Resources::Material& sourceMaterial)
	{
		NLS_PROFILE_SCOPE();
		if (m_deferredDecalShader == nullptr)
			return nullptr;
		const auto sourceStamp = BuildGBufferMaterialSyncStamp(sourceMaterial);
		auto found = m_deferredDecalMaterialResolveCache.find(sourceStamp.sourceMaterialInstanceId);
		const bool insertsNewSource = found == m_deferredDecalMaterialResolveCache.end();
		if (found != m_deferredDecalMaterialResolveCache.end() &&
			found->second.material != nullptr &&
			found->second.sourceStamp == sourceStamp)
		{
			++m_frameDeferredDecalMaterialResolveHitCount;
			return found->second.material;
		}

		++m_frameDeferredDecalMaterialResolveMissCount;
		auto* material = GetOrCreateDeferredDecalMaterial(sourceMaterial);
		if (material == nullptr)
			return nullptr;
		TrimMaterialResolveCacheForInsert(m_deferredDecalMaterialResolveCache, insertsNewSource);
		m_deferredDecalMaterialResolveCache[sourceStamp.sourceMaterialInstanceId] = {
			sourceStamp,
			material
		};
		return material;
	}

	NLS::Render::Resources::Material* DeferredSceneRenderer::ResolveDeferredDecalDrawableMaterial(
		NLS::Render::Resources::Material& sourceMaterial)
	{
		if (sourceMaterial.ResolveShaderForLightMode("DeferredDecal") != nullptr)
			return &sourceMaterial;
		return ResolveFrameDeferredDecalMaterial(sourceMaterial);
	}

	void DeferredSceneRenderer::ClearFrameDeferredDecalMaterialResolveCache()
	{
		m_deferredDecalMaterialResolveCache.clear();
		ResetFrameDeferredDecalMaterialResolveStats();
	}

	void DeferredSceneRenderer::ResetFrameDeferredDecalMaterialResolveStats()
	{
		m_frameDeferredDecalMaterialResolveHitCount = 0u;
		m_frameDeferredDecalMaterialResolveMissCount = 0u;
	}

	void DeferredSceneRenderer::SyncDeferredDecalMaterial(
		NLS::Render::Resources::Material& target,
		const NLS::Render::Resources::Material& sourceMaterial) const
	{
		target.SetGPUInstances(sourceMaterial.GetGPUInstances());
		target.FillUniform();
		target.SetRawParameter("u_Albedo", NLS::Maths::Vector4(1.0f, 1.0f, 1.0f, 1.0f));
		for (const auto& [name, value] : sourceMaterial.GetParameterBlock().Data())
		{
			if (target.GetParameterBlock().Contains(name))
				SetMaterialParameter(target, name.c_str(), value);
		}

		CopyMaterialParameterIfPresent(target, "u_Albedo", sourceMaterial, "u_Diffuse");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_AlbedoMap", sourceMaterial, "u_AlbedoMap");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_OpacityMap", sourceMaterial, "u_OpacityMap");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_AlbedoMap", sourceMaterial, "u_DiffuseMap");
		CopyMaterialParameterIfPresent(target, "u_Albedo", sourceMaterial, "_BaseColor");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_AlbedoMap", sourceMaterial, "_BaseMap");
		CopyMaterialParameterAndTexturePathIfPresent(target, "u_OpacityMap", sourceMaterial, "_OpacityMap");
		EnsureDeferredDecalFallbackParameters(target);
	}

	void DeferredSceneRenderer::DrawGBufferOpaques(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		if (!m_gBufferShader)
			return;

		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();

		if (scene.drawables == nullptr)
			return;

		for (const auto& entry : scene.drawables->opaques)
		{
			const auto& drawable = entry.second;
			if (!drawable.material)
				continue;

			auto& gbufferMaterial = ResolveGBufferDrawableMaterial(*drawable.material);

			const auto gBufferOverrides = BuildGBufferMaterialOverrides(*drawable.material);

			DrawEntity(drawable, gbufferMaterial, gBufferOverrides, pso.depthFunc, "GBuffer");
		}
	}

	void DeferredSceneRenderer::DrawDecals(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();

		if (scene.drawables == nullptr)
			return;

		for (const auto& entry : scene.drawables->decals)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;

			auto* decalMaterial = ResolveDeferredDecalDrawableMaterial(*drawable.material);
			if (decalMaterial == nullptr)
				continue;

			const auto decalOverrides = BuildDeferredDecalMaterialOverrides(*drawable.material);

			DrawEntity(drawable, *decalMaterial, decalOverrides, GetDeferredDecalDepthCompare(), "DeferredDecal");
		}
	}

	void DeferredSceneRenderer::DrawLightingPass(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		if (!m_lightingMaterial || !m_fullscreenQuad || !m_gBufferAlbedoTexture || !m_gBufferNormalTexture || !m_gBufferMaterialTexture)
			return;
		if (!SelectDeferredLightingVariant())
			return;

		NLS::Render::Resources::TextureCube* skyboxTexture = nullptr;
		NLS::Render::Resources::Material* skyboxMaterial = nullptr;
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
		if (scene.drawables == nullptr)
			return;
		for (const auto& entry : scene.drawables->skyboxes)
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
		NLS::Render::Entities::Drawable lightingDrawable;
		lightingDrawable.mesh = m_fullscreenQuad.get();
		lightingDrawable.material = m_lightingMaterial.get();
		lightingDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;

		NLS::Render::Resources::MaterialPipelineStateOverrides compositeOverrides;
		compositeOverrides.depthTest = false;
		compositeOverrides.depthWrite = false;
		compositeOverrides.hasDepthAttachment = false;
		compositeOverrides.culling = false;
		compositeOverrides.colorWrite = true;

		PreparedRecordedDraw preparedDraw;
		if (!PrepareRecordedDraw(lightingDrawable, compositeOverrides, pso.depthFunc, "Forward", preparedDraw))
			return;

		BindPreparedGraphicsPipeline(preparedDraw);
		if (preparedDraw.commandBuffer != nullptr &&
			GetLightGridGraphicsPassBindingSet() != nullptr &&
			preparedDraw.passBindingSet != GetLightGridGraphicsPassBindingSet())
		{
			preparedDraw.commandBuffer->BindBindingSet(
				NLS::Render::RHI::BindingPointMap::kPassDescriptorSet,
				GetLightGridGraphicsPassBindingSet());
		}
		BindPreparedMaterialBindingSet(preparedDraw);
		SubmitPreparedDraw(preparedDraw);
	}

	void DeferredSceneRenderer::DrawTransparents(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();

		NLS::Render::Resources::MaterialPipelineStateOverrides transparentOverrides;
		transparentOverrides.depthWrite = false;

		if (scene.drawables == nullptr)
			return;

		for (const auto& entry : scene.drawables->transparents)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;
			DrawEntity(drawable, transparentOverrides, pso.depthFunc, "Forward");
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

	NLS::Render::Resources::Material& DeferredSceneRendererTestAccess::ResolveGBufferDrawableMaterialForTesting(
		DeferredSceneRenderer& renderer,
		NLS::Render::Resources::Material& sourceMaterial)
	{
		return renderer.ResolveGBufferDrawableMaterial(sourceMaterial);
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

		uint64_t DeferredSceneRendererTestAccess::GetThreadedDrawCountEstimateHitCount(
			const DeferredSceneRenderer& renderer)
		{
			return renderer.m_threadedDrawCountEstimateHitCount;
		}

		uint64_t DeferredSceneRendererTestAccess::GetThreadedDrawCountEstimateMismatchCount(
			const DeferredSceneRenderer& renderer)
		{
			return renderer.m_threadedDrawCountEstimateMismatchCount;
		}

		DeferredSceneRenderer::DeferredThreadedDrawCounts
		DeferredSceneRendererTestAccess::EstimateThreadedDeferredDrawCountsForTesting(
			DeferredSceneRenderer& renderer,
			const BaseSceneRenderer::AllDrawables& drawables)
		{
			return renderer.EstimateThreadedDeferredDrawCounts(drawables);
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

	void DeferredSceneRendererTestAccess::ResetFrameGBufferMaterialResolveStats(
		DeferredSceneRenderer& renderer)
	{
		renderer.ResetFrameGBufferMaterialResolveStats();
	}

	uint64_t DeferredSceneRendererTestAccess::GetFrameGBufferMaterialResolveCacheSize(
		const DeferredSceneRenderer& renderer)
	{
		return static_cast<uint64_t>(renderer.m_gBufferMaterialResolveCache.size());
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

	NLS::Render::Resources::Material* DeferredSceneRendererTestAccess::ResolveDeferredDecalDrawableMaterialForTesting(
		DeferredSceneRenderer& renderer,
		NLS::Render::Resources::Material& sourceMaterial)
	{
		return renderer.ResolveDeferredDecalDrawableMaterial(sourceMaterial);
	}

	DeferredSceneRendererTestAccess::DeferredDecalMaterialCache& DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(
		DeferredSceneRenderer& renderer)
	{
		return renderer.m_deferredDecalMaterialCache;
	}

	const DeferredSceneRendererTestAccess::DeferredDecalMaterialCache& DeferredSceneRendererTestAccess::GetDeferredDecalMaterialCache(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_deferredDecalMaterialCache;
	}

	void DeferredSceneRendererTestAccess::SetDeferredDecalShader(
		DeferredSceneRenderer& renderer,
		NLS::Render::Resources::Shader* shader)
	{
		if (renderer.m_deferredDecalShader == shader)
			return;
		renderer.ClearFrameDeferredDecalMaterialResolveCache();
		renderer.m_deferredDecalMaterialCache.clear();
		renderer.m_deferredDecalShader = shader;
	}

	NLS::Render::Resources::Shader* DeferredSceneRendererTestAccess::GetDeferredDecalShader(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_deferredDecalShader;
	}

	void DeferredSceneRendererTestAccess::ClearFrameDeferredDecalMaterialResolveCache(
		DeferredSceneRenderer& renderer)
	{
		renderer.ClearFrameDeferredDecalMaterialResolveCache();
	}

	void DeferredSceneRendererTestAccess::ResetFrameDeferredDecalMaterialResolveStats(
		DeferredSceneRenderer& renderer)
	{
		renderer.ResetFrameDeferredDecalMaterialResolveStats();
	}

	uint64_t DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveCacheSize(
		const DeferredSceneRenderer& renderer)
	{
		return static_cast<uint64_t>(renderer.m_deferredDecalMaterialResolveCache.size());
	}

	uint64_t DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveHitCount(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_frameDeferredDecalMaterialResolveHitCount;
	}

	uint64_t DeferredSceneRendererTestAccess::GetFrameDeferredDecalMaterialResolveMissCount(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.m_frameDeferredDecalMaterialResolveMissCount;
	}

	bool DeferredSceneRendererTestAccess::HasDeferredThreadedPipelineResourcesForTesting(
		const DeferredSceneRenderer& renderer)
	{
		return renderer.HasDeferredThreadedPipelineResources();
	}

#if defined(NLS_ENABLE_TEST_HOOKS)
	size_t DeferredSceneRendererTestAccess::GetMaterialResolveCacheMaxEntriesForTesting()
	{
		return kMaterialResolveCacheMaxEntries;
	}

	void DeferredSceneRendererTestAccess::SetDeferredDecalShaderDestroyProbe(
		DeferredSceneRenderer& renderer,
		std::function<void()> probe)
	{
		renderer.m_beforeDeferredDecalShaderDestroyForTesting = std::move(probe);
	}

	void DeferredSceneRendererTestAccess::ReleaseDeferredDecalPipelineResourcesForTesting(
		DeferredSceneRenderer& renderer)
	{
		renderer.ReleaseDeferredDecalPipelineResources();
	}
#endif

	NLS::Render::Resources::MaterialPipelineStateOverrides DeferredSceneRendererTestAccess::BuildDeferredDecalMaterialOverridesForTesting(
		const NLS::Render::Resources::Material& sourceMaterial)
	{
		return BuildDeferredDecalMaterialOverrides(sourceMaterial);
	}

	NLS::Render::Resources::MaterialPipelineStateOverrides DeferredSceneRendererTestAccess::BuildGBufferMaterialOverridesForTesting(
		const NLS::Render::Resources::Material& sourceMaterial)
	{
		return BuildGBufferMaterialOverrides(sourceMaterial);
	}

	NLS::Render::Settings::EComparaisonAlgorithm DeferredSceneRendererTestAccess::GetDeferredDecalDepthCompareForTesting()
	{
		return GetDeferredDecalDepthCompare();
	}

	bool DeferredSceneRendererTestAccess::ShouldSkipThreadedDeferredFramePublishForTesting(
		const NLS::Render::Context::RenderSceneBuildData& snapshot,
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
		NLS::Render::Context::RenderSceneBuildData& snapshot,
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
