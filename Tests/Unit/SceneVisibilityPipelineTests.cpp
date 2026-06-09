#include <algorithm>
#include <array>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "Jobs/JobSystem.h"
#include "Math/Matrix4.h"
#include "Rendering/Data/Frustum.h"
#include "Rendering/SceneHLOD.h"
#include "Rendering/SceneLOD.h"
#include "Rendering/SceneOcclusion.h"
#include "Rendering/SceneSpatialIndex.h"
#include "Rendering/SceneStreamingResidency.h"
#include "Rendering/SceneVisibilityPipeline.h"

namespace
{
	using NLS::Engine::Rendering::CullReason;
	using NLS::Engine::Rendering::HLODClusterRecord;
	using NLS::Engine::Rendering::HLODCompatibilityFlags;
	using NLS::Engine::Rendering::LODGroupRecord;
	using NLS::Engine::Rendering::LODLevelRecord;
	using NLS::Engine::Rendering::RepresentationResidencySnapshot;
	using NLS::Engine::Rendering::SceneHLODClusterHandle;
	using NLS::Engine::Rendering::SceneOcclusionFrameInput;
	using NLS::Engine::Rendering::SceneOcclusionHistory;
	using NLS::Engine::Rendering::SceneOcclusionPrimitiveInput;
	using NLS::Engine::Rendering::SceneOcclusionState;
	using NLS::Engine::Rendering::SceneOcclusionSystem;
	using NLS::Engine::Rendering::SceneCullReasonDebugEntry;
	using NLS::Engine::Rendering::SceneCullReasonDebugSnapshot;
	using NLS::Engine::Rendering::ScenePrimitiveCommandOffsetRange;
	using NLS::Engine::Rendering::ScenePrimitiveHandle;
	using NLS::Engine::Rendering::ScenePrimitiveHandleStableHash;
	using NLS::Engine::Rendering::ScenePrimitiveSnapshot;
	using NLS::Engine::Rendering::ScenePrimitiveSnapshotRecord;
	using NLS::Engine::Rendering::SceneRepresentationState;
	using NLS::Engine::Rendering::SceneSpatialIndex;
	using NLS::Engine::Rendering::SceneSpatialIndexPrimitiveClass;
	using NLS::Engine::Rendering::SceneSpatialIndexPrimitiveMetadata;
	using NLS::Engine::Rendering::SceneVisibilityPipeline;
	using NLS::Engine::Rendering::SceneVisibilityPipelineMode;
	using NLS::Engine::Rendering::SceneVisibilityPipelineOptions;
	using NLS::Engine::Rendering::StreamingResidencyPlanInput;

	constexpr uint64_t kSceneId = 0x71u;

	ScenePrimitiveHandle MakeHandle(const uint32_t index, const uint32_t generation = 1u)
	{
		return { kSceneId, index, generation };
	}

	NLS::Render::Data::Frustum CreateForwardFrustum()
	{
		NLS::Render::Data::Frustum frustum;
		const auto view = NLS::Maths::Matrix4::CreateView(
			0.0f, 0.0f, 0.0f,
			0.0f, 0.0f, -1.0f,
			0.0f, 1.0f, 0.0f);
		const auto projection = NLS::Maths::Matrix4::CreatePerspective(90.0f, 1.0f, 0.1f, 100.0f);
		frustum.CalculateFrustum(projection * view);
		return frustum;
	}

	ScenePrimitiveSnapshot BuildSnapshot(const size_t primitiveCount)
	{
		ScenePrimitiveSnapshot snapshot;
		snapshot.sceneId = kSceneId;
		snapshot.snapshotSerial = 1u;
		snapshot.frameSerial = 1u;
		snapshot.memoryArenaSerial = 1u;
		snapshot.primitiveRecords.reserve(primitiveCount);
		snapshot.handleToDenseIndex.reserve(primitiveCount);
		snapshot.denseIndexToHandle.reserve(primitiveCount);
		snapshot.commandOffsetTable.reserve(primitiveCount);

		for (size_t index = 0u; index < primitiveCount; ++index)
		{
			const auto gridX = static_cast<float>(index % 32u) * 12.0f;
			const auto gridZ = -10.0f - static_cast<float>(index / 32u) * 12.0f;

			ScenePrimitiveSnapshotRecord record;
			record.handle = MakeHandle(static_cast<uint32_t>(index));
			record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(index + 1u));
			record.modelBoundingSphere.position = { 0.0f, 0.0f, 0.0f };
			record.modelBoundingSphere.radius = 1.0f;
			record.modelBounds.center = { 0.0f, 0.0f, 0.0f };
			record.modelBounds.size = { 2.0f, 2.0f, 2.0f };
			record.worldMatrix = NLS::Maths::Matrix4::Translation({ gridX, 0.0f, gridZ });
			record.frustumBehaviour =
				NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL;
			record.visibilitySettings.layer = static_cast<uint32_t>(index % 4u);
			record.commandOffsetBegin = index;
			record.commandOffsetEnd = index + 1u;
			record.ownerAlive = true;
			record.ownerActive = true;
			record.occupied = true;
			record.tombstoned = false;
			record.hasMeshBinding = true;
			record.hasValidMaterial = true;

			if (index % 17u == 0u)
				record.ownerActive = false;
			if (index % 19u == 0u)
				record.visibilitySettings.distanceCullingEnabled = true;
			if (index % 19u == 0u)
				record.visibilitySettings.maxDrawDistance = 8.0f;
			if (index % 23u == 0u)
				record.visibilitySettings.layer = 7u;

			const auto denseIndex = static_cast<uint64_t>(snapshot.primitiveRecords.size());
			snapshot.primitiveRecords.push_back(record);
			snapshot.handleToDenseIndex.push_back({ record.handle, denseIndex });
			snapshot.denseIndexToHandle.push_back(record.handle);
			snapshot.commandOffsetTable.push_back({ record.handle, record.commandOffsetBegin, record.commandOffsetEnd });
		}

