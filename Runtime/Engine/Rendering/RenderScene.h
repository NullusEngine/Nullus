#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Math/Matrix4.h>
#include <Rendering/Data/DrawCallOptimizationStats.h>
#include <Rendering/Data/DrawableObjectDescriptor.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Data/FrameInfo.h>
#include <Rendering/Data/StateMask.h>
#include <Rendering/Entities/Drawable.h>
#include <Rendering/Geometry/Bounds.h>
#include <Rendering/Geometry/BoundingSphere.h>

#include "Components/MeshRenderer.h"
#include "EngineDef.h"
#include "Object/Object.h"
#include "Rendering/SceneProxy.h"
#include "Rendering/SceneRenderStore.h"

namespace NLS::Engine
{
	class GameObject;
}

namespace NLS::Engine::Components
{
	class MeshFilter;
	class MeshRenderer;
}

namespace NLS::Engine::SceneSystem
{
	class Scene;
}

namespace NLS::Render::Resources
{
	class Material;
	class Mesh;
}

namespace NLS::Engine::Rendering
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS_ENGINE_API bool HasResolvedDeclaredMaterialTexturesForTesting(
        const NLS::Render::Resources::Material& material);
#endif

	struct LargeSceneSettings;
	struct HLODClusterRecord;
	struct LODGroupRecord;
	struct RenderSceneDeclaredTextureLookupCache;
	struct RepresentationResidencySnapshot;
	struct SceneCullReasonDebugSnapshot;
	struct SceneOcclusionState;
	class SceneSpatialIndex;
	enum class SceneVisibilityPipelineMode : uint8_t;

	struct ScenePrimitiveHandle
	{
		uint64_t sceneId = 0u;
		uint32_t index = ~0u;
		uint32_t generation = 0u;

		[[nodiscard]] bool IsValid() const
		{
			return sceneId != 0u && index != ~0u && generation != 0u;
		}

		[[nodiscard]] bool operator==(const ScenePrimitiveHandle& other) const = default;
	};

	struct ScenePrimitiveVisibilitySettings
	{
		uint32_t layer = 0u;
		bool distanceCullingEnabled = false;
		float minDrawDistance = 0.0f;
		float maxDrawDistance = 0.0f;
	};

	struct ScenePrimitiveCommandOffsetRange
	{
		ScenePrimitiveHandle handle;
		uint64_t commandOffsetBegin = 0u;
		uint64_t commandOffsetEnd = 0u;
	};

	struct SceneLODGroupHandle
	{
		uint32_t index = ~0u;

		[[nodiscard]] bool IsValid() const { return index != ~0u; }
		[[nodiscard]] bool operator==(const SceneLODGroupHandle& other) const = default;
	};

	struct SceneHLODClusterHandle
	{
		uint32_t index = ~0u;

		[[nodiscard]] bool IsValid() const { return index != ~0u; }
		[[nodiscard]] bool operator==(const SceneHLODClusterHandle& other) const = default;
	};

	struct ScenePrimitiveSnapshotRecord
	{
		ScenePrimitiveHandle handle;
		NLS::Render::Resources::Mesh* mesh = nullptr;
		NLS::Render::Geometry::BoundingSphere modelBoundingSphere;
		NLS::Render::Geometry::Bounds modelBounds;
        float maximumWorldDisplacement = 0;
		Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
		Maths::Matrix4 userMatrix = Maths::Matrix4::Identity;
        bool affectDistanceField = true;
        bool distanceFieldMostlyStatic = true;
		Components::MeshRenderer::EFrustumBehaviour frustumBehaviour =
			Components::MeshRenderer::EFrustumBehaviour::DISABLED;
		ScenePrimitiveVisibilitySettings visibilitySettings;
		std::optional<SceneLODGroupHandle> lodGroup;
		std::optional<SceneHLODClusterHandle> hlodCluster;
		uint64_t commandOffsetBegin = 0u;
		uint64_t commandOffsetEnd = 0u;
		bool hasMeshBinding = false;
		bool hasValidMaterial = false;
		bool depthWriteEligibleForOcclusion = false;
		bool ownerAlive = false;
		bool ownerActive = false;
		bool occupied = false;
		bool tombstoned = false;
	};

	struct ScenePrimitiveDenseHandleMapping
	{
		ScenePrimitiveHandle handle;
		uint64_t denseIndex = 0u;
	};

	struct ScenePrimitiveSnapshot
	{
		uint64_t snapshotSerial = 0u;
		uint64_t sceneId = 0u;
		uint64_t frameSerial = 0u;
		std::vector<ScenePrimitiveSnapshotRecord> primitiveRecords;
		std::vector<ScenePrimitiveDenseHandleMapping> handleToDenseIndex;
		std::vector<ScenePrimitiveHandle> denseIndexToHandle;
		std::vector<ScenePrimitiveHandle> dirtySyncHandles;
		std::vector<ScenePrimitiveHandle> removedHandles;
		std::vector<uint64_t> liveHandleBits;
		std::vector<ScenePrimitiveCommandOffsetRange> commandOffsetTable;
		std::vector<ScenePrimitiveHandle> visiblePrimitiveScratch;
		uint64_t memoryArenaSerial = 0u;
	};

		struct RenderSceneSyncOptions
		{
			NLS::Render::Resources::Material* defaultMaterial = nullptr;
			NLS::Render::Resources::Material* overrideMaterial = nullptr;
			bool requireExplicitMaterialTextures = false;
			bool allowDefaultMaterialForUnresolvedExplicitMaterials = false;
			const LargeSceneSettings* largeSceneSettings = nullptr;
			bool trustSceneRenderContentRevision = false;
		};

	struct RenderSceneSyncStats
	{
		uint64_t addedPrimitiveCount = 0u;
		uint64_t reusedPrimitiveCount = 0u;
		uint64_t removedPrimitiveCount = 0u;
		uint64_t rebuiltCachedCommandCount = 0u;
		uint64_t rebuiltOpaqueSortTokenCount = 0u;
		uint64_t syncTouchedPrimitiveCount = 0u;
		uint64_t syncFullSweepCount = 0u;
		uint64_t syncSweepTouchedSlotCount = 0u;
		uint64_t boundsDirtyPrimitiveCount = 0u;
		uint64_t primitiveSlotReuseCount = 0u;
		uint64_t declaredTextureLookupCount = 0u;
		uint64_t declaredTextureCacheHitCount = 0u;
		uint64_t declaredTextureCacheMissCount = 0u;
		uint64_t declaredTextureResourceScanCount = 0u;
		bool usedSceneRenderContentRevisionFastPath = false;
		// True only when this synchronization observed work that can change the
		// rendered image: structure edits, bound/cached command rebuilds, dirty
		// bounds, tracked-material mutations or mesh resource mutations. A sync
		// that merely ran without the revision fast path (for example because the
		// caller does not opt in) must leave this false so view frame caches can
		// still hit across otherwise identical frames.
		bool detectedRenderContentChange = false;
		uint64_t syncTimeNs = 0u;
	};

	/**
	 * Result of the scene-owning (game) thread's half of a synchronization.
	 *
	 * It carries only values: the scene pointer, the revisions that were observed
	 * and, when the fast path did not apply, the collected proxy batch. No
	 * SceneSystem::Scene reference is dereferenced by the consuming half, which is
	 * what lets the apply-and-rebuild step run on the render thread.
	 *
	 * Hand-off contract: BeginSynchronize and CompleteSynchronize must not run
	 * concurrently for the same RenderScene. They alternate once per frame, because
	 * the resolve mutates state the capture reads (the proxy store, the primitive
	 * index, the pending declared-texture set).
	 *
	 * The contract is enforced rather than assumed: a second capture that starts
	 * while one is outstanding is refused with a diagnostic instead of silently
	 * racing. Only one capture may be outstanding at a time.
	 *
	 * A capture must always be resolved, one way or another. Collection advances
	 * the shared SceneProxySourceRegistry cursor, so simply discarding a captured
	 * request would lose those updates rather than defer them, and re-collecting
	 * is not equivalent: Reset() clears the registry's tracking maps, so the next
	 * scan reallocates every handle and churns the whole store.
	 *
	 * A dropped frame therefore calls ApplyDroppedSynchronizeCapture, which
	 * applies the captured batch and releases the claim.
	 */
	struct RenderSceneSyncRequest
	{
		// Identity of the scene the facts were read from. The scene must stay alive
		// until CompleteSynchronize has consumed this request; the pointer is only
		// compared and stored, never dereferenced.
		const SceneSystem::Scene* sceneIdentity = nullptr;
		uint64_t sceneInstanceId = 0u;
			// Captured with the proxy update, including objects with no render proxy.
			uint64_t sceneGameObjectCount = 0u;
		uint64_t sceneRenderContentRevision = 0u;
		uint64_t meshMutationEpoch = 0u;
		uint64_t frameSerial = 0u;
		// Identifies the capture this request came from, so the resolve can mark
		// exactly that capture complete.
		uint64_t captureSerial = 0u;
		std::shared_ptr<SceneRenderStore> proxyStore;
		SceneProxyUpdateBatch proxyBatch;
		bool sceneIdentityChanged = false;
		bool synchronizationOptionsChanged = false;
		bool sourceNeedsCollection = false;
		bool canReuseProxySynchronization = false;
		// Captured so the consuming half does not have to re-derive mutation
		// observations from state that may have advanced since.
		bool meshMutationObserved = false;
		bool trackedMaterialMutationObserved = false;

		[[nodiscard]] bool IsValid() const noexcept { return sceneIdentity != nullptr; }
	};

	struct RenderSceneVisibilityOptions
	{
		const NLS::Render::Data::Frustum* frustum = nullptr;
		Maths::Vector3 cameraPosition{};
		uint32_t visibleLayerMask = 0xFFFF'FFFFu;
		const LargeSceneSettings* largeSceneSettings = nullptr;
		float lodBias = 1.0f;
		float verticalFovRadians = 0.0f;
		uint64_t lodHistoryViewKey = 0u;
		bool allowHLOD = true;
		bool editorInspectionView = false;
		std::vector<ScenePrimitiveHandle> selectedPrimitiveHandles;
		std::vector<NLS::InstanceID> inspectionRootInstanceIds;
		bool enableCullReasonDebugSnapshot = false;
		uint64_t maxCullReasonDebugSnapshotEntries = 0u;
		const SceneOcclusionState* occlusion = nullptr;
		const RepresentationResidencySnapshot* representationResidency = nullptr;
	};

		struct RenderSceneVisibleQueues
	{
		using SceneDrawables = std::vector<std::pair<float, NLS::Render::Entities::Drawable>>;

		SceneDrawables opaques;
		SceneDrawables decals;
		SceneDrawables transparents;
		SceneDrawables skyboxes;
	};

	using DrawCallOptimizationStats = NLS::Render::Data::DrawCallOptimizationStats;

	enum class RenderSceneVisibilityMode
	{
		Auto,
		Serial,
		Parallel
	};

	struct RenderSceneVisibilitySnapshot
	{
		std::vector<uint64_t> primitiveBits;
		std::vector<uint64_t> meshBits;
		std::vector<size_t> visiblePrimitiveIndices;
		std::vector<ScenePrimitiveHandle> visiblePrimitiveHandles;
		std::vector<ScenePrimitiveHandle> representationStreamingInterest;
		uint64_t primitiveCount = 0u;
		uint64_t meshCount = 0u;
		uint64_t visiblePrimitiveCount = 0u;
		uint64_t visibleMeshCount = 0u;
		uint64_t occlusionTestCount = 0u;
		uint64_t occlusionCulledCount = 0u;
		std::array<uint64_t, NLS::Render::Data::kLargeSceneCullReasonCount> culledByReason {};
		std::array<uint64_t, NLS::Render::Data::kLargeSceneLodSelectionBucketCount> lodSelectionCount {};
		std::vector<uint32_t> selectedLODByPrimitive;
		uint64_t activeHLODClusterCount = 0u;
		uint64_t spatialCandidateCount = 0u;
		uint64_t fullScanCandidateCount = 0u;
		uint64_t primitiveRecordsTouched = 0u;
		uint64_t visibilityTestedPrimitiveCount = 0u;
		uint64_t dynamicCandidateCount = 0u;
		uint64_t dynamicRecordsTouched = 0u;
		uint64_t staticIndexRefitCount = 0u;
		uint64_t staticIndexRebuildCount = 0u;
		uint64_t staticIndexLastGoodQueryCount = 0u;
		uint64_t staticIndexDirtyOverlayCount = 0u;
		uint64_t spatialRebuildFallbackCount = 0u;
		uint64_t dynamicIndexUpdateCount = 0u;
		bool usesSparseVisiblePrimitiveIndices = false;
		bool usedParallelEvaluation = false;
	};

	struct RenderCachedDrawCommand
	{
		NLS::Render::Resources::Mesh* mesh = nullptr;
		NLS::Render::Resources::Material* material = nullptr;
		NLS::Render::Data::StateMask stateMask{};
		NLS::Render::Settings::EPrimitiveMode primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;
		uint64_t buildSerial = 0u;
		uint64_t opaqueSortToken = (std::numeric_limits<uint64_t>::max)();
	};

	struct ScenePickablePrimitiveDrawSource
	{
		NLS::InstanceID ownerInstanceId = NLS::InstanceID_None;
		NLS::Render::Resources::Mesh* mesh = nullptr;
		Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
		NLS::Render::Data::StateMask stateMask{};
        NLS::Render::Resources::Material* material=nullptr;
	};

		class SceneProxySourceRegistry;

	class NLS_ENGINE_API RenderScene
	{
	public:
		RenderScene();
		RenderScene(const RenderScene&) = delete;
		RenderScene& operator=(const RenderScene&) = delete;
		RenderScene(RenderScene&& other) noexcept;
		RenderScene& operator=(RenderScene&& other) noexcept;
		~RenderScene();

		/**
		 * Game-thread half: captures the scene facts a synchronization needs and,
		 * when the revision fast path does not apply, walks the scene to collect
		 * the proxy update batch.
		 *
		 * Safe to call on the thread that owns SceneSystem::Scene. The result is
		 * consumed by CompleteSynchronize on the RenderScene-owning thread.
		 */
		[[nodiscard]] RenderSceneSyncRequest BeginSynchronize(
			SceneSystem::Scene& scene,
			const RenderSceneSyncOptions& options = {});

		/**
		 * Render-thread half: applies the collected batch, rebuilds primitives,
		 * spatial index and HLOD records, and publishes the sync telemetry.
		 *
		 * `options` must match the options passed to BeginSynchronize, because the
		 * synchronization stamp is derived from them.
		 */
		RenderSceneSyncStats CompleteSynchronize(
			RenderSceneSyncRequest request,
			const RenderSceneSyncOptions& options = {});

		/**
		 * Resolves a capture whose frame was dropped before it could be rendered.
		 *
		 * Collection already advanced the SceneProxySourceRegistry cursor, so the
		 * updates in this request exist nowhere else and must not be discarded. This
		 * applies the captured batch to the proxy store and releases the capture,
		 * but deliberately skips the primitive rebuild: nothing will be gathered or
		 * drawn from this frame.
		 *
		 * The next CompleteSynchronize observes the store change and rebuilds from
		 * it, so the render-side state converges on the very next rendered frame.
		 * Between the two calls the proxy store is ahead of the primitive index,
		 * which is harmless because no gather runs in that window.
		 */
		SceneRenderStoreApplyStats ApplyDroppedSynchronizeCapture(
			const RenderSceneSyncRequest& request);

		[[nodiscard]] SceneProxyUpdateBatch CollectProxyUpdates(
			SceneSystem::Scene& scene,
			const RenderSceneSyncOptions& options = {},
			uint64_t frameId = 0u);
		SceneRenderStoreApplyStats ApplyProxyUpdates(const SceneProxyUpdateBatch& batch);
		/**
		 * Returns a strong handle to this render scene's proxy store, or null when
		 * no store has been acquired yet.
		 *
		 * A handle rather than a reference: the store pointer is reassigned by both
		 * halves of the synchronization hand-off - the capture half acquires it for
		 * a new scene, the resolve half re-acquires it when the applied batch
		 * belongs to a different store - so a caller holding a bare reference could
		 * be left pointing at a store another thread has already replaced. The
		 * handle keeps whatever it read alive for as long as the caller uses it,
		 * which is what makes reading the store from the render thread safe while
		 * the game thread is publishing.
		 */
		[[nodiscard]] std::shared_ptr<const SceneRenderStore> GetProxyStoreHandle() const;
		[[nodiscard]] std::shared_ptr<SceneRenderStore> GetProxyStoreHandle();
		[[nodiscard]] const void* GetProxySceneIdentity() const;
		void SetProxySceneIdentity(const void* sceneIdentity);
		/**
		 * How many captured synchronizations may be outstanding at once.
		 *
		 * One is the hand-off shape this class was built for: the capture and the
		 * resolve alternate, so a second capture while one is unresolved is
		 * refused. Raising it lets the capture run ahead of the resolve, which is
		 * what a render-thread consumer needs, and beyond the limit the capture is
		 * refused rather than queued without bound.
		 *
		 * Raising the limit is only sound because everything the resolve rewrites
		 * that the capture reads is now behind a published snapshot or a guarded
		 * handle; at depth one neither half can observe the other's partial state.
		 */
		void SetMaxOutstandingSynchronizations(size_t maxOutstanding);
		[[nodiscard]] size_t GetMaxOutstandingSynchronizations() const;
		[[nodiscard]] size_t GetOutstandingSynchronizationCount() const;
		/**
		 * True while that exact capture serial is still claimed.
		 *
		 * Lets a caller release a claim it may or may not still hold without risking
		 * a double release, which matters for the queued hand-off: a dropped frame and
		 * a partially resolved one both need "release only what is still mine".
		 */
		[[nodiscard]] bool HasOutstandingSynchronization(uint64_t captureSerial) const;

		[[nodiscard]] RenderSceneVisibleQueues GatherVisibleCommands(
			const RenderSceneVisibilityOptions& options = {},
			RenderSceneVisibilityMode mode = RenderSceneVisibilityMode::Auto) const;

		[[nodiscard]] size_t GetPrimitiveCount() const;
		[[nodiscard]] uint64_t GetSceneId() const;
		[[nodiscard]] uint64_t GetCachedCommandBuildCountForTesting() const;
		[[nodiscard]] uint64_t GetOpaqueSortTokenBuildCountForTesting() const;
#if defined(NLS_ENABLE_TEST_HOOKS)
		[[nodiscard]] uint64_t GetSpatialCandidateSnapshotCacheHitCountForTesting() const;
#endif
		[[nodiscard]] const DrawCallOptimizationStats& GetLastDrawCallOptimizationStats() const;
		[[nodiscard]] const DrawCallOptimizationStats& GetLastDrawCallOptimizationStatsForTesting() const;
		[[nodiscard]] const NLS::Render::Data::LargeSceneTelemetry& GetLastLargeSceneTelemetry() const;
		[[nodiscard]] const NLS::Render::Data::LargeSceneTelemetry& GetLastLargeSceneTelemetryForTesting() const;
		[[nodiscard]] std::shared_ptr<const SceneCullReasonDebugSnapshot> GetLastCullReasonDebugSnapshot() const;
		[[nodiscard]] const std::vector<ScenePrimitiveHandle>& GetLastVisiblePrimitiveHandles() const;
		[[nodiscard]] const std::vector<ScenePrimitiveHandle>& GetLastRemovedPrimitiveHandles() const;
		[[nodiscard]] std::vector<ScenePrimitiveHandle> GetLivePrimitiveHandles() const;
		[[nodiscard]] std::vector<ScenePickablePrimitiveDrawSource> CreatePickablePrimitiveDrawSourcesForHandles(
			const std::vector<ScenePrimitiveHandle>& handles) const;
		void AppendPickablePrimitiveDrawSourcesForHandles(
			const std::vector<ScenePrimitiveHandle>& handles,
			std::vector<ScenePickablePrimitiveDrawSource>& outSources) const;
		[[nodiscard]] const std::vector<ScenePrimitiveHandle>& GetLastRepresentationStreamingInterest() const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibilityForTesting(
			const RenderSceneVisibilityOptions& options = {},
			RenderSceneVisibilityMode mode = RenderSceneVisibilityMode::Auto) const;
		[[nodiscard]] ScenePrimitiveSnapshot CreatePrimitiveSnapshot(uint64_t frameSerial = 0u) const;
		[[nodiscard]] ScenePrimitiveSnapshot CreatePrimitiveSnapshotForHandles(
			const std::vector<ScenePrimitiveHandle>& handles,
			const std::vector<ScenePrimitiveHandle>& removedHandles,
			uint64_t frameSerial = 0u) const;
		[[nodiscard]] bool IsPrimitiveHandleLive(ScenePrimitiveHandle handle) const;
		void ClearRepresentationRecords();
		// Batch callers can defer RebuildLookup and perform one rebuild after the
		// whole batch; the lookup rebuild cost scales with the number of records.
		SceneLODGroupHandle RegisterLODGroup(
			const LODGroupRecord& group,
			bool rebuildLookup = true,
			bool* outMembershipChanged = nullptr);
		SceneHLODClusterHandle RegisterHLODCluster(const HLODClusterRecord& cluster, bool rebuildLookup = true);
		void ClearRepresentationRecordsForTesting();
		SceneLODGroupHandle RegisterLODGroupForTesting(const LODGroupRecord& group);
		SceneHLODClusterHandle RegisterHLODClusterForTesting(const HLODClusterRecord& cluster);
		[[nodiscard]] ScenePrimitiveSnapshot CreatePrimitiveSnapshotForTesting(uint64_t frameSerial = 0u) const;
		[[nodiscard]] bool IsPrimitiveHandleLiveForTesting(ScenePrimitiveHandle handle) const;
#if defined(NLS_ENABLE_TEST_HOOKS)
		void FinalizeOpaqueQueueForTesting(RenderSceneVisibleQueues::SceneDrawables& opaques) const;
#endif

	private:
		struct RepresentationRegistry;
		struct SpatialCandidateSnapshotCache;
		struct RenderSceneVisibilityRangeSnapshot
		{
			std::vector<uint64_t> primitiveBits;
			std::vector<uint64_t> meshBits;
			std::vector<ScenePrimitiveHandle> visiblePrimitiveHandles;
			size_t primitiveBegin = 0u;
			size_t meshBegin = 0u;
			uint64_t visiblePrimitiveCount = 0u;
			uint64_t visibleMeshCount = 0u;
			uint64_t spatialCandidateCount = 0u;
			uint64_t fullScanCandidateCount = 0u;
			uint64_t primitiveRecordsTouched = 0u;
			uint64_t visibilityTestedPrimitiveCount = 0u;
		};

		struct CachedCommandInputStamp
		{
			NLS::Render::Resources::Mesh* mesh = nullptr;
			NLS::Render::Resources::Material* material = nullptr;
			uint64_t meshContentRevision = 0u;
			uint64_t meshLODRevisionDigest = 0u;
			uint64_t materialInstanceId = 0u;
			uint64_t materialParameterRevision = 0u;
			uint64_t materialRenderStateRevision = 0u;
			uint64_t materialBindingRevision = 0u;
			uint64_t shaderInstanceId = 0u;
			uint64_t shaderGeneration = 0u;
			uint8_t stateMask = 0u;
			NLS::Render::Settings::EPrimitiveMode primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;

			[[nodiscard]] bool operator==(const CachedCommandInputStamp& other) const;
			[[nodiscard]] bool operator!=(const CachedCommandInputStamp& other) const { return !(*this == other); }
		};

		struct PrimitiveInputStamp
		{
				SceneProxyHandle proxyHandle;
				NLS::InstanceID ownerInstanceId = NLS::InstanceID_None;
				Engine::GameObject* owner = nullptr;
				Components::MeshFilter* meshFilter = nullptr;
				NLS::Render::Resources::Mesh* mesh = nullptr;
				Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
				uint32_t layer = 0u;
				uint64_t ownerRenderRevision = 0u;
				uint64_t transformRenderRevision = 0u;
				uint64_t meshFilterRenderRevision = 0u;
				uint64_t meshRendererRenderRevision = 0u;
				uint64_t meshContentRevision = 0u;
				uint64_t meshLODRevisionDigest = 0u;
				NLS::Render::Resources::Material* material = nullptr;
				uint64_t materialInstanceId = 0u;
				uint64_t requestedMaterialInstanceId = 0u;
				uint64_t materialParameterRevision = 0u;
				uint64_t materialRenderStateRevision = 0u;
				bool explicitMaterialTexturesResolved = true;
				bool requireExplicitMaterialTextures = false;
				bool allowDefaultMaterialForUnresolvedExplicitMaterials = false;
				bool ownerAlive = false;
				bool ownerActive = false;

			[[nodiscard]] bool operator==(const PrimitiveInputStamp& other) const;
			[[nodiscard]] bool operator!=(const PrimitiveInputStamp& other) const { return !(*this == other); }
		};

		struct CachedCommandSlot
		{
			struct OpaqueSortTokenEntry
			{
				NLS::Render::Resources::Mesh* mesh = nullptr;
				uint64_t meshContentRevision = 0u;
				uint64_t token = (std::numeric_limits<uint64_t>::max)();
			};

			CachedCommandInputStamp stamp;
			RenderCachedDrawCommand command;
			std::vector<OpaqueSortTokenEntry> opaqueSortTokensByLOD;
			bool valid = false;
		};

			struct SceneSynchronizationStamp
			{
				const SceneSystem::Scene* scene = nullptr;
				NLS::Render::Resources::Material* defaultMaterial = nullptr;
				NLS::Render::Resources::Material* overrideMaterial = nullptr;
				bool requireExplicitMaterialTextures = false;
				bool allowDefaultMaterialForUnresolvedExplicitMaterials = false;
				bool enableSpatialIndex = true;
				bool enableHLOD = false;
				double staticRebuildDirtyRatio = 0.20;
				uint64_t staticRebuildBudgetUs = 0u;

			[[nodiscard]] bool operator==(const SceneSynchronizationStamp& other) const = default;
		};

		/**
		 * Immutable snapshot of the materials the last resolve tracks.
		 *
		 * The capture half has to answer "did any material I care about change?"
		 * before the resolve half runs, and the resolve half rebuilds the tracked
		 * set whenever primitives change. Publishing an immutable copy under the
		 * resolved-state mutex is what lets those two run on different threads
		 * without the capture reading a set that is being rewritten underneath it.
		 */
		struct TrackedMaterialKeys
		{
			std::unordered_set<const NLS::Render::Resources::Material*> materials;
			std::unordered_set<uint64_t> instanceIds;
		};

		struct ResolvedSceneSynchronizationState
		{
			uint64_t sceneRenderContentRevision = 0u;
			uint64_t meshMutationEpoch = 0u;
			uint64_t storeChangeSerial = 0u;
			bool hasPendingDeclaredTextures = false;
			bool hasSynchronizationStamp = false;
			SceneSynchronizationStamp synchronizationStamp{};
		};

		struct TrackedMaterialState
		{
			uint64_t instanceId = 0u;
			uint64_t parameterRevision = 0u;
			uint64_t renderStateRevision = 0u;
			uint64_t bindingRevision = 0u;
		};

		struct RenderPrimitive
		{
			ScenePrimitiveHandle handle;
			SceneProxyHandle proxyHandle;
			uint32_t stableObjectIndex = NLS::Render::Data::DrawableObjectDescriptor::kInvalidStableObjectIndex;
			NLS::InstanceID ownerInstanceId = NLS::InstanceID_None;
			NLS::InstanceID parentInstanceId = NLS::InstanceID_None;
			uint64_t hlodScopeRootInstanceId = 0u;
			std::string sourceObjectKey;
			std::string hlodScopeMetadata;
			Components::MeshRenderer* meshRenderer = nullptr;
			NLS::InstanceID meshRendererInstanceId = NLS::InstanceID_None;
			Engine::GameObject* owner = nullptr;
			Components::MeshFilter* meshFilter = nullptr;
			NLS::Render::Resources::Mesh* mesh = nullptr;
			PrimitiveInputStamp lastInputStamp;
			std::vector<CachedCommandSlot> cachedCommands;
			NLS::Render::Geometry::BoundingSphere modelBoundingSphere;
			NLS::Render::Geometry::BoundingSphere customBoundingSphere;
			NLS::Render::Geometry::Bounds modelBounds;
                float maximumWorldDisplacement = 0;
			Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
			Maths::Matrix4 userMatrix = Maths::Matrix4::Identity;
            bool affectDistanceField = true;
            bool distanceFieldMostlyStatic = true;
			Maths::Vector3 worldPosition{};
			Components::MeshRenderer::EFrustumBehaviour frustumBehaviour =
				Components::MeshRenderer::EFrustumBehaviour::DISABLED;
			bool transientRenderingSuppressed = false;
			ScenePrimitiveVisibilitySettings visibilitySettings;
			std::optional<SceneLODGroupHandle> lodGroup;
			std::optional<SceneHLODClusterHandle> hlodCluster;
			bool hasMeshBinding = false;
			bool hasValidMaterial = false;
			bool ownerAlive = false;
			bool ownerActive = false;
			bool occupied = false;
			bool tombstoned = false;
			std::optional<size_t> nextFreePrimitiveSlot;
		};

		RenderPrimitive& AllocatePrimitiveSlot(
			Components::MeshRenderer& meshRenderer,
			RenderSceneSyncStats& stats);
		RenderPrimitive& FindOrCreatePrimitive(
			Components::MeshRenderer& meshRenderer,
			RenderSceneSyncStats& stats);
		void TombstonePrimitive(size_t primitiveIndex, RenderSceneSyncStats& stats);
		void SynchronizePrimitive(
			RenderPrimitive& primitive,
			const RenderSceneSyncOptions& options,
			RenderSceneSyncStats& stats,
			RenderSceneDeclaredTextureLookupCache& declaredTextureCache);
		[[nodiscard]] PrimitiveInputStamp BuildPrimitiveInputStamp(
			Components::MeshRenderer& meshRenderer,
			const RenderPrimitive& primitive,
			const RenderSceneSyncOptions& options,
			RenderSceneDeclaredTextureLookupCache& declaredTextureCache) const;
		[[nodiscard]] bool CanReuseSynchronizedPrimitive(
			const RenderPrimitive& primitive,
			const PrimitiveInputStamp& stamp,
			RenderSceneDeclaredTextureLookupCache& declaredTextureCache) const;
		[[nodiscard]] bool IntrinsicLODGroupRecordMatches(
			const LODGroupRecord& record,
			const RenderPrimitive& primitive,
			const NLS::Render::Resources::Mesh& mesh) const;
		void MarkPrimitiveDirtyForSnapshot(const RenderPrimitive& primitive);
		void RemoveMissingPrimitives(
			const std::unordered_map<Components::MeshRenderer*, NLS::InstanceID>& liveMeshRenderers,
			RenderSceneSyncStats& stats);
			bool SynchronizeProxyPrimitives(
				const RenderSceneSyncOptions& options,
				RenderSceneSyncStats& stats,
				bool forceFullResync,
				uint64_t baseStoreChangeSerial,
				uint64_t& outObservedStoreChangeSerial);
		NLS::Render::Resources::Material* ResolveMaterialForMesh(
			RenderPrimitive& primitive,
			NLS::Render::Resources::Mesh& mesh,
			const RenderSceneSyncOptions& options,
			RenderSceneSyncStats& stats,
			RenderSceneDeclaredTextureLookupCache& declaredTextureCache) const;
		CachedCommandInputStamp BuildCommandInputStamp(
			const RenderPrimitive& primitive,
			NLS::Render::Resources::Mesh& mesh,
			NLS::Render::Resources::Material& material) const;
		void RebuildCachedCommand(
			CachedCommandSlot& slot,
			const CachedCommandInputStamp& stamp,
			RenderSceneSyncStats& stats);
		[[nodiscard]] uint64_t GetMeshLODRevisionDigestForSynchronization(
			const NLS::Render::Resources::Mesh& mesh) const;
		[[nodiscard]] bool IsPrimitiveVisible(
			const RenderPrimitive& primitive,
			const RenderSceneVisibilityOptions& options) const;
		[[nodiscard]] bool IsMeshVisible(
			const RenderPrimitive& primitive,
			const NLS::Render::Resources::Mesh& mesh,
			const RenderSceneVisibilityOptions& options) const;
		[[nodiscard]] const std::vector<size_t>& GetMeshBaseIndices(uint64_t* touchedPrimitiveCount = nullptr) const;
		void MarkCommandOffsetTableDirty() noexcept;
		void RefreshSpatialIndex(const RenderSceneSyncOptions& options);
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibility(
			const RenderSceneVisibilityOptions& options,
			RenderSceneVisibilityMode mode) const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibility(
			const RenderSceneVisibilityOptions& options,
			RenderSceneVisibilityMode mode,
			const std::vector<size_t>& meshBaseIndices,
			bool buildVisibilityBitsets = true) const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibilitySerialRange(
			const RenderSceneVisibilityOptions& options,
			const std::vector<size_t>& meshBaseIndices,
			size_t primitiveBegin,
			size_t primitiveEnd) const;
		[[nodiscard]] RenderSceneVisibilityRangeSnapshot EvaluateVisibilityCompactRange(
			const RenderSceneVisibilityOptions& options,
			const std::vector<size_t>& meshBaseIndices,
			size_t primitiveBegin,
			size_t primitiveEnd) const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibilityParallel(
			const RenderSceneVisibilityOptions& options,
			const std::vector<size_t>& meshBaseIndices) const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibilitySpatial(
			const RenderSceneVisibilityOptions& options,
			const std::vector<size_t>& meshBaseIndices,
			RenderSceneVisibilityMode mode,
			bool buildVisibilityBitsets = true) const;
		[[nodiscard]] RenderSceneVisibilitySnapshot EvaluateVisibilityThroughPipeline(
			const RenderSceneVisibilityOptions& options,
			const std::vector<size_t>& meshBaseIndices,
			SceneVisibilityPipelineMode mode,
			bool buildVisibilityBitsets = true) const;
		[[nodiscard]] std::vector<SceneHLODClusterHandle> ResolveInspectableHLODClusters(
			const RenderSceneVisibilityOptions& options) const;
		void MergeVisibilityRangeSnapshot(
			RenderSceneVisibilitySnapshot& target,
			const RenderSceneVisibilityRangeSnapshot& source) const;
		void AppendVisibleDrawable(
			RenderSceneVisibleQueues& output,
			const RenderPrimitive& primitive,
			const RenderCachedDrawCommand& command,
			const RenderSceneVisibilityOptions& options) const;
		void FinalizeOpaqueQueue(RenderSceneVisibleQueues::SceneDrawables& opaques) const;
		void AssignVisibleObjectIndices(RenderSceneVisibleQueues& output) const;
		void RefreshSyncTelemetry(const RenderSceneSyncStats& stats);
		void ClearImportedHierarchyHLODRecords(bool rebuildLookup = true);
		void RebuildImportedHierarchyHLODRecords();
		[[nodiscard]] bool HasTrackedMaterialMutation();
		void RefreshTrackedMaterialStates();
		[[nodiscard]] static SceneSynchronizationStamp BuildSceneSynchronizationStamp(
			const SceneSystem::Scene* scene,
			const RenderSceneSyncOptions& options);
		/**
		 * Single source of truth for what the resolve half last accounted for.
		 *
		 * The capture half decides whether it may reuse the previous
		 * synchronization from exactly these values, so they are the hand-off
		 * between the two halves. They are published as one block under one mutex
		 * rather than as loose members: a torn read would let the capture half
		 * believe it had accounted for changes the resolve half had not applied
		 * yet, and it would then skip collecting them.
		 */
		[[nodiscard]] ResolvedSceneSynchronizationState ReadResolvedSynchronizationState() const;
		void PublishResolvedSynchronizationState(const ResolvedSceneSynchronizationState& state);
		[[nodiscard]] std::shared_ptr<const TrackedMaterialKeys> ReadTrackedMaterialKeys() const;
		void PublishTrackedMaterialKeys(std::shared_ptr<const TrackedMaterialKeys> keys);
		/** Marks the capture with this serial resolved. Never moves it backwards. */
		void ReleaseCaptureClaim(uint64_t captureSerial) noexcept;
		void ResetMovedFromState() noexcept;

		uint64_t m_sceneId = 0u;
		std::vector<RenderPrimitive> m_primitives;
		uint32_t m_nextStableObjectIndex = 0u;
		std::vector<uint32_t> m_freeStableObjectIndices;
			std::unordered_map<Components::MeshRenderer*, size_t> m_primitiveIndexByMeshRenderer;
			std::unordered_map<uint64_t, size_t> m_primitiveIndexByProxy;
			std::unordered_map<NLS::InstanceID, NLS::InstanceID> m_parentByOwnerInstanceId;
			std::unordered_set<uint64_t> m_trackedMaterialInstanceIds;
			std::unordered_set<uint64_t> m_pendingDeclaredTextureProxyKeys;
		std::optional<size_t> m_firstFreePrimitiveSlot;
		size_t m_livePrimitiveCount = 0u;
		uint64_t m_lastSceneFastAccessRevision = 0u;
		uint64_t m_lastMeshMutationEpoch = 0u;
		uint64_t m_lastObservedMaterialMutationEpoch = 0u;
		mutable std::unordered_map<const NLS::Render::Resources::Mesh*, uint64_t>
			m_syncMeshLODRevisionDigestCache;
		std::unordered_map<NLS::Render::Resources::Material*, TrackedMaterialState>
			m_trackedMaterialStates;
		uint64_t m_nextCachedCommandBuildSerial = 1u;
		uint64_t m_cachedCommandBuildCount = 0u;
		uint64_t m_opaqueSortTokenBuildCount = 0u;
		mutable uint64_t m_nextPrimitiveSnapshotSerial = 1u;
		mutable std::vector<size_t> m_cachedMeshBaseIndices;
		mutable bool m_commandOffsetTableDirty = true;
		std::vector<ScenePrimitiveHandle> m_lastDirtySyncHandles;
		std::vector<ScenePrimitiveHandle> m_lastRemovedHandles;
		RenderSceneSyncStats m_lastSyncStats{};
		mutable DrawCallOptimizationStats m_lastDrawCallOptimizationStats{};
		mutable NLS::Render::Data::LargeSceneTelemetry m_lastLargeSceneTelemetry{};
		mutable std::shared_ptr<const SceneCullReasonDebugSnapshot> m_lastCullReasonDebugSnapshot;
		mutable std::vector<ScenePrimitiveHandle> m_lastVisiblePrimitiveHandles;
		mutable std::vector<ScenePrimitiveHandle> m_lastRepresentationStreamingInterest;
		std::unique_ptr<SceneSpatialIndex> m_spatialIndex;
		std::unique_ptr<RepresentationRegistry> m_representationRegistry;
		mutable std::unique_ptr<SpatialCandidateSnapshotCache> m_spatialCandidateSnapshotCache;
		mutable uint64_t m_spatialCandidateSnapshotCacheHitCount = 0u;
		std::vector<SceneHLODClusterHandle> m_importedHierarchyHLODClusterHandles;
		std::shared_ptr<SceneProxySourceRegistry> m_proxySourceRegistry;
			std::shared_ptr<SceneRenderStore> m_proxyStore;
			// Guards the two handles above. They are read by the capture half and
			// rewritten by both halves, so every access goes through the accessors.
			mutable std::mutex m_proxyHandleMutex;
			/**
			 * Identity of the scene the handles above belong to.
			 *
			 * Read and written by the capture half, and read by the resolve half when
			 * it has to acquire a store for an applied batch, so it lives under the
			 * same lock as the handles it describes.
			 */
		const void* m_proxySceneIdentity = nullptr;
			uint64_t m_proxyFrameSerial = 0u;
			// Alternation guard for the capture / resolve hand-off. The capture and
			// the resolve touch overlapping state, so only one capture may be
			// outstanding. Serials advance only on a successful claim and are
			// recorded on completion, which keeps a rejected capture from wedging
			// the scene: once the outstanding capture completes, claims work again.
			mutable std::mutex m_outstandingSynchronizationMutex;
			/**
			 * Capture serials that have been claimed but not resolved, oldest first.
			 *
			 * Replaces a pair of start/complete counters: the counters could only
			 * express "none or one outstanding", which is exactly the constraint the
			 * render-thread hand-off has to lift. The queue keeps the same refusal
			 * behaviour at depth one and simply allows more entries at a higher
			 * limit. Resolution removes by serial, so a caller that resolves out of
			 * order still releases exactly its own claim.
			 */
			std::deque<uint64_t> m_outstandingSynchronizations;
			uint64_t m_nextCaptureSerial = 0u;
			size_t m_maxOutstandingSynchronizations = 1u;
			mutable std::mutex m_resolvedSynchronizationMutex;
			ResolvedSceneSynchronizationState m_resolvedSynchronization;
		// Guarded by m_resolvedSynchronizationMutex. Immutable once published.
		std::shared_ptr<const TrackedMaterialKeys> m_trackedMaterialKeys;
		};
}
