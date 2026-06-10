#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Math/Matrix4.h"
#include "Rendering/SceneOcclusion.h"
#include "Rendering/RHI/RHITypes.h"

namespace
{
	using NLS::Engine::Rendering::SceneOcclusionFallbackReason;
	using NLS::Engine::Rendering::SceneOcclusionCapabilityRequest;
	using NLS::Engine::Rendering::SceneOcclusionFrameInput;
	using NLS::Engine::Rendering::SceneOcclusionHistory;
	using NLS::Engine::Rendering::SceneOcclusionHistoryKey;
	using NLS::Engine::Rendering::SceneOcclusionObservationBatch;
	using NLS::Engine::Rendering::SceneOcclusionPrimitivePacketBuildInput;
	using NLS::Engine::Rendering::SceneOcclusionPrimitivePacketSource;
	using NLS::Engine::Rendering::SceneOcclusionPrimitiveInput;
	using NLS::Engine::Rendering::SceneOcclusionSystem;
	using NLS::Engine::Rendering::ScenePrimitiveHandle;
	using NLS::Engine::Rendering::ScenePrimitiveSnapshot;
	using NLS::Engine::Rendering::ScenePrimitiveSnapshotRecord;
	using NLS::Render::RHI::RHIDeviceCapabilities;
	using NLS::Render::RHI::RHIDeviceFeature;
	using NLS::Render::RHI::TextureFormat;
	using NLS::Render::RHI::TextureFormatCapability;

	constexpr uint64_t kSceneId = 0x91u;

	ScenePrimitiveHandle MakeHandle(const uint32_t index, const uint32_t generation = 1u)
	{
		return { kSceneId, index, generation };
	}

	SceneOcclusionFrameInput MakeFrameInput()
	{
		SceneOcclusionFrameInput input;
		input.enabled = true;
		input.backendSupported = true;
		input.historyTextureValid = true;
		input.frameSerial = 10u;
		input.maxHistoryAge = 2u;
		input.viewKey = 7u;
		input.viewCompatibilityHash = 0xA11CEu;
		input.projectionHash = 0xBEEFu;
		input.jitterHash = 0x1234u;
		input.depthFormatKey = 24u;
		input.viewportWidth = 1920u;
		input.viewportHeight = 1080u;
		return input;
	}

	SceneOcclusionPrimitiveInput MakePrimitive(const ScenePrimitiveHandle handle)
	{
		SceneOcclusionPrimitiveInput primitive;
		primitive.handle = handle;
		primitive.boundsGeneration = 3u;
		primitive.transformGeneration = 4u;
		primitive.representationId = 5u;
		primitive.depthWriteEligibilityGeneration = 6u;
		primitive.depthWriteEligible = true;
		return primitive;
	}

	void ExpectHandleEq(const ScenePrimitiveHandle& actual, const ScenePrimitiveHandle& expected)
	{
		EXPECT_EQ(actual.sceneId, expected.sceneId);
		EXPECT_EQ(actual.index, expected.index);
		EXPECT_EQ(actual.generation, expected.generation);
	}

	std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		return std::string(
			std::istreambuf_iterator<char>(stream),
			std::istreambuf_iterator<char>());
	}

	std::string RemoveWhitespace(const std::string& text)
	{
		std::string result;
		result.reserve(text.size());
		for (const unsigned char character : text)
		{
			if (!std::isspace(character))
				result.push_back(static_cast<char>(character));
		}
		return result;
	}

	RHIDeviceCapabilities MakeOcclusionCapabilities()
	{
		RHIDeviceCapabilities capabilities;
		capabilities.SetFeature(RHIDeviceFeature::BackendReady, true);
		capabilities.SetFeature(RHIDeviceFeature::Compute, true);
		capabilities.SetFeature(RHIDeviceFeature::ExplicitBarriers, true);
		capabilities.SetFeature(RHIDeviceFeature::HierarchicalZBuffer, true);
		capabilities.SetFeature(RHIDeviceFeature::ConservativeOcclusion, true);
		capabilities.SetFeature(RHIDeviceFeature::AsyncReadback, true);

		TextureFormatCapability depth;
		depth.sampled = true;
		capabilities.SetTextureFormatCapability(TextureFormat::Depth32F, depth);

		TextureFormatCapability hzb;
		hzb.sampled = true;
		hzb.storage = true;
		capabilities.SetTextureFormatCapability(TextureFormat::R32F, hzb);

		capabilities.SynchronizeLegacyFields();
		return capabilities;
	}
}

TEST(SceneOcclusionTests, InvalidHistoryKeepsPrimitiveConservativelyVisible)
{
	SceneOcclusionHistory history;
	const auto handle = MakeHandle(0u);
	const std::vector<SceneOcclusionPrimitiveInput> primitives { MakePrimitive(handle) };

	const auto result = SceneOcclusionSystem::Evaluate(MakeFrameInput(), primitives, history);

	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), handle);
	EXPECT_TRUE(result.occludedPrimitiveHandles.empty());
	ASSERT_EQ(result.primitiveResults.size(), 1u);
	EXPECT_EQ(result.primitiveResults.front().handle, handle);
	EXPECT_FALSE(result.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(result.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::MissingHistory);
}

TEST(SceneOcclusionTests, ValidRecentHistoryCanCullPrimitive)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	const auto primitive = MakePrimitive(MakeHandle(1u));
	history.RecordOccluded(SceneOcclusionSystem::BuildHistoryKey(input, primitive), input.frameSerial - 1u);

	const auto result = SceneOcclusionSystem::Evaluate(input, { primitive }, history);

	ASSERT_EQ(result.occludedPrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.occludedPrimitiveHandles.front(), primitive.handle);
	EXPECT_TRUE(result.visiblePrimitiveHandles.empty());
	ASSERT_EQ(result.primitiveResults.size(), 1u);
	EXPECT_TRUE(result.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(result.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::None);
}

TEST(SceneOcclusionTests, VisibleObservationClearsPreviousOccludedHistoryForSameKey)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	const auto primitive = MakePrimitive(MakeHandle(8u));
	const auto key = SceneOcclusionSystem::BuildHistoryKey(input, primitive);
	history.RecordOccluded(key, input.frameSerial - 1u);

	auto occluded = SceneOcclusionSystem::Evaluate(input, { primitive }, history);
	ASSERT_EQ(occluded.primitiveResults.size(), 1u);
	ASSERT_TRUE(occluded.primitiveResults.front().culledByOcclusion);

	history.RecordVisible(key);

	const auto visible = SceneOcclusionSystem::Evaluate(input, { primitive }, history);
	ASSERT_EQ(visible.primitiveResults.size(), 1u);
	EXPECT_FALSE(visible.primitiveResults.front().culledByOcclusion);
	ASSERT_EQ(visible.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(visible.visiblePrimitiveHandles.front(), primitive.handle);
}

