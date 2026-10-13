#include <gtest/gtest.h>

#include "Rendering/Context/RenderCommandFence.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Support/DeterministicTextureRhiDevice.h"
#include "Rendering/Settings/DriverSettings.h"
#include "TaskGraph/TaskGraph.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    bool WaitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return predicate();
    }

    class FakeSequenceSource final : public NLS::Render::Context::IRenderCommandSequenceSource
    {
    public:
        std::atomic<uint64_t> dispatched { 0u };
        std::atomic<uint64_t> submitted { 0u };
        std::atomic<uint64_t> presented { 0u };
        std::atomic<uint64_t> presentRequested { 0u };
        std::atomic<uint64_t> firstFailed { 0u };

        uint64_t GetDispatchedSequence() const override { return dispatched.load(); }
        uint64_t GetSubmitSequence() const override { return submitted.load(); }
        uint64_t GetPresentSequence() const override { return presented.load(); }
        uint64_t GetPresentRequestSequence() const override { return presentRequested.load(); }
        std::string GetSubmissionFailure(const uint64_t sequence) const override
        {
            const auto failed = firstFailed.load();
            return failed != 0u && sequence >= failed ? "Injected RHI submission failure" : "";
        }
    };

    /**
     * Stand-in for the driver tick: dispatches render commands, which is what the
     * real render thread does before pumping its named-thread queue.
     */
    class TestRenderTick final : public NLS::Render::Context::IRenderingThreadTick
    {
    public:
        std::atomic<uint32_t> renderTicks { 0u };
        std::atomic<uint32_t> rhiTicks { 0u };
        std::atomic_bool renderIdentitySeen { false };
        std::atomic_bool rhiIdentitySeen { false };
        std::atomic_bool gameIdentitySeenOnRenderThread { false };

        bool TickRenderThread() override
        {
            renderTicks.fetch_add(1u, std::memory_order_acq_rel);
            if (NLS::Render::Context::RenderingThreadSystem::IsInRenderingThread())
                renderIdentitySeen.store(true, std::memory_order_release);
            if (NLS::Render::Context::RenderingThreadSystem::IsInGameThread())
                gameIdentitySeenOnRenderThread.store(true, std::memory_order_release);

            const size_t executed = NLS::Render::Context::RenderCommandDispatcher::Dispatch(64u);
            std::this_thread::sleep_for(1ms);
            return executed > 0u;
        }

        bool TickRhiLane() override
        {
            rhiTicks.fetch_add(1u, std::memory_order_acq_rel);
            if (NLS::Render::Context::RenderingThreadSystem::IsInRhiThread())
                rhiIdentitySeen.store(true, std::memory_order_release);
            std::this_thread::sleep_for(1ms);
            return true;
        }
    };

    class RenderCommandFailureTests : public testing::Test
    {
    protected:
        void SetUp() override
        {
            NLS::Render::Context::RenderCommandDispatcher::ResetForTesting();
            NLS::Render::Context::ResetRenderFatalErrorForTesting();
        }

        void TearDown() override
        {
            NLS::Render::Context::RenderCommandDispatcher::ResetForTesting();
            NLS::Render::Context::ResetRenderFatalErrorForTesting();
        }
    };
}

TEST(RenderCommandPipeTests, SecondDrainerCannotRunCommandsOutOfOrder)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();

    std::atomic_bool firstRunning { false };
    std::atomic_bool firstFinished { false };
    std::atomic_bool overlap { false };
    std::atomic_bool secondRan { false };

    RenderCommandDispatcher::Enqueue(
        [&firstRunning, &firstFinished, &overlap, &secondRan]
        {
            firstRunning.store(true, std::memory_order_release);
            // Long enough that a competing drainer has every chance to start.
            for (int spin = 0; spin < 40; ++spin)
            {
                if (secondRan.load(std::memory_order_acquire))
                    overlap.store(true, std::memory_order_release);
                std::this_thread::sleep_for(1ms);
            }
            firstFinished.store(true, std::memory_order_release);
        });
    RenderCommandDispatcher::Enqueue([&secondRan] { secondRan.store(true, std::memory_order_release); });

    std::thread competing([&firstFinished]
        {
            // Try to steal the pipe while the first command is still running.
            while (!firstFinished.load(std::memory_order_acquire))
            {
                (void)RenderCommandDispatcher::Dispatch();
                std::this_thread::sleep_for(1ms);
            }
        });

    (void)RenderCommandDispatcher::Dispatch();
    firstFinished.store(true, std::memory_order_release);
    competing.join();

    // The first command is only removed from the queue, not finished, while it
    // runs. Without exclusive drain ownership a second drainer would execute the
    // command behind it and silently break the pipe's FIFO contract.
    EXPECT_FALSE(overlap.load(std::memory_order_acquire));
    EXPECT_TRUE(secondRan.load(std::memory_order_acquire));
}

TEST(RenderCommandPipeTests, MainPipeExecutesCommandsInFifoOrder)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();

    std::vector<uint32_t> order;
    for (uint32_t index = 0u; index < 16u; ++index)
        RenderCommandDispatcher::Enqueue([&order, index] { order.push_back(index); });

    EXPECT_EQ(RenderCommandDispatcher::GetMainQueueDepth(), 16u);
    EXPECT_EQ(RenderCommandDispatcher::Dispatch(), 16u);

    ASSERT_EQ(order.size(), 16u);
    for (uint32_t index = 0u; index < 16u; ++index)
        EXPECT_EQ(order[index], index);
    EXPECT_TRUE(RenderCommandDispatcher::IsEmpty());
}

TEST(RenderCommandPipeTests, NestedDrainCannotConsumeTheNextCommandBeforeTheCurrentCommandReturns)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();
    std::vector<int> order;
    RenderCommandDispatcher::Enqueue([&]
    {
        order.push_back(1);
        EXPECT_EQ(RenderCommandDispatcher::Dispatch(), 0u);
        order.push_back(2);
    });
    RenderCommandDispatcher::Enqueue([&] { order.push_back(3); });
    EXPECT_EQ(RenderCommandDispatcher::Dispatch(), 2u);
    EXPECT_EQ(order, (std::vector<int> { 1, 2, 3 }));
}

TEST(RenderCommandPipeTests, CommandCompletionEventSignalsAfterExecution)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();

    RenderCommandEvent completion;
    std::atomic_bool ran { false };
    RenderCommandDispatcher::Enqueue(
        [&ran] { ran.store(true, std::memory_order_release); },
        completion);

    EXPECT_FALSE(completion.IsComplete());
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_TRUE(completion.WaitFor(100ms));
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(RenderCommandPipeTests, MergeAsyncBlocksTheMainPipeUntilTheDependencyRan)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();

    std::atomic_bool asyncFinished { false };
    const auto dependency = RenderCommandDispatcher::EnqueueAsync(
        [&asyncFinished]
        {
            std::this_thread::sleep_for(10ms);
            asyncFinished.store(true, std::memory_order_release);
        });

    std::atomic_bool observedFinished { false };
    RenderCommandDispatcher::MergeAsync(dependency);
    RenderCommandDispatcher::Enqueue(
        [&observedFinished, &asyncFinished]
        {
            observedFinished.store(
                asyncFinished.load(std::memory_order_acquire),
                std::memory_order_release);
        });

    (void)RenderCommandDispatcher::FlushToDrain();
    EXPECT_TRUE(asyncFinished.load(std::memory_order_acquire));
    EXPECT_TRUE(observedFinished.load(std::memory_order_acquire));
}

TEST(RenderCommandFenceTests, RenderDepthFenceWaitsForTheMarkerCommand)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();

    std::atomic_bool ran { false };
    RenderCommandDispatcher::Enqueue([&ran] { ran.store(true, std::memory_order_release); });

    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Render);
    EXPECT_FALSE(fence.IsFenced());

    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_TRUE(fence.IsFenced());
}

TEST(RenderCommandFenceTests, RhiDepthFenceWaitsForDispatchedWorkToBeSubmitted)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();

    FakeSequenceSource source;
    RenderCommandDispatcher::SetSequenceSource(&source);

    // Three lists were handed to the executor before the marker.
    RenderCommandDispatcher::Enqueue([&source] { source.dispatched.store(3u); });

    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Rhi);
    (void)RenderCommandDispatcher::Dispatch();

    EXPECT_EQ(fence.GetTargetSubmitSequence(), 3u);
    EXPECT_FALSE(fence.IsFenced());

    source.submitted.store(2u);
    EXPECT_FALSE(fence.IsFenced());

    source.submitted.store(3u);
    EXPECT_TRUE(fence.IsFenced());
}

TEST(RenderCommandFenceTests, SwapchainDepthFenceAlsoWaitsForThePresent)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();

    FakeSequenceSource source;
    RenderCommandDispatcher::SetSequenceSource(&source);

    RenderCommandDispatcher::Enqueue(
        [&source]
        {
            source.dispatched.store(2u);
            source.presentRequested.store(2u);
        });

    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Swapchain);
    (void)RenderCommandDispatcher::Dispatch();

    EXPECT_EQ(fence.GetTargetSubmitSequence(), 2u);
    EXPECT_EQ(fence.GetTargetPresentSequence(), 2u);

    source.submitted.store(2u);
    EXPECT_FALSE(fence.IsFenced()) << "A swapchain fence must not complete on submit alone.";

    source.presented.store(2u);
    EXPECT_TRUE(fence.IsFenced());
}

