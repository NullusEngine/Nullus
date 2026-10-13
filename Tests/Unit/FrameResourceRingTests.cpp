#include <gtest/gtest.h>

#include "Rendering/Context/FrameResourceRing.h"
#include "Rendering/Settings/RhiThreadMode.h"
#include "Threading/ExecutionThreadBudget.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    class RingTestFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        std::atomic_bool signaled { false };

        std::string_view GetDebugName() const override { return "RingTestFence"; }
        bool IsSignaled() const override { return signaled.load(); }
        void Reset() override { signaled.store(false); }
        bool Wait(uint64_t) override { return signaled.load(); }
    };

    class RingTestResource final : public NLS::Render::RHI::RHIObject
    {
    public:
        std::string_view GetDebugName() const override { return "RingTestResource"; }
    };
}

TEST(FrameResourceRingTests, RejectsAcquireWhileEverySlotHasGpuWorkInFlight)
{
    NLS::Render::Context::FrameResourceRing ring(2u);
    ASSERT_EQ(ring.GetSlotCount(), 2u);

    auto firstFence = std::make_shared<RingTestFence>();
    auto secondFence = std::make_shared<RingTestFence>();

    auto* first = ring.AcquireSlot();
    ASSERT_NE(first, nullptr);
    ring.RetireSlot(first->slotIndex, firstFence);

    auto* second = ring.AcquireSlot();
    ASSERT_NE(second, nullptr);
    ring.RetireSlot(second->slotIndex, secondFence);

    EXPECT_EQ(ring.GetInFlightSlotCount(), 2u);
    EXPECT_EQ(ring.AcquireSlot(), nullptr)
        << "A slot must never be reused while its GPU fence is unsignaled.";
    EXPECT_EQ(ring.GetStarvedCount(), 1u);
}

TEST(FrameResourceRingTests, ReusesSlotOnlyAfterItsGpuFenceSignals)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto fence = std::make_shared<RingTestFence>();

    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    const uint64_t firstLease = slot->leaseGeneration;
    ring.RetireSlot(slot->slotIndex, fence);

    EXPECT_EQ(ring.AcquireSlot(), nullptr);

    fence->signaled.store(true);
    auto* reused = ring.AcquireSlot();
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->slotIndex, slot->slotIndex);
    EXPECT_GT(reused->leaseGeneration, firstLease);
    EXPECT_EQ(ring.GetStarvedCount(), 1u);
}

TEST(FrameResourceRingTests, RetireWithoutFenceMakesTheSlotImmediatelyReusable)
{
    NLS::Render::Context::FrameResourceRing ring(1u);

    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    auto resource = std::make_shared<RingTestResource>();
    std::weak_ptr<RingTestResource> retained = resource;
    slot->ownedResources.push_back(std::move(resource));
    ring.RetireSlot(slot->slotIndex, nullptr);

    EXPECT_TRUE(retained.expired());
    EXPECT_EQ(ring.GetInFlightSlotCount(), 0u);
    EXPECT_NE(ring.AcquireSlot(), nullptr);
}

TEST(FrameResourceRingTests, ReservedTailSlotRemainsAvailableToThePresentationConsumer)
{
    NLS::Render::Context::FrameResourceRing ring(3u);
    for (size_t index = 0u; index < 2u; ++index)
    {
        const auto sceneSlot = ring.FindAvailableSlotIndex(1u);
        ASSERT_TRUE(sceneSlot.has_value());
        EXPECT_LT(*sceneSlot, 2u);
        ASSERT_NE(ring.AcquireSlot(*sceneSlot), nullptr);
    }
    EXPECT_FALSE(ring.FindAvailableSlotIndex(1u).has_value());
    EXPECT_EQ(ring.FindAvailableSlotIndex(), std::optional<size_t>(2u));
    auto fence = std::make_shared<RingTestFence>();
    ASSERT_NE(ring.AcquireSlot(2u), nullptr);
    ring.RetireSlot(2u, fence);
    EXPECT_FALSE(ring.FindAvailableSlotIndex().has_value());
    fence->signaled = true;
    EXPECT_EQ(ring.FindAvailableSlotIndex(), std::optional<size_t>(2u));
    EXPECT_FALSE(ring.FindAvailableSlotIndex(1u).has_value());
    EXPECT_FALSE(ring.FindAvailableSlotIndex(4u).has_value());
}

