#include <gtest/gtest.h>

#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobSystem.h"
#include "TaskGraph/TaskGraph.h"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace Jobs = NLS::Base::Jobs;
namespace TG = NLS::Base::TaskGraph;

namespace
{
    TG::TaskDesc Task(std::function<void()> body, TG::NamedThread lane = TG::NamedThread::Any)
    {
        TG::TaskDesc task;
        task.function = std::move(body);
        task.targetThread = lane;
        return task;
    }

    bool Until(const std::function<bool()>& predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!predicate() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        return predicate();
    }
}

TEST(UnifiedTaskSystemTests, MultipleGraphsBorrowTheExplicitJobsPool)
{
    ASSERT_TRUE(Jobs::TryInitializeJobSystem({1u, 1u, false}));
    {
        TG::TaskGraph first({3u}), second({4u});
        ASSERT_TRUE(first.Start());
        ASSERT_TRUE(second.Start());
        EXPECT_EQ(first.GetWorkerCount(), 1u);
        EXPECT_EQ(second.GetWorkerCount(), 1u);
        EXPECT_EQ(Jobs::GetJobWorkerCount(), 1u);
        auto task = first.SubmitTracked(Task([&]
        {
            EXPECT_TRUE(Jobs::Internal::IsForegroundWorkerThread());
            auto nested = second.SubmitTracked(Task([] {}));
            EXPECT_EQ(nested.completion.Wait(), TG::TaskStatus::Succeeded);
        }));
        EXPECT_TRUE(task.completion.WaitFor(3s));
        first.Shutdown();
        EXPECT_TRUE(Jobs::IsJobSystemInitialized());
        EXPECT_TRUE(second.SubmitTracked(Task([] {})).completion.WaitFor(3s));
    }
    EXPECT_TRUE(Jobs::IsJobSystemInitialized());
    Jobs::ShutdownJobSystem();
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST(UnifiedTaskSystemTests, JobsToGraphToJobsWaitsUseTheSameSingleWorker)
{
    TG::TaskGraph graph({1u});
    ASSERT_TRUE(graph.Start());
    struct Payload { TG::TaskGraph* graph; std::atomic_int value {0}; } payload {&graph};
    Jobs::JobScheduleDesc outer;
    outer.userData = &payload;
    outer.function = [](void* data)
    {
        auto& payload = *static_cast<Payload*>(data);
        auto graphTask = payload.graph->SubmitTracked(Task([&]
        {
            Jobs::JobScheduleDesc child;
            child.function = [](void* data) { ++static_cast<Payload*>(data)->value; };
            child.userData = &payload;
            auto handle = Jobs::ScheduleJob(child);
            Jobs::Complete(handle);
        }));
        EXPECT_TRUE(graphTask.completion.WaitFor(3s));
    };
    const auto handle = Jobs::ScheduleJob(outer);
    ASSERT_NE(handle.id, 0u);
    EXPECT_TRUE(Until([&] { return Jobs::IsCompleted(handle); }));
    EXPECT_EQ(payload.value.load(), 1);
}

TEST(UnifiedTaskSystemTests, ExternalFailureCleanupCanWaitForGraphWorkOnTheSameSingleWorker)
{
    struct Observation
    {
        bool isForegroundWorker = false;
        bool hasJobExecutionScope = false;
        bool completed = false;
        TG::TaskStatus status = TG::TaskStatus::Pending;
        std::thread::id cleanupThread;
    };
    struct State
    {
        TG::TaskGraph* graph = nullptr;
        std::promise<void> dependencyStarted;
        std::promise<void> releaseDependency;
        std::shared_future<void> releaseFuture;
        std::promise<Observation> cleanupFinished;
        std::promise<std::thread::id> childExecuted;
        std::atomic_int dependentBodyRuns {0};
    } state;
    state.releaseFuture = state.releaseDependency.get_future().share();
    auto started = state.dependencyStarted.get_future();
    auto cleanup = state.cleanupFinished.get_future();
    auto childThread = state.childExecuted.get_future();
    // Keep callback storage alive until graph teardown has joined the pool,
    // including failure paths where the nested wait expires before its child runs.
    TG::TaskGraph graph({1u});
    state.graph = &graph;
    ASSERT_TRUE(graph.Start());
    ASSERT_EQ(Jobs::GetJobWorkerCount(), 1u);

    Jobs::BackgroundJobDesc background;
    background.userData = &state;
    background.function = [](void* data)
    {
        auto& state = *static_cast<State*>(data);
        state.dependencyStarted.set_value();
        state.releaseFuture.wait();
        throw std::runtime_error("expected external dependency failure");
    };
    const auto dependency = Jobs::ScheduleBackgroundJob(background);
    const auto startedStatus = started.wait_for(3s);

    Jobs::JobScheduleDesc dependent;
    dependent.dependency = dependency;
    dependent.userData = &state;
    dependent.function = [](void* data)
    {
        ++static_cast<State*>(data)->dependentBodyRuns;
    };
    dependent.cancelUserData = &state;
    dependent.cancelFunction = [](void* data)
    {
        auto& state = *static_cast<State*>(data);
        Observation observation;
        observation.isForegroundWorker = Jobs::Internal::IsForegroundWorkerThread();
        observation.hasJobExecutionScope = Jobs::Internal::IsExecutingJobWorkerThread();
        observation.cleanupThread = std::this_thread::get_id();
        auto child = state.graph->SubmitTracked(Task([&state]
        {
            state.childExecuted.set_value(std::this_thread::get_id());
        }));
        observation.completed = child.completion.WaitFor(3s);
        observation.status = child.completion.GetStatus();
        state.cleanupFinished.set_value(observation);
    };
    const auto cancelled = Jobs::ScheduleJob(dependent);
    // Admit the dependent while its background prerequisite is still pending,
    // so cancellation cannot run inline during ScheduleJob on the test thread.
    state.releaseDependency.set_value();
    ASSERT_NE(dependency.id, 0u);
    ASSERT_NE(cancelled.id, 0u);
    ASSERT_EQ(startedStatus, std::future_status::ready);

    // A plain future wait never assists Jobs. The idle foreground worker must
    // discover the failure and invoke cleanup outside any job execution scope.
    ASSERT_EQ(cleanup.wait_for(6s), std::future_status::ready);
    const auto observation = cleanup.get();
    EXPECT_TRUE(observation.isForegroundWorker);
    EXPECT_FALSE(observation.hasJobExecutionScope);
    EXPECT_NE(observation.cleanupThread, std::this_thread::get_id());
    EXPECT_TRUE(observation.completed);
    EXPECT_EQ(observation.status, TG::TaskStatus::Succeeded);
    ASSERT_EQ(childThread.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(childThread.get(), observation.cleanupThread);
    EXPECT_EQ(state.dependentBodyRuns.load(), 0);
    EXPECT_EQ(Jobs::GetCompletionStatus(dependency), Jobs::JobCompletionStatus::Failed);
}

TEST(UnifiedTaskSystemTests, SealedDiamondPublishesMemoryAndWaitsForEveryBranch)
{
    TG::TaskGraphConfig config {4u};
    config.retainedTerminalTaskCount = 0;
    TG::TaskGraph graph(config);
    TG::TaskGraphBatch batch;
    int input = 0, physics = 0, animation = 0, output = 0;
    const auto root = batch.Add(Task([&] { input = 10; }));
    const auto first = batch.Add(Task([&] { physics = input + 1; }));
    const auto second = batch.Add(Task([&] { animation = input + 2; }));
    const auto join = batch.Add(Task([&] { output = physics + animation; }));
    EXPECT_TRUE(batch.DependsOn(first, root));
    EXPECT_TRUE(batch.DependsOn(second, root));
    EXPECT_TRUE(batch.DependsOn(join, first));
    EXPECT_TRUE(batch.DependsOn(join, second));
    auto completion = batch.Execute(graph);
    ASSERT_TRUE(completion.IsValid());
    EXPECT_TRUE(completion.WaitFor(3s));
    EXPECT_EQ(completion.GetStatus(), TG::TaskStatus::Succeeded);
    EXPECT_EQ(output, 23);
    EXPECT_EQ(graph.GetRetainedTaskCount(), 0u);
    EXPECT_EQ(completion.GetTasks().size(), 4u);
    EXPECT_FALSE(batch.Execute(graph).IsValid());
    EXPECT_EQ(batch.Add(Task([] {})), TG::TaskGraphBatch::InvalidNode);
    EXPECT_FALSE(batch.DependsOn(join, root));
}

TEST(UnifiedTaskSystemTests, CyclicGraphIsRejectedBeforeAnyBodyRuns)
{
    TG::TaskGraph graph({1u});
    TG::TaskGraphBatch batch;
    std::atomic_int ran {0};
    const auto a = batch.Add(Task([&] { ++ran; }));
    const auto b = batch.Add(Task([&] { ++ran; }));
    EXPECT_TRUE(batch.DependsOn(a, b));
    EXPECT_TRUE(batch.DependsOn(b, a));
    EXPECT_FALSE(batch.Execute(graph).IsValid());
    EXPECT_EQ(ran.load(), 0);
    EXPECT_EQ(graph.GetPendingTaskCount(), 0u);
    TG::TaskGraphBatch self;
    const auto node = self.Add(Task([] {}));
    EXPECT_TRUE(self.DependsOn(node, node));
    EXPECT_FALSE(self.Execute(graph).IsValid());
}

TEST(UnifiedTaskSystemTests, EmptyGraphCompletesAndIndependentRootsAllRun)
{
    TG::TaskGraph graph({2u});
    TG::TaskGraphBatch empty;
    auto done = empty.Execute(graph);
    EXPECT_TRUE(done.IsValid());
    EXPECT_TRUE(done.IsComplete());
    EXPECT_EQ(done.Wait(), TG::TaskStatus::Succeeded);
    TG::TaskGraphBatch roots;
    std::atomic_int ran {0};
    for (int i = 0; i < 100; ++i)
        roots.Add(Task([&] { ++ran; }));
    EXPECT_EQ(roots.Execute(graph).Wait(), TG::TaskStatus::Succeeded);
    EXPECT_EQ(ran.load(), 100);
}

TEST(UnifiedTaskSystemTests, FailedGraphWaitIncludesRunningIndependentWork)
{
    TG::TaskGraph graph({2u});
    TG::TaskGraphBatch batch;
    std::atomic_bool release {false}, started {false}, successorRan {false};
    const auto failure = batch.Add(Task([] { throw std::runtime_error("expected"); }));
    const auto successor = batch.Add(Task([&] { successorRan = true; }));
    EXPECT_TRUE(batch.DependsOn(successor, failure));
    batch.Add(Task([&] { started = true; (void)Until([&] { return release.load(); }); }));
    auto completion = batch.Execute(graph);
    EXPECT_TRUE(Until([&] { return started.load(); }));
    EXPECT_FALSE(completion.WaitFor(10ms));
    release = true;
    EXPECT_TRUE(completion.WaitFor(3s));
    EXPECT_EQ(completion.GetStatus(), TG::TaskStatus::Failed);
    EXPECT_FALSE(successorRan.load());
}

TEST(UnifiedTaskSystemTests, DeepFailedDependencyChainDoesNotRecurse)
{
    TG::TaskGraph graph({1u});
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Game));
    TG::TaskGraphBatch batch;
    auto previous = batch.Add(Task([] { throw std::runtime_error("expected"); }, TG::NamedThread::Game));
    std::atomic_int ran {0};
    for (int i = 0; i < 12000; ++i)
    {
        const auto next = batch.Add(Task([&] { ++ran; }));
        EXPECT_TRUE(batch.DependsOn(next, previous));
        previous = next;
    }
    auto completion = batch.Execute(graph);
    ASSERT_TRUE(completion.IsValid());
    EXPECT_TRUE(completion.WaitFor(3s));
    EXPECT_EQ(completion.GetStatus(), TG::TaskStatus::Failed);
    EXPECT_EQ(ran.load(), 0);
}

