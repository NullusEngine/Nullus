#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <Math/Vector3.h>
#include <Rendering/Data/FrameInfo.h>

#include "EngineDef.h"
#include "Rendering/RenderScene.h"

namespace NLS::Engine::Rendering
{
	enum class SceneSpatialIndexPrimitiveClass : uint8_t
	{
		Static,
		Dynamic
	};

	enum class SceneSpatialIndexFallbackReason : uint8_t
	{
		None,
		Disabled,
		Empty,
		RebuildBudgetExceeded,
		NoLastGoodIndex
	};

	struct SceneSpatialIndexUpdateOptions
	{
		double staticRebuildDirtyRatio = 0.20;
		uint64_t rebuildBudgetUs = 0u;
	};

	struct SceneSpatialIndexPrimitiveMetadata
	{
		ScenePrimitiveHandle handle;
		SceneSpatialIndexPrimitiveClass primitiveClass = SceneSpatialIndexPrimitiveClass::Static;
		bool active = true;
	};

	struct SceneSpatialIndexQuery
	{
		Maths::Vector3 center;
		float radius = 0.0f;
		uint32_t visibleLayerMask = 0xFFFF'FFFFu;
		bool distanceCullingEnabled = false;
		float minDistance = 0.0f;
		float maxDistance = 0.0f;
	};

	struct VisibilityCandidateSet
	{
		std::vector<ScenePrimitiveHandle> candidatePrimitiveHandles;
		uint64_t candidateCount = 0u;
		uint64_t fullScanCandidateCount = 0u;
		uint64_t primitiveRecordsTouched = 0u;
		uint64_t dynamicCandidateCount = 0u;
		uint64_t dynamicRecordsTouched = 0u;
		NLS::Render::Data::LargeSceneTelemetry telemetry;
		SceneSpatialIndexFallbackReason fallbackReason = SceneSpatialIndexFallbackReason::None;
	};

	class NLS_ENGINE_API SceneSpatialIndex
	{
	public:
		SceneSpatialIndex();
		~SceneSpatialIndex();
		SceneSpatialIndex(const SceneSpatialIndex&) = delete;
		SceneSpatialIndex& operator=(const SceneSpatialIndex&) = delete;
		SceneSpatialIndex(SceneSpatialIndex&&) noexcept;
		SceneSpatialIndex& operator=(SceneSpatialIndex&&) noexcept;

		void Update(
			const ScenePrimitiveSnapshot& primitives,
			const std::vector<SceneSpatialIndexPrimitiveMetadata>& metadata = {},
			const SceneSpatialIndexUpdateOptions& options = {});
		void UpdateChanged(
			const ScenePrimitiveSnapshot& changedPrimitives,
			const std::vector<SceneSpatialIndexPrimitiveMetadata>& metadata = {},
			const SceneSpatialIndexUpdateOptions& options = {});

		[[nodiscard]] VisibilityCandidateSet Query(const SceneSpatialIndexQuery& query) const;
		[[nodiscard]] VisibilityCandidateSet FullScanForComparison(const SceneSpatialIndexQuery& query) const;

		[[nodiscard]] bool IsInitialized() const;
		[[nodiscard]] NLS::Render::Data::LargeSceneTelemetry GetLastUpdateTelemetry() const;
		[[nodiscard]] size_t GetStaticPrimitiveCount() const;
		[[nodiscard]] size_t GetDynamicPrimitiveCount() const;

	private:
		class Storage;
		std::unique_ptr<Storage> m_storage;
	};
}
