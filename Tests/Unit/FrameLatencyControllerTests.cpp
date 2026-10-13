#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "Rendering/Context/FrameLatencyController.h"
#include "Rendering/Context/RenderingThreadSystem.h"

using NLS::Render::Context::FrameLatencyController;

namespace
{
    constexpr auto kNeverWait = std::chrono::nanoseconds::zero();
    constexpr auto kLongWait = std::chrono::milliseconds(2000);
}

TEST(FrameLatencyControllerTests, OpensFramesUntilTheBudgetIsSpent)
{
    FrameLatencyController controller(3u);

    EXPECT_TRUE(controller.BeginFrame(kNeverWait));
    EXPECT_TRUE(controller.BeginFrame(kNeverWait));
    EXPECT_TRUE(controller.BeginFrame(kNeverWait));
    EXPECT_EQ(controller.GetInFlightFrameCount(), 3u);

    // Budget exhausted: a non-waiting caller is refused rather than dropped into
    // an unbounded queue.
    EXPECT_FALSE(controller.BeginFrame(kNeverWait));
    EXPECT_EQ(controller.GetInFlightFrameCount(), 3u);
    EXPECT_EQ(controller.GetStallTimeoutCount(), 1u);
}

TEST(FrameLatencyControllerTests, RetiringOneFrameAdmitsExactlyOneMore)
{
    FrameLatencyController controller(2u);

    ASSERT_TRUE(controller.BeginFrame(kNeverWait));
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));
    ASSERT_FALSE(controller.BeginFrame(kNeverWait));

    ASSERT_TRUE(controller.CompleteOldestFrame());
    EXPECT_EQ(controller.GetInFlightFrameCount(), 1u);
    EXPECT_TRUE(controller.BeginFrame(kNeverWait));
    EXPECT_FALSE(controller.BeginFrame(kNeverWait));
}

TEST(FrameLatencyControllerTests, ReservedCapacityRemainsAvailableToPresentation)
{
    FrameLatencyController controller(3u);
    ASSERT_TRUE(controller.ReserveFrame(kNeverWait, 1u).has_value());
    ASSERT_TRUE(controller.ReserveFrame(kNeverWait, 1u).has_value());
    EXPECT_FALSE(controller.ReserveFrame(kNeverWait, 1u).has_value());
    EXPECT_EQ(controller.GetInFlightFrameCount(), 2u);

    const auto presentation = controller.ReserveFrame(kNeverWait);
    ASSERT_TRUE(presentation.has_value());
    EXPECT_EQ(controller.GetInFlightFrameCount(), 3u);
    EXPECT_FALSE(controller.ReserveFrame(kNeverWait).has_value());
    ASSERT_TRUE(controller.CancelFrame(*presentation));
    EXPECT_FALSE(controller.ReserveFrame(kNeverWait, 1u).has_value());
    EXPECT_TRUE(controller.ReserveFrame(kNeverWait).has_value());
}

TEST(FrameLatencyControllerTests, QueuedConsumersDoNotConsumeTheProducerReservation)
{
    FrameLatencyController controller(3u);
    const auto firstConsumer = controller.ReserveFrame(kNeverWait);
    ASSERT_TRUE(firstConsumer.has_value());
    ASSERT_TRUE(controller.ReserveFrame(kNeverWait).has_value());
    ASSERT_TRUE(controller.ReserveFrame(kNeverWait, 1u).has_value());
    EXPECT_FALSE(controller.ReserveFrame(kNeverWait, 1u).has_value());

    ASSERT_TRUE(controller.CancelFrame(*firstConsumer));
    EXPECT_TRUE(controller.ReserveFrame(kNeverWait, 1u).has_value());
    EXPECT_EQ(controller.GetInFlightFrameCount(), 3u);
}

TEST(FrameLatencyControllerTests, CompletingWithNoOpenFrameIsRejectedNotCounted)
{
    FrameLatencyController controller(1u);

    EXPECT_FALSE(controller.CompleteOldestFrame());
    EXPECT_EQ(controller.GetInFlightFrameCount(), 0u);
    EXPECT_EQ(controller.GetRetiredFrameCount(), 0u);
}