TEST(UnifiedTaskSystemTests, WorkerCannotPumpOrRegisterTheMainLane)
{
    TG::TaskGraph graph({1u});
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Game));
    bool ran = false;
    auto main = graph.SubmitTracked(Task([&] { ran = true; }, TG::NamedThread::Game));
    auto worker = graph.SubmitTracked(Task([&]
    {
        EXPECT_FALSE(graph.RegisterCurrentThread(TG::NamedThread::Game));
        EXPECT_EQ(graph.Pump(TG::NamedThread::Game), 0u);
    }));
    // Do not pump Game while observing the worker result.
    EXPECT_TRUE(Until([&] { return worker.completion.IsComplete(); }));
    EXPECT_FALSE(ran);
    EXPECT_EQ(graph.Pump(TG::NamedThread::Game), 1u);
    EXPECT_TRUE(main.completion.IsComplete());
    EXPECT_TRUE(ran);
}

TEST(UnifiedTaskSystemTests, InlineModePreservesNamedAffinity)
{
    TG::TaskGraph graph({1u, true});
    bool ran = false;
    auto task = graph.SubmitTracked(Task([&] { ran = true; }, TG::NamedThread::Render));
    EXPECT_FALSE(ran);
    EXPECT_EQ(graph.Pump(TG::NamedThread::Render), 0u);
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Render));
    EXPECT_EQ(graph.Pump(TG::NamedThread::Render), 1u);
    EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Succeeded);
    EXPECT_TRUE(ran);
}