TEST(FrameResourceRingTests, ReclaimInvokesTheReleaseCallbackExactlyOncePerSlot)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto fence = std::make_shared<RingTestFence>();

    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    slot->ownedResources.push_back(std::make_shared<RingTestResource>());
    ring.RetireSlot(slot->slotIndex, fence);

    std::atomic<uint32_t> releases { 0u };
    const auto release = [&releases](NLS::Render::Context::FrameResourceSlot& released)
    {
        releases.fetch_add(1u, std::memory_order_acq_rel);
        released.ownedResources.clear();
    };

    EXPECT_EQ(ring.ReclaimCompletedSlots(release), 0u);
    EXPECT_EQ(releases.load(), 0u);

    fence->signaled.store(true);
    EXPECT_EQ(ring.ReclaimCompletedSlots(release), 1u);
    EXPECT_EQ(releases.load(), 1u);

    // A second reclaim pass has nothing left to release.
    EXPECT_EQ(ring.ReclaimCompletedSlots(release), 0u);
    EXPECT_EQ(releases.load(), 1u);
}

TEST(FrameResourceRingTests, StreamingDependenciesStayPinnedUntilGpuRetirement)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    slot->streamingDependencyPins = { 17u, 29u };
    auto fence = std::make_shared<RingTestFence>();
    ring.RetireSlot(slot->slotIndex, fence);

    EXPECT_EQ(ring.ReclaimCompletedSlots(), 0u);
    EXPECT_EQ(slot->streamingDependencyPins, (std::vector<uint64_t> { 17u, 29u }));
    EXPECT_EQ(ring.AcquireSlot(), nullptr);

    fence->signaled.store(true);
    EXPECT_EQ(ring.ReclaimCompletedSlots(), 1u);
    EXPECT_TRUE(slot->streamingDependencyPins.empty());
    EXPECT_NE(ring.AcquireSlot(), nullptr);
}

TEST(FrameResourceRingTests, DrainAllWaitsForEveryFenceAndReleasesResources)
{
    NLS::Render::Context::FrameResourceRing ring(3u);
    std::vector<std::shared_ptr<RingTestFence>> fences;
    for (uint32_t index = 0u; index < 3u; ++index)
    {
        auto* slot = ring.AcquireSlot();
        ASSERT_NE(slot, nullptr);
        slot->ownedResources.push_back(std::make_shared<RingTestResource>());
        auto fence = std::make_shared<RingTestFence>();
        fences.push_back(fence);
        ring.RetireSlot(slot->slotIndex, fence);
    }
    ASSERT_EQ(ring.GetInFlightSlotCount(), 3u);

    // Signalling from another thread must let the drain complete.
    std::thread signaller([&fences]
    {
        std::this_thread::sleep_for(5ms);
        for (const auto& fence : fences)
            fence->signaled.store(true);
    });

    std::atomic<uint32_t> releases { 0u };
    EXPECT_TRUE(ring.DrainAll(
        [&releases](NLS::Render::Context::FrameResourceSlot&)
        {
            releases.fetch_add(1u, std::memory_order_acq_rel);
        },
        static_cast<uint64_t>(std::chrono::nanoseconds(5s).count())));
    signaller.join();

    EXPECT_EQ(releases.load(), 3u);
    EXPECT_EQ(ring.GetInFlightSlotCount(), 0u);
}

TEST(FrameResourceRingTests, DrainAllTimesOutWhenTheGpuNeverSignals)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    ring.RetireSlot(slot->slotIndex, std::make_shared<RingTestFence>());

    EXPECT_FALSE(ring.DrainAll({}, static_cast<uint64_t>(std::chrono::nanoseconds(20ms).count())));
    EXPECT_EQ(ring.GetInFlightSlotCount(), 1u)
        << "A timed-out drain must leave resources alive rather than freeing them early.";
}

TEST(FrameResourceRingTests, AvailabilityIsDecidedOnlyByTheGpuFence)
{
    NLS::Render::Context::FrameResourceRing ring(2u);

    // A slot with no recorded fence has nothing to wait for.
    EXPECT_TRUE(ring.IsSlotAvailable(0u));

    // An armed fence makes the slot unavailable until it signals.
    auto fence = std::make_shared<RingTestFence>();
    ring.RetireSlot(0u, fence);
    EXPECT_FALSE(ring.IsSlotAvailable(0u))
        << "An unsignaled fence must block slot reuse even when the CPU stage says done.";

    fence->signaled.store(true);
    EXPECT_TRUE(ring.IsSlotAvailable(0u));

    // The recorded fence is remembered, so a later query without a fence still
    // reflects the GPU state instead of assuming it is free.
    const auto pending = std::make_shared<RingTestFence>();
    ring.RetireSlot(1u, pending);
    EXPECT_FALSE(ring.IsSlotAvailable(1u));
    EXPECT_FALSE(ring.IsSlotAvailable(1u));

    // Out-of-range indices are never available.
    EXPECT_FALSE(ring.IsSlotAvailable(7u));
}