TEST(FrameLatencyControllerTests, WaitingCallerIsReleasedByRetirement)
{
    FrameLatencyController controller(1u);
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));

    std::atomic_bool opened { false };
    std::thread waiter(
        [&controller, &opened]()
        {
            opened.store(controller.BeginFrame(kLongWait), std::memory_order_release);
        });

    // The waiter must still be blocked while the single budget slot is held.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(opened.load(std::memory_order_acquire));

    ASSERT_TRUE(controller.CompleteOldestFrame());
    waiter.join();

    EXPECT_TRUE(opened.load(std::memory_order_acquire));
    EXPECT_EQ(controller.GetInFlightFrameCount(), 1u);
    EXPECT_GE(controller.GetStalledFrameCount(), 1u);
}

TEST(FrameLatencyControllerTests, GameWaitPumpsRetirementWithoutHoldingTheBudgetMutex)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::None;
    ASSERT_TRUE(threads.InitRenderingThreads(params));
    FrameLatencyController controller(1u);
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));
    const auto task = graph.Submit([&] { EXPECT_TRUE(controller.CompleteOldestFrame()); }, NamedThread::Game);
    EXPECT_TRUE(controller.BeginFrame(kLongWait));
    EXPECT_EQ(graph.GetCompletion(task).GetStatus(), TaskStatus::Succeeded);
    EXPECT_EQ(controller.GetInFlightFrameCount(), 1u);
    EXPECT_EQ(controller.GetRetiredFrameCount(), 1u);
    threads.ShutdownRenderingThreads();
    graph.Shutdown(TaskGraphShutdownMode::Immediate);
}

TEST(FrameLatencyControllerTests, TimeoutRefusesTheFrameAndLeavesTheBudgetIntact)
{
    FrameLatencyController controller(1u);
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(controller.BeginFrame(std::chrono::milliseconds(20)));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(elapsed, std::chrono::milliseconds(15));
    // A timed-out frame must not consume a slot: the still-open frame keeps the
    // budget, and the count has to stay exact.
    EXPECT_EQ(controller.GetInFlightFrameCount(), 1u);
    EXPECT_EQ(controller.GetStallTimeoutCount(), 1u);
    EXPECT_GT(controller.GetTotalStallTimeNanoseconds(), 0u);
}

TEST(FrameLatencyControllerTests, ReconcileDropsLeakedFramesInsteadOfWedgeing)
{
    FrameLatencyController controller(2u);
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));

    // Two frames were opened but only one is genuinely in flight: the other never
    // reached its retirement site. The gate has to correct itself or the game
    // thread would block forever on a budget that can never be returned.
    EXPECT_EQ(controller.ReconcileInFlight(1u), 1u);
    EXPECT_EQ(controller.GetInFlightFrameCount(), 1u);
    EXPECT_TRUE(controller.BeginFrame(kNeverWait));

    // Reconcile never grows the count: it only releases what the owner says is gone.
    EXPECT_EQ(controller.ReconcileInFlight(9u), 0u);
    EXPECT_EQ(controller.GetInFlightFrameCount(), 2u);
}

