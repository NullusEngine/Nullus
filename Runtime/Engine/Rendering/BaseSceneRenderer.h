#pragma once

#include <chrono>

#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <atomic>

#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Entities/Drawable.h>
#include <Rendering/Context/RenderScenePackageBuilder.h>
#include <Rendering/Context/RenderScenePackage.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Math/Quaternion.h>
#include <Vector3.h>
#include "Rendering/LargeSceneSettings.h"
#include "Rendering/LightGridPrepass.h"
#include "Rendering/RenderScene.h"
#include "Rendering/ParticleSceneRenderer.h"
#include "Rendering/SceneOcclusion.h"
#include "Rendering/SceneStreamingResidency.h"
#include "Rendering/DistanceField/DistanceFieldSystem.h"

#include "EngineDef.h"

class FrameGraph;

namespace NLS::Core::ResourceManagement
{
	class ShaderManager;
}

namespace NLS::Render::FrameGraph
{
	struct CompiledThreadedRenderSceneGraphPass;
	struct ThreadedRenderScenePassMetadata;
}

namespace NLS::Render::Resources
{
	class Material;
	class Mesh;
}

namespace NLS::Engine::SceneSystem
{
	class Scene;
}

namespace NLS::Engine::Rendering
{
	class EngineFrameObjectBindingProvider;
	class SceneLightingProvider;

	class NLS_ENGINE_API BaseSceneRenderer : public NLS::Render::Core::CompositeRenderer
	{
	public:
        using Drawable = Render::Entities::Drawable;
        using Frustum = Render::Data::Frustum;
        using PipelineState = Render::Data::PipelineState;
        using Material = Render::Resources::Material;
        using Mesh = Render::Resources::Mesh;
        using Driver = Render::Context::Driver;

		using SceneDrawables = std::vector<std::pair<float, Drawable>>;
		using OpaqueDrawables = SceneDrawables;
		using DecalDrawables = SceneDrawables;
		using TransparentDrawables = SceneDrawables;
		using SkyboxDrawables = SceneDrawables;

		struct AllDrawables
		{
			OpaqueDrawables opaques;
			DecalDrawables decals;
			TransparentDrawables transparents;
			SkyboxDrawables skyboxes;
                std::vector<ParticleViewInput> particles;
		};

		struct SceneDescriptor
		{
			SceneSystem::Scene& scene;
			std::optional<Frustum> frustumOverride;
			Material* overrideMaterial;
				std::vector<SceneSystem::Scene*> additiveScenes;
				bool includeSkyboxes = true;
					bool requireExplicitMaterialTextures = false;
					bool allowDefaultMaterialForUnresolvedExplicitMaterials = false;
					bool skipSceneDrawables = false;
					bool suppressVisibleMaterialTextureRequests = false;
					bool suppressHZBOcclusion = false;
					bool suppressLightGridCompute = false;
					bool trustSceneRenderContentRevision = false;
				};


		explicit BaseSceneRenderer(Driver& p_driver);
		~BaseSceneRenderer() override;

		static bool PreloadSceneFallbackShader(
			NLS::Core::ResourceManagement::ShaderManager& shaderManager,
			bool logWarningOnFailure = true);

		void BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void BeginFrameForBackgroundPreview(const Render::Data::FrameDescriptor& p_frameDescriptor) override;

		struct BackgroundPreviewDrawPrewarmResult
		{
			size_t nextDrawIndex = 0u;
			size_t totalDrawCount = 0u;
			bool supported = false;
			bool complete = false;
		};
		virtual BackgroundPreviewDrawPrewarmResult PrewarmBackgroundPreviewDraws(
			const Render::Data::FrameDescriptor& frameDescriptor,
			size_t firstDrawIndex,
			size_t maxDraws,
			std::chrono::microseconds timeBudget = std::chrono::milliseconds(1));

		virtual void DrawModelWithSingleMaterial(
			PipelineState p_pso,
			Mesh& p_mesh,
			Material& p_material,
			const Maths::Matrix4& p_modelMatrix
		);

