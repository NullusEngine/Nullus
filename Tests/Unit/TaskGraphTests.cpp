#include <gtest/gtest.h>

#include "TaskGraph/TaskGraph.h"
#include "Threading/ExecutionThreadBudget.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

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
TEST(TaskGraphTests, CompletionWaitForTimesOutOnPendingNamedThreadTask)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    const auto handle = graph.Submit([] {}, NLS::Base::TaskGraph::NamedThread::Render);
    ASSERT_TRUE(handle.IsValid());
    auto completion = graph.GetCompletion(handle);
    ASSERT_TRUE(completion.IsValid());
    EXPECT_FALSE(completion.WaitFor(10ms));
    EXPECT_FALSE(completion.IsComplete());
    EXPECT_EQ(graph.GetNamedThreadQueueDepth(NLS::Base::TaskGraph::NamedThread::Render), 1u);

    graph.Pump(NLS::Base::TaskGraph::NamedThread::Render);
    EXPECT_TRUE(completion.IsComplete());
    EXPECT_EQ(completion.GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);

    graph.Shutdown();
}

TEST(TaskGraphTests, TerminalRecordsAreRecycledPastTheRetentionBound)
{
    NLS::Base::TaskGraph::TaskGraphConfig config;
    config.workerCount = 2u;
    config.retainedTerminalTaskCount = 8u;
    NLS::Base::TaskGraph::TaskGraph graph(config);
    ASSERT_TRUE(graph.Start());

    std::vector<NLS::Base::TaskGraph::TaskHandle> handles;
    std::vector<NLS::Base::TaskGraph::TaskCompletion> completions;
    handles.reserve(64u);
    completions.reserve(64u);
    for (size_t index = 0u; index < 64u; ++index)
    {
        const auto handle = graph.Submit(
            [] {},
            NLS::Base::TaskGraph::NamedThread::Any,
            NLS::Base::TaskGraph::TaskPriority::Normal,
            "retention-probe");
        ASSERT_TRUE(handle.IsValid());
        handles.push_back(handle);
        completions.push_back(graph.GetCompletion(handle));
    }

    for (auto& completion : completions)
        EXPECT_EQ(completion.Wait(), NLS::Base::TaskGraph::TaskStatus::Succeeded);

    // Per-frame RHI graphs must not grow the task table without limit.
    EXPECT_LE(graph.GetRetainedTaskCount(), config.retainedTerminalTaskCount);
    EXPECT_EQ(graph.GetPendingTaskCount(), 0u);

    // The oldest records are gone, so their handles no longer resolve.
    EXPECT_FALSE(graph.GetCompletion(handles.front()).IsValid());

    // Completion tokens copied before recycling stay observable.
    for (const auto& completion : completions)
    {
        EXPECT_TRUE(completion.IsComplete());
        EXPECT_EQ(completion.GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    }

    graph.Shutdown();
}

TEST(TaskGraphTests, NestedSubmissionFromWorkerCompletesWithoutDeadlock)
{
    NLS::Base::TaskGraph::TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());

    std::atomic<uint32_t> observedValue {0u};
    const auto outer = graph.Submit([&graph, &observedValue]
    {
        NLS::Base::TaskGraph::TaskDesc nested;
        nested.debugName = "NestedTask";
        nested.function = [&observedValue] { observedValue.fetch_add(2u, std::memory_order_acq_rel); };
        const auto nestedHandle = graph.Submit(std::move(nested));
        if (!nestedHandle.IsValid())
            return;

        graph.Wait(nestedHandle);
        observedValue.fetch_add(1u, std::memory_order_acq_rel);
    });
    ASSERT_TRUE(outer.IsValid());

    EXPECT_EQ(graph.Wait(outer), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_EQ(observedValue.load(std::memory_order_acquire), 3u);

    graph.Shutdown();
}

TEST(TaskGraphTests, RestartAfterShutdownRejectsStaleHandlesAndKeepsIdsUnique)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    const auto stale = graph.Submit([] {}, NLS::Base::TaskGraph::NamedThread::Rhi);
    ASSERT_TRUE(stale.IsValid());
    graph.Shutdown(NLS::Base::TaskGraph::TaskGraphShutdownMode::Immediate);
    EXPECT_FALSE(graph.GetCompletion(stale).IsValid());

    // The graph stays reusable after a drained/immediate shutdown.
    ASSERT_TRUE(graph.Start());
    const auto fresh = graph.Submit([] {}, NLS::Base::TaskGraph::NamedThread::Any);
    ASSERT_TRUE(fresh.IsValid());
    EXPECT_EQ(graph.Wait(fresh), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_FALSE(graph.GetCompletion(stale).IsValid());
    EXPECT_NE(fresh.id, stale.id);
    EXPECT_NE(fresh.generation, stale.generation);

    graph.Shutdown();
}
}

