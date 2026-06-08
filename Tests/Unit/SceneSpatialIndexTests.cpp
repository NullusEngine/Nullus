#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "LargeSceneOptimizationTestHelpers.h"
#include "Math/Matrix4.h"
#include "Rendering/SceneSpatialIndex.h"

namespace
{
	using NLS::Engine::Rendering::ScenePrimitiveHandle;
	using NLS::Engine::Rendering::ScenePrimitiveSnapshot;
	using NLS::Engine::Rendering::ScenePrimitiveSnapshotRecord;
	using NLS::Engine::Rendering::SceneSpatialIndex;
	using NLS::Engine::Rendering::SceneSpatialIndexPrimitiveClass;
	using NLS::Engine::Rendering::SceneSpatialIndexPrimitiveMetadata;
	using NLS::Engine::Rendering::SceneSpatialIndexQuery;
	using NLS::Engine::Rendering::SceneSpatialIndexUpdateOptions;

	constexpr uint64_t kSceneId = 0x51u;

	ScenePrimitiveHandle MakeHandle(const uint32_t index, const uint32_t generation = 1u)
	{
		return { kSceneId, index, generation };
	}

	ScenePrimitiveSnapshot BuildSnapshot(
		const NLS::Tests::LargeScene::SyntheticPrimitiveScene& scene,
		const std::vector<uint64_t>& omittedIds = {})
	{
		ScenePrimitiveSnapshot snapshot;
		snapshot.sceneId = kSceneId;
		snapshot.snapshotSerial = 1u;
		snapshot.frameSerial = 1u;
		snapshot.memoryArenaSerial = 1u;

		for (const auto& primitive : scene.primitives)
		{
			if (std::find(omittedIds.begin(), omittedIds.end(), primitive.id) != omittedIds.end())
				continue;

			ScenePrimitiveSnapshotRecord record;
			record.handle = MakeHandle(static_cast<uint32_t>(primitive.id));
			record.modelBoundingSphere.position = { 0.0f, 0.0f, 0.0f };
			record.modelBoundingSphere.radius = primitive.radius;
			record.worldMatrix = NLS::Maths::Matrix4::Translation({
				primitive.centerX,
				primitive.centerY,
				primitive.centerZ
			});
			record.frustumBehaviour =
				NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL;
			record.visibilitySettings.layer = primitive.layer;
			record.ownerAlive = true;
			record.ownerActive = primitive.active;
			record.occupied = true;
			record.tombstoned = false;

			const auto denseIndex = static_cast<uint64_t>(snapshot.primitiveRecords.size());
			snapshot.primitiveRecords.push_back(record);
			snapshot.handleToDenseIndex.push_back({ record.handle, denseIndex });
			snapshot.denseIndexToHandle.push_back(record.handle);
		}

		return snapshot;
	}

	std::vector<SceneSpatialIndexPrimitiveMetadata> BuildMetadata(
		const NLS::Tests::LargeScene::SyntheticPrimitiveScene& scene)
	{
		std::vector<SceneSpatialIndexPrimitiveMetadata> metadata;
		metadata.reserve(scene.primitives.size());
		for (const auto& primitive : scene.primitives)
		{
			metadata.push_back({
				MakeHandle(static_cast<uint32_t>(primitive.id)),
				primitive.dynamic
					? SceneSpatialIndexPrimitiveClass::Dynamic
					: SceneSpatialIndexPrimitiveClass::Static,
				primitive.active
			});
		}
		return metadata;
	}

	bool ContainsHandle(
		const std::vector<ScenePrimitiveHandle>& handles,
		const ScenePrimitiveHandle handle)
	{
		return std::find(handles.begin(), handles.end(), handle) != handles.end();
	}

	std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		return std::string(
			std::istreambuf_iterator<char>(stream),
			std::istreambuf_iterator<char>());
	}
}