TEST(UnifiedTaskSystemTests, RenderLaneRemainsOnItsRegisteredThread)
{
    TG::TaskGraph graph({1u});
    std::atomic_bool registered {false}, pump {false};
    std::thread::id executor;
    auto task = graph.SubmitTracked(Task([&] { executor = std::this_thread::get_id(); }, TG::NamedThread::Render));
    std::thread render([&]
    {
        EXPECT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Render));
        registered = true;
        (void)Until([&] { return pump.load(); });
        graph.PumpCurrentThread();
        graph.UnregisterCurrentThread(TG::NamedThread::Render);
    });
    EXPECT_TRUE(Until([&] { return registered.load(); }));
    EXPECT_EQ(graph.Pump(TG::NamedThread::Render), 0u);
    pump = true;
    const auto renderId = render.get_id();
    render.join();
    EXPECT_EQ(executor, renderId);
    EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Succeeded);
}

TEST(UnifiedTaskSystemTests, ImmediateShutdownWaitsForRunningPayloadLifetime)
{
    TG::TaskGraph graph({1u});
    std::atomic_bool started {false}, release {false}, shutdownEntered {false};
    auto task = graph.SubmitTracked(Task([&]
    {
        started = true;
        (void)Until([&] { return release.load(); });
    }));
    EXPECT_TRUE(Until([&] { return started.load(); }));
    auto shutdown = std::async(std::launch::async, [&]
    {
        shutdownEntered = true;
        graph.Shutdown(TG::TaskGraphShutdownMode::Immediate);
    });
    EXPECT_TRUE(Until([&] { return shutdownEntered.load(); }));
    EXPECT_EQ(shutdown.wait_for(10ms), std::future_status::timeout);
    EXPECT_FALSE(task.completion.IsComplete());
    release = true;
    EXPECT_EQ(shutdown.wait_for(3s), std::future_status::ready);
    shutdown.get();
    EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Succeeded);
}