TEST(TaskGraphTests, TaskEventTriggerResetAndGenerationDoNotLoseWakeups)
{
    NLS::Base::TaskGraph::TaskEvent event;
    EXPECT_FALSE(event.IsTriggered());
    EXPECT_EQ(event.Generation(), 0u);

    const uint64_t firstGeneration = event.Trigger();
    EXPECT_TRUE(event.IsTriggered());
    EXPECT_EQ(firstGeneration, 1u);
    EXPECT_TRUE(event.WaitFor(10ms));

    event.Reset();
    EXPECT_FALSE(event.IsTriggered());

    std::atomic_bool observed = false;
    std::thread waiter([&event, firstGeneration, &observed]
    {
        observed.store(event.WaitForGeneration(firstGeneration, 1s), std::memory_order_release);
    });
    std::this_thread::sleep_for(5ms);
    const uint64_t secondGeneration = event.Trigger();
    waiter.join();

    EXPECT_EQ(secondGeneration, 2u);
    EXPECT_TRUE(observed.load(std::memory_order_acquire));
}

TEST(TaskGraphTests, WorkerExecutesDependencyChainInOrder)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    std::atomic<uint32_t> stage {0u};
    std::atomic<uint32_t> observedOrder {0u};
    const auto first = graph.Submit([&stage, &observedOrder]
    {
        EXPECT_EQ(stage.load(std::memory_order_acquire), 0u);
        observedOrder.fetch_add(1u, std::memory_order_acq_rel);
        stage.store(1u, std::memory_order_release);
    });
    ASSERT_TRUE(first.IsValid());

    NLS::Base::TaskGraph::TaskDesc secondDesc;
    secondDesc.prerequisites.push_back(first);
    secondDesc.function = [&stage, &observedOrder]
    {
        EXPECT_EQ(stage.load(std::memory_order_acquire), 1u);
        observedOrder.fetch_add(1u, std::memory_order_acq_rel);
        stage.store(2u, std::memory_order_release);
    };
    const auto second = graph.Submit(std::move(secondDesc));
    ASSERT_TRUE(second.IsValid());

    EXPECT_EQ(graph.Wait(second), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_EQ(stage.load(std::memory_order_acquire), 2u);
    EXPECT_EQ(observedOrder.load(std::memory_order_acquire), 2u);
    graph.Shutdown();
}

