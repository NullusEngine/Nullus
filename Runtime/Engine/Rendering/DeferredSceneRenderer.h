#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <Rendering/Buffers/MultiFramebuffer.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Rendering/RHI/Utils/PipelineCache/PipelineCache.h>

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
	class RHIBindingLayout;
	class RHIBindingSet;
	class RHIBuffer;
	class RHICompletionToken;
	class RHIComputePipeline;
	class RHIPipelineLayout;
	class RHITexture;
	class RHITextureView;
}

namespace NLS::Engine::Rendering
{
	struct SceneOcclusionPrimitivePacket;
	struct DeferredSceneRendererTestAccess;

	class NLS_ENGINE_API DeferredSceneRenderer : public BaseSceneRenderer
	{
		friend struct DeferredSceneRendererTestAccess;

	public:
			struct ConstructionOptions
			{
				// Allows contract tests to validate factory selection without compiling renderer-owned shaders.
				bool loadPipelineResources = true;
				bool deferPipelineResourcesUntilFirstFrame = false;
				};

			/**
			 * Per-pass draw counts the deferred scene is expected to queue this frame.
			 *
			 * These come from cheap drawable resolvability checks and create no
			 * pipelines, binding sets, or object-data uploads, so the game thread can
			 * take them before publishing. That keeps the frame's pass counts knowable
			 * at publish time once the heavy capture moves to the render thread.
			 */
				struct DeferredThreadedDrawCounts
				{
					uint64_t gBufferDrawCount = 0u;
					uint64_t decalDrawCount = 0u;
					uint64_t lightingDrawCount = 0u;
					uint64_t transparentDrawCount = 0u;
					uint64_t unresolvedDecalDrawCount = 0u;

				[[nodiscard]] bool operator==(const DeferredThreadedDrawCounts& other) const noexcept = default;
			};

			struct DeferredThreadedCaptureResult
			{
				DeferredThreadedDrawCounts drawCounts;
				uint64_t unresolvedDecalDrawCount = 0u;
				bool lightingCaptureSucceeded = true;
                    std::vector<NLS::Render::Context::RecordedDrawCommandInput> forwardOpaqueDepth,forwardOpaqueColor;
			};

			DeferredThreadedDrawCounts EstimateThreadedDeferredDrawCounts(const AllDrawables& drawables);

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
		struct GBufferMaterialResolveEntry
		{
			GBufferMaterialSyncStamp sourceStamp;
			NLS::Render::Resources::Material* material = nullptr;
		};
		using DeferredDecalMaterialSyncStamp = GBufferMaterialSyncStamp;
		struct DeferredDecalMaterialCacheEntry
		{
			std::unique_ptr<NLS::Render::Resources::Material> material;
			DeferredDecalMaterialSyncStamp syncedStamp;
			uint64_t syncCount = 0u;
		};
		struct DeferredDecalMaterialResolveEntry
		{
			DeferredDecalMaterialSyncStamp sourceStamp;
			NLS::Render::Resources::Material* material = nullptr;
		};

	protected:
		struct DeferredSceneDescriptor
		{
			// Shared with the pending prepared-render-scene builder so a frame's
			// capture input can outlive BeginFrame without being deep-copied.
			std::shared_ptr<AllDrawables> drawables;
			NLS::Render::Context::RenderScenePackage scenePackage;
			bool hasSkyboxTexture = false;
		};

		/**
		 * Everything the deferred threaded capture needs besides the renderer-owned
		 * pipeline resources. The game thread fills this in and the prepared builder
		 * keeps it alive until capture runs.
		 */
			struct DeferredThreadedCaptureInput
			{
				struct HZBReadbackHandoff
				{
					mutable std::mutex mutex;
					std::optional<NLS::Render::Context::PostSubmitBufferReadbackRequest> request;
					bool ready = false;
					bool cancelled = false;
					bool observationStarted = false;
				};