		return snapshot;
	}

	ScenePrimitiveSnapshot BuildSparseHandleSnapshot(const size_t primitiveCount)
	{
		auto snapshot = BuildSnapshot(primitiveCount);
		for (size_t denseIndex = 0u; denseIndex < snapshot.primitiveRecords.size(); ++denseIndex)
		{
			auto& record = snapshot.primitiveRecords[denseIndex];
			record.handle = MakeHandle(static_cast<uint32_t>(denseIndex * 3u + 2u));
			snapshot.handleToDenseIndex[denseIndex].handle = record.handle;
			snapshot.denseIndexToHandle[denseIndex] = record.handle;
			snapshot.commandOffsetTable[denseIndex].handle = record.handle;
		}
		return snapshot;
	}

	class ScopedVisibilityPipelineJobSystem
	{
	public:
		explicit ScopedVisibilityPipelineJobSystem(const uint32_t workerCount)
		{
			if (NLS::Base::Jobs::IsJobSystemInitialized())
				return;

			NLS::Base::Jobs::JobSystemConfig config;
			config.workerCount = workerCount;
			m_ownsRuntime = NLS::Base::Jobs::TryInitializeJobSystem(config);
		}

		~ScopedVisibilityPipelineJobSystem()
		{
			if (!m_ownsRuntime)
				return;

			NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::Immediate);
#if defined(NLS_ENABLE_TEST_HOOKS)
			NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
		}

	private:
		bool m_ownsRuntime = false;
	};

	std::vector<SceneSpatialIndexPrimitiveMetadata> BuildMetadata(
		const ScenePrimitiveSnapshot& snapshot)
	{
		std::vector<SceneSpatialIndexPrimitiveMetadata> metadata;
		metadata.reserve(snapshot.primitiveRecords.size());
		for (const auto& record : snapshot.primitiveRecords)
		{
			metadata.push_back({
				record.handle,
				record.handle.index % 5u == 0u
					? SceneSpatialIndexPrimitiveClass::Dynamic
					: SceneSpatialIndexPrimitiveClass::Static,
				record.ownerAlive && record.ownerActive
			});
		}
		return metadata;
	}

	bool IsBitSet(const std::vector<uint64_t>& words, const size_t index)
	{
		constexpr size_t kBitsPerWord = sizeof(uint64_t) * 8u;
		if (index >= words.size() * kBitsPerWord)
			return false;
		return (words[index / kBitsPerWord] & (1ull << (index % kBitsPerWord))) != 0u;
	}
}

TEST(SceneVisibilityPipelineTests, SpatialCandidatesMatchFullScanAndKeepVisibilityTouchedBounded)
{
	const auto snapshot = BuildSnapshot(1024u);
	SceneSpatialIndex spatialIndex;
	spatialIndex.Update(snapshot, BuildMetadata(snapshot));

	auto frustum = CreateForwardFrustum();
	SceneVisibilityPipelineOptions options;
	options.frustum = &frustum;
	options.cameraPosition = { 0.0f, 0.0f, 0.0f };
	options.visibleLayerMask = 1u << 1u;
	options.enableSpatialIndex = true;

	const auto spatial = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::Serial);
	const auto fullScan = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::FullScanComparison);

	EXPECT_EQ(spatial.primitiveBits, fullScan.primitiveBits);
	EXPECT_EQ(spatial.meshBits, fullScan.meshBits);
	EXPECT_EQ(spatial.visiblePrimitiveHandles, fullScan.visiblePrimitiveHandles);
	EXPECT_LT(spatial.visibilityTestedPrimitiveCount, fullScan.visibilityTestedPrimitiveCount);
	EXPECT_LT(spatial.primitiveRecordsTouched, fullScan.primitiveRecordsTouched);
	EXPECT_EQ(spatial.fullScanCandidateCount, 0u);
	EXPECT_GT(spatial.spatialCandidateCount, 0u);
	EXPECT_TRUE(spatial.usesSparseVisiblePrimitiveHandles);
	EXPECT_EQ(spatial.cullReasons.size(), snapshot.primitiveRecords.size());

	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		if (IsBitSet(spatial.primitiveBits, index))
			EXPECT_EQ(spatial.cullReasons[index], CullReason::Visible);
	}
}

TEST(SceneVisibilityPipelineTests, FrustumCullingUsesModelAABBInsteadOfBoundingSphere)
{
	ScenePrimitiveSnapshot snapshot;
	snapshot.sceneId = kSceneId;
	snapshot.snapshotSerial = 1u;

	ScenePrimitiveSnapshotRecord record;
	record.handle = MakeHandle(0u);
	record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(1u));
	record.modelBoundingSphere.position = { -12.0f, 0.0f, 0.0f };
	record.modelBoundingSphere.radius = 16.0f;
	record.modelBounds.center = { -12.0f, 0.0f, -10.0f };
	record.modelBounds.size = { 0.5f, 0.5f, 0.5f };
	record.worldMatrix = NLS::Maths::Matrix4::Identity;
	record.frustumBehaviour =
		NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL;
	record.visibilitySettings.layer = 0u;
	record.commandOffsetBegin = 0u;
	record.commandOffsetEnd = 1u;
	record.ownerAlive = true;
	record.ownerActive = true;
	record.occupied = true;
	record.tombstoned = false;
	record.hasMeshBinding = true;
	record.hasValidMaterial = true;
	snapshot.primitiveRecords.push_back(record);
	snapshot.handleToDenseIndex.push_back({ record.handle, 0u });
	snapshot.denseIndexToHandle.push_back(record.handle);
	snapshot.commandOffsetTable.push_back({ record.handle, 0u, 1u });

	auto frustum = CreateForwardFrustum();
	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;
	options.frustum = &frustum;
	options.cameraPosition = {};
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableSpatialIndex = false;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::Serial);

	EXPECT_TRUE(result.visiblePrimitiveHandles.empty());
	ASSERT_EQ(result.cullReasons.size(), 1u);
	EXPECT_EQ(result.cullReasons.front(), CullReason::FrustumCulled);
}

