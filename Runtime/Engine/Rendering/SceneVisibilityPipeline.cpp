#include "Rendering/SceneVisibilityPipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <Math/Vector4.h>
#include <Profiling/Profiler.h>

#include "Rendering/Geometry/BoundingSphereUtils.h"
#include "Jobs/JobSystem.h"
#include "Rendering/SceneSpatialIndex.h"

namespace NLS::Engine::Rendering
{
namespace
{
	constexpr size_t kBitsPerWord = sizeof(uint64_t) * 8u;
	const std::array<SceneCullReasonDisplayBucket, 13u> kCullReasonDisplayBuckets {{
		{ "Visible", { CullReason::Visible, CullReason::Visible }, 1u },
		{ "Inactive", { CullReason::Inactive, CullReason::Inactive }, 1u },
		{ "Layer", { CullReason::LayerMasked, CullReason::LayerMasked }, 1u },
		{ "Distance", { CullReason::DistanceCulled, CullReason::DistanceCulled }, 1u },
		{ "Spatial", { CullReason::SpatialMiss, CullReason::SpatialMiss }, 1u },
		{ "Frustum", { CullReason::FrustumCulled, CullReason::FrustumCulled }, 1u },
		{ "LOD", { CullReason::LODInactive, CullReason::LODInactive }, 1u },
		{ "HLOD", { CullReason::HLODChildSuppressed, CullReason::HLODProxyInactive }, 2u },
		{ "Occluded", { CullReason::Occluded, CullReason::Occluded }, 1u },
		{ "NotResident", { CullReason::NotResident, CullReason::NotResident }, 1u },
		{ "MissingMesh", { CullReason::MissingMesh, CullReason::MissingMesh }, 1u },
		{ "InvalidMaterial", { CullReason::InvalidMaterial, CullReason::InvalidMaterial }, 1u },
		{ "Backend", { CullReason::BackendUnsupported, CullReason::BackendUnsupported }, 1u }
	}};
	static_assert(
		kSceneVisibilityCullReasonCount == 14u,
		"Update cull reason display buckets when adding or removing cull reasons.");

	struct HandleHash
	{
		size_t operator()(const ScenePrimitiveHandle& handle) const noexcept
		{
			auto hash = static_cast<size_t>(handle.sceneId);
			hash ^= static_cast<size_t>(handle.index) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
			hash ^= static_cast<size_t>(handle.generation) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
			return hash;
		}
	};

	struct SnapshotLookup
	{
		std::unordered_map<ScenePrimitiveHandle, size_t, HandleHash> denseIndexByHandle;
		uint64_t meshCount = 0u;
		uint32_t maxHandleIndex = 0u;
		bool hasAnyHandle = false;
	};

	struct RangeResult
	{
		std::vector<uint64_t> meshBits;
		std::vector<ScenePrimitiveHandle> visiblePrimitiveHandles;
		std::vector<ScenePrimitiveCommandOffsetRange> eligibleCommandRanges;
		std::vector<CullReason> cullReasons;
		uint64_t visiblePrimitiveCount = 0u;
		uint64_t visibleMeshCount = 0u;
		uint64_t fullScanCandidateCount = 0u;
		uint64_t primitiveRecordsTouched = 0u;
		uint64_t visibilityTestedPrimitiveCount = 0u;
		size_t begin = 0u;
	};

	size_t BitWordCount(const size_t bitCount)
	{
		return (bitCount + kBitsPerWord - 1u) / kBitsPerWord;
	}

	void SetBit(std::vector<uint64_t>& words, const size_t index)
	{
		if (index >= words.size() * kBitsPerWord)
			return;

		words[index / kBitsPerWord] |= 1ull << (index % kBitsPerWord);
	}

	void ClearBit(std::vector<uint64_t>& words, const size_t index)
	{
		if (index >= words.size() * kBitsPerWord)
			return;

		words[index / kBitsPerWord] &= ~(1ull << (index % kBitsPerWord));
	}

	bool IsBitSet(const std::vector<uint64_t>& words, const size_t index)
	{
		if (index >= words.size() * kBitsPerWord)
			return false;

		return (words[index / kBitsPerWord] & (1ull << (index % kBitsPerWord))) != 0u;
	}

	size_t CountTrailingZeroBits(uint64_t word)
	{
		size_t count = 0u;
		while ((word & 1ull) == 0u)
		{
			word >>= 1u;
			++count;
		}
		return count;
	}

	void MergeBitRange(
		std::vector<uint64_t>& target,
		const std::vector<uint64_t>& source,
		const size_t targetBitBegin)
	{
		for (size_t sourceWordIndex = 0u; sourceWordIndex < source.size(); ++sourceWordIndex)
		{
			auto sourceWord = source[sourceWordIndex];
			while (sourceWord != 0u)
			{
				const auto bitIndex = sourceWordIndex * kBitsPerWord + CountTrailingZeroBits(sourceWord);
				SetBit(target, targetBitBegin + bitIndex);
				sourceWord &= sourceWord - 1u;
			}
		}
	}

	NLS::Render::Geometry::Bounds ExtractWorldBounds(const ScenePrimitiveSnapshotRecord& record)
	{
		return NLS::Render::Geometry::TransformBounds(record.modelBounds, record.worldMatrix);
	}

	float DistanceToBounds(const Maths::Vector3& point, const NLS::Render::Geometry::Bounds& bounds)
	{
		const auto halfSize = bounds.size * 0.5f;
		const auto dx = std::max(std::abs(point.x - bounds.center.x) - halfSize.x, 0.0f);
		const auto dy = std::max(std::abs(point.y - bounds.center.y) - halfSize.y, 0.0f);
		const auto dz = std::max(std::abs(point.z - bounds.center.z) - halfSize.z, 0.0f);
		return Maths::Vector3(dx, dy, dz).Length();
	}