TEST(FrameResourceRingTests, ResetClearsRecordedFencesForRingRebuild)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto fence = std::make_shared<RingTestFence>();
    ring.RetireSlot(0u, fence);
    EXPECT_FALSE(ring.IsSlotAvailable(0u));
    EXPECT_FALSE(ring.Reset());
    EXPECT_EQ(ring.AcquireSlot(), nullptr);

    fence->signaled.store(true);
    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    slot->ownedResources.push_back(std::make_shared<RingTestResource>());
    ring.RetireSlot(slot->slotIndex, fence);

    EXPECT_TRUE(ring.Reset());

    EXPECT_EQ(ring.GetInFlightSlotCount(), 0u);
    EXPECT_EQ(ring.PeekSlot(0u)->ownedResources.size(), 0u);
    EXPECT_TRUE(ring.IsSlotAvailable(0u))
        << "After a rebuild the ring must not remember the previous device's fence.";
}

TEST(FrameResourceRingTests, DrainAlsoWaitsForExternallyRecordedGpuWork)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto fence = std::make_shared<RingTestFence>();
    ring.RetireSlot(0u, fence);
    EXPECT_FALSE(ring.IsSlotAvailable(0u));
    EXPECT_EQ(ring.GetInFlightSlotCount(), 1u);
    EXPECT_FALSE(ring.DrainAll({}, static_cast<uint64_t>(std::chrono::nanoseconds(2ms).count())));
    fence->signaled.store(true);
    EXPECT_TRUE(ring.DrainAll({}, static_cast<uint64_t>(std::chrono::nanoseconds(20ms).count())));
    EXPECT_EQ(ring.GetInFlightSlotCount(), 0u);
    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    EXPECT_FALSE(ring.IsSlotAvailable(0u));
    EXPECT_EQ(ring.AcquireSlot(), nullptr);
}

TEST(FrameResourceRingTests, AcquiredSlotCannotBeClaimedOrReclaimedBeforeRetirement)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    auto resource = std::make_shared<RingTestResource>();
    std::weak_ptr<RingTestResource> retained = resource;
    slot->ownedResources.push_back(std::move(resource));
    EXPECT_FALSE(ring.IsSlotAvailable(0u));
    EXPECT_EQ(ring.AcquireSlot(), nullptr);
    EXPECT_EQ(ring.AcquireSlot(0u), nullptr);
    EXPECT_EQ(ring.ReclaimCompletedSlots(), 0u);
    EXPECT_FALSE(ring.Reset());
    EXPECT_FALSE(retained.expired());

    ring.RetireSlot(0u, nullptr);
    EXPECT_TRUE(retained.expired());
    EXPECT_TRUE(ring.IsSlotAvailable(0u));
    EXPECT_NE(ring.AcquireSlot(), nullptr);
}

TEST(FrameResourceRingTests, AvailabilityQueriesPreserveTheResourceLeaseAndFence)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    const auto generation = slot->leaseGeneration;
    auto fence = std::make_shared<RingTestFence>();
    ring.RetireSlot(0u, fence);
    for (size_t poll = 0u; poll < 8u; ++poll)
    {
        EXPECT_FALSE(ring.IsSlotAvailable(0u));
        EXPECT_EQ(slot->leaseGeneration, generation);
        EXPECT_EQ(slot->gpuFence, fence);
    }
    fence->signaled.store(true);
    EXPECT_TRUE(ring.IsSlotAvailable(0u));
    EXPECT_EQ(slot->gpuFence, fence);
    auto* next = ring.AcquireSlot();
    ASSERT_NE(next, nullptr);
    EXPECT_GT(next->leaseGeneration, generation);
    EXPECT_EQ(next->gpuFence, nullptr);
    EXPECT_FALSE(ring.IsSlotAvailable(0u));
}

TEST(FrameResourceRingTests, ResetKeepsEveryResourceWhenAnyGpuFenceIsPending)
{
    NLS::Render::Context::FrameResourceRing ring(2u);
    std::vector<std::weak_ptr<RingTestResource>> resources;
    std::vector<std::shared_ptr<RingTestFence>> fences;
    for (size_t index = 0u; index < 2u; ++index)
    {
        auto* slot = ring.AcquireSlot();
        ASSERT_NE(slot, nullptr);
        auto resource = std::make_shared<RingTestResource>();
        resources.push_back(resource);
        slot->ownedResources.push_back(std::move(resource));
        auto fence = std::make_shared<RingTestFence>();
        fences.push_back(fence);
        ring.RetireSlot(slot->slotIndex, fence);
    }
    fences.front()->signaled.store(true);
    EXPECT_FALSE(ring.Reset());
    EXPECT_FALSE(resources.front().expired());
    EXPECT_FALSE(resources.back().expired());
    EXPECT_EQ(ring.GetInFlightSlotCount(), 2u);

    fences.back()->signaled.store(true);
    EXPECT_TRUE(ring.Reset());
    EXPECT_TRUE(resources.front().expired());
    EXPECT_TRUE(resources.back().expired());
}