TEST(SceneVisibilityPipelineTests, FrustumCullingKeepsAABBTouchingPlaneVisible)
{
	ScenePrimitiveSnapshot snapshot;
	snapshot.sceneId = kSceneId;
	snapshot.snapshotSerial = 1u;

	ScenePrimitiveSnapshotRecord record;
	record.handle = MakeHandle(0u);
	record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(1u));
	record.modelBoundingSphere.position = { -10.0f, 0.0f, 0.0f };
	record.modelBoundingSphere.radius = 1.0f;
	record.modelBounds.center = { -10.5f, 0.0f, -10.0f };
	record.modelBounds.size = { 1.0f, 1.0f, 1.0f };
	record.worldMatrix = NLS::Maths::Matrix4::Identity;
	record.frustumBehaviour =
		NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL;
	record.visibilitySettings.layer = 0u;
	record.commandOffsetBegin = 0u;
	record.commandOffsetEnd = 1u;
	record.ownerAlive = true;
	record.ownerActive = true;
	record.occupied = true;
	record.tombstoned = false;
	record.hasMeshBinding = true;
	record.hasValidMaterial = true;
	snapshot.primitiveRecords.push_back(record);
	snapshot.handleToDenseIndex.push_back({ record.handle, 0u });
	snapshot.denseIndexToHandle.push_back(record.handle);
	snapshot.commandOffsetTable.push_back({ record.handle, 0u, 1u });

	auto frustum = CreateForwardFrustum();
	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;
	options.frustum = &frustum;
	options.cameraPosition = {};
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableSpatialIndex = false;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::Serial);

	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), record.handle);
	ASSERT_EQ(result.cullReasons.size(), 1u);
	EXPECT_EQ(result.cullReasons.front(), CullReason::Visible);
}

TEST(SceneVisibilityPipelineTests, CullReasonDebugSnapshotUsesPrimitiveSnapshotAndContainsNoLivePointers)
{
	static_assert(!std::is_pointer_v<decltype(SceneCullReasonDebugEntry{}.handle)>);
	static_assert(!std::is_pointer_v<decltype(SceneCullReasonDebugEntry{}.reason)>);

	auto snapshot = BuildSnapshot(4u);
	snapshot.frameSerial = 37u;
	snapshot.sceneId = kSceneId;

	SceneVisibilityPipelineOptions options;
	options.visibleLayerMask = 1u << 1u;
	SceneSpatialIndex spatialIndex;
	const auto visibility = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::FullScanComparison);

	const SceneCullReasonDebugSnapshot debugSnapshot =
		SceneVisibilityPipeline::BuildCullReasonDebugSnapshot(snapshot, visibility);

	EXPECT_EQ(debugSnapshot.frameSerial, snapshot.frameSerial);
	EXPECT_EQ(debugSnapshot.sceneId, kSceneId);
	EXPECT_EQ(debugSnapshot.primitiveCount, snapshot.primitiveRecords.size());
	EXPECT_EQ(debugSnapshot.visiblePrimitiveCount, visibility.visiblePrimitiveCount);
	ASSERT_EQ(debugSnapshot.entries.size(), snapshot.primitiveRecords.size());
	EXPECT_EQ(debugSnapshot.entries[0].handle, snapshot.primitiveRecords[0].handle);
	EXPECT_EQ(debugSnapshot.entries[0].commandOffsetBegin, snapshot.primitiveRecords[0].commandOffsetBegin);
	EXPECT_EQ(debugSnapshot.entries[0].commandOffsetEnd, snapshot.primitiveRecords[0].commandOffsetEnd);
	EXPECT_EQ(debugSnapshot.entries[0].reason, visibility.cullReasons[0]);
	EXPECT_EQ(
		debugSnapshot.reasonCounts[static_cast<size_t>(CullReason::Visible)],
		visibility.visiblePrimitiveCount);
	EXPECT_EQ(
		debugSnapshot.reasonCounts[static_cast<size_t>(CullReason::LayerMasked)],
		2u);
}

TEST(SceneVisibilityPipelineTests, CullReasonDisplayBucketsCoverEnumReasonsAndMergeHLOD)
{
	std::array<uint32_t, NLS::Engine::Rendering::kSceneVisibilityCullReasonCount> coverage {};
	size_t hlodBucketCount = 0u;
	for (const auto& bucket : SceneVisibilityPipeline::GetCullReasonDisplayBuckets())
	{
		ASSERT_NE(bucket.label, nullptr);
		ASSERT_GT(bucket.reasonCount, 0u);
		ASSERT_LE(bucket.reasonCount, bucket.reasons.size());

		bool bucketContainsHLOD = false;
		for (size_t reasonIndex = 0u; reasonIndex < bucket.reasonCount; ++reasonIndex)
		{
			const auto reason = bucket.reasons[reasonIndex];
			const auto denseReason = static_cast<size_t>(reason);
			ASSERT_LT(denseReason, coverage.size());
			++coverage[denseReason];
			bucketContainsHLOD = bucketContainsHLOD ||
				reason == CullReason::HLODChildSuppressed ||
				reason == CullReason::HLODProxyInactive;
		}
		if (bucketContainsHLOD)
		{
			++hlodBucketCount;
			EXPECT_STREQ(bucket.label, "HLOD");
			EXPECT_EQ(bucket.reasonCount, 2u);
		}
	}

	for (size_t reasonIndex = 0u; reasonIndex < coverage.size(); ++reasonIndex)
		EXPECT_EQ(coverage[reasonIndex], 1u) << reasonIndex;
	EXPECT_EQ(hlodBucketCount, 1u);
}

TEST(SceneVisibilityPipelineTests, AutoFallsBackToSerialWhenJobSystemIsUnavailable)
{
	if (NLS::Base::Jobs::IsJobSystemInitialized())
		GTEST_SKIP() << "JobSystem already initialized by the process; fallback is covered when unavailable.";

	const auto snapshot = BuildSnapshot(192u);
	SceneSpatialIndex spatialIndex;
	spatialIndex.Update(snapshot, BuildMetadata(snapshot));

	auto frustum = CreateForwardFrustum();
	SceneVisibilityPipelineOptions options;
	options.frustum = &frustum;
	options.cameraPosition = { 0.0f, 0.0f, 0.0f };
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableSpatialIndex = false;
	options.enableParallelVisibility = true;
	options.parallelVisibilityPrimitiveThreshold = 1u;
	options.parallelVisibilityPrimitivesPerTask = 16u;

	const auto serial = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::Serial);
	const auto automatic = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::Auto);

	EXPECT_FALSE(automatic.usedParallelEvaluation);
	EXPECT_EQ(automatic.primitiveBits, serial.primitiveBits);
	EXPECT_EQ(automatic.meshBits, serial.meshBits);
	EXPECT_EQ(automatic.visiblePrimitiveHandles, serial.visiblePrimitiveHandles);
	EXPECT_EQ(automatic.fullScanCandidateCount, serial.fullScanCandidateCount);
}

