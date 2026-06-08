#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "Rendering/LargeSceneSettings.h"
#include "Rendering/SceneStreamingResidency.h"

namespace
{
	using NLS::Engine::Rendering::LargeSceneSettings;
	using NLS::Engine::Rendering::RepresentationResidencySnapshot;
	using NLS::Engine::Rendering::ResidencyTicketState;
	using NLS::Engine::Rendering::ScenePrimitiveHandle;
	using NLS::Engine::Rendering::SceneStreamingResidency;
	using NLS::Engine::Rendering::StreamingDependencySource;
	using NLS::Engine::Rendering::StreamingResidencyFramePins;
	using NLS::Engine::Rendering::StreamingResidencyPlanInput;
	using NLS::Engine::Rendering::StreamingResourceDependency;
	using NLS::Engine::Rendering::StreamingResourceType;

	ScenePrimitiveHandle MakeHandle(const uint32_t index)
	{
		return { 0x57u, index, 1u };
	}

	StreamingResourceDependency MakeDependency(
		const uint64_t id,
		const char* artifact,
		const uint64_t cpuBytes,
		const uint64_t gpuBytes)
	{
		StreamingResourceDependency dependency;
		dependency.dependencyId = id;
		dependency.source = StreamingDependencySource::Visibility;
		dependency.resourceType = StreamingResourceType::Mesh;
		dependency.artifactId = artifact;
		dependency.cpuBytes = cpuBytes;
		dependency.gpuBytes = gpuBytes;
		dependency.ioBytes = cpuBytes;
		dependency.gpuUploadBytes = gpuBytes;
		dependency.cpuCommitUs = 10u;
		dependency.requiredForVisibleRepresentation = true;
		return dependency;
	}

	LargeSceneSettings GenerousSettings()
	{
		auto settings = LargeSceneSettings::Defaults();
		settings.streamingCpuBudgetUs = 1000u;
		settings.streamingIoBudgetBytes = 1024ull * 1024ull;
		settings.streamingGpuUploadBudgetBytes = 1024ull * 1024ull;
		settings.streamingCpuMemoryBudgetBytes = 1024ull * 1024ull;
		settings.streamingGpuMemoryBudgetBytes = 1024ull * 1024ull;
		return settings;
	}

	std::string ReadRepoFile(const std::filesystem::path& relativePath)
	{
		const auto path = std::filesystem::path(NLS_ROOT_DIR) / relativePath;
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			return {};
		std::ostringstream buffer;
		buffer << stream.rdbuf();
		return buffer.str();
	}
}

TEST(SceneStreamingResidencyTests, PrimitiveDependencyRegistrationUsesHashSetForDuplicateBindings)
{
	const auto header = ReadRepoFile("Runtime/Engine/Rendering/SceneStreamingResidency.h");
	const auto source = ReadRepoFile("Runtime/Engine/Rendering/SceneStreamingResidency.cpp");

	ASSERT_FALSE(header.empty());
	ASSERT_FALSE(source.empty());
	EXPECT_NE(header.find("PrimitiveDependencyBindingHash"), std::string::npos);
	EXPECT_NE(header.find("primitiveDependencyBindings"), std::string::npos);

	const auto registerStart = source.find("void SceneStreamingResidency::RegisterPrimitiveDependency");
	const auto planStart = source.find("StreamingResidencyPlan SceneStreamingResidency::Plan", registerStart);
	ASSERT_NE(registerStart, std::string::npos);
	ASSERT_NE(planStart, std::string::npos);
	const auto registerBody = source.substr(registerStart, planStart - registerStart);

	EXPECT_NE(registerBody.find("primitiveDependencyBindings.insert"), std::string::npos);
	EXPECT_EQ(registerBody.find("std::find_if("), std::string::npos);
}