	float FarthestDistanceToBounds(const Maths::Vector3& point, const NLS::Render::Geometry::Bounds& bounds)
	{
		const auto corners = NLS::Render::Geometry::BuildBoundsCorners(bounds);
		float distance = 0.0f;
		for (const auto& corner : corners)
			distance = std::max(distance, Maths::Vector3::Distance(point, corner));
		return distance;
	}

	bool LayerPasses(const ScenePrimitiveSnapshotRecord& record, const uint32_t visibleLayerMask)
	{
		if (record.visibilitySettings.layer >= 32u)
			return false;
		return (visibleLayerMask & (1u << record.visibilitySettings.layer)) != 0u;
	}

	bool DistancePasses(
		const ScenePrimitiveSnapshotRecord& record,
		const SceneVisibilityPipelineOptions& options)
	{
		if (!record.visibilitySettings.distanceCullingEnabled)
			return true;

		const auto bounds = ExtractWorldBounds(record);
		const auto nearestDistance = DistanceToBounds(options.cameraPosition, bounds);
		const auto farthestDistance = FarthestDistanceToBounds(options.cameraPosition, bounds);
		if (farthestDistance < record.visibilitySettings.minDrawDistance)
			return false;
		if (record.visibilitySettings.maxDrawDistance > 0.0f &&
			nearestDistance > record.visibilitySettings.maxDrawDistance)
		{
			return false;
		}
		return true;
	}

	bool FrustumPasses(
		const ScenePrimitiveSnapshotRecord& record,
		const SceneVisibilityPipelineOptions& options)
	{
		if (options.frustum == nullptr)
			return true;
		if (record.frustumBehaviour == Components::MeshRenderer::EFrustumBehaviour::DISABLED)
			return true;

		return options.frustum->BoundsInFrustum(record.modelBounds, record.worldMatrix);
	}

	CullReason RevalidateCandidate(
		const ScenePrimitiveSnapshotRecord& record,
		const SceneVisibilityPipelineOptions& options)
	{
		if (!record.occupied || record.tombstoned || !record.ownerAlive || !record.ownerActive)
			return CullReason::Inactive;
		if (!record.hasMeshBinding)
			return CullReason::MissingMesh;
		if (record.mesh == nullptr)
			return CullReason::NotResident;
		if (!record.hasValidMaterial)
			return CullReason::InvalidMaterial;
		if (!LayerPasses(record, options.visibleLayerMask))
			return CullReason::LayerMasked;
		if (!DistancePasses(record, options))
			return CullReason::DistanceCulled;
		if (!FrustumPasses(record, options))
			return CullReason::FrustumCulled;
		return CullReason::Visible;
	}

	CullReason RevalidateRepresentationProxyCandidate(
		const ScenePrimitiveSnapshotRecord& record,
		const SceneVisibilityPipelineOptions& options)
	{
		if (!record.occupied || record.tombstoned || !record.ownerAlive)
			return CullReason::Inactive;
		if (!record.hasMeshBinding)
			return CullReason::MissingMesh;
		if (record.mesh == nullptr)
			return CullReason::NotResident;
		if (!record.hasValidMaterial)
			return CullReason::InvalidMaterial;
		if (!LayerPasses(record, options.visibleLayerMask))
			return CullReason::LayerMasked;
		if (!DistancePasses(record, options))
			return CullReason::DistanceCulled;
		if (!FrustumPasses(record, options))
			return CullReason::FrustumCulled;
		return CullReason::Visible;
	}

	SnapshotLookup BuildLookup(const ScenePrimitiveSnapshot& primitives)
	{
		SnapshotLookup lookup;
		lookup.denseIndexByHandle.reserve(primitives.primitiveRecords.size());
		for (size_t denseIndex = 0u; denseIndex < primitives.primitiveRecords.size(); ++denseIndex)
		{
			const auto& record = primitives.primitiveRecords[denseIndex];
			lookup.denseIndexByHandle[record.handle] = denseIndex;
			lookup.meshCount = std::max<uint64_t>(lookup.meshCount, record.commandOffsetEnd);
			lookup.maxHandleIndex = std::max<uint32_t>(lookup.maxHandleIndex, record.handle.index);
			lookup.hasAnyHandle = true;
		}
		return lookup;
	}

	uint64_t ResolvePrimitiveCount(const SnapshotLookup& lookup)
	{
		return lookup.hasAnyHandle
			? static_cast<uint64_t>(lookup.maxHandleIndex) + 1u
			: 0u;
	}

	void MarkVisible(
		SceneVisibilityPipelineResult& result,
		const ScenePrimitiveSnapshotRecord& record,
		const size_t denseIndex)
	{
		SetBit(result.primitiveBits, record.handle.index);
		for (uint64_t commandOffset = record.commandOffsetBegin; commandOffset < record.commandOffsetEnd; ++commandOffset)
		{
			SetBit(result.meshBits, static_cast<size_t>(commandOffset));
			++result.visibleMeshCount;
		}
		++result.visiblePrimitiveCount;
		result.visiblePrimitiveHandles.push_back(record.handle);
		result.eligibleCommandRanges.push_back({ record.handle, record.commandOffsetBegin, record.commandOffsetEnd });
		result.cullReasons[denseIndex] = CullReason::Visible;
	}

	bool ContainsHandle(const std::vector<ScenePrimitiveHandle>& handles, const ScenePrimitiveHandle handle)
	{
		return std::find(handles.begin(), handles.end(), handle) != handles.end();
	}

	void AddUniqueHandle(std::vector<ScenePrimitiveHandle>& handles, const ScenePrimitiveHandle handle)
	{
		if (!ContainsHandle(handles, handle))
			handles.push_back(handle);
	}

