#include "Rendering/SceneLOD.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace NLS::Engine::Rendering
{
namespace
{
	uint32_t ClampLOD(const uint32_t lod, const size_t levelCount)
	{
		if (levelCount == 0u)
			return 0u;
		return std::min<uint32_t>(lod, static_cast<uint32_t>(levelCount - 1u));
	}

	float ComputeScreenRelativeSize(const SceneLODViewInput& input, const LODGroupRecord& group)
	{
		const auto distance = std::max(
			Maths::Vector3::Distance(input.cameraPosition, group.worldReferencePoint),
			std::numeric_limits<float>::epsilon());
		const auto bias = std::max(0.0f, input.lodBias);
		return std::max(0.0f, group.worldSize) / distance * bias;
	}

	uint32_t SelectByThreshold(const std::vector<LODLevelRecord>& levels, const float screenRelativeSize)
	{
		if (levels.empty())
			return 0u;

		for (size_t index = 0u; index < levels.size(); ++index)
		{
			if (screenRelativeSize >= levels[index].screenRelativeThreshold)
				return static_cast<uint32_t>(index);
		}
		return static_cast<uint32_t>(levels.size() - 1u);
	}
}

LODSelectionResult SceneLODSystem::Select(
	const SceneLODViewInput& input,
	const LODGroupRecord& group,
	LODSelectionHistory* history)
{
	LODSelectionResult result;
	result.screenRelativeSize = ComputeScreenRelativeSize(input, group);

	if (group.levels.empty())
		return result;

	if (group.forcedLOD.has_value())
	{
		result.selectedLOD = ClampLOD(*group.forcedLOD, group.levels.size());
		result.usedForcedLOD = true;
	}
	else
	{
		result.selectedLOD = SelectByThreshold(group.levels, result.screenRelativeSize);
		if (history != nullptr &&
			history->hasSelection &&
			history->selectedLOD < group.levels.size())
		{
			const auto boundaryLOD = std::min(result.selectedLOD, history->selectedLOD);
			const auto boundaryThreshold = group.levels[boundaryLOD].screenRelativeThreshold;
			if (std::abs(result.screenRelativeSize - boundaryThreshold) <= group.hysteresis)
			{
				result.selectedLOD = history->selectedLOD;
				result.usedHysteresis = true;
			}
		}
	}

	result.activePrimitiveHandles = group.levels[result.selectedLOD].primitiveHandles;
	if (history != nullptr)
	{
		history->hasSelection = true;
		history->selectedLOD = result.selectedLOD;
	}
	return result;
}
}