				std::shared_ptr<AllDrawables> drawables;
				std::shared_ptr<SceneParseInput> sceneParseInput;
				SceneParseCaptureDropper sceneParseDropper;
				bool suppressLightGridCompute = false;
				NLS::Render::Resources::TextureCube* skyboxTexture = nullptr;
				NLS::Render::Resources::Material* skyboxMaterial = nullptr;
				bool logPreparedDrawResults = false;
				bool hzbFrameInputEnabled = false;
				std::shared_ptr<HZBReadbackHandoff> hzbReadbackHandoff;
			// Recorded draws produced by this frame's threaded capture. The builder
			// injects them into the package, so they no longer have to travel through
			// shared renderer scratch state and the snapshot.
			std::shared_ptr<std::vector<NLS::Render::Context::RecordedDrawCommandInput>>
				recordedDrawCommands;
		};

			DeferredThreadedCaptureInput BuildThreadedCaptureInputForPreparedBuilder();
			bool ResolveThreadedSceneCapture(
				NLS::Render::Context::RenderSceneBuildData& snapshot,
				DeferredThreadedCaptureInput& captureInput,
				const NLS::Render::FrameGraph::FrozenFrameDescriptor& frame);
			void ConsumeThreadedHZBReadbackHandoffs();
			void CancelCurrentThreadedHZBReadbackHandoff();

		static void SynchronizeThreadedDeferredSnapshot(
			NLS::Render::Context::RenderSceneBuildData& snapshot,
			uint64_t queuedGBufferDrawCount,
			uint64_t queuedDecalDrawCount,
			uint64_t queuedLightingDrawCount,
			uint64_t queuedTransparentDrawCount);
			NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest BuildDeferredPreparedSceneResourceRequest() const;
			DeferredThreadedCaptureResult CaptureThreadedDeferredDraws(
				const AllDrawables& drawables,
				NLS::Render::Resources::TextureCube* skyboxTexture,
				NLS::Render::Resources::Material* skyboxMaterial,
				bool logPreparedDrawResults,
				std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outCommands);

		void LogPreparedDrawResult(
			const char* stage,
			bool captured,
			bool queued,
			const PreparedRecordedDraw& preparedDraw) const;
		NLS::Render::Context::RenderSceneBuildCommand BuildDeferredRenderSceneCommand(
			NLS::Render::Context::RenderFramePacket packet,
			std::vector<NLS::Render::Context::RecordedDrawCommandInput> helperDrawCommands,
			uint64_t helperPassCount,
			bool hasSkyboxTexture,
			DeferredThreadedCaptureInput captureInput,
			std::vector<NLS::Render::Context::RenderPassCommandInput> appendedPassInputs = {},
					std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> appendedPassMetadata = {},
						std::shared_ptr<NLS::Render::RHI::RHITexture> preferredReadbackTexture = nullptr,
						uint64_t preferredReadbackTextureGeneration = 0u,
						uint64_t additionalRenderTargetUseCount = 0u,
						bool includeDeferredSceneExecution = true);
		NLS::Render::Context::RenderSceneBuildCommand BuildRenderSceneCommand(
			const NLS::Render::Context::RenderFramePacket& packet) override;
			bool TryPublishThreadedFrame() override;
			void OnThreadedFramePublishFailed() override;

