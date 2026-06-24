#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "Core/EditorBackgroundTaskTracker.h"
#include "Core/RecentBackgroundWorkGate.h"
#include "Debug/Logger.h"
#include "Jobs/JobSystem.h"

namespace
{
    std::filesystem::path RepoPath(std::string_view relativePath)
    {
        return std::filesystem::path(NLS_ROOT_DIR) / std::filesystem::path(relativePath);
    }

    std::string ReadSourceText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input.is_open()) << "Failed to open source file: " << path.string();
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }
}

class JobSystemMigrationBehaviorTests : public ::testing::Test
{
protected:
    void TearDown() override
    {
        NLS::Base::Jobs::ShutdownJobSystem();
#if defined(NLS_ENABLE_TEST_HOOKS)
        NLS::Base::Jobs::ResetJobSystemForTesting();
#endif
    }
};

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerRunsTasksAndDrainsOnDestruction)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<int> counter = 0;
    {
        NLS::Editor::Core::EditorBackgroundTaskTracker tracker(4u);
        EXPECT_TRUE(tracker.Track([&counter] { counter.fetch_add(1, std::memory_order_acq_rel); }));
        EXPECT_EQ(counter.load(std::memory_order_acquire), 0);
    }

    EXPECT_EQ(counter.load(std::memory_order_acquire), 1);
}

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerQueuesRequiredWorkPastSoftCapacityAndRejectsAfterStop)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> release = false;
    std::atomic<int> started = 0;
    std::atomic<int> secondStarted = 0;
    NLS::Editor::Core::EditorBackgroundTaskTracker tracker(1u);
    EXPECT_TRUE(tracker.Track(
        [&]
        {
            started.fetch_add(1, std::memory_order_acq_rel);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        }));

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    EXPECT_TRUE(tracker.Track(
        [&]
        {
            secondStarted.fetch_add(1, std::memory_order_acq_rel);
        }));
    release.store(true, std::memory_order_release);
    tracker.StopAndComplete();
    EXPECT_EQ(secondStarted.load(std::memory_order_acquire), 1);
    EXPECT_FALSE(tracker.Track([] {}));
}

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerRejectsRequiredWorkAtHardCapacity)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> release = false;
    std::atomic<int> started = 0;
    std::atomic<int> completed = 0;
    NLS::Editor::Core::EditorBackgroundTaskTracker tracker(1u, 2u);
    const auto blockingTask = [&]
    {
        started.fetch_add(1, std::memory_order_acq_rel);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::yield();
        completed.fetch_add(1, std::memory_order_acq_rel);
    };

    EXPECT_TRUE(tracker.Track(blockingTask));
    EXPECT_TRUE(tracker.Track(blockingTask));
    EXPECT_FALSE(tracker.Track([] {}))
        << "Required editor work may exceed the soft capacity, but the tracker must still enforce a hard bound.";

    release.store(true, std::memory_order_release);
    tracker.StopAndComplete();
    EXPECT_EQ(started.load(std::memory_order_acquire), 2);
    EXPECT_EQ(completed.load(std::memory_order_acquire), 2);
}

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerRejectsOpportunisticWorkWithoutQueueFullWarning)
{
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = 0u;
    config.backgroundWorkerCount = 1u;
    ASSERT_TRUE(NLS::Base::Jobs::InitializeJobSystem(config));

    std::atomic<bool> release = false;
    std::atomic<int> started = 0;
    NLS::Editor::Core::EditorBackgroundTaskTracker tracker(1u);
    EXPECT_TRUE(tracker.Track(
        [&]
        {
            started.fetch_add(1, std::memory_order_acq_rel);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::yield();
        }));

    for (int attempt = 0; attempt < 200 && started.load(std::memory_order_acquire) == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(started.load(std::memory_order_acquire), 1);

    size_t queueCapacityWarnings = 0u;
    const auto listener = NLS::Debug::Logger::LogEvent +=
        [&queueCapacityWarnings](const NLS::Debug::LogData& log)
        {
            const bool editorBackgroundQueueWarning =
                log.message.find("Editor background task queue") != std::string::npos ||
                log.message.find("Editor background task rejected by the shared JobSystem") != std::string::npos;
            if (log.logLevel == NLS::Debug::ELogLevel::LOG_WARNING && editorBackgroundQueueWarning)
            {
                ++queueCapacityWarnings;
            }
        };

    EXPECT_FALSE(tracker.TrackOpportunistic([] {}));
    NLS::Debug::Logger::LogEvent -= listener;

    release.store(true, std::memory_order_release);
    tracker.StopAndComplete();
    EXPECT_EQ(queueCapacityWarnings, 0u);
}

TEST_F(JobSystemMigrationBehaviorTests, RecentBackgroundWorkGateSuppressesCompletedKeysUntilTtlExpires)
{
    using Gate = NLS::Editor::Core::RecentBackgroundWorkGate;
    const auto ttl = std::chrono::seconds(3);
    Gate gate(8u, ttl);

    const auto start = Gate::Clock::time_point(std::chrono::seconds(10));
    EXPECT_TRUE(gate.TryBegin("prefab-a", start));
    EXPECT_TRUE(gate.IsInFlight("prefab-a"));
    EXPECT_FALSE(gate.TryBegin("prefab-a", start + std::chrono::milliseconds(1)));

    gate.End("prefab-a");
    EXPECT_FALSE(gate.IsInFlight("prefab-a"));
    EXPECT_FALSE(gate.TryBegin("prefab-a", start + std::chrono::seconds(2)));
    EXPECT_TRUE(gate.TryBegin("prefab-a", start + ttl + std::chrono::milliseconds(1)));
}

TEST_F(JobSystemMigrationBehaviorTests, RecentBackgroundWorkGatePrunesToCapacity)
{
    using Gate = NLS::Editor::Core::RecentBackgroundWorkGate;
    Gate gate(2u, std::chrono::minutes(5));

    const auto start = Gate::Clock::time_point(std::chrono::seconds(20));
    EXPECT_TRUE(gate.TryBegin("prefab-a", start));
    gate.End("prefab-a");
    EXPECT_TRUE(gate.TryBegin("prefab-b", start + std::chrono::milliseconds(1)));
    gate.End("prefab-b");
    EXPECT_TRUE(gate.TryBegin("prefab-c", start + std::chrono::milliseconds(2)));
    gate.End("prefab-c");

    EXPECT_LE(gate.EntryCountForTesting(), 2u);
    EXPECT_TRUE(gate.TryBegin("prefab-a", start + std::chrono::milliseconds(3)));
}

TEST_F(JobSystemMigrationBehaviorTests, RecentBackgroundWorkGateCompletionReleasesInFlightWhenScopeThrows)
{
    using Gate = NLS::Editor::Core::RecentBackgroundWorkGate;
    const auto ttl = std::chrono::seconds(3);
    Gate gate(8u, ttl);

    const auto start = Gate::Clock::time_point(std::chrono::seconds(30));
    ASSERT_TRUE(gate.TryBegin("prefab-a", start));
    EXPECT_TRUE(gate.IsInFlight("prefab-a"));

    try
    {
        auto completion = gate.CompleteOnScopeExit("prefab-a");
        throw std::runtime_error("simulated preload failure");
    }
    catch (const std::runtime_error&)
    {
    }

    EXPECT_FALSE(gate.IsInFlight("prefab-a"));
    EXPECT_FALSE(gate.TryBegin("prefab-a", start + std::chrono::seconds(1)));
    EXPECT_TRUE(gate.TryBegin("prefab-a", start + ttl + std::chrono::milliseconds(1)));
}

TEST_F(JobSystemMigrationBehaviorTests, ResourceManagersScheduleAsyncArtifactLoadsOnBackgroundJobQueue)
{
    struct ManagerSourceExpectation
    {
        std::filesystem::path path;
        std::string requestSymbol;
        std::string startSymbol;
        std::string loaderSymbol;
    };

    const ManagerSourceExpectation managerPaths[] = {
        {
            RepoPath("Runtime/Rendering/ResourceManagement/MeshManager.cpp"),
            "MeshManager::Mesh* MeshManager::RequestAsyncArtifact",
            "StartMeshArtifactLoad(",
            "LoadMeshArtifact("
        },
        {
            RepoPath("Runtime/Rendering/ResourceManagement/MaterialManager.cpp"),
            "Material* MaterialManager::RequestAsyncArtifact",
            "StartMaterialArtifactLoad(",
            "ReadSerializedPayload("
        },
        {
            RepoPath("Runtime/Rendering/ResourceManagement/TextureManager.cpp"),
            "Texture2D* TextureManager::RequestAsyncArtifact",
            "StartTextureArtifactLoad(",
            "LoadTextureArtifact("
        },
    };

    for (const auto& manager : managerPaths)
    {
        const auto source = ReadSourceText(manager.path);
        EXPECT_NE(source.find("ScheduleBackgroundJob"), std::string::npos)
            << manager.path.string() << " must route async artifact IO through the unified JobSystem executor.";
        EXPECT_EQ(source.find("std::thread"), std::string::npos)
            << manager.path.string() << " must not spawn unmanaged asset loading threads.";
        EXPECT_EQ(source.find(".detach()"), std::string::npos)
            << manager.path.string() << " must not detach unmanaged asset loading threads.";
        EXPECT_EQ(source.find("std::packaged_task"), std::string::npos)
            << manager.path.string() << " should use JobSystem payloads rather than detached packaged_task workers.";
        EXPECT_EQ(source.find("std::async"), std::string::npos)
            << manager.path.string() << " must not route artifact IO through std::async.";
        EXPECT_EQ(source.find("std::launch::async"), std::string::npos)
            << manager.path.string() << " must not route artifact IO through std::launch::async.";

        const auto requestOffset = source.find(manager.requestSymbol);
        ASSERT_NE(requestOffset, std::string::npos)
            << manager.path.string() << " must keep RequestAsyncArtifact discoverable by the migration guard.";
        const auto startOffset = source.find(manager.startSymbol);
        ASSERT_NE(startOffset, std::string::npos)
            << manager.path.string() << " must keep the async artifact submission helper discoverable by the migration guard.";
        ASSERT_LT(startOffset, requestOffset)
            << manager.path.string() << " should define the JobSystem submission helper before RequestAsyncArtifact.";
        const auto helperBody = source.substr(startOffset, requestOffset - startOffset);
        EXPECT_NE(helperBody.find("ScheduleBackgroundJob"), std::string::npos)
            << manager.path.string() << " async artifact submission helper must submit artifact work to JobSystem.";
        const auto invalidHandleOffset = helperBody.find("if (handle.id == 0u)");
        ASSERT_NE(invalidHandleOffset, std::string::npos)
            << manager.path.string() << " must keep scheduling failure handling explicit.";
        const auto invalidHandleReturnOffset = helperBody.find("return {};", invalidHandleOffset);
        ASSERT_NE(invalidHandleReturnOffset, std::string::npos)
            << manager.path.string() << " scheduling failure path must return without sync fallback.";
        EXPECT_EQ(
            helperBody.substr(invalidHandleOffset, invalidHandleReturnOffset - invalidHandleOffset).find(manager.loaderSymbol),
            std::string::npos)
            << manager.path.string() << " scheduling failure path must not synchronously load artifacts.";

        const auto cancelOffset = source.find("CancelAsyncArtifact", requestOffset);
        ASSERT_NE(cancelOffset, std::string::npos)
            << manager.path.string() << " must keep RequestAsyncArtifact bounded for source-guard validation.";
        const auto requestBody = source.substr(requestOffset, cancelOffset - requestOffset);
        EXPECT_NE(requestBody.find(manager.startSymbol), std::string::npos)
            << manager.path.string() << " RequestAsyncArtifact path must submit through the JobSystem helper.";
        EXPECT_EQ(requestBody.find(manager.loaderSymbol), std::string::npos)
            << manager.path.string() << " RequestAsyncArtifact path must not synchronously load artifacts as a scheduling fallback.";
    }
}

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundAssetWorkUsesUnifiedJobSystemExecutor)
{
    const std::filesystem::path editorPaths[] = {
        RepoPath("Project/Editor/Assets/AssetThumbnailService.cpp"),
        RepoPath("Project/Editor/Panels/AssetBrowser.cpp"),
    };

    for (const auto& editorPath : editorPaths)
    {
        const auto source = ReadSourceText(editorPath);
        EXPECT_NE(source.find("ScheduleBackgroundJob"), std::string::npos)
            << editorPath.string() << " must route editor asset background work through the unified JobSystem executor.";
        EXPECT_EQ(source.find("std::async"), std::string::npos)
            << editorPath.string() << " must not route editor asset work through std::async.";
        EXPECT_EQ(source.find("std::launch::async"), std::string::npos)
            << editorPath.string() << " must not route editor asset work through std::launch::async.";
        EXPECT_EQ(source.find("std::thread"), std::string::npos)
            << editorPath.string() << " must not spawn unmanaged editor asset worker threads.";
        EXPECT_EQ(source.find(".detach()"), std::string::npos)
            << editorPath.string() << " must not detach unmanaged editor asset worker threads.";
    }
}
