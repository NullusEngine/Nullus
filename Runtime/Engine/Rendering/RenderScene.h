#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Math/Matrix4.h>
#include <Rendering/Data/DrawCallOptimizationStats.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Data/FrameInfo.h>
#include <Rendering/Data/StateMask.h>
#include <Rendering/Entities/Drawable.h>
#include <Rendering/Geometry/Bounds.h>
#include <Rendering/Geometry/BoundingSphere.h>

#include "Components/MeshRenderer.h"
#include "EngineDef.h"
#include "Object/Object.h"

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
		Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
		Maths::Matrix4 userMatrix = Maths::Matrix4::Identity;
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
		};

	struct RenderSceneSyncStats
	{
		uint64_t addedPrimitiveCount = 0u;
		uint64_t reusedPrimitiveCount = 0u;
		uint64_t removedPrimitiveCount = 0u;
		uint64_t rebuiltCachedCommandCount = 0u;
		uint64_t syncTouchedPrimitiveCount = 0u;
		uint64_t syncFullSweepCount = 0u;
		uint64_t syncSweepTouchedSlotCount = 0u;
		uint64_t boundsDirtyPrimitiveCount = 0u;
		uint64_t primitiveSlotReuseCount = 0u;
		uint64_t declaredTextureLookupCount = 0u;
		uint64_t declaredTextureCacheHitCount = 0u;
		uint64_t declaredTextureCacheMissCount = 0u;
		uint64_t declaredTextureResourceScanCount = 0u;
		uint64_t syncTimeNs = 0u;
	};

	struct RenderSceneVisibilityOptions
	{
		const NLS::Render::Data::Frustum* frustum = nullptr;
		Maths::Vector3 cameraPosition{};
		uint32_t visibleLayerMask = 0xFFFF'FFFFu;
		const LargeSceneSettings* largeSceneSettings = nullptr;
		float lodBias = 1.0f;
		uint64_t lodHistoryViewKey = 0u;
		bool allowHLOD = true;
		bool editorInspectionView = false;
		std::vector<ScenePrimitiveHandle> selectedPrimitiveHandles;
		std::vector<const Engine::GameObject*> inspectionRootObjects;
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
	};

	struct ScenePickablePrimitiveDrawSource
	{
		NLS::Engine::GameObject* owner = nullptr;
		NLS::Render::Resources::Mesh* mesh = nullptr;
		Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
		NLS::Render::Data::StateMask stateMask{};
	};

	class NLS_ENGINE_API RenderScene
	{
	public:
		RenderScene();
		RenderScene(const RenderScene&) = delete;
		RenderScene& operator=(const RenderScene&) = delete;
		RenderScene(RenderScene&& other) noexcept;
		RenderScene& operator=(RenderScene&& other) noexcept;
		~RenderScene();

		RenderSceneSyncStats Synchronize(
			SceneSystem::Scene& scene,
			const RenderSceneSyncOptions& options = {});

		[[nodiscard]] RenderSceneVisibleQueues GatherVisibleCommands(
			const RenderSceneVisibilityOptions& options = {},
			RenderSceneVisibilityMode mode = RenderSceneVisibilityMode::Auto) const;

		[[nodiscard]] size_t GetPrimitiveCount() const;
		[[nodiscard]] uint64_t GetSceneId() const;
		[[nodiscard]] uint64_t GetCachedCommandBuildCountForTesting() const;
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
		SceneLODGroupHandle RegisterLODGroup(const LODGroupRecord& group);
		SceneHLODClusterHandle RegisterHLODCluster(const HLODClusterRecord& cluster);
		void ClearRepresentationRecordsForTesting();
		SceneLODGroupHandle RegisterLODGroupForTesting(const LODGroupRecord& group);
		SceneHLODClusterHandle RegisterHLODClusterForTesting(const HLODClusterRecord& cluster);
		[[nodiscard]] ScenePrimitiveSnapshot CreatePrimitiveSnapshotForTesting(uint64_t frameSerial = 0u) const;
		[[nodiscard]] bool IsPrimitiveHandleLiveForTesting(ScenePrimitiveHandle handle) const;

	private:
		struct RepresentationRegistry;
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
			uint64_t materialInstanceId = 0u;
			uint64_t materialParameterRevision = 0u;
			uint64_t materialRenderStateRevision = 0u;
			uint8_t stateMask = 0u;
			NLS::Render::Settings::EPrimitiveMode primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;

			[[nodiscard]] bool operator==(const CachedCommandInputStamp& other) const;
			[[nodiscard]] bool operator!=(const CachedCommandInputStamp& other) const { return !(*this == other); }
		};

		struct PrimitiveInputStamp
		{
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
				uint64_t materialInstanceId = 0u;
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
			CachedCommandInputStamp stamp;
			RenderCachedDrawCommand command;
			bool valid = false;
		};

		struct RenderPrimitive
		{
			ScenePrimitiveHandle handle;
			Components::MeshRenderer* meshRenderer = nullptr;
			NLS::InstanceID meshRendererInstanceId = NLS::InstanceID_None;
			Engine::GameObject* owner = nullptr;
			Components::MeshFilter* meshFilter = nullptr;
			NLS::Render::Resources::Mesh* mesh = nullptr;
			PrimitiveInputStamp lastInputStamp;
			std::vector<CachedCommandSlot> cachedCommands;
			NLS::Render::Geometry::BoundingSphere modelBoundingSphere;
			NLS::Render::Geometry::Bounds modelBounds;
			Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
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
			const RenderSceneSyncOptions& options) const;
		[[nodiscard]] bool CanReuseSynchronizedPrimitive(
			const RenderPrimitive& primitive,
			const PrimitiveInputStamp& stamp) const;
		void MarkPrimitiveDirtyForSnapshot(const RenderPrimitive& primitive);
		void RemoveMissingPrimitives(
			const std::unordered_map<Components::MeshRenderer*, NLS::InstanceID>& liveMeshRenderers,
			RenderSceneSyncStats& stats);
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
		void RebuildImportedHierarchyHLODRecords(const SceneSystem::Scene& scene);
		void ResetMovedFromState() noexcept;

		uint64_t m_sceneId = 0u;
		std::vector<RenderPrimitive> m_primitives;
		std::unordered_map<Components::MeshRenderer*, size_t> m_primitiveIndexByMeshRenderer;
		std::optional<size_t> m_firstFreePrimitiveSlot;
		size_t m_livePrimitiveCount = 0u;
		uint64_t m_lastSceneFastAccessRevision = 0u;
		uint64_t m_nextCachedCommandBuildSerial = 1u;
		uint64_t m_cachedCommandBuildCount = 0u;
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
		std::vector<SceneHLODClusterHandle> m_importedHierarchyHLODClusterHandles;
	};
}