		private:
			void LoadPipelineResources();
			void EnsureDeferredPipelineResourceAssets();
			bool SelectDeferredLightingVariant();
			void ReleaseDeferredDecalPipelineResources();
			void EnsureGBufferTargets(uint16_t width, uint16_t height);
			std::shared_ptr<NLS::Render::RHI::RHITextureView> LoadPublishedGBufferDepthView() const;
			void StorePublishedGBufferDepthView(std::shared_ptr<NLS::Render::RHI::RHITextureView> view);
		bool HasDeferredThreadedPipelineResources() const;
		bool EnsureHZBTargets(uint16_t width, uint16_t height);
		bool PrepareHZBOcclusionPrimitiveBuffers(std::span<const SceneOcclusionPrimitivePacket> packets);
		bool PollHZBOcclusionResultReadback();
		bool BeginHZBOcclusionResultReadback();
		std::optional<NLS::Render::Context::PostSubmitBufferReadbackRequest> BuildHZBPostSubmitReadbackRequest(
			bool waitForLastComputeQueueCompletion = true);
		void AdoptHZBPostSubmitReadbackRequest(
			const NLS::Render::Context::PostSubmitBufferReadbackRequest& request);
		void ClearHZBPendingResultReadback(bool clearObservationPrimitiveCount = true);
		void DiscardHZBObservationIfNoReadbackWasPublished();
		bool IsHZBOcclusionSupported() const;
		void BeginHZBOcclusionObservationFrame(
			const SceneOcclusionFrameInput& frame,
			std::span<const SceneOcclusionPrimitiveInput> primitiveInputs);
		SceneOcclusionObservationStats CompleteHZBOcclusionObservationFrame(
			std::span<const uint32_t> primitiveResultFlags);
		bool EnsureHZBPipelines();
		bool PrepareHZBFrameResources(const NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest& deferredResources);
		NLS::Render::FrameGraph::HZBFrameResourceRequest BuildHZBFrameResourceRequest() const;
		std::unique_ptr<NLS::Render::Resources::Material> CreateGBufferMaterial() const;
		NLS::Render::Resources::Material& GetOrCreateGBufferMaterial(NLS::Render::Resources::Material& sourceMaterial);
		NLS::Render::Resources::Material& ResolveFrameGBufferMaterial(NLS::Render::Resources::Material& sourceMaterial);
		NLS::Render::Resources::Material& ResolveGBufferDrawableMaterial(NLS::Render::Resources::Material& sourceMaterial);
		void ClearFrameGBufferMaterialResolveCache();
		void ResetFrameGBufferMaterialResolveStats();
		void SyncGBufferMaterial(NLS::Render::Resources::Material& target, const NLS::Render::Resources::Material& sourceMaterial) const;
		std::unique_ptr<NLS::Render::Resources::Material> CreateDeferredDecalMaterial() const;
		NLS::Render::Resources::Material* GetOrCreateDeferredDecalMaterial(NLS::Render::Resources::Material& sourceMaterial);
		NLS::Render::Resources::Material* ResolveFrameDeferredDecalMaterial(NLS::Render::Resources::Material& sourceMaterial);
		NLS::Render::Resources::Material* ResolveDeferredDecalDrawableMaterial(NLS::Render::Resources::Material& sourceMaterial);
		void ClearFrameDeferredDecalMaterialResolveCache();
		void ResetFrameDeferredDecalMaterialResolveStats();
		void SyncDeferredDecalMaterial(NLS::Render::Resources::Material& target, const NLS::Render::Resources::Material& sourceMaterial) const;
		void DrawGBufferOpaques(NLS::Render::Data::PipelineState pso);
		void DrawDecals(NLS::Render::Data::PipelineState pso);
		void DrawLightingPass(NLS::Render::Data::PipelineState pso);
		void DrawTransparents(NLS::Render::Data::PipelineState pso);