TEST(SceneOcclusionTests, HistoryPrunesOldPerHandleKeysAndKeepsLatestKeyClassifiable)
{
	SceneOcclusionHistory history;
	auto input = MakeFrameInput();
	input.frameSerial = 200u;
	input.maxHistoryAge = 1000u;
	auto primitive = MakePrimitive(MakeHandle(108u));

	for (uint64_t generation = 0u; generation < 96u; ++generation)
	{
		primitive.transformGeneration = generation + 1u;
		history.RecordOccluded(
			SceneOcclusionSystem::BuildHistoryKey(input, primitive),
			100u + generation);
	}

	primitive.transformGeneration = 96u;
	const auto latest = SceneOcclusionSystem::Evaluate(input, { primitive }, history);
	ASSERT_EQ(latest.primitiveResults.size(), 1u);
	EXPECT_TRUE(latest.primitiveResults.front().culledByOcclusion);

	primitive.transformGeneration = 1u;
	const auto oldest = SceneOcclusionSystem::Evaluate(input, { primitive }, history);
	ASSERT_EQ(oldest.primitiveResults.size(), 1u);
	EXPECT_FALSE(oldest.primitiveResults.front().culledByOcclusion);
	EXPECT_NE(oldest.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::None);
}

TEST(SceneOcclusionTests, HistoryPrunesRemovedHandlesAndGenerationsToLiveSet)
{
	SceneOcclusionHistory history;
	auto input = MakeFrameInput();
	input.frameSerial = 300u;
	input.maxHistoryAge = 1000u;

	const auto liveHandle = MakeHandle(7u, 2u);
	const auto oldGenerationHandle = MakeHandle(7u, 1u);
	const auto removedHandle = MakeHandle(8u, 1u);
	const auto otherSceneHandle = ScenePrimitiveHandle{ kSceneId + 1u, 7u, 2u };

	const auto livePrimitive = MakePrimitive(liveHandle);
	history.RecordOccluded(
		SceneOcclusionSystem::BuildHistoryKey(input, livePrimitive),
		input.frameSerial - 1u);
	history.RecordOccluded(
		SceneOcclusionSystem::BuildHistoryKey(input, MakePrimitive(oldGenerationHandle)),
		input.frameSerial - 1u);
	history.RecordOccluded(
		SceneOcclusionSystem::BuildHistoryKey(input, MakePrimitive(removedHandle)),
		input.frameSerial - 1u);
	history.RecordOccluded(
		SceneOcclusionSystem::BuildHistoryKey(input, MakePrimitive(otherSceneHandle)),
		input.frameSerial - 1u);

	const auto stats = history.PruneByLiveHandleSweepForLifecycleFallback(
		std::span<const ScenePrimitiveHandle>{ &liveHandle, 1u });

	EXPECT_EQ(stats.trackedHandleCountBefore, 4u);
	EXPECT_EQ(stats.trackedHandleCountAfter, 1u);
	EXPECT_EQ(stats.removedHandleCount, 3u);
	EXPECT_EQ(stats.removedKeyCount, 3u);

	const auto live = SceneOcclusionSystem::Evaluate(input, { livePrimitive }, history);
	ASSERT_EQ(live.primitiveResults.size(), 1u);
	EXPECT_TRUE(live.primitiveResults.front().culledByOcclusion);

	const auto removed = SceneOcclusionSystem::Evaluate(input, { MakePrimitive(removedHandle) }, history);
	ASSERT_EQ(removed.primitiveResults.size(), 1u);
	EXPECT_FALSE(removed.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(removed.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::MissingHistory);
}

TEST(SceneOcclusionTests, ObservationBatchUpdatesHistoryWithoutCurrentFrameReadbackFlags)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	const auto visiblePrimitive = MakePrimitive(MakeHandle(9u));
	const auto hiddenPrimitive = MakePrimitive(MakeHandle(10u));
	const auto visibleKey = SceneOcclusionSystem::BuildHistoryKey(input, visiblePrimitive);
	const auto hiddenKey = SceneOcclusionSystem::BuildHistoryKey(input, hiddenPrimitive);
	history.RecordOccluded(visibleKey, input.frameSerial - 1u);

	const auto stats = SceneOcclusionSystem::ApplyObservationResults(
		history,
		{
			{ visibleKey, input.frameSerial, false },
			{ hiddenKey, input.frameSerial, true }
		});

	EXPECT_EQ(stats.observedPrimitiveCount, 2u);
	EXPECT_EQ(stats.visiblePrimitiveCount, 1u);
	EXPECT_EQ(stats.occludedPrimitiveCount, 1u);
	EXPECT_FALSE(stats.usedSynchronousReadback);
	EXPECT_FALSE(stats.waitedForGpuFence);
	EXPECT_FALSE(stats.blockedOnReadbackMap);
	EXPECT_FALSE(stats.requestedCurrentFrameReadback);

	const auto result = SceneOcclusionSystem::Evaluate(
		input,
		{ visiblePrimitive, hiddenPrimitive },
		history);
	ASSERT_EQ(result.primitiveResults.size(), 2u);
	EXPECT_FALSE(result.primitiveResults[0].culledByOcclusion);
	EXPECT_TRUE(result.primitiveResults[1].culledByOcclusion);
}

TEST(SceneOcclusionTests, HZBPrimitivePacketsProjectWorldBoundsIntoScreenRectAndNearestDepth)
{
	SceneOcclusionPrimitivePacketBuildInput buildInput;
	buildInput.viewProjection = NLS::Maths::Matrix4::Identity;
	buildInput.viewportWidth = 100u;
	buildInput.viewportHeight = 50u;

	SceneOcclusionPrimitivePacketSource source;
	source.primitive = MakePrimitive(MakeHandle(18u));
	source.modelBounds.center = { 0.0f, 0.0f, 0.5f };
	source.modelBounds.size = { 0.5f, 0.5f, 0.5f };
	source.worldMatrix = NLS::Maths::Matrix4::Identity;

	auto ineligible = source;
	ineligible.primitive.handle = MakeHandle(19u);
	ineligible.primitive.depthWriteEligible = false;

	const auto packets = SceneOcclusionSystem::BuildHZBPrimitivePackets(
		buildInput,
		{ source, ineligible });

	ASSERT_EQ(packets.primitiveInputs.size(), 1u);
	ASSERT_EQ(packets.primitivePackets.size(), 1u);
	EXPECT_EQ(packets.primitiveInputs.front().handle, source.primitive.handle);
	EXPECT_EQ(packets.rejectedPrimitiveCount, 1u);

	const auto& packet = packets.primitivePackets.front();
	EXPECT_FLOAT_EQ(packet.screenMinX, 37.5f);
	EXPECT_FLOAT_EQ(packet.screenMinY, 18.75f);
	EXPECT_FLOAT_EQ(packet.screenMaxX, 62.5f);
	EXPECT_FLOAT_EQ(packet.screenMaxY, 31.25f);
	EXPECT_FLOAT_EQ(packet.nearestDepth, 0.25f);
	EXPECT_EQ(packet.flags, 1u);
}