TEST(SceneStreamingResidencyTests, DependencyClosureDeduplicatesVisibleAndRepresentationInterest)
{
	SceneStreamingResidency residency;
	const auto primitive = MakeHandle(1u);
	const auto proxy = MakeHandle(2u);
	auto mesh = MakeDependency(10u, "mesh:A", 64u, 128u);
	auto material = MakeDependency(11u, "material:A", 16u, 32u);
	material.resourceType = StreamingResourceType::Material;
	mesh.childDependencyIds = { material.dependencyId };

	residency.RegisterDependency(mesh);
	residency.RegisterDependency(material);
	residency.RegisterPrimitiveDependency(primitive, mesh.dependencyId);
	residency.RegisterPrimitiveDependency(proxy, mesh.dependencyId);

	StreamingResidencyPlanInput input;
	input.frameSerial = 7u;
	input.visiblePrimitiveHandles = { primitive, primitive };
	input.representationStreamingInterest = { proxy };

	const auto plan = residency.Plan(input, GenerousSettings());

	ASSERT_EQ(plan.dependencyClosure.size(), 2u);
	EXPECT_EQ(plan.dependencyClosure[0].dependencyId, mesh.dependencyId);
	EXPECT_EQ(plan.dependencyClosure[1].dependencyId, material.dependencyId);
	ASSERT_EQ(plan.tickets.size(), 2u);
	EXPECT_EQ(plan.tickets[0].coalescedRequestCount, 3u);
	ASSERT_EQ(plan.primitiveDependencyBindings.size(), 2u);
	EXPECT_EQ(plan.primitiveDependencyBindings[0].primitive, primitive);
	EXPECT_EQ(plan.primitiveDependencyBindings[0].dependencyId, mesh.dependencyId);
	EXPECT_EQ(plan.primitiveDependencyBindings[1].primitive, proxy);
	EXPECT_EQ(plan.primitiveDependencyBindings[1].dependencyId, mesh.dependencyId);
	EXPECT_EQ(plan.telemetry.streamingDependencyCount, 2u);
	EXPECT_EQ(plan.telemetry.streamingRequestCount, 2u);
	EXPECT_EQ(plan.telemetry.requestedCpuBytes, 80u);
	EXPECT_EQ(plan.telemetry.requestedGpuBytes, 160u);
}

TEST(SceneStreamingResidencyTests, RepresentationResidencySnapshotStateUpdatesAreMutuallyExclusive)
{
	RepresentationResidencySnapshot snapshot;
	const auto primitive = MakeHandle(1u);

	snapshot.MarkReady(primitive);
	EXPECT_TRUE(snapshot.IsReady(primitive));
	EXPECT_FALSE(snapshot.IsNotResident(primitive));

	snapshot.MarkNotResident(primitive);
	EXPECT_FALSE(snapshot.IsReady(primitive));
	EXPECT_FALSE(snapshot.IsHLODProxyReady(primitive));
	EXPECT_FALSE(snapshot.HasFallback(primitive));
	EXPECT_TRUE(snapshot.IsNotResident(primitive));

	snapshot.MarkHLODProxyReady(primitive);
	EXPECT_TRUE(snapshot.IsReady(primitive));
	EXPECT_TRUE(snapshot.IsHLODProxyReady(primitive));
	EXPECT_FALSE(snapshot.IsNotResident(primitive));

	snapshot.MarkNotResident(primitive);
	snapshot.MarkFallback(primitive);
	EXPECT_TRUE(snapshot.HasFallback(primitive));
	EXPECT_FALSE(snapshot.IsReady(primitive));
	EXPECT_FALSE(snapshot.IsHLODProxyReady(primitive));
	EXPECT_FALSE(snapshot.IsNotResident(primitive));

	snapshot.MarkReady(primitive);
	EXPECT_TRUE(snapshot.IsReady(primitive));
	EXPECT_FALSE(snapshot.IsHLODProxyReady(primitive));
	EXPECT_FALSE(snapshot.HasFallback(primitive));
	EXPECT_FALSE(snapshot.IsNotResident(primitive));

	snapshot.MarkHLODProxyReady(primitive);
	EXPECT_TRUE(snapshot.IsReady(primitive));
	EXPECT_TRUE(snapshot.IsHLODProxyReady(primitive));
	EXPECT_FALSE(snapshot.HasFallback(primitive));
	EXPECT_FALSE(snapshot.IsNotResident(primitive));

	snapshot.MarkFallback(primitive);
	EXPECT_FALSE(snapshot.IsReady(primitive));
	EXPECT_FALSE(snapshot.IsHLODProxyReady(primitive));
	EXPECT_TRUE(snapshot.HasFallback(primitive));
	EXPECT_FALSE(snapshot.IsNotResident(primitive));
}

TEST(SceneStreamingResidencyTests, TicketCoalescingKeepsFirstRequestAndAppliesPriorityAging)
{
	SceneStreamingResidency residency;
	const auto primitive = MakeHandle(1u);
	auto mesh = MakeDependency(20u, "mesh:B", 64u, 128u);
	mesh.priority = 4u;

	residency.RegisterDependency(mesh);
	residency.RegisterPrimitiveDependency(primitive, mesh.dependencyId);

	StreamingResidencyPlanInput input;
	input.frameSerial = 10u;
	input.visiblePrimitiveHandles = { primitive };
	auto plan = residency.Plan(input, GenerousSettings());
	ASSERT_EQ(plan.tickets.size(), 1u);
	EXPECT_EQ(plan.tickets.front().requestFrame, 10u);
	EXPECT_EQ(plan.tickets.front().priority, 4u);

	input.frameSerial = 15u;
	plan = residency.Plan(input, GenerousSettings());

	ASSERT_EQ(plan.tickets.size(), 1u);
	EXPECT_EQ(plan.tickets.front().requestFrame, 10u);
	EXPECT_EQ(plan.tickets.front().lastInterestFrame, 15u);
	EXPECT_GT(plan.tickets.front().priority, 4u);
}

