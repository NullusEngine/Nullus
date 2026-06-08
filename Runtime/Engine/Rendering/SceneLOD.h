#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <Math/Vector3.h>

#include "EngineDef.h"
#include "Rendering/RenderScene.h"

namespace NLS::Engine::Rendering
{
	struct LODLevelRecord
	{
		float screenRelativeThreshold = 0.0f;
		std::vector<ScenePrimitiveHandle> primitiveHandles;
		float fadeDurationSeconds = 0.0f;
	};

	struct LODGroupRecord
	{
		SceneLODGroupHandle groupHandle;
		std::vector<LODLevelRecord> levels;
		float hysteresis = 0.0f;
		Maths::Vector3 worldReferencePoint {};
		float worldSize = 1.0f;
		std::optional<uint32_t> forcedLOD;
	};

	struct SceneLODViewInput
	{
		Maths::Vector3 cameraPosition {};
		float lodBias = 1.0f;
	};

	struct LODSelectionHistory
	{
		bool hasSelection = false;
		uint32_t selectedLOD = 0u;
	};

	struct LODSelectionResult
	{
		uint32_t selectedLOD = 0u;
		std::vector<ScenePrimitiveHandle> activePrimitiveHandles;
		float screenRelativeSize = 0.0f;
		bool usedHysteresis = false;
		bool usedForcedLOD = false;
	};

	class NLS_ENGINE_API SceneLODSystem
	{
	public:
		[[nodiscard]] static LODSelectionResult Select(
			const SceneLODViewInput& input,
			const LODGroupRecord& group,
			LODSelectionHistory* history);
	};
}