TEST(SceneOcclusionTests, HZBPrimitivePacketsUseModelAABBInsteadOfBoundingSphere)
{
	SceneOcclusionPrimitivePacketBuildInput buildInput;
	buildInput.viewProjection = NLS::Maths::Matrix4::Identity;
	buildInput.viewportWidth = 100u;
	buildInput.viewportHeight = 100u;

	SceneOcclusionPrimitivePacketSource source;
	source.primitive = MakePrimitive(MakeHandle(118u));
	source.modelBounds.center = { 0.0f, 0.0f, 0.5f };
	source.modelBounds.size = { 0.2f, 0.4f, 0.2f };
	source.worldMatrix = NLS::Maths::Matrix4::Identity;

	const auto packets = SceneOcclusionSystem::BuildHZBPrimitivePackets(buildInput, { source });

	ASSERT_EQ(packets.primitivePackets.size(), 1u);
	const auto& packet = packets.primitivePackets.front();
	EXPECT_FLOAT_EQ(packet.screenMinX, 45.0f);
	EXPECT_FLOAT_EQ(packet.screenMaxX, 55.0f);
	EXPECT_FLOAT_EQ(packet.screenMinY, 40.0f);
	EXPECT_FLOAT_EQ(packet.screenMaxY, 60.0f);
	EXPECT_FLOAT_EQ(packet.nearestDepth, 0.4f);
}

TEST(SceneOcclusionTests, HZBPrimitivePacketsProjectPerspectiveBoundsWithoutFullscreenFallback)
{
	SceneOcclusionPrimitivePacketBuildInput buildInput;
	buildInput.viewProjection = NLS::Maths::Matrix4::CreatePerspective(90.0f, 1.0f, 0.1f, 100.0f);
	buildInput.viewportWidth = 100u;
	buildInput.viewportHeight = 100u;

	SceneOcclusionPrimitivePacketSource source;
	source.primitive = MakePrimitive(MakeHandle(119u));
	source.modelBounds.center = { 0.0f, 0.0f, -10.0f };
	source.modelBounds.size = { 0.5f, 0.5f, 0.5f };
	source.worldMatrix = NLS::Maths::Matrix4::Identity;

	const auto packets = SceneOcclusionSystem::BuildHZBPrimitivePackets(buildInput, { source });

	ASSERT_EQ(packets.primitiveInputs.size(), 1u);
	ASSERT_EQ(packets.primitivePackets.size(), 1u);
	EXPECT_EQ(packets.rejectedPrimitiveCount, 0u);

	const auto& packet = packets.primitivePackets.front();
	EXPECT_GT(packet.screenMinX, 45.0f);
	EXPECT_GT(packet.screenMinY, 45.0f);
	EXPECT_LT(packet.screenMaxX, 55.0f);
	EXPECT_LT(packet.screenMaxY, 55.0f);
	EXPECT_LT((packet.screenMaxX - packet.screenMinX + 1.0f) * (packet.screenMaxY - packet.screenMinY + 1.0f), 64.0f);
	EXPECT_LT(packet.nearestDepth, 1.0f);
}

TEST(SceneOcclusionTests, HZBPrimitivePacketsRejectNearPlaneCrossingBoundsConservatively)
{
	SceneOcclusionPrimitivePacketBuildInput buildInput;
	buildInput.viewProjection = NLS::Maths::Matrix4::Identity;
	buildInput.viewProjection.data[14] = 1.0f;
	buildInput.viewProjection.data[15] = 0.0f;
	buildInput.viewportWidth = 100u;
	buildInput.viewportHeight = 50u;

	SceneOcclusionPrimitivePacketSource source;
	source.primitive = MakePrimitive(MakeHandle(118u));
	source.modelBounds.center = { 0.0f, 0.0f, 0.5f };
	source.modelBounds.size = { 1.5f, 1.5f, 1.5f };
	source.worldMatrix = NLS::Maths::Matrix4::Identity;

	const auto packets = SceneOcclusionSystem::BuildHZBPrimitivePackets(buildInput, { source });

	EXPECT_TRUE(packets.primitiveInputs.empty());
	EXPECT_TRUE(packets.primitivePackets.empty());
	EXPECT_EQ(packets.rejectedPrimitiveCount, 1u);
}

TEST(SceneOcclusionTests, HZBPrimitivePacketsRejectFullyOffscreenBoundsConservatively)
{
	SceneOcclusionPrimitivePacketBuildInput buildInput;
	buildInput.viewProjection = NLS::Maths::Matrix4::Identity;
	buildInput.viewportWidth = 100u;
	buildInput.viewportHeight = 50u;

	SceneOcclusionPrimitivePacketSource source;
	source.primitive = MakePrimitive(MakeHandle(120u));
	source.modelBounds.center = { 3.0f, 0.0f, 0.5f };
	source.modelBounds.size = { 0.5f, 0.5f, 0.5f };
	source.worldMatrix = NLS::Maths::Matrix4::Identity;

	const auto packets = SceneOcclusionSystem::BuildHZBPrimitivePackets(buildInput, { source });

	EXPECT_TRUE(packets.primitiveInputs.empty());
	EXPECT_TRUE(packets.primitivePackets.empty());
	EXPECT_EQ(packets.rejectedPrimitiveCount, 1u);
}

TEST(SceneOcclusionTests, HZBPrimitivePacketsRejectScreenEdgeCrossingBoundsConservatively)
{
	SceneOcclusionPrimitivePacketBuildInput buildInput;
	buildInput.viewProjection = NLS::Maths::Matrix4::Identity;
	buildInput.viewportWidth = 100u;
	buildInput.viewportHeight = 50u;

	SceneOcclusionPrimitivePacketSource source;
	source.primitive = MakePrimitive(MakeHandle(121u));
	source.modelBounds.center = { 0.90f, 0.0f, 0.5f };
	source.modelBounds.size = { 0.40f, 0.40f, 0.40f };
	source.worldMatrix = NLS::Maths::Matrix4::Identity;

	const auto packets = SceneOcclusionSystem::BuildHZBPrimitivePackets(buildInput, { source });

	EXPECT_TRUE(packets.primitiveInputs.empty());
	EXPECT_TRUE(packets.primitivePackets.empty());
	EXPECT_EQ(packets.rejectedPrimitiveCount, 1u);
}

