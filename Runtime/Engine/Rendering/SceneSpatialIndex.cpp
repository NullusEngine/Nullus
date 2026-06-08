#include "Rendering/SceneSpatialIndex.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <Math/Vector4.h>

namespace NLS::Engine::Rendering
{
	namespace
	{
		constexpr float kDefaultCellSize = 64.0f;

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

		struct CellKey
		{
			int32_t x = 0;
			int32_t y = 0;
			int32_t z = 0;

			bool operator==(const CellKey& other) const = default;
		};

		struct CellKeyHash
		{
			size_t operator()(const CellKey& key) const noexcept
			{
				auto hash = static_cast<size_t>(static_cast<uint32_t>(key.x));
				hash ^= static_cast<size_t>(static_cast<uint32_t>(key.y)) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
				hash ^= static_cast<size_t>(static_cast<uint32_t>(key.z)) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
				return hash;
			}
		};

		struct IndexedPrimitive
		{
			ScenePrimitiveHandle handle;
			Maths::Vector3 center;
			float radius = 0.0f;
			uint32_t layer = 0u;
			bool active = true;
			Components::MeshRenderer::EFrustumBehaviour frustumBehaviour =
				Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL;
			SceneSpatialIndexPrimitiveClass primitiveClass = SceneSpatialIndexPrimitiveClass::Static;
		};

		using PrimitiveMap = std::unordered_map<ScenePrimitiveHandle, IndexedPrimitive, HandleHash>;
		using CellBuckets = std::unordered_map<CellKey, std::vector<ScenePrimitiveHandle>, CellKeyHash>;
		using BucketCellMap = std::unordered_map<ScenePrimitiveHandle, std::vector<CellKey>, HandleHash>;

		CellKey ToCell(const Maths::Vector3& center)
		{
			return {
				static_cast<int32_t>(std::floor(center.x / kDefaultCellSize)),
				static_cast<int32_t>(std::floor(center.y / kDefaultCellSize)),
				static_cast<int32_t>(std::floor(center.z / kDefaultCellSize))
			};
		}

		Maths::Vector3 TransformPoint(const Maths::Matrix4& matrix, const Maths::Vector3& point)
		{
			const auto transformed = matrix * Maths::Vector4(point, 1.0f);
			if (transformed.w != 0.0f && transformed.w != 1.0f)
			{
				return {
					transformed.x / transformed.w,
					transformed.y / transformed.w,
					transformed.z / transformed.w
				};
			}

			return { transformed.x, transformed.y, transformed.z };
		}

		float ExtractMaxScale(const Maths::Matrix4& matrix)
		{
			const Maths::Vector3 columns[] = {
				{ matrix.data[0], matrix.data[4], matrix.data[8] },
				{ matrix.data[1], matrix.data[5], matrix.data[9] },
				{ matrix.data[2], matrix.data[6], matrix.data[10] }
			};
			return std::max({
				columns[0].Length(),
				columns[1].Length(),
				columns[2].Length(),
				1.0f
			});
		}

		Maths::Vector3 ExtractWorldCenter(const ScenePrimitiveSnapshotRecord& record)
		{
			return TransformPoint(record.worldMatrix, record.modelBoundingSphere.position);
		}

		float ExtractWorldRadius(const ScenePrimitiveSnapshotRecord& record)
		{
			return std::max(0.0f, record.modelBoundingSphere.radius) * ExtractMaxScale(record.worldMatrix);
		}

		bool SameSpatialRecord(const IndexedPrimitive& lhs, const IndexedPrimitive& rhs)
		{
			return lhs.handle == rhs.handle &&
				lhs.center == rhs.center &&
				lhs.radius == rhs.radius &&
				lhs.layer == rhs.layer &&
				lhs.active == rhs.active &&
				lhs.frustumBehaviour == rhs.frustumBehaviour &&
				lhs.primitiveClass == rhs.primitiveClass;
		}