TEST(RenderingThreadSystemTests, ReportsThreadIdentityForDedicatedRhiMode)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    TestRenderTick tick;
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.tick = &tick;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::DedicatedThread;
    ASSERT_TRUE(threads.InitRenderingThreads(params));

    EXPECT_TRUE(RenderingThreadSystem::IsInGameThread());
    EXPECT_EQ(graph.GetCurrentNamedThread(), NLS::Base::TaskGraph::NamedThread::Game);
    EXPECT_FALSE(RenderingThreadSystem::IsInRenderingThread());
    EXPECT_FALSE(RenderingThreadSystem::IsInRhiThread());

    EXPECT_TRUE(WaitUntil([&tick] { return tick.renderIdentitySeen.load(); }, 2s));
    EXPECT_FALSE(tick.gameIdentitySeenOnRenderThread.load())
        << "The render thread must not report itself as the game thread.";

    EXPECT_TRUE(WaitUntil([&tick] { return tick.rhiTicks.load() > 0u; }, 2s));
    EXPECT_TRUE(tick.rhiIdentitySeen.load());
    EXPECT_EQ(threads.GetRhiThreadMode(), RhiThreadMode::DedicatedThread);

    threads.ShutdownRenderingThreads();
    EXPECT_FALSE(threads.IsInitialized());
    EXPECT_FALSE(graph.IsNamedThreadRegistered(NLS::Base::TaskGraph::NamedThread::Game));
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, StartupGameIdentitySurvivesRenderingShutdown)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    const bool wasGameThread = RenderingThreadSystem::IsInGameThread();
    TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    {
        const ScopedGameThread startup;
        EXPECT_TRUE(RenderingThreadSystem::IsInGameThread());
        EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Any);
        RenderingThreadSystem threads;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.rhiThreadMode = RhiThreadMode::None;
        ASSERT_TRUE(threads.InitRenderingThreads(params));
        EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Game);
        threads.ShutdownRenderingThreads();
        EXPECT_TRUE(RenderingThreadSystem::IsInGameThread());
        EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Any);
    }
    EXPECT_EQ(RenderingThreadSystem::IsInGameThread(), wasGameThread);
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, StartupGameScopeRetainsAttachmentAndPumpsCallbacks)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    {
        const ScopedGameThread startup(&graph);
        RenderingThreadSystem threads;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.rhiThreadMode = RhiThreadMode::None;
        ASSERT_TRUE(threads.InitRenderingThreads(params));
        threads.ShutdownRenderingThreads();
        EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Game);
        bool callbackRan = false;
        auto completion = graph.GetCompletion(graph.Submit([&] { callbackRan = true; }, NamedThread::Game));
        EXPECT_EQ(RenderingThreadSystem::PumpGameThreadTasks(), 1u);
        EXPECT_TRUE(completion.IsComplete());
        EXPECT_TRUE(callbackRan);
    }
    EXPECT_FALSE(graph.IsNamedThreadRegistered(NamedThread::Game));
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, StartupGameScopeRejectsAnotherThread)
{
    using namespace NLS::Render::Context;
    const ScopedGameThread startup;
    std::thread competitor([]
    {
        EXPECT_THROW(ScopedGameThread {}, std::runtime_error);
        EXPECT_FALSE(RenderingThreadSystem::IsInGameThread());
    });
    competitor.join();
    EXPECT_TRUE(RenderingThreadSystem::IsInGameThread());
}

TEST(RenderingThreadSystemTests, GameAttachmentPumpsTaskCompletionDependencies)
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
    const auto gameId = std::this_thread::get_id();
    std::atomic_bool ranOnGame { false };
    const auto gameTask = graph.Submit([&]
    {
        ranOnGame.store(std::this_thread::get_id() == gameId && RenderingThreadSystem::IsInGameThread());
    }, NamedThread::Game);
    TaskDesc worker;
    worker.prerequisites = { gameTask };
    worker.function = [] {};
    auto completion = graph.GetCompletion(graph.Submit(std::move(worker)));
    EXPECT_TRUE(completion.WaitFor(1s));
    EXPECT_EQ(completion.GetStatus(), TaskStatus::Succeeded);
    EXPECT_TRUE(ranOnGame.load());
    threads.ShutdownRenderingThreads();
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, SharedGameAttachmentIsReleasedByItsLastOwner)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    const bool wasGameThread = RenderingThreadSystem::IsInGameThread();
    TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderingThreadSystem first;
    RenderingThreadSystem second;
    RenderingThreadInitParams params;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::None;
    ASSERT_TRUE(first.InitRenderingThreads(params));
    ASSERT_TRUE(second.InitRenderingThreads(params));
    first.ShutdownRenderingThreads();
    EXPECT_TRUE(RenderingThreadSystem::IsInGameThread());
    EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Game);
    auto completion = graph.GetCompletion(graph.Submit([] {}, NamedThread::Game));
    EXPECT_TRUE(completion.WaitFor(1s));
    second.ShutdownRenderingThreads();
    EXPECT_EQ(RenderingThreadSystem::IsInGameThread(), wasGameThread);
    EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Any);
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, BorrowedGameAttachmentSurvivesRenderingShutdown)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    const bool wasGameThread = RenderingThreadSystem::IsInGameThread();
    TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    ASSERT_TRUE(graph.RegisterCurrentThread(NamedThread::Game));
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::None;
    ASSERT_TRUE(threads.InitRenderingThreads(params));
    threads.ShutdownRenderingThreads();
    EXPECT_EQ(RenderingThreadSystem::IsInGameThread(), wasGameThread);
    EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Game);
    graph.UnregisterCurrentThread(NamedThread::Game);
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, AnotherThreadCannotTakeGameOwnership)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderingThreadSystem owner;
    RenderingThreadInitParams params;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::None;
    ASSERT_TRUE(owner.InitRenderingThreads(params));
    std::atomic_bool accepted { true };
    std::thread competitor([&]
    {
        RenderingThreadSystem other;
        accepted.store(other.InitRenderingThreads(params));
        EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Any);
    });
    competitor.join();
    EXPECT_FALSE(accepted.load());
    EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Game);
    owner.ShutdownRenderingThreads();
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, ForeignOwnerShutdownReleasesTheOriginalGameAttachment)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    const bool wasGameThread = RenderingThreadSystem::IsInGameThread();
    TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::None;
    ASSERT_TRUE(threads.InitRenderingThreads(params));
    std::thread shutdown([&] { threads.ShutdownRenderingThreads(); });
    shutdown.join();
    EXPECT_EQ(RenderingThreadSystem::IsInGameThread(), wasGameThread);
    EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Any);
    EXPECT_FALSE(graph.IsNamedThreadRegistered(NamedThread::Game));
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, SuspendAndResumeKeepGameTasksAttached)
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
    threads.SuspendRenderingThreads();
    EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Game);
    auto completion = graph.GetCompletion(graph.Submit([] {}, NamedThread::Game));
    EXPECT_TRUE(completion.WaitFor(1s));
    EXPECT_TRUE(threads.ResumeRenderingThreads());
    EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Game);
    threads.ShutdownRenderingThreads();
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, ShutdownPumpsGameDependenciesOfStoppingThreads)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    class ShutdownTick final : public IRenderingThreadTick
    {
    public:
        explicit ShutdownTick(TaskGraph& graph) : graph(graph) {}
        bool TickRenderThread() override { return false; }
        bool TickRhiLane() override { return false; }
        void OnRenderThreadStopping() override { WaitOnGame(); }
        void OnRhiLaneStopping() override { WaitOnGame(); }
        void WaitOnGame()
        {
            auto completion = graph.GetCompletion(graph.Submit([] {}, NamedThread::Game));
            if (completion.WaitFor(1s) && completion.GetStatus() == TaskStatus::Succeeded)
                successfulCallbacks.fetch_add(1u);
        }
        TaskGraph& graph;
        std::atomic_uint32_t successfulCallbacks { 0u };
    } tick(graph);
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.taskGraph = &graph;
    params.tick = &tick;
    params.rhiThreadMode = RhiThreadMode::DedicatedThread;
    ASSERT_TRUE(threads.InitRenderingThreads(params));
    threads.ShutdownRenderingThreads();
    EXPECT_EQ(tick.successfulCallbacks.load(), 2u);
    graph.Shutdown(TaskGraphShutdownMode::Immediate);
}

TEST(RenderCommandEventTests, GameWaitPumpsTasksWithoutHoldingTheEventMutex)
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
    RenderCommandEvent event;
    const auto task = graph.Submit([event] { event.Signal(); }, NamedThread::Game);
    EXPECT_TRUE(event.WaitFor(1s));
    EXPECT_TRUE(event.Succeeded());
    EXPECT_EQ(graph.GetCompletion(task).GetStatus(), TaskStatus::Succeeded);
    threads.ShutdownRenderingThreads();
    graph.Shutdown(TaskGraphShutdownMode::Immediate);
}

TEST(RenderCommandFenceTests, GameWaitPumpsDependenciesAtEverySyncDepth)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;
    for (const auto depth : { RenderCommandSyncDepth::Render, RenderCommandSyncDepth::Rhi,
        RenderCommandSyncDepth::Swapchain })
    {
        SCOPED_TRACE(ToString(depth));
        RenderCommandDispatcher::ResetForTesting();
        TaskGraph graph({ 1u, false, true });
        ASSERT_TRUE(graph.Start());
        FakeSequenceSource source;
        source.dispatched.store(1u);
        source.presentRequested.store(1u);
        RenderCommandDispatcher::SetSequenceSource(&source);
        TestRenderTick tick;
        RenderingThreadSystem threads;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.tick = &tick;
        params.rhiThreadMode = RhiThreadMode::None;
        ASSERT_TRUE(threads.InitRenderingThreads(params));
        if (depth == RenderCommandSyncDepth::Render)
        {
            EnqueueRenderCommand<struct GameDependencyCommand>([&]
            {
                auto completion = graph.GetCompletion(graph.Submit([] {}, NamedThread::Game));
                EXPECT_TRUE(completion.WaitFor(1s));
                EXPECT_EQ(completion.GetStatus(), TaskStatus::Succeeded);
            });
        }
        RenderCommandFence fence;
        fence.BeginFence(depth);
        if (depth != RenderCommandSyncDepth::Render)
        {
            EXPECT_TRUE(WaitUntil([&] { return fence.GetTargetSubmitSequence() == 1u; }, 1s));
            const auto task = graph.Submit([&]
            {
                source.submitted.store(1u);
                source.presented.store(1u);
            }, NamedThread::Game);
            EXPECT_TRUE(task.IsValid());
        }
        EXPECT_TRUE(fence.WaitFor(1s));
        EXPECT_TRUE(fence.Succeeded());
        threads.ShutdownRenderingThreads();
        RenderCommandDispatcher::SetSequenceSource(nullptr);
        graph.Shutdown(TaskGraphShutdownMode::Immediate);
    }
}