TEST(SceneOcclusionTests, HZBPrimitivePacketSourcesFollowVisibleSnapshotAndStableInvalidationHashes)
{
	ScenePrimitiveSnapshot snapshot;
	snapshot.sceneId = kSceneId;
	snapshot.snapshotSerial = 7u;
	snapshot.frameSerial = 11u;

	auto makeRecord = [](const ScenePrimitiveHandle handle, const float x, const bool depthWriteEligible)
	{
		ScenePrimitiveSnapshotRecord record;
		record.handle = handle;
		record.modelBoundingSphere.position = { 0.0f, 0.0f, 0.5f };
		record.modelBoundingSphere.radius = 0.25f;
		record.modelBounds.center = { 0.0f, 0.0f, 0.5f };
		record.modelBounds.size = { 0.5f, 0.5f, 0.5f };
		record.worldMatrix = NLS::Maths::Matrix4::Translation({ x, 0.0f, 0.0f });
		record.occupied = true;
		record.tombstoned = false;
		record.ownerAlive = true;
		record.ownerActive = true;
		record.hasMeshBinding = true;
		record.hasValidMaterial = true;
		record.depthWriteEligibleForOcclusion = depthWriteEligible;
		return record;
	};

	const auto visibleHandle = MakeHandle(20u);
	const auto transparentHandle = MakeHandle(21u);
	const auto missingHandle = MakeHandle(22u);
	snapshot.primitiveRecords.push_back(makeRecord(visibleHandle, 0.0f, true));
	snapshot.primitiveRecords.push_back(makeRecord(transparentHandle, 2.0f, false));

	const auto sources = SceneOcclusionSystem::BuildHZBPrimitivePacketSources(
		snapshot,
		{ visibleHandle, transparentHandle, missingHandle });

	ASSERT_EQ(sources.sources.size(), 1u);
	EXPECT_EQ(sources.sources.front().primitive.handle, visibleHandle);
	EXPECT_TRUE(sources.sources.front().primitive.depthWriteEligible);
	EXPECT_NE(sources.sources.front().primitive.boundsGeneration, 0u);
	EXPECT_NE(sources.sources.front().primitive.transformGeneration, 0u);
	EXPECT_NE(sources.sources.front().primitive.depthWriteEligibilityGeneration, 0u);
	EXPECT_EQ(sources.rejectedPrimitiveCount, 2u);

	const auto stableSources = SceneOcclusionSystem::BuildHZBPrimitivePacketSources(
		snapshot,
		{ visibleHandle });
	ASSERT_EQ(stableSources.sources.size(), 1u);
	EXPECT_EQ(
		stableSources.sources.front().primitive.boundsGeneration,
		sources.sources.front().primitive.boundsGeneration);
	EXPECT_EQ(
		stableSources.sources.front().primitive.transformGeneration,
		sources.sources.front().primitive.transformGeneration);

	auto movedSnapshot = snapshot;
	movedSnapshot.primitiveRecords.front().worldMatrix =
		NLS::Maths::Matrix4::Translation({ 1.0f, 0.0f, 0.0f });
	const auto movedSources = SceneOcclusionSystem::BuildHZBPrimitivePacketSources(
		movedSnapshot,
		{ visibleHandle });
	ASSERT_EQ(movedSources.sources.size(), 1u);
	EXPECT_EQ(
		movedSources.sources.front().primitive.boundsGeneration,
		sources.sources.front().primitive.boundsGeneration);
	EXPECT_NE(
		movedSources.sources.front().primitive.transformGeneration,
		sources.sources.front().primitive.transformGeneration);

	auto resizedSnapshot = snapshot;
	resizedSnapshot.primitiveRecords.front().modelBounds.size = { 0.25f, 0.5f, 0.5f };
	const auto resizedSources = SceneOcclusionSystem::BuildHZBPrimitivePacketSources(
		resizedSnapshot,
		{ visibleHandle });
	ASSERT_EQ(resizedSources.sources.size(), 1u);
	EXPECT_NE(
		resizedSources.sources.front().primitive.boundsGeneration,
		sources.sources.front().primitive.boundsGeneration);
	EXPECT_EQ(
		resizedSources.sources.front().primitive.transformGeneration,
		sources.sources.front().primitive.transformGeneration);
}

TEST(SceneOcclusionTests, HZBObservationCandidatesRetainPreviouslyObservedPrimitives)
{
	const auto visibleHandle = MakeHandle(24u);
	const auto previouslyObservedHandle = MakeHandle(25u);
	const auto duplicateHandle = MakeHandle(26u);
	const auto otherSceneHandle = ScenePrimitiveHandle{ kSceneId + 1u, 27u, 1u };

	std::vector<SceneOcclusionPrimitiveInput> previousInputs;
	previousInputs.push_back(MakePrimitive(previouslyObservedHandle));
	previousInputs.push_back(MakePrimitive(duplicateHandle));
	previousInputs.push_back(MakePrimitive(otherSceneHandle));

	const auto candidates = SceneOcclusionSystem::BuildHZBObservationCandidateHandles(
		{ visibleHandle, duplicateHandle },
		previousInputs,
		kSceneId);

	ASSERT_EQ(candidates.size(), 3u);
	ExpectHandleEq(candidates[0], visibleHandle);
	ExpectHandleEq(candidates[1], duplicateHandle);
	ExpectHandleEq(candidates[2], previouslyObservedHandle);
}

TEST(SceneOcclusionTests, HZBObservationCandidatesRebuildPacketsForPreviouslyObservedLivePrimitive)
{
	ScenePrimitiveSnapshot snapshot;
	snapshot.sceneId = kSceneId;
	snapshot.snapshotSerial = 9u;
	snapshot.frameSerial = 13u;

	const auto retainedHandle = MakeHandle(28u);
	ScenePrimitiveSnapshotRecord record;
	record.handle = retainedHandle;
	record.modelBoundingSphere.position = { 0.0f, 0.0f, 0.5f };
	record.modelBoundingSphere.radius = 0.25f;
	record.modelBounds.center = { 0.0f, 0.0f, 0.5f };
	record.modelBounds.size = { 0.5f, 0.5f, 0.5f };
	record.worldMatrix = NLS::Maths::Matrix4::Identity;
	record.occupied = true;
	record.tombstoned = false;
	record.ownerAlive = true;
	record.ownerActive = true;
	record.hasMeshBinding = true;
	record.hasValidMaterial = true;
	record.depthWriteEligibleForOcclusion = true;
	snapshot.primitiveRecords.push_back(record);

	std::vector<SceneOcclusionPrimitiveInput> previousInputs;
	previousInputs.push_back(MakePrimitive(retainedHandle));

	const auto candidates = SceneOcclusionSystem::BuildHZBObservationCandidateHandles(
		{},
		previousInputs,
		kSceneId);
	const auto sources = SceneOcclusionSystem::BuildHZBPrimitivePacketSources(snapshot, candidates);

	SceneOcclusionPrimitivePacketBuildInput buildInput;
	buildInput.viewProjection = NLS::Maths::Matrix4::Identity;
	buildInput.viewportWidth = 100u;
	buildInput.viewportHeight = 50u;
	const auto packets = SceneOcclusionSystem::BuildHZBPrimitivePackets(buildInput, sources.sources);

	ASSERT_EQ(candidates.size(), 1u);
	ExpectHandleEq(candidates.front(), retainedHandle);
	ASSERT_EQ(sources.sources.size(), 1u);
	ASSERT_EQ(packets.primitiveInputs.size(), 1u);
	ASSERT_EQ(packets.primitivePackets.size(), 1u);
	ExpectHandleEq(packets.primitiveInputs.front().handle, retainedHandle);
}