TEST(UnifiedTaskSystemTests, DrainCancelsOrphanLanesAndRejectsUntilExplicitRestart)
{
    TG::TaskGraph graph({1u});
    bool ran = false;
    auto orphan = graph.SubmitTracked(Task([&] { ran = true; }, TG::NamedThread::Render));
    graph.Shutdown();
    EXPECT_FALSE(ran);
    EXPECT_EQ(orphan.completion.GetStatus(), TG::TaskStatus::Cancelled);
    EXPECT_FALSE(graph.Submit([] {}).IsValid());
    EXPECT_TRUE(graph.Start());
    EXPECT_TRUE(graph.SubmitTracked(Task([] {})).completion.WaitFor(3s));
}

TEST(UnifiedTaskSystemTests, CapacityRejectsWithoutRunningAndRecoversAfterCancel)
{
    TG::TaskGraphConfig config {1u};
    config.maxPendingTaskCount = 2;
    TG::TaskGraph graph(config);
    auto first = graph.SubmitTracked(Task([] {}, TG::NamedThread::Render));
    auto second = graph.SubmitTracked(Task([] {}, TG::NamedThread::Render));
    EXPECT_TRUE(first.handle.IsValid());
    EXPECT_TRUE(second.handle.IsValid());
    EXPECT_FALSE(graph.Submit([] {}).IsValid());
    EXPECT_TRUE(graph.RequestCancel(first.handle));
    EXPECT_EQ(first.completion.Wait(), TG::TaskStatus::Cancelled);
    EXPECT_TRUE(graph.SubmitTracked(Task([] {})).completion.WaitFor(3s));
}

TEST(UnifiedTaskSystemTests, NonCancellableTaskRejectsExplicitCancellation)
{
    TG::TaskGraph graph({1u});
    auto desc = Task([] {}, TG::NamedThread::Game);
    desc.cancellable = false;
    auto task = graph.SubmitTracked(std::move(desc));
    EXPECT_FALSE(graph.RequestCancel(task.handle));
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Game));
    EXPECT_EQ(task.completion.Wait(), TG::TaskStatus::Succeeded);
}

TEST(UnifiedTaskSystemTests, WaitPumpsEveryNamedLaneOwnedByCurrentThread)
{
    TG::TaskGraph graph({1u});
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Game));
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Render));
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Rhi));
    const auto owner = std::this_thread::get_id();
    int executed = 0;
    TG::TaskGraphBatch frame;
    const auto render = frame.Add(Task([&]
    {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        ++executed;
    }, TG::NamedThread::Render));
    const auto rhi = frame.Add(Task([&]
    {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        ++executed;
    }, TG::NamedThread::Rhi));
    const auto game = frame.Add(Task([&] { ++executed; }, TG::NamedThread::Game));
    ASSERT_TRUE(frame.DependsOn(rhi, render));
    ASSERT_TRUE(frame.DependsOn(game, rhi));
    auto completion = frame.Execute(graph);
    EXPECT_TRUE(completion.WaitFor(100ms));
    EXPECT_EQ(completion.GetStatus(), TG::TaskStatus::Succeeded);
    EXPECT_EQ(executed, 3);
}

TEST(UnifiedTaskSystemTests, PumpCurrentThreadRespectsTotalBudgetAcrossOwnedLanes)
{
    TG::TaskGraph graph({1u});
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Game));
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Render));
    int executed = 0;
    for (const auto lane : {TG::NamedThread::Game, TG::NamedThread::Render})
        for (int index = 0; index < 2; ++index)
            ASSERT_TRUE(graph.Submit([&] { ++executed; }, lane).IsValid());
    EXPECT_EQ(graph.PumpCurrentThread(3), 3u);
    EXPECT_EQ(executed, 3);
    EXPECT_EQ(graph.PumpCurrentThread(), 1u);
    EXPECT_EQ(executed, 4);
}