		SceneLightingProvider& GetSceneLightingProvider();
		const SceneLightingProvider& GetSceneLightingProvider() const;
		void SetLargeSceneSettings(const LargeSceneSettings& settings);
		[[nodiscard]] const LargeSceneSettings& GetLargeSceneSettings() const;
			const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetLightGridGraphicsPassBindingSet() const;
        void SetDistanceFieldDebugSettings(Render::DistanceField::DistanceFieldDebugSettings settings)
        { std::atomic_store(&m_distanceFieldDebug, std::make_shared<const Render::DistanceField::DistanceFieldDebugSettings>(settings)); }
        Render::DistanceField::DistanceFieldDebugSettings GetDistanceFieldDebugSettings() const
        { return *std::atomic_load(&m_distanceFieldDebug); }
        Render::DistanceField::DistanceFieldStats GetDistanceFieldStats() const
        { auto system = std::atomic_load(&m_distanceFieldSystem); return system ? system->GetStats() : Render::DistanceField::DistanceFieldStats{}; }
			NLS::Render::FrameGraph::LightGridCompileContext BuildLightGridCompileContext(
				bool hasSkyboxTexture = false) const;
			NLS::Render::FrameGraph::LightGridCompileContext BuildLightGridCompileContext(
				bool hasSkyboxTexture,
				bool suppressCompute) const;
			/**
			 * Thread-safe overload for work that outlives the game-thread frame.
			 *
			 * The live descriptor is rewritten by the game thread every BeginFrame
			 * and its camera object is mutated by the game thread, so render-thread
			 * work has to key off a frozen descriptor instead. The returned context
			 * always points at the frozen camera, so a cache hit can never hand back
			 * a camera the current frame does not own.
			 */
			NLS::Render::FrameGraph::LightGridCompileContext BuildLightGridCompileContext(
				const NLS::Render::FrameGraph::FrozenFrameDescriptor& frozenFrame,
				bool hasSkyboxTexture,
				bool suppressCompute = false) const;
			/**
			 * Light-grid inputs captured on the game thread.
			 *
			 * The lighting descriptor and the descriptor map belong to the game
			 * thread, which replaces both every BeginFrame. A context built later on
			 * the render thread therefore has to be handed captured values, not read
			 * them back out of the renderer.
			 */
			struct LightGridFrameCapture
			{
				bool hasSkyboxTexture = false;
				std::optional<LightGridPrepass::PreparedFrameInputs> frameInputs;
			};
			LightGridFrameCapture CaptureLightGridFrameInputs(
				bool hasSkyboxTexture = false) const;
			/**
			 * Render-thread overload: no descriptor or live-descriptor reads.
			 */
			NLS::Render::FrameGraph::LightGridCompileContext BuildLightGridCompileContext(
				const NLS::Render::FrameGraph::FrozenFrameDescriptor& frozenFrame,
				const LightGridFrameCapture& capture,
				bool suppressCompute = false) const;
		bool HasPendingLightGridFrameInputs() const { return false; }
		/**
		 * Published results of the last scene parse.
		 *
		 * ParseScene is the expensive half of the frame and is being made safe to
		 * run off the game thread, so its results cannot be plain members that a
		 * game-thread reader walks while a parse rewrites them. Each accessor
		 * returns the current immutable publication; holding it keeps that parse's
		 * data alive and unchanged even after the next parse publishes.
		 */
		[[nodiscard]] std::shared_ptr<const SceneOcclusionPrimitivePacketBuildResult>
			GetLastHZBOcclusionPrimitivePacketBuildResult() const;
		const SceneOcclusionHistory& GetHZBOcclusionHistoryForTesting() const;
		[[nodiscard]] bool HasLastVisiblePickablePrimitiveDrawSources() const;
		[[nodiscard]] std::shared_ptr<const std::vector<ScenePickablePrimitiveDrawSource>>
			GetLastVisiblePickablePrimitiveDrawSources() const;
		[[nodiscard]] bool HasCompletedVisibleMaterialTexturePumpForReadback() const;
		/**
		 * Scene-scoped render content revision used as a view frame-cache key input.
		 * It advances only when this renderer could not reuse its own scene-content
		 * synchronization fast path, so unrelated resource churn elsewhere in the
		 * process does not invalidate the cached frame.
		 */
		[[nodiscard]] uint64_t GetSceneRenderContentRevision() const;
				const Render::Data::DrawCallOptimizationStats& GetLastDrawCallOptimizationStats() const;
#if defined(NLS_ENABLE_TEST_HOOKS)
				const Render::Data::DrawCallOptimizationStats& GetLastDrawCallOptimizationStatsForTesting() const;
#endif

