#include <gtest/gtest.h>

#include "Assets/ArtifactWriter.h"
#include "Assets/ImportProgressTracker.h"
#include "Assets/EditorAssetDatabase.h"
#include "Guid.h"

#include <algorithm>
#include <barrier>
#include <mutex>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
using NLS::Core::Assets::AssetId;
using NLS::Editor::Assets::ImportJobId;
using NLS::Editor::Assets::ImportJobTerminalStatus;
using NLS::Editor::Assets::ImportPhase;
using NLS::Editor::Assets::ImportProgressTracker;

AssetId Id(const char* guid)
{
    return AssetId(NLS::Guid::Parse(guid));
}

std::filesystem::path MakeImportProgressRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_import_progress_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    return root;
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool HasPhase(
    const std::vector<NLS::Editor::Assets::ImportProgressEvent>& events,
    ImportPhase phase)
{
    return std::any_of(
        events.begin(),
        events.end(),
        [phase](const NLS::Editor::Assets::ImportProgressEvent& event)
        {
            return event.phase == phase;
        });
}

bool HasCommand(
    const std::vector<NLS::Editor::Assets::EditorAssetCommandDescriptor>& commands,
    const std::string& commandId,
    const bool enabled)
{
    return std::any_of(
        commands.begin(),
        commands.end(),
        [&commandId, enabled](const NLS::Editor::Assets::EditorAssetCommandDescriptor& command)
        {
            return command.commandId == commandId && command.enabled == enabled;
        });
}
}

TEST(AssetImportProgressTests, RecordsImportPhaseProgressEventsAndSubscriptions)
{
    ImportProgressTracker tracker;
    std::vector<NLS::Editor::Assets::ImportProgressEvent> observed;
    tracker.Subscribe([&observed](const auto& event)
    {
        observed.push_back(event);
    });

    const auto asset = Id("d2010101-0101-4101-8101-010101010101");
    const auto job = tracker.BeginJob(asset, "Assets/Models/Hero.gltf", "editor", 2u);
    tracker.ReportProgress(job, ImportPhase::DependencyCopy, 0.1, "Copy dependencies");
    tracker.ReportProgress(job, ImportPhase::SourceParse, 0.25, "Parse source");
    tracker.ReportProgress(job, ImportPhase::IntermediateConversion, 0.5, "Convert scene");
    tracker.ReportProgress(job, ImportPhase::ArtifactWrite, 0.75, "Write artifacts");
    tracker.ReportProgress(job, ImportPhase::Postprocess, 0.9, "Postprocess");
    tracker.ReportProgress(job, ImportPhase::Commit, 1.0, "Commit");

    const auto current = tracker.GetCurrentEvent(job);
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->phase, ImportPhase::Commit);
    EXPECT_EQ(current->message, "Commit");

    const auto active = tracker.GetActiveEvent();
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->jobId, job);

    tracker.FinishJob(job, ImportJobTerminalStatus::Succeeded, {});

    const auto events = tracker.GetEvents(job);
    ASSERT_EQ(events.size(), 8u);
    EXPECT_EQ(observed.size(), events.size());
    EXPECT_TRUE(HasPhase(events, ImportPhase::Queued));
    EXPECT_TRUE(HasPhase(events, ImportPhase::DependencyCopy));
    EXPECT_TRUE(HasPhase(events, ImportPhase::SourceParse));
    EXPECT_TRUE(HasPhase(events, ImportPhase::IntermediateConversion));
    EXPECT_TRUE(HasPhase(events, ImportPhase::ArtifactWrite));
    EXPECT_TRUE(HasPhase(events, ImportPhase::Postprocess));
    EXPECT_TRUE(HasPhase(events, ImportPhase::Commit));
    EXPECT_EQ(events.back().terminalStatus, ImportJobTerminalStatus::Succeeded);
    EXPECT_DOUBLE_EQ(events.back().normalizedProgress, 0.5);

    const auto batch = tracker.GetBatchProgress();
    EXPECT_EQ(batch.totalAssets, 2u);
    EXPECT_EQ(batch.completedAssets, 1u);
    EXPECT_EQ(batch.failedAssets, 0u);
    EXPECT_EQ(batch.cancelledAssets, 0u);
    EXPECT_FALSE(batch.activeJob.has_value());
    EXPECT_FALSE(tracker.GetActiveEvent().has_value());
}