TEST(SceneVisibilityPipelineTests, ParallelAndFullScanComparisonPreserveSparseHandleBits)
{
	ScopedVisibilityPipelineJobSystem jobSystem(2u);
	ASSERT_TRUE(NLS::Base::Jobs::IsJobSystemInitialized());

	const auto snapshot = BuildSparseHandleSnapshot(256u);
	SceneSpatialIndex spatialIndex;
	spatialIndex.Update(snapshot, BuildMetadata(snapshot));

	auto frustum = CreateForwardFrustum();
	SceneVisibilityPipelineOptions options;
	options.frustum = &frustum;
	options.cameraPosition = { 0.0f, 0.0f, 0.0f };
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableSpatialIndex = false;
	options.enableParallelVisibility = true;
	options.parallelVisibilityPrimitiveThreshold = 1u;
	options.parallelVisibilityPrimitivesPerTask = 16u;

	const auto serial = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::Serial);
	const auto parallel = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::Parallel);
	const auto fullScan = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::FullScanComparison);

	EXPECT_TRUE(parallel.usedParallelEvaluation);
	EXPECT_EQ(parallel.primitiveBits, serial.primitiveBits);
	EXPECT_EQ(parallel.meshBits, serial.meshBits);
	EXPECT_EQ(parallel.visiblePrimitiveHandles, serial.visiblePrimitiveHandles);
	EXPECT_EQ(fullScan.primitiveBits, serial.primitiveBits);
	EXPECT_EQ(fullScan.meshBits, serial.meshBits);
	EXPECT_EQ(fullScan.visiblePrimitiveHandles, serial.visiblePrimitiveHandles);
}

TEST(SceneVisibilityPipelineTests, EmptySnapshotReportsZeroPrimitiveCount)
{
	ScenePrimitiveSnapshot snapshot;
	snapshot.sceneId = kSceneId;
	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		SceneVisibilityPipelineMode::Serial);

	EXPECT_EQ(result.primitiveCount, 0u);
	EXPECT_TRUE(result.primitiveBits.empty());
	EXPECT_TRUE(result.visiblePrimitiveHandles.empty());
}

TEST(SceneVisibilityPipelineTests, HLODVisibilityMembershipRequiresExactHandle)
{
	auto snapshot = BuildSnapshot(2u);
	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		auto& record = snapshot.primitiveRecords[index];
		record.handle = MakeHandle(static_cast<uint32_t>(index), 7u);
		record.worldMatrix = NLS::Maths::Matrix4::Translation({ 0.0f, 0.0f, -10.0f });
		record.visibilitySettings.layer = 0u;
		record.visibilitySettings.distanceCullingEnabled = false;
		record.ownerAlive = true;
		record.ownerActive = true;
		record.occupied = true;
		record.tombstoned = false;
		snapshot.handleToDenseIndex[index] = { record.handle, static_cast<uint64_t>(index) };
		snapshot.denseIndexToHandle[index] = record.handle;
		snapshot.commandOffsetTable[index].handle = record.handle;
	}

	HLODClusterRecord cluster;
	cluster.clusterHandle = { 0u };
	cluster.childPrimitives = { MakeHandle(0u, 1u) };
	cluster.proxyPrimitive = snapshot.primitiveRecords[1].handle;
	cluster.worldReferencePoint = { 0.0f, 0.0f, -10.0f };
	cluster.worldSize = 1.0f;
	cluster.activationScreenRelativeSize = 0.50f;
	cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;
	const std::vector<HLODClusterRecord> clusters { cluster };

	RepresentationResidencySnapshot residency;
	residency.MarkReady(snapshot.primitiveRecords[0].handle);
	residency.MarkHLODProxyReady(snapshot.primitiveRecords[1].handle);

	SceneRepresentationState representation;
	representation.hlodClusters = &clusters;
	representation.residency = &residency;

	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;
	options.cameraPosition = {};
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableHLOD = true;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		representation,
		SceneVisibilityPipelineMode::Serial);

	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), snapshot.primitiveRecords[0].handle);
	EXPECT_EQ(result.cullReasons[0], CullReason::Visible);
	EXPECT_EQ(result.cullReasons[1], CullReason::HLODProxyInactive);
	EXPECT_TRUE(result.activeHLODClusters.empty());
	EXPECT_TRUE(result.suppressedByHLOD.empty());
}

TEST(SceneVisibilityPipelineTests, RepresentationCandidateExpansionUsesPipelineMetadata)
{
	auto snapshot = BuildSnapshot(2u);
	snapshot.primitiveRecords[0].handle = MakeHandle(10u);
	snapshot.primitiveRecords[1].handle = MakeHandle(20u);
	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		auto& record = snapshot.primitiveRecords[index];
		snapshot.handleToDenseIndex[index] = { record.handle, static_cast<uint64_t>(index) };
		snapshot.denseIndexToHandle[index] = record.handle;
		snapshot.commandOffsetTable[index].handle = record.handle;
	}

	LODGroupRecord lodGroup;
	lodGroup.groupHandle = { 3u };
	lodGroup.levels = {
		LODLevelRecord { 0.50f, { MakeHandle(10u) } },
		LODLevelRecord { 0.00f, { MakeHandle(11u) } }
	};
	snapshot.primitiveRecords[0].lodGroup = lodGroup.groupHandle;

	HLODClusterRecord cluster;
	cluster.clusterHandle = { 5u };
	cluster.childPrimitives = { MakeHandle(20u), MakeHandle(21u) };
	cluster.proxyPrimitive = MakeHandle(22u);
	cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;
	snapshot.primitiveRecords[1].hlodCluster = cluster.clusterHandle;

	const std::vector<LODGroupRecord> lodGroups { lodGroup };
	const std::vector<HLODClusterRecord> hlodClusters { cluster };
	SceneRepresentationState representation;
	representation.lodGroups = &lodGroups;
	representation.hlodClusters = &hlodClusters;

	const auto expansion = SceneVisibilityPipeline::ExpandRepresentationCandidates(
		{ MakeHandle(10u), MakeHandle(20u) },
		snapshot,
		representation);

	EXPECT_NE(
		std::find(expansion.primitiveHandles.begin(), expansion.primitiveHandles.end(), MakeHandle(11u)),
		expansion.primitiveHandles.end());
	EXPECT_NE(
		std::find(expansion.primitiveHandles.begin(), expansion.primitiveHandles.end(), MakeHandle(21u)),
		expansion.primitiveHandles.end());
	EXPECT_NE(
		std::find(expansion.primitiveHandles.begin(), expansion.primitiveHandles.end(), MakeHandle(22u)),
		expansion.primitiveHandles.end());
	ASSERT_EQ(expansion.lodGroupIndices.size(), 1u);
	EXPECT_EQ(expansion.lodGroupIndices.front(), 0u);
	EXPECT_EQ(lodGroups[expansion.lodGroupIndices.front()].groupHandle, lodGroup.groupHandle);
	ASSERT_EQ(expansion.hlodClusterIndices.size(), 1u);
	EXPECT_EQ(expansion.hlodClusterIndices.front(), 0u);
	EXPECT_EQ(hlodClusters[expansion.hlodClusterIndices.front()].clusterHandle, cluster.clusterHandle);
}