TEST(SceneStreamingResidencyTests, TicketCoalescedRequestCountReflectsCurrentPlanNotLifetime)
{
	SceneStreamingResidency residency;
	const auto primitive = MakeHandle(1u);
	const auto proxy = MakeHandle(2u);
	auto mesh = MakeDependency(21u, "mesh:B2", 64u, 128u);

	residency.RegisterDependency(mesh);
	residency.RegisterPrimitiveDependency(primitive, mesh.dependencyId);
	residency.RegisterPrimitiveDependency(proxy, mesh.dependencyId);

	StreamingResidencyPlanInput input;
	input.frameSerial = 10u;
	input.visiblePrimitiveHandles = { primitive, primitive };
	input.representationStreamingInterest = { proxy };
	auto plan = residency.Plan(input, GenerousSettings());
	ASSERT_EQ(plan.tickets.size(), 1u);
	EXPECT_EQ(plan.tickets.front().coalescedRequestCount, 3u);

	input.frameSerial = 11u;
	input.visiblePrimitiveHandles = { primitive };
	input.representationStreamingInterest.clear();
	plan = residency.Plan(input, GenerousSettings());

	ASSERT_EQ(plan.tickets.size(), 1u);
	EXPECT_EQ(plan.tickets.front().requestFrame, 10u);
	EXPECT_EQ(plan.tickets.front().lastInterestFrame, 11u);
	EXPECT_EQ(plan.tickets.front().coalescedRequestCount, 1u);
}

TEST(SceneStreamingResidencyTests, CommitProgressesTicketsThroughBudgetedResidencyStates)
{
	SceneStreamingResidency residency;
	const auto primitive = MakeHandle(1u);
	const auto mesh = MakeDependency(30u, "mesh:C", 64u, 128u);

	residency.RegisterDependency(mesh);
	residency.RegisterPrimitiveDependency(primitive, mesh.dependencyId);

	StreamingResidencyPlanInput input;
	input.frameSerial = 1u;
	input.visiblePrimitiveHandles = { primitive };
	const auto plan = residency.Plan(input, GenerousSettings());

	auto commit = residency.Commit(plan, GenerousSettings(), {});
	ASSERT_EQ(commit.tickets.size(), 1u);
	EXPECT_EQ(commit.tickets.front().state, ResidencyTicketState::LoadingCpu);

	commit = residency.Commit(plan, GenerousSettings(), {});
	ASSERT_EQ(commit.tickets.size(), 1u);
	EXPECT_EQ(commit.tickets.front().state, ResidencyTicketState::PendingGpuUpload);

	commit = residency.Commit(plan, GenerousSettings(), {});
	ASSERT_EQ(commit.tickets.size(), 1u);
	EXPECT_EQ(commit.tickets.front().state, ResidencyTicketState::VisibleResident);
	EXPECT_EQ(commit.telemetry.streamingCommitCount, 1u);
	EXPECT_EQ(commit.telemetry.residentCpuBytes, mesh.cpuBytes);
	EXPECT_EQ(commit.telemetry.residentGpuBytes, mesh.gpuBytes);
}

TEST(SceneStreamingResidencyTests, BudgetExhaustionLeavesTicketsRequestedAndReportsRequestedBytes)
{
	SceneStreamingResidency residency;
	const auto primitive = MakeHandle(1u);
	const auto mesh = MakeDependency(40u, "mesh:D", 4096u, 8192u);
	auto settings = GenerousSettings();
	settings.streamingCpuBudgetUs = 0u;

	residency.RegisterDependency(mesh);
	residency.RegisterPrimitiveDependency(primitive, mesh.dependencyId);

	StreamingResidencyPlanInput input;
	input.frameSerial = 1u;
	input.visiblePrimitiveHandles = { primitive };
	const auto plan = residency.Plan(input, settings);
	const auto commit = residency.Commit(plan, settings, {});

	ASSERT_EQ(commit.tickets.size(), 1u);
	EXPECT_EQ(commit.tickets.front().state, ResidencyTicketState::Requested);
	EXPECT_TRUE(commit.cpuBudgetExhausted);
	EXPECT_EQ(commit.telemetry.requestedCpuBytes, mesh.cpuBytes);
	EXPECT_EQ(commit.telemetry.requestedGpuBytes, mesh.gpuBytes);
}

