#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <Math/Vector3.h>
#include <Rendering/Data/Frustum.h>
#include <Rendering/Data/FrameInfo.h>

#include "EngineDef.h"
#include "Rendering/RenderScene.h"
#include "Rendering/SceneHLOD.h"
#include "Rendering/SceneLOD.h"
#include "Rendering/SceneOcclusion.h"
#include "Rendering/SceneStreamingResidency.h"

namespace NLS::Engine::Rendering
{
	class SceneSpatialIndex;

	enum class CullReason : uint8_t
	{
		Visible,
		Inactive,
		LayerMasked,
		DistanceCulled,
		SpatialMiss,
		FrustumCulled,
		LODInactive,
		HLODChildSuppressed,
		HLODProxyInactive,
		Occluded,
		NotResident,
		MissingMesh,
		InvalidMaterial,
		BackendUnsupported
	};

	static constexpr size_t kSceneVisibilityCullReasonCount =
		static_cast<size_t>(CullReason::BackendUnsupported) + 1u;
	static_assert(
		kSceneVisibilityCullReasonCount <= NLS::Render::Data::kLargeSceneCullReasonCount,
		"Large-scene frame telemetry must be able to store every visibility cull reason.");

	struct SceneCullReasonDisplayBucket
	{
		const char* label = "";
		std::array<CullReason, 2u> reasons {};
		size_t reasonCount = 0u;
	};

	enum class SceneVisibilityPipelineMode : uint8_t
	{
		Auto,
		Serial,
		Parallel,
		FullScanComparison
	};

	struct SceneVisibilityPipelineOptions
	{
		const NLS::Render::Data::Frustum* frustum = nullptr;
		Maths::Vector3 cameraPosition{};
		uint32_t visibleLayerMask = 0xFFFF'FFFFu;
		bool enableSpatialIndex = false;
		bool enableParallelVisibility = true;
		bool enableLOD = false;
		bool enableHLOD = false;
		bool enableOcclusion = false;
		size_t parallelVisibilityPrimitiveThreshold = 1024u;
		size_t parallelVisibilityPrimitivesPerTask = 128u;
		uint32_t maxVisibilityJobs = 0u;
		float spatialQueryRadius = 0.0f;
		float lodBias = 1.0f;
		uint64_t lodHistoryViewKey = 0u;
		bool allowHLOD = true;
		bool editorInspectionView = false;
		std::vector<ScenePrimitiveHandle> selectedPrimitiveHandles;
	};

	struct SceneRepresentationState
	{
		const std::vector<LODGroupRecord>* lodGroups = nullptr;
		const std::vector<HLODClusterRecord>* hlodClusters = nullptr;
		const std::unordered_map<ScenePrimitiveHandle, std::vector<uint32_t>, ScenePrimitiveHandleStableHash>* lodGroupsByPrimitive = nullptr;
		const std::unordered_map<ScenePrimitiveHandle, std::vector<uint32_t>, ScenePrimitiveHandleStableHash>* hlodClustersByPrimitive = nullptr;
		std::vector<LODSelectionHistory>* lodSelectionHistory = nullptr;
		const RepresentationResidencySnapshot* residency = nullptr;
		const SceneOcclusionState* occlusion = nullptr;
	};

	struct SceneRepresentationCandidateExpansion
	{
		std::vector<ScenePrimitiveHandle> primitiveHandles;
		std::vector<size_t> lodGroupIndices;
		std::vector<size_t> hlodClusterIndices;
	};

	struct SceneVisibilityPipelineResult
	{
		std::vector<uint64_t> primitiveBits;
		std::vector<uint64_t> meshBits;
		std::vector<ScenePrimitiveHandle> visiblePrimitiveHandles;
		std::vector<ScenePrimitiveCommandOffsetRange> eligibleCommandRanges;
		std::vector<CullReason> cullReasons;
		std::vector<uint32_t> selectedLOD;
		std::vector<SceneHLODClusterHandle> activeHLODClusters;
		std::vector<ScenePrimitiveHandle> suppressedByHLOD;
		std::vector<ScenePrimitiveHandle> occludedPrimitiveHandles;
		std::vector<ScenePrimitiveHandle> representationStreamingInterest;
		std::vector<ScenePrimitiveHandle> representationInputs;
		uint64_t primitiveCount = 0u;
		uint64_t meshCount = 0u;
		uint64_t visiblePrimitiveCount = 0u;
		uint64_t visibleMeshCount = 0u;
		uint64_t occlusionTestCount = 0u;
		uint64_t occlusionCulledCount = 0u;
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
		bool usesSparseVisiblePrimitiveHandles = false;
		bool usedParallelEvaluation = false;
	};

	struct SceneCullReasonDebugEntry
	{
		ScenePrimitiveHandle handle;
		CullReason reason = CullReason::Visible;
		uint32_t selectedLOD = 0u;
		uint64_t commandOffsetBegin = 0u;
		uint64_t commandOffsetEnd = 0u;
		bool visible = false;
	};

	struct SceneCullReasonDebugSnapshot
	{
		uint64_t frameSerial = 0u;
		uint64_t sceneId = 0u;
		uint64_t primitiveCount = 0u;
		uint64_t visiblePrimitiveCount = 0u;
		std::array<uint64_t, NLS::Render::Data::kLargeSceneCullReasonCount> reasonCounts {};
		std::vector<SceneCullReasonDebugEntry> entries;
	};

	class NLS_ENGINE_API SceneVisibilityPipeline
	{
	public:
		[[nodiscard]] static SceneVisibilityPipelineResult Evaluate(
			const SceneVisibilityPipelineOptions& options,
			const ScenePrimitiveSnapshot& primitives,
			const SceneSpatialIndex& spatialIndex,
			SceneVisibilityPipelineMode mode = SceneVisibilityPipelineMode::Auto);
		[[nodiscard]] static SceneVisibilityPipelineResult Evaluate(
			const SceneVisibilityPipelineOptions& options,
			const ScenePrimitiveSnapshot& primitives,
			const SceneSpatialIndex& spatialIndex,
			const SceneRepresentationState& representation,
			SceneVisibilityPipelineMode mode = SceneVisibilityPipelineMode::Auto);
		[[nodiscard]] static SceneRepresentationCandidateExpansion ExpandRepresentationCandidates(
			const std::vector<ScenePrimitiveHandle>& candidateHandles,
			const ScenePrimitiveSnapshot& candidatePrimitives,
			const SceneRepresentationState& representation);
		[[nodiscard]] static StreamingResidencyPlanInput BuildStreamingResidencyInput(
			uint64_t frameSerial,
			const SceneVisibilityPipelineResult& visibility);
		[[nodiscard]] static SceneCullReasonDebugSnapshot BuildCullReasonDebugSnapshot(
			const ScenePrimitiveSnapshot& primitives,
			const SceneVisibilityPipelineResult& visibility,
			uint64_t maxEntries = UINT64_MAX);
		[[nodiscard]] static const std::array<SceneCullReasonDisplayBucket, 13u>&
			GetCullReasonDisplayBuckets();
	};
}