TEST(SceneVisibilityPipelineTests, RepresentationCandidateExpansionUsesPrimitiveAdjacencyIndexes)
{
	auto snapshot = BuildSnapshot(1u);
	snapshot.primitiveRecords[0].handle = MakeHandle(10u);
	snapshot.handleToDenseIndex[0] = { snapshot.primitiveRecords[0].handle, 0u };
	snapshot.denseIndexToHandle[0] = snapshot.primitiveRecords[0].handle;
	snapshot.commandOffsetTable[0].handle = snapshot.primitiveRecords[0].handle;

	std::vector<LODGroupRecord> lodGroups(8u);
	lodGroups[7].groupHandle = { 7u };
	lodGroups[7].levels = {
		LODLevelRecord { 0.50f, { MakeHandle(10u) } },
		LODLevelRecord { 0.00f, { MakeHandle(11u) } }
	};

	std::vector<HLODClusterRecord> hlodClusters(9u);
	hlodClusters[8].clusterHandle = { 8u };
	hlodClusters[8].childPrimitives = { MakeHandle(10u), MakeHandle(12u) };
	hlodClusters[8].proxyPrimitive = MakeHandle(13u);
	hlodClusters[8].compatibilityFlags =
		HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;

	std::unordered_map<ScenePrimitiveHandle, std::vector<uint32_t>, ScenePrimitiveHandleStableHash>
		lodGroupsByPrimitive;
	std::unordered_map<ScenePrimitiveHandle, std::vector<uint32_t>, ScenePrimitiveHandleStableHash>
		hlodClustersByPrimitive;
	lodGroupsByPrimitive[snapshot.primitiveRecords[0].handle] = { 7u };
	hlodClustersByPrimitive[snapshot.primitiveRecords[0].handle] = { 8u };

	SceneRepresentationState representation;
	representation.lodGroups = &lodGroups;
	representation.hlodClusters = &hlodClusters;
	representation.lodGroupsByPrimitive = &lodGroupsByPrimitive;
	representation.hlodClustersByPrimitive = &hlodClustersByPrimitive;

	const auto expansion = SceneVisibilityPipeline::ExpandRepresentationCandidates(
		{ snapshot.primitiveRecords[0].handle },
		snapshot,
		representation);

	ASSERT_EQ(expansion.lodGroupIndices.size(), 1u);
	EXPECT_EQ(expansion.lodGroupIndices.front(), 7u);
	ASSERT_EQ(expansion.hlodClusterIndices.size(), 1u);
	EXPECT_EQ(expansion.hlodClusterIndices.front(), 8u);
	EXPECT_NE(
		std::find(expansion.primitiveHandles.begin(), expansion.primitiveHandles.end(), MakeHandle(11u)),
		expansion.primitiveHandles.end());
	EXPECT_NE(
		std::find(expansion.primitiveHandles.begin(), expansion.primitiveHandles.end(), MakeHandle(12u)),
		expansion.primitiveHandles.end());
	EXPECT_NE(
		std::find(expansion.primitiveHandles.begin(), expansion.primitiveHandles.end(), MakeHandle(13u)),
		expansion.primitiveHandles.end());
}

TEST(SceneVisibilityPipelineTests, LODSelectionSuppressesInactiveLevelsBeforeCommandEligibility)
{
	auto snapshot = BuildSnapshot(3u);
	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		auto& record = snapshot.primitiveRecords[index];
		record.handle = MakeHandle(static_cast<uint32_t>(index));
		record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(index + 1u));
		record.worldMatrix = NLS::Maths::Matrix4::Translation({ 0.0f, 0.0f, -100.0f });
		record.visibilitySettings.layer = 0u;
		record.visibilitySettings.distanceCullingEnabled = false;
		record.visibilitySettings.minDrawDistance = 0.0f;
		record.visibilitySettings.maxDrawDistance = 0.0f;
		record.commandOffsetBegin = index;
		record.commandOffsetEnd = index + 1u;
		record.ownerAlive = true;
		record.ownerActive = true;
		record.occupied = true;
		record.tombstoned = false;
		snapshot.handleToDenseIndex[index] = { record.handle, static_cast<uint64_t>(index) };
		snapshot.denseIndexToHandle[index] = record.handle;
		snapshot.commandOffsetTable[index] = {
			record.handle,
			record.commandOffsetBegin,
			record.commandOffsetEnd
		};
	}

	LODGroupRecord group;
	group.groupHandle = { 0u };
	group.worldReferencePoint = { 0.0f, 0.0f, -100.0f };
	group.worldSize = 20.0f;
	group.levels = {
		LODLevelRecord { 0.50f, { snapshot.primitiveRecords[0].handle } },
		LODLevelRecord { 0.20f, { snapshot.primitiveRecords[1].handle } },
		LODLevelRecord { 0.00f, { snapshot.primitiveRecords[2].handle } }
	};
	const std::vector<LODGroupRecord> groups { group };

	SceneRepresentationState representation;
	representation.lodGroups = &groups;

	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;
	options.cameraPosition = { 0.0f, 0.0f, 0.0f };
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableLOD = true;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		representation,
		SceneVisibilityPipelineMode::Serial);

	ASSERT_EQ(result.selectedLOD.size(), snapshot.primitiveRecords.size());
	EXPECT_EQ(result.selectedLOD[0], 1u);
	EXPECT_EQ(result.selectedLOD[1], 1u);
	EXPECT_EQ(result.selectedLOD[2], 1u);
	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), snapshot.primitiveRecords[1].handle);
	EXPECT_EQ(result.cullReasons[0], CullReason::LODInactive);
	EXPECT_EQ(result.cullReasons[1], CullReason::Visible);
	EXPECT_EQ(result.cullReasons[2], CullReason::LODInactive);
	ASSERT_EQ(result.eligibleCommandRanges.size(), 1u);
	EXPECT_EQ(result.eligibleCommandRanges.front().handle, snapshot.primitiveRecords[1].handle);
}

