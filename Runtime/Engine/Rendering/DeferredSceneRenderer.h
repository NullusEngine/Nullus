#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <Rendering/Buffers/MultiFramebuffer.h>

#include "Rendering/BaseSceneRenderer.h"

namespace NLS::Render::Resources
{
	struct MaterialPipelineStateOverrides;
	class Material;
	class Mesh;
	class Shader;
	class Texture2D;
	class TextureCube;
}

namespace NLS::Render::RHI
{
	class RHITextureView;
}

namespace NLS::Engine::Rendering
{
	struct DeferredSceneRendererTestAccess;

	class NLS_ENGINE_API DeferredSceneRenderer : public BaseSceneRenderer
	{
		friend struct DeferredSceneRendererTestAccess;

	public:
		struct ConstructionOptions
		{
			// Allows contract tests to validate factory selection without compiling renderer-owned shaders.
			bool loadPipelineResources = true;
		};

		explicit DeferredSceneRenderer(NLS::Render::Context::Driver& p_driver);
		DeferredSceneRenderer(
			NLS::Render::Context::Driver& p_driver,
			ConstructionOptions options);
		~DeferredSceneRenderer() override;

		void BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void DrawFrame() override;
		bool IsThreadedFramePublishSkippedForCurrentFrame() const;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> GetDeferredPreparedSceneDepthViewForEditorHelpers() const;

		struct GBufferMaterialSyncStamp
		{
			uint64_t sourceMaterialInstanceId = 0u;
			uint64_t parameterRevision = 0u;
			uint64_t renderStateRevision = 0u;
			uint64_t bindingRevision = 0u;
			uint64_t shaderInstanceId = 0u;
			uint64_t shaderGeneration = 0u;
		};
		struct GBufferMaterialCacheEntry
		{
			std::unique_ptr<NLS::Render::Resources::Material> material;
			GBufferMaterialSyncStamp syncedStamp;
			uint64_t syncCount = 0u;
		};
		struct FrameGBufferMaterialResolveEntry
		{
			GBufferMaterialSyncStamp sourceStamp;
			NLS::Render::Resources::Material* material = nullptr;
		};

	protected:
		struct DeferredSceneDescriptor
		{
			AllDrawables drawables;
			NLS::Render::Context::RenderScenePackage scenePackage;
			bool hasSkyboxTexture = false;
		};

		static void SynchronizeThreadedDeferredSnapshot(
			NLS::Render::Context::FrameSnapshot& snapshot,
			uint64_t queuedGBufferDrawCount,
			uint64_t queuedDecalDrawCount,
			uint64_t queuedLightingDrawCount,
			uint64_t queuedTransparentDrawCount);
		NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest BuildDeferredPreparedSceneResourceRequest() const;
		void LogPreparedDrawResult(
			const char* stage,
			bool captured,
			bool queued,
			const PreparedRecordedDraw& preparedDraw) const;
		NLS::Render::Context::PreparedRenderSceneBuilder BuildDeferredPreparedRenderSceneBuilder(
			NLS::Render::Context::FrameSnapshot snapshot,
			bool hasSkyboxTexture,
			std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs = {},
			std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedPassMetadata = {},
			std::shared_ptr<NLS::Render::RHI::RHITexture> preferredReadbackTexture = nullptr,
			uint64_t additionalRenderTargetUseCount = 0u) const;
		NLS::Render::Context::PreparedRenderSceneBuilder BuildPreparedRenderSceneBuilder(
			const NLS::Render::Context::FrameSnapshot& snapshot) const override;
		bool TryPublishThreadedFrame() override;

	private:
		void LoadPipelineResources();
		void EnsureGBufferTargets(uint16_t width, uint16_t height);
		bool HasDeferredThreadedPipelineResources() const;
		std::unique_ptr<NLS::Render::Resources::Material> CreateGBufferMaterial() const;
		NLS::Render::Resources::Material& GetOrCreateGBufferMaterial(NLS::Render::Resources::Material& sourceMaterial);
		NLS::Render::Resources::Material& ResolveFrameGBufferMaterial(NLS::Render::Resources::Material& sourceMaterial);
		void ClearFrameGBufferMaterialResolveCache();
		void SyncGBufferMaterial(NLS::Render::Resources::Material& target, const NLS::Render::Resources::Material& sourceMaterial) const;
		void DrawGBufferOpaques(NLS::Render::Data::PipelineState pso);
		void DrawDecals(NLS::Render::Data::PipelineState pso);
		void DrawLightingPass(NLS::Render::Data::PipelineState pso);
		void DrawTransparents(NLS::Render::Data::PipelineState pso);

