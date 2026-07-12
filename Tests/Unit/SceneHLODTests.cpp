#include <gtest/gtest.h>

#include "Rendering/SceneHLOD.h"
#include "Rendering/SceneStreamingResidency.h"

namespace
{
	using NLS::Engine::Rendering::HLODClusterRecord;
	using NLS::Engine::Rendering::HLODCompatibilityFlags;
	using NLS::Engine::Rendering::RepresentationResidencySnapshot;
	using NLS::Engine::Rendering::SceneHLODClusterHandle;
	using NLS::Engine::Rendering::SceneHLODSystem;
	using NLS::Engine::Rendering::SceneHLODViewInput;
	using NLS::Engine::Rendering::ScenePrimitiveHandle;

	ScenePrimitiveHandle MakeHandle(const uint32_t index)
	{
		return { 0x53u, index, 1u };
	}

	HLODClusterRecord MakeCluster()
	{
		HLODClusterRecord cluster;
		cluster.clusterHandle = { 3u };
		cluster.childPrimitives = { MakeHandle(0u), MakeHandle(1u), MakeHandle(2u) };
		cluster.proxyPrimitive = MakeHandle(30u);
		cluster.worldReferencePoint = { 0.0f, 0.0f, -200.0f };
		cluster.worldSize = 80.0f;
		cluster.activationScreenRelativeSize = 0.50f;
		cluster.compatibilityFlags = HLODCompatibilityFlags::OpaqueOnly | HLODCompatibilityFlags::ProxySafe;
		return cluster;
	}
}

TEST(SceneHLODTests, ReadyProxySuppressesResidentChildrenForDistantView)
{
	const auto cluster = MakeCluster();
	ASSERT_TRUE(cluster.proxyPrimitive.has_value());
	RepresentationResidencySnapshot residency;
	residency.MarkHLODProxyReady(*cluster.proxyPrimitive);
	for (const auto& child : cluster.childPrimitives)
		residency.MarkReady(child);

	SceneHLODViewInput input;
	input.cameraPosition = { 0.0f, 0.0f, 0.0f };
	input.allowHLOD = true;

	const auto result = SceneHLODSystem::SelectCluster(input, cluster, residency);

	EXPECT_TRUE(result.usesProxy);
	EXPECT_EQ(result.proxyPrimitive, cluster.proxyPrimitive);
	EXPECT_EQ(result.suppressedChildPrimitives, cluster.childPrimitives);
	EXPECT_TRUE(result.streamingInterest.empty());
}

TEST(SceneHLODTests, MissingProxyFallsBackToChildrenAndEmitsReadOnlyInterest)
{
	const auto cluster = MakeCluster();
	ASSERT_TRUE(cluster.proxyPrimitive.has_value());
	RepresentationResidencySnapshot residency;
	for (const auto& child : cluster.childPrimitives)
		residency.MarkReady(child);

	SceneHLODViewInput input;
	input.cameraPosition = { 0.0f, 0.0f, 0.0f };
	input.allowHLOD = true;

	const auto result = SceneHLODSystem::SelectCluster(input, cluster, residency);

	EXPECT_FALSE(result.usesProxy);
	EXPECT_TRUE(result.suppressedChildPrimitives.empty());
	ASSERT_EQ(result.streamingInterest.size(), 1u);
	EXPECT_EQ(result.streamingInterest.front(), cluster.proxyPrimitive);
}

TEST(SceneHLODTests, TransparentOrOrderDependentChildrenRequireExplicitProxySafety)
{
	auto cluster = MakeCluster();
	ASSERT_TRUE(cluster.proxyPrimitive.has_value());
	cluster.compatibilityFlags = HLODCompatibilityFlags::ContainsTransparent |
		HLODCompatibilityFlags::ContainsOrderDependent;

	RepresentationResidencySnapshot residency;
	residency.MarkHLODProxyReady(*cluster.proxyPrimitive);
	for (const auto& child : cluster.childPrimitives)
		residency.MarkReady(child);

	SceneHLODViewInput input;
	input.cameraPosition = { 0.0f, 0.0f, 0.0f };
	input.allowHLOD = true;

	auto result = SceneHLODSystem::SelectCluster(input, cluster, residency);

	EXPECT_FALSE(result.usesProxy);
	EXPECT_TRUE(result.suppressedChildPrimitives.empty());

	cluster.compatibilityFlags = cluster.compatibilityFlags | HLODCompatibilityFlags::TransparentProxySafe;
	result = SceneHLODSystem::SelectCluster(input, cluster, residency);

	EXPECT_TRUE(result.usesProxy);
	EXPECT_EQ(result.suppressedChildPrimitives, cluster.childPrimitives);
}

TEST(SceneHLODTests, EditorSelectedChildrenOverrideSuppressionPerView)
{
	const auto cluster = MakeCluster();
	ASSERT_TRUE(cluster.proxyPrimitive.has_value());
	RepresentationResidencySnapshot residency;
	residency.MarkHLODProxyReady(*cluster.proxyPrimitive);
	for (const auto& child : cluster.childPrimitives)
		residency.MarkReady(child);

	SceneHLODViewInput input;
	input.cameraPosition = { 0.0f, 0.0f, 0.0f };
	input.allowHLOD = true;
	input.editorInspectionView = true;
	input.selectedPrimitiveHandles = { cluster.childPrimitives[1] };

	const auto result = SceneHLODSystem::SelectCluster(input, cluster, residency);

	EXPECT_TRUE(result.usesProxy);
	ASSERT_EQ(result.suppressedChildPrimitives.size(), 2u);
	EXPECT_EQ(result.suppressedChildPrimitives[0], cluster.childPrimitives[0]);
	EXPECT_EQ(result.suppressedChildPrimitives[1], cluster.childPrimitives[2]);
	EXPECT_EQ(result.inspectableChildPrimitives, input.selectedPrimitiveHandles);
}

TEST(SceneHLODTests, EditorInspectableClusterKeepsAllChildrenVisible)
{
	const auto cluster = MakeCluster();
	ASSERT_TRUE(cluster.proxyPrimitive.has_value());
	RepresentationResidencySnapshot residency;
	residency.MarkHLODProxyReady(*cluster.proxyPrimitive);
	for (const auto& child : cluster.childPrimitives)
		residency.MarkReady(child);

	SceneHLODViewInput input;
	input.cameraPosition = { 0.0f, 0.0f, 0.0f };
	input.allowHLOD = true;
	input.editorInspectionView = true;
	input.forceInspectableHLODClusters = { cluster.clusterHandle };

	const auto result = SceneHLODSystem::SelectCluster(input, cluster, residency);

	EXPECT_FALSE(result.usesProxy);
	EXPECT_FALSE(result.proxyPrimitive.has_value());
	EXPECT_TRUE(result.suppressedChildPrimitives.empty())
		<< "Selecting an imported prefab or HLOD root in the editor must keep the generated children inspectable.";
	EXPECT_EQ(result.inspectableChildPrimitives, cluster.childPrimitives);
}