		bool HandleLess(const ScenePrimitiveHandle& lhs, const ScenePrimitiveHandle& rhs)
		{
			if (lhs.sceneId != rhs.sceneId)
				return lhs.sceneId < rhs.sceneId;
			if (lhs.index != rhs.index)
				return lhs.index < rhs.index;
			return lhs.generation < rhs.generation;
		}

		void SortAndUnique(std::vector<ScenePrimitiveHandle>& handles)
		{
			std::sort(handles.begin(), handles.end(), HandleLess);
			handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
		}

		bool LayerPasses(const IndexedPrimitive& primitive, const uint32_t visibleLayerMask)
		{
			if (primitive.layer >= 32u)
				return false;
			return (visibleLayerMask & (1u << primitive.layer)) != 0u;
		}

		bool QueryPasses(const IndexedPrimitive& primitive, const SceneSpatialIndexQuery& query)
		{
			if (!primitive.active || !LayerPasses(primitive, query.visibleLayerMask))
				return false;

			const auto distance = Maths::Vector3::Distance(query.center, primitive.center);
			const bool frustumUnbounded =
				primitive.frustumBehaviour == Components::MeshRenderer::EFrustumBehaviour::DISABLED;
			if (!frustumUnbounded && distance > query.radius + primitive.radius)
				return false;

			if (query.distanceCullingEnabled)
			{
				if (distance + primitive.radius < query.minDistance)
					return false;
				if (query.maxDistance > 0.0f && distance - primitive.radius > query.maxDistance)
					return false;
			}

			return true;
		}

		std::vector<CellKey> ResolveOverlappedCells(const IndexedPrimitive& primitive)
		{
			std::vector<CellKey> cells;
			const auto radius = std::max(0.0f, primitive.radius);
			const auto minCell = ToCell(primitive.center - Maths::Vector3(radius, radius, radius));
			const auto maxCell = ToCell(primitive.center + Maths::Vector3(radius, radius, radius));
			for (int32_t z = minCell.z; z <= maxCell.z; ++z)
			{
				for (int32_t y = minCell.y; y <= maxCell.y; ++y)
				{
					for (int32_t x = minCell.x; x <= maxCell.x; ++x)
						cells.push_back({ x, y, z });
				}
			}
			return cells;
		}

		void InsertBucket(CellBuckets& buckets, BucketCellMap& cellMap, const IndexedPrimitive& primitive)
		{
			auto cells = ResolveOverlappedCells(primitive);
			for (const auto& cell : cells)
				buckets[cell].push_back(primitive.handle);
			cellMap[primitive.handle] = std::move(cells);
		}

		void RemoveBucket(CellBuckets& buckets, BucketCellMap& cellMap, const ScenePrimitiveHandle& handle)
		{
			const auto foundCells = cellMap.find(handle);
			if (foundCells == cellMap.end())
				return;

			for (const auto& cell : foundCells->second)
			{
				const auto foundBucket = buckets.find(cell);
				if (foundBucket == buckets.end())
					continue;

				auto& handles = foundBucket->second;
				handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
				if (handles.empty())
					buckets.erase(foundBucket);
			}
			cellMap.erase(foundCells);
		}

		void RemoveFrustumUnboundedHandle(std::vector<ScenePrimitiveHandle>& handles, const ScenePrimitiveHandle& handle)
		{
			handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
		}

		void AddFrustumUnboundedHandle(std::vector<ScenePrimitiveHandle>& handles, const ScenePrimitiveHandle& handle)
		{
			handles.push_back(handle);
			SortAndUnique(handles);
		}

		bool CellIntersectsQueryAABB(
			const CellKey& cell,
			const CellKey& minCell,
			const CellKey& maxCell)
		{
			return cell.x >= minCell.x && cell.x <= maxCell.x &&
				cell.y >= minCell.y && cell.y <= maxCell.y &&
				cell.z >= minCell.z && cell.z <= maxCell.z;
		}