TEST(SceneStreamingResidencyTests, FallbackAwareEvictionSkipsPinnedVisibleResources)
{
	SceneStreamingResidency residency;
	const auto primaryPrimitive = MakeHandle(1u);
	const auto fallbackPrimitive = MakeHandle(2u);
	auto primary = MakeDependency(50u, "mesh:high", 512u, 512u);
	auto fallback = MakeDependency(51u, "mesh:low", 64u, 64u);
	primary.fallbackDependencyId = fallback.dependencyId;

	residency.RegisterDependency(primary);
	residency.RegisterDependency(fallback);
	residency.RegisterPrimitiveDependency(primaryPrimitive, primary.dependencyId);
	residency.RegisterPrimitiveDependency(fallbackPrimitive, fallback.dependencyId);

	auto settings = GenerousSettings();
	StreamingResidencyPlanInput input;
	input.frameSerial = 1u;
	input.visiblePrimitiveHandles = { primaryPrimitive, fallbackPrimitive };
	auto plan = residency.Plan(input, settings);
	[[maybe_unused]] NLS::Engine::Rendering::StreamingCommitResult warmupCommit;
	for (int i = 0; i < 3; ++i)
		warmupCommit = residency.Commit(plan, settings, {});

	settings.streamingCpuMemoryBudgetBytes = fallback.cpuBytes;
	settings.streamingGpuMemoryBudgetBytes = fallback.gpuBytes;
	StreamingResidencyFramePins pins;
	pins.pinnedDependencyIds = { primary.dependencyId };
	auto commit = residency.Commit(plan, settings, pins);
	EXPECT_EQ(commit.telemetry.streamingEvictCount, 0u);
	EXPECT_EQ(commit.FindTicket(primary.dependencyId)->state, ResidencyTicketState::VisibleResident);

	pins.pinnedDependencyIds.clear();
	commit = residency.Commit(plan, settings, pins);
	EXPECT_EQ(commit.telemetry.streamingEvictCount, 1u);
	EXPECT_EQ(commit.FindTicket(primary.dependencyId)->state, ResidencyTicketState::Evicted);
	EXPECT_EQ(commit.FindTicket(fallback.dependencyId)->state, ResidencyTicketState::VisibleResident);
}

TEST(SceneStreamingResidencyTests, MemoryBudgetEvictsOffscreenResidentResources)
{
	SceneStreamingResidency residency;
	const auto firstPrimitive = MakeHandle(1u);
	const auto firstFallbackPrimitive = MakeHandle(2u);
	const auto secondPrimitive = MakeHandle(3u);
	auto firstHigh = MakeDependency(60u, "mesh:first-high", 512u, 512u);
	auto firstFallback = MakeDependency(61u, "mesh:first-low", 64u, 64u);
	auto second = MakeDependency(62u, "mesh:second", 64u, 64u);
	firstHigh.fallbackDependencyId = firstFallback.dependencyId;

	residency.RegisterDependency(firstHigh);
	residency.RegisterDependency(firstFallback);
	residency.RegisterDependency(second);
	residency.RegisterPrimitiveDependency(firstPrimitive, firstHigh.dependencyId);
	residency.RegisterPrimitiveDependency(firstFallbackPrimitive, firstFallback.dependencyId);
	residency.RegisterPrimitiveDependency(secondPrimitive, second.dependencyId);

	auto settings = GenerousSettings();
	StreamingResidencyPlanInput firstInput;
	firstInput.frameSerial = 1u;
	firstInput.visiblePrimitiveHandles = { firstPrimitive, firstFallbackPrimitive };
	auto firstPlan = residency.Plan(firstInput, settings);
	for (int i = 0; i < 3; ++i)
		(void)residency.Commit(firstPlan, settings, {});

	StreamingResidencyPlanInput secondInput;
	secondInput.frameSerial = 2u;
	secondInput.visiblePrimitiveHandles = { secondPrimitive };
	auto secondPlan = residency.Plan(secondInput, settings);
	for (int i = 0; i < 3; ++i)
		(void)residency.Commit(secondPlan, settings, {});

	settings.streamingCpuMemoryBudgetBytes = firstFallback.cpuBytes + second.cpuBytes;
	settings.streamingGpuMemoryBudgetBytes = firstFallback.gpuBytes + second.gpuBytes;
	auto commit = residency.Commit(secondPlan, settings, {});

	EXPECT_EQ(commit.telemetry.streamingEvictCount, 1u);
	EXPECT_EQ(commit.FindTicket(firstHigh.dependencyId)->state, ResidencyTicketState::Evicted);
	EXPECT_EQ(commit.FindTicket(firstFallback.dependencyId)->state, ResidencyTicketState::Resident);
	EXPECT_EQ(commit.FindTicket(second.dependencyId)->state, ResidencyTicketState::VisibleResident);
	EXPECT_EQ(commit.telemetry.residentCpuBytes, firstFallback.cpuBytes + second.cpuBytes);
	EXPECT_EQ(commit.telemetry.residentGpuBytes, firstFallback.gpuBytes + second.gpuBytes);
	EXPECT_FALSE(commit.cpuMemoryBudgetExhausted);
	EXPECT_FALSE(commit.gpuMemoryBudgetExhausted);
}

