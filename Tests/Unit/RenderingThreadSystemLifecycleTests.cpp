#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "Jobs/JobSystem.h"
#include "Jobs/BackgroundJobQueue.h"
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "TaskGraph/TaskGraphSystem.h"

namespace
{
    using namespace std::chrono_literals;
    using namespace NLS::Base::TaskGraph;
    using namespace NLS::Render::Context;
    namespace Jobs = NLS::Base::Jobs;

    class RenderingThreadLifecycleTests : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ASSERT_FALSE(RenderingThreadSystem::HasRenderingThreadOwners());
            ShutdownGlobalTaskGraph(TaskGraphShutdownMode::Immediate);
            Jobs::ShutdownJobSystem();
            ResetRenderFatalErrorForTesting();
            RenderCommandDispatcher::ResetForTesting();
            Jobs::JobSystemConfig config;
            config.workerCount = 2u;
            config.backgroundWorkerCount = 1u;
            ASSERT_TRUE(Jobs::TryInitializeJobSystem(config));
        }

        void TearDown() override
        {
            EXPECT_FALSE(RenderingThreadSystem::HasRenderingThreadOwners());
            EXPECT_FALSE(RenderingThreadSystem::HasActiveRenderThread());
            RenderCommandDispatcher::ResetForTesting();
            ShutdownGlobalTaskGraph(TaskGraphShutdownMode::Immediate);
            Jobs::ShutdownJobSystem();
            EXPECT_FALSE(Jobs::IsJobSystemInitialized());
            ResetRenderFatalErrorForTesting();
        }
    };

    TEST_F(RenderingThreadLifecycleTests, SuspendedOwnerKeepsSharedGraphAvailableAfterAnotherOwnerStops)
    {
        TaskGraph graph({ 8u, false, true });
        ASSERT_TRUE(graph.Start());
        EXPECT_EQ(graph.GetWorkerCount(), 2u);

        RenderingThreadSystem first;
        RenderingThreadSystem second;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.rhiThreadMode = RhiThreadMode::None;
        ASSERT_TRUE(first.InitRenderingThreads(params));
        ASSERT_TRUE(second.InitRenderingThreads(params));
        first.SuspendRenderingThreads();
        second.ShutdownRenderingThreads();

        EXPECT_FALSE(RenderingThreadSystem::HasActiveRenderThread());
        EXPECT_TRUE(RenderingThreadSystem::HasRenderingThreadOwners());
        // This is the application close condition; a suspended peer retains it.
        if (!RenderingThreadSystem::HasRenderingThreadOwners())
            graph.Shutdown();
        EXPECT_TRUE(graph.IsStarted());
        // The first application's Jobs owner may leave while the graph remains.
        Jobs::ShutdownJobSystem();
        EXPECT_TRUE(Jobs::IsJobSystemInitialized());
        ASSERT_TRUE(first.ResumeRenderingThreads());
        std::atomic_bool renderTaskRan { false };
        const auto task = graph.Submit([&] { renderTaskRan.store(true); }, NamedThread::Render);
        EXPECT_TRUE(graph.WaitFor(task, 2s));
        EXPECT_TRUE(renderTaskRan.load());

        first.ShutdownRenderingThreads();
        EXPECT_FALSE(RenderingThreadSystem::HasRenderingThreadOwners());
        graph.Shutdown();
        EXPECT_FALSE(Jobs::IsJobSystemInitialized());
    }

    TEST_F(RenderingThreadLifecycleTests, DedicatedNamedLanesKeepTheirOwnersWhileAnyUsesSharedJobs)
    {
        TaskGraph graph({ 8u, false, true });
        ASSERT_TRUE(graph.Start());
        RenderingThreadSystem threads;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.rhiThreadMode = RhiThreadMode::DedicatedThread;
        ASSERT_TRUE(threads.InitRenderingThreads(params));
        const auto mainThread = std::this_thread::get_id();
        std::atomic_bool renderIdentity { false };
        std::atomic_bool rhiIdentity { false };
        std::atomic_bool workerIdentity { false };
        bool gameIdentity = false;
        const auto renderTask = graph.Submit([&]
        {
            renderIdentity = RenderingThreadSystem::IsInRenderingThread() &&
                graph.GetCurrentNamedThread() == NamedThread::Render &&
                std::this_thread::get_id() != mainThread;
        }, NamedThread::Render);
        const auto rhiTask = graph.Submit([&]
        {
            rhiIdentity = RenderingThreadSystem::IsInRhiThread() &&
                graph.GetCurrentNamedThread() == NamedThread::Rhi &&
                std::this_thread::get_id() != mainThread;
        }, NamedThread::Rhi);
        const auto workerTask = graph.Submit([&]
        {
            workerIdentity = graph.IsWorkerThread() && !RenderingThreadSystem::IsInGameThread() &&
                !RenderingThreadSystem::IsInRenderingThread();
        });
        const auto gameTask = graph.Submit([&]
        {
            gameIdentity = RenderingThreadSystem::IsInGameThread() &&
                std::this_thread::get_id() == mainThread;
        }, NamedThread::Game);
        EXPECT_TRUE(graph.WaitFor(renderTask, 2s));
        EXPECT_TRUE(graph.WaitFor(rhiTask, 2s));
        EXPECT_TRUE(graph.WaitFor(workerTask, 2s));
        (void)RenderingThreadSystem::PumpGameThreadTasks(0u);
        EXPECT_TRUE(graph.WaitFor(gameTask, 2s));
        EXPECT_TRUE(renderIdentity.load());
        EXPECT_TRUE(rhiIdentity.load());
        EXPECT_TRUE(workerIdentity.load());
        EXPECT_TRUE(gameIdentity);
        threads.ShutdownRenderingThreads();
        EXPECT_EQ(graph.GetCurrentNamedThread(), NamedThread::Any);
        graph.Shutdown();
        EXPECT_TRUE(Jobs::IsJobSystemInitialized());
    }

    TEST_F(RenderingThreadLifecycleTests, DrainCompletesBackgroundToRenderToGameBeforeLanesStop)
    {
        TaskGraph graph({ 2u, false, true });
        ASSERT_TRUE(graph.Start());
        RenderingThreadSystem threads;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.rhiThreadMode = RhiThreadMode::None;
        ASSERT_TRUE(threads.InitRenderingThreads(params));
        struct Chain
        {
            TaskGraph* graph;
            std::atomic_bool renderComplete { false };
            std::atomic_bool gameComplete { false };
            std::atomic_bool backgroundComplete { false };
        } chain { &graph };
        Jobs::BackgroundJobDesc job;
        job.userData = &chain;
        job.function = [](void* raw)
        {
            auto& state = *static_cast<Chain*>(raw);
            const auto render = state.graph->Submit([&state]
            {
                const auto game = state.graph->Submit([&state]
                {
                    state.gameComplete = RenderingThreadSystem::IsInGameThread();
                }, NamedThread::Game);
                state.renderComplete = state.graph->WaitFor(game, 2s) &&
                    RenderingThreadSystem::IsInRenderingThread();
            }, NamedThread::Render);
            state.backgroundComplete = state.graph->WaitFor(render, 3s);
        };
        ASSERT_NE(Jobs::ScheduleBackgroundJob(job).id, 0u);
        Jobs::DrainAcceptedJobs([] { (void)RenderingThreadSystem::PumpGameThreadTasks(64u); });
        EXPECT_TRUE(chain.backgroundComplete.load());
        EXPECT_TRUE(chain.renderComplete.load());
        EXPECT_TRUE(chain.gameComplete.load());
        threads.ShutdownRenderingThreads();
        graph.Shutdown();
        EXPECT_TRUE(Jobs::IsJobSystemInitialized());
        Jobs::ShutdownJobSystem();
        EXPECT_FALSE(Jobs::IsJobSystemInitialized());
    }

    TEST_F(RenderingThreadLifecycleTests, RepeatedSuspendResumeAndShutdownBalanceOwnerCount)
    {
        TaskGraph graph({ 2u, false, true });
        ASSERT_TRUE(graph.Start());
        RenderingThreadSystem threads;
        RenderingThreadInitParams params;
        params.taskGraph = &graph;
        params.rhiThreadMode = RhiThreadMode::DedicatedThread;
        for (unsigned iteration = 0u; iteration < 3u; ++iteration)
        {
            ASSERT_TRUE(threads.InitRenderingThreads(params));
            ASSERT_TRUE(threads.InitRenderingThreads(params));
            EXPECT_TRUE(RenderingThreadSystem::HasRenderingThreadOwners());
            threads.SuspendRenderingThreads();
            threads.SuspendRenderingThreads();
            EXPECT_FALSE(RenderingThreadSystem::HasActiveRenderThread());
            EXPECT_TRUE(RenderingThreadSystem::HasRenderingThreadOwners());
            ASSERT_TRUE(threads.ResumeRenderingThreads());
            threads.ShutdownRenderingThreads();
            threads.ShutdownRenderingThreads();
            EXPECT_FALSE(RenderingThreadSystem::HasRenderingThreadOwners());
        }
        graph.Shutdown();
    }
}