		uint64_t ResolveCellVolume(const CellKey& minCell, const CellKey& maxCell)
		{
			const auto extentX = static_cast<uint64_t>(std::max<int64_t>(
				0,
				static_cast<int64_t>(maxCell.x) - static_cast<int64_t>(minCell.x) + 1));
			const auto extentY = static_cast<uint64_t>(std::max<int64_t>(
				0,
				static_cast<int64_t>(maxCell.y) - static_cast<int64_t>(minCell.y) + 1));
			const auto extentZ = static_cast<uint64_t>(std::max<int64_t>(
				0,
				static_cast<int64_t>(maxCell.z) - static_cast<int64_t>(minCell.z) + 1));
			if (extentX == 0u || extentY == 0u || extentZ == 0u)
				return 0u;
			if (extentX > std::numeric_limits<uint64_t>::max() / extentY)
				return std::numeric_limits<uint64_t>::max();
			const auto xy = extentX * extentY;
			if (xy > std::numeric_limits<uint64_t>::max() / extentZ)
				return std::numeric_limits<uint64_t>::max();
			return xy * extentZ;
		}

		template <typename VisitHandle>
		void VisitQueryCells(
			const CellBuckets& buckets,
			const CellKey& minCell,
			const CellKey& maxCell,
			const bool dynamicBuckets,
			VisitHandle&& visitHandle)
		{
			const auto queryCellVolume = ResolveCellVolume(minCell, maxCell);
			if (queryCellVolume > static_cast<uint64_t>(buckets.size()))
			{
				for (const auto& [cell, handles] : buckets)
				{
					if (!CellIntersectsQueryAABB(cell, minCell, maxCell))
						continue;
					for (const auto& handle : handles)
						visitHandle(handle, dynamicBuckets);
				}
				return;
			}

			for (int32_t z = minCell.z; z <= maxCell.z; ++z)
			{
				for (int32_t y = minCell.y; y <= maxCell.y; ++y)
				{
					for (int32_t x = minCell.x; x <= maxCell.x; ++x)
					{
						const auto foundBucket = buckets.find({ x, y, z });
						if (foundBucket == buckets.end())
							continue;
						for (const auto& handle : foundBucket->second)
							visitHandle(handle, dynamicBuckets);
					}
				}
			}
		}

		IndexedPrimitive BuildIndexedPrimitive(
			const ScenePrimitiveSnapshotRecord& record,
			const std::unordered_map<ScenePrimitiveHandle, SceneSpatialIndexPrimitiveMetadata, HandleHash>& metadataByHandle)
		{
			IndexedPrimitive indexed;
			indexed.handle = record.handle;
			indexed.center = ExtractWorldCenter(record);
			indexed.radius = ExtractWorldRadius(record);
			indexed.layer = record.visibilitySettings.layer;
			indexed.active = record.ownerAlive && record.ownerActive;
			indexed.frustumBehaviour = record.frustumBehaviour;

			if (const auto found = metadataByHandle.find(record.handle); found != metadataByHandle.end())
			{
				indexed.primitiveClass = found->second.primitiveClass;
				indexed.active = indexed.active && found->second.active;
			}

			return indexed;
		}

		std::unordered_map<ScenePrimitiveHandle, SceneSpatialIndexPrimitiveMetadata, HandleHash> BuildMetadataMap(
			const std::vector<SceneSpatialIndexPrimitiveMetadata>& metadata)
		{
			std::unordered_map<ScenePrimitiveHandle, SceneSpatialIndexPrimitiveMetadata, HandleHash> metadataByHandle;
			metadataByHandle.reserve(metadata.size());
			for (const auto& entry : metadata)
				metadataByHandle[entry.handle] = entry;
			return metadataByHandle;
		}
	}

	class SceneSpatialIndex::Storage
	{
	public:
		PrimitiveMap records;
		CellBuckets staticBuckets;
		CellBuckets dynamicBuckets;
		BucketCellMap staticBucketCells;
		BucketCellMap dynamicBucketCells;
		std::vector<ScenePrimitiveHandle> dirtyOverlayHandles;
		std::vector<ScenePrimitiveHandle> frustumUnboundedHandles;
		NLS::Render::Data::LargeSceneTelemetry lastUpdateTelemetry;
		size_t staticPrimitiveCount = 0u;
		size_t dynamicPrimitiveCount = 0u;
		bool initialized = false;
		bool useLastGoodStaticBuckets = false;