TEST(SceneSpatialIndexTests, StaticLocalizedQueryReturnsBoundedCandidatesAndMatchesFullScan)
{
	const auto scene = NLS::Tests::LargeScene::BuildPartitionedPrimitiveScene(
		316u,
		317u,
		10.0f);
	ASSERT_GE(scene.primitives.size(), 100000u);

	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	SceneSpatialIndexQuery query;
	query.center = { 400.0f, 0.0f, 400.0f };
	query.radius = 90.0f;
	query.visibleLayerMask = 0xFFFF'FFFFu;

	const auto indexed = index.Query(query);
	const auto fullScan = index.FullScanForComparison(query);

	EXPECT_EQ(indexed.candidatePrimitiveHandles, fullScan.candidatePrimitiveHandles);
	EXPECT_TRUE(NLS::Tests::LargeScene::IsCandidateRatioWithinBudget(
		indexed.candidateCount,
		static_cast<uint64_t>(scene.primitives.size()),
		0.30));
	EXPECT_TRUE(NLS::Tests::LargeScene::IsCandidateRatioWithinBudget(
		indexed.primitiveRecordsTouched,
		static_cast<uint64_t>(scene.primitives.size()),
		0.35));
	EXPECT_EQ(indexed.fullScanCandidateCount, 0u);
	EXPECT_GT(fullScan.fullScanCandidateCount, indexed.candidateCount);
}

TEST(SceneSpatialIndexTests, DynamicLocalizedQueryReportsDynamicCandidatesAndTouchedCounts)
{
	auto scene = NLS::Tests::LargeScene::BuildPartitionedPrimitiveScene(
		128u,
		128u,
		8.0f,
		2u);

	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	SceneSpatialIndexQuery query;
	query.center = { 128.0f, 0.0f, 128.0f };
	query.radius = 48.0f;

	const auto result = index.Query(query);

	EXPECT_GT(index.GetDynamicPrimitiveCount(), 0u);
	EXPECT_GT(result.dynamicCandidateCount, 0u);
	EXPECT_GE(result.dynamicRecordsTouched, result.dynamicCandidateCount);
	EXPECT_LT(result.dynamicRecordsTouched, scene.dynamicPrimitiveCount);
	EXPECT_LT(result.primitiveRecordsTouched, scene.primitives.size());
}

TEST(SceneSpatialIndexTests, PrimitiveClassCountersDoNotScanAllRecordsOnTelemetryRead)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/SceneSpatialIndex.cpp");

	const auto staticGetter = source.find("size_t SceneSpatialIndex::GetStaticPrimitiveCount() const");
	const auto initializedGetter = source.find("bool SceneSpatialIndex::IsInitialized() const", staticGetter);
	const auto dynamicGetter = source.find("size_t SceneSpatialIndex::GetDynamicPrimitiveCount() const");
	ASSERT_NE(staticGetter, std::string::npos);
	ASSERT_NE(initializedGetter, std::string::npos);
	ASSERT_NE(dynamicGetter, std::string::npos);

	const auto staticBody = source.substr(staticGetter, initializedGetter - staticGetter);
	const auto dynamicEnd = source.find("\n}", dynamicGetter);
	ASSERT_NE(dynamicEnd, std::string::npos);
	const auto dynamicBody = source.substr(dynamicGetter, dynamicEnd - dynamicGetter);

	EXPECT_EQ(staticBody.find("std::count_if"), std::string::npos);
	EXPECT_EQ(staticBody.find("storage.records"), std::string::npos);
	EXPECT_NE(staticBody.find("storage.staticPrimitiveCount"), std::string::npos);
	EXPECT_EQ(dynamicBody.find("std::count_if"), std::string::npos);
	EXPECT_EQ(dynamicBody.find("storage.records"), std::string::npos);
	EXPECT_NE(dynamicBody.find("storage.dynamicPrimitiveCount"), std::string::npos);
}