	protected:
		void RefreshSceneLightingDescriptor(SceneSystem::Scene& scene);
		void SetSceneLightingDescriptor();
		/**
		 * Sizes the render scenes' outstanding-synchronization depth for this mode.
		 *
		 * In threaded rendering the game thread captures while the render thread
		 * resolves, so the game thread legitimately runs ahead. At depth one the
		 * next frame's capture would be refused and that frame silently dropped, so
		 * the depth follows how many frames the pipeline can have in flight. The
		 * inline path keeps depth one, where capture and resolve still alternate.
		 */
		void ConfigureRenderSceneSynchronizationDepth();
		/**
		 * Builds this frame's drawables.
		 *
		 * Takes a frozen descriptor rather than reading the live m_frameDescriptor:
		 * the parse is the expensive part of the frame and must be independent of
		 * the game thread rewriting frame inputs for a later frame.
		 */
		AllDrawables ParseScene(
			const NLS::Render::FrameGraph::FrozenFrameDescriptor& frame);

		/**
		 * Game-thread half of a scene parse.
		 *
		 * Captures which scenes this frame renders and collects each one's proxy
		 * update batch. This is the only half that dereferences SceneSystem::Scene,
		 * because the scene graph belongs to the game thread. Scene identities are
		 * kept as pointers for comparison only; the resolve half never dereferences
		 * them.
		 */
		struct SceneSynchronizeCapture
		{
			struct SynchronizationResult
			{
				RenderSceneSyncStats stats;
				bool completed = false;
			};
			// Identity of the source scene. Compared by the resolve half, never
			// dereferenced, so a stale scene cannot be read from another thread.
			SceneSystem::Scene* sceneIdentity = nullptr;
			RenderScene* renderScene = nullptr;
			std::shared_ptr<RenderScene> renderSceneOwner;
			std::shared_ptr<SynchronizationResult> synchronizationResult;
			RenderSceneSyncRequest syncRequest;
			bool includeSkyboxes = true;
			bool requireExplicitMaterialTextures = false;
			bool allowDefaultMaterialForUnresolvedExplicitMaterials = false;
			bool trustSceneRenderContentRevision = false;
			bool isMainScene = false;
		};

		struct SceneParseInput
		{
			std::vector<SceneSynchronizeCapture> scenes;
                std::vector<ParticleViewInput> particles;
			// The additive scenes still referenced by the descriptor this frame.
			std::vector<SceneSystem::Scene*> activeAdditiveSceneIdentities;
			// Owned so the resolve half can point RenderSceneSyncOptions at stable
			// storage instead of at a local that is gone by then.
			LargeSceneSettings occlusionSettings;
			/**
			 * Frozen answers of the editor hooks the visibility gather needs.
			 *
			 * The resolve half runs on the render thread. The editor overrides of
			 * GetEditorInspectionRoots read the renderer's descriptor map, which the
			 * game thread rebuilds every frame (AddDescriptor in the panel's frame
			 * init, ClearDescriptors in CompositeRenderer::EndFrame). Reading that map
			 * from the render thread races the game thread's rehash and its node
			 * frees, so the values are captured here instead.
			 */
			std::vector<NLS::InstanceID> inspectionRootInstanceIds;
			bool publishCullReasonDebugSnapshots = false;
			uint64_t cullReasonDebugSnapshotMaxEntries = 0u;
			NLS::Render::Resources::Material* defaultMaterial = nullptr;
			NLS::Render::Resources::Material* overrideMaterial = nullptr;
			std::optional<Frustum> frustumOverride;
			bool skipSceneDrawables = false;
			bool suppressVisibleMaterialTextureRequests = false;
			/**
			 * True when at least one scene could not claim a synchronization slot
			 * because the previous capture is still outstanding.
			 *
			 * The capture half is deliberately refused rather than queued, so the
			 * rest of this input describes no scene at all: resolving it would
			 * produce an empty draw list. Consumers must drop the frame instead of
			 * rendering or publishing the void.
			 */
			bool captureRefused = false;
		};