		void BeginUpdate()
		{
			lastUpdateTelemetry = {};
		}

		void RemovePrimitive(const ScenePrimitiveHandle& handle)
		{
			const auto existing = records.find(handle);
			if (existing == records.end())
				return;

			if (existing->second.primitiveClass == SceneSpatialIndexPrimitiveClass::Dynamic)
				dynamicPrimitiveCount = dynamicPrimitiveCount > 0u ? dynamicPrimitiveCount - 1u : 0u;
			else
				staticPrimitiveCount = staticPrimitiveCount > 0u ? staticPrimitiveCount - 1u : 0u;

			RemoveBucket(staticBuckets, staticBucketCells, handle);
			RemoveBucket(dynamicBuckets, dynamicBucketCells, handle);
			RemoveFrustumUnboundedHandle(frustumUnboundedHandles, handle);
			RemoveFrustumUnboundedHandle(dirtyOverlayHandles, handle);
			records.erase(handle);
		}

		void InsertPrimitive(const IndexedPrimitive& primitive)
		{
			records[primitive.handle] = primitive;
			if (primitive.primitiveClass == SceneSpatialIndexPrimitiveClass::Dynamic)
			{
				++dynamicPrimitiveCount;
				InsertBucket(dynamicBuckets, dynamicBucketCells, primitive);
			}
			else if (!useLastGoodStaticBuckets)
			{
				++staticPrimitiveCount;
				InsertBucket(staticBuckets, staticBucketCells, primitive);
			}
			else
			{
				++staticPrimitiveCount;
				dirtyOverlayHandles.push_back(primitive.handle);
				SortAndUnique(dirtyOverlayHandles);
			}

			if (primitive.frustumBehaviour == Components::MeshRenderer::EFrustumBehaviour::DISABLED)
				AddFrustumUnboundedHandle(frustumUnboundedHandles, primitive.handle);
		}

		void ReplacePrimitive(const IndexedPrimitive& primitive)
		{
			RemovePrimitive(primitive.handle);
			InsertPrimitive(primitive);
		}

		void RefreshPrimitiveClassCounts()
		{
			staticPrimitiveCount = 0u;
			dynamicPrimitiveCount = 0u;
			for (const auto& [handle, primitive] : records)
			{
				if (primitive.primitiveClass == SceneSpatialIndexPrimitiveClass::Dynamic)
					++dynamicPrimitiveCount;
				else
					++staticPrimitiveCount;
			}
		}

		void RebuildStaticBucketsFromRecords()
		{
			staticBuckets.clear();
			staticBucketCells.clear();
			for (const auto& [handle, primitive] : records)
			{
				if (primitive.primitiveClass == SceneSpatialIndexPrimitiveClass::Static)
					InsertBucket(staticBuckets, staticBucketCells, primitive);
			}
			dirtyOverlayHandles.clear();
			useLastGoodStaticBuckets = false;
			lastUpdateTelemetry.staticIndexRebuildCount = 1u;
		}
	};

	SceneSpatialIndex::SceneSpatialIndex()
		: m_storage(std::make_unique<Storage>())
	{
	}

	SceneSpatialIndex::~SceneSpatialIndex() = default;
	SceneSpatialIndex::SceneSpatialIndex(SceneSpatialIndex&&) noexcept = default;
	SceneSpatialIndex& SceneSpatialIndex::operator=(SceneSpatialIndex&&) noexcept = default;