TEST(RenderingThreadSystemTests, TasksModeUsesTheLogicalRhiLaneWithoutADedicatedThread)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    TestRenderTick tick;
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.tick = &tick;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::Tasks;
    ASSERT_TRUE(threads.InitRenderingThreads(params));

    EXPECT_EQ(threads.GetRhiThreadMode(), RhiThreadMode::Tasks);
    EXPECT_FALSE(RenderingThreadSystem::IsInRhiThread());
    {
        RenderingThreadSystem::ScopedRhiLaneScope laneScope;
        EXPECT_TRUE(RenderingThreadSystem::IsInRhiThread())
            << "Tasks mode defines the RHI lane by scope, not by OS thread id.";
    }
    EXPECT_FALSE(RenderingThreadSystem::IsInRhiThread());

    threads.ShutdownRenderingThreads();
    EXPECT_EQ(tick.rhiTicks.load(), 0u) << "Tasks mode must not spawn a dedicated RHI thread.";
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, FatalErrorStopsTheThreadLoops)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    // The fatal sink is process-wide, so start from a clean slate and clear it
    // again before returning.
    ResetRenderFatalErrorForTesting();
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    TestRenderTick tick;
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.tick = &tick;
    params.taskGraph = &graph;
    ASSERT_TRUE(threads.InitRenderingThreads(params));
    ASSERT_TRUE(WaitUntil([&tick] { return tick.renderTicks.load() > 0u; }, 2s));

    threads.MarkFatalError("unit-test failure");
    EXPECT_TRUE(threads.HasFatalError());
    EXPECT_EQ(threads.GetFatalError(), "unit-test failure");
    EXPECT_TRUE(WaitUntil([&threads] { return threads.IsStopRequested(); }, 2s));

    // Shutdown must return instead of waiting for a loop that cannot make progress.
    threads.ShutdownRenderingThreads();
    EXPECT_FALSE(threads.IsInitialized());
    ResetRenderFatalErrorForTesting();
    graph.Shutdown();
}

TEST(RenderingThreadSystemTests, RenderThreadDispatchesQueuedCommandsForFlush)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    TestRenderTick tick;
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.tick = &tick;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::DedicatedThread;
    ASSERT_TRUE(threads.InitRenderingThreads(params));

    std::atomic<uint32_t> executed { 0u };
    RenderCommandDispatcher::Enqueue([&executed] { executed.fetch_add(1u); });
    ASSERT_TRUE(WaitUntil([&executed] { return executed.load() > 0u; }, 2s));

    // With no RHI executor bound, the flush stops at the render depth and returns.
    FlushRenderingCommands(RhiFlushFlags::Dispatch | RhiFlushFlags::Translate);
    EXPECT_EQ(RenderCommandDispatcher::GetMainQueueDepth(), 0u);

    threads.ShutdownRenderingThreads();
    graph.Shutdown();
}

TEST(RenderCommandFenceTests, SlowRenderThreadRetainsPipeOwnershipDuringGameThreadFlush)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();
    std::atomic_bool allowDispatch { false };
    std::atomic_bool ranOnRenderThread { false };
    class DelayedTick final : public IRenderingThreadTick
    {
    public:
        explicit DelayedTick(std::atomic_bool& ready) : m_ready(ready) {}
        bool TickRenderThread() override
        {
            return m_ready.load() && RenderCommandDispatcher::Dispatch() > 0u;
        }
        bool TickRhiLane() override { return false; }
    private:
        std::atomic_bool& m_ready;
    } tick(allowDispatch);

    NLS::Base::TaskGraph::TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.taskGraph = &graph;
    params.tick = &tick;
    params.rhiThreadMode = RhiThreadMode::None;
    ASSERT_TRUE(threads.InitRenderingThreads(params));
    RenderCommandDispatcher::Enqueue([&]
    {
        ranOnRenderThread.store(RenderingThreadSystem::IsInRenderingThread());
    });
    EXPECT_EQ(RenderCommandDispatcher::Dispatch(), 0u);
    std::thread release([&]
    {
        std::this_thread::sleep_for(30ms);
        allowDispatch.store(true);
    });
    FlushRenderingCommands(RhiFlushFlags::Dispatch);
    release.join();
    EXPECT_TRUE(ranOnRenderThread.load());

    std::atomic_bool workerRejected { false };
    std::thread worker([&]
    {
        try { FlushRenderingCommands(); }
        catch (const std::logic_error&) { workerRejected.store(true); }
    });
    worker.join();
    EXPECT_TRUE(workerRejected.load());
    threads.ShutdownRenderingThreads();
    EXPECT_FALSE(RenderingThreadSystem::HasActiveRenderThread());
    graph.Shutdown();
}

TEST(DriverRhiThreadModeTests, ThreadedDriverReportsTheResolvedRhiLaneMode)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    // With no device the backend is reported as unknown, so the safe default is
    // the dedicated lane; the summary must still name the decision.
    const std::string summary = driver.GetRhiThreadModeSummary();
    EXPECT_NE(summary.find("resolved=DedicatedThread"), std::string::npos) << summary;
    EXPECT_EQ(summary.find("rejected"), std::string::npos) << summary;
}

TEST(DriverRhiThreadModeTests, ForcedUnsupportedModeFailsStartupWithTheCapabilityReason)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    // The None backend does not advertise the tasks lane capability.
    settings.rhiThreadMode = NLS::Render::Settings::RhiThreadModePreference::ForceTasks;

    try
    {
        NLS::Render::Context::Driver driver(settings);
        FAIL() << "An unsupported forced mode must fail startup.";
    }
    catch (const std::runtime_error& error)
    {
        const std::string reason = error.what();
        EXPECT_NE(reason.find("rejected"), std::string::npos) << reason;
        EXPECT_NE(reason.find("Tasks"), std::string::npos) << reason;
        EXPECT_NE(reason.find("capability"), std::string::npos) << reason;
    }
}

TEST(RenderCommandAsyncPipeTests, RunsOnAPoolWorkerInsteadOfTheCallingThread)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderCommandDispatcher::SetTaskGraph(&graph);

    const std::thread::id callerId = std::this_thread::get_id();
    std::thread::id bodyThreadId {};
    std::atomic_bool ran { false };

    const auto dependency = RenderCommandDispatcher::EnqueueAsync(
        [&bodyThreadId, &ran]
        {
            bodyThreadId = std::this_thread::get_id();
            ran.store(true, std::memory_order_release);
        });

    ASSERT_TRUE(dependency.WaitFor(5s));
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_NE(bodyThreadId, callerId)
        << "A bound TaskGraph must run async render commands off the calling thread.";
    EXPECT_NE(bodyThreadId, std::thread::id {});

    EXPECT_TRUE(RenderCommandDispatcher::FlushToDrain() >= 0u);
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 0u);

    RenderCommandDispatcher::SetTaskGraph(nullptr);
    graph.Shutdown();
}

TEST(RenderCommandAsyncPipeTests, FlushWaitsForOutstandingAsyncWork)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderCommandDispatcher::SetTaskGraph(&graph);

    std::atomic_bool prepared { false };
    RenderCommandDispatcher::EnqueueAsync(
        [&prepared]
        {
            std::this_thread::sleep_for(20ms);
            prepared.store(true, std::memory_order_release);
        });
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 1u);

    // A full drain must not return while pool work is still live.
    (void)RenderCommandDispatcher::FlushToDrain();
    EXPECT_TRUE(prepared.load(std::memory_order_acquire));
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 0u);

    RenderCommandDispatcher::SetTaskGraph(nullptr);
    graph.Shutdown();
}

TEST(RenderCommandAsyncPipeTests, FallsBackToInlineExecutionWithoutAGraph)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    ASSERT_EQ(RenderCommandDispatcher::GetTaskGraph(), nullptr);

    const std::thread::id callerId = std::this_thread::get_id();
    std::thread::id bodyThreadId {};
    const auto dependency = RenderCommandDispatcher::EnqueueAsync(
        [&bodyThreadId] { bodyThreadId = std::this_thread::get_id(); });

    EXPECT_FALSE(dependency.IsComplete());
    EXPECT_EQ(RenderCommandDispatcher::GetAsyncQueueDepth(), 1u);

    (void)RenderCommandDispatcher::FlushToDrain();
    EXPECT_TRUE(dependency.IsComplete());
    EXPECT_EQ(bodyThreadId, callerId)
        << "Without a pool the async pipe must degrade to inline execution.";
}

TEST(RenderCommandPipeTests, ClearingSignalsDroppedCommandCompletions)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    const auto discardedBefore = RenderCommandDispatcher::GetMainPipe().GetDiscardedCount();

    RenderCommandEvent dropped;
    bool executed = false;
    RenderCommandDispatcher::Enqueue([&executed] { executed = true; }, dropped);
    ASSERT_EQ(RenderCommandDispatcher::GetMainQueueDepth(), 1u);

    // Clearing drops the command, so it must also release anyone waiting on its
    // completion: a fence left waiting on a discarded marker would block forever.
    RenderCommandDispatcher::GetMainPipe().Clear();
    EXPECT_TRUE(dropped.IsComplete());
    EXPECT_EQ(dropped.GetStatus(), RenderCommandStatus::Cancelled);
    EXPECT_FALSE(dropped.Succeeded());
    EXPECT_NE(dropped.GetDiagnostic().find("cleared"), std::string::npos);
    EXPECT_FALSE(executed);
    EXPECT_EQ(RenderCommandDispatcher::GetMainQueueDepth(), 0u);
    EXPECT_EQ(RenderCommandDispatcher::GetMainPipe().GetDiscardedCount(), discardedBefore + 1u);
}

