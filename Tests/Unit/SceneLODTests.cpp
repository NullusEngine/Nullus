#include <gtest/gtest.h>

#include <cmath>

#include "Rendering/SceneLOD.h"

namespace
{
	using NLS::Engine::Rendering::LODGroupRecord;
	using NLS::Engine::Rendering::LODLevelRecord;
	using NLS::Engine::Rendering::LODSelectionHistory;
	using NLS::Engine::Rendering::SceneLODGroupHandle;
	using NLS::Engine::Rendering::SceneLODSystem;
	using NLS::Engine::Rendering::SceneLODViewInput;
	using NLS::Engine::Rendering::ScenePrimitiveHandle;

	ScenePrimitiveHandle MakeHandle(const uint32_t index)
	{
		return { 0x41u, index, 1u };
	}

	LODGroupRecord MakeThreeLevelGroup()
	{
		LODGroupRecord group;
		group.groupHandle = { 7u };
		group.worldReferencePoint = { 0.0f, 0.0f, -100.0f };
		group.worldSize = 20.0f;
		group.levels = {
			LODLevelRecord { 0.50f, { MakeHandle(0u) } },
			LODLevelRecord { 0.20f, { MakeHandle(1u) } },
			LODLevelRecord { 0.00f, { MakeHandle(2u) } }
		};
		return group;
	}
}

TEST(SceneLODTests, SelectsScreenRelativeLevelWithViewBias)
{
	auto group = MakeThreeLevelGroup();
	SceneLODViewInput input;
	input.cameraPosition = { 0.0f, 0.0f, 0.0f };

	auto result = SceneLODSystem::Select(input, group, nullptr);

	ASSERT_EQ(result.selectedLOD, 1u);
	ASSERT_EQ(result.activePrimitiveHandles, group.levels[1].primitiveHandles);
	EXPECT_FLOAT_EQ(result.screenRelativeSize, 0.2f);

	input.lodBias = 3.0f;
	result = SceneLODSystem::Select(input, group, nullptr);

	ASSERT_EQ(result.selectedLOD, 0u);
	ASSERT_EQ(result.activePrimitiveHandles, group.levels[0].primitiveHandles);
}

TEST(SceneLODTests, HysteresisKeepsPreviousLevelInsideStabilityBand)
{
	auto group = MakeThreeLevelGroup();
	group.hysteresis = 0.05f;

	SceneLODViewInput input;
	input.cameraPosition = { 0.0f, 0.0f, -60.0f };

	LODSelectionHistory history;
	history.hasSelection = true;
	history.selectedLOD = 1u;

	auto result = SceneLODSystem::Select(input, group, &history);

	EXPECT_EQ(result.selectedLOD, 1u);
	EXPECT_TRUE(result.usedHysteresis);

	input.cameraPosition = { 0.0f, 0.0f, -65.0f };
	result = SceneLODSystem::Select(input, group, &history);

	EXPECT_EQ(result.selectedLOD, 0u);
	EXPECT_FALSE(result.usedHysteresis);
}

TEST(SceneLODTests, ForcedLODOverridesThresholdsAndClampsToAvailableLevels)
{
	auto group = MakeThreeLevelGroup();
	group.forcedLOD = 99u;

	SceneLODViewInput input;
	input.cameraPosition = { 0.0f, 0.0f, 0.0f };

	const auto result = SceneLODSystem::Select(input, group, nullptr);

	EXPECT_TRUE(result.usedForcedLOD);
	EXPECT_EQ(result.selectedLOD, 2u);
    EXPECT_EQ(result.activePrimitiveHandles, group.levels[2].primitiveHandles);
}

TEST(SceneLODTests, UsesProjectionAwareBoundsSphereWhenViewProjectionIsProvided)
{
    auto group = MakeThreeLevelGroup();
    group.levels[0].screenRelativeThreshold = 0.25f;
    group.levels[1].screenRelativeThreshold = 0.05f;
    group.worldSize = 999.0f;
    group.boundsSphereRadius = 10.0f;

    SceneLODViewInput input;
    input.cameraPosition = {0.0f, 0.0f, 0.0f};
    input.verticalFovRadians = 1.57079632679f;

    const auto result = SceneLODSystem::Select(input, group, nullptr);

    EXPECT_NEAR(result.screenRelativeSize, 0.2f, 0.0001f);
    EXPECT_EQ(result.selectedLOD, 1u);
}

TEST(SceneLODTests, AppliesMinAndQualityMaxLODConstraints)
{
    auto group = MakeThreeLevelGroup();
    group.minLOD = 1u;
    group.maxLOD = 1u;

    SceneLODViewInput input;
    input.cameraPosition = {0.0f, 0.0f, -99.0f};

    const auto result = SceneLODSystem::Select(input, group, nullptr);

    EXPECT_EQ(result.selectedLOD, 1u);
    EXPECT_TRUE(result.usedLODConstraint);
}

TEST(SceneLODTests, FallsBackToResidentLODWhenSelectedResourceIsUnavailable)
{
    auto group = MakeThreeLevelGroup();
    group.levels[1].resident = false;

    SceneLODViewInput input;
    input.cameraPosition = {0.0f, 0.0f, 0.0f};

    const auto result = SceneLODSystem::Select(input, group, nullptr);

    EXPECT_EQ(result.selectedLOD, 2u);
    EXPECT_TRUE(result.usedResidencyFallback);
}