	void SceneSpatialIndex::Update(
		const ScenePrimitiveSnapshot& primitives,
		const std::vector<SceneSpatialIndexPrimitiveMetadata>& metadata,
		const SceneSpatialIndexUpdateOptions& options)
	{
		auto& storage = *m_storage;
		storage.BeginUpdate();

		const auto metadataByHandle = BuildMetadataMap(metadata);

		PrimitiveMap nextRecords;
		nextRecords.reserve(primitives.primitiveRecords.size());
		for (const auto& record : primitives.primitiveRecords)
		{
			if (!record.handle.IsValid() || !record.occupied || record.tombstoned)
				continue;

			const auto indexed = BuildIndexedPrimitive(record, metadataByHandle);
			nextRecords[indexed.handle] = indexed;
		}

		std::vector<ScenePrimitiveHandle> dirtyStaticHandles;
		size_t staticPrimitiveCount = 0u;
		for (const auto& [handle, next] : nextRecords)
		{
			if (next.primitiveClass != SceneSpatialIndexPrimitiveClass::Static)
				continue;

			++staticPrimitiveCount;
			const auto previous = storage.records.find(handle);
			if (previous == storage.records.end() || !SameSpatialRecord(previous->second, next))
				dirtyStaticHandles.push_back(handle);
		}

		for (const auto& [handle, previous] : storage.records)
		{
			if (previous.primitiveClass != SceneSpatialIndexPrimitiveClass::Static)
				continue;
			if (nextRecords.find(handle) == nextRecords.end())
				dirtyStaticHandles.push_back(handle);
		}
		SortAndUnique(dirtyStaticHandles);

		const auto dirtyRatio = staticPrimitiveCount > 0u
			? static_cast<double>(dirtyStaticHandles.size()) / static_cast<double>(staticPrimitiveCount)
			: 0.0;
		const bool deferStaticRebuild =
			!storage.staticBuckets.empty() &&
			options.rebuildBudgetUs > 0u &&
			dirtyRatio > options.staticRebuildDirtyRatio;

		storage.records = std::move(nextRecords);
		storage.RefreshPrimitiveClassCounts();

		if (deferStaticRebuild)
		{
			storage.useLastGoodStaticBuckets = true;
			storage.dirtyOverlayHandles.clear();
			for (const auto& handle : dirtyStaticHandles)
			{
				if (const auto found = storage.records.find(handle);
					found != storage.records.end() &&
					found->second.primitiveClass == SceneSpatialIndexPrimitiveClass::Static)
				{
					storage.dirtyOverlayHandles.push_back(handle);
				}
			}
			SortAndUnique(storage.dirtyOverlayHandles);
			storage.lastUpdateTelemetry.staticIndexRefitCount = 1u;
			storage.lastUpdateTelemetry.staticIndexDirtyOverlayCount =
				static_cast<uint64_t>(storage.dirtyOverlayHandles.size());
			storage.lastUpdateTelemetry.spatialRebuildFallbackCount = 1u;
		}
		else
		{
			storage.staticBuckets.clear();
			storage.staticBucketCells.clear();
			for (const auto& [handle, primitive] : storage.records)
			{
				if (primitive.primitiveClass == SceneSpatialIndexPrimitiveClass::Static)
					InsertBucket(storage.staticBuckets, storage.staticBucketCells, primitive);
			}
			storage.dirtyOverlayHandles.clear();
			storage.useLastGoodStaticBuckets = false;
			storage.lastUpdateTelemetry.staticIndexRebuildCount = 1u;
		}

		storage.dynamicBuckets.clear();
		storage.dynamicBucketCells.clear();
		storage.frustumUnboundedHandles.clear();
		for (const auto& [handle, primitive] : storage.records)
		{
			if (primitive.primitiveClass == SceneSpatialIndexPrimitiveClass::Dynamic)
				InsertBucket(storage.dynamicBuckets, storage.dynamicBucketCells, primitive);
			if (primitive.frustumBehaviour == Components::MeshRenderer::EFrustumBehaviour::DISABLED)
				storage.frustumUnboundedHandles.push_back(handle);
		}
		SortAndUnique(storage.frustumUnboundedHandles);
		storage.lastUpdateTelemetry.dynamicIndexUpdateCount = 1u;
		storage.initialized = true;
	}