TEST(SceneSpatialIndexTests, WideQueryUsesDirectCellsAndOnlyFallsBackForHugeVolumes)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/SceneSpatialIndex.cpp");

	const auto visitQueryCellsStart = source.find("void VisitQueryCells(");
	ASSERT_NE(visitQueryCellsStart, std::string::npos);
	const auto queryStart = source.find("VisibilityCandidateSet SceneSpatialIndex::Query");
	ASSERT_NE(queryStart, std::string::npos);
	const auto visitQueryCellsBody = source.substr(visitQueryCellsStart, queryStart - visitQueryCellsStart);

	EXPECT_NE(visitQueryCellsBody.find("queryCellVolume"), std::string::npos);
	EXPECT_NE(visitQueryCellsBody.find("queryCellVolume > static_cast<uint64_t>(buckets.size())"), std::string::npos);
	EXPECT_NE(visitQueryCellsBody.find("for (const auto& [cell, handles] : buckets)"), std::string::npos);
	EXPECT_NE(visitQueryCellsBody.find("CellIntersectsQueryAABB(cell, minCell, maxCell)"), std::string::npos);
	EXPECT_NE(visitQueryCellsBody.find("for (int32_t z = minCell.z; z <= maxCell.z; ++z)"), std::string::npos);
	EXPECT_NE(visitQueryCellsBody.find("buckets.find({ x, y, z })"), std::string::npos);

	const auto fullScanStart = source.find("VisibilityCandidateSet SceneSpatialIndex::FullScanForComparison", queryStart);
	ASSERT_NE(fullScanStart, std::string::npos);
	const auto queryBody = source.substr(queryStart, fullScanStart - queryStart);

	EXPECT_NE(queryBody.find("VisitQueryCells("), std::string::npos);
}

TEST(SceneSpatialIndexTests, HighDynamicCountSceneStillUsesLocalizedDynamicBuckets)
{
	auto scene = NLS::Tests::LargeScene::BuildPartitionedPrimitiveScene(
		320u,
		320u,
		8.0f,
		1u);
	ASSERT_GE(scene.dynamicPrimitiveCount, 100000u);

	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	SceneSpatialIndexQuery query;
	query.center = { 512.0f, 0.0f, 512.0f };
	query.radius = 56.0f;

	const auto indexed = index.Query(query);
	const auto fullScan = index.FullScanForComparison(query);

	EXPECT_EQ(indexed.candidatePrimitiveHandles, fullScan.candidatePrimitiveHandles);
	EXPECT_GT(indexed.dynamicCandidateCount, 0u);
	EXPECT_EQ(indexed.candidateCount, indexed.dynamicCandidateCount);
	EXPECT_TRUE(NLS::Tests::LargeScene::IsCandidateRatioWithinBudget(
		indexed.dynamicRecordsTouched,
		scene.dynamicPrimitiveCount,
		0.35));
	EXPECT_LT(indexed.primitiveRecordsTouched, fullScan.primitiveRecordsTouched);
}

TEST(SceneSpatialIndexTests, LargeRadiusPrimitiveIsInsertedIntoOverlappedCells)
{
	auto scene = NLS::Tests::LargeScene::BuildLinearPrimitiveScene(32u, 128.0f);
	scene.primitives[31u].centerX = 500.0f;
	scene.primitives[31u].radius = 600.0f;

	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	SceneSpatialIndexQuery query;
	query.center = { 0.0f, 0.0f, 0.0f };
	query.radius = 1.0f;

	const auto indexed = index.Query(query);
	const auto fullScan = index.FullScanForComparison(query);

	EXPECT_EQ(indexed.candidatePrimitiveHandles, fullScan.candidatePrimitiveHandles);
	EXPECT_TRUE(ContainsHandle(indexed.candidatePrimitiveHandles, MakeHandle(31u)));
	EXPECT_EQ(indexed.fullScanCandidateCount, 0u);
	EXPECT_LT(indexed.primitiveRecordsTouched, fullScan.primitiveRecordsTouched);
}

TEST(SceneSpatialIndexTests, LayerActiveAndDistanceMetadataFilterWithoutFullScan)
{
	auto scene = NLS::Tests::LargeScene::BuildLinearPrimitiveScene(256u, 5.0f);
	for (size_t index = 0u; index < scene.primitives.size(); ++index)
	{
		scene.primitives[index].layer = index % 2u;
		scene.primitives[index].active = index % 5u != 0u;
	}

	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	SceneSpatialIndexQuery query;
	query.center = { 100.0f, 0.0f, 0.0f };
	query.radius = 100.0f;
	query.visibleLayerMask = 1u << 1u;
	query.distanceCullingEnabled = true;
	query.minDistance = 10.0f;
	query.maxDistance = 75.0f;

	const auto result = index.Query(query);
	ASSERT_FALSE(result.candidatePrimitiveHandles.empty());
	for (const auto& handle : result.candidatePrimitiveHandles)
	{
		const auto& primitive = scene.primitives[handle.index];
		EXPECT_TRUE(primitive.active);
		EXPECT_EQ(primitive.layer, 1u);
		const auto distance = NLS::Maths::Vector3::Distance(
			query.center,
			{ primitive.centerX, primitive.centerY, primitive.centerZ });
		EXPECT_GE(distance + primitive.radius, query.minDistance);
		EXPECT_LE(distance - primitive.radius, query.maxDistance);
	}
	EXPECT_EQ(result.fullScanCandidateCount, 0u);
}