	void AddUniqueCluster(std::vector<SceneHLODClusterHandle>& handles, const SceneHLODClusterHandle handle)
	{
		if (std::find(handles.begin(), handles.end(), handle) == handles.end())
			handles.push_back(handle);
	}

	std::optional<size_t> FindDenseIndex(
		const SnapshotLookup& lookup,
		const ScenePrimitiveSnapshot& primitives,
		const ScenePrimitiveHandle handle)
	{
		const auto found = lookup.denseIndexByHandle.find(handle);
		if (found == lookup.denseIndexByHandle.end())
			return std::nullopt;
		if (found->second >= primitives.primitiveRecords.size())
			return std::nullopt;
		if (primitives.primitiveRecords[found->second].handle != handle)
			return std::nullopt;
		return found->second;
	}

	bool IsHandleVisible(
		const SceneVisibilityPipelineResult& result,
		const SnapshotLookup& lookup,
		const ScenePrimitiveSnapshot& primitives,
		const ScenePrimitiveHandle handle)
	{
		const auto denseIndex = FindDenseIndex(lookup, primitives, handle);
		if (!denseIndex.has_value())
			return false;
		return IsBitSet(result.primitiveBits, primitives.primitiveRecords[*denseIndex].handle.index);
	}

	void RemoveVisibleHandle(
		SceneVisibilityPipelineResult& result,
		const SnapshotLookup& lookup,
		const ScenePrimitiveSnapshot& primitives,
		const ScenePrimitiveHandle handle,
		const CullReason reason)
	{
		const auto denseIndex = FindDenseIndex(lookup, primitives, handle);
		if (!denseIndex.has_value() || !IsHandleVisible(result, lookup, primitives, handle))
			return;

		const auto& record = primitives.primitiveRecords[*denseIndex];
		ClearBit(result.primitiveBits, record.handle.index);
		for (uint64_t commandOffset = record.commandOffsetBegin; commandOffset < record.commandOffsetEnd; ++commandOffset)
		{
			if (IsBitSet(result.meshBits, static_cast<size_t>(commandOffset)) && result.visibleMeshCount > 0u)
				--result.visibleMeshCount;
			ClearBit(result.meshBits, static_cast<size_t>(commandOffset));
		}
		if (result.visiblePrimitiveCount > 0u)
			--result.visiblePrimitiveCount;
		result.cullReasons[*denseIndex] = reason;
	}

	void AddVisibleHandle(
		SceneVisibilityPipelineResult& result,
		const SnapshotLookup& lookup,
		const ScenePrimitiveSnapshot& primitives,
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveHandle handle,
		const bool forceRepresentationProxy = false)
	{
		const auto denseIndex = FindDenseIndex(lookup, primitives, handle);
		if (!denseIndex.has_value() || IsHandleVisible(result, lookup, primitives, handle))
			return;

		const auto& record = primitives.primitiveRecords[*denseIndex];
		const auto reason = forceRepresentationProxy
			? RevalidateRepresentationProxyCandidate(record, options)
			: RevalidateCandidate(record, options);
		if (reason != CullReason::Visible)
			return;

		MarkVisible(result, record, *denseIndex);
	}

	void RebuildSparseOutputs(
		SceneVisibilityPipelineResult& result,
		const ScenePrimitiveSnapshot& primitives)
	{
		NLS_PROFILE_NAMED_SCOPE("SceneVisibilityPipeline::RebuildSparseOutputs");
		result.visiblePrimitiveHandles.clear();
		result.eligibleCommandRanges.clear();
		for (const auto& record : primitives.primitiveRecords)
		{
			if (!IsBitSet(result.primitiveBits, record.handle.index))
				continue;
			result.visiblePrimitiveHandles.push_back(record.handle);
			result.eligibleCommandRanges.push_back({
				record.handle,
				record.commandOffsetBegin,
				record.commandOffsetEnd
			});
		}
		result.usesSparseVisiblePrimitiveHandles = true;
	}

	void ApplyLODSelection(
		SceneVisibilityPipelineResult& result,
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveSnapshot& primitives,
		const SceneRepresentationState& representation,
		const SnapshotLookup& lookup)
	{
		NLS_PROFILE_NAMED_SCOPE("SceneVisibilityPipeline::ApplyLODSelection");
		if (!options.enableLOD || representation.lodGroups == nullptr)
			return;

		result.selectedLOD.assign(primitives.primitiveRecords.size(), ~0u);
		SceneLODViewInput input;
		input.cameraPosition = options.cameraPosition;
		input.lodBias = options.lodBias;

		for (const auto& group : *representation.lodGroups)
		{
			bool hasVisibleMember = false;
			for (const auto& level : group.levels)
			{
				for (const auto& handle : level.primitiveHandles)
					hasVisibleMember |= IsHandleVisible(result, lookup, primitives, handle);
			}
			if (!hasVisibleMember)
				continue;

			LODSelectionHistory* history = nullptr;
			if (representation.lodSelectionHistory != nullptr &&
				group.groupHandle.index < representation.lodSelectionHistory->size())
			{
				history = &(*representation.lodSelectionHistory)[group.groupHandle.index];
			}
			const auto selection = SceneLODSystem::Select(input, group, history);
			for (size_t levelIndex = 0u; levelIndex < group.levels.size(); ++levelIndex)
			{
				const auto& level = group.levels[levelIndex];
				for (const auto& handle : level.primitiveHandles)
				{
					const auto denseIndex = FindDenseIndex(lookup, primitives, handle);
					if (denseIndex.has_value())
						result.selectedLOD[*denseIndex] = selection.selectedLOD;
					if (levelIndex == selection.selectedLOD)
						AddVisibleHandle(result, lookup, primitives, options, handle);
					else
						RemoveVisibleHandle(result, lookup, primitives, handle, CullReason::LODInactive);
				}
			}
		}
	}