TEST(UnifiedTaskSystemTests, SelfAndDescendantWaitFailInsteadOfDeadlocking)
{
    TG::TaskGraph graph({1u});
    ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Game));
    TG::TaskHandle self;
    self = graph.Submit([&] { (void)graph.Wait(self); }, TG::NamedThread::Game);
    EXPECT_EQ(graph.Wait(self), TG::TaskStatus::Failed);
    TG::TaskHandle child;
    auto parent = graph.Submit([&] { (void)graph.Wait(child); }, TG::NamedThread::Game);
    auto childDesc = Task([] {});
    childDesc.prerequisites = {parent};
    child = graph.Submit(std::move(childDesc));
    EXPECT_EQ(graph.Wait(parent), TG::TaskStatus::Failed);
    EXPECT_EQ(graph.Wait(child), TG::TaskStatus::Cancelled);
}

TEST(UnifiedTaskSystemTests, ConcurrentSubmitAndRetainedCompletionLifetimes)
{
    TG::TaskGraphConfig config {4u};
    config.retainedTerminalTaskCount = 0;
    TG::TaskGraph graph(config);
    ASSERT_TRUE(graph.Start());
    std::atomic_int ran {0};
    std::vector<std::thread> submitters;
    for (int lane = 0; lane < 8; ++lane)
        submitters.emplace_back([&]
        {
            std::vector<TG::TaskCompletion> completions;
            for (int i = 0; i < 300; ++i)
            {
                auto task = graph.SubmitTracked(Task([&] { ++ran; }));
                EXPECT_TRUE(task.handle.IsValid());
                completions.push_back(task.completion);
            }
            for (auto& completion : completions)
                EXPECT_EQ(completion.Wait(), TG::TaskStatus::Succeeded);
        });
    for (auto& thread : submitters)
        thread.join();
    EXPECT_EQ(ran.load(), 2400);
    EXPECT_EQ(graph.GetRetainedTaskCount(), 0u);
}

TEST(UnifiedTaskSystemTests, RepeatedRuntimeLifecycleKeepsSharedOwnerAlive)
{
    ASSERT_TRUE(Jobs::TryInitializeJobSystem({2u, 1u, false}));
    TG::TaskGraph graph;
    for (int iteration = 0; iteration < 30; ++iteration)
    {
        ASSERT_TRUE(graph.Start());
        ASSERT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Game));
        TG::TaskGraphBatch frame;
        const auto update = frame.Add(Task([] {}));
        const auto commit = frame.Add(Task([] {}, TG::NamedThread::Game));
        EXPECT_TRUE(frame.DependsOn(commit, update));
        EXPECT_EQ(frame.Execute(graph).Wait(), TG::TaskStatus::Succeeded);
        graph.Shutdown();
        EXPECT_TRUE(Jobs::IsJobSystemInitialized());
    }
    Jobs::ShutdownJobSystem();
}

TEST(UnifiedTaskSystemTests, HandlesFromAnotherGraphAreRejected)
{
    TG::TaskGraph first({1u}), second({1u});
    const auto foreign = first.Submit([] {}, TG::NamedThread::Render);
    const auto local = second.Submit([] {}, TG::NamedThread::Render);
    EXPECT_NE(foreign.generation, local.generation);
    auto task = Task([] {});
    task.prerequisites = {foreign};
    EXPECT_FALSE(second.Submit(std::move(task)).IsValid());
    EXPECT_FALSE(second.GetCompletion(foreign).IsValid());
}

TEST(UnifiedTaskSystemTests, CaptureDestructorCanReenterGraphInspection)
{
    TG::TaskGraph graph({1u});
    std::atomic_bool released {false};
    struct Capture
    {
        TG::TaskGraph& graph;
        std::atomic_bool& released;
        ~Capture()
        {
            (void)graph.GetPendingTaskCount();
            released = true;
        }
    };
    auto capture = std::make_shared<Capture>(graph, released);
    auto task = graph.SubmitTracked(Task([capture = std::move(capture)] {}));
    EXPECT_TRUE(task.completion.WaitFor(3s));
    EXPECT_TRUE(released.load());
}

TEST(UnifiedTaskSystemTests, FiniteWorkerWaitHonorsDeadlineWhileHelping)
{
    TG::TaskGraph graph({1u});
    const auto named = graph.Submit([] {}, TG::NamedThread::Game);
    std::atomic<long long> elapsed {-1};
    auto worker = graph.SubmitTracked(Task([&]
    {
        for (int i = 0; i < 200; ++i)
            (void)graph.Submit([] { std::this_thread::sleep_for(1ms); });
        const auto start = std::chrono::steady_clock::now();
        EXPECT_FALSE(graph.WaitFor(named, 2ms));
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
    }));
    EXPECT_TRUE(worker.completion.WaitFor(3s));
    EXPECT_GE(elapsed.load(), 0);
    EXPECT_LT(elapsed.load(), 150);
}

TEST(UnifiedTaskSystemTests, RegisteredLogicalRhiWorkerPumpsItsOwnLane)
{
    TG::TaskGraph graph({1u});
    auto worker = graph.SubmitTracked(Task([&]
    {
        EXPECT_TRUE(graph.RegisterCurrentThread(TG::NamedThread::Rhi));
        bool ran = false;
        auto rhi = graph.SubmitTracked(Task([&] { ran = true; }, TG::NamedThread::Rhi));
        EXPECT_EQ(graph.PumpCurrentThread(), 1u);
        EXPECT_TRUE(ran);
        EXPECT_EQ(rhi.completion.GetStatus(), TG::TaskStatus::Succeeded);
        graph.UnregisterCurrentThread(TG::NamedThread::Rhi);
    }));
    EXPECT_TRUE(worker.completion.WaitFor(3s));
}