TEST(TaskGraphTests, CompletedSuccessfulDependenciesDoNotCancelLateDependents)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    const auto first = graph.Submit([] {});
    const auto second = graph.Submit([] {});
    ASSERT_EQ(graph.Wait(first), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    ASSERT_EQ(graph.Wait(second), NLS::Base::TaskGraph::TaskStatus::Succeeded);

    std::atomic_bool ran {false};
    NLS::Base::TaskGraph::TaskDesc dependent;
    dependent.prerequisites = {first, second};
    dependent.function = [&ran] { ran.store(true); };
    const auto handle = graph.Submit(std::move(dependent));
    ASSERT_TRUE(handle.IsValid());
    EXPECT_TRUE(graph.WaitFor(handle, 1s));
    EXPECT_EQ(graph.GetCompletion(handle).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_TRUE(ran.load());
    graph.Shutdown();
}

TEST(TaskGraphTests, LateDependentWaitsOnlyForItsPendingDependencies)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    const auto completed = graph.Submit([] {});
    ASSERT_EQ(graph.Wait(completed), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    const auto pending = graph.Submit([] {}, NLS::Base::TaskGraph::NamedThread::Render);
    ASSERT_TRUE(pending.IsValid());

    std::atomic_bool ran {false};
    NLS::Base::TaskGraph::TaskDesc dependent;
    dependent.prerequisites = {completed, pending};
    dependent.function = [&ran] { ran.store(true); };
    const auto handle = graph.Submit(std::move(dependent));
    ASSERT_TRUE(handle.IsValid());
    EXPECT_FALSE(graph.WaitFor(handle, 10ms));
    EXPECT_FALSE(ran.load());
    EXPECT_EQ(graph.Pump(NLS::Base::TaskGraph::NamedThread::Render), 1u);
    EXPECT_TRUE(graph.WaitFor(handle, 1s));
    EXPECT_EQ(graph.GetCompletion(handle).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_TRUE(ran.load());
    graph.Shutdown();
}

TEST(TaskGraphTests, CompletedFailedOrCancelledDependenciesCancelLateDependents)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    const auto completed = graph.Submit([] {});
    ASSERT_EQ(graph.Wait(completed), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    const auto failed = graph.Submit([] { throw std::runtime_error("expected failure"); });
    ASSERT_EQ(graph.Wait(failed), NLS::Base::TaskGraph::TaskStatus::Failed);
    const auto cancelled = graph.Submit([] {}, NLS::Base::TaskGraph::NamedThread::Render);
    ASSERT_TRUE(graph.RequestCancel(cancelled));
    ASSERT_EQ(graph.Wait(cancelled), NLS::Base::TaskGraph::TaskStatus::Cancelled);

    for (const auto unsuccessful : {failed, cancelled})
    {
        bool ran = false;
        NLS::Base::TaskGraph::TaskDesc dependent;
        dependent.prerequisites = {completed, unsuccessful};
        dependent.function = [&ran] { ran = true; };
        const auto handle = graph.Submit(std::move(dependent));
        ASSERT_TRUE(handle.IsValid());
        EXPECT_EQ(graph.Wait(handle), NLS::Base::TaskGraph::TaskStatus::Cancelled);
        EXPECT_FALSE(ran);
    }
    graph.Shutdown();
}

TEST(TaskGraphTests, NamedThreadTasksOnlyRunWhenTheirThreadPumps)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());
    ASSERT_TRUE(graph.RegisterCurrentThread(NLS::Base::TaskGraph::NamedThread::Game));

    std::atomic_bool ran = false;
    const auto handle = graph.Submit(
        [&ran] { ran.store(true, std::memory_order_release); },
        NLS::Base::TaskGraph::NamedThread::Game,
        NLS::Base::TaskGraph::TaskPriority::Normal,
        "GameThreadOnly");
    ASSERT_TRUE(handle.IsValid());

    std::this_thread::sleep_for(10ms);
    EXPECT_FALSE(ran.load(std::memory_order_acquire));
    EXPECT_EQ(graph.GetNamedThreadQueueDepth(NLS::Base::TaskGraph::NamedThread::Game), 1u);

    EXPECT_EQ(graph.PumpCurrentThread(), 1u);
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_EQ(graph.Wait(handle), NLS::Base::TaskGraph::TaskStatus::Succeeded);

    graph.UnregisterCurrentThread(NLS::Base::TaskGraph::NamedThread::Game);
    graph.Shutdown();
}

TEST(TaskGraphTests, WaitingOnNamedThreadTaskPumpsTheCurrentThreadQueue)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());
    ASSERT_TRUE(graph.RegisterCurrentThread(NLS::Base::TaskGraph::NamedThread::Game));

    std::atomic_bool ran = false;
    const auto handle = graph.Submit(
        [&ran] { ran.store(true, std::memory_order_release); },
        NLS::Base::TaskGraph::NamedThread::Game);
    ASSERT_TRUE(handle.IsValid());

    EXPECT_EQ(graph.Wait(handle), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_TRUE(ran.load(std::memory_order_acquire));

    graph.UnregisterCurrentThread(NLS::Base::TaskGraph::NamedThread::Game);
    graph.Shutdown();
}