TEST(SceneOcclusionTests, BaseSceneRendererBuildsHZBPacketsFromRetainedObservationCandidates)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/BaseSceneRenderer.cpp");

	const auto hzbBlock = source.find("if (occlusionSettings.enableHZBOcclusion)");
	ASSERT_NE(hzbBlock, std::string::npos);
	const auto streamingBlock = source.find("RegisterRuntimeStreamingDependencies", hzbBlock);
	ASSERT_NE(streamingBlock, std::string::npos);
	const auto body = source.substr(hzbBlock, streamingBlock - hzbBlock);
	const auto normalizedBody = RemoveWhitespace(body);

	EXPECT_NE(body.find("candidateStart"), std::string::npos);
	EXPECT_NE(body.find("BuildHZBObservationCandidateHandles"), std::string::npos);
	EXPECT_NE(body.find("previousSceneHZBOcclusionPrimitiveInputs"), std::string::npos);
	EXPECT_NE(body.find("renderScene.GetSceneId()"), std::string::npos);
	EXPECT_NE(body.find("hzbBuildTimeNs += ElapsedNanoseconds(candidateStart)"), std::string::npos);
	EXPECT_NE(
		normalizedBody.find(RemoveWhitespace(
			"CreatePrimitiveSnapshotForHandles(hzbObservationCandidateHandles, {})")),
		std::string::npos);
	EXPECT_NE(
		normalizedBody.find(RemoveWhitespace(
			"BuildHZBPrimitivePacketSources(hzbPrimitiveSnapshot, hzbObservationCandidateHandles)")),
		std::string::npos);
	EXPECT_EQ(
		normalizedBody.find(RemoveWhitespace(
			"BuildHZBPrimitivePacketSources(hzbPrimitiveSnapshot, renderScene.GetLastVisiblePrimitiveHandles())")),
		std::string::npos);
}

TEST(SceneOcclusionTests, BaseSceneRendererMovesPreviousHZBInputsInsteadOfCopyingLargeFrameVectors)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/BaseSceneRenderer.cpp");

	EXPECT_NE(
		source.find("std::move(m_lastHZBOcclusionPrimitivePacketBuildResult.primitiveInputs)"),
		std::string::npos);
	EXPECT_NE(
		source.find("previousHZBOcclusionPrimitiveInputsByScene[sceneId].push_back(std::move(input))"),
		std::string::npos);
	EXPECT_EQ(
		source.find("const auto previousHZBOcclusionPrimitiveInputs =\n\t\tm_lastHZBOcclusionPrimitivePacketBuildResult.primitiveInputs"),
		std::string::npos);
	EXPECT_EQ(
		source.find("previousHZBOcclusionPrimitiveInputsByScene[input.handle.sceneId].push_back(input)"),
		std::string::npos);
}

TEST(SceneOcclusionTests, BaseSceneRendererUsesStableHZBViewKeyInsteadOfCameraObjectAddress)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/BaseSceneRenderer.cpp");

	EXPECT_NE(source.find("BuildHZBViewKey(m_frameDescriptor)"), std::string::npos);
	EXPECT_EQ(source.find("reinterpret_cast<uintptr_t>(&camera)"), std::string::npos);
}

TEST(SceneOcclusionTests, BaseSceneRendererLogsHZBObservationReadbackFlagDistribution)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/BaseSceneRenderer.cpp");

	const auto functionStart = source.find("BaseSceneRenderer::CompleteHZBOcclusionObservationFrame");
	ASSERT_NE(functionStart, std::string::npos);
	const auto functionEnd = source.find("void BaseSceneRenderer::RefreshSceneLightingDescriptor", functionStart);
	ASSERT_NE(functionEnd, std::string::npos);
	const auto body = source.substr(functionStart, functionEnd - functionStart);

	EXPECT_NE(body.find("[BaseSceneRenderer][HZBObservation]"), std::string::npos);
	EXPECT_NE(body.find("gpuOccludedFlags"), std::string::npos);
	EXPECT_NE(body.find("appliedOccluded"), std::string::npos);
	EXPECT_NE(body.find("incompatibleView"), std::string::npos);
	EXPECT_NE(body.find("std::count_if"), std::string::npos);
}

TEST(SceneOcclusionTests, ObservationBatchRejectsStaleFrameAndIncompatibleViewResults)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	const auto validPrimitive = MakePrimitive(MakeHandle(11u));
	const auto stalePrimitive = MakePrimitive(MakeHandle(12u));
	const auto wrongViewPrimitive = MakePrimitive(MakeHandle(13u));
	const auto validKey = SceneOcclusionSystem::BuildHistoryKey(input, validPrimitive);
	const auto staleKey = SceneOcclusionSystem::BuildHistoryKey(input, stalePrimitive);
	auto wrongView = input;
	++wrongView.viewCompatibilityHash;
	const auto wrongViewKey = SceneOcclusionSystem::BuildHistoryKey(wrongView, wrongViewPrimitive);

	const auto stats = SceneOcclusionSystem::ApplyObservationResults(
		history,
		input,
		{
			{ validKey, input.frameSerial, true },
			{ staleKey, input.frameSerial - 1u, true },
			{ wrongViewKey, input.frameSerial, true }
		});

	EXPECT_EQ(stats.observedPrimitiveCount, 3u);
	EXPECT_EQ(stats.occludedPrimitiveCount, 1u);
	EXPECT_EQ(stats.visiblePrimitiveCount, 0u);
	EXPECT_EQ(stats.discardedPrimitiveCount, 2u);
	EXPECT_EQ(stats.staleFrameCount, 1u);
	EXPECT_EQ(stats.incompatibleViewCount, 1u);

	const auto result = SceneOcclusionSystem::Evaluate(
		input,
		{ validPrimitive, stalePrimitive, wrongViewPrimitive },
		history);
	ASSERT_EQ(result.primitiveResults.size(), 3u);
	EXPECT_TRUE(result.primitiveResults[0].culledByOcclusion);
	EXPECT_FALSE(result.primitiveResults[1].culledByOcclusion);
	EXPECT_FALSE(result.primitiveResults[2].culledByOcclusion);
}

TEST(SceneOcclusionTests, HZBViewCompatibilityHashDoesNotUseCameraPose)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/BaseSceneRenderer.cpp");

	const auto functionStart = source.find("uint64_t HashCameraViewCompatibility");
	ASSERT_NE(functionStart, std::string::npos);
	const auto functionEnd = source.find("uint64_t ResolveDepthFormatKey", functionStart);
	ASSERT_NE(functionEnd, std::string::npos);
	const auto body = source.substr(functionStart, functionEnd - functionStart);

	EXPECT_EQ(body.find("GetPosition()"), std::string::npos);
	EXPECT_EQ(body.find("GetRotation()"), std::string::npos);
	EXPECT_NE(body.find("GetNear()"), std::string::npos);
	EXPECT_NE(body.find("GetFar()"), std::string::npos);
	EXPECT_NE(body.find("GetProjectionMode()"), std::string::npos);
}