TEST(SceneStreamingResidencyTests, CommitReturnsCurrentAndEvictedTicketsWithoutCopyingHistoricalTable)
{
	SceneStreamingResidency residency;
	auto settings = GenerousSettings();

	constexpr uint32_t kHistoricalCount = 32u;
	for (uint32_t index = 0u; index < kHistoricalCount; ++index)
	{
		const auto primitive = MakeHandle(index + 1u);
		const auto dependency = MakeDependency(
			1000u + index,
			"mesh:historical",
			16u,
			32u);
		residency.RegisterDependency(dependency);
		residency.RegisterPrimitiveDependency(primitive, dependency.dependencyId);

		StreamingResidencyPlanInput input;
		input.frameSerial = index + 1u;
		input.visiblePrimitiveHandles = { primitive };
		const auto plan = residency.Plan(input, settings);
		for (int step = 0; step < 3; ++step)
			(void)residency.Commit(plan, settings, {});
	}

	const auto currentPrimitive = MakeHandle(kHistoricalCount + 1u);
	const auto currentDependency = MakeDependency(2000u, "mesh:current", 24u, 48u);
	residency.RegisterDependency(currentDependency);
	residency.RegisterPrimitiveDependency(currentPrimitive, currentDependency.dependencyId);

	StreamingResidencyPlanInput currentInput;
	currentInput.frameSerial = 100u;
	currentInput.visiblePrimitiveHandles = { currentPrimitive };
	const auto currentPlan = residency.Plan(currentInput, settings);
	const auto commit = residency.Commit(currentPlan, settings, {});

	EXPECT_EQ(commit.telemetry.residencyTicketCount, kHistoricalCount + 1u);
	EXPECT_EQ(commit.tickets.size(), currentPlan.tickets.size());
	ASSERT_NE(commit.FindTicket(currentDependency.dependencyId), nullptr);
	EXPECT_EQ(commit.FindTicket(currentDependency.dependencyId)->state, ResidencyTicketState::LoadingCpu);
	EXPECT_EQ(commit.FindTicket(1000u), nullptr);
	EXPECT_EQ(commit.telemetry.residentCpuBytes, kHistoricalCount * 16u);
	EXPECT_EQ(commit.telemetry.residentGpuBytes, kHistoricalCount * 32u);
	EXPECT_EQ(commit.telemetry.requestedCpuBytes, currentDependency.cpuBytes);
	EXPECT_EQ(commit.telemetry.requestedGpuBytes, currentDependency.gpuBytes);
}

TEST(SceneStreamingResidencyTests, MemoryEvictionSkipsResidentScanWhenWithinBudget)
{
	const auto source = ReadRepoFile("Runtime/Engine/Rendering/SceneStreamingResidency.cpp");
	ASSERT_FALSE(source.empty());

	const auto evictionStart = source.find("void SceneStreamingResidency::ApplyMemoryBudgetEviction");
	ASSERT_NE(evictionStart, std::string::npos);
	const auto currentInterestStart = source.find("std::unordered_set<uint64_t> currentInterest", evictionStart);
	ASSERT_NE(currentInterestStart, std::string::npos);
	const auto preScanBody = source.substr(evictionStart, currentInterestStart - evictionStart);

	EXPECT_NE(preScanBody.find("residentCpuBytes <= settings.streamingCpuMemoryBudgetBytes"), std::string::npos);
	EXPECT_NE(preScanBody.find("residentGpuBytes <= settings.streamingGpuMemoryBudgetBytes"), std::string::npos);
	EXPECT_NE(preScanBody.find("return;"), std::string::npos);
}