TEST(TaskGraphTests, CompletionCopiesObserveTheSameTerminalState)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    const auto handle = graph.Submit([] {});
    ASSERT_TRUE(handle.IsValid());
    auto first = graph.GetCompletion(handle);
    auto second = first;
    ASSERT_TRUE(first.IsValid());
    ASSERT_TRUE(second.IsValid());

    EXPECT_EQ(first.Wait(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_TRUE(second.IsComplete());
    EXPECT_EQ(second.GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    graph.Shutdown();
}

TEST(TaskGraphTests, InvalidAndSelfDependenciesAreRejected)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    NLS::Base::TaskGraph::TaskDesc invalidDesc;
    invalidDesc.function = [] {};
    invalidDesc.prerequisites.push_back({1u, 1u});
    EXPECT_FALSE(graph.Submit(std::move(invalidDesc)).IsValid());

    std::atomic_bool firstRan = false;
    const auto first = graph.Submit([&firstRan] { firstRan.store(true); });
    ASSERT_TRUE(first.IsValid());

    NLS::Base::TaskGraph::TaskDesc duplicateDesc;
    duplicateDesc.function = [] {};
    duplicateDesc.prerequisites = {first, first};
    EXPECT_FALSE(graph.Submit(std::move(duplicateDesc)).IsValid());
    EXPECT_EQ(graph.Wait(first), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    graph.Shutdown();
}

TEST(TaskGraphTests, FailedDependencyCancelsDependents)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    const auto failed = graph.Submit([] { throw std::runtime_error("expected failure"); });
    ASSERT_TRUE(failed.IsValid());

    std::atomic_bool dependentRan = false;
    NLS::Base::TaskGraph::TaskDesc dependentDesc;
    dependentDesc.function = [&dependentRan] { dependentRan.store(true); };
    dependentDesc.prerequisites.push_back(failed);
    const auto dependent = graph.Submit(std::move(dependentDesc));
    ASSERT_TRUE(dependent.IsValid());

    EXPECT_EQ(graph.Wait(failed), NLS::Base::TaskGraph::TaskStatus::Failed);
    EXPECT_EQ(graph.Wait(dependent), NLS::Base::TaskGraph::TaskStatus::Cancelled);
    EXPECT_FALSE(dependentRan.load(std::memory_order_acquire));
    graph.Shutdown();
}

TEST(TaskGraphTests, WorkerCannotExecuteNamedThreadTasks)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    std::atomic_bool ran = false;
    const auto handle = graph.Submit([&ran] { ran.store(true); }, NLS::Base::TaskGraph::NamedThread::Render);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_FALSE(graph.WaitFor(handle, 15ms));
    EXPECT_FALSE(ran.load(std::memory_order_acquire));

    graph.Shutdown(NLS::Base::TaskGraph::TaskGraphShutdownMode::Immediate);
    EXPECT_EQ(graph.GetCompletion(handle).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Cancelled);
}

TEST(TaskGraphTests, ImmediateShutdownCancelsPendingNamedAndWorkerTasks)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    const auto workerTask = graph.Submit([] { std::this_thread::sleep_for(20ms); });
    const auto namedTask = graph.Submit([] {}, NLS::Base::TaskGraph::NamedThread::Rhi);
    ASSERT_TRUE(workerTask.IsValid());
    ASSERT_TRUE(namedTask.IsValid());

    graph.Shutdown(NLS::Base::TaskGraph::TaskGraphShutdownMode::Immediate);
    EXPECT_EQ(graph.GetPendingTaskCount(), 0u);
    EXPECT_EQ(graph.GetRunningTaskCount(), 0u);
}

TEST(TaskGraphTests, InlineExecutionCompletesDuringSubmit)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, true, true});
    std::atomic_bool ran = false;
    const auto handle = graph.Submit([&ran] { ran.store(true, std::memory_order_release); });
    ASSERT_TRUE(handle.IsValid());
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_EQ(graph.GetCompletion(handle).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
}

TEST(TaskGraphTests, ExplicitWorkerCountIsHonored)
{
    NLS::Base::TaskGraph::TaskGraph graph({3u, false, true});
    ASSERT_TRUE(graph.Start());
    EXPECT_EQ(graph.GetWorkerCount(), 3u);
    graph.Shutdown();
}

TEST(TaskGraphTests, AutoWorkerCountUsesTheSharedExecutionPoolBudget)
{
    NLS::Base::TaskGraph::TaskGraph graph;
    ASSERT_TRUE(graph.Start());

    const uint32_t hardware = (std::max)(1u, std::thread::hardware_concurrency());
    const auto expected =
        NLS::Base::Threading::ResolveExecutionPoolBudget(hardware).taskGraphWorkerCount;
    EXPECT_EQ(graph.GetWorkerCount(), expected);
    if (hardware > NLS::Base::Threading::kReservedNamedExecutionLaneCount)
    {
        EXPECT_LT(graph.GetWorkerCount(), hardware)
            << "The rendering TaskGraph must leave lanes for the game, render, and RHI threads.";
    }
    graph.Shutdown();
}

