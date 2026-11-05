#include "JobTestSupport.h"
#include <ECS/Core/World.h>
#include <ECS/Serialization/SceneStreaming.h>
#include <Base/Jobs/BackgroundJobQueue.h>
#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
using namespace NLS::ECS;
namespace Jobs = NLS::Base::Jobs;
struct Saved { int value = 0; Entity target; };
void Register(World& world)
{
    world.RegisterComponent<Saved>("stream.Saved");
    RegisterComponentCodec<Saved>(world.GetRegistry(),
        [](const Saved& value, const EntityWriteContext& context)
        { return ComponentJson{{"value", value.value}, {"target", context.EntityId(value.target)}}; },
        [](Saved& value, const ComponentJson& json, const EntityReadContext& context)
        { value = {json.at("value").get<int>(), context.Resolve(json.at("target").get<std::string>())}; });
}
struct Files
{
    std::filesystem::path directory;
    Files()
    {
        static std::atomic<unsigned> serial{0};
        directory = std::filesystem::path("tmp") / ("ecs-streaming-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial++));
        std::filesystem::create_directories(directory);
    }
    ~Files() { std::error_code error; std::filesystem::remove_all(directory, error); }
    WorldDocument Document(int value = 7)
    {
        World source; Register(source); const auto a = source.CreateEntity(), b = source.CreateEntity();
        source.AddComponent<Saved>(a, {value, b}); source.AddComponent<Saved>(b, {value + 1, a});
        const std::array records{PersistentEntity{a, "a", "author-a"}, PersistentEntity{b, "b", "author-b"}};
        return WorldArchive::Capture(source, records);
    }
    std::filesystem::path Save(std::string name, int value = 7)
    { auto path = directory / name; WorldArchive::WriteFile(path, Document(value)); return path; }
};
class BackgroundGate
{
public:
    BackgroundGate()
    {
        Jobs::BackgroundJobDesc desc; desc.userData = this; desc.debugName = "ECS test background gate";
        desc.function = [](void* pointer)
        {
            auto& gate = *static_cast<BackgroundGate*>(pointer); std::unique_lock lock(gate.mutex);
            gate.entered = true; gate.condition.notify_all();
            gate.condition.wait(lock, [&] { return gate.released; });
        };
        job = Jobs::ScheduleBackgroundJob(desc);
        if (!job.id) throw std::runtime_error("Test background gate rejected");
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }))
        { released = true; condition.notify_all(); throw std::runtime_error("Test background gate did not run"); }
    }
    void Release() { std::lock_guard lock(mutex); released = true; condition.notify_all(); }
    ~BackgroundGate() { Release(); Jobs::Complete(job); }