	private:
		NLS::Render::Buffers::MultiFramebuffer m_gBuffer;
		std::unique_ptr<NLS::Render::Resources::Mesh> m_fullscreenQuad;
		std::unique_ptr<NLS::Render::Resources::Material> m_lightingMaterial;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferAlbedoTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferNormalTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferMaterialTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferDepthTexture;
		std::unordered_map<std::string, GBufferMaterialCacheEntry> m_gBufferMaterialCache;
		std::unordered_map<uint64_t, FrameGBufferMaterialResolveEntry> m_frameGBufferMaterialResolveCache;
		NLS::Render::Resources::Shader* m_gBufferShader = nullptr;
		NLS::Render::Resources::Shader* m_lightingShader = nullptr;
		uint64_t m_threadedQueuedGBufferDrawCount = 0u;
		uint64_t m_threadedQueuedDecalDrawCount = 0u;
		uint64_t m_threadedQueuedLightingDrawCount = 0u;
		uint64_t m_threadedQueuedTransparentDrawCount = 0u;
		uint64_t m_frameGBufferMaterialSyncCount = 0u;
		uint64_t m_frameGBufferMaterialResolveHitCount = 0u;
		uint64_t m_frameGBufferMaterialResolveMissCount = 0u;
		bool m_skipThreadedFramePublish = false;
	};

	struct NLS_ENGINE_API DeferredSceneRendererTestAccess final
	{
		using GBufferMaterialCache = std::unordered_map<std::string, DeferredSceneRenderer::GBufferMaterialCacheEntry>;

		static NLS::Render::Resources::Material& GetOrCreateGBufferMaterial(
			DeferredSceneRenderer& renderer,
			NLS::Render::Resources::Material& sourceMaterial);
		static NLS::Render::Resources::Material& ResolveFrameGBufferMaterial(
			DeferredSceneRenderer& renderer,
			NLS::Render::Resources::Material& sourceMaterial);
		static GBufferMaterialCache& GetGBufferMaterialCache(DeferredSceneRenderer& renderer);
		static const GBufferMaterialCache& GetGBufferMaterialCache(const DeferredSceneRenderer& renderer);
		static void SetGBufferShader(DeferredSceneRenderer& renderer, NLS::Render::Resources::Shader* shader);
		static NLS::Render::Resources::Shader* GetGBufferShader(const DeferredSceneRenderer& renderer);
		static void ResetFrameGBufferMaterialSyncCount(DeferredSceneRenderer& renderer);
		static uint64_t GetFrameGBufferMaterialSyncCount(const DeferredSceneRenderer& renderer);
		static void ClearFrameGBufferMaterialResolveCache(DeferredSceneRenderer& renderer);
		static uint64_t GetFrameGBufferMaterialResolveCacheSize(const DeferredSceneRenderer& renderer);
		static uint64_t GetFrameGBufferMaterialResolveHitCount(const DeferredSceneRenderer& renderer);
		static uint64_t GetFrameGBufferMaterialResolveMissCount(const DeferredSceneRenderer& renderer);
		static NLS::Render::Resources::MaterialPipelineStateOverrides BuildDeferredDecalMaterialOverridesForTesting(
			const NLS::Render::Resources::Material& sourceMaterial);
		static NLS::Render::Settings::EComparaisonAlgorithm GetDeferredDecalDepthCompareForTesting();
		static bool ShouldSkipThreadedDeferredFramePublishForTesting(
			const NLS::Render::Context::FrameSnapshot& snapshot,
			uint64_t queuedGBufferDrawCount,
			uint64_t queuedDecalDrawCount,
			uint64_t queuedLightingDrawCount,
			uint64_t queuedTransparentDrawCount);
		static void SynchronizeThreadedDeferredSnapshotForTesting(
			NLS::Render::Context::FrameSnapshot& snapshot,
			uint64_t queuedGBufferDrawCount,
			uint64_t queuedDecalDrawCount,
			uint64_t queuedLightingDrawCount,
			uint64_t queuedTransparentDrawCount);
		static void EnsureGBufferTargets(DeferredSceneRenderer& renderer, uint16_t width, uint16_t height);
		static const NLS::Render::Resources::Texture2D* GetGBufferAlbedoTexture(const DeferredSceneRenderer& renderer);
		static const NLS::Render::Resources::Texture2D* GetGBufferNormalTexture(const DeferredSceneRenderer& renderer);
		static const NLS::Render::Resources::Texture2D* GetGBufferMaterialTexture(const DeferredSceneRenderer& renderer);
		static const NLS::Render::Resources::Texture2D* GetGBufferDepthTexture(const DeferredSceneRenderer& renderer);
	};
}