	void SceneSpatialIndex::UpdateChanged(
		const ScenePrimitiveSnapshot& changedPrimitives,
		const std::vector<SceneSpatialIndexPrimitiveMetadata>& metadata,
		const SceneSpatialIndexUpdateOptions& options)
	{
		auto& storage = *m_storage;
		storage.BeginUpdate();
		if (!storage.initialized)
		{
			Update(changedPrimitives, metadata);
			return;
		}

		bool touchedStatic = false;
		for (const auto& handle : changedPrimitives.removedHandles)
		{
			if (const auto previous = storage.records.find(handle); previous != storage.records.end())
			{
				if (previous->second.primitiveClass == SceneSpatialIndexPrimitiveClass::Dynamic)
					storage.lastUpdateTelemetry.dynamicIndexUpdateCount = 1u;
				else
				{
					storage.lastUpdateTelemetry.staticIndexRefitCount = 1u;
					touchedStatic = true;
				}
			}
			storage.RemovePrimitive(handle);
		}

		const auto metadataByHandle = BuildMetadataMap(metadata);
		bool touchedDynamic = false;
		for (const auto& record : changedPrimitives.primitiveRecords)
		{
			if (!record.handle.IsValid() || !record.occupied || record.tombstoned)
			{
				if (const auto previous = storage.records.find(record.handle); previous != storage.records.end())
				{
					if (previous->second.primitiveClass == SceneSpatialIndexPrimitiveClass::Dynamic)
						storage.lastUpdateTelemetry.dynamicIndexUpdateCount = 1u;
					else
					{
						storage.lastUpdateTelemetry.staticIndexRefitCount = 1u;
						touchedStatic = true;
					}
				}
				storage.RemovePrimitive(record.handle);
				continue;
			}

			const auto indexed = BuildIndexedPrimitive(record, metadataByHandle);
			if (indexed.primitiveClass == SceneSpatialIndexPrimitiveClass::Dynamic)
				touchedDynamic = true;

			const auto previous = storage.records.find(indexed.handle);
			if (previous != storage.records.end() && SameSpatialRecord(previous->second, indexed))
				continue;

			if (indexed.primitiveClass == SceneSpatialIndexPrimitiveClass::Static)
			{
				storage.lastUpdateTelemetry.staticIndexRefitCount = 1u;
				storage.lastUpdateTelemetry.staticIndexDirtyOverlayCount += 1u;
				touchedStatic = true;
			}
			storage.ReplacePrimitive(indexed);
		}
		if (touchedDynamic)
			storage.lastUpdateTelemetry.dynamicIndexUpdateCount = 1u;

		if (storage.useLastGoodStaticBuckets && (!touchedStatic || options.rebuildBudgetUs == 0u))
			storage.RebuildStaticBucketsFromRecords();
	}