private:
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
    Jobs::JobHandle job;
};
TEST(ECSStreaming, BackgroundParsingDoesNotTouchWorldAndOwnerCommitRemapsReferences)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    const auto handle = streaming.RequestLoad(1, files.Save("first.json"));
    EXPECT_EQ(world.GetEntityCount(), 0u); EXPECT_EQ(streaming.GetInfo(handle).status, SceneLoadStatus::Loading);
    const auto info = streaming.Wait(handle);
    ASSERT_EQ(info.status, SceneLoadStatus::Loaded) << info.error;
    EXPECT_TRUE(info.parsedOnBackgroundWorker); EXPECT_GT(info.parseMilliseconds, 0);
    const auto a = streaming.Find(1, "a"), b = streaming.Find(1, "b");
    ASSERT_TRUE(world.IsAlive(a)); EXPECT_EQ(world.ReadComponent<Saved>(a).target, b);
    EXPECT_EQ(world.ReadComponent<Saved>(b).target, a); EXPECT_EQ(info.entityCount, 2u);
    EXPECT_EQ(streaming.Unload(1), 2u); EXPECT_FALSE(world.IsAlive(a));
}
TEST(ECSStreaming, MultipleChunksRemapSameLocalIdsIndependentlyAndUnloadOnlyTheirInstances)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    const auto unrelated = world.CreateEntity(); world.SetScene(unrelated, 11);
    ArchiveLoadOptions first; first.sceneOwner = 11;
    ArchiveLoadOptions second; second.sceneOwner = 22;
    EXPECT_EQ(streaming.Wait(streaming.RequestLoad(101, files.Save("a.json"), first)).status, SceneLoadStatus::Loaded);
    EXPECT_EQ(streaming.Wait(streaming.RequestLoad(202, files.Save("b.json", 20), second)).status, SceneLoadStatus::Loaded);
    const auto a = streaming.Find(101, "a"), b = streaming.Find(202, "a");
    EXPECT_NE(a, b); EXPECT_EQ(world.GetScene(a), 11u); EXPECT_EQ(world.GetScene(b), 22u);
    EXPECT_EQ(world.ReadComponent<Saved>(b).target, streaming.Find(202, "b"));
    EXPECT_EQ(streaming.Unload(101), 2u); EXPECT_TRUE(world.IsAlive(unrelated)); EXPECT_TRUE(world.IsAlive(b));
    EXPECT_EQ(streaming.UnloadAll(), 2u); EXPECT_EQ(world.GetEntityCount(), 1u);
}
TEST(ECSStreaming, ReplacementKeepsOldChunkUntilSuccessfulCommitAndInvalidatesOldHandlesAfterwards)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    streaming.Wait(streaming.RequestLoad(1, files.Save("first.json"))); const auto old = streaming.Find(1, "a");
    const auto missing = streaming.Wait(streaming.RequestLoad(1, files.directory / "missing.json"));
    EXPECT_EQ(missing.status, SceneLoadStatus::Failed); EXPECT_FALSE(missing.error.empty()); EXPECT_EQ(streaming.Find(1, "a"), old);
    auto invalid = files.Document(); invalid.entities[1].components[0].value["value"] = "invalid";
    WorldArchive::WriteFile(files.directory / "invalid.json", invalid);
    const auto badDecode = streaming.Wait(streaming.RequestLoad(1, files.directory / "invalid.json"));
    EXPECT_EQ(badDecode.status, SceneLoadStatus::Failed); EXPECT_EQ(streaming.Find(1, "a"), old); EXPECT_EQ(world.GetEntityCount(), 2u);
    auto next = streaming.RequestLoad(1, files.Save("next.json", 90)); EXPECT_EQ(streaming.Find(1, "a"), old);
    ASSERT_EQ(streaming.Wait(next).status, SceneLoadStatus::Loaded); EXPECT_FALSE(world.IsAlive(old));
    EXPECT_EQ(world.ReadComponent<Saved>(streaming.Find(1, "a")).value, 90); EXPECT_EQ(world.GetEntityCount(), 2u);
}
TEST(ECSStreaming, NewRequestSupersedesQueuedOldGeneration)
{
    ECSTest::JobSystemScope jobs; Files files; const auto first = files.Save("first.json"), next = files.Save("next.json", 90);
    BackgroundGate gate; World world; Register(world); SceneStreaming streaming(world);
    const auto old = streaming.RequestLoad(1, first), current = streaming.RequestLoad(1, next);
    EXPECT_EQ(streaming.GetInfo(old).status, SceneLoadStatus::Cancelled); EXPECT_TRUE(streaming.Forget(old));
    gate.Release(); ASSERT_EQ(streaming.Wait(current).status, SceneLoadStatus::Loaded);
    EXPECT_EQ(world.ReadComponent<Saved>(streaming.Find(1, "a")).value, 90); EXPECT_EQ(world.GetEntityCount(), 2u);
}
TEST(ECSStreaming, UnloadCancelsPendingReplacementAndCannotResurrectChunk)
{
    ECSTest::JobSystemScope jobs; Files files; const auto path = files.Save("scene.json");
    World world; Register(world); SceneStreaming streaming(world); streaming.Wait(streaming.RequestLoad(1, path));
    BackgroundGate gate; const auto pending = streaming.RequestLoad(1, path);
    EXPECT_EQ(streaming.Unload(1), 2u); EXPECT_EQ(streaming.GetInfo(pending).status, SceneLoadStatus::Cancelled);
    gate.Release(); EXPECT_EQ(streaming.Pump(), 0u); EXPECT_EQ(world.GetEntityCount(), 0u);
}
TEST(ECSStreaming, CancelAndDestructionLeaveBackgroundJobsIndependentOfWorldAndService)
{
    ECSTest::JobSystemScope jobs; Files files; const auto path = files.Save("scene.json"); BackgroundGate gate;
    {
        World world; Register(world); SceneStreaming streaming(world);
        const auto pending = streaming.RequestLoad(1, path); EXPECT_TRUE(streaming.Cancel(pending)); EXPECT_TRUE(streaming.Forget(pending));
        streaming.RequestLoad(2, path); // Manager and World die while parsing is queued.
    }
    gate.Release(); // Real Jobs shutdown drains the remaining owned parse payloads.
}
TEST(ECSStreaming, PumpLimitsSuccessfulChunkCommitsAndDoesNotRequireRunningQueueAfterParse)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    streaming.RequestLoad(1, files.Save("a.json")); streaming.RequestLoad(2, files.Save("b.json"));
    Jobs::ShutdownJobSystem(); // Finish parsing without invoking any World callback.
    EXPECT_EQ(world.GetEntityCount(), 0u); EXPECT_EQ(streaming.Pump(1), 1u); EXPECT_EQ(world.GetEntityCount(), 2u);
    EXPECT_EQ(streaming.Pump(1), 1u); EXPECT_EQ(world.GetEntityCount(), 4u); EXPECT_EQ(streaming.Pump(), 0u);
}
TEST(ECSStreaming, ResourceResolutionRunsOnOwnerAndFailurePreservesLoadedChunk)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    streaming.Wait(streaming.RequestLoad(1, files.Save("old.json"))); const auto old = streaming.Find(1, "a");
    auto document = files.Document(50); document.resources.push_back({"12345678-1234-1234-1234-123456789abc", "version-1"});
    const auto path = files.directory / "resource.json"; WorldArchive::WriteFile(path, document);
    ArchiveLoadOptions options; const auto owner = std::this_thread::get_id(); bool called = false;
    options.resolveResource = [&](const ResourceDependency& resource)
    { EXPECT_EQ(std::this_thread::get_id(), owner); EXPECT_EQ(resource.hashOrVersion, "version-1"); called = true; return false; };
    EXPECT_EQ(streaming.Wait(streaming.RequestLoad(1, path, options)).status, SceneLoadStatus::Failed);
    EXPECT_TRUE(called); EXPECT_EQ(streaming.Find(1, "a"), old); EXPECT_EQ(world.GetEntityCount(), 2u);
    options.resolveResource = [](const auto&) { return true; };
    EXPECT_EQ(streaming.Wait(streaming.RequestLoad(1, path, options)).status, SceneLoadStatus::Loaded);
}
TEST(ECSStreaming, CancellationInResourceCallbackRollsBackCandidateAndPreservesOldChunk)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    streaming.Wait(streaming.RequestLoad(1, files.Save("old.json"))); const auto old = streaming.Find(1, "a");
    auto document = files.Document(); document.resources.push_back({"12345678-1234-1234-1234-123456789abc", "v1"});
    const auto path = files.directory / "resource.json"; WorldArchive::WriteFile(path, document);
    SceneLoadHandle pending; ArchiveLoadOptions options;
    options.resolveResource = [&](const auto&) { EXPECT_TRUE(streaming.Cancel(pending)); return true; };
    pending = streaming.RequestLoad(1, path, options);
    EXPECT_EQ(streaming.Wait(pending).status, SceneLoadStatus::Cancelled); EXPECT_EQ(streaming.Find(1, "a"), old);
    EXPECT_EQ(world.GetEntityCount(), 2u);
}
TEST(ECSStreaming, WrongThreadAndIterationCommitAreRejectedWithoutLosingRequest)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    const auto unrelated = world.CreateEntity(); world.AddComponent<Saved>(unrelated);
    const auto pending = streaming.RequestLoad(1, files.Save("scene.json"));
    std::atomic<bool> rejected = false;
    std::thread thread([&] { try { streaming.Pump(); } catch (const std::logic_error&) { rejected = true; } }); thread.join();
    EXPECT_TRUE(rejected); world.Query<Read<Saved>>().Each([&](Entity, const Saved&) { EXPECT_THROW(streaming.Pump(), std::logic_error); });
    EXPECT_EQ(streaming.Wait(pending).status, SceneLoadStatus::Loaded); EXPECT_EQ(world.GetEntityCount(), 3u);
}
TEST(ECSStreaming, ForeignHandlesCannotCancelOrForgetOtherRequestsAndHistoryIsBounded)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming first(world, 1), second(world, 1);
    const auto path = files.Save("scene.json"), missing = files.directory / "missing.json";
    const auto a = first.RequestLoad(1, path), b = second.RequestLoad(1, path);
    EXPECT_THROW(second.Cancel(a), std::invalid_argument); EXPECT_FALSE(second.Forget(a)); EXPECT_NE(a, b);
    EXPECT_THROW(first.RequestLoad(2, missing), std::length_error);
    EXPECT_EQ(first.Wait(a).status, SceneLoadStatus::Loaded); EXPECT_TRUE(first.Forget(a));
    EXPECT_THROW(first.GetInfo(a), std::invalid_argument); EXPECT_EQ(first.Wait(first.RequestLoad(2, missing)).status, SceneLoadStatus::Failed);
    second.Cancel(b);
}
TEST(ECSStreaming, ClosedWorldIsDetectedBeforeCommitAndQueuedDataCanBeCancelled)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    const auto pending = streaming.RequestLoad(1, files.Save("scene.json")); world.Close();
    EXPECT_THROW(streaming.Pump(), std::logic_error);
    EXPECT_THROW(streaming.Find(1, "a"), std::logic_error);
    EXPECT_TRUE(streaming.Cancel(pending)); EXPECT_TRUE(streaming.Forget(pending));
}
TEST(ECSStreaming, WrongDocumentKindAndArchiveLimitsAreRejected)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    auto prefab = files.Document(); prefab.kind = DocumentKind::Prefab; const auto path = files.directory / "prefab.json";
    WorldArchive::WriteFile(path, prefab); EXPECT_EQ(streaming.Wait(streaming.RequestLoad(1, path)).status, SceneLoadStatus::Failed);
    ArchiveLoadOptions options; options.limits.maxBytes = 16;
    EXPECT_EQ(streaming.Wait(streaming.RequestLoad(1, path, options)).status, SceneLoadStatus::Failed); EXPECT_EQ(world.GetEntityCount(), 0u);
}
TEST(ECSStreaming, WorldCancellationPreventsPendingChunksFromBeingCommitted)
{
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    const auto pending = streaming.RequestLoad(1, files.Save("scene.json")); world.RequestCancellation();
    EXPECT_EQ(streaming.Pump(), 0u); EXPECT_EQ(streaming.GetInfo(pending).status, SceneLoadStatus::Cancelled);
    world.ClearCancellation(); EXPECT_EQ(streaming.Wait(pending).status, SceneLoadStatus::Cancelled); EXPECT_EQ(world.GetEntityCount(), 0u);
    auto document = files.Document(); document.resources.push_back({"12345678-1234-1234-1234-123456789abc", "v1"});
    const auto path = files.directory / "cancel-during-commit.json"; WorldArchive::WriteFile(path, document);
    ArchiveLoadOptions options; options.resolveResource = [&](const auto&) { world.RequestCancellation(); return true; };
    EXPECT_EQ(streaming.Wait(streaming.RequestLoad(1, path, options)).status, SceneLoadStatus::Cancelled);
    EXPECT_EQ(world.GetEntityCount(), 0u); world.ClearCancellation();
}
TEST(ECSStreaming, OwnedDocumentSourceReadsOnBackgroundWorkerAndSurvivesCallerRelease)
{
    class Source final : public IWorldDocumentSource
    {
    public:
        std::string text;
        std::thread::id owner;
        WorldDocument Read(ArchiveLimits limits) const override
        {
            EXPECT_TRUE(Jobs::Internal::IsExecutingBackgroundJobWorkerThread());
            EXPECT_NE(std::this_thread::get_id(), owner);
            return WorldArchive::Parse(text, limits);
        }
    };
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    auto source = std::make_shared<Source>(); source->text = WorldArchive::Encode(files.Document()); source->owner = std::this_thread::get_id();
    const auto pending = streaming.RequestLoadSource(1, source); source.reset();
    ASSERT_EQ(streaming.Wait(pending).status, SceneLoadStatus::Loaded);
    EXPECT_EQ(world.ReadComponent<Saved>(streaming.Find(1, "a")).target, streaming.Find(1, "b"));
}
TEST(ECSStreaming, DocumentSourceReceivesArchiveLimitsAndNullSourcesAreRejected)
{
    class Source final : public IWorldDocumentSource
    {
    public:
        std::string text;
        WorldDocument Read(ArchiveLimits limits) const override { return WorldArchive::Parse(text, limits); }
    };
    ECSTest::JobSystemScope jobs; Files files; World world; Register(world); SceneStreaming streaming(world);
    auto source = std::make_shared<Source>(); source->text = WorldArchive::Encode(files.Document());
    ArchiveLoadOptions options; options.limits.maxBytes = 16;
    EXPECT_EQ(streaming.Wait(streaming.RequestLoadSource(1, source, options)).status, SceneLoadStatus::Failed);
    EXPECT_THROW(streaming.RequestLoadSource(1, nullptr), std::invalid_argument);
    EXPECT_EQ(world.GetEntityCount(), 0u);
}
}