	void ApplyHLODSelection(
		SceneVisibilityPipelineResult& result,
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveSnapshot& primitives,
		const SceneRepresentationState& representation,
		const SnapshotLookup& lookup)
	{
		NLS_PROFILE_NAMED_SCOPE("SceneVisibilityPipeline::ApplyHLODSelection");
		if (!options.enableHLOD ||
			representation.hlodClusters == nullptr ||
			representation.residency == nullptr)
		{
			return;
		}

		for (const auto& cluster : *representation.hlodClusters)
		{
			if (cluster.proxyPrimitive.has_value())
				RemoveVisibleHandle(
					result,
					lookup,
					primitives,
					*cluster.proxyPrimitive,
					CullReason::HLODProxyInactive);
		}

		SceneHLODViewInput input;
		input.cameraPosition = options.cameraPosition;
		input.allowHLOD = options.allowHLOD;
		input.editorInspectionView = options.editorInspectionView;
		input.selectedPrimitiveHandles = options.selectedPrimitiveHandles;
		input.forceInspectableHLODClusters = options.forceInspectableHLODClusters;

		for (const auto& cluster : *representation.hlodClusters)
		{
			bool hasVisibleChild = false;
			for (const auto& child : cluster.childPrimitives)
				hasVisibleChild |= IsHandleVisible(result, lookup, primitives, child);
			if (!hasVisibleChild)
				continue;

			const auto selection = SceneHLODSystem::SelectCluster(input, cluster, *representation.residency);
			for (const auto& interest : selection.streamingInterest)
				AddUniqueHandle(result.representationStreamingInterest, interest);
			if (!selection.usesProxy)
				continue;

			AddUniqueCluster(result.activeHLODClusters, cluster.clusterHandle);
			if (selection.proxyPrimitive.has_value())
				AddVisibleHandle(result, lookup, primitives, options, *selection.proxyPrimitive, true);

			for (const auto& child : selection.suppressedChildPrimitives)
			{
				AddUniqueHandle(result.suppressedByHLOD, child);
				RemoveVisibleHandle(result, lookup, primitives, child, CullReason::HLODChildSuppressed);
			}
		}
	}

	void ApplyOcclusionSelection(
		SceneVisibilityPipelineResult& result,
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveSnapshot& primitives,
		const SceneRepresentationState& representation,
		const SnapshotLookup& lookup)
	{
		NLS_PROFILE_NAMED_SCOPE("SceneVisibilityPipeline::ApplyOcclusionSelection");
		if (!options.enableOcclusion ||
			representation.occlusion == nullptr ||
			representation.occlusion->history == nullptr ||
			representation.occlusion->primitiveInputs == nullptr)
		{
			return;
		}

		std::unordered_map<ScenePrimitiveHandle, const SceneOcclusionPrimitiveInput*, HandleHash> inputByHandle;
		inputByHandle.reserve(representation.occlusion->primitiveInputs->size());
		for (const auto& input : *representation.occlusion->primitiveInputs)
			inputByHandle[input.handle] = &input;

		std::vector<SceneOcclusionPrimitiveInput> visibleOcclusionInputs;
		visibleOcclusionInputs.reserve(result.visiblePrimitiveHandles.size());
		for (const auto& handle : result.visiblePrimitiveHandles)
		{
			if (!IsHandleVisible(result, lookup, primitives, handle))
				continue;

			const auto found = inputByHandle.find(handle);
			if (found == inputByHandle.end() || found->second == nullptr)
				continue;
			visibleOcclusionInputs.push_back(*found->second);
		}

		if (visibleOcclusionInputs.empty())
			return;

		const auto occlusionResult = SceneOcclusionSystem::Evaluate(
			representation.occlusion->frameInput,
			visibleOcclusionInputs,
			*representation.occlusion->history);
		result.occlusionTestCount += static_cast<uint64_t>(occlusionResult.primitiveResults.size());

		for (const auto& primitiveResult : occlusionResult.primitiveResults)
		{
			if (!primitiveResult.culledByOcclusion)
				continue;

			++result.occlusionCulledCount;
			AddUniqueHandle(result.occludedPrimitiveHandles, primitiveResult.handle);
			RemoveVisibleHandle(
				result,
				lookup,
				primitives,
				primitiveResult.handle,
				CullReason::Occluded);
		}
	}

	void ApplyRepresentationSelection(
		SceneVisibilityPipelineResult& result,
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveSnapshot& primitives,
		const SceneRepresentationState& representation,
		const SnapshotLookup& lookup)
	{
		ApplyLODSelection(result, options, primitives, representation, lookup);
		ApplyHLODSelection(result, options, primitives, representation, lookup);
		ApplyOcclusionSelection(result, options, primitives, representation, lookup);
		if (options.enableLOD || options.enableHLOD || options.enableOcclusion)
		{
			RebuildSparseOutputs(result, primitives);
		}
		result.representationInputs = result.visiblePrimitiveHandles;
	}