TEST_F(RenderCommandFailureTests, CancelledWorkerDependencyCannotWedgeTheMainPipe)
{
    using namespace NLS::Render::Context;
    using namespace NLS::Base::TaskGraph;

    NLS::Base::TaskGraph::TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderCommandDispatcher::SetTaskGraph(&graph);

    RenderCommandEvent workerStarted;
    RenderCommandEvent releaseWorker;
    (void)graph.Submit([workerStarted, releaseWorker]
    {
        workerStarted.Signal();
        (void)releaseWorker.WaitFor(5s);
    });
    ASSERT_TRUE(workerStarted.WaitFor(1s));

    std::atomic_bool bodyRan { false };
    const auto dependency = RenderCommandDispatcher::EnqueueAsync(
        [&bodyRan] { bodyRan.store(true); });
    RenderCommandDispatcher::SetTaskGraph(nullptr);
    std::thread shutdown([&graph] { graph.Shutdown(TaskGraphShutdownMode::Immediate); });
    // Cancellation must signal before shutdown can join the occupied worker.
    EXPECT_TRUE(dependency.WaitFor(1s));
    releaseWorker.Signal();
    shutdown.join();
    EXPECT_EQ(dependency.GetStatus(), RenderCommandStatus::Cancelled);
    EXPECT_FALSE(bodyRan.load());
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 0u);

    RenderCommandDispatcher::MergeAsync(dependency, 50ms);
    bool consumed = false;
    const auto consumer = EnqueueRenderCommand<struct CancelledDependencyConsumer>([&] { consumed = true; });

    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(RenderCommandDispatcher::Dispatch(), 1u);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 2s);
    EXPECT_EQ(RenderCommandDispatcher::GetAbandonedAsyncMergeCount(), 0u);
    EXPECT_FALSE(consumed);
    EXPECT_EQ(consumer.GetStatus(), RenderCommandStatus::Cancelled);
    EXPECT_TRUE(HasRenderFatalError());
}

TEST_F(RenderCommandFailureTests, EventFailureReleasesAllWaitersAndPreservesFirstOutcome)
{
    using namespace NLS::Render::Context;
    RenderCommandEvent event;
    std::vector<std::future<RenderCommandStatus>> waiters;
    for (int index = 0; index < 8; ++index)
        waiters.push_back(std::async(std::launch::async, [event]
        {
            event.Wait();
            return event.GetStatus();
        }));
    event.Fail("original failure");
    event.Signal();
    event.Cancel("later cancellation");
    for (auto& waiter : waiters)
        EXPECT_EQ(waiter.get(), RenderCommandStatus::Failed);
    EXPECT_FALSE(event.Succeeded());
    EXPECT_EQ(event.GetDiagnostic(), "original failure");
}

TEST_F(RenderCommandFailureTests, CommandExceptionCancelsConsumersAndEveryFenceDepth)
{
    using namespace NLS::Render::Context;
    const auto failing = EnqueueRenderCommand<struct FailingRenderCommand>([]
    {
        throw std::runtime_error("injected scene update failure");
    });
    bool consumed = false;
    const auto consumer = EnqueueRenderCommand<struct AfterFailedSceneUpdate>([&] { consumed = true; });
    RenderCommandFence renderFence;
    RenderCommandFence rhiFence;
    RenderCommandFence presentFence;
    renderFence.BeginFence(RenderCommandSyncDepth::Render);
    rhiFence.BeginFence(RenderCommandSyncDepth::Rhi);
    presentFence.BeginFence(RenderCommandSyncDepth::Swapchain);

    EXPECT_EQ(RenderCommandDispatcher::Dispatch(), 1u);
    EXPECT_EQ(failing.GetStatus(), RenderCommandStatus::Failed);
    EXPECT_FALSE(consumed);
    EXPECT_EQ(consumer.GetStatus(), RenderCommandStatus::Cancelled);
    for (const auto* fence : { &renderFence, &rhiFence, &presentFence })
    {
        EXPECT_TRUE(fence->WaitFor(100ms));
        EXPECT_FALSE(fence->Succeeded());
        EXPECT_NE(fence->GetDiagnostic().find("injected scene update failure"), std::string::npos);
    }
    const auto rejected = EnqueueRenderCommand<struct AfterFatalRenderError>([&] { consumed = true; });
    EXPECT_EQ(rejected.GetStatus(), RenderCommandStatus::Cancelled);
    EXPECT_TRUE(RenderCommandDispatcher::IsEmpty());
}

TEST_F(RenderCommandFailureTests, AsyncExceptionCancelsResourceConsumers)
{
    using namespace NLS::Render::Context;
    const auto dependency = RenderCommandDispatcher::EnqueueAsync([]
    {
        throw std::runtime_error("injected resource preparation failure");
    });
    RenderCommandDispatcher::MergeAsync(dependency);
    bool consumed = false;
    const auto consumer = EnqueueRenderCommand<struct FailedPreparationConsumer>([&] { consumed = true; });
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_TRUE(dependency.WaitFor(100ms));
    EXPECT_EQ(dependency.GetStatus(), RenderCommandStatus::Failed);
    EXPECT_NE(dependency.GetDiagnostic().find("injected resource preparation failure"), std::string::npos);
    EXPECT_FALSE(consumed);
    EXPECT_EQ(consumer.GetStatus(), RenderCommandStatus::Cancelled);
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 0u);
}

TEST_F(RenderCommandFailureTests, DependencyTimeoutFailsBeforeConsumingUnpreparedResources)
{
    using namespace NLS::Render::Context;
    RenderCommandEvent missingProducer;
    RenderCommandDispatcher::MergeAsync(missingProducer, 5ms);
    bool consumed = false;
    const auto consumer = EnqueueRenderCommand<struct TimedOutPreparationConsumer>([&] { consumed = true; });
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_TRUE(missingProducer.IsComplete());
    EXPECT_EQ(missingProducer.GetStatus(), RenderCommandStatus::Failed);
    EXPECT_EQ(missingProducer.GetDiagnostic(), "Async render dependency timed out");
    EXPECT_FALSE(consumed);
    EXPECT_EQ(consumer.GetStatus(), RenderCommandStatus::Cancelled);
    EXPECT_EQ(RenderCommandDispatcher::GetAbandonedAsyncMergeCount(), 1u);
}

TEST_F(RenderCommandFailureTests, FatalErrorCancelsQueuedWorkerDependenciesBeforeWorkersUnblock)
{
    using namespace NLS::Render::Context;
    NLS::Base::TaskGraph::TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderCommandDispatcher::SetTaskGraph(&graph);
    RenderCommandEvent started;
    RenderCommandEvent release;
    (void)graph.Submit([started, release]
    {
        started.Signal();
        (void)release.WaitFor(5s);
    });
    ASSERT_TRUE(started.WaitFor(1s));
    bool ran = false;
    const auto dependency = RenderCommandDispatcher::EnqueueAsync([&] { ran = true; });
    ReportRenderFatalError("fatal error while resource task was queued");
    EXPECT_TRUE(dependency.WaitFor(100ms));
    EXPECT_EQ(dependency.GetStatus(), RenderCommandStatus::Cancelled);
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 1u);

    RenderCommandDispatcher::ResetForTesting();
    ResetRenderFatalErrorForTesting();
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 1u);
    release.Signal();
    graph.Shutdown();
    EXPECT_FALSE(ran);
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 0u);
}

TEST_F(RenderCommandFailureTests, ResetCannotUnderflowTheCountOfAnAlreadyRunningAsyncBody)
{
    using namespace NLS::Render::Context;
    NLS::Base::TaskGraph::TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    RenderCommandDispatcher::SetTaskGraph(&graph);
    RenderCommandEvent started;
    RenderCommandEvent release;
    const auto dependency = RenderCommandDispatcher::EnqueueAsync([started, release]
    {
        started.Signal();
        (void)release.WaitFor(5s);
    });
    ASSERT_TRUE(started.WaitFor(1s));
    RenderCommandDispatcher::ResetForTesting();
    EXPECT_TRUE(dependency.IsComplete());
    EXPECT_FALSE(dependency.Succeeded());
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 1u);
    release.Signal();
    graph.Shutdown();
    EXPECT_EQ(RenderCommandDispatcher::GetPendingAsyncCount(), 0u);
}

TEST_F(RenderCommandFailureTests, DeepFenceReportsRhiFailureWithoutInvalidatingEarlierFences)
{
    using namespace NLS::Render::Context;
    FakeSequenceSource source;
    RenderCommandDispatcher::SetSequenceSource(&source);
    source.dispatched.store(1u);
    source.submitted.store(1u);
    source.presentRequested.store(1u);
    source.presented.store(1u);
    RenderCommandFence earlier;
    earlier.BeginFence(RenderCommandSyncDepth::Swapchain);
    (void)RenderCommandDispatcher::Dispatch();
    ASSERT_TRUE(earlier.Succeeded());
    source.dispatched.store(2u);
    source.presentRequested.store(2u);
    RenderCommandFence rhiFence;
    RenderCommandFence presentFence;
    rhiFence.BeginFence(RenderCommandSyncDepth::Rhi);
    presentFence.BeginFence(RenderCommandSyncDepth::Swapchain);
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_FALSE(rhiFence.IsFenced());
    source.firstFailed.store(2u);
    for (const auto* fence : { &rhiFence, &presentFence })
    {
        EXPECT_TRUE(fence->WaitFor(100ms));
        EXPECT_FALSE(fence->Succeeded());
        EXPECT_EQ(fence->GetDiagnostic(), "Injected RHI submission failure");
    }
    EXPECT_TRUE(earlier.Succeeded());
    EXPECT_TRUE(earlier.GetDiagnostic().empty());
    RenderCommandDispatcher::SetSequenceSource(nullptr);
    EXPECT_FALSE(rhiFence.Succeeded());
    EXPECT_EQ(rhiFence.GetDiagnostic(), "Injected RHI submission failure");
    EXPECT_TRUE(earlier.Succeeded());
}

TEST_F(RenderCommandFailureTests, DetachingTheExecutorCancelsADeepFenceWithOutstandingWork)
{
    using namespace NLS::Render::Context;
    FakeSequenceSource source;
    source.dispatched.store(1u);
    source.presentRequested.store(1u);
    RenderCommandDispatcher::SetSequenceSource(&source);
    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Swapchain);
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_FALSE(fence.IsFenced());
    RenderCommandDispatcher::SetSequenceSource(nullptr);
    EXPECT_TRUE(fence.WaitFor(100ms));
    EXPECT_FALSE(fence.Succeeded());
    EXPECT_NE(fence.GetDiagnostic().find("executor detached"), std::string::npos);
}