TEST(SceneVisibilityPipelineTests, HLODProxySuppressesChildrenWithoutGlobalPrimitiveMutation)
{
	auto snapshot = BuildSnapshot(3u);
	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		auto& record = snapshot.primitiveRecords[index];
		record.handle = MakeHandle(static_cast<uint32_t>(index));
		record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(index + 1u));
		record.worldMatrix = NLS::Maths::Matrix4::Translation({ 0.0f, 0.0f, -200.0f });
		record.visibilitySettings.layer = 0u;
		record.visibilitySettings.distanceCullingEnabled = false;
		record.visibilitySettings.minDrawDistance = 0.0f;
		record.visibilitySettings.maxDrawDistance = 0.0f;
		record.commandOffsetBegin = index;
		record.commandOffsetEnd = index + 1u;
		record.ownerAlive = true;
		record.ownerActive = true;
		record.occupied = true;
		record.tombstoned = false;
		snapshot.handleToDenseIndex[index] = { record.handle, static_cast<uint64_t>(index) };
		snapshot.denseIndexToHandle[index] = record.handle;
		snapshot.commandOffsetTable[index] = {
			record.handle,
			record.commandOffsetBegin,
			record.commandOffsetEnd
		};
	}

	HLODClusterRecord cluster;
	cluster.clusterHandle = { 0u };
	cluster.childPrimitives = {
		snapshot.primitiveRecords[0].handle,
		snapshot.primitiveRecords[1].handle
	};
	cluster.proxyPrimitive = snapshot.primitiveRecords[2].handle;
	cluster.worldReferencePoint = { 0.0f, 0.0f, -200.0f };
	cluster.worldSize = 80.0f;
	cluster.activationScreenRelativeSize = 0.50f;
	cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;
	const std::vector<HLODClusterRecord> clusters { cluster };

	RepresentationResidencySnapshot residency;
	residency.MarkReady(snapshot.primitiveRecords[0].handle);
	residency.MarkReady(snapshot.primitiveRecords[1].handle);
	residency.MarkHLODProxyReady(snapshot.primitiveRecords[2].handle);

	SceneRepresentationState representation;
	representation.hlodClusters = &clusters;
	representation.residency = &residency;

	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;
	options.cameraPosition = { 0.0f, 0.0f, 0.0f };
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableHLOD = true;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		representation,
		SceneVisibilityPipelineMode::Serial);

	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), snapshot.primitiveRecords[2].handle);
	ASSERT_EQ(result.suppressedByHLOD.size(), 2u);
	EXPECT_EQ(result.cullReasons[0], CullReason::HLODChildSuppressed);
	EXPECT_EQ(result.cullReasons[1], CullReason::HLODChildSuppressed);
	EXPECT_EQ(result.cullReasons[2], CullReason::Visible);
	ASSERT_EQ(result.activeHLODClusters.size(), 1u);
	EXPECT_EQ(result.activeHLODClusters.front(), SceneHLODClusterHandle { 0u });
	EXPECT_TRUE(snapshot.primitiveRecords[0].ownerActive);
	EXPECT_TRUE(snapshot.primitiveRecords[1].ownerActive);
}

TEST(SceneVisibilityPipelineTests, HLODMissingProxyExposesStreamingInterest)
{
	auto snapshot = BuildSnapshot(3u);
	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		auto& record = snapshot.primitiveRecords[index];
		record.handle = MakeHandle(static_cast<uint32_t>(index));
		record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(index + 1u));
		record.worldMatrix = NLS::Maths::Matrix4::Translation({ 0.0f, 0.0f, -200.0f });
		record.visibilitySettings.layer = 0u;
		record.visibilitySettings.distanceCullingEnabled = false;
		record.commandOffsetBegin = index;
		record.commandOffsetEnd = index + 1u;
		record.ownerAlive = true;
		record.ownerActive = true;
		record.occupied = true;
		record.tombstoned = false;
		snapshot.handleToDenseIndex[index] = { record.handle, static_cast<uint64_t>(index) };
		snapshot.denseIndexToHandle[index] = record.handle;
		snapshot.commandOffsetTable[index] = {
			record.handle,
			record.commandOffsetBegin,
			record.commandOffsetEnd
		};
	}

	HLODClusterRecord cluster;
	cluster.clusterHandle = { 0u };
	cluster.childPrimitives = {
		snapshot.primitiveRecords[0].handle,
		snapshot.primitiveRecords[1].handle
	};
	cluster.proxyPrimitive = snapshot.primitiveRecords[2].handle;
	cluster.worldReferencePoint = { 0.0f, 0.0f, -200.0f };
	cluster.worldSize = 80.0f;
	cluster.activationScreenRelativeSize = 0.50f;
	cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;
	const std::vector<HLODClusterRecord> clusters { cluster };

	RepresentationResidencySnapshot residency;
	residency.MarkReady(snapshot.primitiveRecords[0].handle);
	residency.MarkReady(snapshot.primitiveRecords[1].handle);

	SceneRepresentationState representation;
	representation.hlodClusters = &clusters;
	representation.residency = &residency;

	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;
	options.cameraPosition = {};
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableHLOD = true;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		representation,
		SceneVisibilityPipelineMode::Serial);

	ASSERT_EQ(result.representationStreamingInterest.size(), 1u);
	EXPECT_EQ(result.representationStreamingInterest.front(), snapshot.primitiveRecords[2].handle);
	EXPECT_EQ(result.visiblePrimitiveHandles.size(), 2u);
	EXPECT_EQ(result.cullReasons[2], CullReason::HLODProxyInactive);
	EXPECT_TRUE(result.activeHLODClusters.empty());
}