TEST(SceneOcclusionTests, GpuPrimitiveResultFlagsBuildObservationBatchForValidatedFrameMerge)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	const auto visiblePrimitive = MakePrimitive(MakeHandle(14u));
	const auto hiddenPrimitive = MakePrimitive(MakeHandle(15u));
	const std::vector<SceneOcclusionPrimitiveInput> primitiveInputs {
		visiblePrimitive,
		hiddenPrimitive
	};

	const auto observations = SceneOcclusionSystem::BuildObservationsFromPrimitiveResultFlags(
		input,
		primitiveInputs,
		{ 0u, 1u, 1u });

	ASSERT_EQ(observations.size(), 2u);
	EXPECT_EQ(observations[0].key, SceneOcclusionSystem::BuildHistoryKey(input, visiblePrimitive));
	EXPECT_EQ(observations[0].frameSerial, input.frameSerial);
	EXPECT_FALSE(observations[0].occluded);
	EXPECT_EQ(observations[1].key, SceneOcclusionSystem::BuildHistoryKey(input, hiddenPrimitive));
	EXPECT_EQ(observations[1].frameSerial, input.frameSerial);
	EXPECT_TRUE(observations[1].occluded);

	const auto stats = SceneOcclusionSystem::ApplyObservationResults(history, input, observations);
	EXPECT_EQ(stats.observedPrimitiveCount, 2u);
	EXPECT_EQ(stats.visiblePrimitiveCount, 1u);
	EXPECT_EQ(stats.occludedPrimitiveCount, 1u);
	EXPECT_FALSE(stats.usedSynchronousReadback);
	EXPECT_FALSE(stats.waitedForGpuFence);
	EXPECT_FALSE(stats.blockedOnReadbackMap);
	EXPECT_FALSE(stats.requestedCurrentFrameReadback);

	const auto result = SceneOcclusionSystem::Evaluate(input, primitiveInputs, history);
	ASSERT_EQ(result.primitiveResults.size(), 2u);
	EXPECT_FALSE(result.primitiveResults[0].culledByOcclusion);
	EXPECT_TRUE(result.primitiveResults[1].culledByOcclusion);
}

TEST(SceneOcclusionTests, PendingObservationBatchMergesOnlyAfterAsyncResultsAreReady)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	const auto visiblePrimitive = MakePrimitive(MakeHandle(16u));
	const auto hiddenPrimitive = MakePrimitive(MakeHandle(17u));
	const std::vector<SceneOcclusionPrimitiveInput> primitiveInputs {
		visiblePrimitive,
		hiddenPrimitive
	};

	const SceneOcclusionObservationBatch pending =
		SceneOcclusionSystem::CreatePendingObservationBatch(input, primitiveInputs);

	EXPECT_FALSE(pending.ready);
	EXPECT_EQ(pending.frame.frameSerial, input.frameSerial);
	EXPECT_EQ(pending.primitiveInputs.size(), 2u);

	const auto pendingStats = SceneOcclusionSystem::ApplyReadyObservationBatch(
		history,
		input,
		pending);
	EXPECT_EQ(pendingStats.observedPrimitiveCount, 0u);
	EXPECT_EQ(pendingStats.discardedPrimitiveCount, 2u);
	EXPECT_FALSE(pendingStats.usedSynchronousReadback);
	EXPECT_FALSE(pendingStats.waitedForGpuFence);
	EXPECT_FALSE(pendingStats.blockedOnReadbackMap);
	EXPECT_FALSE(pendingStats.requestedCurrentFrameReadback);

	auto beforeReady = SceneOcclusionSystem::Evaluate(input, primitiveInputs, history);
	ASSERT_EQ(beforeReady.primitiveResults.size(), 2u);
	EXPECT_FALSE(beforeReady.primitiveResults[0].culledByOcclusion);
	EXPECT_FALSE(beforeReady.primitiveResults[1].culledByOcclusion);

	const SceneOcclusionObservationBatch ready =
		SceneOcclusionSystem::CompleteObservationBatchWithPrimitiveResultFlags(pending, { 0u, 1u });

	EXPECT_TRUE(ready.ready);
	EXPECT_EQ(ready.observations.size(), 2u);
	EXPECT_EQ(ready.primitiveInputs.size(), 2u);

	const auto readyStats = SceneOcclusionSystem::ApplyReadyObservationBatch(
		history,
		input,
		ready);
	EXPECT_EQ(readyStats.observedPrimitiveCount, 2u);
	EXPECT_EQ(readyStats.visiblePrimitiveCount, 1u);
	EXPECT_EQ(readyStats.occludedPrimitiveCount, 1u);
	EXPECT_EQ(readyStats.discardedPrimitiveCount, 0u);
	EXPECT_FALSE(readyStats.usedSynchronousReadback);
	EXPECT_FALSE(readyStats.waitedForGpuFence);
	EXPECT_FALSE(readyStats.blockedOnReadbackMap);
	EXPECT_FALSE(readyStats.requestedCurrentFrameReadback);

	const auto afterReady = SceneOcclusionSystem::Evaluate(input, primitiveInputs, history);
	ASSERT_EQ(afterReady.primitiveResults.size(), 2u);
	EXPECT_FALSE(afterReady.primitiveResults[0].culledByOcclusion);
	EXPECT_TRUE(afterReady.primitiveResults[1].culledByOcclusion);
}