		SceneParseInput BeginSceneParse(
			const NLS::Render::FrameGraph::FrozenFrameDescriptor& frame);
		AllDrawables ResolveSceneParse(
			SceneParseInput& input,
			const NLS::Render::FrameGraph::FrozenFrameDescriptor& frame);
            void AppendParticleScenePass(Render::Context::RenderScenePackage& package,
                const Render::FrameGraph::FrozenFrameDescriptor& frame, const std::vector<ParticleViewInput>& particles);
        void AppendDistanceFieldScenePass(Render::Context::RenderScenePackage& package,
            const Render::FrameGraph::FrozenFrameDescriptor& frame,
            const Render::DistanceField::DistanceFieldDebugSettings& settings, uint64_t frameId);
		/**
		 * Releases every capture in `input` that is still claimed.
		 *
		 * The capture half claims a slot per scene; the resolve half releases it. Any
		 * path that drops the input without resolving - a frame that never produced a
		 * package, or a prepared builder that was discarded before it ran - would
		 * otherwise leave those claims held forever, and once the outstanding limit is
		 * reached every later capture is refused and the scene stops rendering.
		 *
		 * Only claims that are still outstanding are released, so a partially
		 * resolved input is safe to pass here and nothing is applied twice.
		 */
		void ReleaseUnresolvedSceneParseCapture(SceneParseInput& input);

		/**
		 * Drops-without-resolving guard for a SceneParseInput.
		 *
		 * Held alongside the input and shared between copies, so the claim is released
		 * exactly once whether the input is resolved normally, dropped because the
		 * frame could not be published, or destroyed with a prepared builder that was
		 * never invoked.
		 */
		/**
		 * Liveness token for this renderer's scene-parse capture claims.
		 *
		 * A prepared builder can outlive its renderer: the driver retains un-run
		 * builders in its frame slots and destroys them from
		 * Driver::ShutdownRhiResources, which runs after the renderer is already
		 * gone. A dropper that called back through a raw renderer pointer at that
		 * point would lock a RenderScene mutex inside freed memory and hang the
		 * process forever. The renderer owns this token and invalidates it before
		 * its members are destroyed; the dropper holds a weak reference and skips
		 * the callback once the token is gone.
		 *
		 * Skipping is safe: the claims live in RenderScene objects owned by the
		 * destroyed renderer, so they were destroyed with it.
		 */
		struct SceneParseCaptureLifetime
		{
		};

		struct SceneParseCaptureDropper
		{
			void MarkResolved() noexcept
			{
				if (state != nullptr)
					state->resolved = true;
			}

			struct State
			{
				std::weak_ptr<SceneParseCaptureLifetime> ownerLifetime;
				BaseSceneRenderer* owner = nullptr;
				std::shared_ptr<SceneParseInput> input;
				bool resolved = false;

				~State()
				{
					if (resolved || owner == nullptr || input == nullptr)
						return;

					// The renderer is gone: its RenderScene - and every claim it
					// held - was destroyed with it, and `owner` is a dangling
					// pointer that must not be dereferenced.
					if (ownerLifetime.expired())
						return;

					owner->ReleaseUnresolvedSceneParseCapture(*input);
				}
			};

			std::shared_ptr<State> state;
		};