	private:
		NLS::Render::Buffers::MultiFramebuffer m_gBuffer;
		std::unique_ptr<NLS::Render::Resources::Mesh> m_fullscreenQuad;
			std::unique_ptr<NLS::Render::Resources::Material> m_lightingMaterial;
			std::string m_deferredLightingVariantKeyword;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferAlbedoTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferNormalTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferMaterialTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferDepthTexture;
			// libc++ has no std::atomic<std::shared_ptr<T>> specialisation, so the
			// published view is guarded by a mutex. Loads/stores happen a few times per
			// frame, where the lock is not measurable.
			mutable std::mutex m_publishedGBufferDepthViewMutex;
			std::shared_ptr<NLS::Render::RHI::RHITextureView> m_publishedGBufferDepthView;
		std::shared_ptr<NLS::Render::RHI::RHITexture> m_hzbTexture;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> m_hzbOcclusionPrimitiveInputBuffer;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> m_hzbOcclusionPrimitiveResultBuffer;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> m_hzbOcclusionConstantsBuffer;
		std::shared_ptr<NLS::Render::RHI::RHITexture> m_hzbPreparedDepthTexture;
		std::shared_ptr<NLS::Render::RHI::RHITexture> m_hzbPreparedHZBTexture;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> m_hzbPreparedOcclusionPrimitiveInputBuffer;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> m_hzbPreparedOcclusionPrimitiveResultBuffer;
		std::shared_ptr<NLS::Render::RHI::RHIBuffer> m_hzbPreparedOcclusionConstantsBuffer;
		std::shared_ptr<NLS::Render::RHI::RHICompletionToken> m_hzbOcclusionResultReadbackCompletion;
		std::shared_ptr<std::vector<uint32_t>> m_hzbOcclusionResultReadbackFlags;
		std::shared_ptr<NLS::Render::Context::PostSubmitBufferReadbackState> m_hzbOcclusionResultReadbackState;
			std::shared_ptr<DeferredThreadedCaptureInput::HZBReadbackHandoff> m_currentThreadedHZBReadbackHandoff;
			std::vector<std::shared_ptr<DeferredThreadedCaptureInput::HZBReadbackHandoff>> m_threadedHZBReadbackHandoffs;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> m_hzbDepthReadView;
		std::shared_ptr<NLS::Render::RHI::RHITextureView> m_hzbReadView;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> m_hzbMipReadViews;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> m_hzbMipWriteViews;
		std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_hzbBuildBindingLayout;
		std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> m_hzbOcclusionBindingLayout;
		std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> m_hzbBuildPipelineLayout;
		std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> m_hzbOcclusionPipelineLayout;
		std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_hzbBuildPipeline;
		std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> m_hzbOcclusionPipeline;
		std::vector<std::shared_ptr<NLS::Render::RHI::RHIBindingSet>> m_hzbBuildBindingSets;
		std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_hzbOcclusionBindingSet;
		std::unordered_map<std::string, GBufferMaterialCacheEntry> m_gBufferMaterialCache;
				std::unordered_map<uint64_t, GBufferMaterialResolveEntry> m_gBufferMaterialResolveCache;
				std::unordered_map<std::string, DeferredDecalMaterialCacheEntry> m_deferredDecalMaterialCache;
				std::unordered_map<uint64_t, DeferredDecalMaterialResolveEntry> m_deferredDecalMaterialResolveCache;
				std::shared_ptr<SceneParseInput> m_pendingThreadedSceneParse;
				SceneParseCaptureDropper m_pendingThreadedSceneParseDropper;
				NLS::Render::Resources::Shader* m_gBufferShader = nullptr;
		NLS::Render::Resources::Shader* m_deferredDecalShader = nullptr;
		NLS::Render::Resources::Shader* m_lightingShader = nullptr;
		NLS::Render::Resources::Shader* m_hzbBuildShader = nullptr;
		NLS::Render::Resources::Shader* m_hzbOcclusionShader = nullptr;
		NLS::Render::RHI::PipelineCacheKey m_hzbBuildPipelineKey;
		NLS::Render::RHI::PipelineCacheKey m_hzbOcclusionPipelineKey;
		std::vector<std::array<uint32_t, 3u>> m_hzbBuildDispatchGroupsByMip;
		std::array<uint32_t, 3u> m_hzbOcclusionDispatchGroups{ 1u, 1u, 1u };
		uint32_t m_hzbOcclusionPrimitiveCount = 1u;
		uint32_t m_hzbOcclusionObservationPrimitiveCount = 0u;
				// Telemetry validating the game-thread capturable-count estimate against the capture loop.
				uint64_t m_threadedDrawCountEstimateHitCount = 0u;
				uint64_t m_threadedDrawCountEstimateMismatchCount = 0u;
				uint64_t m_threadedDrawCountEstimateGbufferDelta = 0u;
				uint64_t m_threadedDrawCountEstimateDecalDelta = 0u;
				uint64_t m_threadedDrawCountEstimateLightingDelta = 0u;
				uint64_t m_threadedDrawCountEstimateTransparentDelta = 0u;
		uint64_t m_frameGBufferMaterialSyncCount = 0u;
			uint64_t m_frameGBufferMaterialResolveHitCount = 0u;
			uint64_t m_frameGBufferMaterialResolveMissCount = 0u;
			uint64_t m_frameDeferredDecalMaterialResolveHitCount = 0u;
			uint64_t m_frameDeferredDecalMaterialResolveMissCount = 0u;
			mutable uint32_t m_framePreparedDrawDiagnosticLogCount = 0u;
			bool m_skipThreadedFramePublish = false;
			bool m_pipelineResourceAssetsLoaded = false;
			bool m_deferPipelineResourceAssetsUntilFirstFrame = false;
#if defined(NLS_ENABLE_TEST_HOOKS)
			std::function<void()> m_beforeDeferredDecalShaderDestroyForTesting;
#endif
		};