TEST(SceneVisibilityPipelineTests, StreamingPlanInputUsesFinalVisibilityAndRepresentationInterest)
{
	auto visibility = NLS::Engine::Rendering::SceneVisibilityPipelineResult {};
	const auto visible = MakeHandle(1u);
	const auto proxyInterest = MakeHandle(7u);
	const auto textureInterest = MakeHandle(8u);
	visibility.visiblePrimitiveHandles = { visible };
	visibility.representationStreamingInterest = {
		proxyInterest,
		proxyInterest,
		textureInterest
	};

	const auto input = SceneVisibilityPipeline::BuildStreamingResidencyInput(42u, visibility);

	EXPECT_EQ(input.frameSerial, 42u);
	ASSERT_EQ(input.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(input.visiblePrimitiveHandles.front(), visible);
	ASSERT_EQ(input.representationStreamingInterest.size(), 2u);
	EXPECT_EQ(input.representationStreamingInterest[0], proxyInterest);
	EXPECT_EQ(input.representationStreamingInterest[1], textureInterest);
}

TEST(SceneVisibilityPipelineTests, HLODProxyPrimitiveIsInactiveUnlessClusterSelectsIt)
{
	auto snapshot = BuildSnapshot(3u);
	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		auto& record = snapshot.primitiveRecords[index];
		record.handle = MakeHandle(static_cast<uint32_t>(index));
		record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(index + 1u));
		record.worldMatrix = NLS::Maths::Matrix4::Translation({ 0.0f, 0.0f, -200.0f });
		record.visibilitySettings.layer = 0u;
		record.visibilitySettings.distanceCullingEnabled = false;
		record.commandOffsetBegin = index;
		record.commandOffsetEnd = index + 1u;
		record.ownerAlive = true;
		record.ownerActive = true;
		record.occupied = true;
		record.tombstoned = false;
		snapshot.handleToDenseIndex[index] = { record.handle, static_cast<uint64_t>(index) };
		snapshot.denseIndexToHandle[index] = record.handle;
		snapshot.commandOffsetTable[index] = {
			record.handle,
			record.commandOffsetBegin,
			record.commandOffsetEnd
		};
	}

	HLODClusterRecord cluster;
	cluster.clusterHandle = { 0u };
	cluster.childPrimitives = {
		snapshot.primitiveRecords[0].handle,
		snapshot.primitiveRecords[1].handle
	};
	cluster.proxyPrimitive = snapshot.primitiveRecords[2].handle;
	cluster.worldReferencePoint = { 0.0f, 0.0f, -200.0f };
	cluster.worldSize = 80.0f;
	cluster.activationScreenRelativeSize = 0.50f;
	cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;
	const std::vector<HLODClusterRecord> clusters { cluster };

	RepresentationResidencySnapshot residency;
	residency.MarkReady(snapshot.primitiveRecords[0].handle);
	residency.MarkReady(snapshot.primitiveRecords[1].handle);
	residency.MarkHLODProxyReady(snapshot.primitiveRecords[2].handle);

	SceneRepresentationState representation;
	representation.hlodClusters = &clusters;
	representation.residency = &residency;

	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;
	options.cameraPosition = {};
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableHLOD = true;
	options.allowHLOD = false;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		representation,
		SceneVisibilityPipelineMode::Serial);

	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 2u);
	EXPECT_NE(
		std::find(result.visiblePrimitiveHandles.begin(), result.visiblePrimitiveHandles.end(), snapshot.primitiveRecords[0].handle),
		result.visiblePrimitiveHandles.end());
	EXPECT_NE(
		std::find(result.visiblePrimitiveHandles.begin(), result.visiblePrimitiveHandles.end(), snapshot.primitiveRecords[1].handle),
		result.visiblePrimitiveHandles.end());
	EXPECT_EQ(
		std::find(result.visiblePrimitiveHandles.begin(), result.visiblePrimitiveHandles.end(), snapshot.primitiveRecords[2].handle),
		result.visiblePrimitiveHandles.end());
	EXPECT_EQ(result.cullReasons[2], CullReason::HLODProxyInactive);
	EXPECT_TRUE(result.activeHLODClusters.empty());
}

TEST(SceneVisibilityPipelineTests, ValidOcclusionHistoryRemovesVisiblePrimitiveBeforeCommandEligibility)
{
	auto snapshot = BuildSnapshot(2u);
	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		auto& record = snapshot.primitiveRecords[index];
		record.handle = MakeHandle(static_cast<uint32_t>(index));
		record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(index + 1u));
		record.worldMatrix = NLS::Maths::Matrix4::Translation({ 0.0f, 0.0f, -20.0f });
		record.visibilitySettings.layer = 0u;
		record.visibilitySettings.distanceCullingEnabled = false;
		record.commandOffsetBegin = index;
		record.commandOffsetEnd = index + 1u;
		record.ownerAlive = true;
		record.ownerActive = true;
		record.occupied = true;
		record.tombstoned = false;
		snapshot.handleToDenseIndex[index] = { record.handle, static_cast<uint64_t>(index) };
		snapshot.denseIndexToHandle[index] = record.handle;
		snapshot.commandOffsetTable[index] = {
			record.handle,
			record.commandOffsetBegin,
			record.commandOffsetEnd
		};
	}

	SceneOcclusionFrameInput frameInput;
	frameInput.enabled = true;
	frameInput.backendSupported = true;
	frameInput.historyTextureValid = true;
	frameInput.frameSerial = 42u;
	frameInput.maxHistoryAge = 2u;
	frameInput.viewKey = 9u;
	frameInput.viewCompatibilityHash = 0x100u;
	frameInput.projectionHash = 0x200u;
	frameInput.jitterHash = 0x300u;
	frameInput.depthFormatKey = 24u;
	frameInput.viewportWidth = 1280u;
	frameInput.viewportHeight = 720u;

	std::vector<SceneOcclusionPrimitiveInput> occlusionInputs;
	occlusionInputs.reserve(snapshot.primitiveRecords.size());
	for (const auto& record : snapshot.primitiveRecords)
	{
		SceneOcclusionPrimitiveInput input;
		input.handle = record.handle;
		input.boundsGeneration = record.handle.index + 10u;
		input.transformGeneration = record.handle.index + 20u;
		input.representationId = record.handle.index + 30u;
		input.depthWriteEligibilityGeneration = record.handle.index + 40u;
		input.depthWriteEligible = true;
		occlusionInputs.push_back(input);
	}

	SceneOcclusionHistory history;
	history.RecordOccluded(
		SceneOcclusionSystem::BuildHistoryKey(frameInput, occlusionInputs[1]),
		frameInput.frameSerial - 1u);

	SceneOcclusionState occlusion;
	occlusion.frameInput = frameInput;
	occlusion.history = &history;
	occlusion.primitiveInputs = &occlusionInputs;

	SceneSpatialIndex spatialIndex;
	SceneRepresentationState representation;
	representation.occlusion = &occlusion;

	SceneVisibilityPipelineOptions options;
	options.cameraPosition = {};
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableOcclusion = true;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		representation,
		SceneVisibilityPipelineMode::Serial);

	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), snapshot.primitiveRecords[0].handle);
	ASSERT_EQ(result.occludedPrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.occludedPrimitiveHandles.front(), snapshot.primitiveRecords[1].handle);
	EXPECT_EQ(result.cullReasons[0], CullReason::Visible);
	EXPECT_EQ(result.cullReasons[1], CullReason::Occluded);
	ASSERT_EQ(result.eligibleCommandRanges.size(), 1u);
	EXPECT_EQ(result.eligibleCommandRanges.front().handle, snapshot.primitiveRecords[0].handle);
}