TEST(FrameResourceRingTests, ReclaimRunsFrameCleanupBeforeReleasingItsStrongReferences)
{
    NLS::Render::Context::FrameResourceRing ring(2u);
    auto* slot = ring.AcquireSlot(1u);
    ASSERT_NE(slot, nullptr);
    auto resource = std::make_shared<RingTestResource>();
    std::weak_ptr<RingTestResource> retained = resource;
    slot->ownedResources.push_back(std::move(resource));
    slot->retirementPending = true;
    slot->uiTextureRetirementFrameId = 37u;
    auto fence = std::make_shared<RingTestFence>();
    ring.RetireSlot(1u, fence);
    uint32_t cleanupCount = 0u;
    const auto cleanup = [&](NLS::Render::Context::FrameResourceSlot& retired)
    {
        EXPECT_FALSE(retained.expired());
        EXPECT_EQ(retired.uiTextureRetirementFrameId, 37u);
        ++cleanupCount;
    };
    EXPECT_FALSE(ring.ReclaimSlot(1u, cleanup));
    EXPECT_TRUE(ring.HasPendingRetirements());
    fence->signaled.store(true);
    EXPECT_TRUE(ring.ReclaimSlot(1u, cleanup));
    EXPECT_TRUE(retained.expired());
    EXPECT_FALSE(ring.HasPendingRetirements());
    EXPECT_EQ(slot->uiTextureRetirementFrameId, 0u);
    EXPECT_EQ(ring.GetOwnedResourceCount(), 0u);
    EXPECT_FALSE(ring.ReclaimSlot(1u, cleanup));
    EXPECT_EQ(cleanupCount, 1u);
}

TEST(FrameResourceRingTests, QuarantinePreservesResourcesEvenWhenAStaleFenceIsSignaled)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    auto resource = std::make_shared<RingTestResource>();
    std::weak_ptr<RingTestResource> retained = resource;
    slot->ownedResources.push_back(std::move(resource));
    auto fence = std::make_shared<RingTestFence>();
    fence->signaled.store(true);
    ring.RetireSlot(0u, fence);
    ring.QuarantineSlot(0u);
    EXPECT_FALSE(ring.IsSlotAvailable(0u));
    EXPECT_EQ(ring.AcquireSlot(), nullptr);
    EXPECT_EQ(ring.ReclaimCompletedSlots(), 0u);
    EXPECT_FALSE(ring.ReclaimSlot(0u));
    EXPECT_FALSE(ring.Reset());
    EXPECT_FALSE(ring.DrainAll({}, 1'000'000u));
    ring.RetireSlot(0u, nullptr);
    EXPECT_FALSE(retained.expired());
    EXPECT_EQ(ring.GetOwnedResourceCount(), 1u);
}

TEST(FrameResourceRingTests, GrowingTheRingPreservesPendingResourcesAndFenceOwnership)
{
    NLS::Render::Context::FrameResourceRing ring(1u);
    ring.EnsureFrameContexts(1u);
    auto* context = &ring.GetFrameContext(0u);
    context->frameIndex = 91u;
    auto* slot = ring.AcquireSlot();
    ASSERT_NE(slot, nullptr);
    auto resource = std::make_shared<RingTestResource>();
    std::weak_ptr<RingTestResource> retained = resource;
    slot->ownedResources.push_back(std::move(resource));
    auto fence = std::make_shared<RingTestFence>();
    ring.RetireSlot(0u, fence);
    ring.Grow(3u);
    EXPECT_EQ(ring.GetSlotCount(), 3u);
    EXPECT_EQ(&ring.GetFrameContext(0u), context);
    EXPECT_EQ(context->frameIndex, 91u);
    ring.EnsureFrameContexts(3u);
    EXPECT_EQ(ring.GetFrameContextCount(), 3u);
    EXPECT_EQ(&ring.GetFrameContext(0u), context);
    EXPECT_EQ(ring.GetFrameContext(2u).frameIndex, 0u);
    EXPECT_EQ(ring.PeekSlot(0u)->gpuFence, fence);
    EXPECT_EQ(ring.GetOwnedResourceCount(), 1u);
    EXPECT_FALSE(retained.expired());
    EXPECT_EQ(ring.AcquireSlot(0u), nullptr);
    EXPECT_NE(ring.AcquireSlot(2u), nullptr);
    ring.RetireSlot(2u, nullptr);
    fence->signaled.store(true);
    EXPECT_TRUE(ring.ReclaimSlot(0u));
    EXPECT_TRUE(retained.expired());
}