TEST(UnifiedTaskSystemTests, RepeatedNamedCancellationDoesNotAccumulateQueueEntries)
{
    TG::TaskGraphConfig config {1u};
    config.retainedTerminalTaskCount = 0;
    config.maxPendingTaskCount = 2;
    TG::TaskGraph graph(config);
    for (int i = 0; i < 2000; ++i)
    {
        auto task = graph.SubmitTracked(Task([] {}, TG::NamedThread::Render));
        ASSERT_TRUE(task.handle.IsValid());
        EXPECT_TRUE(graph.RequestCancel(task.handle));
        EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Cancelled);
    }
    EXPECT_EQ(graph.GetNamedThreadQueueDepth(TG::NamedThread::Render), 0u);
    EXPECT_EQ(graph.GetRetainedTaskCount(), 0u);
}

TEST(UnifiedTaskSystemTests, LastFacadeReferenceInTaskCaptureReleasesSchedulerLease)
{
    auto graph = std::make_shared<TG::TaskGraph>(TG::TaskGraphConfig {1u});
    std::atomic_bool release {false}, started {false};
    auto task = graph->SubmitTracked(Task([owner = graph, &release, &started]
    {
        started = true;
        (void)Until([&] { return release.load(); });
    }));
    EXPECT_TRUE(Until([&] { return started.load(); }));
    graph.reset();
    release = true;
    EXPECT_TRUE(task.completion.WaitFor(3s));
    EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Succeeded);
    EXPECT_TRUE(Until([] { return !Jobs::IsJobSystemInitialized(); }));
    // Also joins a shutdown reaper if it has just published its terminal state.
    Jobs::ShutdownJobSystem();
}

TEST(UnifiedTaskSystemTests, InlineCaptureCanReleaseTheLastFacadeReference)
{
    TG::TaskGraphConfig config {1u};
    config.forceInlineExecution = true;
    config.retainedTerminalTaskCount = 0u;
    auto graph = std::make_shared<TG::TaskGraph>(config);
    const std::weak_ptr<TG::TaskGraph> facade = graph;
    auto* raw = graph.get();
    bool ran = false;
    auto task = raw->SubmitTracked(Task([owner = std::move(graph), &ran] { ran = true; }));
    EXPECT_TRUE(ran);
    EXPECT_TRUE(facade.expired());
    EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Succeeded);
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST(UnifiedTaskSystemTests, NamedPumpCaptureCanReleaseTheLastFacadeReference)
{
    TG::TaskGraphConfig config {1u};
    config.retainedTerminalTaskCount = 0u;
    auto graph = std::make_shared<TG::TaskGraph>(config);
    ASSERT_TRUE(graph->RegisterCurrentThread(TG::NamedThread::Game));
    const std::weak_ptr<TG::TaskGraph> facade = graph;
    auto* raw = graph.get();
    bool ran = false;
    auto task = raw->SubmitTracked(Task(
        [owner = std::move(graph), &ran] { ran = true; }, TG::NamedThread::Game));
    ASSERT_TRUE(task.handle.IsValid());
    EXPECT_FALSE(facade.expired());
    EXPECT_EQ(raw->Pump(TG::NamedThread::Game), 1u);
    EXPECT_TRUE(ran);
    EXPECT_TRUE(facade.expired());
    EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Succeeded);
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST(UnifiedTaskSystemTests, CurrentThreadPumpCaptureCanReleaseTheLastFacadeReference)
{
    TG::TaskGraphConfig config {1u};
    config.retainedTerminalTaskCount = 0u;
    auto graph = std::make_shared<TG::TaskGraph>(config);
    ASSERT_TRUE(graph->RegisterCurrentThread(TG::NamedThread::Game));
    ASSERT_TRUE(graph->RegisterCurrentThread(TG::NamedThread::Render));
    const std::weak_ptr<TG::TaskGraph> facade = graph;
    auto* raw = graph.get();
    bool ran = false;
    auto task = raw->SubmitTracked(Task(
        [owner = std::move(graph), &ran] { ran = true; }, TG::NamedThread::Game));
    ASSERT_TRUE(task.handle.IsValid());
    EXPECT_EQ(raw->PumpCurrentThread(), 1u);
    EXPECT_TRUE(ran);
    EXPECT_TRUE(facade.expired());
    EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Succeeded);
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST(UnifiedTaskSystemTests, CancelledNamedCaptureCanReleaseTheLastFacadeReference)
{
    TG::TaskGraphConfig config {1u};
    config.retainedTerminalTaskCount = 0u;
    auto graph = std::make_shared<TG::TaskGraph>(config);
    const std::weak_ptr<TG::TaskGraph> facade = graph;
    auto* raw = graph.get();
    bool ran = false;
    auto task = raw->SubmitTracked(Task(
        [owner = std::move(graph), &ran] { ran = true; }, TG::NamedThread::Render));
    ASSERT_TRUE(task.handle.IsValid());
    EXPECT_TRUE(raw->RequestCancel(task.handle));
    EXPECT_FALSE(ran);
    EXPECT_TRUE(facade.expired());
    EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Cancelled);
    EXPECT_FALSE(Jobs::IsJobSystemInitialized());
}