TEST(TaskGraphTests, SingleWorkerNestedWaitCompletesThroughAssist)
{
    // A one-worker pool can only satisfy a nested wait if the blocked worker runs
    // runnable work itself instead of parking on the completion.
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    std::atomic<uint32_t> observedValue {0u};
    const auto outer = graph.Submit([&graph, &observedValue]
    {
        NLS::Base::TaskGraph::TaskDesc nested;
        nested.debugName = "NestedOnSingleWorker";
        nested.function = [&observedValue]
        {
            observedValue.fetch_add(4u, std::memory_order_acq_rel);
        };
        const auto nestedHandle = graph.Submit(std::move(nested));
        if (!nestedHandle.IsValid())
            return;

        graph.Wait(nestedHandle);
        observedValue.fetch_add(1u, std::memory_order_acq_rel);
    });
    ASSERT_TRUE(outer.IsValid());

    EXPECT_EQ(graph.Wait(outer), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_EQ(observedValue.load(std::memory_order_acquire), 5u);
    graph.Shutdown();
}

TEST(TaskGraphTests, NestedSubmissionsAreExecutedByThePoolWithoutExternalPumping)
{
    // Nested work must be visible to every lane: the submitting worker may be busy
    // and an idle peer has to steal it.
    NLS::Base::TaskGraph::TaskGraph graph({4u, false, true});
    ASSERT_TRUE(graph.Start());

    std::atomic<uint32_t> completedChildren {0u};
    std::vector<NLS::Base::TaskGraph::TaskHandle> roots;
    for (uint32_t parent = 0u; parent < 16u; ++parent)
    {
        roots.push_back(graph.Submit([&graph, &completedChildren]
        {
            for (uint32_t child = 0u; child < 8u; ++child)
            {
                const auto childHandle = graph.Submit([&completedChildren]
                {
                    completedChildren.fetch_add(1u, std::memory_order_acq_rel);
                });
                if (childHandle.IsValid())
                    graph.Wait(childHandle);
            }
        }));
    }

    for (const auto handle : roots)
        EXPECT_EQ(graph.Wait(handle), NLS::Base::TaskGraph::TaskStatus::Succeeded);

    EXPECT_EQ(completedChildren.load(std::memory_order_acquire), 128u);

    // Both worker paths ran: the pool popped nested work from the submitting lane
    // and consumed externally submitted work from the injection queue. Whether a
    // peer *stole* anything is a scheduling coincidence here - a worker that is
    // inside its parent pops its own lane first and can drain it before an idle
    // peer looks - so the steal path is pinned by
    // `IdleWorkerStealsChildrenQueuedByAParkedPeer`, which makes a steal the only
    // way the work can complete.
    EXPECT_GT(graph.GetWorkerLocalExecutionCount(), 0u);
    EXPECT_GT(graph.GetWorkerInjectionExecutionCount(), 0u);

    graph.Shutdown();
}

TEST(TaskGraphTests, IdleWorkerStealsChildrenQueuedByAParkedPeer)
{
    // Two workers, and the parent parks without nesting a wait after publishing its
    // children. Nested submissions stay on the submitting worker's lane, so those
    // children can only complete if the idle peer steals that lane: the steal path is
    // a required part of the contract here instead of a timing coincidence.
    NLS::Base::TaskGraph::TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());

    constexpr uint32_t kChildCount = 8u;
    std::atomic<uint32_t> completedChildren {0u};
    std::atomic_bool childrenQueued {false};
    std::atomic_bool releaseParent {false};

    const auto parent = graph.Submit(
        [&graph, &completedChildren, &childrenQueued, &releaseParent]
        {
            for (uint32_t child = 0u; child < kChildCount; ++child)
            {
                (void)graph.Submit([&completedChildren]
                {
                    completedChildren.fetch_add(1u, std::memory_order_acq_rel);
                });
            }
            childrenQueued.store(true, std::memory_order_release);

            // Deliberately not graph.Wait: a nested wait assists, which would let
            // this worker run its own children. That assist path is covered by
            // `SingleWorkerNestedWaitCompletesThroughAssist`.
            while (!releaseParent.load(std::memory_order_acquire))
                std::this_thread::yield();
        });
    ASSERT_TRUE(parent.IsValid());

    ASSERT_TRUE(WaitUntil(
        [&childrenQueued] { return childrenQueued.load(std::memory_order_acquire); },
        2s));
    const bool drainedByPeer = WaitUntil(
        [&completedChildren] { return completedChildren.load(std::memory_order_acquire) == kChildCount; },
        2s);

    releaseParent.store(true, std::memory_order_release);
    EXPECT_EQ(graph.Wait(parent), NLS::Base::TaskGraph::TaskStatus::Succeeded);

    EXPECT_TRUE(drainedByPeer)
        << "the idle worker never drained the parked worker's lane";
    EXPECT_EQ(completedChildren.load(std::memory_order_acquire), kChildCount);
    EXPECT_GT(graph.GetWorkerStealCount(), 0u)
        << "the parked worker's lane was drained without a steal";
    graph.Shutdown();
}

