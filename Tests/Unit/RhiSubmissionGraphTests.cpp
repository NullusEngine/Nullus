#include <gtest/gtest.h>

#include "Rendering/Context/RhiSubmissionGraph.h"
#include "TaskGraph/TaskGraph.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using NLS::Base::TaskGraph::NamedThread;
using NLS::Base::TaskGraph::TaskGraph;
using NLS::Base::TaskGraph::TaskStatus;
using NLS::Render::Context::RhiSubmissionGraph;
using NLS::Render::Context::RhiSubmissionGraphConfig;
using NLS::Render::Context::RhiSubmissionStage;
using NLS::Render::Context::RhiSubmissionStageCallbacks;
using NLS::Render::Context::kRhiSubmissionStageCount;

namespace
{
    struct StageRecorder
    {
        std::mutex mutex;
        std::vector<RhiSubmissionStage> order;
        std::vector<std::thread::id> threads;

        void Record(const RhiSubmissionStage stage)
        {
            std::lock_guard lock(mutex);
            order.push_back(stage);
            threads.push_back(std::this_thread::get_id());
        }
    };

    RhiSubmissionStageCallbacks MakeCallbacks(StageRecorder& recorder)
    {
        RhiSubmissionStageCallbacks callbacks;
        callbacks.recordWorkUnits = [&recorder] { recorder.Record(RhiSubmissionStage::RecordWorkUnits); };
        callbacks.translateWorkUnits = [&recorder] { recorder.Record(RhiSubmissionStage::TranslateWorkUnits); };
        callbacks.queueSubmit = [&recorder] { recorder.Record(RhiSubmissionStage::QueueSubmit); };
        callbacks.present = [&recorder] { recorder.Record(RhiSubmissionStage::Present); };
        callbacks.readback = [&recorder] { recorder.Record(RhiSubmissionStage::Readback); };
        callbacks.retire = [&recorder] { recorder.Record(RhiSubmissionStage::Retire); };
        return callbacks;
    }

    std::string ReadSourceFile(const std::filesystem::path& relativePath)
    {
        std::ifstream input(std::filesystem::path(NLS_ROOT_DIR) / relativePath, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    void ExpectStagesRanInDependencyOrder(const StageRecorder& recorder)
    {
        ASSERT_EQ(recorder.order.size(), kRhiSubmissionStageCount);
        for (size_t index = 0u; index < recorder.order.size(); ++index)
        {
            EXPECT_EQ(recorder.order[index], static_cast<RhiSubmissionStage>(index)) << "stage index " << index;
        }
    }
}

TEST(RhiSubmissionGraphTests, ScheduleCreatesOneDistinctNodePerStage)
{
    TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());
    ASSERT_TRUE(graph.RegisterCurrentThread(NamedThread::Rhi));

    StageRecorder recorder;
    const auto schedule = RhiSubmissionGraph::Schedule(graph, {}, MakeCallbacks(recorder));
    ASSERT_TRUE(schedule.IsValid());

    std::vector<uint64_t> nodeIds;
    nodeIds.reserve(kRhiSubmissionStageCount);
    for (const auto handle : schedule.stageHandles)
    {
        ASSERT_TRUE(handle.IsValid());
        nodeIds.push_back(handle.id);
    }

    std::sort(nodeIds.begin(), nodeIds.end());
    EXPECT_EQ(std::unique(nodeIds.begin(), nodeIds.end()), nodeIds.end())
        << "Every stage must be its own graph node, not a shared handle.";
    EXPECT_TRUE(schedule.completion.IsValid());

    EXPECT_EQ(RhiSubmissionGraph::WaitForCompletion(graph, schedule), TaskStatus::Succeeded);

    graph.UnregisterCurrentThread(NamedThread::Rhi);
    graph.Shutdown();
}

TEST(RhiSubmissionGraphTests, ParallelStagesRunOnWorkersAndRhiStagesRunOnTheNamedThread)
{
    TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());
    ASSERT_TRUE(graph.RegisterCurrentThread(NamedThread::Rhi));

    StageRecorder recorder;
    const auto rhiThreadId = std::this_thread::get_id();

    RhiSubmissionGraphConfig config;
    config.parallelRecordWorkUnits = true;
    config.parallelTranslateWorkUnits = true;

    const auto schedule = RhiSubmissionGraph::Schedule(graph, config, MakeCallbacks(recorder));
    ASSERT_TRUE(schedule.IsValid());
    EXPECT_EQ(RhiSubmissionGraph::WaitForCompletion(graph, schedule), TaskStatus::Succeeded);

    ExpectStagesRanInDependencyOrder(recorder);
    ASSERT_EQ(recorder.threads.size(), kRhiSubmissionStageCount);

    // Record and translate are capability-parallel work units: they must land on the
    // TaskGraph worker pool, never on the RHI named thread.
    EXPECT_NE(recorder.threads[static_cast<size_t>(RhiSubmissionStage::RecordWorkUnits)], rhiThreadId);
    EXPECT_NE(recorder.threads[static_cast<size_t>(RhiSubmissionStage::TranslateWorkUnits)], rhiThreadId);

    // Queue submit, present, readback and retire stay affine to the RHI named thread.
    for (size_t index = static_cast<size_t>(RhiSubmissionStage::QueueSubmit);
         index < kRhiSubmissionStageCount;
         ++index)
    {
        EXPECT_EQ(recorder.threads[index], rhiThreadId)
            << "stage index " << index << " must run on the RHI named thread";
    }