		/** Builds a dropper that cannot outlive this renderer's capture state. */
		SceneParseCaptureDropper MakeSceneParseCaptureDropper(
			std::shared_ptr<SceneParseInput> input);
        static void RefreshSceneBuildVisibility(
            NLS::Render::Context::RenderSceneBuildData& snapshot,
			const AllDrawables& drawables);
        void RefreshFrameBuildMetadata(NLS::Render::Context::RenderSceneBuildData& scene) const;
		virtual bool ShouldPublishCullReasonDebugSnapshots() const;
		virtual uint64_t GetCullReasonDebugSnapshotMaxEntries() const;
		virtual std::vector<NLS::InstanceID> GetEditorInspectionRoots() const;
		bool CaptureThreadedPreparedDraw(
			PipelineState pso,
			const Drawable& drawable,
			PreparedRecordedDraw& outDraw);
		bool CaptureThreadedPreparedDraw(
			const Drawable& drawable,
			Render::Resources::MaterialPipelineStateOverrides pipelineOverrides,
			Render::Settings::EComparaisonAlgorithm depthCompareOverride,
			std::string_view lightMode,
			PreparedRecordedDraw& outDraw);
		bool CaptureThreadedPreparedDraw(
			const Drawable& drawable,
			Material& effectiveMaterial,
			Render::Resources::MaterialPipelineStateOverrides pipelineOverrides,
			Render::Settings::EComparaisonAlgorithm depthCompareOverride,
			std::string_view lightMode,
			PreparedRecordedDraw& outDraw);
		/**
		 * Per-phase cost of one threaded draw capture, filled only when a caller asks
		 * for it. Kept as an out-parameter so the capture path pays nothing when the
		 * diagnostics switch is off.
		 */
		struct CaptureDrawPhaseTimings
		{
			uint64_t prepareDrawNs = 0u;
			uint64_t prepareRecordedDrawNs = 0u;
			uint64_t captureBindingSetsNs = 0u;
		};
		bool CaptureThreadedPreparedDraw(
			const Drawable& drawable,
			Material& effectiveMaterial,
			Render::Resources::MaterialPipelineStateOverrides pipelineOverrides,
			Render::Settings::EComparaisonAlgorithm depthCompareOverride,
			std::string_view lightMode,
			PreparedRecordedDraw& outDraw,
			CaptureDrawPhaseTimings* outTimings);
		// Small POD: returned by value so the caller owns a consistent copy.
		[[nodiscard]] SceneOcclusionFrameInput GetLastHZBOcclusionFrameInput() const;
		bool HasPendingHZBOcclusionObservationFrame() const;
		void DiscardPendingHZBOcclusionObservationFrame();
		void BeginHZBOcclusionObservationFrame(
			const SceneOcclusionFrameInput& frame,
			std::span<const SceneOcclusionPrimitiveInput> primitiveInputs);
		SceneOcclusionObservationStats CompleteHZBOcclusionObservationFrame(
			std::span<const uint32_t> primitiveResultFlags);
		std::optional<LightGridPrepass::PreparedFrameInputs> BuildLightGridFrameInputs(
			bool hasSkyboxTexture = false) const;
			const std::shared_ptr<LightGridPrepass>& GetLightGridPrepass() const;

			/**
			 * Cheap, allocation-free gate for "does this frame have scene work at all".
			 * Used before the per-frame scene pipeline so a frame that cannot publish
			 * does not pay for parse, visibility, sorting and index assignment.
			 * Additive scenes are deliberately excluded: ignoring them can only make
			 * the probe more conservative.
			 */
			[[nodiscard]] size_t GetRenderScenePrimitiveCount() const;

		public:
		static const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& GetPreparedPassBindingSetPlaceholder();
		static void ResolvePreparedPassBindingSetPlaceholders(
			NLS::Render::Context::RenderScenePackage& package,
			const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet);
		static void ResolvePreparedScenePassBindingSetPlaceholders(
			NLS::Render::Context::RenderScenePackage& package,
			const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& resolvedBindingSet,
			uint64_t sceneDrawCount);

	private:
		struct LightGridCompileContextCache
		{
			bool valid = false;
			bool hasSkyboxTexture = false;
			/**
			 * True when the key came from the view's live frame descriptor, whose
			 * camera object is stable for the view's lifetime. A frozen frame owns
			 * a camera per frame, so that path cannot compare camera addresses.
			 */
			bool keyedByLiveCamera = false;
			// `camera` is compared but never dereferenced: a frozen key leaves this
			// pointing at a camera the frame has already released.
			NLS::Render::Data::FrameDescriptor frameDescriptor{};
			Maths::Vector3 cameraPosition{};
			Maths::Quaternion cameraRotation{};
			NLS::Render::FrameGraph::LightGridCompileContext context;
		};

		void InvalidateLightGridCompileContextCache() const;
		void SetLastCullReasonDebugSnapshot(
			const std::shared_ptr<const SceneCullReasonDebugSnapshot>& snapshot) const;
		Material* ResolveDefaultSceneMaterial();
		NLS::Render::FrameGraph::LightGridCompileContext BuildLightGridCompileContextFromFrame(
			const Render::Data::FrameDescriptor& frameSnapshot,
			const LightGridFrameCapture& capture,
			bool suppressCompute,
			bool keyedByLiveCamera) const;
		bool IsLightGridCompileContextCacheHit(
			const NLS::Render::Data::FrameDescriptor& frameDescriptor,
			bool hasSkyboxTexture,
			bool keyedByLiveCamera) const;
		bool AreSameLightGridFrameInputs(
			const LightGridCompileContextCache& cached,
			const NLS::Render::Data::FrameDescriptor& current,
			bool requireSameCameraAddress) const;