TEST(AssetImportProgressTests, SequentialBatchProgressAggregatesAcrossJobsWithoutResetting)
{
    ImportProgressTracker tracker;
    std::vector<NLS::Editor::Assets::ImportProgressEvent> observed;
    tracker.Subscribe([&observed](const auto& event)
    {
        observed.push_back(event);
    });

    const auto first = tracker.BeginJob(
        Id("d2020202-0202-4202-8202-020202020202"),
        "Assets/Models/First.gltf",
        "editor",
        2u);
    tracker.ReportProgress(first, ImportPhase::SourceParse, 0.5, "Parsing first");
    tracker.FinishJob(first, ImportJobTerminalStatus::Succeeded, {});

    const auto second = tracker.BeginJob(
        Id("d2030303-0303-4303-8303-030303030303"),
        "Assets/Models/Second.gltf",
        "editor",
        2u);
    tracker.ReportProgress(second, ImportPhase::SourceParse, 0.25, "Parsing second");
    tracker.FinishJob(second, ImportJobTerminalStatus::Succeeded, {});

    ASSERT_FALSE(observed.empty());
    double previousProgress = 0.0;
    for (const auto& event : observed)
    {
        EXPECT_GE(event.normalizedProgress, previousProgress);
        previousProgress = event.normalizedProgress;
    }

    const auto firstEvents = tracker.GetEvents(first);
    const auto secondEvents = tracker.GetEvents(second);
    ASSERT_GE(firstEvents.size(), 3u);
    ASSERT_GE(secondEvents.size(), 3u);
    EXPECT_DOUBLE_EQ(firstEvents[1].normalizedProgress, 0.25);
    EXPECT_DOUBLE_EQ(firstEvents.back().normalizedProgress, 0.5);
    EXPECT_DOUBLE_EQ(secondEvents.front().normalizedProgress, 0.5);
    EXPECT_DOUBLE_EQ(secondEvents[1].normalizedProgress, 0.625);
    EXPECT_DOUBLE_EQ(secondEvents.back().normalizedProgress, 1.0);
}

TEST(AssetImportProgressTests, ManualImportSchedulerAllowsEditorCommandsWhileJobsAdvance)
{
    ImportProgressTracker tracker;
    const auto first = tracker.BeginJob(
        Id("d3010101-0101-4101-8101-010101010101"),
        "Assets/Models/Hero.gltf",
        "editor",
        2u);
    const auto second = tracker.BeginJob(
        Id("d3020202-0202-4202-8202-020202020202"),
        "Assets/Models/Tree.fbx",
        "editor",
        2u);

    tracker.ReportProgress(first, ImportPhase::SourceParse, 0.25, "Parsing Hero");

    NLS::Editor::Assets::EditorAssetDatabase database;
    const auto dragCommands = database.GetAssetDragDropCommandSurface({
        NLS::Editor::Assets::AssetDragDropCommandSubject::HierarchyObjectToAssetFolder,
        true,
        false,
        false,
        true,
        false
    });
    EXPECT_TRUE(HasCommand(dragCommands, "dragdrop.save-as-prefab", true));
    EXPECT_TRUE(tracker.HasRunningJobs());

    tracker.ReportProgress(second, ImportPhase::DependencyCopy, 0.1, "Copy Tree dependencies");
    tracker.FinishJob(first, ImportJobTerminalStatus::Succeeded, {});
    tracker.FinishJob(second, ImportJobTerminalStatus::Succeeded, {});

    const auto batch = tracker.GetBatchProgress();
    EXPECT_EQ(batch.completedAssets, 2u);
    EXPECT_FALSE(tracker.HasRunningJobs());
}

