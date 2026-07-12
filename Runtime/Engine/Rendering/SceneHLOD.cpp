#include "Rendering/SceneHLOD.h"

#include <algorithm>
#include <limits>

namespace NLS::Engine::Rendering
{
HLODCompatibilityFlags operator|(const HLODCompatibilityFlags lhs, const HLODCompatibilityFlags rhs)
{
	return static_cast<HLODCompatibilityFlags>(
		static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

bool HasFlag(const HLODCompatibilityFlags value, const HLODCompatibilityFlags flag)
{
	return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0u;
}

namespace
{
	float ComputeScreenRelativeSize(const SceneHLODViewInput& input, const HLODClusterRecord& cluster)
	{
		const auto distance = std::max(
			Maths::Vector3::Distance(input.cameraPosition, cluster.worldReferencePoint),
			std::numeric_limits<float>::epsilon());
		return std::max(0.0f, cluster.worldSize) / distance;
	}

	bool ContainsHandle(const std::vector<ScenePrimitiveHandle>& handles, const ScenePrimitiveHandle handle)
	{
		return std::find(handles.begin(), handles.end(), handle) != handles.end();
	}

	bool ContainsClusterHandle(
		const std::vector<SceneHLODClusterHandle>& handles,
		const SceneHLODClusterHandle handle)
	{
		return std::find(handles.begin(), handles.end(), handle) != handles.end();
	}

	bool IsProxyCompatible(const HLODClusterRecord& cluster)
	{
		if (!HasFlag(cluster.compatibilityFlags, HLODCompatibilityFlags::ProxySafe) &&
			!HasFlag(cluster.compatibilityFlags, HLODCompatibilityFlags::TransparentProxySafe))
		{
			return false;
		}
		if (HasFlag(cluster.compatibilityFlags, HLODCompatibilityFlags::ContainsSkinned) ||
			HasFlag(cluster.compatibilityFlags, HLODCompatibilityFlags::ContainsAnimated) ||
			HasFlag(cluster.compatibilityFlags, HLODCompatibilityFlags::ContainsEditorOnly))
		{
			return false;
		}
		if ((HasFlag(cluster.compatibilityFlags, HLODCompatibilityFlags::ContainsTransparent) ||
			 HasFlag(cluster.compatibilityFlags, HLODCompatibilityFlags::ContainsOrderDependent)) &&
			!HasFlag(cluster.compatibilityFlags, HLODCompatibilityFlags::TransparentProxySafe))
		{
			return false;
		}
		return true;
	}
}

HLODSelectionResult SceneHLODSystem::SelectCluster(
	const SceneHLODViewInput& input,
	const HLODClusterRecord& cluster,
	const RepresentationResidencySnapshot& residency)
{
	HLODSelectionResult result;
	result.screenRelativeSize = ComputeScreenRelativeSize(input, cluster);

	if (!input.allowHLOD ||
		cluster.childPrimitives.empty() ||
		!cluster.proxyPrimitive.has_value() ||
		result.screenRelativeSize > cluster.activationScreenRelativeSize ||
		!IsProxyCompatible(cluster))
	{
		return result;
	}

	if (input.editorInspectionView &&
		ContainsClusterHandle(input.forceInspectableHLODClusters, cluster.clusterHandle))
	{
		result.inspectableChildPrimitives = cluster.childPrimitives;
		return result;
	}

	if (!residency.IsHLODProxyReady(*cluster.proxyPrimitive))
	{
		if (!residency.IsReady(*cluster.proxyPrimitive) && !residency.HasFallback(*cluster.proxyPrimitive))
			result.streamingInterest.push_back(*cluster.proxyPrimitive);
		return result;
	}

	result.usesProxy = true;
	result.proxyPrimitive = cluster.proxyPrimitive;
	for (const auto& child : cluster.childPrimitives)
	{
		if (input.editorInspectionView && ContainsHandle(input.selectedPrimitiveHandles, child))
		{
			result.inspectableChildPrimitives.push_back(child);
			continue;
		}
		result.suppressedChildPrimitives.push_back(child);
	}
	return result;
}
}