		std::shared_ptr<LightGridPrepass> m_lightGridPrepass;
		// Owns the liveness token a scene-parse dropper weakly observes. Declared
		// before the scene state so it is the last of the two to be destroyed and
		// stays invalidated until every claim-holding member is gone.
		std::shared_ptr<SceneParseCaptureLifetime> m_sceneParseCaptureLifetime =
			std::make_shared<SceneParseCaptureLifetime>();
		std::unique_ptr<SceneLightingProvider> m_sceneLightingProvider;
		std::unique_ptr<SceneLightingProvider> m_renderSceneLightingProvider;
			uint64_t m_renderSceneGameObjectCount = 0u;
		std::unique_ptr<Material> m_sceneFallbackMaterial;
		NLS::Render::Resources::Shader* m_sceneFallbackShader = nullptr;
		uint64_t m_sceneFallbackShaderInstanceId = 0u;
		uint64_t m_sceneFallbackShaderGeneration = 0u;
		std::string m_sceneFallbackShaderResourcePath;
		RenderScene m_renderScene;
            ParticleSceneRenderer m_particleRenderer;
        // Publish immutable snapshots through the portable shared_ptr atomic operations.
        std::shared_ptr<Render::DistanceField::DistanceFieldSystem> m_distanceFieldSystem;
        std::shared_ptr<const Render::DistanceField::DistanceFieldDebugSettings> m_distanceFieldDebug{
            std::make_shared<const Render::DistanceField::DistanceFieldDebugSettings>()};
        std::shared_ptr<const uint64_t> m_distanceFieldViewLifetime =
            Render::DistanceField::DistanceFieldSystem::CreateViewLifetime();
		std::mutex m_additiveRenderScenesMutex;
		std::unordered_map<SceneSystem::Scene*, std::shared_ptr<RenderScene>> m_additiveRenderScenes;
		std::vector<ScenePickablePrimitiveDrawSource> m_lastVisiblePickablePrimitiveDrawSources;
		// Read by the game thread for view-cache invalidation while the parse may
		// write it from the render thread. It is a monotonic counter, so an atomic
		// (rather than a locked publish) preserves the semantics: a stale read only
		// means a cache invalidates one frame later, never a wrong value.
		std::atomic<uint64_t> m_sceneRenderContentRevision { 0u };
		// Stays valid until the next successful scene parse so editor picking
		// keeps using the visible-source path on frames that reuse a cached frame.
		bool m_hasLastVisiblePickablePrimitiveDrawSources = false;
		// Published parse results. The accumulators above are written only by
		// ParseScene; at the end of a parse they are moved into these immutable
		// snapshots, which readers on any thread acquire under this mutex.
		mutable std::mutex m_lastParseResultMutex;
		std::shared_ptr<const std::vector<ScenePickablePrimitiveDrawSource>>
			m_publishedVisiblePickablePrimitiveDrawSources;
		std::shared_ptr<const std::vector<uint64_t>> m_publishedStreamingDependencyPins;
		std::shared_ptr<const NLS::Render::Context::LargeSceneCullReasonDebugSnapshot>
			m_publishedCullReasonDebugSnapshot;
		std::shared_ptr<const SceneOcclusionPrimitivePacketBuildResult>
			m_publishedHZBOcclusionPrimitivePacketBuildResult;
		SceneOcclusionFrameInput m_publishedHZBOcclusionFrameInput;
		bool m_publishedHasVisiblePickablePrimitiveDrawSources = false;
			mutable std::mutex m_lightGridCompileContextCacheMutex;
			mutable LightGridCompileContextCache m_lightGridCompileContextCache;
			mutable NLS::Render::Context::LargeSceneCullReasonDebugSnapshot m_lastCullReasonDebugSnapshot;
		SceneOcclusionPrimitivePacketBuildResult m_lastHZBOcclusionPrimitivePacketBuildResult;
		SceneOcclusionFrameInput m_lastHZBOcclusionFrameInput;
		SceneOcclusionHistory m_hzbOcclusionHistory;
		SceneOcclusionObservationBatch m_hzbPendingOcclusionObservationBatch;
			uint64_t m_hzbOcclusionFrameSerial = 0u;
			SceneStreamingResidency m_streamingResidency;
			LargeSceneSettings m_largeSceneSettings;
			RepresentationResidencySnapshot m_lastRepresentationResidency;
			std::vector<uint64_t> m_lastStreamingDependencyPins;
			uint64_t m_streamingResidencyFrameSerial = 0u;
	};
}