TEST_F(RenderCommandFailureTests, FailureDuringTheProgressReadCannotLookLikeSuccessfulSubmission)
{
    using namespace NLS::Render::Context;
    class FailingSequenceSource final : public IRenderCommandSequenceSource
    {
    public:
        uint64_t GetDispatchedSequence() const override { return 1u; }
        uint64_t GetSubmitSequence() const override
        {
            failed = true;
            return 1u;
        }
        uint64_t GetPresentSequence() const override { return 1u; }
        uint64_t GetPresentRequestSequence() const override { return 1u; }
        std::string GetSubmissionFailure(uint64_t) const override
        {
            return failed ? "RHI failure advanced the sequence" : "";
        }
        mutable bool failed = false;
    } source;
    RenderCommandDispatcher::SetSequenceSource(&source);
    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Rhi);
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_TRUE(fence.WaitFor(100ms));
    EXPECT_FALSE(fence.Succeeded());
    EXPECT_EQ(fence.GetDiagnostic(), "RHI failure advanced the sequence");
}

TEST_F(RenderCommandFailureTests, GameThreadFlushReportsCommandFailureWithoutCallingTheRhiHook)
{
    using namespace NLS::Render::Context;
    NLS::Base::TaskGraph::TaskGraph graph({ 1u, false, true });
    ASSERT_TRUE(graph.Start());
    TestRenderTick tick;
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.taskGraph = &graph;
    params.tick = &tick;
    params.rhiThreadMode = RhiThreadMode::None;
    ASSERT_TRUE(threads.InitRenderingThreads(params));
    std::atomic<uint32_t> hookCalls { 0u };
    SetRenderCommandFlushHook([&](RhiFlushFlags) { ++hookCalls; });
    EnqueueRenderCommand<struct FailedFlushCommand>([] { throw std::runtime_error("flush scene failure"); });
    try
    {
        FlushRenderingCommands();
        FAIL() << "A failed render command must fail the flush.";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string(error.what()).find("flush scene failure"), std::string::npos);
    }
    EXPECT_EQ(hookCalls.load(), 0u);
    threads.ShutdownRenderingThreads();
    graph.Shutdown();
}

TEST_F(RenderCommandFailureTests, TickExceptionsStopBothThreadsAndReleaseQueuedFences)
{
    using namespace NLS::Render::Context;
    for (const bool failRhi : { false, true })
    {
        SCOPED_TRACE(failRhi);
        RenderCommandDispatcher::ResetForTesting();
        ResetRenderFatalErrorForTesting();
        class ThrowingTick final : public IRenderingThreadTick
        {
        public:
            explicit ThrowingTick(bool rhi) : failRhi(rhi) {}
            bool TickRenderThread() override
            {
                if (!failRhi)
                    throw std::runtime_error("injected render tick failure");
                return false;
            }
            bool TickRhiLane() override
            {
                if (failRhi)
                    throw std::runtime_error("injected RHI tick failure");
                return false;
            }
            bool failRhi;
        } tick(failRhi);

        NLS::Base::TaskGraph::TaskGraph graph({ 1u, false, true });
        ASSERT_TRUE(graph.Start());
        RenderCommandFence fence;
        fence.BeginFence(RenderCommandSyncDepth::Swapchain);
        RenderingThreadSystem threads;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.tick = &tick;
        params.rhiThreadMode = RhiThreadMode::DedicatedThread;
        ASSERT_TRUE(threads.InitRenderingThreads(params));
        EXPECT_TRUE(fence.WaitFor(1s));
        EXPECT_FALSE(fence.Succeeded());
        EXPECT_NE(fence.GetDiagnostic().find(failRhi ? "injected RHI tick failure" :
            "injected render tick failure"), std::string::npos);
        threads.ShutdownRenderingThreads();
        EXPECT_FALSE(threads.IsInitialized());
        EXPECT_FALSE(graph.IsNamedThreadRegistered(NLS::Base::TaskGraph::NamedThread::Render));
        EXPECT_FALSE(graph.IsNamedThreadRegistered(NLS::Base::TaskGraph::NamedThread::Rhi));
        graph.Shutdown();
    }
}

TEST_F(RenderCommandFailureTests, ShutdownCallbackExceptionsStillDetachTheNamedThreads)
{
    using namespace NLS::Render::Context;
    for (const bool failRhi : { false, true })
    {
        SCOPED_TRACE(failRhi);
        RenderCommandDispatcher::ResetForTesting();
        ResetRenderFatalErrorForTesting();
        class ThrowingShutdownTick final : public IRenderingThreadTick
        {
        public:
            explicit ThrowingShutdownTick(bool rhi) : failRhi(rhi) {}
            bool TickRenderThread() override { return false; }
            bool TickRhiLane() override { return false; }
            void OnRenderThreadStopping() override
            {
                if (!failRhi)
                    throw std::runtime_error("injected render shutdown failure");
            }
            void OnRhiLaneStopping() override
            {
                if (failRhi)
                    throw std::runtime_error("injected RHI shutdown failure");
            }
            bool failRhi;
        } tick(failRhi);
        NLS::Base::TaskGraph::TaskGraph graph({ 1u, false, true });
        ASSERT_TRUE(graph.Start());
        RenderingThreadSystem threads;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.tick = &tick;
        params.rhiThreadMode = RhiThreadMode::DedicatedThread;
        ASSERT_TRUE(threads.InitRenderingThreads(params));
        threads.ShutdownRenderingThreads();
        EXPECT_FALSE(threads.IsInitialized());
        EXPECT_TRUE(HasRenderFatalError());
        EXPECT_NE(GetRenderFatalError().find(failRhi ? "injected RHI shutdown failure" :
            "injected render shutdown failure"), std::string::npos);
        EXPECT_FALSE(graph.IsNamedThreadRegistered(NLS::Base::TaskGraph::NamedThread::Render));
        EXPECT_FALSE(graph.IsNamedThreadRegistered(NLS::Base::TaskGraph::NamedThread::Rhi));
        graph.Shutdown();
    }
}

TEST(RenderCommandPipeTests, CompletedAsyncDependencyIsNeverCountedAsAbandoned)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();

    std::atomic_bool bodyRan { false };
    const auto dependency = RenderCommandDispatcher::EnqueueAsync(
        [&bodyRan] { bodyRan.store(true, std::memory_order_release); });

    // Without a bound pool the async pipe runs inline, so the drain is what
    // actually executes the body and releases the dependency.
    (void)RenderCommandDispatcher::FlushToDrain();
    ASSERT_TRUE(dependency.IsComplete());

    RenderCommandDispatcher::MergeAsync(dependency, 50ms);
    EXPECT_EQ(RenderCommandDispatcher::Dispatch(), 1u);

    EXPECT_TRUE(bodyRan.load(std::memory_order_acquire));
    EXPECT_EQ(RenderCommandDispatcher::GetAbandonedAsyncMergeCount(), 0u);
}

TEST(DriverFlushTests, GameThreadDrainWaitsForRenderThreadSceneBuild)
{
    using namespace NLS::Render::Context;
    std::atomic_bool ranOnRenderThread { false };
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);

    RenderCommandEvent release;
    RenderCommandDispatcher::Enqueue([release] { (void)release.WaitFor(2s); });
    RenderFramePacket snapshot;
    snapshot.frameId = 92u;
    const bool published = DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
        [&](size_t)
        {
            ranOnRenderThread.store(RenderingThreadSystem::IsInRenderingThread());
            RenderScenePackage package;
            package.frameId = snapshot.frameId;
            return package;
        });
    std::thread unblock([release]
    {
        std::this_thread::sleep_for(30ms);
        release.Signal();
    });
    const bool drained = DriverRendererAccess::TryDrainThreadedRendering(driver, false);
    unblock.join();
    EXPECT_TRUE(published);
    EXPECT_TRUE(drained);
    EXPECT_TRUE(ranOnRenderThread.load());
    const auto* impl = DriverTestAccess::GetImplForTesting(driver);
    EXPECT_EQ(impl->pendingRenderFrameCommands.load(), 0u);
    EXPECT_EQ(Detail::GetPendingCpuFrameCount(*impl), 0u);
}

TEST(DriverFlushTests, StartupDrainBudgetWaitsPastTheRuntimeBudget)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    impl->renderDrainTimeout = 20ms;

    RenderCommandEvent entered;
    RenderCommandEvent release;
    EnqueueRenderCommand<struct HoldStartupSceneBuild>([entered, release]
    {
        entered.Signal();
        (void)release.WaitFor(2s);
    });
    RenderFramePacket packet;
    packet.frameId = 93u;
    const bool published = DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [](size_t) { return RenderScenePackage {}; });
    EXPECT_TRUE(entered.WaitFor(1s));
    std::thread unblock([release]
    {
        std::this_thread::sleep_for(60ms);
        release.Signal();
    });
    const bool drained = DriverRendererAccess::TryDrainThreadedRendering(driver, false, 500ms);
    unblock.join();

    EXPECT_TRUE(published);
    EXPECT_TRUE(drained);
    EXPECT_EQ(impl->renderDrainTimeout, 20ms);
    EXPECT_EQ(Detail::GetPendingCpuFrameCount(*impl), 0u);
}

TEST(DriverFlushTests, ExplicitDrainBudgetIsBoundedAndDoesNotChangeLaterDrains)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    impl->renderDrainTimeout = 20ms;

    RenderCommandEvent release;
    EnqueueRenderCommand<struct HoldBoundedStartupDrain>([release] { (void)release.WaitFor(2s); });
    RenderFramePacket packet;
    packet.frameId = 94u;
    const bool published = DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [](size_t) { return RenderScenePackage {}; });
    const bool explicitDrain = DriverRendererAccess::TryDrainThreadedRendering(driver, false, 10ms);
    const bool defaultDrain = DriverRendererAccess::TryDrainThreadedRendering(driver, false);
    release.Signal();
    const bool finalDrain = DriverRendererAccess::TryDrainThreadedRendering(driver, false, 500ms);

    EXPECT_TRUE(published);
    EXPECT_FALSE(explicitDrain);
    EXPECT_FALSE(defaultDrain);
    EXPECT_TRUE(finalDrain);
    EXPECT_EQ(impl->renderDrainTimeout, 20ms);
    EXPECT_EQ(Detail::GetPendingCpuFrameCount(*impl), 0u);
}