TEST(SceneSpatialIndexTests, QueryUsesTransformedBoundsCenterAndScaledRadius)
{
	ScenePrimitiveSnapshot snapshot;
	snapshot.sceneId = kSceneId;

	ScenePrimitiveSnapshotRecord record;
	record.handle = MakeHandle(7u);
	record.modelBoundingSphere.position = { 5.0f, 0.0f, 0.0f };
	record.modelBoundingSphere.radius = 2.0f;
	record.worldMatrix =
		NLS::Maths::Matrix4::Translation({ 10.0f, 0.0f, 0.0f }) *
		NLS::Maths::Matrix4::Scaling({ 3.0f, 2.0f, 1.0f });
	record.visibilitySettings.layer = 0u;
	record.ownerAlive = true;
	record.ownerActive = true;
	record.occupied = true;
	snapshot.primitiveRecords.push_back(record);

	SceneSpatialIndex index;
	index.Update(snapshot);

	SceneSpatialIndexQuery query;
	query.center = { 31.0f, 0.0f, 0.0f };
	query.radius = 1.0f;

	const auto result = index.Query(query);

	ASSERT_EQ(result.candidatePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.candidatePrimitiveHandles.front(), record.handle);
}

TEST(SceneSpatialIndexTests, RebuildBudgetUsesLastGoodStaticIndexAndDirtyOverlay)
{
	auto scene = NLS::Tests::LargeScene::BuildLinearPrimitiveScene(128u, 10.0f);
	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	const auto removedHandle = MakeHandle(0u);
	const auto movedHandle = MakeHandle(127u);
	scene.primitives[127u].centerX = 5.0f;

	SceneSpatialIndexUpdateOptions budgetedOptions;
	budgetedOptions.staticRebuildDirtyRatio = 0.001;
	budgetedOptions.rebuildBudgetUs = 1u;
	index.Update(BuildSnapshot(scene, { 0u }), BuildMetadata(scene), budgetedOptions);

	SceneSpatialIndexQuery query;
	query.center = { 5.0f, 0.0f, 0.0f };
	query.radius = 12.0f;

	const auto result = index.Query(query);

	EXPECT_FALSE(ContainsHandle(result.candidatePrimitiveHandles, removedHandle));
	EXPECT_TRUE(ContainsHandle(result.candidatePrimitiveHandles, movedHandle));
	EXPECT_GT(result.telemetry.staticIndexLastGoodQueryCount, 0u);
	EXPECT_GT(result.telemetry.staticIndexDirtyOverlayCount, 0u);
	EXPECT_GT(result.telemetry.spatialRebuildFallbackCount, 0u);
}

TEST(SceneSpatialIndexTests, BudgetRecoveryRebuildsStaticIndexWithoutNewDirtyHandles)
{
	auto scene = NLS::Tests::LargeScene::BuildLinearPrimitiveScene(128u, 10.0f);
	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	scene.primitives[127u].centerX = 5.0f;

	SceneSpatialIndexUpdateOptions budgetedOptions;
	budgetedOptions.staticRebuildDirtyRatio = 0.001;
	budgetedOptions.rebuildBudgetUs = 1u;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene), budgetedOptions);

	SceneSpatialIndexQuery query;
	query.center = { 5.0f, 0.0f, 0.0f };
	query.radius = 12.0f;
	ASSERT_GT(index.Query(query).telemetry.spatialRebuildFallbackCount, 0u);

	ScenePrimitiveSnapshot noChanges;
	noChanges.sceneId = kSceneId;
	index.UpdateChanged(noChanges, BuildMetadata(scene), budgetedOptions);

	const auto recovered = index.Query(query);
	EXPECT_TRUE(ContainsHandle(recovered.candidatePrimitiveHandles, MakeHandle(127u)));
	EXPECT_EQ(recovered.telemetry.staticIndexLastGoodQueryCount, 0u);
	EXPECT_EQ(recovered.telemetry.staticIndexDirtyOverlayCount, 0u);
	EXPECT_EQ(recovered.telemetry.spatialRebuildFallbackCount, 0u);
	EXPECT_EQ(recovered.telemetry.staticIndexRebuildCount, 1u);
}