TEST(TaskGraphTests, RequestCancelCancelsQueuedTaskAndItsDependents)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    // Hold the single worker so the cancelled task cannot be claimed first.
    std::atomic_bool releaseWorker {false};
    const auto blocker = graph.Submit([&releaseWorker]
    {
        while (!releaseWorker.load(std::memory_order_acquire))
            std::this_thread::sleep_for(1ms);
    });
    ASSERT_TRUE(blocker.IsValid());

    std::atomic_bool cancelledBodyRan {false};
    const auto victim = graph.Submit([&cancelledBodyRan]
    {
        cancelledBodyRan.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(victim.IsValid());

    NLS::Base::TaskGraph::TaskDesc dependentDesc;
    dependentDesc.function = [] {};
    dependentDesc.prerequisites.push_back(victim);
    const auto dependent = graph.Submit(std::move(dependentDesc));
    ASSERT_TRUE(dependent.IsValid());

    ASSERT_TRUE(graph.RequestCancel(victim));
    EXPECT_TRUE(graph.IsCancellationRequested(victim));
    EXPECT_EQ(graph.GetCompletion(victim).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Cancelled);
    EXPECT_EQ(graph.GetCompletion(dependent).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Cancelled);

    releaseWorker.store(true, std::memory_order_release);
    EXPECT_EQ(graph.Wait(blocker), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_FALSE(cancelledBodyRan.load(std::memory_order_acquire));

    graph.Shutdown();
}

TEST(TaskGraphTests, RequestCancelOnRunningTaskIsCooperative)
{
    NLS::Base::TaskGraph::TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());

    std::atomic_bool bodyEntered {false};
    std::atomic_bool releaseBody {false};
    const auto running = graph.Submit([&bodyEntered, &releaseBody]
    {
        bodyEntered.store(true, std::memory_order_release);
        while (!releaseBody.load(std::memory_order_acquire))
            std::this_thread::sleep_for(1ms);
    });
    ASSERT_TRUE(running.IsValid());
    ASSERT_TRUE(WaitUntil([&bodyEntered] { return bodyEntered.load(std::memory_order_acquire); }, 2s));

    // The body cannot be preempted, so the request is recorded and the body's own
    // result is the terminal status.
    EXPECT_TRUE(graph.RequestCancel(running));
    EXPECT_TRUE(graph.IsCancellationRequested(running));
    EXPECT_EQ(graph.GetCompletion(running).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Pending);

    releaseBody.store(true, std::memory_order_release);
    EXPECT_EQ(graph.Wait(running), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    graph.Shutdown();
}

TEST(TaskGraphTests, RequestCancelRejectsUnknownStaleAndTerminalHandles)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());

    EXPECT_FALSE(graph.RequestCancel({}));
    EXPECT_FALSE(graph.RequestCancel({9999u, 4242u}));

    const auto finished = graph.Submit([] {});
    ASSERT_TRUE(finished.IsValid());
    EXPECT_EQ(graph.Wait(finished), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_FALSE(graph.RequestCancel(finished));

    graph.Shutdown();
}