/**
 * An unavailable threaded render pipeline must be reported, not silently refused.
 *
 * The standalone inline frame path is gone, so a frame can only start through the
 * threaded pipeline. Returning a bare false put every caller into a retry loop that
 * surfaced as an endless "render-busy" with no cause named - which is what made the
 * GPU thumbnail preview look like a renderer bug instead of a configuration state.
 */
TEST(DriverFlushTests, BeginRendererFrameReportsAnUnavailableThreadedPipeline)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    auto* impl = DriverTestAccess::GetImplForTesting(driver);
    ASSERT_NE(impl, nullptr);

    // A live device is what makes the branch reachable: without one, BeginRendererFrame
    // is a no-op success.
    auto device = std::make_shared<NLS::Tests::DeterministicTextureRhiDevice>();
    DriverTestAccess::SetExplicitDevice(driver, device);
    driver.ShutdownThreadedRendering();

    EXPECT_FALSE(impl->reportedMissingThreadedPipeline);
    EXPECT_FALSE(RenderThreadCoordinator::BeginRendererFrame(driver));
    EXPECT_TRUE(impl->reportedMissingThreadedPipeline)
        << "the refusal must be recorded so the reason is reported to the log";

    // Idempotent: the condition is persistent, so the flag stays set and the frame is
    // still refused instead of the state being consumed.
    EXPECT_FALSE(RenderThreadCoordinator::BeginRendererFrame(driver));
    EXPECT_TRUE(impl->reportedMissingThreadedPipeline);
}
TEST(DriverFlushTests, MainPipeOrdersSceneUpdatesFramesAndRenderFence)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Driver driver(settings);

    RenderCommandEvent release;
    EnqueueRenderCommand<struct HoldRenderCommands>([release] { (void)release.WaitFor(2s); });
    int sceneRevision = 0;
    std::vector<int> renderedRevisions;
    std::atomic_bool everyBuildOnRenderThread { true };
    const auto publishFrame = [&](uint64_t frameId)
    {
        RenderFramePacket snapshot;
        snapshot.frameId = frameId;
        return DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
            [&](size_t)
            {
                if (!RenderingThreadSystem::IsInRenderingThread())
                    everyBuildOnRenderThread.store(false);
                renderedRevisions.push_back(sceneRevision);
                return RenderScenePackage {};
            });
    };
    EnqueueRenderCommand<struct FirstSceneUpdate>([&] { sceneRevision = 1; });
    const bool firstPublished = publishFrame(101u);
    EnqueueRenderCommand<struct SecondSceneUpdate>([&] { sceneRevision = 2; });
    const bool secondPublished = publishFrame(102u);
    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Render);
    release.Signal();
    const bool fenced = fence.WaitFor(3s);
    const bool drained = DriverRendererAccess::TryDrainThreadedRendering(driver, false);

    EXPECT_TRUE(firstPublished);
    EXPECT_TRUE(secondPublished);
    EXPECT_TRUE(fenced);
    EXPECT_TRUE(drained);
    EXPECT_TRUE(everyBuildOnRenderThread.load());
    EXPECT_EQ(renderedRevisions, (std::vector<int> { 1, 2 }));
}

TEST(DriverFlushTests, MainPipeOwnsFrameCapturesUntilTheirCommandExecutes)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);
    DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    auto capture = std::make_shared<int>(3);
    const std::weak_ptr<int> captureLifetime = capture;
    uint32_t builds = 0u;
    RenderFramePacket packet;
    packet.frameId = 103u;
    ASSERT_TRUE(DriverRendererAccess::TryEnqueueRenderFrame(driver, packet,
        [capture, &builds](const size_t)
        {
            ++builds;
            RenderScenePackage package;
            package.visibleDrawCount = static_cast<uint64_t>(*capture);
            return package;
        }));
    capture.reset();
    EXPECT_FALSE(captureLifetime.expired());
    EXPECT_EQ(builds, 0u);
    const auto* impl = DriverTestAccess::GetImplForTesting(driver);
    EXPECT_EQ(impl->pendingRenderFrameCommands.load(), 1u);
    EXPECT_TRUE(DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    EXPECT_EQ(builds, 1u);
    EXPECT_TRUE(captureLifetime.expired());
    EXPECT_EQ(impl->pendingRenderFrameCommands.load(), 0u);
    EXPECT_EQ(Detail::GetPendingCpuFrameCount(*impl), 0u);
}

TEST_F(RenderCommandFailureTests, FailedSceneBuildCancelsConsumersAndEveryFenceDepth)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;
    Driver driver(settings);

    RenderCommandEvent release;
    EnqueueRenderCommand<struct HoldBeforeFailedSceneBuild>([release] { (void)release.WaitFor(3s); });
    RenderFramePacket snapshot;
    snapshot.frameId = 202u;
    const auto readback = std::make_shared<PostSubmitTextureReadbackState>();
    PostSubmitTextureReadbackRequest request;
    request.state = readback;
    snapshot.postSubmitTextureReadbacks.push_back(request);
    const bool published = DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
        [](size_t) -> RenderScenePackage
        {
            throw std::runtime_error("injected scene preparation failure");
        });
    bool consumed = false;
    RenderCommandEvent consumer;
    EnqueueRenderCommand<struct ConsumeFailedScene>([&] { consumed = true; }, consumer);
    RenderCommandFence render, rhi, swapchain;
    render.BeginFence(RenderCommandSyncDepth::Render);
    rhi.BeginFence(RenderCommandSyncDepth::Rhi);
    swapchain.BeginFence(RenderCommandSyncDepth::Swapchain);
    release.Signal();

    EXPECT_TRUE(published);
    EXPECT_TRUE(consumer.WaitFor(3s));
    EXPECT_EQ(consumer.GetStatus(), RenderCommandStatus::Cancelled);
    EXPECT_FALSE(consumed);
    for (auto* fence : { &render, &rhi, &swapchain })
    {
        EXPECT_TRUE(fence->WaitFor(3s));
        EXPECT_FALSE(fence->Succeeded());
        EXPECT_NE(fence->GetDiagnostic().find("injected scene preparation failure"), std::string::npos);
    }
    const auto* impl = DriverTestAccess::GetImplForTesting(driver);
    const auto cleanupDeadline = std::chrono::steady_clock::now() + 3s;
    while (impl->pendingRenderFrameCommands.load() != 0u && std::chrono::steady_clock::now() < cleanupDeadline)
        std::this_thread::yield();
    EXPECT_EQ(impl->pendingRenderFrameCommands.load(), 0u);
    EXPECT_FALSE(DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
        [](size_t) { return RenderScenePackage {}; }));
    EXPECT_THROW(FlushRenderingCommands(), std::runtime_error);

    EXPECT_EQ(Detail::GetPendingCpuFrameCount(*impl), 0u);
    {
        std::lock_guard lock(impl->frameTelemetryMutex);
        EXPECT_EQ(impl->frameTelemetry.successfullyRetiredFrameCount, 0u);
        EXPECT_EQ(impl->frameTelemetry.failedRetiredFrameCount, 1u);
    }
    std::lock_guard lock(readback->mutex);
    EXPECT_TRUE(readback->beginAttempted);
    EXPECT_FALSE(readback->beginInProgress);
    EXPECT_FALSE(readback->beginSucceeded);
    EXPECT_EQ(readback->resultCode, NLS::Render::RHI::RHIReadbackStatusCode::BackendFailure);
    EXPECT_NE(readback->resultMessage.find("injected scene preparation failure"), std::string::npos);
}

TEST_F(RenderCommandFailureTests, FailedSceneBuildCancelsLaterPublishedFramesAndReadbacks)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 2u;
    Driver driver(settings);
    RenderCommandEvent release;
    EnqueueRenderCommand<struct HoldBeforeTwoFrames>([release] { (void)release.WaitFor(3s); });
    RenderFramePacket first;
    first.frameId = 203u;
    RenderFramePacket second;
    second.frameId = 204u;
    auto readback = std::make_shared<PostSubmitTextureReadbackState>();
    PostSubmitTextureReadbackRequest request;
    request.state = readback;
    second.postSubmitTextureReadbacks.push_back(request);
    const bool firstPublished = DriverRendererAccess::TryEnqueueRenderFrame(driver, first,
        [](size_t) -> RenderScenePackage { throw std::runtime_error("first frame failed"); });
    bool secondBuilt = false;
    auto secondCapture = std::make_shared<int>(1);
    const std::weak_ptr<int> secondCaptureLifetime = secondCapture;
    const bool secondPublished = DriverRendererAccess::TryEnqueueRenderFrame(driver, second,
        [&, secondCapture](size_t)
        {
            secondBuilt = *secondCapture != 0;
            return RenderScenePackage {};
        });
    const auto* impl = DriverTestAccess::GetImplForTesting(driver);
    EXPECT_EQ(impl->pendingRenderFrameCommands.load(), 2u);
    secondCapture.reset();
    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Rhi);
    release.Signal();

    EXPECT_TRUE(firstPublished);
    EXPECT_TRUE(secondPublished);
    EXPECT_TRUE(fence.WaitFor(3s));
    EXPECT_FALSE(fence.Succeeded());
    EXPECT_FALSE(secondBuilt);
    EXPECT_TRUE(WaitUntil([&] { return impl->pendingRenderFrameCommands.load() == 0u; }, 3s));
    EXPECT_EQ(Detail::GetPendingCpuFrameCount(*impl), 0u);
    EXPECT_TRUE(WaitUntil([&] { return secondCaptureLifetime.expired(); }, 3s));
    {
        std::lock_guard lock(impl->frameTelemetryMutex);
        EXPECT_EQ(impl->frameTelemetry.failedRetiredFrameCount, 2u);
        EXPECT_EQ(impl->frameTelemetry.successfullyRetiredFrameCount, 0u);
    }
    EXPECT_TRUE(DriverRendererAccess::CollectStreamingDependencyPins(driver).empty());
    std::lock_guard lock(readback->mutex);
    EXPECT_TRUE(readback->beginAttempted);
    EXPECT_FALSE(readback->beginSucceeded);
    EXPECT_NE(readback->resultMessage.find("first frame failed"), std::string::npos);
}