	RangeResult EvaluateFullScanRange(
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveSnapshot& primitives,
		const SnapshotLookup& lookup,
		const size_t begin,
		const size_t end)
	{
		RangeResult range;
		range.begin = begin;
		const auto clampedEnd = std::min(end, primitives.primitiveRecords.size());
		range.cullReasons.assign(clampedEnd - begin, CullReason::SpatialMiss);
		const auto meshBegin = begin < primitives.primitiveRecords.size()
			? primitives.primitiveRecords[begin].commandOffsetBegin
			: 0u;
		const auto meshEnd = clampedEnd > begin
			? primitives.primitiveRecords[clampedEnd - 1u].commandOffsetEnd
			: meshBegin;
		range.meshBits.resize(BitWordCount(static_cast<size_t>(meshEnd - meshBegin)));

		for (size_t denseIndex = begin; denseIndex < clampedEnd; ++denseIndex)
		{
			const auto& record = primitives.primitiveRecords[denseIndex];
			++range.fullScanCandidateCount;
			++range.primitiveRecordsTouched;
			++range.visibilityTestedPrimitiveCount;
			const auto reason = RevalidateCandidate(record, options);
			range.cullReasons[denseIndex - begin] = reason;
			if (reason != CullReason::Visible)
				continue;

			for (uint64_t commandOffset = record.commandOffsetBegin; commandOffset < record.commandOffsetEnd; ++commandOffset)
			{
				SetBit(range.meshBits, static_cast<size_t>(commandOffset - meshBegin));
				++range.visibleMeshCount;
			}
			++range.visiblePrimitiveCount;
			range.visiblePrimitiveHandles.push_back(record.handle);
			range.eligibleCommandRanges.push_back({ record.handle, record.commandOffsetBegin, record.commandOffsetEnd });
		}

		(void)lookup;
		return range;
	}

	SceneVisibilityPipelineResult EvaluateFullScanSerial(
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveSnapshot& primitives,
		const SnapshotLookup& lookup)
	{
		NLS_PROFILE_NAMED_SCOPE("SceneVisibilityPipeline::EvaluateFullScanSerial");
		SceneVisibilityPipelineResult result;
		result.primitiveCount = ResolvePrimitiveCount(lookup);
		result.meshCount = lookup.meshCount;
		result.primitiveBits.resize(BitWordCount(static_cast<size_t>(result.primitiveCount)));
		result.meshBits.resize(BitWordCount(static_cast<size_t>(result.meshCount)));
		result.cullReasons.assign(primitives.primitiveRecords.size(), CullReason::SpatialMiss);

		for (size_t denseIndex = 0u; denseIndex < primitives.primitiveRecords.size(); ++denseIndex)
		{
			const auto& record = primitives.primitiveRecords[denseIndex];
			++result.fullScanCandidateCount;
			++result.primitiveRecordsTouched;
			++result.visibilityTestedPrimitiveCount;
			const auto reason = RevalidateCandidate(record, options);
			result.cullReasons[denseIndex] = reason;
			if (reason != CullReason::Visible)
				continue;

			MarkVisible(result, record, denseIndex);
		}

		return result;
	}

	SceneVisibilityPipelineResult EvaluateFullScanParallel(
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveSnapshot& primitives,
		const SnapshotLookup& lookup)
	{
		NLS_PROFILE_NAMED_SCOPE("SceneVisibilityPipeline::EvaluateFullScanParallel");
		if (primitives.primitiveRecords.empty() || !NLS::Base::Jobs::IsJobSystemInitialized())
			return EvaluateFullScanSerial(options, primitives, lookup);

		const auto primitivesPerTask = std::max<size_t>(1u, options.parallelVisibilityPrimitivesPerTask);
		const auto hardwareThreads = std::max<uint32_t>(1u, NLS::Base::Jobs::GetJobWorkerCount() + 1u);
		const auto desiredTaskCount =
			(primitives.primitiveRecords.size() + primitivesPerTask - 1u) /
			primitivesPerTask;
		const auto maxVisibilityJobs = options.maxVisibilityJobs != 0u
			? options.maxVisibilityJobs
			: hardwareThreads;
		const auto taskLimit = std::max<uint32_t>(1u, std::min(hardwareThreads, maxVisibilityJobs));
		const auto taskCount = std::min<size_t>(taskLimit, desiredTaskCount);
		if (taskCount <= 1u)
			return EvaluateFullScanSerial(options, primitives, lookup);

		const auto rangeBaseSize = primitives.primitiveRecords.size() / taskCount;
		const auto largerRangeCount = primitives.primitiveRecords.size() % taskCount;
		std::vector<RangeResult> rangeResults(taskCount);
		std::vector<std::atomic_bool> rangeCompleted(taskCount);
		for (auto& completed : rangeCompleted)
			completed.store(false, std::memory_order_relaxed);

		struct VisibilityRangeJob final : public NLS::Base::Jobs::IJobParallelFor
		{
			const SceneVisibilityPipelineOptions* options = nullptr;
			const ScenePrimitiveSnapshot* primitives = nullptr;
			const SnapshotLookup* lookup = nullptr;
			std::vector<RangeResult>* rangeResults = nullptr;
			std::vector<std::atomic_bool>* rangeCompleted = nullptr;
			size_t rangeBaseSize = 0u;
			size_t largerRangeCount = 0u;

			void Execute(uint32_t taskIndex)
			{
				const auto rangeIndex = static_cast<size_t>(taskIndex);
				const auto begin =
					rangeIndex * rangeBaseSize + std::min(rangeIndex, largerRangeCount);
				if (begin >= primitives->primitiveRecords.size())
					return;

				const auto end = begin + rangeBaseSize + (rangeIndex < largerRangeCount ? 1u : 0u);
				(*rangeResults)[static_cast<size_t>(taskIndex)] =
					EvaluateFullScanRange(*options, *primitives, *lookup, begin, end);
				(*rangeCompleted)[static_cast<size_t>(taskIndex)].store(true, std::memory_order_release);
			}
		};

		VisibilityRangeJob job;
		job.options = &options;
		job.primitives = &primitives;
		job.lookup = &lookup;
		job.rangeResults = &rangeResults;
		job.rangeCompleted = &rangeCompleted;
		job.rangeBaseSize = rangeBaseSize;
		job.largerRangeCount = largerRangeCount;

		NLS::Base::Jobs::JobParallelForScheduleOptions scheduleOptions;
		scheduleOptions.batchSize = 1u;
		scheduleOptions.debugName = "SceneVisibilityPipeline::Evaluate";
		auto handle = NLS::Base::Jobs::ScheduleParallelFor(
			job,
			static_cast<uint32_t>(taskCount),
			scheduleOptions);
		if (handle.id == 0u)
			return EvaluateFullScanSerial(options, primitives, lookup);

		NLS::Base::Jobs::Complete(handle);
		for (const auto& completed : rangeCompleted)
		{
			if (!completed.load(std::memory_order_acquire))
				return EvaluateFullScanSerial(options, primitives, lookup);
		}

		SceneVisibilityPipelineResult result;
		result.primitiveCount = ResolvePrimitiveCount(lookup);
		result.meshCount = lookup.meshCount;
		result.primitiveBits.resize(BitWordCount(static_cast<size_t>(result.primitiveCount)));
		result.meshBits.resize(BitWordCount(static_cast<size_t>(result.meshCount)));
		result.cullReasons.assign(primitives.primitiveRecords.size(), CullReason::SpatialMiss);
		result.usedParallelEvaluation = true;

		for (const auto& range : rangeResults)
		{
			if (range.begin < result.cullReasons.size())
			{
				const auto cullReasonBegin = range.begin;
				const auto cullReasonEnd = std::min(
					cullReasonBegin + range.cullReasons.size(),
					result.cullReasons.size());
				if (cullReasonEnd > cullReasonBegin)
				{
					std::copy(
						range.cullReasons.begin(),
						range.cullReasons.begin() + static_cast<std::ptrdiff_t>(cullReasonEnd - cullReasonBegin),
						result.cullReasons.begin() + static_cast<std::ptrdiff_t>(cullReasonBegin));
				}
			}
			const auto meshBegin = range.begin < primitives.primitiveRecords.size()
				? primitives.primitiveRecords[range.begin].commandOffsetBegin
				: 0u;
			MergeBitRange(result.meshBits, range.meshBits, static_cast<size_t>(meshBegin));
			result.visiblePrimitiveCount += range.visiblePrimitiveCount;
			result.visibleMeshCount += range.visibleMeshCount;
			result.fullScanCandidateCount += range.fullScanCandidateCount;
			result.primitiveRecordsTouched += range.primitiveRecordsTouched;
			result.visibilityTestedPrimitiveCount += range.visibilityTestedPrimitiveCount;
			result.visiblePrimitiveHandles.insert(
				result.visiblePrimitiveHandles.end(),
				range.visiblePrimitiveHandles.begin(),
				range.visiblePrimitiveHandles.end());
			for (const auto& handle : range.visiblePrimitiveHandles)
				SetBit(result.primitiveBits, handle.index);
			result.eligibleCommandRanges.insert(
				result.eligibleCommandRanges.end(),
				range.eligibleCommandRanges.begin(),
				range.eligibleCommandRanges.end());
		}

		return result;
	}