TEST(SceneVisibilityPipelineTests, OcclusionConsumesFinalHLODRepresentationOnly)
{
	auto snapshot = BuildSnapshot(3u);
	for (size_t index = 0u; index < snapshot.primitiveRecords.size(); ++index)
	{
		auto& record = snapshot.primitiveRecords[index];
		record.handle = MakeHandle(static_cast<uint32_t>(index));
		record.mesh = reinterpret_cast<NLS::Render::Resources::Mesh*>(static_cast<uintptr_t>(index + 1u));
		record.worldMatrix = NLS::Maths::Matrix4::Translation({ 0.0f, 0.0f, -200.0f });
		record.visibilitySettings.layer = 0u;
		record.visibilitySettings.distanceCullingEnabled = false;
		record.commandOffsetBegin = index;
		record.commandOffsetEnd = index + 1u;
		record.ownerAlive = true;
		record.ownerActive = true;
		record.occupied = true;
		record.tombstoned = false;
		snapshot.handleToDenseIndex[index] = { record.handle, static_cast<uint64_t>(index) };
		snapshot.denseIndexToHandle[index] = record.handle;
		snapshot.commandOffsetTable[index] = {
			record.handle,
			record.commandOffsetBegin,
			record.commandOffsetEnd
		};
	}

	HLODClusterRecord cluster;
	cluster.clusterHandle = { 0u };
	cluster.childPrimitives = {
		snapshot.primitiveRecords[0].handle,
		snapshot.primitiveRecords[1].handle
	};
	cluster.proxyPrimitive = snapshot.primitiveRecords[2].handle;
	cluster.worldReferencePoint = { 0.0f, 0.0f, -200.0f };
	cluster.worldSize = 80.0f;
	cluster.activationScreenRelativeSize = 0.50f;
	cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;
	const std::vector<HLODClusterRecord> clusters { cluster };

	RepresentationResidencySnapshot residency;
	residency.MarkReady(snapshot.primitiveRecords[0].handle);
	residency.MarkReady(snapshot.primitiveRecords[1].handle);
	residency.MarkHLODProxyReady(snapshot.primitiveRecords[2].handle);

	SceneOcclusionFrameInput frameInput;
	frameInput.enabled = true;
	frameInput.backendSupported = true;
	frameInput.historyTextureValid = true;
	frameInput.frameSerial = 64u;
	frameInput.maxHistoryAge = 2u;
	frameInput.viewKey = 12u;
	frameInput.viewCompatibilityHash = 0x1200u;
	frameInput.projectionHash = 0x2200u;
	frameInput.jitterHash = 0x3200u;
	frameInput.depthFormatKey = 24u;
	frameInput.viewportWidth = 1920u;
	frameInput.viewportHeight = 1080u;

	SceneOcclusionPrimitiveInput suppressedChildInput;
	suppressedChildInput.handle = snapshot.primitiveRecords[0].handle;
	suppressedChildInput.boundsGeneration = 10u;
	suppressedChildInput.transformGeneration = 20u;
	suppressedChildInput.representationId = 30u;
	suppressedChildInput.depthWriteEligibilityGeneration = 40u;
	suppressedChildInput.depthWriteEligible = true;
	const std::vector<SceneOcclusionPrimitiveInput> occlusionInputs { suppressedChildInput };

	SceneOcclusionHistory history;
	history.RecordOccluded(
		SceneOcclusionSystem::BuildHistoryKey(frameInput, suppressedChildInput),
		frameInput.frameSerial - 1u);

	SceneOcclusionState occlusion;
	occlusion.frameInput = frameInput;
	occlusion.history = &history;
	occlusion.primitiveInputs = &occlusionInputs;

	SceneRepresentationState representation;
	representation.hlodClusters = &clusters;
	representation.residency = &residency;
	representation.occlusion = &occlusion;

	SceneSpatialIndex spatialIndex;
	SceneVisibilityPipelineOptions options;
	options.cameraPosition = {};
	options.visibleLayerMask = 0xFFFF'FFFFu;
	options.enableHLOD = true;
	options.enableOcclusion = true;

	const auto result = SceneVisibilityPipeline::Evaluate(
		options,
		snapshot,
		spatialIndex,
		representation,
		SceneVisibilityPipelineMode::Serial);

	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), snapshot.primitiveRecords[2].handle);
	EXPECT_TRUE(result.occludedPrimitiveHandles.empty());
	EXPECT_EQ(result.cullReasons[0], CullReason::HLODChildSuppressed);
	EXPECT_EQ(result.cullReasons[1], CullReason::HLODChildSuppressed);
	EXPECT_EQ(result.cullReasons[2], CullReason::Visible);
	ASSERT_EQ(result.eligibleCommandRanges.size(), 1u);
	EXPECT_EQ(result.eligibleCommandRanges.front().handle, snapshot.primitiveRecords[2].handle);
}