TEST(SceneOcclusionTests, MovedPrimitiveRepresentationOrViewChangeInvalidatesHistory)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	auto primitive = MakePrimitive(MakeHandle(2u));
	history.RecordOccluded(SceneOcclusionSystem::BuildHistoryKey(input, primitive), input.frameSerial - 1u);

	auto movedPrimitive = primitive;
	++movedPrimitive.transformGeneration;
	auto moved = SceneOcclusionSystem::Evaluate(input, { movedPrimitive }, history);
	ASSERT_EQ(moved.primitiveResults.size(), 1u);
	EXPECT_FALSE(moved.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(moved.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::PrimitiveChanged);

	auto representationChanged = primitive;
	++representationChanged.representationId;
	auto representation = SceneOcclusionSystem::Evaluate(input, { representationChanged }, history);
	ASSERT_EQ(representation.primitiveResults.size(), 1u);
	EXPECT_FALSE(representation.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(representation.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::RepresentationChanged);

	auto changedView = input;
	++changedView.projectionHash;
	auto view = SceneOcclusionSystem::Evaluate(changedView, { primitive }, history);
	ASSERT_EQ(view.primitiveResults.size(), 1u);
	EXPECT_FALSE(view.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(view.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::ViewChanged);
}

TEST(SceneOcclusionTests, JitterDepthFormatAndViewportChangesInvalidateHistory)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	const auto primitive = MakePrimitive(MakeHandle(5u));
	history.RecordOccluded(SceneOcclusionSystem::BuildHistoryKey(input, primitive), input.frameSerial - 1u);

	auto jitterChanged = input;
	++jitterChanged.jitterHash;
	auto jitter = SceneOcclusionSystem::Evaluate(jitterChanged, { primitive }, history);
	ASSERT_EQ(jitter.primitiveResults.size(), 1u);
	EXPECT_FALSE(jitter.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(jitter.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::ViewChanged);

	auto depthFormatChanged = input;
	++depthFormatChanged.depthFormatKey;
	auto depth = SceneOcclusionSystem::Evaluate(depthFormatChanged, { primitive }, history);
	ASSERT_EQ(depth.primitiveResults.size(), 1u);
	EXPECT_FALSE(depth.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(depth.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::ViewChanged);

	auto viewportChanged = input;
	viewportChanged.viewportWidth += 16u;
	auto viewport = SceneOcclusionSystem::Evaluate(viewportChanged, { primitive }, history);
	ASSERT_EQ(viewport.primitiveResults.size(), 1u);
	EXPECT_FALSE(viewport.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(viewport.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::ViewChanged);
}

TEST(SceneOcclusionTests, UnsupportedBackendAndIneligibleDepthFallbackConservatively)
{
	SceneOcclusionHistory history;
	auto input = MakeFrameInput();
	auto primitive = MakePrimitive(MakeHandle(3u));
	history.RecordOccluded(SceneOcclusionSystem::BuildHistoryKey(input, primitive), input.frameSerial - 1u);

	input.backendSupported = false;
	auto unsupported = SceneOcclusionSystem::Evaluate(input, { primitive }, history);
	ASSERT_EQ(unsupported.primitiveResults.size(), 1u);
	EXPECT_FALSE(unsupported.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(unsupported.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::BackendUnsupported);

	input = MakeFrameInput();
	primitive.depthWriteEligible = false;
	auto ineligible = SceneOcclusionSystem::Evaluate(input, { primitive }, history);
	ASSERT_EQ(ineligible.primitiveResults.size(), 1u);
	EXPECT_FALSE(ineligible.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(ineligible.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::DepthWriteIneligible);
}

TEST(SceneOcclusionTests, InvalidHistoryTextureFallsBackConservatively)
{
	SceneOcclusionHistory history;
	auto input = MakeFrameInput();
	const auto primitive = MakePrimitive(MakeHandle(7u));
	history.RecordOccluded(SceneOcclusionSystem::BuildHistoryKey(input, primitive), input.frameSerial - 1u);
	input.historyTextureValid = false;

	const auto result = SceneOcclusionSystem::Evaluate(input, { primitive }, history);

	ASSERT_EQ(result.primitiveResults.size(), 1u);
	EXPECT_FALSE(result.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(result.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::HistoryTextureInvalid);
	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), primitive.handle);
}

TEST(SceneOcclusionTests, DisabledOcclusionFeatureFallsBackConservatively)
{
	SceneOcclusionHistory history;
	auto input = MakeFrameInput();
	const auto primitive = MakePrimitive(MakeHandle(6u));
	history.RecordOccluded(SceneOcclusionSystem::BuildHistoryKey(input, primitive), input.frameSerial - 1u);
	input.enabled = false;

	const auto result = SceneOcclusionSystem::Evaluate(input, { primitive }, history);

	ASSERT_EQ(result.primitiveResults.size(), 1u);
	EXPECT_FALSE(result.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(result.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::Disabled);
	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), primitive.handle);
	EXPECT_TRUE(result.occludedPrimitiveHandles.empty());
}

TEST(SceneOcclusionTests, ExpiredHistoryFallsBackConservatively)
{
	SceneOcclusionHistory history;
	auto input = MakeFrameInput();
	input.frameSerial = 20u;
	input.maxHistoryAge = 2u;
	const auto primitive = MakePrimitive(MakeHandle(8u));
	history.RecordOccluded(SceneOcclusionSystem::BuildHistoryKey(input, primitive), input.frameSerial - 3u);

	const auto result = SceneOcclusionSystem::Evaluate(input, { primitive }, history);

	ASSERT_EQ(result.primitiveResults.size(), 1u);
	EXPECT_FALSE(result.primitiveResults.front().culledByOcclusion);
	EXPECT_EQ(result.primitiveResults.front().fallbackReason, SceneOcclusionFallbackReason::HistoryTooOld);
	ASSERT_EQ(result.visiblePrimitiveHandles.size(), 1u);
	EXPECT_EQ(result.visiblePrimitiveHandles.front(), primitive.handle);
}

TEST(SceneOcclusionTests, CapabilityFallbacksPreserveFeatureDiagnostics)
{
	const SceneOcclusionCapabilityRequest request;

	auto missingHZB = MakeOcclusionCapabilities();
	missingHZB.SetFeature(RHIDeviceFeature::HierarchicalZBuffer, false, "HZB diagnostic");
	const auto hzb = SceneOcclusionSystem::ResolveCapabilities(missingHZB, request);
	EXPECT_FALSE(hzb.backendSupported);
	EXPECT_EQ(hzb.fallbackReason, SceneOcclusionFallbackReason::HZBUnsupported);
	EXPECT_EQ(hzb.diagnosticReason, "HZB diagnostic");

	auto missingAsyncReadback = MakeOcclusionCapabilities();
	missingAsyncReadback.SetFeature(RHIDeviceFeature::AsyncReadback, false, "async diagnostic");
	const auto readback = SceneOcclusionSystem::ResolveCapabilities(missingAsyncReadback, request);
	EXPECT_FALSE(readback.backendSupported);
	EXPECT_EQ(readback.fallbackReason, SceneOcclusionFallbackReason::AsyncReadbackUnsupported);
	EXPECT_EQ(readback.diagnosticReason, "async diagnostic");
}

TEST(SceneOcclusionTests, OrdinaryOcclusionEvaluationDoesNotRequestSynchronousReadback)
{
	SceneOcclusionHistory history;
	const auto input = MakeFrameInput();
	const auto primitive = MakePrimitive(MakeHandle(4u));
	history.RecordOccluded(SceneOcclusionSystem::BuildHistoryKey(input, primitive), input.frameSerial - 1u);

	const auto result = SceneOcclusionSystem::Evaluate(input, { primitive }, history);

	EXPECT_FALSE(result.usedSynchronousReadback);
	EXPECT_FALSE(result.waitedForGpuFence);
	EXPECT_FALSE(result.blockedOnReadbackMap);
	EXPECT_FALSE(result.requestedCurrentFrameReadback);
}

TEST(SceneOcclusionTests, RhiCapabilitiesGateHZBOcclusionAndAsyncReadback)
{
	SceneOcclusionCapabilityRequest request;
	const auto supported = SceneOcclusionSystem::ResolveCapabilities(MakeOcclusionCapabilities(), request);
	EXPECT_TRUE(supported.backendSupported);
	EXPECT_EQ(supported.fallbackReason, SceneOcclusionFallbackReason::None);

	auto missingHZB = MakeOcclusionCapabilities();
	missingHZB.SetFeature(RHIDeviceFeature::HierarchicalZBuffer, false, "HZB disabled for backend");
	const auto hzb = SceneOcclusionSystem::ResolveCapabilities(missingHZB, request);
	EXPECT_FALSE(hzb.backendSupported);
	EXPECT_EQ(hzb.fallbackReason, SceneOcclusionFallbackReason::HZBUnsupported);

	auto missingOcclusion = MakeOcclusionCapabilities();
	missingOcclusion.SetFeature(RHIDeviceFeature::ConservativeOcclusion, false, "occlusion disabled for backend");
	const auto occlusion = SceneOcclusionSystem::ResolveCapabilities(missingOcclusion, request);
	EXPECT_FALSE(occlusion.backendSupported);
	EXPECT_EQ(occlusion.fallbackReason, SceneOcclusionFallbackReason::OcclusionUnsupported);

	auto missingAsyncReadback = MakeOcclusionCapabilities();
	missingAsyncReadback.SetFeature(RHIDeviceFeature::AsyncReadback, false, "async readback queue unavailable");
	const auto readback = SceneOcclusionSystem::ResolveCapabilities(missingAsyncReadback, request);
	EXPECT_FALSE(readback.backendSupported);
	EXPECT_EQ(readback.fallbackReason, SceneOcclusionFallbackReason::AsyncReadbackUnsupported);
}

TEST(SceneOcclusionTests, RhiTextureFormatCapabilitiesGateOcclusionResources)
{
	SceneOcclusionCapabilityRequest request;

	auto missingDepthSampling = MakeOcclusionCapabilities();
	TextureFormatCapability depth;
	depth.sampled = false;
	depth.diagnosticReason = "depth sampling unavailable";
	missingDepthSampling.SetTextureFormatCapability(TextureFormat::Depth32F, depth);
	const auto depthResult = SceneOcclusionSystem::ResolveCapabilities(missingDepthSampling, request);
	EXPECT_FALSE(depthResult.backendSupported);
	EXPECT_EQ(depthResult.fallbackReason, SceneOcclusionFallbackReason::OpaqueDepthTextureUnsupported);

	auto missingHZBStorage = MakeOcclusionCapabilities();
	TextureFormatCapability hzb;
	hzb.sampled = true;
	hzb.storage = false;
	hzb.diagnosticReason = "R32F storage unavailable";
	missingHZBStorage.SetTextureFormatCapability(TextureFormat::R32F, hzb);
	const auto hzbResult = SceneOcclusionSystem::ResolveCapabilities(missingHZBStorage, request);
	EXPECT_FALSE(hzbResult.backendSupported);
	EXPECT_EQ(hzbResult.fallbackReason, SceneOcclusionFallbackReason::HZBTextureUnsupported);

	auto missingUnrelatedStorage = MakeOcclusionCapabilities();
	TextureFormatCapability unrelated;
	unrelated.storage = false;
	unrelated.diagnosticReason = "unused output UAV unavailable";
	missingUnrelatedStorage.SetTextureFormatCapability(TextureFormat::RG32F, unrelated);
	const auto unrelatedResult = SceneOcclusionSystem::ResolveCapabilities(missingUnrelatedStorage, request);
	EXPECT_TRUE(unrelatedResult.backendSupported);
	EXPECT_EQ(unrelatedResult.fallbackReason, SceneOcclusionFallbackReason::None);
}

TEST(SceneOcclusionTests, CapabilityRequestUsesRequestedOpaqueDepthFormat)
{
	SceneOcclusionCapabilityRequest request;
	request.opaqueDepthFormat = TextureFormat::Depth24Stencil8;

	auto capabilities = MakeOcclusionCapabilities();
	TextureFormatCapability depth32;
	depth32.sampled = true;
	capabilities.SetTextureFormatCapability(TextureFormat::Depth32F, depth32);
	TextureFormatCapability depth24;
	depth24.sampled = false;
	depth24.diagnosticReason = "deferred depth format is not sampleable";
	capabilities.SetTextureFormatCapability(TextureFormat::Depth24Stencil8, depth24);

	const auto support = SceneOcclusionSystem::ResolveCapabilities(capabilities, request);

	EXPECT_FALSE(support.backendSupported);
	EXPECT_EQ(support.fallbackReason, SceneOcclusionFallbackReason::OpaqueDepthTextureUnsupported);
	EXPECT_EQ(support.diagnosticReason, "deferred depth format is not sampleable");
}

TEST(SceneOcclusionTests, AsyncReadbackRequirementCanBeDisabledForGpuOnlyHistory)
{
	SceneOcclusionCapabilityRequest request;
	request.requireAsyncReadback = false;

	auto capabilities = MakeOcclusionCapabilities();
	capabilities.SetFeature(RHIDeviceFeature::AsyncReadback, false, "async readback unavailable");

	const auto support = SceneOcclusionSystem::ResolveCapabilities(capabilities, request);

	EXPECT_TRUE(support.backendSupported);
	EXPECT_EQ(support.fallbackReason, SceneOcclusionFallbackReason::None);
}

TEST(SceneOcclusionTests, RhiDeviceCapabilityResolverConsumesGetCapabilitiesOnly)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/SceneOcclusion.cpp");

	const auto resolver = source.find("SceneOcclusionSystem::ResolveCapabilities(");
	ASSERT_NE(resolver, std::string::npos);
	const auto deviceResolver = source.find("const NLS::Render::RHI::RHIDevice& device", resolver);
	ASSERT_NE(deviceResolver, std::string::npos);
	const auto deviceResolverEnd = source.find("SceneOcclusionResult SceneOcclusionSystem::Evaluate", deviceResolver);
	ASSERT_NE(deviceResolverEnd, std::string::npos);
	const auto deviceResolverBody = source.substr(deviceResolver, deviceResolverEnd - deviceResolver);

	EXPECT_NE(deviceResolverBody.find("device.GetCapabilities()"), std::string::npos);
	EXPECT_EQ(deviceResolverBody.find("ReadPixelsChecked"), std::string::npos);
	EXPECT_EQ(deviceResolverBody.find("BeginReadPixels"), std::string::npos);
}

TEST(SceneOcclusionTests, OrdinaryOcclusionSourcePathDoesNotContainBlockingReadbackCalls)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/SceneOcclusion.cpp");

	const auto evaluate = source.find("SceneOcclusionResult SceneOcclusionSystem::Evaluate");
	ASSERT_NE(evaluate, std::string::npos);
	const auto evaluateBody = source.substr(evaluate);

	EXPECT_EQ(evaluateBody.find("ReadPixelsChecked"), std::string::npos);
	EXPECT_EQ(evaluateBody.find("BeginReadPixels"), std::string::npos);
	EXPECT_EQ(evaluateBody.find(".Wait("), std::string::npos);
	EXPECT_EQ(evaluateBody.find("->Wait("), std::string::npos);
	EXPECT_EQ(evaluateBody.find(".Map("), std::string::npos);
	EXPECT_EQ(evaluateBody.find("->Map("), std::string::npos);
}

TEST(SceneOcclusionTests, OrdinaryOcclusionMissClassificationDoesNotScanFullHistory)
{
	const auto source = ReadTextFile(
		std::filesystem::path(NLS_ROOT_DIR) /
		"Runtime/Engine/Rendering/SceneOcclusion.cpp");

	const auto evaluate = source.find("SceneOcclusionResult SceneOcclusionSystem::Evaluate");
	ASSERT_NE(evaluate, std::string::npos);
	const auto evaluateBody = source.substr(evaluate);

	EXPECT_EQ(evaluateBody.find("FindKeysForHandle"), std::string::npos);
	EXPECT_NE(evaluateBody.find("ClassifyMissingHistoryForHandle"), std::string::npos);

	const auto classifier = source.find("SceneOcclusionHistory::ClassifyMissingHistoryForHandle");
	ASSERT_NE(classifier, std::string::npos);
	const auto classifierEnd = source.find("SceneOcclusionHistoryKey SceneOcclusionSystem::BuildHistoryKey", classifier);
	ASSERT_NE(classifierEnd, std::string::npos);
	const auto classifierBody = source.substr(classifier, classifierEnd - classifier);

	EXPECT_NE(classifierBody.find("m_keysByHandle.find"), std::string::npos);
	EXPECT_EQ(classifierBody.find("m_occludedFrameByKey"), std::string::npos);
}