TEST(SceneSpatialIndexTests, BudgetedIncrementalStaticChangeUsesDirtyOverlayAndLastGoodIndex)
{
	auto scene = NLS::Tests::LargeScene::BuildLinearPrimitiveScene(128u, 10.0f);
	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	scene.primitives[127u].centerX = 5.0f;
	ScenePrimitiveSnapshot changed;
	changed.sceneId = kSceneId;
	const auto movedSnapshot = BuildSnapshot(scene);
	ASSERT_GT(movedSnapshot.primitiveRecords.size(), 127u);
	changed.primitiveRecords.push_back(movedSnapshot.primitiveRecords[127u]);
	changed.dirtySyncHandles.push_back(MakeHandle(127u));

	SceneSpatialIndexUpdateOptions budgetedOptions;
	budgetedOptions.staticRebuildDirtyRatio = 0.001;
	budgetedOptions.rebuildBudgetUs = 1u;
	index.UpdateChanged(changed, BuildMetadata(scene), budgetedOptions);

	SceneSpatialIndexQuery query;
	query.center = { 5.0f, 0.0f, 0.0f };
	query.radius = 12.0f;

	const auto result = index.Query(query);
	EXPECT_TRUE(ContainsHandle(result.candidatePrimitiveHandles, MakeHandle(127u)));
	EXPECT_GT(result.telemetry.staticIndexLastGoodQueryCount, 0u);
	EXPECT_GT(result.telemetry.staticIndexDirtyOverlayCount, 0u);
	EXPECT_GT(result.telemetry.spatialRebuildFallbackCount, 0u);
	EXPECT_EQ(index.GetLastUpdateTelemetry().staticIndexRebuildCount, 0u);
}

TEST(SceneSpatialIndexTests, TopologyChurnRemovesStaleHandlesAndMatchesFullScanAfterRebuild)
{
	auto scene = NLS::Tests::LargeScene::BuildLinearPrimitiveScene(2048u, 6.0f);
	SceneSpatialIndex index;
	index.Update(BuildSnapshot(scene), BuildMetadata(scene));

	std::vector<uint64_t> omittedIds;
	omittedIds.reserve(256u);
	for (uint64_t id = 0u; id < 256u; ++id)
		omittedIds.push_back(id);
	for (uint64_t id = 1800u; id < 1850u; ++id)
		scene.primitives[static_cast<size_t>(id)].centerX = 12.0f + static_cast<float>(id - 1800u);

	index.Update(BuildSnapshot(scene, omittedIds), BuildMetadata(scene));

	SceneSpatialIndexQuery query;
	query.center = { 24.0f, 0.0f, 0.0f };
	query.radius = 48.0f;

	const auto indexed = index.Query(query);
	const auto fullScan = index.FullScanForComparison(query);

	EXPECT_EQ(indexed.candidatePrimitiveHandles, fullScan.candidatePrimitiveHandles);
	EXPECT_FALSE(ContainsHandle(indexed.candidatePrimitiveHandles, MakeHandle(0u)));
	EXPECT_FALSE(ContainsHandle(indexed.candidatePrimitiveHandles, MakeHandle(255u)));
	EXPECT_TRUE(ContainsHandle(indexed.candidatePrimitiveHandles, MakeHandle(1800u)));
	EXPECT_EQ(indexed.telemetry.staticIndexRebuildCount, 1u);
	EXPECT_LT(indexed.primitiveRecordsTouched, fullScan.primitiveRecordsTouched);
}