TEST(UnifiedTaskSystemTests, SuccessfulAnyCaptureCanCancelItsOnlySuccessorWithZeroRetention)
{
    TG::TaskHandle successor;
    std::atomic_bool cancellationAccepted {false}, childRan {false};
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    TG::TaskGraphConfig config {1u};
    config.retainedTerminalTaskCount = 0u;
    TG::TaskGraph graph(config);
    struct Capture
    {
        TG::TaskGraph& graph;
        TG::TaskHandle& successor;
        std::atomic_bool& accepted;
        Capture(TG::TaskGraph& owner, TG::TaskHandle& child, std::atomic_bool& result)
            : graph(owner), successor(child), accepted(result) {}
        ~Capture() { accepted.store(graph.RequestCancel(successor)); }
    };
    auto capture = std::make_shared<Capture>(graph, successor, cancellationAccepted);
    auto root = graph.SubmitTracked(Task([capture = std::move(capture), releaseFuture]
    {
        releaseFuture.wait();
    }));
    auto desc = Task([&] { childRan.store(true); });
    desc.prerequisites = {root.handle};
    auto child = graph.SubmitTracked(std::move(desc));
    successor = child.handle;
    release.set_value();
    ASSERT_TRUE(root.handle.IsValid());
    ASSERT_TRUE(child.handle.IsValid());
    EXPECT_TRUE(root.completion.WaitFor(3s));
    EXPECT_TRUE(child.completion.WaitFor(3s));
    EXPECT_EQ(root.completion.GetStatus(), TG::TaskStatus::Succeeded);
    EXPECT_EQ(child.completion.GetStatus(), TG::TaskStatus::Cancelled);
    EXPECT_TRUE(cancellationAccepted.load());
    EXPECT_FALSE(childRan.load());
    EXPECT_EQ(graph.GetRetainedTaskCount(), 0u);
}

TEST(UnifiedTaskSystemTests, ConcurrentDependentSubmissionCanRaceSuccessfulRootFinalization)
{
    for (int round = 0; round < 16; ++round)
    {
        SCOPED_TRACE(round);
        std::atomic_int children {0};
        std::promise<void> entered, release;
        auto enteredFuture = entered.get_future();
        auto releaseFuture = release.get_future().share();
        TG::TaskGraph graph({2u});
        auto root = graph.SubmitTracked(Task([&entered, releaseFuture]
        {
            entered.set_value();
            releaseFuture.wait();
        }));
        const auto started = enteredFuture.wait_for(3s);
        auto submitChild = [&]
        {
            auto desc = Task([&] { ++children; });
            desc.prerequisites = {root.handle};
            return graph.SubmitTracked(std::move(desc));
        };
        auto early = submitChild();
        TG::TaskSubmission late;
        std::thread submitter([&]
        {
            releaseFuture.wait();
            late = submitChild();
        });
        // Both the executing root and the concurrent submitter leave the same
        // gate. Finalize must inspect and consume its dependent list atomically.
        release.set_value();
        submitter.join();
        ASSERT_EQ(started, std::future_status::ready);
        ASSERT_TRUE(early.handle.IsValid());
        ASSERT_TRUE(late.handle.IsValid());
        EXPECT_TRUE(root.completion.WaitFor(3s));
        EXPECT_TRUE(early.completion.WaitFor(3s));
        EXPECT_TRUE(late.completion.WaitFor(3s));
        EXPECT_EQ(root.completion.GetStatus(), TG::TaskStatus::Succeeded);
        EXPECT_EQ(early.completion.GetStatus(), TG::TaskStatus::Succeeded);
        EXPECT_EQ(late.completion.GetStatus(), TG::TaskStatus::Succeeded);
        EXPECT_EQ(children.load(), 2);
    }
}

TEST(UnifiedTaskSystemTests, TelemetryDoesNotUnderflowAfterSharedPoolStops)
{
    TG::TaskGraph first({1u});
    EXPECT_TRUE(first.SubmitTracked(Task([] {})).completion.WaitFor(3s));
    TG::TaskGraph second({1u});
    ASSERT_TRUE(second.Start()); // Its baseline already contains first's dispatch.
    first.Shutdown();
    second.Shutdown();
    EXPECT_EQ(second.GetWorkerStealCount(), 0u);
    EXPECT_EQ(second.GetWorkerLocalExecutionCount(), 0u);
    EXPECT_EQ(second.GetWorkerInjectionExecutionCount(), 0u);
}

