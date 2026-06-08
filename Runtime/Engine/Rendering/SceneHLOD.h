#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <Math/Vector3.h>

#include "EngineDef.h"
#include "Rendering/RenderScene.h"
#include "Rendering/SceneStreamingResidency.h"

namespace NLS::Engine::Rendering
{
	enum class HLODCompatibilityFlags : uint32_t
	{
		None = 0u,
		OpaqueOnly = 1u << 0u,
		ContainsTransparent = 1u << 1u,
		ContainsOrderDependent = 1u << 2u,
		ContainsSkinned = 1u << 3u,
		ContainsAnimated = 1u << 4u,
		ContainsEditorOnly = 1u << 5u,
		ProxySafe = 1u << 6u,
		TransparentProxySafe = 1u << 7u
	};

	[[nodiscard]] NLS_ENGINE_API HLODCompatibilityFlags operator|(
		HLODCompatibilityFlags lhs,
		HLODCompatibilityFlags rhs);
	[[nodiscard]] NLS_ENGINE_API bool HasFlag(HLODCompatibilityFlags value, HLODCompatibilityFlags flag);

	struct HLODClusterRecord
	{
		SceneHLODClusterHandle clusterHandle;
		std::vector<ScenePrimitiveHandle> childPrimitives;
		std::optional<ScenePrimitiveHandle> proxyPrimitive;
		Maths::Vector3 worldReferencePoint {};
		float worldSize = 1.0f;
		float activationScreenRelativeSize = 0.0f;
		HLODCompatibilityFlags compatibilityFlags = HLODCompatibilityFlags::None;
	};

	struct SceneHLODViewInput
	{
		Maths::Vector3 cameraPosition {};
		bool allowHLOD = false;
		bool editorInspectionView = false;
		std::vector<ScenePrimitiveHandle> selectedPrimitiveHandles;
	};

	struct HLODSelectionResult
	{
		bool usesProxy = false;
		std::optional<ScenePrimitiveHandle> proxyPrimitive;
		std::vector<ScenePrimitiveHandle> suppressedChildPrimitives;
		std::vector<ScenePrimitiveHandle> inspectableChildPrimitives;
		std::vector<ScenePrimitiveHandle> streamingInterest;
		float screenRelativeSize = 0.0f;
	};

	class NLS_ENGINE_API SceneHLODSystem
	{
	public:
		[[nodiscard]] static HLODSelectionResult SelectCluster(
			const SceneHLODViewInput& input,
			const HLODClusterRecord& cluster,
			const RepresentationResidencySnapshot& residency);
	};
}