TEST(DriverFlushTests, DrainJoinsCommandsEvenWhenNoViewFrameWasPublished)
{
    using namespace NLS::Render::Context;
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Driver driver(settings);
    std::atomic_bool applied { false };
    std::atomic_bool onRenderThread { false };
    EnqueueRenderCommand<struct SceneUpdateWithoutView>([&]
    {
        onRenderThread.store(RenderingThreadSystem::IsInRenderingThread());
        applied.store(true);
    });
    EXPECT_TRUE(DriverRendererAccess::TryDrainThreadedRendering(driver, false));
    EXPECT_TRUE(applied.load());
    EXPECT_TRUE(onRenderThread.load());
}

TEST(DriverFlushTests, FlushRenderingCommandsRetiresThreadedFramesWithoutARenderThread)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1u;

    NLS::Render::Context::Driver driver(settings);
    // Workers are joined, so nothing is draining the serial pipe and nothing is
    // advancing the RHI stages: this is the state a flush has to cope with.
    NLS::Render::Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);


    NLS::Render::Context::RenderFramePacket snapshot;
    snapshot.frameId = 91u;
    ASSERT_TRUE(NLS::Render::Context::DriverRendererAccess::TryEnqueueRenderFrame(driver, snapshot,
        [snapshot](size_t) { return NLS::Render::Context::BuildRenderFramePackage(snapshot); }));
    const auto* impl = NLS::Render::Context::DriverTestAccess::GetImplForTesting(driver);
    ASSERT_EQ(impl->pendingRenderFrameCommands.load(), 1u);

    // Asking for the Rhi depth must actually retire the frame. Before the driver
    // installed its drain as the flush hook, this returned immediately while the
    // frame was still in flight - and with no render thread it could also have
    // waited forever for a marker nobody would run.
    NLS::Render::Context::FlushRenderingCommands(
        NLS::Render::Context::RhiFlushFlags::Submit);

    EXPECT_EQ(impl->pendingRenderFrameCommands.load(), 0u);
    EXPECT_EQ(NLS::Render::Context::Detail::GetPendingCpuFrameCount(*impl), 0u);
    std::lock_guard lock(impl->frameTelemetryMutex);
    EXPECT_EQ(impl->frameTelemetry.publishedFrameCount, 1u);
    EXPECT_EQ(impl->frameTelemetry.successfullyRetiredFrameCount + impl->frameTelemetry.failedRetiredFrameCount, 1u);
}

TEST(RenderCommandFenceTests, UnobservableDepthIsCountedInsteadOfSilentlyClaimed)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();
    RenderCommandFence::ResetUnobservedDepthCompletionCountForTesting();
    // Nothing in this test registers a sequence source, which is exactly the
    // production situation today: no executor publishes submission sequences.
    ASSERT_EQ(RenderCommandDispatcher::GetSequenceSource(), nullptr);

    RenderCommandFence renderDepth;
    renderDepth.BeginFence(RenderCommandSyncDepth::Render);
    // The marker is a command on the serial pipe, so something has to run it.
    (void)RenderCommandDispatcher::Dispatch();
    ASSERT_TRUE(renderDepth.WaitFor(2s));
    EXPECT_EQ(RenderCommandFence::GetUnobservedDepthCompletionCount(), 0u)
        << "The Render depth is observable without a sequence source.";

    RenderCommandFence rhiDepth;
    rhiDepth.BeginFence(RenderCommandSyncDepth::Rhi);
    (void)RenderCommandDispatcher::Dispatch();
    ASSERT_TRUE(rhiDepth.WaitFor(2s));

    // Completing is the right call - blocking forever would be worse, and callers
    // poll through IsFenced - but it must not look like a real depth wait.
    EXPECT_EQ(RenderCommandFence::GetUnobservedDepthCompletionCount(), 1u);

    RenderCommandFence swapchainDepth;
    swapchainDepth.BeginFence(RenderCommandSyncDepth::Swapchain);
    (void)RenderCommandDispatcher::Dispatch();
    ASSERT_TRUE(swapchainDepth.WaitFor(2s));
    EXPECT_EQ(RenderCommandFence::GetUnobservedDepthCompletionCount(), 2u);
}

TEST(RenderCommandFenceTests, FlushHookReceivesTheRequestedFlagsOnTheCallingThread)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();

    // The flush waits for its marker to run, and only a render thread drains the
    // serial pipe. Other suites leave their own drivers alive in this process, so
    // the test must provide the draining thread rather than depend on ambient
    // state to supply one.
    RenderCommandDispatcher::ResetForTesting();
    RhiFlushFlags observed = RhiFlushFlags::None;
    uint32_t hookCalls = 0u;
    std::thread::id hookThreadId {};
    SetRenderCommandFlushHook(
        [&observed, &hookCalls, &hookThreadId](const RhiFlushFlags flags)
        {
            observed = flags;
            ++hookCalls;
            hookThreadId = std::this_thread::get_id();
        });

    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());
    TestRenderTick tick;
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.tick = &tick;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::DedicatedThread;
    params.registerGameThread = true;
    ASSERT_TRUE(threads.InitRenderingThreads(params));

    const std::thread::id callerId = std::this_thread::get_id();
    FlushRenderingCommands(RhiFlushFlags::Submit | RhiFlushFlags::Translate);

    EXPECT_EQ(hookCalls, 1u);
    EXPECT_EQ(observed, RhiFlushFlags::Submit | RhiFlushFlags::Translate);
    EXPECT_EQ(hookThreadId, callerId);

    threads.ShutdownRenderingThreads();
    graph.Shutdown();
    SetRenderCommandFlushHook({});
}

TEST(RenderCommandFenceTests, FlushFromTheRenderThreadIsRejected)
{
    using namespace NLS::Render::Context;

    RenderCommandDispatcher::ResetForTesting();

    std::atomic<uint32_t> hookCalls { 0u };
    std::atomic<uint32_t> executed { 0u };
    SetRenderCommandFlushHook([&hookCalls](const RhiFlushFlags) { hookCalls.fetch_add(1u); });

    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());
    TestRenderTick tick;
    RenderingThreadSystem threads;
    RenderingThreadInitParams params;
    params.tick = &tick;
    params.taskGraph = &graph;
    params.rhiThreadMode = RhiThreadMode::DedicatedThread;
    params.registerGameThread = false;
    ASSERT_TRUE(threads.InitRenderingThreads(params));

    std::atomic_bool ranOnRenderThread { false };
    RenderCommandDispatcher::Enqueue(
        [&executed, &ranOnRenderThread]
        {
            ranOnRenderThread.store(
                RenderingThreadSystem::IsInRenderingThread(),
                std::memory_order_release);
            EXPECT_THROW(FlushRenderingCommands(RhiFlushFlags::Submit | RhiFlushFlags::Swapchain), std::logic_error);
            executed.fetch_add(1u);
        });

    ASSERT_TRUE(WaitUntil([&executed] { return executed.load() > 0u; }, 3s));
    EXPECT_TRUE(ranOnRenderThread.load(std::memory_order_acquire));
    EXPECT_EQ(hookCalls.load(), 0u)
        << "A rejected flush must not drive queue stages.";

    threads.ShutdownRenderingThreads();
    graph.Shutdown();
    SetRenderCommandFlushHook({});
}

TEST(RenderCommandFenceTests, DestroyingAFenceBeforeItsMarkerRunsIsSafe)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();

    // The fence is created and destroyed while the main pipe still holds its
    // marker command: the marker must not touch the destroyed fence.
    {
        RenderCommandFence fence;
        fence.BeginFence(RenderCommandSyncDepth::Rhi);
        EXPECT_FALSE(fence.IsFenced());
        EXPECT_EQ(RenderCommandDispatcher::GetMainQueueDepth(), 1u);
    }

    EXPECT_EQ(RenderCommandDispatcher::Dispatch(), 1u)
        << "The queued marker must still run after the fence is gone.";
}

TEST(RenderCommandFenceTests, RepeatedBeginFenceDoesNotReuseAnEarlierMarkerTarget)
{
    using namespace NLS::Render::Context;
    RenderCommandDispatcher::ResetForTesting();

    FakeSequenceSource source;
    RenderCommandDispatcher::SetSequenceSource(&source);

    RenderCommandFence fence;
    RenderCommandDispatcher::Enqueue([&source] { source.dispatched.store(1u); });
    fence.BeginFence(RenderCommandSyncDepth::Rhi);
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_EQ(fence.GetTargetSubmitSequence(), 1u);

    // A second BeginFence must start from a fresh target, not inherit the first
    // marker's captured sequence.
    RenderCommandDispatcher::Enqueue([&source] { source.dispatched.store(5u); });
    fence.BeginFence(RenderCommandSyncDepth::Rhi);
    (void)RenderCommandDispatcher::Dispatch();

    EXPECT_EQ(fence.GetTargetSubmitSequence(), 5u);
    source.submitted.store(1u);
    EXPECT_FALSE(fence.IsFenced())
        << "The re-armed fence must track the newer marker, not the older one.";
    source.submitted.store(5u);
    EXPECT_TRUE(fence.IsFenced());
}