TEST(RhiThreadModePolicyTests, BackendDefaultsMatchTheDocumentedMatrix)
{
    using NLS::Render::RHI::NativeBackendType;
    using NLS::Render::Settings::GetDefaultRhiThreadMode;
    using NLS::Render::Settings::RhiThreadMode;

    EXPECT_EQ(GetDefaultRhiThreadMode(NativeBackendType::DX12), RhiThreadMode::Tasks);
    EXPECT_EQ(GetDefaultRhiThreadMode(NativeBackendType::Vulkan), RhiThreadMode::Tasks);
    EXPECT_EQ(
        GetDefaultRhiThreadMode(NativeBackendType::Metal),
        RhiThreadMode::DedicatedThread);
}

TEST(RhiThreadModePolicyTests, ForcedUnsupportedModeIsRejectedInsteadOfSilentlyDegraded)
{
    using NLS::Render::RHI::NativeBackendType;
    using NLS::Render::RHI::RHIDeviceCapabilities;
    using NLS::Render::Settings::ResolveRhiThreadMode;
    using NLS::Render::Settings::RhiThreadMode;
    using NLS::Render::Settings::RhiThreadModePreference;

    RHIDeviceCapabilities metalCapabilities;
    metalCapabilities.SetFeature(
        NLS::Render::RHI::RHIDeviceFeature::RhiTasksMode,
        false,
        "Metal RHI tasks mode is gated until thread ownership is validated");

    const auto forced = ResolveRhiThreadMode(
        NativeBackendType::Metal,
        metalCapabilities,
        RhiThreadModePreference::ForceTasks);
    EXPECT_FALSE(forced.accepted);
    EXPECT_EQ(forced.mode, RhiThreadMode::Tasks);
    EXPECT_FALSE(forced.usedBackendDefault);
    EXPECT_NE(forced.summary.find("rejected"), std::string::npos);
    EXPECT_NE(forced.summary.find("thread ownership"), std::string::npos);

    // DedicatedThread is always available, so the same backend accepts it.
    const auto dedicated = ResolveRhiThreadMode(
        NativeBackendType::Metal,
        metalCapabilities,
        RhiThreadModePreference::ForceDedicatedThread);
    EXPECT_TRUE(dedicated.accepted);
    EXPECT_EQ(dedicated.mode, RhiThreadMode::DedicatedThread);
}

TEST(RhiThreadModePolicyTests, CapableBackendAcceptsTasksMode)
{
    using NLS::Render::RHI::NativeBackendType;
    using NLS::Render::RHI::RHIDeviceCapabilities;
    using NLS::Render::Settings::ResolveRhiThreadMode;
    using NLS::Render::Settings::RhiThreadMode;
    using NLS::Render::Settings::RhiThreadModePreference;

    RHIDeviceCapabilities dx12Capabilities;
    dx12Capabilities.supportsRhiTasksMode = true;

    const auto resolution = ResolveRhiThreadMode(
        NativeBackendType::DX12,
        dx12Capabilities,
        RhiThreadModePreference::BackendDefault);
    EXPECT_TRUE(resolution.accepted);
    EXPECT_EQ(resolution.mode, RhiThreadMode::Tasks);
    EXPECT_TRUE(resolution.usedBackendDefault);
}

TEST(RhiThreadModePolicyTests, TasksModeReturnsTheDedicatedRhiLaneToThePool)
{
    using NLS::Base::Threading::ResolveExecutionPoolBudget;

    constexpr uint32_t hardware = 16u;
    const auto dedicated = ResolveExecutionPoolBudget(hardware, 0u, true);
    const auto tasks = ResolveExecutionPoolBudget(hardware, 0u, false);

    EXPECT_EQ(
        tasks.taskGraphWorkerCount - dedicated.taskGraphWorkerCount,
        1u)
        << "Tasks mode must hand the dedicated RHI lane to the rendering pool.";
    EXPECT_EQ(
        tasks.jobsWorkerCount + tasks.taskGraphWorkerCount,
        dedicated.jobsWorkerCount + dedicated.taskGraphWorkerCount + 1u);
}