    graph.UnregisterCurrentThread(NamedThread::Rhi);
    graph.Shutdown();
}

TEST(RhiSubmissionGraphTests, SerialCapabilityCombinationRunsEveryStageOnTheRhiNamedThread)
{
    TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());
    ASSERT_TRUE(graph.RegisterCurrentThread(NamedThread::Rhi));

    StageRecorder recorder;
    const auto rhiThreadId = std::this_thread::get_id();

    // Both parallel capabilities unavailable: the graph must degrade to serial nodes
    // owned by the RHI named thread instead of falling back to another scheduler.
    RhiSubmissionGraphConfig config;
    config.parallelRecordWorkUnits = false;
    config.parallelTranslateWorkUnits = false;

    const auto schedule = RhiSubmissionGraph::Schedule(graph, config, MakeCallbacks(recorder));
    ASSERT_TRUE(schedule.IsValid());
    EXPECT_EQ(RhiSubmissionGraph::WaitForCompletion(graph, schedule), TaskStatus::Succeeded);

    ExpectStagesRanInDependencyOrder(recorder);
    for (const auto threadId : recorder.threads)
        EXPECT_EQ(threadId, rhiThreadId);

    graph.UnregisterCurrentThread(NamedThread::Rhi);
    graph.Shutdown();
}

TEST(RhiSubmissionGraphTests, FailedStageCancelsDependentsThroughCompletion)
{
    TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());
    ASSERT_TRUE(graph.RegisterCurrentThread(NamedThread::Rhi));

    StageRecorder recorder;
    auto callbacks = MakeCallbacks(recorder);
    callbacks.recordWorkUnits = [&recorder]
    {
        recorder.Record(RhiSubmissionStage::RecordWorkUnits);
        throw std::runtime_error("record stage failed");
    };

    const auto schedule = RhiSubmissionGraph::Schedule(graph, {}, std::move(callbacks));
    ASSERT_TRUE(schedule.IsValid());

    const auto status = RhiSubmissionGraph::WaitForCompletion(graph, schedule);
    EXPECT_NE(status, TaskStatus::Succeeded);

    // Only the failing stage ran; every downstream stage was cancelled.
    ASSERT_EQ(recorder.order.size(), 1u);
    EXPECT_EQ(recorder.order.front(), RhiSubmissionStage::RecordWorkUnits);

    for (size_t index = 1u; index < kRhiSubmissionStageCount; ++index)
    {
        const auto handle = schedule.stageHandles[index];
        ASSERT_TRUE(handle.IsValid());
        EXPECT_EQ(graph.GetCompletion(handle).GetStatus(), TaskStatus::Cancelled)
            << "stage index " << index << " must be cancelled by its failed prerequisite";
    }
    EXPECT_EQ(schedule.completion.GetStatus(), TaskStatus::Cancelled);

    graph.UnregisterCurrentThread(NamedThread::Rhi);
    graph.Shutdown();
}

TEST(RhiSubmissionGraphTests, InlineExecutionRunsTheSixStagesInOrderOnTheCallingThread)
{
    StageRecorder recorder;
    const auto callerThreadId = std::this_thread::get_id();

    auto callbacks = MakeCallbacks(recorder);
    EXPECT_EQ(RhiSubmissionGraph::ExecuteInline(callbacks), TaskStatus::Succeeded);

    ExpectStagesRanInDependencyOrder(recorder);
    for (const auto threadId : recorder.threads)
        EXPECT_EQ(threadId, callerThreadId);
}

TEST(RhiSubmissionGraphTests, InlineExecutionStopsAtTheFirstFailingStage)
{
    StageRecorder recorder;
    auto callbacks = MakeCallbacks(recorder);
    callbacks.queueSubmit = [&recorder]
    {
        recorder.Record(RhiSubmissionStage::QueueSubmit);
        throw std::runtime_error("queue submit failed");
    };

    EXPECT_EQ(RhiSubmissionGraph::ExecuteInline(callbacks), TaskStatus::Failed);

    // Record, translate and queue submit ran; present, readback and retire did not.
    ASSERT_EQ(recorder.order.size(), 3u);
    EXPECT_EQ(recorder.order[0u], RhiSubmissionStage::RecordWorkUnits);
    EXPECT_EQ(recorder.order[1u], RhiSubmissionStage::TranslateWorkUnits);
    EXPECT_EQ(recorder.order[2u], RhiSubmissionStage::QueueSubmit);
}

TEST(RhiSubmissionGraphTests, RhiCoordinatorNoLongerFallsBackToJobsParallelFor)
{
    const auto source = ReadSourceFile("Runtime/Rendering/Context/RhiThreadCoordinator.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("Jobs::ScheduleParallelFor"), std::string::npos)
        << "RHI recording must stay on the TaskGraph instead of falling back to the JobSystem.";
    EXPECT_EQ(source.find("NLS::Base::Jobs::"), std::string::npos);

    // The six stage bodies must exist as coordinator-side graph nodes.
    for (const char* stageEntry : {
             "RecordThreadedRhiSubmissionWork",
             "TranslateThreadedRhiSubmissionWork",
             "QueueSubmitThreadedRhiSubmission",
             "PresentThreadedRhiSubmission",
             "ReadbackThreadedRhiSubmission",
             "RetireThreadedRhiSubmission"})
    {
        EXPECT_NE(source.find(stageEntry), std::string::npos) << stageEntry;
    }
}
