#include "TestComponents.h"
#include "JobTestSupport.h"
#include <ECS/Systems/SystemScheduler.h>
#include <Base/TaskGraph/TaskGraph.h>
#include <Base/Jobs/JobSafety.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <set>
#include <thread>
using namespace NLS::ECS;
using namespace ECSTest;
namespace TG = NLS::Base::TaskGraph;
namespace Jobs = NLS::Base::Jobs;
namespace
{
class ParallelSystem final : public System
{
public:
    explicit ParallelSystem(std::function<void(SystemContext&)> callback) : m_callback(std::move(callback)) {}
    void OnUpdate(SystemContext& context) override { m_callback(context); }
private:
    std::function<void(SystemContext&)> m_callback;
};
}

TEST(ECSParallel, TrackedSubmissionSurvivesImmediateTerminalRecycling)
{
    TG::TaskGraphConfig config; config.workerCount = 1; config.forceInlineExecution = true; config.retainedTerminalTaskCount = 0;
    TG::TaskGraph graph(config); ASSERT_TRUE(graph.Start());
    TG::TaskDesc task; task.function = [] {};
    auto submission = graph.SubmitTracked(std::move(task));
    ASSERT_TRUE(submission.handle.IsValid()); ASSERT_TRUE(submission.completion.IsValid());
    EXPECT_EQ(submission.completion.Wait(), TG::TaskStatus::Succeeded);
    EXPECT_FALSE(graph.GetCompletion(submission.handle).IsValid());
}
TEST(ECSParallel, ColumnBatchesExecuteOnWorkersAndPreserveData)
{
    WorldConfig config; config.chunkBytes = 1024; config.minimumParallelEntities = 0; config.chunksPerJob = 4;
    World world(config); Register(world); auto entities = world.CreateEntities(12000, Signature<Position, Velocity>(world));
    world.Query<Write<Velocity>>().Each([](Entity, Velocity& v) { v.x = 3; });
    JobSystemScope jobs(4);
    std::mutex threadsMutex; std::condition_variable threadsReady; std::set<std::thread::id> threads;
    world.Query<Write<Position>, Read<Velocity>>().ParallelForEachChunk([&](ChunkView& view)
    {
        {
            std::unique_lock lock(threadsMutex);
            const bool first = threads.empty();
            threads.insert(std::this_thread::get_id());
            if (threads.size() >= 2) threadsReady.notify_all();
            // A fast range may legitimately be drained by a single caller.
            // Hold only the first callback until another lane enters; bound the
            // wait so a regression to serial execution fails without deadlock.
            if (first) threadsReady.wait_for(lock, std::chrono::seconds(2), [&] { return threads.size() >= 2; });
        }
        auto positions = view.WriteSpan<Position>(); auto velocities = view.ReadSpan<Velocity>();
        for (size_t row = 0; row < view.Size(); ++row) positions[row].x += velocities[row].x;
    });
    // Complete may execute foreground work on the waiting caller as well.
    EXPECT_GE(threads.size(), 2u);
    for (auto entity : entities) EXPECT_EQ(world.ReadComponent<Position>(entity).x, 3);
    EXPECT_GT(world.GetStatistics().jobs, 1u);
    EXPECT_TRUE(world.DestroyEntity(entities[0])); // All jobs and storage leases joined.
}
TEST(ECSParallel, IndependentReadSystemsOverlapAndMainThreadSystemsStayOnOwner)
{
    World world; Register(world); world.CreateEntity(Signature<Position>(world));
    JobSystemScope jobs(2);
    std::atomic<int> entered{0}; std::atomic<bool> overlapped{false};
    auto& systems = world.Systems(); const auto owner = std::this_thread::get_id();
    for (int i = 0; i < 2; ++i)
    {
        SystemDescription description{"read" + std::to_string(i)}; description.mainThread = false; description.Reads<Position>(world);
        systems.RegisterSystem<ParallelSystem>(description, [&](SystemContext& context)
        {
            ++entered;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (entered.load() < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            if (entered.load() == 2) overlapped = true;
            EXPECT_NE(Jobs::Internal::GetCurrentJobWorkerHandle().id, 0u);
            EXPECT_TRUE(context.jobsEnabled);
            EXPECT_EQ(context.world.Query<Read<Position>>().Count(), 1u);
        });
    }
    systems.RegisterSystem<ParallelSystem>({"main"}, [&](SystemContext&) { EXPECT_EQ(std::this_thread::get_id(), owner); });
    systems.RunPhase(SystemPhase::Update, {});
    EXPECT_TRUE(overlapped.load()); EXPECT_EQ(entered.load(), 2);
    EXPECT_EQ(systems.GetStatistics()[0].updates, 1u);
    EXPECT_FALSE(systems.GetStatistics()[2].lastRanOnWorker);
}
TEST(ECSParallel, WriteConflictsAreSerializedAcrossWorkers)
{
    World world; Register(world); world.SetResource<Position>({}); auto& systems = world.Systems();
    JobSystemScope jobs(4);
    for (int i = 0; i < 6; ++i)
    {
        SystemDescription description{"writer" + std::to_string(i)}; description.mainThread = false; description.WritesResource<Position>(world);
        systems.RegisterSystem<ParallelSystem>(description, [](SystemContext& context) { context.world.WriteResource<Position>().x += 1; });
    }
    for (int iteration = 0; iteration < 40; ++iteration) systems.RunPhase(SystemPhase::Update, {});
    EXPECT_EQ(world.ReadResource<Position>().x, 240);
}
TEST(ECSParallel, NestedChunkJobsCompleteWithOneWorker)
{
    WorldConfig config; config.chunkBytes = 512; config.minimumParallelEntities = 0; config.chunksPerJob = 2;
    World world(config); Register(world); world.CreateEntities(3000, Signature<Position>(world));
    JobSystemScope jobs(1);
    SystemDescription description{"nested"}; description.mainThread = false; description.Writes<Position>(world);
    auto& systems = world.Systems();
    systems.RegisterSystem<ParallelSystem>(description, [](SystemContext& context)
    { context.world.Query<Write<Position>>().ParallelEach([](Entity, Position& p) { p.x += 2; }); });
    systems.RunPhase(SystemPhase::Update, {});
    world.Query<Read<Position>>().Each([](Entity, const Position& p) { EXPECT_EQ(p.x, 2); });
}
TEST(ECSParallel, CancellationJoinsAcceptedJobsBeforeStorageCanChange)
{
    WorldConfig config; config.chunkBytes = 512; config.minimumParallelEntities = 0; config.chunksPerJob = 1;
    World world(config); Register(world); world.CreateEntities(4000, Signature<Position>(world));
    JobSystemScope jobs(3);
    std::atomic<bool> entered{false};
    std::thread canceller([&]
    { while (!entered.load()) std::this_thread::yield(); world.RequestCancellation(); });
    EXPECT_THROW(world.Query<Write<Position>>().ParallelForEachChunk([&](ChunkView&)
    { entered = true; while (!world.IsCancellationRequested()) std::this_thread::yield(); }), std::runtime_error);
    canceller.join(); world.ClearCancellation();
    EXPECT_NO_THROW(world.CreateEntity());
}
TEST(ECSParallel, DebugSingleThreadModeRunsSystemsAndChunksOnOwner)
{
    WorldConfig config; config.singleThreaded = true; config.minimumParallelEntities = 0;
    World world(config); Register(world); world.CreateEntities(3000, Signature<Position>(world));
    JobSystemScope jobs(2);
    const auto owner = std::this_thread::get_id();
    SystemDescription description{"serial"}; description.mainThread = false; description.Writes<Position>(world);
    auto& systems = world.Systems(); systems.RegisterSystem<ParallelSystem>(description, [&](SystemContext& context)
    {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        EXPECT_FALSE(context.jobsEnabled);
        context.world.Query<Write<Position>>().ParallelEach([&](Entity, Position& p) { EXPECT_EQ(std::this_thread::get_id(), owner); p.x = 4; });
    });
    systems.RunPhase(SystemPhase::Update, {}); EXPECT_EQ(world.GetStatistics().jobs, 0u);
}
TEST(ECSParallel, ParallelChunkCommandWritersRespectSystemOrderingDomains)
{
    WorldConfig config; config.chunkBytes = 512; config.minimumParallelEntities = 0; config.chunksPerJob = 2;
    World world(config); Register(world); world.CreateEntities(2000, Signature<Position>(world));
    JobSystemScope jobs(3);
    SystemDescription description{"commands"}; description.mainThread = false; description.Reads<Position>(world);
    auto& systems = world.Systems(); systems.RegisterSystem<ParallelSystem>(description, [](SystemContext& context)
    {
        context.world.Query<Read<Position>>().ParallelForEachChunk([&](ChunkView& chunk)
        {
            auto writer = context.commands.Fork(chunk.Entities()[0].index);
            for (const auto entity : chunk.Entities()) writer.DestroyEntity(entity);
        });
    });
    systems.RunPhase(SystemPhase::Update, {});
    EXPECT_EQ(world.GetEntityCount(), 0u); EXPECT_EQ(systems.GetLastPlayback().applied, 2000u);
}
TEST(ECSParallel, ForbiddenSyncWaitRejectsBeforeAnyCallbackOrWrite)
{
    JobSystemScope jobs;
    WorldConfig config; config.minimumParallelEntities = 0;
    World world(config); Register(world); const auto entity = world.CreateEntity(Signature<Position>(world));
    {
        Jobs::DisallowJobSyncWaitScope disallow;
        EXPECT_THROW(world.Query<Write<Position>>().ParallelEach([](Entity, Position& p) { p.x = 99; }), std::logic_error);
    }
    EXPECT_FLOAT_EQ(world.ReadComponent<Position>(entity).x, 0);
    EXPECT_NO_THROW(world.DestroyEntity(entity));
}
TEST(ECSParallel, WorkerExceptionPropagatesAndAllStorageLeasesAreJoined)
{
    JobSystemScope jobs;
    WorldConfig config; config.minimumParallelEntities = 0; config.chunkBytes = 512;
    World world(config); Register(world); world.CreateEntities(3000, Signature<Position>(world));
    EXPECT_THROW(world.Query<Write<Position>>().ParallelEach([](Entity, Position&) { throw std::out_of_range("worker error"); }), std::out_of_range);
    EXPECT_NO_THROW(world.CreateEntity());
    EXPECT_NO_THROW(world.Query<Write<Position>>().ParallelEach([](Entity, Position& p) { p.x = 5; }));
    world.Query<Read<Position>>().Each([](Entity, const Position& p) { EXPECT_FLOAT_EQ(p.x, 5); });
}
TEST(ECSParallel, UninitializedServiceAllowsSerialSystemsButRejectsExplicitLargeParallelQuery)
{
    ASSERT_FALSE(Jobs::IsJobSystemInitialized());
    WorldConfig config; config.minimumParallelEntities = 0;
    World world(config); Register(world); const auto entity = world.CreateEntity(Signature<Position>(world));
    SystemDescription description{"serial fallback"}; description.mainThread = false; description.Writes<Position>(world);
    world.Systems().RegisterSystem<ParallelSystem>(description, [](SystemContext& context)
    { EXPECT_FALSE(context.jobsEnabled); context.world.Query<Write<Position>>().Each([](Entity, Position& p) { ++p.x; }); });
    world.Systems().RunPhase(SystemPhase::Update, {});
    EXPECT_FLOAT_EQ(world.ReadComponent<Position>(entity).x, 1);
    EXPECT_THROW(world.Query<Write<Position>>().ParallelEach([](Entity, Position&) {}), std::logic_error);
}
TEST(ECSParallel, PhaseFailureJoinsOtherSystemsAndDropsRecordedCommands)
{
    JobSystemScope jobs(2); World world; std::atomic<int> completed = 0;
    SystemDescription failing{"failing"}; failing.mainThread = false;
    world.Systems().RegisterSystem<ParallelSystem>(failing, [](SystemContext& context)
    { context.commands.CreateEntity(); throw std::out_of_range("system failure"); });
    SystemDescription independent{"independent"}; independent.mainThread = false;
    world.Systems().RegisterSystem<ParallelSystem>(independent, [&](SystemContext&) { ++completed; });
    EXPECT_THROW(world.Systems().RunPhase(SystemPhase::Update, {}), std::out_of_range);
    EXPECT_EQ(completed.load(), 1); EXPECT_EQ(world.GetEntityCount(), 0u);
    EXPECT_NO_THROW(world.CreateEntity()); EXPECT_TRUE(world.Systems().Remove("failing"));
    EXPECT_NO_THROW(world.Systems().RunPhase(SystemPhase::Update, {}));
    EXPECT_EQ(world.GetEntityCount(), 1u); EXPECT_EQ(completed.load(), 2);
}
TEST(ECSParallel, MainThreadOnlyPhaseDoesNotRequireJobCompletion)
{
    JobSystemScope jobs; World world; int updates = 0;
    world.Systems().RegisterSystem<ParallelSystem>({"main"}, [&](SystemContext&) { ++updates; });
    Jobs::DisallowJobSyncWaitScope disallow;
    EXPECT_NO_THROW(world.Systems().RunPhase(SystemPhase::Update, {})); EXPECT_EQ(updates, 1);
}
