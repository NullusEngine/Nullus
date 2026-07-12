#pragma once

#include <cstddef>
#include <cstdint>

#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
	enum class LargeSceneSettingId : uint8_t
	{
		EnableSpatialIndex,
		EnableParallelVisibility,
		EnableLOD,
		EnableHLOD,
		EnableHZBOcclusion,
		MaxVisibilityJobs,
		ParallelVisibilityPrimitiveThreshold,
		ParallelVisibilityPrimitivesPerTask,
		StaticRebuildDirtyRatio,
		StaticRebuildBudgetUs,
		StreamingCpuBudgetUs,
		StreamingGpuUploadBudgetBytes,
		StreamingIoBudgetBytes,
		StreamingCpuMemoryBudgetBytes,
		StreamingGpuMemoryBudgetBytes,
		MaxOcclusionHistoryAge
	};

	struct NLS_ENGINE_API LargeSceneSettings
	{
		bool enableSpatialIndex = true;
		bool enableParallelVisibility = true;
		bool enableLOD = true;
		bool enableHLOD = false;
		bool enableHZBOcclusion = false;
		uint32_t maxVisibilityJobs = 0u;
		size_t parallelVisibilityPrimitiveThreshold = 1024u;
		size_t parallelVisibilityPrimitivesPerTask = 128u;
		double staticRebuildDirtyRatio = 0.20;
		uint64_t staticRebuildBudgetUs = 0u;
		uint64_t streamingCpuBudgetUs = 1000u;
		uint64_t streamingGpuUploadBudgetBytes = 16ull * 1024ull * 1024ull;
		uint64_t streamingIoBudgetBytes = 32ull * 1024ull * 1024ull;
		uint64_t streamingCpuMemoryBudgetBytes = 1024ull * 1024ull * 1024ull;
		uint64_t streamingGpuMemoryBudgetBytes = 1024ull * 1024ull * 1024ull;
		uint32_t maxOcclusionHistoryAge = 2u;

		[[nodiscard]] static LargeSceneSettings Defaults();
		[[nodiscard]] static const char* DebugLabel(LargeSceneSettingId setting);
	};
}