TEST(AssetImportProgressTests, CancellationPreservesPreviousCommittedArtifactAndCleansStaging)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportProgressRoot();
    const auto stagingRoot = root / "Staging";
    const auto committedRoot = root / "Committed";
    std::filesystem::create_directories(committedRoot / "Hero");
    {
        std::ofstream previous(committedRoot / "Hero" / "prefab.nprefab", std::ios::binary | std::ios::trunc);
        previous << "previous";
    }

    ArtifactManifest previousManifest;
    previousManifest.sourceAssetId = Id("d4010101-0101-4101-8101-010101010101");
    previousManifest.primarySubAssetKey = "prefab:Previous";
    previousManifest.subAssets.push_back({
        previousManifest.sourceAssetId,
        "prefab:Previous",
        ArtifactType::Prefab,
        "prefab",
        (committedRoot / "Hero" / "prefab.nprefab").string(),
        "previous"
    });

    ArtifactWriteRequest request;
    request.sourceAssetId = previousManifest.sourceAssetId;
    request.importerId = "scene-model";
    request.targetPlatform = "editor";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab.nprefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });

    ImportProgressTracker tracker;
    const auto job = tracker.BeginJob(previousManifest.sourceAssetId, "Assets/Models/Hero.gltf", "editor", 1u);
    auto token = tracker.GetCancellationToken(job);
    ASSERT_TRUE(token.has_value());
    token->get().Cancel();

    ArtifactWriter writer(stagingRoot, committedRoot);
    const auto result = writer.WriteAndCommit(request, &previousManifest, &token->get());
    tracker.FinishJob(job, ImportJobTerminalStatus::Cancelled, result.diagnostics);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, "artifact-write-cancelled");
    EXPECT_EQ(result.manifest.primarySubAssetKey, "prefab:Previous");
    EXPECT_EQ(ReadTextFile(committedRoot / "Hero" / "prefab.nprefab"), "previous");
    EXPECT_FALSE(std::filesystem::exists(stagingRoot));

    const auto batch = tracker.GetBatchProgress();
    EXPECT_EQ(batch.cancelledAssets, 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportProgressTests, FailedArtifactCommitReportsFailureAfterRollback)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportProgressRoot();
    const auto committedRoot = root / "Committed";
    std::filesystem::create_directories(committedRoot / "Hero" / "bad.nmesh");

    ArtifactWriteRequest request;
    request.sourceAssetId = Id("d5010101-0101-4101-8101-010101010101");
    request.importerId = "scene-model";
    request.targetPlatform = "editor";
    request.primarySubAssetKey = "mesh:Bad";
    request.artifacts.push_back({
        "mesh:Bad",
        ArtifactType::Mesh,
        "mesh",
        "Hero/bad.nmesh",
        std::vector<uint8_t>{'b', 'a', 'd'}
    });

    ImportProgressTracker tracker;
    const auto job = tracker.BeginJob(request.sourceAssetId, "Assets/Models/Hero.gltf", "editor", 1u);
    tracker.ReportProgress(job, ImportPhase::ArtifactWrite, 0.75, "Writing");

    ArtifactWriter writer(root / "Staging", committedRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);
    tracker.FinishJob(job, result.committed ? ImportJobTerminalStatus::Succeeded : ImportJobTerminalStatus::Failed, result.diagnostics);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, "artifact-commit-failed");
    EXPECT_TRUE(std::filesystem::is_directory(committedRoot / "Hero" / "bad.nmesh"));
    const auto events = tracker.GetEvents(job);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().terminalStatus, ImportJobTerminalStatus::Failed);

    std::filesystem::remove_all(root);
}

TEST(AssetImportProgressTests, AssetBrowserImportProgressCommandSurfaceReportsRunningAndCancelledState)
{
    NLS::Editor::Assets::EditorAssetDatabase database;
    ImportProgressTracker tracker;
    const auto job = tracker.BeginJob(
        Id("d6010101-0101-4101-8101-010101010101"),
        "Assets/Models/Hero.gltf",
        "editor",
        1u);

    auto running = database.GetImportProgressCommandSurface(tracker.GetBatchProgress());
    EXPECT_TRUE(HasCommand(running, "assetimport.cancel", true));
    EXPECT_TRUE(HasCommand(running, "assetimport.show-progress", true));

    auto token = tracker.GetCancellationToken(job);
    ASSERT_TRUE(token.has_value());
    token->get().Cancel();
    tracker.FinishJob(job, ImportJobTerminalStatus::Cancelled, {});

    auto stopped = database.GetImportProgressCommandSurface(tracker.GetBatchProgress());
    EXPECT_TRUE(HasCommand(stopped, "assetimport.cancel", false));
    EXPECT_TRUE(HasCommand(stopped, "assetimport.show-progress", true));
}