namespace
{
    std::string ReadRepositorySource(const char* relativePath)
    {
        std::ifstream input(std::filesystem::path(NLS_ROOT_DIR) / relativePath, std::ios::binary);
        EXPECT_TRUE(input.is_open()) << "Failed to open " << relativePath;
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    /**
     * Extracts one C++ definition body. Declarations (which terminate with ';'
     * before any '{') are skipped so a forward declaration cannot be mistaken
     * for the definition.
     */
    std::string ExtractDefinitionBody(const std::string& source, const std::string_view signature)
    {
        size_t searchFrom = 0u;
        while (true)
        {
            const auto signatureOffset = source.find(signature, searchFrom);
            if (signatureOffset == std::string::npos)
                break;

            const auto afterSignature = signatureOffset + signature.size();
            const auto bodyStart = source.find('{', afterSignature);
            const auto declarationEnd = source.find(';', afterSignature);
            if (bodyStart != std::string::npos &&
                (declarationEnd == std::string::npos || bodyStart < declarationEnd))
            {
                size_t depth = 0u;
                for (size_t index = bodyStart; index < source.size(); ++index)
                {
                    if (source[index] == '{')
                    {
                        ++depth;
                    }
                    else if (source[index] == '}')
                    {
                        --depth;
                        if (depth == 0u)
                            return source.substr(bodyStart, index - bodyStart + 1u);
                    }
                }
            }
            searchFrom = afterSignature;
        }
        EXPECT_TRUE(false) << "Missing definition: " << signature;
        return {};
    }
}

/**
 * Frame overlap is a contract, not an implementation detail: dispatching frame
 * N+1 must not synchronously drain frame N. These guards keep a later cleanup
 * from quietly restoring the previous single-frame serialization.
 */
TEST(RenderingThreadPipelineTests, FrameDispatchDoesNotDrainThePreviousSubmission)
{
    const std::string coordinator = ReadRepositorySource(
        "Runtime/Rendering/Context/RhiCommandListExecutorFrame.cpp");
    const std::string driverInternal = ReadRepositorySource(
        "Runtime/Rendering/Context/DriverInternal.h");
    const std::string executorHeader = ReadRepositorySource(
        "Runtime/Rendering/Context/RhiCommandListExecutor.h");

    const std::string dispatchBody = ExtractDefinitionBody(
        coordinator, "bool RhiCommandListExecutor::DispatchRenderFrame(");
    ASSERT_FALSE(dispatchBody.empty());
    EXPECT_EQ(
        dispatchBody.find("DrainProducerOwnedThreadedSubmission"),
        std::string::npos)
        << "Frame dispatch must not wait for earlier frame submissions.";
    EXPECT_NE(dispatchBody.find("ScheduleClaimedRhiFrame"), std::string::npos);
    EXPECT_NE(
        dispatchBody.find("ScopedThreadedProductionGate productionGate"),
        std::string::npos)
        << "Frame dispatch must still serialize its claim/prepare/hand-off window.";

    const std::string finalizeBody = ExtractDefinitionBody(
        coordinator, "bool RhiCommandListExecutor::FinalizeThreadedSubmissionProduction(");
    ASSERT_FALSE(finalizeBody.empty());
    EXPECT_NE(
        finalizeBody.find("RetireCompletedThreadedSubmissions"),
        std::string::npos)
        << "Completed frames have to retire through the render-lane pump.";

    EXPECT_EQ(
        driverInternal.find("pendingThreadedRhiSubmissions"),
        std::string::npos);
    EXPECT_NE(
        executorHeader.find("std::deque<"),
        std::string::npos)
        << "The executor owns the ordered in-flight frames.";
    EXPECT_EQ(
        driverInternal.find("pendingThreadedRhiSubmission;"),
        std::string::npos)
        << "The single in-flight submission pointer must not come back.";
    EXPECT_EQ(
        driverInternal.find("threadedRhiProducerLane"),
        std::string::npos)
        << "The stored producer lane must not come back: producers own a local "
           "scoped gate and slot reuse is gated by the frame resource fence.";
}

/**
 * Swapchain acquisition must be placed on the ordered lane at frame begin and
 * waited for only where the frame actually consumes the backbuffer. Reverting to
 * a synchronous acquire in BeginThreadedRhiFrame would stall the render lane on
 * the previous frame's Present again.
 */
TEST(RenderingThreadPipelineTests, SwapchainAcquisitionIsEnqueuedEarlyAndBoundLate)
{
    const std::string coordinator = ReadRepositorySource(
        "Runtime/Rendering/Context/RhiCommandListExecutorFrame.cpp");
    const std::string executorHeader = ReadRepositorySource(
        "Runtime/Rendering/Context/RhiCommandListExecutor.h");

    EXPECT_NE(
        executorHeader.find("EnqueueOnSubmitLane"),
        std::string::npos)
        << "The unordered submit-lane enqueue API is what makes early acquisition possible.";
    EXPECT_NE(executorHeader.find("WaitForSubmitLaneOperation"), std::string::npos);

    const std::string beginBody = ExtractDefinitionBody(
        coordinator, "Detail::BeginThreadedRhiFrame(");
    ASSERT_FALSE(beginBody.empty());
    EXPECT_NE(beginBody.find("EnqueueOnSubmitLane"), std::string::npos)
        << "Frame begin must enqueue swapchain acquisition without blocking on the lane.";
    EXPECT_NE(beginBody.find("acquisition->enqueued = true"), std::string::npos);

    const std::string resolveBody = ExtractDefinitionBody(
        coordinator, "bool Detail::ResolveThreadedSwapchainAcquisition(");
    ASSERT_FALSE(resolveBody.empty());
    EXPECT_NE(resolveBody.find("WaitForSubmitLaneOperation"), std::string::npos);
    EXPECT_NE(resolveBody.find("acquisition.resolved = true"), std::string::npos)
        << "The lane operation must be consumed at most once per frame.";
    EXPECT_NE(
        resolveBody.find("SeedAcquiredSwapchainBackbufferState"),
        std::string::npos)
        << "Barrier seeding must follow the resolved acquisition, not precede it.";

    const std::string recordingBody = ExtractDefinitionBody(
        coordinator, "void RecordThreadedRhiSubmissionWork(");
    ASSERT_FALSE(recordingBody.empty());
    EXPECT_NE(
        recordingBody.find("Detail::ResolveThreadedSwapchainAcquisition"),
        std::string::npos)
        << "Recording must bind the backbuffer lazily, once a pass needs it.";
    EXPECT_NE(
        recordingBody.find("ensureSwapchainAcquisition"),
        std::string::npos)
        << "The serial recording path must resolve the acquisition lazily at the "
           "first work unit that consumes the backbuffer.";

    const std::string parallelRecordingBody = ExtractDefinitionBody(
        coordinator, "RecordParallelCommandWorkUnits(");
    ASSERT_FALSE(parallelRecordingBody.empty());
    EXPECT_NE(
        parallelRecordingBody.find("FindFirstSwapchainDependentWorkUnitPosition"),
        std::string::npos)
        << "Parallel recording must split at the first swapchain-dependent work unit.";
    const auto viewIndependentRecord = parallelRecordingBody.find(
        "for (size_t orderedIndex = 0u; orderedIndex < swapchainBatchBegin;");
    const auto acquisitionResolve = parallelRecordingBody.find(
        "Detail::ResolveThreadedSwapchainAcquisition");
    ASSERT_NE(viewIndependentRecord, std::string::npos);
    ASSERT_NE(acquisitionResolve, std::string::npos);
    EXPECT_LT(viewIndependentRecord, acquisitionResolve)
        << "View-independent work units must record before the frame waits for "
           "the backbuffer.";

    const std::string orderedChildBody = ExtractDefinitionBody(
        coordinator, "bool RecordOrderedInRenderPassChildAndSerialWorkUnits(");
    ASSERT_FALSE(orderedChildBody.empty());
    EXPECT_NE(orderedChildBody.find("ensureSwapchainAcquisition"), std::string::npos)
        << "The ordered child/serial path must resolve the acquisition lazily too.";
    EXPECT_NE(orderedChildBody.find("childGroupTargetsSwapchain"), std::string::npos)
        << "A child command group that targets the swapchain must resolve the "
           "acquisition before opening the parent pass.";

    const std::string abortBody = ExtractDefinitionBody(
        coordinator, "void AbortThreadedRhiSubmission(");
    ASSERT_FALSE(abortBody.empty());
    EXPECT_NE(
        abortBody.find("Detail::ResolveThreadedSwapchainAcquisition"),
        std::string::npos)
        << "An aborted frame must still consume its enqueued acquisition.";
}

/**
 * The split boundary is what lets view-independent recording run while the
 * previous frame is still being submitted: work units before the first
 * backbuffer consumer record without the acquired image, and everything from
 * that position stays behind the wait in submission order.
 */
TEST(RenderingThreadPipelineTests, SwapchainDependentWorkUnitsSplitAtTheirSubmissionOrderBoundary)
{
    using namespace NLS::Render::Context;

    std::vector<ParallelCommandWorkUnit> workUnits(4u);
    workUnits[0].submissionOrder = 10u;
    workUnits[1].submissionOrder = 20u;
    workUnits[2].submissionOrder = 30u;
    workUnits[3].submissionOrder = 40u;
    workUnits[1].commandInput.targetsSwapchain = true;
    workUnits[3].commandInput.targetsSwapchain = true;

    const std::vector<size_t> orderedIndices { 0u, 1u, 2u, 3u };
    EXPECT_EQ(
        Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, orderedIndices),
        1u)
        << "The wait belongs at the first swapchain consumer, not before the frame.";

    workUnits[1].commandInput.targetsSwapchain = false;
    workUnits[3].commandInput.targetsSwapchain = false;
    EXPECT_EQ(
        Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, orderedIndices),
        workUnits.size())
        << "An offscreen frame records every work unit before any image wait.";

    workUnits[0].commandInput.targetsSwapchain = true;
    EXPECT_EQ(
        Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, orderedIndices),
        0u)
        << "A leading swapchain consumer must stop the view-independent phase.";

    workUnits[0].commandInput.targetsSwapchain = false;
    const std::vector<size_t> staleIndices { 0u, 7u };
    EXPECT_EQ(
        Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, staleIndices),
        1u)
        << "An unusable index conservatively closes the view-independent phase.";

    workUnits[3].commandInput.targetsSwapchain = true;
    const std::vector<size_t> reorderedIndices { 3u, 0u, 1u, 2u };
    EXPECT_EQ(
        Detail::FindFirstSwapchainDependentWorkUnitPosition(workUnits, reorderedIndices),
        0u)
        << "The boundary follows the supplied recording order, not the index order.";
}
