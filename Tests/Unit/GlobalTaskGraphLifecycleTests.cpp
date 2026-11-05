#include <gtest/gtest.h>

#include "TaskGraph/TaskGraphSystem.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace NLS::Base::TaskGraph
{
TEST(GlobalTaskGraphLifecycleTests, ReadingStoppedGraphDoesNotRestartItsWorkers)
{
    ShutdownGlobalTaskGraph(TaskGraphShutdownMode::Immediate);
    TaskGraphConfig config;
    config.workerCount = 1;
    ASSERT_TRUE(InitializeGlobalTaskGraph(config));
    auto* graph = &GetGlobalTaskGraph();
    ShutdownGlobalTaskGraph();
    EXPECT_EQ(&GetGlobalTaskGraph(), graph);
    EXPECT_FALSE(IsGlobalTaskGraphStarted());
    EXPECT_FALSE(graph->Submit([] {}).IsValid());
    EXPECT_TRUE(InitializeGlobalTaskGraph(config));
    EXPECT_TRUE(IsGlobalTaskGraphStarted());
    ShutdownGlobalTaskGraph();
}

TEST(GlobalTaskGraphLifecycleTests, AcceptedCallbackCanInspectGlobalGraphDuringShutdown)
{
    ShutdownGlobalTaskGraph(TaskGraphShutdownMode::Immediate);
    TaskGraphConfig config;
    config.workerCount = 1;
    ASSERT_TRUE(InitializeGlobalTaskGraph(config));
    auto& graph = GetGlobalTaskGraph();
    std::promise<void> entered;
    std::promise<void> inspect;
    auto enteredFuture = entered.get_future();
    auto inspectFuture = inspect.get_future();
    std::atomic_bool observed{false};
    TaskDesc task;
    task.function = [&]
    {
        entered.set_value();
        inspectFuture.wait();
        observed.store(&GetGlobalTaskGraph() == &graph && IsGlobalTaskGraphStarted());
    };
    const auto submission = graph.SubmitTracked(std::move(task));
    ASSERT_TRUE(submission.handle.IsValid());
    enteredFuture.wait();
    std::thread shutdown([] { ShutdownGlobalTaskGraph(); });

    // Wait until shutdown has closed acceptance before releasing the callback.
    // The callback must be able to look up the graph that shutdown is draining.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (graph.Submit([] {}).IsValid() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    inspect.set_value();
    shutdown.join();
    EXPECT_TRUE(observed.load());
    EXPECT_EQ(submission.completion.GetStatus(), TaskStatus::Succeeded);
    EXPECT_FALSE(IsGlobalTaskGraphStarted());
}
}