	struct NLS_ENGINE_API DeferredSceneRendererTestAccess final
	{
		using GBufferMaterialCache = std::unordered_map<std::string, DeferredSceneRenderer::GBufferMaterialCacheEntry>;
		using DeferredDecalMaterialCache = std::unordered_map<std::string, DeferredSceneRenderer::DeferredDecalMaterialCacheEntry>;

		static NLS::Render::Resources::Material& GetOrCreateGBufferMaterial(
			DeferredSceneRenderer& renderer,
			NLS::Render::Resources::Material& sourceMaterial);
		static NLS::Render::Resources::Material& ResolveFrameGBufferMaterial(
			DeferredSceneRenderer& renderer,
			NLS::Render::Resources::Material& sourceMaterial);
		static NLS::Render::Resources::Material& ResolveGBufferDrawableMaterialForTesting(
			DeferredSceneRenderer& renderer,
			NLS::Render::Resources::Material& sourceMaterial);
		static GBufferMaterialCache& GetGBufferMaterialCache(DeferredSceneRenderer& renderer);
		static const GBufferMaterialCache& GetGBufferMaterialCache(const DeferredSceneRenderer& renderer);
		static void SetGBufferShader(DeferredSceneRenderer& renderer, NLS::Render::Resources::Shader* shader);
		static NLS::Render::Resources::Shader* GetGBufferShader(const DeferredSceneRenderer& renderer);
		static void ResetFrameGBufferMaterialSyncCount(DeferredSceneRenderer& renderer);
		static uint64_t GetFrameGBufferMaterialSyncCount(const DeferredSceneRenderer& renderer);
		static uint64_t GetThreadedDrawCountEstimateHitCount(const DeferredSceneRenderer& renderer);
		static uint64_t GetThreadedDrawCountEstimateMismatchCount(const DeferredSceneRenderer& renderer);
		static DeferredSceneRenderer::DeferredThreadedDrawCounts EstimateThreadedDeferredDrawCountsForTesting(
			DeferredSceneRenderer& renderer,
			const BaseSceneRenderer::AllDrawables& drawables);
		static void ClearFrameGBufferMaterialResolveCache(DeferredSceneRenderer& renderer);
		static void ResetFrameGBufferMaterialResolveStats(DeferredSceneRenderer& renderer);
		static uint64_t GetFrameGBufferMaterialResolveCacheSize(const DeferredSceneRenderer& renderer);
		static uint64_t GetFrameGBufferMaterialResolveHitCount(const DeferredSceneRenderer& renderer);
		static uint64_t GetFrameGBufferMaterialResolveMissCount(const DeferredSceneRenderer& renderer);
		static NLS::Render::Resources::Material* ResolveDeferredDecalDrawableMaterialForTesting(
			DeferredSceneRenderer& renderer,
			NLS::Render::Resources::Material& sourceMaterial);
		static DeferredDecalMaterialCache& GetDeferredDecalMaterialCache(DeferredSceneRenderer& renderer);
		static const DeferredDecalMaterialCache& GetDeferredDecalMaterialCache(const DeferredSceneRenderer& renderer);
		static void SetDeferredDecalShader(DeferredSceneRenderer& renderer, NLS::Render::Resources::Shader* shader);
		static NLS::Render::Resources::Shader* GetDeferredDecalShader(const DeferredSceneRenderer& renderer);
		static void ClearFrameDeferredDecalMaterialResolveCache(DeferredSceneRenderer& renderer);
		static void ResetFrameDeferredDecalMaterialResolveStats(DeferredSceneRenderer& renderer);
		static uint64_t GetFrameDeferredDecalMaterialResolveCacheSize(const DeferredSceneRenderer& renderer);
		static uint64_t GetFrameDeferredDecalMaterialResolveHitCount(const DeferredSceneRenderer& renderer);
		static uint64_t GetFrameDeferredDecalMaterialResolveMissCount(const DeferredSceneRenderer& renderer);
#if defined(NLS_ENABLE_TEST_HOOKS)
		static size_t GetMaterialResolveCacheMaxEntriesForTesting();
#endif
		static bool HasDeferredThreadedPipelineResourcesForTesting(const DeferredSceneRenderer& renderer);
#if defined(NLS_ENABLE_TEST_HOOKS)
		static void SetDeferredDecalShaderDestroyProbe(
			DeferredSceneRenderer& renderer,
			std::function<void()> probe);
		static void ReleaseDeferredDecalPipelineResourcesForTesting(DeferredSceneRenderer& renderer);
#endif
		static NLS::Render::Resources::MaterialPipelineStateOverrides BuildDeferredDecalMaterialOverridesForTesting(
			const NLS::Render::Resources::Material& sourceMaterial);
		static NLS::Render::Resources::MaterialPipelineStateOverrides BuildGBufferMaterialOverridesForTesting(
			const NLS::Render::Resources::Material& sourceMaterial);
		static NLS::Render::Settings::EComparaisonAlgorithm GetDeferredDecalDepthCompareForTesting();
		static bool ShouldSkipThreadedDeferredFramePublishForTesting(
			const NLS::Render::Context::RenderSceneBuildData& snapshot,
			uint64_t queuedGBufferDrawCount,
			uint64_t queuedDecalDrawCount,
			uint64_t queuedLightingDrawCount,
			uint64_t queuedTransparentDrawCount);
		static void SynchronizeThreadedDeferredSnapshotForTesting(
			NLS::Render::Context::RenderSceneBuildData& snapshot,
			uint64_t queuedGBufferDrawCount,
			uint64_t queuedDecalDrawCount,
			uint64_t queuedLightingDrawCount,
			uint64_t queuedTransparentDrawCount);
		static void EnsureGBufferTargets(DeferredSceneRenderer& renderer, uint16_t width, uint16_t height);
		static bool EnsureHZBTargets(DeferredSceneRenderer& renderer, uint16_t width, uint16_t height);
		static bool PrepareHZBOcclusionPrimitiveBuffers(
			DeferredSceneRenderer& renderer,
			std::span<const SceneOcclusionPrimitivePacket> packets);
		static bool PollHZBOcclusionResultReadback(DeferredSceneRenderer& renderer);
		static bool BeginHZBOcclusionResultReadback(DeferredSceneRenderer& renderer);
		static void BeginHZBOcclusionObservationFrame(
			DeferredSceneRenderer& renderer,
			const SceneOcclusionFrameInput& frame,
			std::span<const SceneOcclusionPrimitiveInput> primitiveInputs);
		static bool HasPendingHZBOcclusionObservationFrame(const DeferredSceneRenderer& renderer);
		static void DiscardPendingHZBOcclusionObservationFrame(DeferredSceneRenderer& renderer);
		static SceneOcclusionObservationStats CompleteHZBOcclusionObservationFrame(
			DeferredSceneRenderer& renderer,
			std::span<const uint32_t> primitiveResultFlags);
		static const SceneOcclusionHistory& GetHZBOcclusionHistory(const DeferredSceneRenderer& renderer);
		static NLS::Render::FrameGraph::HZBFrameResourceRequest BuildHZBFrameResourceRequest(
			const DeferredSceneRenderer& renderer);
		static const NLS::Render::Resources::Texture2D* GetGBufferAlbedoTexture(const DeferredSceneRenderer& renderer);
		static const NLS::Render::Resources::Texture2D* GetGBufferNormalTexture(const DeferredSceneRenderer& renderer);
		static const NLS::Render::Resources::Texture2D* GetGBufferMaterialTexture(const DeferredSceneRenderer& renderer);
		static const NLS::Render::Resources::Texture2D* GetGBufferDepthTexture(const DeferredSceneRenderer& renderer);
	};
}
