#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "Core/EditorBackgroundTaskTracker.h"
#include "Jobs/JobSystem.h"

namespace
{
    std::string ReadRepoFile(const std::filesystem::path& relativePath)
    {
        const auto absolutePath = std::filesystem::path(NLS_ROOT_DIR) / relativePath;
        std::ifstream input(absolutePath, std::ios::binary);
        if (!input)
            return {};

        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
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

TEST_F(JobSystemMigrationBehaviorTests, EditorBackgroundTaskTrackerRejectsAfterStopAndHonorsCapacity)
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

    EXPECT_FALSE(tracker.Track([] {}));
    release.store(true, std::memory_order_release);
    tracker.StopAndComplete();
    EXPECT_FALSE(tracker.Track([] {}));
}

TEST(JobSystemMigrationTests, EditorActionsTracksBackgroundTasksThroughJobSystem)
{
    const auto source = ReadRepoFile("Project/Editor/Core/EditorActions.cpp");
    ASSERT_FALSE(source.empty());

    const auto trackBackgroundTask = source.find("bool Editor::Core::EditorActions::TrackBackgroundTask");
    ASSERT_NE(trackBackgroundTask, std::string::npos);

    const auto nextMethod = source.find("\nvoid Editor::Core::EditorActions::", trackBackgroundTask + 1u);
    const auto methodBody = source.substr(
        trackBackgroundTask,
        nextMethod == std::string::npos ? std::string::npos : nextMethod - trackBackgroundTask);

    EXPECT_NE(methodBody.find("m_backgroundTasks.Track"), std::string::npos);
    EXPECT_EQ(methodBody.find("std::thread"), std::string::npos);
    EXPECT_EQ(methodBody.find("m_backgroundTaskQueue"), std::string::npos);
}

TEST(JobSystemMigrationTests, EditorActionsRequiresSharedJobSystemOwner)
{
    const auto source = ReadRepoFile("Project/Editor/Core/EditorActions.cpp");
    ASSERT_FALSE(source.empty());

    const auto trackBackgroundTask = source.find("bool Editor::Core::EditorActions::TrackBackgroundTask");
    ASSERT_NE(trackBackgroundTask, std::string::npos);

    const auto nextMethod = source.find("\nvoid Editor::Core::EditorActions::", trackBackgroundTask + 1u);
    const auto methodBody = source.substr(
        trackBackgroundTask,
        nextMethod == std::string::npos ? std::string::npos : nextMethod - trackBackgroundTask);

    const auto trackerSource = ReadRepoFile("Project/Editor/Core/EditorBackgroundTaskTracker.cpp");
    ASSERT_FALSE(trackerSource.empty());

    EXPECT_NE(trackerSource.find("NLS::Base::Jobs::IsJobSystemInitialized()"), std::string::npos);
    EXPECT_EQ(methodBody.find("TryInitializeJobSystem"), std::string::npos);
    EXPECT_EQ(methodBody.find("ShutdownJobSystem"), std::string::npos);
}

TEST(JobSystemMigrationTests, RuntimeTestHookDefinitionsPropagateToExecutableConsumers)
{
    const auto baseCmake = ReadRepoFile("Runtime/Base/CMakeLists.txt");
    const auto coreCmake = ReadRepoFile("Runtime/Core/CMakeLists.txt");
    const auto renderCmake = ReadRepoFile("Runtime/Rendering/CMakeLists.txt");
    ASSERT_FALSE(baseCmake.empty());
    ASSERT_FALSE(coreCmake.empty());
    ASSERT_FALSE(renderCmake.empty());

    const auto requiresPublicTestHooks = [](const std::string& cmake, const std::string& target)
    {
        const std::string marker = "target_compile_definitions(" + target + " PUBLIC";
        size_t searchOffset = 0u;
        while (true)
        {
            const auto definition = cmake.find(marker, searchOffset);
            if (definition == std::string::npos)
                return false;

            const auto blockEnd = cmake.find("\n)", definition);
            if (blockEnd == std::string::npos)
                return false;

            if (cmake.substr(definition, blockEnd - definition).find("NLS_ENABLE_TEST_HOOKS") != std::string::npos)
                return true;

            searchOffset = blockEnd + 2u;
        }
    };

    EXPECT_TRUE(requiresPublicTestHooks(baseCmake, "NLS_Base"));
    EXPECT_TRUE(requiresPublicTestHooks(coreCmake, "NLS_Core"));
    EXPECT_TRUE(requiresPublicTestHooks(renderCmake, "NLS_Render"));
}

TEST(JobSystemMigrationTests, RuntimeTestHooksDoNotChangeArtifactDatabaseObjectLayout)
{
    const auto header = ReadRepoFile("Runtime/Core/Assets/ArtifactDatabase.h");
    ASSERT_FALSE(header.empty());

    const auto member = header.find("m_indexRebuildCountForTesting");
    ASSERT_NE(member, std::string::npos);

    const auto previousGuard = header.rfind("#if defined(NLS_ENABLE_TEST_HOOKS)", member);
    const auto previousEndif = header.rfind("#endif", member);
    EXPECT_TRUE(previousGuard == std::string::npos || (previousEndif != std::string::npos && previousEndif > previousGuard))
        << "Test hook declarations may be conditional, but ArtifactDatabase data members must keep a stable ABI.";
}

TEST(JobSystemMigrationTests, EditorActionsNoLongerOwnsPrivateBackgroundWorkerThreads)
{
    const auto header = ReadRepoFile("Project/Editor/Core/EditorActions.h");
    const auto source = ReadRepoFile("Project/Editor/Core/EditorActions.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(header.find("BackgroundWorker"), std::string::npos);
    EXPECT_EQ(header.find("m_backgroundWorkers"), std::string::npos);
    EXPECT_EQ(header.find("m_backgroundTaskQueue"), std::string::npos);
    EXPECT_EQ(header.find("m_backgroundTaskCondition"), std::string::npos);
    EXPECT_EQ(header.find("EnsureBackgroundWorkersStarted"), std::string::npos);
    EXPECT_EQ(header.find("RunBackgroundWorker"), std::string::npos);

    EXPECT_EQ(source.find("EnsureBackgroundWorkersStarted"), std::string::npos);
    EXPECT_EQ(source.find("RunBackgroundWorker"), std::string::npos);
    EXPECT_EQ(source.find("std::thread([this] { RunBackgroundWorker(); })"), std::string::npos);
}

TEST(JobSystemMigrationTests, EditorActionsDestructorDoesNotDiscardPrivateBackgroundQueue)
{
    const auto source = ReadRepoFile("Project/Editor/Core/EditorActions.cpp");
    ASSERT_FALSE(source.empty());

    const auto destructor = source.find("Editor::Core::EditorActions::~EditorActions()");
    ASSERT_NE(destructor, std::string::npos);

    const auto nextMethod = source.find("\nvoid Editor::Core::EditorActions::", destructor + 1u);
    const auto destructorBody = source.substr(
        destructor,
        nextMethod == std::string::npos ? std::string::npos : nextMethod - destructor);

    EXPECT_EQ(destructorBody.find("m_backgroundTaskQueue"), std::string::npos);
    EXPECT_EQ(destructorBody.find("m_stopBackgroundWorkers"), std::string::npos);
    EXPECT_EQ(destructorBody.find("join"), std::string::npos);
}

TEST(JobSystemMigrationTests, EditorActionsTracksJobSystemBackgroundHandlesForDestructorCompletion)
{
    const auto header = ReadRepoFile("Project/Editor/Core/EditorActions.h");
    const auto source = ReadRepoFile("Project/Editor/Core/EditorActions.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("EditorBackgroundTaskTracker m_backgroundTasks"), std::string::npos);

    const auto trackBackgroundTask = source.find("bool Editor::Core::EditorActions::TrackBackgroundTask");
    ASSERT_NE(trackBackgroundTask, std::string::npos);
    const auto nextTrackMethod = source.find("\nvoid Editor::Core::EditorActions::", trackBackgroundTask + 1u);
    const auto methodBody = source.substr(
        trackBackgroundTask,
        nextTrackMethod == std::string::npos ? std::string::npos : nextTrackMethod - trackBackgroundTask);

    EXPECT_NE(methodBody.find("m_backgroundTasks.Track"), std::string::npos);

    const auto destructor = source.find("Editor::Core::EditorActions::~EditorActions()");
    ASSERT_NE(destructor, std::string::npos);
    const auto nextDestructorMethod = source.find("\nvoid Editor::Core::EditorActions::", destructor + 1u);
    const auto destructorBody = source.substr(
        destructor,
        nextDestructorMethod == std::string::npos ? std::string::npos : nextDestructorMethod - destructor);

    EXPECT_NE(destructorBody.find("m_backgroundTasks.StopAndComplete()"), std::string::npos);
    EXPECT_EQ(destructorBody.find("ShutdownJobSystem"), std::string::npos);

    const auto trackerSource = ReadRepoFile("Project/Editor/Core/EditorBackgroundTaskTracker.cpp");
    ASSERT_FALSE(trackerSource.empty());
    EXPECT_NE(trackerSource.find("m_trackedTasks.size() >= m_capacity"), std::string::npos);
    EXPECT_NE(trackerSource.find("NLS::Base::Jobs::Complete(handle)"), std::string::npos);
}

TEST(JobSystemMigrationTests, EditorBackgroundTaskTrackerPayloadOwnershipIsClaimedOnce)
{
    const auto trackerSource = ReadRepoFile("Project/Editor/Core/EditorBackgroundTaskTracker.cpp");
    ASSERT_FALSE(trackerSource.empty());

    EXPECT_NE(trackerSource.find("#include <atomic>"), std::string::npos);
    EXPECT_NE(trackerSource.find("std::atomic<bool> claimed"), std::string::npos);
    EXPECT_NE(trackerSource.find("claimed.exchange(true"), std::string::npos);
    EXPECT_NE(trackerSource.find("std::unique_ptr<TaskPayload> payload(rawPayload)"), std::string::npos);
    EXPECT_EQ(trackerSource.find("delete static_cast<TaskPayload*>(userData);"), std::string::npos);
}

TEST(JobSystemMigrationTests, EditorOwnsSharedJobSystemLifetime)
{
    const auto header = ReadRepoFile("Project/Editor/Core/Editor.h");
    const auto source = ReadRepoFile("Project/Editor/Core/Editor.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("struct JobSystemLifetime"), std::string::npos);
    EXPECT_NE(header.find("JobSystemLifetime m_jobSystemLifetime"), std::string::npos);
    EXPECT_NE(source.find("Editor::Core::Editor::JobSystemLifetime::JobSystemLifetime()"), std::string::npos);
    EXPECT_NE(source.find("NLS::Base::Jobs::TryInitializeJobSystem(config)"), std::string::npos);
    EXPECT_NE(source.find("Editor::Core::Editor::JobSystemLifetime::~JobSystemLifetime()"), std::string::npos);
    EXPECT_NE(source.find("NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork)"), std::string::npos);
}

TEST(JobSystemMigrationTests, EditorUpdateDrainsJobSystemMainThreadContinuations)
{
    const auto source = ReadRepoFile("Project/Editor/Core/Editor.cpp");
    ASSERT_FALSE(source.empty());

    const auto update = source.find("void Editor::Core::Editor::Update(float p_deltaTime)");
    ASSERT_NE(update, std::string::npos);
    const auto nextMethod = source.find("\nvoid Editor::Core::Editor::RefreshProfilerRecordingState", update + 1u);
    ASSERT_NE(nextMethod, std::string::npos);
    const auto updateBody = source.substr(update, nextMethod - update);

    EXPECT_NE(updateBody.find("m_editorActions.ExecuteDelayedActions()"), std::string::npos);
    EXPECT_NE(updateBody.find("kEditorMainThreadContinuationDrainBudget"), std::string::npos);
    EXPECT_NE(
        updateBody.find("NLS::Base::Jobs::DrainMainThreadContinuations(kEditorMainThreadContinuationDrainBudget)"),
        std::string::npos);
    EXPECT_GT(
        updateBody.find("NLS::Base::Jobs::DrainMainThreadContinuations(kEditorMainThreadContinuationDrainBudget)"),
        updateBody.find("m_editorActions.ExecuteDelayedActions()"));
}