	float ResolveSpatialQueryRadius(const SceneVisibilityPipelineOptions& options)
	{
		if (options.spatialQueryRadius > 0.0f)
			return options.spatialQueryRadius;
		if (options.frustum == nullptr)
			return 0.0f;

		const auto planeNormal = [](const std::array<float, 4>& plane)
		{
			return Maths::Vector3(plane[0], plane[1], plane[2]);
		};
		const auto intersectPlanes = [&](const std::array<float, 4>& first,
			const std::array<float, 4>& second,
			const std::array<float, 4>& third,
			Maths::Vector3& point)
		{
			const auto n1 = planeNormal(first);
			const auto n2 = planeNormal(second);
			const auto n3 = planeNormal(third);
			const auto n2CrossN3 = Maths::Vector3::Cross(n2, n3);
			const auto denominator = Maths::Vector3::Dot(n1, n2CrossN3);
			if (std::abs(denominator) <= std::numeric_limits<float>::epsilon())
				return false;

			point =
				(n2CrossN3 * -first[3] -
				 Maths::Vector3::Cross(n3, n1) * second[3] -
				 Maths::Vector3::Cross(n1, n2) * third[3]) /
				denominator;
			return true;
		};

		const auto farPlane = options.frustum->GetFarPlane();
		const auto farSignedDistance =
			farPlane[0] * options.cameraPosition.x +
			farPlane[1] * options.cameraPosition.y +
			farPlane[2] * options.cameraPosition.z +
			farPlane[3];
		float queryRadius = std::abs(farSignedDistance);

		const std::array<std::array<float, 4>, 2u> horizontalPlanes = {
			options.frustum->GetLeftPlane(),
			options.frustum->GetRightPlane()
		};
		const std::array<std::array<float, 4>, 2u> verticalPlanes = {
			options.frustum->GetBottomPlane(),
			options.frustum->GetTopPlane()
		};
		for (const auto& horizontalPlane : horizontalPlanes)
		{
			for (const auto& verticalPlane : verticalPlanes)
			{
				Maths::Vector3 corner;
				if (!intersectPlanes(farPlane, horizontalPlane, verticalPlane, corner))
					continue;
				queryRadius = std::max(queryRadius, Maths::Vector3::Distance(options.cameraPosition, corner));
			}
		}

		return queryRadius > 0.0f ? queryRadius : 0.0f;
	}