	VisibilityCandidateSet SceneSpatialIndex::Query(const SceneSpatialIndexQuery& query) const
	{
		const auto& storage = *m_storage;
		VisibilityCandidateSet result;
		if (storage.records.empty())
		{
			result.fallbackReason = SceneSpatialIndexFallbackReason::Empty;
			return result;
		}

		std::unordered_set<ScenePrimitiveHandle, HandleHash> visited;
		auto visitHandle = [&](const ScenePrimitiveHandle& handle, const bool dynamicRecord)
		{
			if (!visited.insert(handle).second)
				return;

			const auto found = storage.records.find(handle);
			if (found == storage.records.end())
				return;

			const auto& primitive = found->second;
			++result.primitiveRecordsTouched;
			if (dynamicRecord)
				++result.dynamicRecordsTouched;

			if (!QueryPasses(primitive, query))
				return;

			result.candidatePrimitiveHandles.push_back(handle);
			if (dynamicRecord)
				++result.dynamicCandidateCount;
		};

		auto visitBuckets = [&](const CellBuckets& buckets, const bool dynamicBuckets)
		{
			const auto radius = std::max(0.0f, query.radius);
			const auto minCell = ToCell(query.center - Maths::Vector3(radius, radius, radius));
			const auto maxCell = ToCell(query.center + Maths::Vector3(radius, radius, radius));
			VisitQueryCells(buckets, minCell, maxCell, dynamicBuckets, visitHandle);
		};

		visitBuckets(storage.staticBuckets, false);
		if (storage.useLastGoodStaticBuckets)
		{
			result.telemetry.staticIndexLastGoodQueryCount = 1u;
			result.telemetry.staticIndexDirtyOverlayCount =
				static_cast<uint64_t>(storage.dirtyOverlayHandles.size());
			result.telemetry.spatialRebuildFallbackCount = 1u;
			result.fallbackReason = SceneSpatialIndexFallbackReason::RebuildBudgetExceeded;
			for (const auto& handle : storage.dirtyOverlayHandles)
				visitHandle(handle, false);
		}

		visitBuckets(storage.dynamicBuckets, true);
		for (const auto& handle : storage.frustumUnboundedHandles)
			visitHandle(handle, false);

		SortAndUnique(result.candidatePrimitiveHandles);
		result.candidateCount = static_cast<uint64_t>(result.candidatePrimitiveHandles.size());
		result.telemetry.spatialCandidateCount = result.candidateCount;
		result.telemetry.primitiveRecordsTouched = result.primitiveRecordsTouched;
		result.telemetry.visibilityTestedPrimitiveCount = result.candidateCount;
		result.telemetry.dynamicCandidateCount = result.dynamicCandidateCount;
		result.telemetry.dynamicRecordsTouched = result.dynamicRecordsTouched;
		result.telemetry.staticIndexRefitCount = storage.lastUpdateTelemetry.staticIndexRefitCount;
		result.telemetry.staticIndexRebuildCount = storage.lastUpdateTelemetry.staticIndexRebuildCount;
		result.telemetry.dynamicIndexUpdateCount = storage.lastUpdateTelemetry.dynamicIndexUpdateCount;
		return result;
	}

	VisibilityCandidateSet SceneSpatialIndex::FullScanForComparison(const SceneSpatialIndexQuery& query) const
	{
		const auto& storage = *m_storage;
		VisibilityCandidateSet result;
		result.fullScanCandidateCount = static_cast<uint64_t>(storage.records.size());
		result.primitiveRecordsTouched = static_cast<uint64_t>(storage.records.size());
		result.telemetry.fullScanCandidateCount = result.fullScanCandidateCount;

		for (const auto& [handle, primitive] : storage.records)
		{
			if (!QueryPasses(primitive, query))
				continue;

			result.candidatePrimitiveHandles.push_back(handle);
			if (primitive.primitiveClass == SceneSpatialIndexPrimitiveClass::Dynamic)
				++result.dynamicCandidateCount;
		}

		SortAndUnique(result.candidatePrimitiveHandles);
		result.candidateCount = static_cast<uint64_t>(result.candidatePrimitiveHandles.size());
		result.telemetry.spatialCandidateCount = 0u;
		result.telemetry.primitiveRecordsTouched = result.primitiveRecordsTouched;
		result.telemetry.visibilityTestedPrimitiveCount = result.candidateCount;
		result.telemetry.dynamicCandidateCount = result.dynamicCandidateCount;
		return result;
	}

	size_t SceneSpatialIndex::GetStaticPrimitiveCount() const
	{
		const auto& storage = *m_storage;
		return storage.staticPrimitiveCount;
	}

	bool SceneSpatialIndex::IsInitialized() const
	{
		return m_storage->initialized;
	}

	NLS::Render::Data::LargeSceneTelemetry SceneSpatialIndex::GetLastUpdateTelemetry() const
	{
		return m_storage->lastUpdateTelemetry;
	}

	size_t SceneSpatialIndex::GetDynamicPrimitiveCount() const
	{
		const auto& storage = *m_storage;
		return storage.dynamicPrimitiveCount;
	}
}