TEST(UnifiedTaskSystemTests, CompletionPollingPublishesBodyAndCaptureDestructionWrites)
{
    for (const bool pollStatus : {false, true})
    {
        SCOPED_TRACE(pollStatus ? "GetStatus" : "IsComplete");
        int bodyWrite = 0;
        int captureWrite = 0;
        std::promise<void> entered;
        std::promise<void> release;
        auto enteredFuture = entered.get_future();
        auto releaseFuture = release.get_future().share();
        // Destroy the graph before the non-atomic storage even on assertion failure.
        TG::TaskGraph graph({1u});
        struct Capture
        {
            int& output;
            explicit Capture(int& value) : output(value) {}
            ~Capture() { output = 73; }
        };
        auto capture = std::make_shared<Capture>(captureWrite);
        auto task = graph.SubmitTracked(Task(
            [capture = std::move(capture), &bodyWrite, &entered, releaseFuture]
            {
                entered.set_value();
                releaseFuture.wait();
                bodyWrite = 41;
            }));
        ASSERT_TRUE(task.handle.IsValid());
        const auto enteredStatus = enteredFuture.wait_for(3s);
        // Always open the gate before asserting, so a late worker cannot be stranded.
        release.set_value();
        ASSERT_EQ(enteredStatus, std::future_status::ready);

        const auto deadline = std::chrono::steady_clock::now() + 3s;
        bool observedCompletion = false;
        do
        {
            observedCompletion = pollStatus
                ? task.completion.GetStatus() == TG::TaskStatus::Succeeded
                : task.completion.IsComplete();
            if (observedCompletion)
                break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);

        // There is deliberately no Wait/GetStatus before these reads. Only the
        // polling operation may acquire the body and capture-destructor writes.
        // A timeout must not read concurrently written non-atomic storage.
        ASSERT_TRUE(observedCompletion);
        EXPECT_EQ(bodyWrite, 41);
        EXPECT_EQ(captureWrite, 73);
        EXPECT_EQ(task.completion.GetStatus(), TG::TaskStatus::Succeeded);
    }
}

TEST(UnifiedTaskSystemTests, ConcurrentCompletionWaitersObserveTheSamePublication)
{
    struct Writes
    {
        int body = 0;
        int retiredCapture = 0;
    };
    struct Capture
    {
        std::shared_ptr<Writes> writes;
        explicit Capture(std::shared_ptr<Writes> value) : writes(std::move(value)) {}
        ~Capture() { writes->retiredCapture = 97; }
    };
    struct Observation
    {
        bool wasPending = false;
        bool completed = false;
        TG::TaskStatus status = TG::TaskStatus::Pending;
        int body = 0;
        int retiredCapture = 0;
    };

    TG::TaskGraph graph({1u});
    constexpr int waiterCount = 8;
    for (int round = 0; round < 16; ++round)
    {
        SCOPED_TRACE(round);
        auto writes = std::make_shared<Writes>();
        auto capture = std::make_shared<Capture>(writes);
        std::promise<void> release;
        auto releaseFuture = release.get_future().share();
        std::promise<void> allWaitersReady;
        auto readyFuture = allWaitersReady.get_future();
        std::atomic_int readyCount {0};
        auto task = graph.SubmitTracked(Task(
            [writes, capture = std::move(capture), releaseFuture]
            {
                releaseFuture.wait();
                writes->body = 83;
            }));
        ASSERT_TRUE(task.handle.IsValid());
        std::vector<std::future<Observation>> waiters;
        for (int index = 0; index < waiterCount; ++index)
            waiters.push_back(std::async(std::launch::async,
                [completion = task.completion, writes, &readyCount, &allWaitersReady]() mutable
                {
                    Observation observation;
                    observation.wasPending = !completion.IsComplete();
                    if (readyCount.fetch_add(1, std::memory_order_acq_rel) + 1 == waiterCount)
                        allWaitersReady.set_value();
                    observation.completed = completion.WaitFor(3s);
                    observation.status = completion.GetStatus();
                    if (observation.completed && observation.status == TG::TaskStatus::Succeeded)
                    {
                        observation.body = writes->body;
                        observation.retiredCapture = writes->retiredCapture;
                    }
                    return observation;
                }));

        const auto readyStatus = readyFuture.wait_for(3s);
        // Race publication with the last waiter's check-to-sleep boundary;
        // readiness is synchronized explicitly, without sleeping to infer it.
        release.set_value();
        for (auto& waiter : waiters)
        {
            const auto observation = waiter.get();
            EXPECT_TRUE(observation.wasPending);
            EXPECT_TRUE(observation.completed);
            EXPECT_EQ(observation.status, TG::TaskStatus::Succeeded);
            EXPECT_EQ(observation.body, 83);
            EXPECT_EQ(observation.retiredCapture, 97);
        }
        ASSERT_EQ(readyStatus, std::future_status::ready);
    }
}