	SceneVisibilityPipelineResult EvaluateSpatial(
		const SceneVisibilityPipelineOptions& options,
		const ScenePrimitiveSnapshot& primitives,
		const SceneSpatialIndex& spatialIndex,
		const SnapshotLookup& lookup)
	{
		const auto queryRadius = ResolveSpatialQueryRadius(options);
		if (queryRadius <= 0.0f)
			return EvaluateFullScanSerial(options, primitives, lookup);

		SceneSpatialIndexQuery query;
		query.center = options.cameraPosition;
		query.radius = queryRadius;
		query.visibleLayerMask = options.visibleLayerMask;

		const auto candidates = spatialIndex.Query(query);

		SceneVisibilityPipelineResult result;
		result.primitiveCount = ResolvePrimitiveCount(lookup);
		result.meshCount = lookup.meshCount;
		result.primitiveBits.resize(BitWordCount(static_cast<size_t>(result.primitiveCount)));
		result.meshBits.resize(BitWordCount(static_cast<size_t>(result.meshCount)));
		result.cullReasons.assign(primitives.primitiveRecords.size(), CullReason::SpatialMiss);
		result.usesSparseVisiblePrimitiveHandles = true;
		result.spatialCandidateCount = candidates.candidateCount;
		result.fullScanCandidateCount = candidates.fullScanCandidateCount;
		result.primitiveRecordsTouched = candidates.primitiveRecordsTouched;
		result.dynamicCandidateCount = candidates.dynamicCandidateCount;
		result.dynamicRecordsTouched = candidates.dynamicRecordsTouched;
		result.staticIndexRefitCount = candidates.telemetry.staticIndexRefitCount;
		result.staticIndexRebuildCount = candidates.telemetry.staticIndexRebuildCount;
		result.staticIndexLastGoodQueryCount = candidates.telemetry.staticIndexLastGoodQueryCount;
		result.staticIndexDirtyOverlayCount = candidates.telemetry.staticIndexDirtyOverlayCount;
		result.spatialRebuildFallbackCount = candidates.telemetry.spatialRebuildFallbackCount;
		result.dynamicIndexUpdateCount = candidates.telemetry.dynamicIndexUpdateCount;

		for (const auto& handle : candidates.candidatePrimitiveHandles)
		{
			const auto found = lookup.denseIndexByHandle.find(handle);
			if (found == lookup.denseIndexByHandle.end())
				continue;

			const auto denseIndex = found->second;
			const auto& record = primitives.primitiveRecords[denseIndex];
			if (record.handle != handle)
				continue;

			++result.visibilityTestedPrimitiveCount;
			const auto reason = RevalidateCandidate(record, options);
			result.cullReasons[denseIndex] = reason;
			if (reason != CullReason::Visible)
				continue;

			MarkVisible(result, record, denseIndex);
		}

		return result;
	}
}

SceneVisibilityPipelineResult SceneVisibilityPipeline::Evaluate(
	const SceneVisibilityPipelineOptions& options,
	const ScenePrimitiveSnapshot& primitives,
	const SceneSpatialIndex& spatialIndex,
	const SceneVisibilityPipelineMode mode)
{
	return Evaluate(options, primitives, spatialIndex, {}, mode);
}

SceneVisibilityPipelineResult SceneVisibilityPipeline::Evaluate(
	const SceneVisibilityPipelineOptions& options,
	const ScenePrimitiveSnapshot& primitives,
	const SceneSpatialIndex& spatialIndex,
	const SceneRepresentationState& representation,
	const SceneVisibilityPipelineMode mode)
{
	const auto lookup = BuildLookup(primitives);
	SceneVisibilityPipelineResult result;
	if (mode == SceneVisibilityPipelineMode::FullScanComparison)
	{
		result = EvaluateFullScanSerial(options, primitives, lookup);
		ApplyRepresentationSelection(result, options, primitives, representation, lookup);
		return result;
	}

	if (options.enableSpatialIndex)
	{
		result = EvaluateSpatial(options, primitives, spatialIndex, lookup);
		ApplyRepresentationSelection(result, options, primitives, representation, lookup);
		return result;
	}

	if (mode == SceneVisibilityPipelineMode::Parallel)
	{
		result = EvaluateFullScanParallel(options, primitives, lookup);
		ApplyRepresentationSelection(result, options, primitives, representation, lookup);
		return result;
	}
	if (mode == SceneVisibilityPipelineMode::Auto &&
		options.enableParallelVisibility &&
		primitives.primitiveRecords.size() >= options.parallelVisibilityPrimitiveThreshold)
	{
		result = EvaluateFullScanParallel(options, primitives, lookup);
		ApplyRepresentationSelection(result, options, primitives, representation, lookup);
		return result;
	}

	result = EvaluateFullScanSerial(options, primitives, lookup);
	ApplyRepresentationSelection(result, options, primitives, representation, lookup);
	return result;
}

SceneRepresentationCandidateExpansion SceneVisibilityPipeline::ExpandRepresentationCandidates(
	const std::vector<ScenePrimitiveHandle>& candidateHandles,
	const ScenePrimitiveSnapshot& candidatePrimitives,
	const SceneRepresentationState& representation)
{
	SceneRepresentationCandidateExpansion expansion;
	expansion.primitiveHandles = candidateHandles;
	std::unordered_set<ScenePrimitiveHandle, HandleHash> primitiveSet(
		candidateHandles.begin(),
		candidateHandles.end());
	std::unordered_set<uint32_t> lodGroupHandles;
	std::unordered_set<uint32_t> hlodClusterHandles;
	const auto addPrimitive = [&expansion, &primitiveSet](const ScenePrimitiveHandle handle)
	{
		if (primitiveSet.insert(handle).second)
			expansion.primitiveHandles.push_back(handle);
	};

	auto addLODGroup = [&](const uint32_t groupIndex)
	{
		if (representation.lodGroups == nullptr ||
			groupIndex >= representation.lodGroups->size() ||
			!lodGroupHandles.insert(groupIndex).second)
		{
			return;
		}
		expansion.lodGroupIndices.push_back(groupIndex);
		const auto& group = (*representation.lodGroups)[groupIndex];
		for (const auto& level : group.levels)
		{
			for (const auto handle : level.primitiveHandles)
				addPrimitive(handle);
		}
	};
	auto addHLODCluster = [&](const uint32_t clusterIndex)
	{
		if (representation.hlodClusters == nullptr ||
			clusterIndex >= representation.hlodClusters->size() ||
			!hlodClusterHandles.insert(clusterIndex).second)
		{
			return;
		}
		expansion.hlodClusterIndices.push_back(clusterIndex);
		const auto& cluster = (*representation.hlodClusters)[clusterIndex];
		for (const auto child : cluster.childPrimitives)
			addPrimitive(child);
		if (cluster.proxyPrimitive.has_value())
			addPrimitive(*cluster.proxyPrimitive);
	};

	if (representation.lodGroupsByPrimitive != nullptr)
	{
		for (const auto& record : candidatePrimitives.primitiveRecords)
		{
			const auto groupsIt = representation.lodGroupsByPrimitive->find(record.handle);
			if (groupsIt == representation.lodGroupsByPrimitive->end())
				continue;
			for (const auto groupIndex : groupsIt->second)
				addLODGroup(groupIndex);
		}
	}
	else if (representation.lodGroups != nullptr)
	{
		std::unordered_map<uint32_t, size_t> lodGroupIndexByHandle;
		lodGroupIndexByHandle.reserve(representation.lodGroups->size());
		for (size_t groupIndex = 0u; groupIndex < representation.lodGroups->size(); ++groupIndex)
		{
			const auto& group = (*representation.lodGroups)[groupIndex];
			if (group.groupHandle.IsValid())
				lodGroupIndexByHandle.emplace(group.groupHandle.index, groupIndex);
		}
		for (const auto& record : candidatePrimitives.primitiveRecords)
		{
			if (!record.lodGroup.has_value())
				continue;
			const auto groupIndexIt = lodGroupIndexByHandle.find(record.lodGroup->index);
			if (groupIndexIt != lodGroupIndexByHandle.end())
				addLODGroup(static_cast<uint32_t>(groupIndexIt->second));
		}
	}

	if (representation.hlodClustersByPrimitive != nullptr)
	{
		for (const auto& record : candidatePrimitives.primitiveRecords)
		{
			const auto clustersIt = representation.hlodClustersByPrimitive->find(record.handle);
			if (clustersIt == representation.hlodClustersByPrimitive->end())
				continue;
			for (const auto clusterIndex : clustersIt->second)
				addHLODCluster(clusterIndex);
		}
	}
	else if (representation.hlodClusters != nullptr)
	{
		std::unordered_map<uint32_t, size_t> hlodClusterIndexByHandle;
		hlodClusterIndexByHandle.reserve(representation.hlodClusters->size());
		for (size_t clusterIndex = 0u; clusterIndex < representation.hlodClusters->size(); ++clusterIndex)
		{
			const auto& cluster = (*representation.hlodClusters)[clusterIndex];
			if (cluster.clusterHandle.IsValid())
				hlodClusterIndexByHandle.emplace(cluster.clusterHandle.index, clusterIndex);
		}
		for (const auto& record : candidatePrimitives.primitiveRecords)
		{
			if (!record.hlodCluster.has_value())
				continue;
			const auto clusterIndexIt = hlodClusterIndexByHandle.find(record.hlodCluster->index);
			if (clusterIndexIt != hlodClusterIndexByHandle.end())
				addHLODCluster(static_cast<uint32_t>(clusterIndexIt->second));
		}
	}

	return expansion;
}

StreamingResidencyPlanInput SceneVisibilityPipeline::BuildStreamingResidencyInput(
	const uint64_t frameSerial,
	const SceneVisibilityPipelineResult& visibility)
{
	StreamingResidencyPlanInput input;
	input.frameSerial = frameSerial;
	input.visiblePrimitiveHandles = visibility.visiblePrimitiveHandles;
	for (const auto handle : visibility.representationStreamingInterest)
		AddUniqueHandle(input.representationStreamingInterest, handle);
	return input;
}

SceneCullReasonDebugSnapshot SceneVisibilityPipeline::BuildCullReasonDebugSnapshot(
	const ScenePrimitiveSnapshot& primitives,
	const SceneVisibilityPipelineResult& visibility,
	const uint64_t maxEntries)
{
	SceneCullReasonDebugSnapshot snapshot;
	snapshot.frameSerial = primitives.frameSerial;
	snapshot.sceneId = primitives.sceneId;
	snapshot.primitiveCount = static_cast<uint64_t>(primitives.primitiveRecords.size());
	snapshot.visiblePrimitiveCount = visibility.visiblePrimitiveCount;
	snapshot.entries.reserve(static_cast<size_t>(std::min<uint64_t>(
		primitives.primitiveRecords.size(),
		maxEntries)));

	for (size_t denseIndex = 0u; denseIndex < primitives.primitiveRecords.size(); ++denseIndex)
	{
		const auto& primitive = primitives.primitiveRecords[denseIndex];
		const auto reason = denseIndex < visibility.cullReasons.size()
			? visibility.cullReasons[denseIndex]
			: CullReason::BackendUnsupported;
		const auto reasonIndex = static_cast<size_t>(reason);
		if (reasonIndex < snapshot.reasonCounts.size())
			++snapshot.reasonCounts[reasonIndex];
		if (snapshot.entries.size() >= maxEntries)
			continue;

		SceneCullReasonDebugEntry entry;
		entry.handle = primitive.handle;
		entry.reason = reason;
		entry.selectedLOD = denseIndex < visibility.selectedLOD.size()
			? visibility.selectedLOD[denseIndex]
			: 0u;
		entry.commandOffsetBegin = primitive.commandOffsetBegin;
		entry.commandOffsetEnd = primitive.commandOffsetEnd;
		entry.visible = reason == CullReason::Visible;
		snapshot.entries.push_back(entry);
	}

	return snapshot;
}

const std::array<SceneCullReasonDisplayBucket, 13u>&
SceneVisibilityPipeline::GetCullReasonDisplayBuckets()
{
	return kCullReasonDisplayBuckets;
}
}