TEST(FrameLatencyControllerTests, ResetReleasesEveryWaiter)
{
    FrameLatencyController controller(1u);
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));

    std::atomic_bool opened { false };
    std::thread waiter(
        [&controller, &opened]()
        {
            opened.store(controller.BeginFrame(kLongWait), std::memory_order_release);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(opened.load(std::memory_order_acquire));

    controller.Reset();
    waiter.join();

    EXPECT_TRUE(opened.load(std::memory_order_acquire));
    EXPECT_EQ(controller.GetInFlightFrameCount(), 1u);
}

TEST(FrameLatencyControllerTests, StopRejectsNewFramesAndReleasesAllInfiniteWaiters)
{
    FrameLatencyController controller(1u);
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));
    std::atomic<uint32_t> completed { 0u };
    std::atomic<uint32_t> admitted { 0u };
    std::vector<std::thread> waiters;
    for (uint32_t i = 0u; i < 3u; ++i)
    {
        waiters.emplace_back([&]
        {
            if (controller.BeginFrame(std::chrono::nanoseconds::max()))
                admitted.fetch_add(1u);
            completed.fetch_add(1u);
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + kLongWait;
    while (controller.GetStalledFrameCount() < 3u && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_EQ(controller.GetStalledFrameCount(), 3u);
    controller.Stop();
    for (auto& waiter : waiters)
        waiter.join();
    EXPECT_EQ(completed.load(), 3u);
    EXPECT_EQ(admitted.load(), 0u);
    EXPECT_FALSE(controller.BeginFrame(kNeverWait));
    EXPECT_EQ(controller.GetInFlightFrameCount(), 1u);
    EXPECT_TRUE(controller.CompleteOldestFrame());
    EXPECT_FALSE(controller.BeginFrame(kNeverWait));
    controller.Reset();
    EXPECT_TRUE(controller.BeginFrame(kNeverWait));
}

TEST(FrameLatencyControllerTests, BudgetIsAlwaysAtLeastOne)
{
    FrameLatencyController controller(0u);
    EXPECT_EQ(controller.GetMaxFramesInFlight(), 1u);
    EXPECT_TRUE(controller.BeginFrame(kNeverWait));
    EXPECT_FALSE(controller.BeginFrame(kNeverWait));

    controller.SetMaxFramesInFlight(4u);
    EXPECT_EQ(controller.GetMaxFramesInFlight(), 4u);
    EXPECT_TRUE(controller.BeginFrame(kNeverWait));

    // Lowering the limit below the open count must not unblock a waiter early.
    controller.SetMaxFramesInFlight(1u);
    EXPECT_FALSE(controller.BeginFrame(kNeverWait));
}

TEST(FrameLatencyControllerTests, TelemetryCountsOpenAndRetireOnce)
{
    FrameLatencyController controller(2u);

    ASSERT_TRUE(controller.BeginFrame(kNeverWait));
    ASSERT_TRUE(controller.BeginFrame(kNeverWait));
    ASSERT_TRUE(controller.CompleteOldestFrame());
    ASSERT_TRUE(controller.CompleteOldestFrame());

    EXPECT_EQ(controller.GetOpenedFrameCount(), 2u);
    EXPECT_EQ(controller.GetRetiredFrameCount(), 2u);
    EXPECT_EQ(controller.GetInFlightFrameCount(), 0u);
    EXPECT_EQ(controller.GetStalledFrameCount(), 0u);
}

namespace
{
    class LatencySequenceSource final : public NLS::Render::Context::IRenderCommandSequenceSource
    {
    public:
        std::atomic<uint64_t> dispatched { 1u };
        std::atomic<uint64_t> submitted { 0u };
        std::atomic_bool failed { false };
        uint64_t GetDispatchedSequence() const override { return dispatched.load(); }
        uint64_t GetSubmitSequence() const override { return submitted.load(); }
        uint64_t GetPresentSequence() const override { return 0u; }
        uint64_t GetPresentRequestSequence() const override { return dispatched.load(); }
        std::string GetSubmissionFailure(const uint64_t sequence) const override
        {
            return sequence != 0u && failed.load() ? "Latency submission failed" : "";
        }
    };

    class FrameLatencyFenceTests : public testing::Test
    {
    protected:
        void SetUp() override
        {
            NLS::Render::Context::RenderCommandDispatcher::ResetForTesting();
            NLS::Render::Context::RenderCommandDispatcher::SetSequenceSource(&source);
        }
        void TearDown() override
        {
            NLS::Render::Context::RenderCommandDispatcher::ResetForTesting();
        }
        NLS::Render::Context::RenderCommandFence Arm(FrameLatencyController& controller, uint64_t frameId)
        {
            using namespace NLS::Render::Context;
            RenderCommandFence fence;
            fence.BeginFence(RenderCommandSyncDepth::Rhi);
            EXPECT_TRUE(controller.ArmFrameFence(frameId, fence));
            return fence;
        }
        LatencySequenceSource source;
    };
}

TEST_F(FrameLatencyFenceTests, RhiFenceReleasesBudgetWithoutWaitingForPresent)
{
    using namespace NLS::Render::Context;
    FrameLatencyController controller(1u);
    const auto frame = controller.ReserveFrame();
    ASSERT_TRUE(frame.has_value());
    auto fence = Arm(controller, *frame);
    EXPECT_FALSE(controller.BeginFrame());
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_FALSE(controller.BeginFrame());
    EXPECT_FALSE(fence.IsFenced());
    source.submitted.store(1u);
    EXPECT_TRUE(controller.BeginFrame());
    EXPECT_EQ(controller.GetRetiredFrameCount(), 1u);
    EXPECT_EQ(source.GetPresentSequence(), 0u);
    EXPECT_EQ(controller.PollCompletedFrames(), 0u);
}

TEST_F(FrameLatencyFenceTests, CancellingNewerReservationDoesNotRetireOlderFence)
{
    using namespace NLS::Render::Context;
    FrameLatencyController controller(2u);
    const auto first = controller.ReserveFrame();
    const auto second = controller.ReserveFrame();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    auto fence = Arm(controller, *first);
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_TRUE(controller.CancelFrame(*second));
    EXPECT_FALSE(controller.CancelFrame(*second));
    ASSERT_TRUE(controller.ReserveFrame().has_value());
    EXPECT_FALSE(controller.BeginFrame());
    EXPECT_FALSE(fence.IsFenced());
    source.submitted.store(1u);
    EXPECT_EQ(controller.PollCompletedFrames(), 1u);
    EXPECT_EQ(controller.GetInFlightFrameCount(), 1u);
    EXPECT_EQ(controller.GetRetiredFrameCount(), 2u);
}

TEST_F(FrameLatencyFenceTests, FailedFenceReleasesWaitersAndRejectsNewFrames)
{
    using namespace NLS::Render::Context;
    FrameLatencyController controller(1u);
    const auto frame = controller.ReserveFrame();
    ASSERT_TRUE(frame.has_value());
    auto fence = Arm(controller, *frame);
    (void)RenderCommandDispatcher::Dispatch();
    std::atomic<uint32_t> completed { 0u };
    std::atomic<uint32_t> admitted { 0u };
    std::vector<std::thread> waiters;
    for (uint32_t i = 0u; i < 3u; ++i)
        waiters.emplace_back([&]
        {
            if (controller.BeginFrame(std::chrono::nanoseconds::max()))
                ++admitted;
            ++completed;
        });
    const auto deadline = std::chrono::steady_clock::now() + kLongWait;
    while (controller.GetStalledFrameCount() < 3u && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    source.failed.store(true);
    for (auto& waiter : waiters)
        waiter.join();
    EXPECT_EQ(completed.load(), 3u);
    EXPECT_EQ(admitted.load(), 0u);
    EXPECT_FALSE(controller.BeginFrame());
    EXPECT_EQ(controller.GetFailureDiagnostic(), "Latency submission failed");
    EXPECT_FALSE(fence.Succeeded());
}

TEST_F(FrameLatencyFenceTests, DiscardedMarkerReleasesBudgetWithFailure)
{
    using namespace NLS::Render::Context;
    FrameLatencyController controller(1u);
    const auto frame = controller.ReserveFrame();
    ASSERT_TRUE(frame.has_value());
    auto fence = Arm(controller, *frame);
    RenderCommandDispatcher::ResetForTesting();
    EXPECT_EQ(controller.PollCompletedFrames(), 1u);
    EXPECT_FALSE(controller.BeginFrame());
    EXPECT_FALSE(controller.GetFailureDiagnostic().empty());
    EXPECT_FALSE(fence.Succeeded());
}
