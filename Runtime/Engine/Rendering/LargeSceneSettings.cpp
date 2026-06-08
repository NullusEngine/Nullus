#include "Rendering/LargeSceneSettings.h"

namespace NLS::Engine::Rendering
{
	LargeSceneSettings LargeSceneSettings::Defaults()
	{
		return {};
	}

	const char* LargeSceneSettings::DebugLabel(const LargeSceneSettingId setting)
	{
		switch (setting)
		{
		case LargeSceneSettingId::EnableSpatialIndex:
			return "enableSpatialIndex";
		case LargeSceneSettingId::EnableParallelVisibility:
			return "enableParallelVisibility";
		case LargeSceneSettingId::EnableLOD:
			return "enableLOD";
		case LargeSceneSettingId::EnableHLOD:
			return "enableHLOD";
		case LargeSceneSettingId::EnableHZBOcclusion:
			return "enableHZBOcclusion";
		case LargeSceneSettingId::MaxVisibilityJobs:
			return "maxVisibilityJobs";
		case LargeSceneSettingId::ParallelVisibilityPrimitiveThreshold:
			return "parallelVisibilityPrimitiveThreshold";
		case LargeSceneSettingId::ParallelVisibilityPrimitivesPerTask:
			return "parallelVisibilityPrimitivesPerTask";
		case LargeSceneSettingId::StaticRebuildDirtyRatio:
			return "staticRebuildDirtyRatio";
		case LargeSceneSettingId::StaticRebuildBudgetUs:
			return "staticRebuildBudgetUs";
		case LargeSceneSettingId::StreamingCpuBudgetUs:
			return "streamingCpuBudgetUs";
		case LargeSceneSettingId::StreamingGpuUploadBudgetBytes:
			return "streamingGpuUploadBudgetBytes";
		case LargeSceneSettingId::StreamingIoBudgetBytes:
			return "streamingIoBudgetBytes";
		case LargeSceneSettingId::StreamingCpuMemoryBudgetBytes:
			return "streamingCpuMemoryBudgetBytes";
		case LargeSceneSettingId::StreamingGpuMemoryBudgetBytes:
			return "streamingGpuMemoryBudgetBytes";
		case LargeSceneSettingId::MaxOcclusionHistoryAge:
			return "maxOcclusionHistoryAge";
		default:
			return "unknown";
		}
	}
}