TEST(AssetImportProgressTests, ImportProgressStatusReportsStartupWorkForGlobalStatusBar)
{
    NLS::Editor::Assets::EditorAssetDatabase database;
    ImportProgressTracker tracker;
    const auto job = tracker.BeginJob(
        Id("d6020202-0202-4202-8202-020202020202"),
        "Startup Scene",
        "editor",
        1u);

    tracker.ReportProgress(job, ImportPhase::SourceParse, 0.35, "Loading startup scene models and shaders");

    const auto status = database.GetImportProgressStatus(tracker.GetActiveEvent());
    ASSERT_TRUE(status.visible);
    EXPECT_FALSE(status.cancellable);
    EXPECT_EQ(status.normalizedProgress, 0.35f);
    EXPECT_EQ(status.label, "Loading startup scene models and shaders - Startup Scene");

    tracker.FinishJob(job, ImportJobTerminalStatus::Succeeded, {});
    EXPECT_FALSE(database.GetImportProgressStatus(tracker.GetActiveEvent()).visible);
}

TEST(AssetImportProgressTests, ConcurrentImportProgressUpdatesKeepUniqueJobsAndBatchCounts)
{
    ImportProgressTracker tracker;
    constexpr size_t threadCount = 8u;
    constexpr size_t jobsPerThread = 32u;
    constexpr size_t totalJobs = threadCount * jobsPerThread;

    std::barrier startGate(static_cast<std::ptrdiff_t>(threadCount));
    std::mutex idsMutex;
    std::vector<ImportJobId> jobIds;
    jobIds.reserve(totalJobs);
    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    for (size_t threadIndex = 0u; threadIndex < threadCount; ++threadIndex)
    {
        workers.emplace_back([&, threadIndex]
        {
            startGate.arrive_and_wait();
            std::vector<ImportJobId> localIds;
            localIds.reserve(jobsPerThread);
            for (size_t jobIndex = 0u; jobIndex < jobsPerThread; ++jobIndex)
            {
                const auto job = tracker.BeginJob(
                    Id("d7010101-0101-4101-8101-010101010101"),
                    "Assets/Models/Concurrent" + std::to_string(threadIndex) + "_" + std::to_string(jobIndex) + ".gltf",
                    "editor",
                    totalJobs);
                tracker.ReportProgress(job, ImportPhase::SourceParse, 0.25, "Parse");
                tracker.ReportProgress(job, ImportPhase::ArtifactWrite, 0.75, "Write");
                tracker.FinishJob(job, ImportJobTerminalStatus::Succeeded, {});
                localIds.push_back(job);
            }

            std::lock_guard lock(idsMutex);
            jobIds.insert(jobIds.end(), localIds.begin(), localIds.end());
        });
    }

    for (auto& worker : workers)
        worker.join();

    std::sort(
        jobIds.begin(),
        jobIds.end(),
        [](const ImportJobId& lhs, const ImportJobId& rhs)
        {
            return lhs.value < rhs.value;
        });
    const auto uniqueEnd = std::unique(jobIds.begin(), jobIds.end());

    EXPECT_EQ(static_cast<size_t>(std::distance(jobIds.begin(), uniqueEnd)), totalJobs);
    EXPECT_EQ(tracker.GetBatchProgress().completedAssets, totalJobs);
    EXPECT_FALSE(tracker.HasRunningJobs());
    for (auto it = jobIds.begin(); it != uniqueEnd; ++it)
    {
        const auto events = tracker.GetEvents(*it);
        ASSERT_FALSE(events.empty());
        EXPECT_EQ(events.back().terminalStatus, ImportJobTerminalStatus::Succeeded);
    }
}
