#include <gtest/gtest.h>

#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/Context/RhiCommandListRecorder.h"
#include "Rendering/Context/RenderCommandFence.h"
#include "TaskGraph/TaskGraph.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    using NLS::Render::Context::RhiCommandList;
    using NLS::Render::Context::RhiCommandListExecutor;
    using NLS::Render::Context::RhiExecutorTargets;
    using NLS::Render::Context::RhiFlushFlags;
    using NLS::Render::Context::RhiThreadMode;
    using NLS::Render::Context::RhiTranslatedBatch;

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

    /** Minimal native command buffer so translation can be observed. */
    class StubCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
    {
    public:
        std::string_view GetDebugName() const override { return "StubCommandBuffer"; }

        void Begin() override { recording = !failBegin; }
        void End() override { recording = false; }
        void Reset() override { recording = false; }
        bool IsRecording() const override { return recording; }
        bool IsClosedForSubmission() const override { return !recording && !failClose; }
        NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override { return {}; }
        void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetViewport(const NLS::Render::RHI::RHIViewport&) override {}
        void SetScissor(const NLS::Render::RHI::RHIRect2D&) override {}
        void BindGraphicsPipeline(
            const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>&) override
        {
        }
        void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override
        {
        }
        void BindBindingSet(
            uint32_t,
            const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>&) override
        {
        }
        void PushConstants(
            NLS::Render::RHI::ShaderStageMask,
            uint32_t,
            uint32_t size,
            const void* data) override
        {
            constants.resize(size);
            if (size != 0u)
                std::memcpy(constants.data(), data, size);
        }
        void BindVertexBuffer(uint32_t, const NLS::Render::RHI::RHIVertexBufferView&) override {}
        void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView&) override {}
        void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override { ++draws; }
        void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
        void Dispatch(uint32_t, uint32_t, uint32_t) override {}
        void CopyBuffer(
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const std::shared_ptr<NLS::Render::RHI::RHIBuffer>&,
            const NLS::Render::RHI::RHIBufferCopyRegion&) override
        {
        }
        void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc&) override {}
        void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc&) override {}
        void Barrier(const NLS::Render::RHI::RHIBarrierDesc& barrier) override { barriers.push_back(barrier); }
        NLS::Render::RHI::RHIBarrierDesc BuildQueueReleaseBarriers(
            const NLS::Render::RHI::RHIBarrierDesc& barrier) const override
        {
            return requiresQueueRelease ? barrier : NLS::Render::RHI::RHIBarrierDesc{};
        }
        NLS::Render::RHI::RHICommandRecordingResult BarrierChecked(
            const NLS::Render::RHI::RHIBarrierDesc& barrier) override
        {
            if (failBarrier)
                return { NLS::Render::RHI::RHICommandRecordingStatusCode::BackendFailure, "native barrier rejected" };
            Barrier(barrier);
            return {};
        }
        void CommitResourceStates(const NLS::Render::RHI::RHIBarrierDesc& barrier) override { commits.push_back(barrier); }
        bool failBarrier = false;
        bool requiresQueueRelease = false;
        bool failBegin = false;
        bool failClose = false;
        bool recording = false;
        uint32_t draws = 0u;
        std::vector<std::byte> constants;
        std::vector<NLS::Render::RHI::RHIBarrierDesc> barriers;
        std::vector<NLS::Render::RHI::RHIBarrierDesc> commits;
    };

    class PlannedTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        std::string_view GetDebugName() const override { return "PlannedTexture"; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return desc; }
        NLS::Render::RHI::ResourceState GetState() const override
        {
            if (forbidReads)
                throw std::logic_error("Translation read shared resource state");
            return NLS::Render::RHI::ResourceState::ShaderRead;
        }
        NLS::Render::RHI::RHITextureDesc desc;
        bool forbidReads = false;
    };

    class StubFence final : public NLS::Render::RHI::RHIFence
    {
    public:
        std::atomic_bool signaled { false };

        std::string_view GetDebugName() const override { return "StubFence"; }
        bool IsSignaled() const override { return signaled.load(); }
        void Reset() override { signaled.store(false); }
        bool Wait(uint64_t) override { return signaled.load(); }
    };

    std::shared_ptr<RhiCommandList> MakeList(const std::string& name)
    {
        auto list = std::make_shared<RhiCommandList>();
        list->SetDebugName(name);
        return list;
    }

    /**
     * Stand-in for the dedicated RHI thread loop: pumps the RHI named-thread queue
     * and drives the executor's logical lane, which is exactly what the driver's
     * TickRhiLane does in production.
     */
    class RhiLaneDriver final
    {
    public:
        RhiLaneDriver(NLS::Base::TaskGraph::TaskGraph& graph, RhiCommandListExecutor& executor)
            : m_graph(graph)
            , m_executor(executor)
            , m_thread([this] { Run(); })
        {
        }

        ~RhiLaneDriver()
        {
            m_stop.store(true, std::memory_order_release);
            if (m_thread.joinable())
                m_thread.join();
        }

        RhiLaneDriver(const RhiLaneDriver&) = delete;
        RhiLaneDriver& operator=(const RhiLaneDriver&) = delete;

    private:
        void Run()
        {
            (void)m_graph.RegisterCurrentThread(NLS::Base::TaskGraph::NamedThread::Rhi);
            while (!m_stop.load(std::memory_order_acquire))
            {
                if (m_executor.GetMode() == RhiThreadMode::DedicatedThread)
                {
                    (void)m_graph.Pump(NLS::Base::TaskGraph::NamedThread::Rhi);
                    (void)m_executor.Pump();
                }
                std::this_thread::sleep_for(1ms);
            }
            m_graph.UnregisterCurrentThread(NLS::Base::TaskGraph::NamedThread::Rhi);
        }

        NLS::Base::TaskGraph::TaskGraph& m_graph;
        RhiCommandListExecutor& m_executor;
        std::atomic_bool m_stop { false };
        std::thread m_thread;
    };

}

TEST(RhiCommandListRecorderTests, OwnsPushConstantBytesAndResourceReferences)
{
    using namespace NLS::Render;
    StubCommandBuffer native;
    auto tracker = RHI::CreateDefaultResourceStateTracker();
    auto texture = std::make_shared<PlannedTexture>();
    std::weak_ptr<PlannedTexture> weakTexture = texture;
    auto list = MakeList("OwnedArguments");
    {
        Context::RhiCommandListRecorder recorder(*list, native, tracker);
        recorder.Begin();
        uint32_t constants = 0x12345678u;
        recorder.PushConstants(RHI::ShaderStageMask::Vertex, 0u, sizeof(constants), &constants);
        RHI::RHIBarrierDesc barrier;
        barrier.textureBarriers.push_back({texture, RHI::ResourceState::Unknown, RHI::ResourceState::RenderTarget});
        ASSERT_TRUE(recorder.BarrierChecked(barrier).Succeeded());
        recorder.End();
        constants = 0u;
    }
    tracker->Reset();
    texture.reset();
    EXPECT_FALSE(weakTexture.expired());
    EXPECT_TRUE(native.constants.empty());
    ASSERT_TRUE(list->Translate(native));
    uint32_t captured = 0u;
    ASSERT_EQ(native.constants.size(), sizeof(captured));
    std::memcpy(&captured, native.constants.data(), sizeof(captured));
    EXPECT_EQ(captured, 0x12345678u);
    EXPECT_TRUE(native.commits.empty());
    native.barriers.clear();
    list.reset();
    EXPECT_TRUE(weakTexture.expired());
}

TEST(RhiCommandListRecorderTests, ReverseTranslationKeepsOrderedBarrierStatesAndDefersGlobalCommit)
{
    using namespace NLS::Render;
    auto tracker = RHI::CreateDefaultResourceStateTracker();
    auto texture = std::make_shared<PlannedTexture>();
    auto first = MakeList("Producer");
    auto second = MakeList("Consumer");
    StubCommandBuffer firstNative;
    StubCommandBuffer secondNative;
    RHI::RHIBarrierDesc barrier;
    barrier.textureBarriers.push_back({texture, RHI::ResourceState::Unknown, RHI::ResourceState::RenderTarget});
    {
        Context::RhiCommandListRecorder recorder(*first, firstNative, tracker);
        recorder.Begin();
        ASSERT_TRUE(recorder.BarrierChecked(barrier).Succeeded());
        recorder.End();
    }
    barrier.textureBarriers.front().after = RHI::ResourceState::ShaderRead;
    {
        Context::RhiCommandListRecorder recorder(*second, secondNative, tracker);
        recorder.Begin();
        ASSERT_TRUE(recorder.BarrierChecked(barrier).Succeeded());
        recorder.End();
    }
    texture->forbidReads = true;
    ASSERT_TRUE(second->Translate(secondNative));
    ASSERT_TRUE(first->Translate(firstNative));
    ASSERT_EQ(firstNative.barriers.size(), 1u);
    ASSERT_EQ(secondNative.barriers.size(), 1u);
    EXPECT_TRUE(firstNative.barriers.front().statesAreExplicit);
    EXPECT_EQ(firstNative.barriers.front().textureBarriers.front().before, RHI::ResourceState::ShaderRead);
    EXPECT_EQ(secondNative.barriers.front().textureBarriers.front().before, RHI::ResourceState::RenderTarget);
    EXPECT_TRUE(firstNative.commits.empty());
    EXPECT_TRUE(secondNative.commits.empty());
    first->CommitResourceStates(firstNative);
    second->CommitResourceStates(secondNative);
    EXPECT_EQ(firstNative.commits.size(), 1u);
    EXPECT_EQ(secondNative.commits.size(), 1u);
}

TEST(RhiCommandListRecorderTests, NativeBarrierFailureStopsLaterCommandsWithDiagnostic)
{
    using namespace NLS::Render;
    StubCommandBuffer native;
    auto list = MakeList("FailingBarrier");
    Context::RhiCommandListRecorder recorder(*list, native, RHI::CreateDefaultResourceStateTracker());
    recorder.Begin();
    RHI::RHIBarrierDesc barrier;
    barrier.textureBarriers.push_back({std::make_shared<PlannedTexture>(), RHI::ResourceState::Unknown, RHI::ResourceState::RenderTarget});
    ASSERT_TRUE(recorder.BarrierChecked(barrier).Succeeded());
    recorder.Draw(3u, 1u, 0u, 0u);
    recorder.End();
    native.failBarrier = true;
    std::string diagnostic;
    EXPECT_FALSE(list->Translate(native, &diagnostic));
    EXPECT_NE(diagnostic.find("native barrier rejected"), std::string::npos);
    EXPECT_EQ(native.draws, 0u);
    EXPECT_TRUE(native.commits.empty());
}

TEST(RhiCommandListRecorderTests, QueueReleaseAfterGpuCommandsIsRejectedWithoutChangingState)
{
    using namespace NLS::Render;
    for (const bool copy : {false, true})
    {
        SCOPED_TRACE(copy);
        StubCommandBuffer native;
        native.requiresQueueRelease = true;
        auto tracker = RHI::CreateDefaultResourceStateTracker();
        auto texture = std::make_shared<PlannedTexture>();
        auto list = MakeList("LateQueueRelease");
        size_t releases = 0u;
        Context::RhiCommandListRecorder recorder(*list, native, tracker,
            [&](const auto&)
            {
                ++releases;
                return RHI::RHICommandRecordingResult{};
            });
        recorder.Begin();
        if (copy)
            recorder.CopyTexture({});
        else
            recorder.Dispatch(1u, 1u, 1u);
        RHI::RHIBarrierDesc barrier;
        barrier.textureBarriers.push_back({texture, RHI::ResourceState::ShaderRead, RHI::ResourceState::ShaderWrite});
        const auto result = recorder.BarrierChecked(barrier);
        EXPECT_FALSE(result.Succeeded());
        EXPECT_NE(result.message.find("split the RHI command list"), std::string::npos);
        EXPECT_EQ(releases, 0u);
        EXPECT_EQ(list->GetCommandCount(), 1u);
        EXPECT_FALSE(tracker->GetTextureState(texture, {}).has_value());
        recorder.End();
    }
}

TEST(RhiCommandListRecorderTests, QueueReleaseRequiresCallbackAndSuccessfulReleaseBeforeCommitting)
{
    using namespace NLS::Render;
    for (const bool withCallback : {false, true})
    {
        SCOPED_TRACE(withCallback);
        StubCommandBuffer native;
        native.requiresQueueRelease = true;
        auto tracker = RHI::CreateDefaultResourceStateTracker();
        auto texture = std::make_shared<PlannedTexture>();
        auto list = MakeList("RejectedQueueRelease");
        Context::RhiCommandListRecorder::QueueReleaseFunction release;
        if (withCallback)
            release = [](const auto&)
            {
                return RHI::RHICommandRecordingResult{RHI::RHICommandRecordingStatusCode::BackendFailure,
                    "queue release failed"};
            };
        Context::RhiCommandListRecorder recorder(*list, native, tracker, std::move(release));
        recorder.Begin();
        RHI::RHIBarrierDesc barrier;
        barrier.textureBarriers.push_back({texture, RHI::ResourceState::ShaderRead, RHI::ResourceState::ShaderWrite});
        const auto result = recorder.BarrierChecked(barrier);
        EXPECT_FALSE(result.Succeeded());
        EXPECT_NE(result.message.find(withCallback ? "queue release failed" : "ordered Graphics release list"),
            std::string::npos);
        EXPECT_EQ(list->GetCommandCount(), 0u);
        EXPECT_FALSE(tracker->GetTextureState(texture, {}).has_value());
        recorder.End();
    }
}

TEST(RhiCommandListExecutorTests, NativeBeginAndCloseFailuresReleaseEveryFenceWithoutSubmitting)
{
    for (const bool failBegin : { true, false })
    {
        SCOPED_TRACE(failBegin);
        auto& executor = RhiCommandListExecutor::Get();
        executor.ResetForTesting();
        auto native = std::make_shared<StubCommandBuffer>();
        native->failBegin = failBegin;
        native->failClose = !failBegin;
        size_t submitCalls = 0u;
        RhiExecutorTargets targets;
        targets.acquireCommandBuffer = [native](auto, auto) { return native; };
        targets.submit = [&submitCalls](const auto&)
        {
            ++submitCalls;
            return NLS::Render::RHI::RHIQueueOperationResult {};
        };
        executor.Initialize(RhiThreadMode::None, std::move(targets));
        auto list = MakeList("InvalidNativeLifecycle");
        ASSERT_TRUE(list->Record("Draw", [](auto& buffer) { buffer.Draw(3u); }));
        list->Close();
        ASSERT_TRUE(list->Submit());
        EXPECT_TRUE(list->GetDispatchEvent().Succeeded());
        EXPECT_TRUE(list->GetTranslateEvent().IsComplete());
        EXPECT_FALSE(list->GetTranslateEvent().Succeeded());
        EXPECT_TRUE(list->GetSubmitEvent().IsComplete());
        EXPECT_FALSE(list->GetSubmitEvent().Succeeded());
        EXPECT_TRUE(list->GetGpuFence().IsComplete());
        EXPECT_FALSE(list->GetGpuFence().Succeeded());
        EXPECT_TRUE(executor.HasFailed());
        EXPECT_NE(executor.GetFailureDiagnostic().find(failBegin ? "Begin failed" : "close failed"), std::string::npos);
        EXPECT_EQ(submitCalls, 0u);
        EXPECT_EQ(native->draws, failBegin ? 0u : 1u);
        EXPECT_TRUE(native->commits.empty());
        executor.ResetForTesting();
    }
}

TEST(RhiCommandListExecutorTests, SubmitsInDispatchOrderEvenWhenTranslationFinishesOutOfOrder)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 4u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    std::mutex orderMutex;
    std::vector<uint64_t> submitOrder;

    RhiExecutorTargets targets;
    targets.submit = [&orderMutex, &submitOrder](const RhiTranslatedBatch& batch)
    {
        {
            std::lock_guard lock(orderMutex);
            submitOrder.push_back(batch.sequence);
        }
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::Tasks, std::move(targets), &graph);
    RhiLaneDriver laneDriver(graph, executor);

    // The first list translates slowly, the later ones translate immediately, so
    // completion order is the reverse of submission order.
    std::vector<std::shared_ptr<RhiCommandList>> lists;
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        auto list = MakeList("ordered-" + std::to_string(index));
        const uint32_t delayMs = index == 0u ? 30u : 0u;
        list->Record(
            "delay",
            [delayMs](NLS::Render::RHI::RHICommandBuffer&)
            {
                if (delayMs > 0u)
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            });
        list->Close();
        ASSERT_TRUE(executor.Dispatch(list));
        lists.push_back(std::move(list));
    }

    ASSERT_TRUE(WaitUntil(
        [&executor] { return executor.GetSubmitSequence() >= 4u; },
        5s));

    {
        std::lock_guard lock(orderMutex);
        ASSERT_EQ(submitOrder.size(), 4u);
        EXPECT_EQ(submitOrder[0], 1u);
        EXPECT_EQ(submitOrder[1], 2u);
        EXPECT_EQ(submitOrder[2], 3u);
        EXPECT_EQ(submitOrder[3], 4u);
    }

    for (const auto& list : lists)
    {
        EXPECT_TRUE(list->GetTranslateEvent().IsComplete());
        EXPECT_TRUE(list->GetTranslateEvent().Succeeded());
        EXPECT_TRUE(list->GetSubmitEvent().Succeeded());
    }

    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, TranslateFlushDoesNotRequireSubmission)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    // DedicatedThread, not Tasks: in Tasks mode the submit step is posted to a
    // pool worker as soon as a translation finishes - that is the whole point of
    // the mode - so the submit would run unprompted and the assertion below would
    // be racing the pool. With DedicatedThread the submit is posted to the RHI
    // named lane, which has no thread registered in this test graph, so the lane
    // really is never pumped and the outcome is deterministic.
    RhiExecutorTargets targets;
    targets.submit = [](const RhiTranslatedBatch&)
    {
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::DedicatedThread, std::move(targets), &graph);

    auto list = MakeList("translate-only");
    list->Record("noop", [](NLS::Render::RHI::RHICommandBuffer&) {});
    list->Close();
    ASSERT_TRUE(executor.Dispatch(list));

    // Translation runs on the pool; the RHI lane is not driven here.
    EXPECT_TRUE(executor.Flush(NLS::Render::Context::RhiFlushFlags::Translate))
        << "A translate flush must complete without waiting for submission.";
    EXPECT_TRUE(list->GetTranslateEvent().IsComplete());
    EXPECT_FALSE(list->GetSubmitEvent().IsComplete())
        << "Nothing must have been submitted: the RHI lane was never pumped.";
    EXPECT_EQ(executor.GetSubmitSequence(), 0u);

    RhiLaneDriver laneDriver(graph, executor);
    EXPECT_TRUE(executor.Flush(NLS::Render::Context::RhiFlushFlags::Submit));
    EXPECT_TRUE(list->GetSubmitEvent().Succeeded());

    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, ReleaseResourcesRetiresCompleteBatchesWithoutGpuWait)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    std::atomic<uint32_t> retired { 0u };
    RhiExecutorTargets targets;
    targets.submit = [](const RhiTranslatedBatch&)
    {
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    targets.retireCommandBuffer = [&retired](const RhiTranslatedBatch&)
    {
        retired.fetch_add(1u, std::memory_order_acq_rel);
    };
    executor.Initialize(RhiThreadMode::Tasks, std::move(targets), &graph);
    RhiLaneDriver laneDriver(graph, executor);

    // One batch whose GPU work already finished, one still running.
    auto completeFence = std::make_shared<StubFence>();
    completeFence->signaled.store(true);

    auto completeList = MakeList("gpu-complete");
    completeList->Record("noop", [](NLS::Render::RHI::RHICommandBuffer&) {});
    completeList->SetSignalFence(completeFence);
    completeList->Close();
    ASSERT_TRUE(executor.Dispatch(completeList));

    auto pendingFence = std::make_shared<StubFence>();
    auto pendingList = MakeList("gpu-pending");
    pendingList->Record("noop", [](NLS::Render::RHI::RHICommandBuffer&) {});
    pendingList->SetSignalFence(pendingFence);
    pendingList->Close();
    ASSERT_TRUE(executor.Dispatch(pendingList));

    ASSERT_TRUE(WaitUntil([&executor] { return executor.GetSubmitSequence() >= 2u; }, 5s));

    const auto flags = NLS::Render::Context::RhiFlushFlags::Submit |
        NLS::Render::Context::RhiFlushFlags::ReleaseResources;
    EXPECT_TRUE(executor.Flush(flags));

    EXPECT_EQ(retired.load(std::memory_order_acquire), 1u)
        << "Only the GPU-complete batch may be retired.";

    // The pending fence stays unpolled and keeps its resources alive.
    pendingFence->signaled.store(true);
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 1u);
    EXPECT_EQ(retired.load(std::memory_order_acquire), 2u);

    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, TranslateReplaysRecordedCommandsIntoTheNativeBuffer)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    std::atomic<uint32_t> translatedCommands { 0u };
    RhiExecutorTargets targets;
    targets.acquireCommandBuffer = [](NLS::Render::RHI::QueueType, uint64_t)
    {
        return std::make_shared<StubCommandBuffer>();
    };
    targets.submit = [](const RhiTranslatedBatch&)
    {
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::DedicatedThread, std::move(targets), &graph);
    RhiLaneDriver laneDriver(graph, executor);

    auto list = MakeList("replay");
    for (uint32_t index = 0u; index < 5u; ++index)
    {
        EXPECT_TRUE(list->Record(
            "command",
            [&translatedCommands](NLS::Render::RHI::RHICommandBuffer&)
            {
                translatedCommands.fetch_add(1u, std::memory_order_acq_rel);
            }));
    }
    ASSERT_TRUE(list->RecordBarrier(NLS::Render::RHI::RHIBarrierDesc {}));
    list->Close();
    ASSERT_TRUE(executor.Dispatch(list));

    ASSERT_TRUE(WaitUntil(
        [&list] { return list->GetSubmitEvent().IsComplete(); },
        5s));
    EXPECT_EQ(translatedCommands.load(std::memory_order_acquire), 5u);
    EXPECT_TRUE(list->GetSubmitEvent().Succeeded());

    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, SubmitFailureCancelsOutstandingWorkAndReleasesWaiters)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    RhiExecutorTargets targets;
    NLS::Render::Context::RenderCommandEvent allowFailure;
    targets.submit = [allowFailure](const RhiTranslatedBatch&)
    {
        allowFailure.Wait();
        return NLS::Render::RHI::RHIQueueOperationResult {
            NLS::Render::RHI::RHIQueueOperationStatusCode::DeviceLost,
            "device removed during submit"
        };
    };
    executor.Initialize(RhiThreadMode::DedicatedThread, std::move(targets), &graph);
    RhiLaneDriver laneDriver(graph, executor);

    auto failing = MakeList("failing");
    failing->Record("noop", [](NLS::Render::RHI::RHICommandBuffer&) {});
    failing->Close();
    ASSERT_TRUE(executor.Dispatch(failing));

    auto queued = MakeList("queued");
    queued->Record("noop", [](NLS::Render::RHI::RHICommandBuffer&) {});
    queued->Close();
    const bool queuedBeforeFailure = executor.Dispatch(queued);
    allowFailure.Signal();
    ASSERT_TRUE(queuedBeforeFailure);

    ASSERT_TRUE(WaitUntil([&executor] { return executor.HasFailed(); }, 8s));
    EXPECT_EQ(executor.GetFailureDiagnostic(), "device removed during submit");

    EXPECT_TRUE(failing->GetSubmitEvent().IsComplete());
    EXPECT_FALSE(failing->GetSubmitEvent().Succeeded());
    EXPECT_EQ(failing->GetSubmitEvent().GetDiagnostic(), "device removed during submit");

    // The queued list must not leave a waiter blocked forever.
    ASSERT_TRUE(queued->GetSubmitEvent().WaitFor(2s));
    EXPECT_FALSE(queued->GetSubmitEvent().Succeeded());

    auto rejected = MakeList("rejected");
    rejected->Close();
    EXPECT_FALSE(executor.Dispatch(rejected));
    EXPECT_FALSE(rejected->GetDispatchEvent().Succeeded());

    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, PresentsAfterSubmitAndAdvancesPresentSequence)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    std::vector<std::string> events;
    std::mutex eventsMutex;
    RhiExecutorTargets targets;
    targets.submit = [&events, &eventsMutex](const RhiTranslatedBatch&)
    {
        std::lock_guard lock(eventsMutex);
        events.push_back("submit");
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    targets.present = [&events, &eventsMutex](const RhiTranslatedBatch&)
    {
        std::lock_guard lock(eventsMutex);
        events.push_back("present");
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::DedicatedThread, std::move(targets), &graph);
    RhiLaneDriver laneDriver(graph, executor);

    auto list = MakeList("frame");
    list->Record("noop", [](NLS::Render::RHI::RHICommandBuffer&) {});
    list->RequestPresent(true);
    list->Close();
    ASSERT_TRUE(executor.Dispatch(list));

    ASSERT_TRUE(WaitUntil([&list] { return list->GetSubmitEvent().IsComplete(); }, 5s));
    EXPECT_TRUE(list->GetSubmitEvent().Succeeded());
    EXPECT_EQ(executor.GetPresentSequence(), 1u);

    {
        std::lock_guard lock(eventsMutex);
        ASSERT_EQ(events.size(), 2u);
        EXPECT_EQ(events[0], "submit");
        EXPECT_EQ(events[1], "present");
    }

    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, RetiresTranslatedBatchesOnlyAfterTheGpuFenceSignals)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    std::atomic<uint32_t> retired { 0u };
    RhiExecutorTargets targets;
    targets.submit = [](const RhiTranslatedBatch&)
    {
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    targets.retireCommandBuffer = [&retired](const RhiTranslatedBatch&)
    {
        retired.fetch_add(1u, std::memory_order_acq_rel);
    };
    executor.Initialize(RhiThreadMode::DedicatedThread, std::move(targets), &graph);
    RhiLaneDriver laneDriver(graph, executor);

    auto gpuFence = std::make_shared<StubFence>();
    auto list = MakeList("gpu-fenced");
    list->Record("noop", [](NLS::Render::RHI::RHICommandBuffer&) {});
    list->SetSignalFence(gpuFence);
    list->Close();
    ASSERT_TRUE(executor.Dispatch(list));

    ASSERT_TRUE(WaitUntil([&list] { return list->GetSubmitEvent().IsComplete(); }, 5s));

    // The GPU has not signaled yet: resources must stay alive.
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 0u);
    EXPECT_EQ(retired.load(std::memory_order_acquire), 0u);

    gpuFence->signaled.store(true);
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 1u);
    EXPECT_EQ(retired.load(std::memory_order_acquire), 1u);

    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, NativeSubmitLaneSerializesOverlappingQueueOperations)
{
    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    constexpr uint32_t kCallerCount = 8u;
    std::atomic<uint32_t> active { 0u };
    std::atomic<uint32_t> overlapping { 0u };
    std::atomic<uint32_t> completed { 0u };
    std::atomic<uint32_t> failures { 0u };

    std::vector<std::thread> callers;
    callers.reserve(kCallerCount);
    for (uint32_t caller = 0u; caller < kCallerCount; ++caller)
    {
        callers.emplace_back(
            [&executor, &active, &overlapping, &completed, &failures]
            {
                const auto result = executor.RunOnSubmitLane(
                    "native-submit",
                    [&active, &overlapping]
                    {
                        if (active.fetch_add(1u, std::memory_order_acq_rel) != 0u)
                            overlapping.fetch_add(1u, std::memory_order_acq_rel);
                        std::this_thread::sleep_for(1ms);
                        active.fetch_sub(1u, std::memory_order_acq_rel);
                        return NLS::Render::RHI::RHIQueueOperationResult {};
                    });
                if (!result.Succeeded())
                    failures.fetch_add(1u, std::memory_order_acq_rel);
                completed.fetch_add(1u, std::memory_order_acq_rel);
            });
    }
    for (auto& caller : callers)
        caller.join();

    EXPECT_EQ(completed.load(std::memory_order_acquire), kCallerCount);
    EXPECT_EQ(failures.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(overlapping.load(std::memory_order_acquire), 0u)
        << "Two native queue operations overlapped: the submit lane is not single-flight.";
    EXPECT_EQ(executor.GetSubmitSequence(), static_cast<uint64_t>(kCallerCount));

    executor.ResetForTesting();
}

/**
 * Frame setup enqueues swapchain acquisition at frame begin and only waits at
 * the first consumer. That contract requires the enqueue itself to return while
 * the lane is still busy with earlier work.
 */
TEST(RhiCommandListExecutorTests, EnqueueOnSubmitLaneReturnsBeforeItsOperationRuns)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 4u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    RhiExecutorTargets targets;
    targets.submit = [](const RhiTranslatedBatch&)
    {
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::Tasks, std::move(targets), &graph);
    RhiLaneDriver laneDriver(graph, executor);

    std::atomic_bool started { false };
    std::atomic_bool release { false };
    std::atomic<uint32_t> runs { 0u };

    const auto enqueueStarted = std::chrono::steady_clock::now();
    const auto handle = executor.EnqueueOnSubmitLane(
        "deferred-lane-operation",
        [&started, &release, &runs]
        {
            started.store(true, std::memory_order_release);
            runs.fetch_add(1u, std::memory_order_acq_rel);
            // Bounded so a blocking enqueue fails the timing assertion instead of
            // hanging the suite.
            for (int attempt = 0; attempt < 4000; ++attempt)
            {
                if (release.load(std::memory_order_acquire))
                    break;
                std::this_thread::sleep_for(1ms);
            }
            NLS::Render::RHI::RHIQueueOperationResult result;
            result.message = "deferred-operation-result";
            return result;
        });
    const auto enqueueElapsed = std::chrono::steady_clock::now() - enqueueStarted;

    EXPECT_LT(enqueueElapsed, 1s)
        << "EnqueueOnSubmitLane must not wait for its own operation to finish.";
    EXPECT_TRUE(WaitUntil([&started] { return started.load(std::memory_order_acquire); }, 5s))
        << "The submit lane never started the deferred operation.";
    EXPECT_FALSE(handle.completion.IsComplete())
        << "The handle must still be pending while the operation is blocked.";

    release.store(true, std::memory_order_release);
    const auto result = executor.WaitForSubmitLaneOperation(handle, "deferred-lane-operation");
    EXPECT_TRUE(result.Succeeded()) << result.message;
    EXPECT_EQ(result.message, "deferred-operation-result");
    EXPECT_EQ(runs.load(std::memory_order_acquire), 1u);
    EXPECT_EQ(executor.GetSubmitSequence(), 1u)
        << "The deferred native operation still owns the only submission sequence.";

    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, NativeWaitTimeoutCancelsPendingOperationsWithinItsConfiguredBudget)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    // Leave the named RHI lane stopped so no native operation can execute.
    executor.Initialize(RhiThreadMode::DedicatedThread, {}, &graph, nullptr, 100ms);
    std::atomic_uint32_t executed {0u};
    const auto first = executor.EnqueueOnSubmitLane("stalled acquire", [&executed]
    {
        ++executed;
        return NLS::Render::RHI::RHIQueueOperationResult {};
    });
    const auto later = executor.EnqueueOnSubmitLane("cancelled present", [&executed]
    {
        ++executed;
        return NLS::Render::RHI::RHIQueueOperationResult {};
    }, true);
    const auto started = std::chrono::steady_clock::now();
    const auto result = executor.WaitForSubmitLaneOperation(first, "stalled acquire");
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.mayHaveQueuedGpuWork);
    EXPECT_NE(result.message.find("stalled acquire"), std::string::npos);
    EXPECT_TRUE(later.completion.WaitFor(100ms));
    EXPECT_FALSE(later.completion.Succeeded());
    EXPECT_FALSE(later.result->mayHaveQueuedGpuWork);
    EXPECT_EQ(executed.load(), 0u);
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_FALSE(executor.Flush(RhiFlushFlags::Swapchain));
    executor.Shutdown();
    graph.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, FlushTimeoutUsesConfiguredBudgetAndLeavesOrderedWorkResumable)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::DedicatedThread, {}, &graph, nullptr, 100ms);
    std::vector<uint32_t> order;
    const auto first = executor.EnqueueOnSubmitLane("first submit", [&order]
    {
        order.push_back(1u);
        return NLS::Render::RHI::RHIQueueOperationResult {};
    });
    const auto present = executor.EnqueueOnSubmitLane("second present", [&order]
    {
        order.push_back(2u);
        return NLS::Render::RHI::RHIQueueOperationResult {};
    }, true);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(executor.Flush(RhiFlushFlags::Swapchain));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    EXPECT_FALSE(executor.HasFailed());
    EXPECT_FALSE(first.completion.IsComplete());
    EXPECT_FALSE(present.completion.IsComplete());
    {
        RhiLaneDriver lane(graph, executor);
        EXPECT_TRUE(executor.Flush(RhiFlushFlags::Swapchain));
    }
    EXPECT_TRUE(first.completion.Succeeded());
    EXPECT_TRUE(present.completion.Succeeded());
    EXPECT_EQ(order, (std::vector<uint32_t>{1u, 2u}));
    EXPECT_EQ(executor.GetSubmitSequence(), 2u);
    EXPECT_EQ(executor.GetPresentSequence(), 2u);
    executor.Shutdown();
    graph.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, NoneModeTranslationTimeoutRetainsResourcesUntilTheWorkerStops)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    RhiExecutorTargets targets;
    targets.acquireCommandBuffer = [](NLS::Render::RHI::QueueType, uint64_t)
    {
        return std::make_shared<StubCommandBuffer>();
    };
    std::atomic_uint32_t submitted {0u};
    targets.submit = [&submitted](const RhiTranslatedBatch&)
    {
        ++submitted;
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::None, std::move(targets), &graph, nullptr, 100ms);
    std::promise<void> releaseTranslation;
    const auto release = releaseTranslation.get_future().share();
    std::atomic_bool translating {false};
    auto list = MakeList("slow validation translate");
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> resourceProbe = resource;
    list->Record("slow command", [release, resource, &translating](NLS::Render::RHI::RHICommandBuffer&)
    {
        translating.store(true, std::memory_order_release);
        (void)release.wait_for(3s);
    });
    resource.reset();
    list->Close();
    auto dispatch = std::async(std::launch::async, [&executor, list] { return executor.Dispatch(list); });
    EXPECT_TRUE(WaitUntil([&translating] { return translating.load(std::memory_order_acquire); }, 1s));
    EXPECT_TRUE(WaitUntil([&executor] { return executor.HasFailed(); }, 1s));
    EXPECT_EQ(dispatch.wait_for(20ms), std::future_status::timeout);
    EXPECT_FALSE(resourceProbe.expired());
    EXPECT_TRUE(list->GetSubmitEvent().WaitFor(100ms));
    EXPECT_FALSE(list->GetSubmitEvent().Succeeded());
    releaseTranslation.set_value();
    EXPECT_TRUE(dispatch.get());
    graph.Shutdown();
    EXPECT_EQ(submitted.load(), 0u);
    EXPECT_EQ(executor.GetPendingTranslateCount(), 0u);
    EXPECT_TRUE(list->GetGpuFence().IsComplete());
    EXPECT_FALSE(list->GetGpuFence().Succeeded());
    executor.Shutdown();
    executor.ResetForTesting();
    list.reset();
    EXPECT_TRUE(resourceProbe.expired());
}

TEST(RhiCommandListExecutorTests, EnqueueOnSubmitLaneRejectsWorkWithoutConsumingASequence)
{
    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    const auto empty = executor.EnqueueOnSubmitLane("empty-action", {});
    EXPECT_TRUE(empty.completion.IsComplete())
        << "A rejected enqueue must complete immediately so the caller cannot wait forever.";
    EXPECT_FALSE(empty.result->Succeeded());
    EXPECT_FALSE(empty.result->message.empty());
    EXPECT_EQ(executor.GetDispatchSequence(), 0u);

    executor.NotifyDeviceLost("device lost before enqueue");
    const auto quarantined = executor.EnqueueOnSubmitLane(
        "after-device-loss",
        [] { return NLS::Render::RHI::RHIQueueOperationResult {}; });
    EXPECT_TRUE(quarantined.completion.IsComplete());
    EXPECT_FALSE(quarantined.result->Succeeded());
    EXPECT_EQ(executor.GetDispatchSequence(), 0u)
        << "Work rejected by a quarantined lane must not consume a sequence.";

    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, NativeSubmitLaneRejectsReentrantQueueOperations)
{
    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    NLS::Render::RHI::RHIQueueOperationResult nested;

    const auto outer = executor.RunOnSubmitLane(
        "outer",
        [&executor, &nested]
        {
            nested = executor.RunOnSubmitLane(
                "nested",
                [] { return NLS::Render::RHI::RHIQueueOperationResult {}; });
            return NLS::Render::RHI::RHIQueueOperationResult {};
        });

    EXPECT_TRUE(outer.Succeeded());
    EXPECT_FALSE(nested.Succeeded());
    EXPECT_FALSE(nested.message.empty());
    EXPECT_EQ(executor.GetSubmitSequence(), 1u)
        << "A rejected nested operation must not consume a submission sequence.";

    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, BackendSubOperationsUseTheConfiguredLaneWithoutAllocatingAnotherSequence)
{
    for (const auto mode : {RhiThreadMode::None, RhiThreadMode::Tasks, RhiThreadMode::DedicatedThread})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
        ASSERT_TRUE(graph.Start());
        auto& executor = RhiCommandListExecutor::Get();
        executor.ResetForTesting();
        executor.Initialize(mode, {}, &graph);
        RhiLaneDriver laneDriver(graph, executor);
        std::atomic<uint32_t> operations {0u};
        EXPECT_TRUE(executor.RunBackendOperation("upload", [&]
        {
            const auto laneThread = std::this_thread::get_id();
            EXPECT_TRUE(NLS::Render::Context::RenderingThreadSystem::IsInRhiThread());
            if (mode == RhiThreadMode::Tasks)
                EXPECT_TRUE(graph.IsWorkerThread());
            return executor.RunBackendOperation("upload signal", [&]
            {
                EXPECT_EQ(std::this_thread::get_id(), laneThread);
                EXPECT_TRUE(NLS::Render::Context::RenderingThreadSystem::IsInRhiThread());
                operations.fetch_add(1u);
                return NLS::Render::RHI::RHIQueueOperationResult{};
            });
        }).Succeeded());
        EXPECT_EQ(operations.load(), 1u);
        EXPECT_EQ(executor.GetDispatchedSequence(), 1u);
        EXPECT_EQ(executor.GetSubmitSequence(), 1u);
        executor.Shutdown();
        executor.ResetForTesting();
        graph.Shutdown();
    }
}

TEST(RhiCommandListExecutorTests, IgnoredBackendFailureStillFailsTheParentAndRejectsLaterQueueWork)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    bool touchedDevice = false;
    const auto parent = executor.RunOnSubmitLane("parent", [&]
    {
        const auto failure = executor.RunBackendOperation("signal", []
        {
            return NLS::Render::RHI::RHIQueueOperationResult{
                NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
                "injected backend signal failure", true};
        });
        EXPECT_FALSE(failure.Succeeded());
        EXPECT_FALSE(executor.RunBackendOperation("later copy", [&]
        {
            touchedDevice = true;
            return NLS::Render::RHI::RHIQueueOperationResult{};
        }).Succeeded());
        return NLS::Render::RHI::RHIQueueOperationResult{};
    });
    EXPECT_FALSE(parent.Succeeded());
    EXPECT_TRUE(parent.mayHaveQueuedGpuWork);
    EXPECT_FALSE(touchedDevice);
    EXPECT_EQ(parent.message, "injected backend signal failure");
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_FALSE(executor.Flush(RhiFlushFlags::Submit));
    EXPECT_EQ(executor.GetSubmitSequence(), 1u);
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, BackendFailureInABatchCancelsPresentAndCompletesAllWaiters)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    auto list = MakeList("failed backend batch");
    list->Close();
    Context::RhiCommandListBatch batch;
    batch.commandLists = {list};
    batch.acquireCommandBuffer = [](size_t, RHI::QueueType, uint64_t)
    {
        return std::make_shared<StubCommandBuffer>();
    };
    bool presented = false;
    batch.submit = [&executor](const auto&)
    {
        (void)executor.RunBackendOperation("batch signal", []
        {
            return RHI::RHIQueueOperationResult{RHI::RHIQueueOperationStatusCode::BackendFailure,
                "batch signal failed", true};
        });
        return RHI::RHIQueueOperationResult{};
    };
    batch.requestsPresent = true;
    batch.present = [&presented](const auto&)
    {
        presented = true;
        return RHI::RHIQueueOperationResult{};
    };
    Context::RhiSubmitEvent completed;
    EXPECT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    EXPECT_TRUE(completed.IsComplete());
    EXPECT_FALSE(completed.Succeeded());
    EXPECT_FALSE(presented);
    EXPECT_TRUE(list->GetTranslateEvent().Succeeded());
    EXPECT_TRUE(list->GetSubmitEvent().IsComplete());
    EXPECT_FALSE(list->GetSubmitEvent().Succeeded());
    EXPECT_TRUE(list->GetGpuFence().IsComplete());
    EXPECT_FALSE(list->GetGpuFence().Succeeded());
    EXPECT_EQ(executor.GetPendingSubmitCount(), 0u);
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, BackendExceptionRetainsTheDiagnosticAndReleasesItsParentWait)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    const auto parent = executor.RunOnSubmitLane("parent", [&]
    {
        return executor.RunBackendOperation("native upload", []() -> NLS::Render::RHI::RHIQueueOperationResult
        {
            throw std::runtime_error("injected queue exception");
        });
    });
    EXPECT_FALSE(parent.Succeeded());
    EXPECT_TRUE(parent.mayHaveQueuedGpuWork);
    EXPECT_NE(parent.message.find("native upload: injected queue exception"), std::string::npos);
    EXPECT_FALSE(executor.IsSubmitInFlight());
    EXPECT_EQ(executor.GetPendingSubmitCount(), 0u);
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, NativeSubmitExceptionQuarantinesTheLaneAndReleasesFlush)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});

    const auto result = executor.RunOnSubmitLane(
        "ThrowingNativeSubmit",
        []() -> NLS::Render::RHI::RHIQueueOperationResult
        {
            throw std::runtime_error("injected native queue exception");
        });
    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(result.mayHaveQueuedGpuWork);
    EXPECT_FALSE(result.frameFenceSignalQueued);
    EXPECT_NE(result.message.find("ThrowingNativeSubmit: injected native queue exception"), std::string::npos);
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_FALSE(executor.IsSubmitInFlight());
    EXPECT_FALSE(executor.Flush(RhiFlushFlags::Submit));

    bool touchedDevice = false;
    EXPECT_FALSE(executor.RunOnSubmitLane("AfterFailure", [&touchedDevice]
    {
        touchedDevice = true;
        return NLS::Render::RHI::RHIQueueOperationResult {};
    }).Succeeded());
    EXPECT_FALSE(touchedDevice);

    auto rejected = MakeList("rejected");
    rejected->Close();
    EXPECT_FALSE(executor.Dispatch(rejected));
    EXPECT_TRUE(rejected->GetTranslateEvent().IsComplete());
    EXPECT_TRUE(rejected->GetSubmitEvent().IsComplete());
    EXPECT_TRUE(rejected->GetGpuFence().IsComplete());
    EXPECT_FALSE(rejected->GetGpuFence().Succeeded());
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, FlushWaitsForNativeSubmitAndDeferredSubmitsShareTheLane)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    std::promise<void> releaseNative;
    auto release = releaseNative.get_future().share();
    std::atomic_bool nativeActive { false };
    std::atomic_bool overlapped { false };
    RhiExecutorTargets targets;
    targets.submit = [&nativeActive, &overlapped](const RhiTranslatedBatch&)
    {
        overlapped.store(nativeActive.load());
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    auto native = std::async(std::launch::async, [&executor, &nativeActive, release]
    {
        return executor.RunOnSubmitLane("blocked native", [&nativeActive, release]
        {
            nativeActive.store(true);
            (void)release.wait_for(2s);
            nativeActive.store(false);
            return NLS::Render::RHI::RHIQueueOperationResult {};
        });
    });
    EXPECT_TRUE(WaitUntil([&nativeActive] { return nativeActive.load(); }, 1s));
    EXPECT_TRUE(executor.IsSubmitInFlight());
    auto flush = std::async(std::launch::async, [&executor] { return executor.Flush(RhiFlushFlags::Submit); });
    EXPECT_EQ(flush.wait_for(20ms), std::future_status::timeout);

    auto list = MakeList("deferred");
    list->Close();
    auto dispatch = std::async(std::launch::async, [&executor, list] { return executor.Dispatch(list); });
    EXPECT_EQ(dispatch.wait_for(20ms), std::future_status::timeout);
    releaseNative.set_value();
    EXPECT_TRUE(native.get().Succeeded());
    EXPECT_TRUE(dispatch.get());
    EXPECT_TRUE(flush.get());
    EXPECT_FALSE(overlapped.load());
    EXPECT_TRUE(list->GetSubmitEvent().Succeeded());
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, BackendExceptionsCompleteEveryStageWithTheirDiagnostic)
{
    auto& executor = RhiCommandListExecutor::Get();
    for (const std::string stage : { "acquire", "record", "submit", "present", "unknown" })
    {
        SCOPED_TRACE(stage);
        executor.ResetForTesting();
        RhiExecutorTargets targets;
        targets.acquireCommandBuffer = [stage](NLS::Render::RHI::QueueType, uint64_t)
            -> std::shared_ptr<NLS::Render::RHI::RHICommandBuffer>
        {
            if (stage == "acquire")
                throw std::runtime_error("acquire failed");
            return std::make_shared<StubCommandBuffer>();
        };
        targets.submit = [stage](const RhiTranslatedBatch&) -> NLS::Render::RHI::RHIQueueOperationResult
        {
            if (stage == "submit")
                throw std::runtime_error("submit failed");
            if (stage == "unknown")
                throw 42;
            return {};
        };
        targets.present = [](const RhiTranslatedBatch&) -> NLS::Render::RHI::RHIQueueOperationResult
        {
            throw std::runtime_error("present failed");
        };
        executor.Initialize(RhiThreadMode::None, std::move(targets));
        auto list = MakeList(stage);
        if (stage == "record")
            list->Record("record failed", [](NLS::Render::RHI::RHICommandBuffer&)
            {
                throw std::runtime_error("record failed");
            });
        list->RequestPresent(stage == "present");
        list->Close();
        EXPECT_TRUE(executor.Dispatch(list));
        EXPECT_TRUE(list->GetTranslateEvent().IsComplete());
        EXPECT_TRUE(list->GetSubmitEvent().IsComplete());
        EXPECT_FALSE(list->GetSubmitEvent().Succeeded());
        EXPECT_TRUE(list->GetGpuFence().IsComplete());
        EXPECT_FALSE(list->GetGpuFence().Succeeded());
        EXPECT_NE(executor.GetFailureDiagnostic().find(
            stage == "unknown" ? "unknown exception" : stage + " failed"), std::string::npos);
        EXPECT_FALSE(executor.IsSubmitInFlight());
        EXPECT_FALSE(executor.Flush(RhiFlushFlags::Swapchain));
        executor.Shutdown();
    }
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, SwapchainFlushDoesNotWaitForGpuOrReleaseCapturedResources)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    auto fence = std::make_shared<StubFence>();
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> resourceProbe = resource;
    auto list = MakeList("presented gpu work");
    list->Record("resource", [resource](NLS::Render::RHI::RHICommandBuffer&) {});
    list->SetSignalFence(fence);
    list->RequestPresent(true);
    list->Close();
    EXPECT_TRUE(executor.Dispatch(list));
    auto gpuEvent = list->GetGpuFence();
    resource.reset();
    list.reset();

    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(executor.Flush(RhiFlushFlags::Swapchain | RhiFlushFlags::ReleaseResources));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 0u);
    EXPECT_FALSE(gpuEvent.IsComplete());
    EXPECT_FALSE(resourceProbe.expired());

    fence->signaled.store(true);
    EXPECT_TRUE(executor.Flush(RhiFlushFlags::Gpu | RhiFlushFlags::ReleaseResources));
    EXPECT_TRUE(gpuEvent.Succeeded());
    EXPECT_TRUE(resourceProbe.expired());
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, ShutdownKeepsUnconfirmedGpuResourcesAlive)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> resourceProbe = resource;
    auto list = MakeList("unconfirmed");
    list->Record("resource", [resource](NLS::Render::RHI::RHICommandBuffer&) {});
    list->SetSignalFence(std::make_shared<StubFence>());
    list->Close();
    EXPECT_TRUE(executor.Dispatch(list));
    resource.reset();
    list.reset();
    executor.Shutdown();
    EXPECT_FALSE(resourceProbe.expired());
    executor.ResetForTesting();
    EXPECT_TRUE(resourceProbe.expired());
}

TEST(RhiCommandListExecutorTests, NativeFailureCancelsAnAlreadyRunningTranslation)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    std::promise<void> releaseTranslation;
    auto release = releaseTranslation.get_future().share();
    std::atomic_bool translating { false };
    std::atomic_bool nativeActive { false };
    std::promise<void> releaseNative;
    const auto nativeRelease = releaseNative.get_future().share();
    std::atomic<uint32_t> submitted { 0u };
    RhiExecutorTargets targets;
    targets.acquireCommandBuffer = [&translating, release](NLS::Render::RHI::QueueType, uint64_t)
    {
        translating.store(true);
        (void)release.wait_for(2s);
        return std::make_shared<StubCommandBuffer>();
    };
    targets.submit = [&submitted](const RhiTranslatedBatch&)
    {
        submitted.fetch_add(1u);
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::Tasks, std::move(targets), &graph);
    auto native = std::async(std::launch::async, [&executor, &nativeActive, nativeRelease]
    {
        return executor.RunOnSubmitLane("failure", [&nativeActive, nativeRelease]() ->
            NLS::Render::RHI::RHIQueueOperationResult
        {
            nativeActive.store(true);
            (void)nativeRelease.wait_for(2s);
            throw std::runtime_error("cancel all stages");
        });
    });
    EXPECT_TRUE(WaitUntil([&nativeActive] { return nativeActive.load(); }, 1s));
    auto list = MakeList("active translation");
    list->Close();
    EXPECT_TRUE(executor.Dispatch(list));
    EXPECT_TRUE(WaitUntil([&translating] { return translating.load(); }, 1s));
    releaseNative.set_value();
    EXPECT_FALSE(native.get().Succeeded());
    EXPECT_TRUE(list->GetTranslateEvent().WaitFor(100ms));
    EXPECT_TRUE(list->GetSubmitEvent().WaitFor(100ms));
    EXPECT_TRUE(list->GetGpuFence().WaitFor(100ms));
    EXPECT_FALSE(list->GetTranslateEvent().Succeeded());
    releaseTranslation.set_value();
    graph.Shutdown();
    EXPECT_EQ(submitted.load(), 0u);
    EXPECT_EQ(executor.GetPendingSubmitCount(), 0u);
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, NativeAndDeferredSubmissionsShareFifoSequencesAndFenceTargets)
{
    using NLS::Render::Context::RenderCommandDispatcher;
    using NLS::Render::Context::RenderCommandFence;
    using NLS::Render::Context::RenderCommandSyncDepth;

    for (const auto mode : {RhiThreadMode::Tasks, RhiThreadMode::DedicatedThread})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        NLS::Base::TaskGraph::TaskGraph graph({2u, false, true});
        ASSERT_TRUE(graph.Start());
        auto& executor = RhiCommandListExecutor::Get();
        executor.ResetForTesting();
        RenderCommandDispatcher::ResetForTesting();

        std::promise<void> releaseFirstTranslation;
        const auto release = releaseFirstTranslation.get_future().share();
        std::atomic_bool firstTranslating {false};
        std::mutex orderMutex;
        std::vector<std::string> order;
        RhiExecutorTargets targets;
        targets.acquireCommandBuffer = [&firstTranslating, release](
            NLS::Render::RHI::QueueType, const uint64_t sequence)
        {
            if (sequence == 1u)
            {
                firstTranslating.store(true);
                (void)release.wait_for(2s);
            }
            return std::make_shared<StubCommandBuffer>();
        };
        targets.submit = [&orderMutex, &order](const RhiTranslatedBatch& batch)
        {
            std::lock_guard lock(orderMutex);
            order.push_back(batch.debugName);
            return NLS::Render::RHI::RHIQueueOperationResult {};
        };
        executor.Initialize(mode, std::move(targets), &graph);
        RhiLaneDriver laneDriver(graph, executor);

        auto first = MakeList("first list");
        first->Close();
        EXPECT_TRUE(executor.Dispatch(first));
        EXPECT_TRUE(WaitUntil([&firstTranslating] { return firstTranslating.load(); }, 1s));
        auto native = std::async(std::launch::async, [&executor, &orderMutex, &order]
        {
            return executor.RunOnSubmitLane("native present", [&orderMutex, &order]
            {
                std::lock_guard lock(orderMutex);
                order.push_back("native present");
                return NLS::Render::RHI::RHIQueueOperationResult {};
            }, true);
        });
        EXPECT_TRUE(WaitUntil([&executor] { return executor.GetDispatchedSequence() == 2u; }, 1s));
        auto later = MakeList("later list");
        later->Close();
        EXPECT_TRUE(executor.Dispatch(later));
        EXPECT_TRUE(later->GetTranslateEvent().WaitFor(1s));

        RenderCommandFence rhiFence;
        RenderCommandFence presentFence;
        rhiFence.BeginFence(RenderCommandSyncDepth::Rhi);
        presentFence.BeginFence(RenderCommandSyncDepth::Swapchain);
        (void)RenderCommandDispatcher::Dispatch();
        EXPECT_EQ(first->GetSequence(), 1u);
        EXPECT_EQ(later->GetSequence(), 3u);
        EXPECT_EQ(executor.GetPresentRequestSequence(), 2u);
        EXPECT_EQ(executor.GetSubmitSequence(), 0u);
        EXPECT_FALSE(rhiFence.IsFenced());
        EXPECT_FALSE(presentFence.IsFenced());
        EXPECT_EQ(native.wait_for(10ms), std::future_status::timeout);

        releaseFirstTranslation.set_value();
        EXPECT_TRUE(native.get().Succeeded());
        EXPECT_TRUE(executor.Flush(RhiFlushFlags::Swapchain));
        EXPECT_TRUE(rhiFence.WaitFor(1s));
        EXPECT_TRUE(presentFence.WaitFor(1s));
        EXPECT_EQ(executor.GetSubmitSequence(), 3u);
        EXPECT_EQ(executor.GetPresentSequence(), 2u);
        {
            std::lock_guard lock(orderMutex);
            EXPECT_EQ(order, (std::vector<std::string>{"first list", "native present", "later list"}));
        }
        executor.Shutdown();
        executor.ResetForTesting();
        RenderCommandDispatcher::ResetForTesting();
        graph.Shutdown();
    }
}

TEST(RhiCommandListExecutorTests, SingleWorkerNativeWaitAssistsEarlierTranslationAndSubmission)
{
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    std::atomic<uint32_t> submitted {0u};
    RhiExecutorTargets targets;
    targets.acquireCommandBuffer = [](NLS::Render::RHI::QueueType, uint64_t)
    {
        return std::make_shared<StubCommandBuffer>();
    };
    targets.submit = [&submitted](const RhiTranslatedBatch& batch)
    {
        EXPECT_EQ(batch.sequence, 1u);
        submitted.fetch_add(1u);
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };
    executor.Initialize(RhiThreadMode::Tasks, std::move(targets), &graph);
    const auto task = graph.Submit([&executor, &submitted]
    {
        auto list = MakeList("single-worker list");
        list->Close();
        EXPECT_TRUE(executor.Dispatch(list));
        EXPECT_TRUE(executor.RunOnSubmitLane("single-worker native", [&submitted]
        {
            EXPECT_EQ(submitted.load(), 1u);
            return NLS::Render::RHI::RHIQueueOperationResult {};
        }).Succeeded());
        EXPECT_TRUE(list->GetSubmitEvent().Succeeded());
    });
    EXPECT_TRUE(graph.WaitFor(task, 2s));
    EXPECT_EQ(graph.GetCompletion(task).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
    EXPECT_TRUE(executor.Flush(RhiFlushFlags::Submit));
    EXPECT_EQ(executor.GetDispatchedSequence(), 2u);
    EXPECT_EQ(executor.GetSubmitSequence(), 2u);
    executor.Shutdown();
    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, NativeSubmissionsExecuteOnTheConfiguredRhiLane)
{
    for (const auto mode : {RhiThreadMode::None, RhiThreadMode::Tasks, RhiThreadMode::DedicatedThread})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
        ASSERT_TRUE(graph.Start());
        auto& executor = RhiCommandListExecutor::Get();
        executor.ResetForTesting();
        executor.Initialize(mode, {}, &graph);
        RhiLaneDriver laneDriver(graph, executor);
        const auto caller = std::this_thread::get_id();
        EXPECT_TRUE(executor.RunOnSubmitLane("configured lane", [&graph, caller, mode]
        {
            EXPECT_TRUE(NLS::Render::Context::RenderingThreadSystem::IsInRhiThread());
            if (mode == RhiThreadMode::None)
                EXPECT_EQ(std::this_thread::get_id(), caller);
            else if (mode == RhiThreadMode::Tasks)
                EXPECT_TRUE(graph.IsWorkerThread());
            else
                EXPECT_EQ(graph.GetCurrentNamedThread(), NLS::Base::TaskGraph::NamedThread::Rhi);
            return NLS::Render::RHI::RHIQueueOperationResult {};
        }).Succeeded());
        EXPECT_EQ(executor.GetDispatchedSequence(), 1u);
        EXPECT_EQ(executor.GetSubmitSequence(), 1u);
        executor.Shutdown();
        executor.ResetForTesting();
        graph.Shutdown();
    }
}

TEST(RhiCommandListExecutorTests, TranslationFailureCancelsQueuedNativePresentAndReleasesFences)
{
    using NLS::Render::Context::RenderCommandDispatcher;
    using NLS::Render::Context::RenderCommandFence;
    using NLS::Render::Context::RenderCommandSyncDepth;
    NLS::Base::TaskGraph::TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    RenderCommandDispatcher::ResetForTesting();
    std::promise<void> releaseTranslation;
    const auto release = releaseTranslation.get_future().share();
    std::atomic_bool translating {false};
    std::atomic_bool touchedDevice {false};
    RhiExecutorTargets targets;
    targets.acquireCommandBuffer = [&translating, release](NLS::Render::RHI::QueueType, uint64_t) ->
        std::shared_ptr<NLS::Render::RHI::RHICommandBuffer>
    {
        translating.store(true);
        (void)release.wait_for(2s);
        throw std::runtime_error("earlier translation failed");
    };
    executor.Initialize(RhiThreadMode::Tasks, std::move(targets), &graph);
    auto list = MakeList("failed list");
    list->Close();
    EXPECT_TRUE(executor.Dispatch(list));
    EXPECT_TRUE(WaitUntil([&translating] { return translating.load(); }, 1s));
    auto native = std::async(std::launch::async, [&executor, &touchedDevice]
    {
        return executor.RunOnSubmitLane("cancelled present", [&touchedDevice]
        {
            touchedDevice.store(true);
            return NLS::Render::RHI::RHIQueueOperationResult {};
        }, true);
    });
    EXPECT_TRUE(WaitUntil([&executor] { return executor.GetDispatchedSequence() == 2u; }, 1s));
    RenderCommandFence fence;
    fence.BeginFence(RenderCommandSyncDepth::Swapchain);
    (void)RenderCommandDispatcher::Dispatch();
    EXPECT_FALSE(fence.IsFenced());
    releaseTranslation.set_value();
    const auto result = native.get();
    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.mayHaveQueuedGpuWork);
    EXPECT_NE(result.message.find("earlier translation failed"), std::string::npos);
    EXPECT_FALSE(touchedDevice.load());
    EXPECT_TRUE(fence.WaitFor(1s));
    EXPECT_FALSE(fence.Succeeded());
    EXPECT_NE(fence.GetDiagnostic().find("earlier translation failed"), std::string::npos);
    EXPECT_FALSE(executor.Flush(RhiFlushFlags::Swapchain));
    EXPECT_FALSE(list->GetSubmitEvent().Succeeded());
    graph.Shutdown();
    executor.Shutdown();
    executor.ResetForTesting();
    RenderCommandDispatcher::ResetForTesting();
}

TEST(RhiCommandListExecutorTests, ReinitializeKeepsMixedSubmissionSequencesMonotonic)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    for (uint64_t index = 0u; index < 2u; ++index)
    {
        executor.Initialize(RhiThreadMode::None, {});
        EXPECT_TRUE(executor.RunOnSubmitLane("native", []
        {
            return NLS::Render::RHI::RHIQueueOperationResult {};
        }).Succeeded());
        auto list = MakeList("list");
        list->Close();
        EXPECT_TRUE(executor.Dispatch(list));
        const uint64_t expectedSequence = (index + 1u) * 2u;
        EXPECT_EQ(list->GetSequence(), expectedSequence);
        EXPECT_EQ(executor.GetSubmitSequence(), expectedSequence);
        EXPECT_TRUE(list->GetSubmitEvent().Succeeded());
        executor.Shutdown();
        bool touchedDevice = false;
        EXPECT_FALSE(executor.RunOnSubmitLane("after shutdown", [&touchedDevice]
        {
            touchedDevice = true;
            return NLS::Render::RHI::RHIQueueOperationResult {};
        }).Succeeded());
        EXPECT_FALSE(touchedDevice);
        EXPECT_EQ(executor.GetDispatchedSequence(), expectedSequence);
    }
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, NativeSubmitLaneAdvancesPresentSequenceOnlyForPresents)
{
    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    const auto submit = executor.RunOnSubmitLane(
        "submit",
        [] { return NLS::Render::RHI::RHIQueueOperationResult {}; });
    ASSERT_TRUE(submit.Succeeded());
    EXPECT_EQ(executor.GetSubmitSequence(), 1u);
    EXPECT_EQ(executor.GetPresentSequence(), 0u);

    const auto present = executor.RunOnSubmitLane(
        "present",
        [] { return NLS::Render::RHI::RHIQueueOperationResult {}; },
        true);
    ASSERT_TRUE(present.Succeeded());
    EXPECT_EQ(executor.GetSubmitSequence(), 2u);
    EXPECT_EQ(executor.GetPresentSequence(), 2u);

    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, ShutdownFromADriverThatLostTheLaneIsRefused)
{
    NLS::Base::TaskGraph::TaskGraph graph({ 2u, false, true });
    ASSERT_TRUE(graph.Start());

    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    int firstOwner = 0;
    int secondOwner = 0;
    RhiExecutorTargets targets;
    targets.submit = [](const RhiTranslatedBatch&)
    {
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };

    executor.Initialize(RhiThreadMode::None, targets, &graph, &firstOwner);
    ASSERT_EQ(executor.GetOwner(), static_cast<const void*>(&firstOwner));

    // A second driver takes the lane. The first driver is still alive and must
    // not be able to tear down the queue state the new owner is using.
    executor.Initialize(RhiThreadMode::None, targets, &graph, &secondOwner);
    ASSERT_EQ(executor.GetOwner(), static_cast<const void*>(&secondOwner));

    executor.Shutdown(RhiFlushFlags::Dispatch, &firstOwner);
    EXPECT_TRUE(executor.IsInitialized())
        << "A driver that lost the lane must not be able to shut it down.";
    EXPECT_EQ(executor.GetOwner(), static_cast<const void*>(&secondOwner));

    // The current owner can still shut the lane down, and doing so releases the
    // ownership so a later driver is not refused.
    executor.Shutdown(RhiFlushFlags::Dispatch, &secondOwner);
    EXPECT_FALSE(executor.IsInitialized());
    EXPECT_EQ(executor.GetOwner(), nullptr);

    executor.Initialize(RhiThreadMode::None, targets, &graph, &firstOwner);
    EXPECT_EQ(executor.GetOwner(), static_cast<const void*>(&firstOwner));
    executor.Shutdown(RhiFlushFlags::Dispatch, &firstOwner);

    executor.ResetForTesting();
    graph.Shutdown();
}

TEST(RhiCommandListExecutorTests, OwnerlessShutdownStaysUnconditionalForTeardownPaths)
{
    RhiCommandListExecutor& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();

    int owner = 0;
    RhiExecutorTargets targets;
    targets.submit = [](const RhiTranslatedBatch&)
    {
        return NLS::Render::RHI::RHIQueueOperationResult {};
    };

    executor.Initialize(RhiThreadMode::None, targets, nullptr, &owner);
    ASSERT_TRUE(executor.IsInitialized());

    // Process shutdown has no driver to name, so it keeps the old behaviour.
    executor.Shutdown(RhiFlushFlags::Dispatch);
    EXPECT_FALSE(executor.IsInitialized());
    EXPECT_EQ(executor.GetOwner(), static_cast<const void*>(&owner))
        << "An ownerless shutdown must not silently rewrite ownership.";

    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, BatchWaitsForAllTranslationsAndKeepsOneOrderedSequence)
{
    using namespace NLS::Render;
    NLS::Base::TaskGraph::TaskGraph graph({4u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::Tasks, {}, &graph);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic_bool firstStarted = false;
    auto first = MakeList("BatchFirst");
    first->Record("BlockedTranslate", [&firstStarted, released](RHI::RHICommandBuffer&)
    {
        firstStarted.store(true);
        released.wait();
    });
    first->Close();
    auto second = MakeList("BatchSecond");
    second->Close();
    auto fence = std::make_shared<StubFence>();
    auto third = MakeList("NextBatch");
    third->SetSignalFence(fence);
    third->Close();
    std::mutex mutex;
    std::vector<std::string> order;
    Context::RhiCommandListBatch batch;
    batch.commandLists = {first, second};
    batch.acquireCommandBuffer = [](size_t, RHI::QueueType, uint64_t)
        { return std::make_shared<StubCommandBuffer>(); };
    batch.submit = [&order, &mutex](const auto& translated)
    {
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
        EXPECT_EQ(translated.size(), 2u);
        EXPECT_EQ(translated[0].sequence, translated[1].sequence);
        std::lock_guard lock(mutex);
        order.push_back("BatchSubmit");
        return RHI::RHIQueueOperationResult {};
    };
    batch.requestsPresent = true;
    batch.present = [&order, &mutex](const auto&)
    {
        std::lock_guard lock(mutex);
        order.push_back("BatchPresent");
        return RHI::RHIQueueOperationResult {};
    };
    Context::RhiSubmitEvent completed;
    EXPECT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    const bool translatedSecond = WaitUntil([&]
        { return firstStarted.load() && second->GetTranslateEvent().IsComplete(); }, 2s);
    EXPECT_TRUE(translatedSecond);
    EXPECT_FALSE(completed.IsComplete());
    EXPECT_EQ(executor.GetSubmitSequence(), 0u);
    RhiExecutorTargets targets;
    targets.submit = [&order, &mutex](const auto&)
    {
        std::lock_guard lock(mutex);
        order.push_back("NextSubmit");
        return RHI::RHIQueueOperationResult {};
    };
    executor.SetTargets(std::move(targets));
    EXPECT_TRUE(executor.Dispatch(third));
    EXPECT_EQ(first->GetSequence(), 1u);
    EXPECT_EQ(second->GetSequence(), 1u);
    EXPECT_EQ(third->GetSequence(), 2u);
    release.set_value();
    EXPECT_TRUE(WaitUntil([&] { return third->GetSubmitEvent().IsComplete(); }, 2s));
    EXPECT_TRUE(completed.Succeeded());
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(order, (std::vector<std::string>{"BatchSubmit", "BatchPresent", "NextSubmit"}));
    }
    EXPECT_FALSE(third->GetGpuFence().IsComplete());
    fence->signaled.store(true);
    (void)executor.ReleaseRetiredBatches();
    EXPECT_TRUE(third->GetGpuFence().Succeeded());
    executor.Shutdown();
    graph.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, BatchGpuEventsAndOwnersWaitForTheWholeFrameFence)
{
    using namespace NLS::Render;
    for (const bool signalOnPresent : {false, true})
    {
        SCOPED_TRACE(signalOnPresent);
        auto& executor = RhiCommandListExecutor::Get();
        executor.ResetForTesting();
        executor.Initialize(RhiThreadMode::None, {});
        auto owner = std::make_shared<int>(42);
        std::weak_ptr<int> weakOwner = owner;
        auto frameFence = std::make_shared<StubFence>();
        auto earlyFence = std::make_shared<StubFence>();
        earlyFence->signaled.store(true);
        auto first = MakeList("FrameGraphics");
        first->Record("KeepOwner", [owner](RHI::RHICommandBuffer&) {});
        first->Close();
        auto second = MakeList("FrameCompute");
        second->SetQueueType(RHI::QueueType::Compute);
        second->SetSignalFence(earlyFence);
        second->Close();
        auto firstGpu = first->GetGpuFence();
        auto secondGpu = second->GetGpuFence();
        uint32_t retired = 0u;
        std::vector<std::string> order;
        Context::RhiCommandListBatch batch;
        batch.commandLists = {first, second};
        batch.retirementFence = frameFence;
        batch.acquireCommandBuffer = [](size_t, RHI::QueueType, uint64_t)
            { return std::make_shared<StubCommandBuffer>(); };
        batch.submit = [&order, signalOnPresent, earlyFence](const auto& translated)
        {
            EXPECT_EQ(translated.size(), 2u);
            EXPECT_EQ(translated[0].signalFence, nullptr);
            EXPECT_EQ(translated[1].signalFence, earlyFence);
            order.push_back("Submit");
            RHI::RHIQueueOperationResult result;
            result.frameFenceSignalQueued = !signalOnPresent;
            return result;
        };
        batch.requestsPresent = true;
        batch.present = [&order, signalOnPresent](const auto&)
        {
            order.push_back("Present");
            RHI::RHIQueueOperationResult result;
            result.frameFenceSignalQueued = signalOnPresent;
            return result;
        };
        batch.retireCommandBuffer = [&retired](const auto&) { ++retired; };
        Context::RhiSubmitEvent completed;
        ASSERT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
        EXPECT_TRUE(completed.Succeeded()) << completed.GetDiagnostic();
        EXPECT_EQ(order, (std::vector<std::string>{"Submit", "Present"}));
        EXPECT_EQ(executor.GetPresentSequence(), completed.GetSequence());
        owner.reset();
        first.reset();
        second.reset();

        EXPECT_EQ(executor.ReleaseRetiredBatches(), 0u);
        EXPECT_FALSE(firstGpu.IsComplete());
        EXPECT_FALSE(secondGpu.IsComplete());
        EXPECT_FALSE(weakOwner.expired());
        EXPECT_EQ(retired, 0u);

        frameFence->signaled.store(true);
        EXPECT_EQ(executor.ReleaseRetiredBatches(), 2u);
        EXPECT_TRUE(firstGpu.Succeeded());
        EXPECT_TRUE(secondGpu.Succeeded());
        EXPECT_TRUE(weakOwner.expired());
        EXPECT_EQ(retired, 2u);
        executor.Shutdown();
        executor.ResetForTesting();
    }
}

TEST(RhiCommandListExecutorTests, CompletedQueueRetiresBeforeItsReusableFenceIsReset)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    RhiExecutorTargets targets;
    targets.submit = [](const auto&) { return NLS::Render::RHI::RHIQueueOperationResult{}; };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    auto pendingFence = std::make_shared<StubFence>();
    auto completedFence = std::make_shared<StubFence>();
    completedFence->signaled.store(true);
    auto pending = MakeList("PendingCompute");
    pending->SetQueueType(NLS::Render::RHI::QueueType::Compute);
    pending->SetSignalFence(pendingFence);
    pending->Close();
    auto completed = MakeList("CompletedGraphics");
    completed->SetSignalFence(completedFence);
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weakOwner = owner;
    completed->Record("KeepOwner", [owner](NLS::Render::RHI::RHICommandBuffer&) {});
    completed->Close();
    auto pendingGpu = pending->GetGpuFence();
    auto completedGpu = completed->GetGpuFence();
    ASSERT_TRUE(executor.Dispatch(pending));
    ASSERT_TRUE(executor.Dispatch(completed));
    owner.reset();
    pending.reset();
    completed.reset();
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 1u);
    EXPECT_FALSE(pendingGpu.IsComplete());
    EXPECT_TRUE(completedGpu.Succeeded());
    EXPECT_TRUE(weakOwner.expired());

    completedFence->Reset();
    pendingFence->signaled.store(true);
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 1u);
    EXPECT_TRUE(pendingGpu.Succeeded());
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, EmptyBatchDoesNotRequireAnUnqueuedFrameFence)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    auto frameFence = std::make_shared<StubFence>();
    std::vector<std::string> order;
    Context::RhiCommandListBatch empty;
    empty.retirementFence = frameFence;
    empty.submitSignalsFences = false;
    empty.submit = [&order](const auto& translated)
    {
        EXPECT_TRUE(translated.empty());
        order.push_back("Empty");
        return RHI::RHIQueueOperationResult{};
    };
    Context::RhiSubmitEvent emptyCompleted;
    ASSERT_TRUE(executor.DispatchBatch(std::move(empty), &emptyCompleted));
    EXPECT_TRUE(emptyCompleted.Succeeded()) << emptyCompleted.GetDiagnostic();
    EXPECT_FALSE(executor.HasFailed());
    EXPECT_FALSE(frameFence->IsSignaled());
    EXPECT_EQ(executor.GetSubmitSequence(), emptyCompleted.GetSequence());

    auto list = MakeList("FollowingEmptyFrame");
    list->Close();
    Context::RhiCommandListBatch next;
    next.commandLists = {list};
    next.retirementFence = frameFence;
    next.submit = [&order](const auto& translated)
    {
        EXPECT_EQ(translated.size(), 1u);
        order.push_back("Submit");
        RHI::RHIQueueOperationResult result;
        result.mayHaveQueuedGpuWork = true;
        result.frameFenceSignalQueued = true;
        return result;
    };
    Context::RhiSubmitEvent nextCompleted;
    ASSERT_TRUE(executor.DispatchBatch(std::move(next), &nextCompleted));
    EXPECT_TRUE(nextCompleted.Succeeded()) << nextCompleted.GetDiagnostic();
    EXPECT_EQ(nextCompleted.GetSequence(), emptyCompleted.GetSequence() + 1u);
    EXPECT_EQ(order, (std::vector<std::string>{"Empty", "Submit"}));
    EXPECT_FALSE(list->GetGpuFence().IsComplete());
    frameFence->signaled.store(true);
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 1u);
    EXPECT_TRUE(list->GetGpuFence().Succeeded());
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, EmptyBatchWithQueueWorkStillRequiresRetirementFence)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    Context::RhiCommandListBatch batch;
    batch.retirementFence = std::make_shared<StubFence>();
    batch.submitSignalsFences = false;
    batch.submit = [](const auto& translated)
    {
        EXPECT_TRUE(translated.empty());
        RHI::RHIQueueOperationResult result;
        result.mayHaveQueuedGpuWork = true;
        return result;
    };
    Context::RhiSubmitEvent completed;
    ASSERT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_TRUE(completed.IsComplete());
    EXPECT_FALSE(completed.Succeeded());
    EXPECT_NE(completed.GetDiagnostic().find("did not queue its GPU retirement fence"), std::string::npos);
    EXPECT_FALSE(executor.Flush(RhiFlushFlags::Submit));
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, MissingBatchFenceSignalFailsAndQuarantinesGpuOwners)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weakOwner = owner;
    auto frameFence = std::make_shared<StubFence>();
    frameFence->signaled.store(true);
    auto list = MakeList("UnconfirmedFrameFence");
    list->SetSignalFence(frameFence);
    list->Record("KeepOwner", [owner](RHI::RHICommandBuffer&) {});
    list->Close();
    auto gpu = list->GetGpuFence();
    uint32_t retired = 0u;
    Context::RhiCommandListBatch batch;
    batch.commandLists = {list};
    batch.retirementFence = frameFence;
    batch.submit = [](const auto&) { return RHI::RHIQueueOperationResult{}; };
    batch.retireCommandBuffer = [&retired](const auto&) { ++retired; };
    Context::RhiSubmitEvent completed;
    ASSERT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_TRUE(completed.IsComplete());
    EXPECT_FALSE(completed.Succeeded());
    EXPECT_NE(completed.GetDiagnostic().find("did not queue its GPU retirement fence"), std::string::npos);
    EXPECT_TRUE(gpu.IsComplete());
    EXPECT_FALSE(gpu.Succeeded());
    owner.reset();
    list.reset();
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 0u);
    EXPECT_FALSE(weakOwner.expired());
    executor.Shutdown();
    EXPECT_FALSE(weakOwner.expired());
    EXPECT_EQ(retired, 0u);
    executor.ResetForTesting();
    EXPECT_TRUE(weakOwner.expired());
}

TEST(RhiCommandListExecutorTests, BatchFailureBeforeQueueWorkReleasesOwnersWithoutWaitingForFence)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> weakOwner = owner;
    auto frameFence = std::make_shared<StubFence>();
    auto list = MakeList("RejectedBeforeSubmit");
    list->Record("KeepOwner", [owner](RHI::RHICommandBuffer&) {});
    list->Close();
    auto gpu = list->GetGpuFence();
    uint32_t retired = 0u;
    Context::RhiCommandListBatch batch;
    batch.commandLists = {list};
    batch.retirementFence = frameFence;
    batch.submit = [](const auto&)
    {
        return RHI::RHIQueueOperationResult{
            RHI::RHIQueueOperationStatusCode::BackendFailure, "queue rejected before GPU work"};
    };
    batch.retireCommandBuffer = [&retired](const auto&) { ++retired; };
    Context::RhiSubmitEvent completed;
    ASSERT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    EXPECT_FALSE(completed.Succeeded());
    EXPECT_TRUE(gpu.IsComplete());
    EXPECT_FALSE(gpu.Succeeded());
    owner.reset();
    list.reset();
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 1u);
    EXPECT_TRUE(weakOwner.expired());
    EXPECT_EQ(retired, 1u);
    EXPECT_FALSE(frameFence->IsSignaled());
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, FailedBatchDoesNotReleaseAnAllocatorStillUsedByAnotherTranslation)
{
    using namespace NLS::Render;
    NLS::Base::TaskGraph::TaskGraph graph({4u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::Tasks, {}, &graph);
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic_bool active = false;
    std::atomic_bool failingStarted = false;
    std::atomic_uint32_t submits = 0u;
    std::atomic_uint32_t retires = 0u;
    auto running = MakeList("RunningTranslate");
    running->Record("Wait", [&active, released](RHI::RHICommandBuffer&)
    {
        active.store(true);
        released.wait();
        active.store(false);
    });
    running->Close();
    auto failing = MakeList("FailingTranslate");
    failing->Record("Failure", [&active, &failingStarted](RHI::RHICommandBuffer&)
    {
        failingStarted.store(true);
        (void)WaitUntil([&] { return active.load(); }, 2s);
        throw std::runtime_error("batch translate failure");
    });
    failing->Close();
    Context::RhiCommandListBatch batch;
    batch.commandLists = {running, failing};
    batch.acquireCommandBuffer = [](size_t, RHI::QueueType, uint64_t)
        { return std::make_shared<StubCommandBuffer>(); };
    batch.submit = [&submits](const auto&)
    {
        ++submits;
        return RHI::RHIQueueOperationResult {};
    };
    batch.retireCommandBuffer = [&retires, &active](const auto&)
    {
        EXPECT_FALSE(active.load());
        ++retires;
    };
    Context::RhiSubmitEvent completed;
    EXPECT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    EXPECT_TRUE(WaitUntil([&] { return failingStarted.load() && executor.HasFailed(); }, 3s));
    EXPECT_TRUE(running->GetSubmitEvent().IsComplete());
    EXPECT_FALSE(running->GetSubmitEvent().Succeeded());
    EXPECT_FALSE(completed.IsComplete());
    EXPECT_EQ(retires.load(), 0u);
    EXPECT_EQ(submits.load(), 0u);
    release.set_value();
    EXPECT_TRUE(WaitUntil([&] { return completed.IsComplete(); }, 2s));
    EXPECT_FALSE(completed.Succeeded());
    EXPECT_EQ(retires.load(), 2u);
    EXPECT_NE(completed.GetDiagnostic().find("batch translate failure"), std::string::npos);
    executor.Shutdown();
    graph.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, SerialBackendBatchUsesTheSamePipelineWithoutConcurrentTranslation)
{
    using namespace NLS::Render;
    NLS::Base::TaskGraph::TaskGraph graph({4u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::Tasks, {}, &graph);
    std::atomic_uint32_t active = 0u;
    std::atomic_uint32_t translatedCount = 0u;
    Context::RhiCommandListBatch batch;
    batch.parallelTranslation = false;
    for (size_t index = 0u; index < 8u; ++index)
    {
        auto list = MakeList("SerialBackend");
        list->Record("Translate", [&active, &translatedCount](RHI::RHICommandBuffer&)
        {
            EXPECT_EQ(active.fetch_add(1u), 0u);
            ++translatedCount;
            active.fetch_sub(1u);
        });
        list->Close();
        batch.commandLists.push_back(std::move(list));
    }
    batch.acquireCommandBuffer = [](size_t, RHI::QueueType, uint64_t)
        { return std::make_shared<StubCommandBuffer>(); };
    batch.submit = [&translatedCount](const auto&)
    {
        EXPECT_EQ(translatedCount.load(), 8u);
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
        return RHI::RHIQueueOperationResult {};
    };
    Context::RhiSubmitEvent completed;
    EXPECT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    EXPECT_TRUE(WaitUntil([&] { return completed.IsComplete(); }, 2s));
    EXPECT_TRUE(completed.Succeeded());
    executor.Shutdown();
    graph.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, NoneBatchTranslatesOnWorkersAndSubmitsOnItsCaller)
{
    using namespace NLS::Render;
    NLS::Base::TaskGraph::TaskGraph graph({1u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {}, &graph);
    const auto caller = std::this_thread::get_id();
    auto list = MakeList("NoneTranslate");
    list->Record("Worker", [&graph, caller](RHI::RHICommandBuffer&)
    {
        EXPECT_TRUE(graph.IsWorkerThread());
        EXPECT_NE(std::this_thread::get_id(), caller);
    });
    list->Close();
    Context::RhiCommandListBatch batch;
    batch.commandLists = {list};
    batch.acquireCommandBuffer = [](size_t, RHI::QueueType, uint64_t)
        { return std::make_shared<StubCommandBuffer>(); };
    batch.submit = [caller](const auto&)
    {
        EXPECT_EQ(std::this_thread::get_id(), caller);
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
        return RHI::RHIQueueOperationResult {};
    };
    Context::RhiSubmitEvent completed;
    EXPECT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    EXPECT_TRUE(completed.Succeeded());
    executor.Shutdown();
    graph.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, CompletedBatchDropsCpuFrameCapturesBeforeRetiringGpuLists)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    auto cpuFrame = std::make_shared<int>(7);
    std::weak_ptr<int> cpuProbe = cpuFrame;
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> gpuProbe = resource;
    auto fence = std::make_shared<StubFence>();
    auto list = MakeList("CpuAndGpuLifetime");
    list->Record("OwnedResource", [resource](RHI::RHICommandBuffer&) {});
    list->SetSignalFence(fence);
    list->Close();
    Context::RhiCommandListBatch batch;
    batch.commandLists = {list};
    batch.acquireCommandBuffer = [cpuFrame](size_t, RHI::QueueType, uint64_t)
        { return std::make_shared<StubCommandBuffer>(); };
    batch.submit = [cpuFrame](const auto&) { return RHI::RHIQueueOperationResult {}; };
    batch.onComplete = [cpuFrame] {};
    cpuFrame.reset();
    Context::RhiSubmitEvent completed;
    EXPECT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
    EXPECT_TRUE(completed.Succeeded());
    EXPECT_TRUE(cpuProbe.expired());
    resource.reset();
    list.reset();
    EXPECT_FALSE(gpuProbe.expired());
    fence->signaled.store(true);
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 1u);
    EXPECT_TRUE(gpuProbe.expired());
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, UnusedResourcesReleaseWithoutGpuWorkOrFenceTarget)
{
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> probe = resource;
    const auto completed = executor.DeferRelease({std::move(resource)});
    EXPECT_TRUE(completed.Succeeded());
    EXPECT_TRUE(probe.expired());
    EXPECT_EQ(executor.GetPendingResourceReleaseCount(), 0u);
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, ResourceRetirementWaitsForEveryQueueAndCpuFlushDoesNotWaitGpu)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    const std::vector<std::shared_ptr<StubFence>> fences {
        std::make_shared<StubFence>(), std::make_shared<StubFence>(), std::make_shared<StubFence>() };
    size_t markerCalls = 0u;
    RhiExecutorTargets targets;
    targets.signalGpuFences = [&fences, &markerCalls](auto, auto& output)
    {
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
        ++markerCalls;
        output.assign(fences.begin(), fences.end());
        return RHI::RHIQueueOperationResult { RHI::RHIQueueOperationStatusCode::Success, {}, true, true };
    };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    ASSERT_TRUE(executor.RunOnSubmitLane("NativeUpload", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> probe = resource;
    const auto completed = executor.DeferRelease({std::move(resource)});
    EXPECT_EQ(markerCalls, 1u);
    EXPECT_FALSE(completed.IsComplete());
    EXPECT_TRUE(executor.Flush(RhiFlushFlags::Submit | RhiFlushFlags::ReleaseResources));
    EXPECT_FALSE(probe.expired());
    for (size_t i = 0u; i < fences.size(); ++i)
    {
        fences[i]->signaled.store(true);
        EXPECT_EQ(executor.ReleaseRetiredBatches(), i + 1u == fences.size() ? 1u : 0u);
        EXPECT_EQ(probe.expired(), i + 1u == fences.size());
    }
    EXPECT_TRUE(completed.Succeeded());
    EXPECT_TRUE(executor.Flush(RhiFlushFlags::Gpu));
    EXPECT_EQ(executor.GetPendingResourceReleaseCount(), 0u);
    executor.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, ResourceRetirementStaysAfterEarlierTranslationAndBeforeLaterPresent)
{
    using namespace NLS::Render;
    NLS::Base::TaskGraph::TaskGraph graph({2u, false, true});
    ASSERT_TRUE(graph.Start());
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    std::promise<void> releaseTranslate;
    auto release = releaseTranslate.get_future().share();
    std::atomic_bool translating { false };
    std::mutex orderMutex;
    std::vector<std::string> order;
    auto fence = std::make_shared<StubFence>();
    RhiExecutorTargets targets;
    targets.acquireCommandBuffer = [](auto, auto) { return std::make_shared<StubCommandBuffer>(); };
    targets.submit = [&orderMutex, &order](const auto&)
    {
        std::lock_guard lock(orderMutex);
        order.push_back("scene");
        return RHI::RHIQueueOperationResult {};
    };
    targets.signalGpuFences = [&graph, &orderMutex, &order, fence](auto, auto& output)
    {
        EXPECT_TRUE(graph.IsWorkerThread());
        EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
        std::lock_guard lock(orderMutex);
        order.push_back("retirement");
        output.push_back(fence);
        return RHI::RHIQueueOperationResult { RHI::RHIQueueOperationStatusCode::Success, {}, true, true };
    };
    executor.Initialize(RhiThreadMode::Tasks, std::move(targets), &graph);
    auto list = MakeList("PendingScene");
    list->Record("BlockTranslation", [&translating, release](auto&)
    {
        translating.store(true);
        release.wait();
    });
    list->Close();
    EXPECT_TRUE(list->Submit());
    const bool started = WaitUntil([&] { return translating.load(); }, 2s);
    EXPECT_TRUE(started);
    const auto retirement = executor.DeferRelease({std::make_shared<int>(42)});
    {
        std::lock_guard lock(orderMutex);
        EXPECT_TRUE(order.empty());
    }
    releaseTranslate.set_value();
    EXPECT_TRUE(executor.RunOnSubmitLane("LaterPresent", [&orderMutex, &order]
    {
        std::lock_guard lock(orderMutex);
        order.push_back("present");
        return RHI::RHIQueueOperationResult {};
    }, true).Succeeded());
    EXPECT_TRUE(executor.Flush(RhiFlushFlags::Submit));
    {
        std::lock_guard lock(orderMutex);
        EXPECT_EQ(order, (std::vector<std::string>{"scene", "retirement", "present"}));
    }
    EXPECT_FALSE(retirement.IsComplete());
    fence->signaled.store(true);
    (void)executor.ReleaseRetiredBatches();
    EXPECT_TRUE(retirement.Succeeded());
    executor.Shutdown(RhiFlushFlags::Gpu);
    graph.Shutdown();
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, FailedRetirementSignalsWaitersAndQuarantinesOwners)
{
    using namespace NLS::Render;
    for (const bool throwFromTarget : {false, true})
    {
        SCOPED_TRACE(throwFromTarget);
        auto& executor = RhiCommandListExecutor::Get();
        executor.ResetForTesting();
        auto fence = std::make_shared<StubFence>();
        RhiExecutorTargets targets;
        targets.signalGpuFences = [fence, throwFromTarget](auto, auto& output) -> RHI::RHIQueueOperationResult
        {
            output.push_back(fence);
            if (throwFromTarget)
                throw std::runtime_error("Injected retirement exception");
            return { RHI::RHIQueueOperationStatusCode::BackendFailure, "Injected retirement signal failure", true };
        };
        executor.Initialize(RhiThreadMode::None, std::move(targets));
        ASSERT_TRUE(executor.RunOnSubmitLane("EarlierUpload", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
        auto resource = std::make_shared<int>(42);
        std::weak_ptr<int> probe = resource;
        const auto completed = executor.DeferRelease({std::move(resource)});
        EXPECT_TRUE(completed.WaitFor(100ms));
        EXPECT_FALSE(completed.Succeeded());
        EXPECT_NE(completed.GetDiagnostic().find("Injected retirement"), std::string::npos);
        EXPECT_TRUE(executor.HasFailed());
        fence->signaled.store(true);
        EXPECT_EQ(executor.ReleaseRetiredBatches(), 0u);
        EXPECT_EQ(executor.GetQuarantinedResourceReleaseCount(), 1u);
        EXPECT_FALSE(probe.expired());
        executor.Shutdown();
        EXPECT_FALSE(probe.expired());
        executor.ResetForTesting();
        EXPECT_TRUE(probe.expired());
    }
}

TEST(RhiCommandListExecutorTests, RetirementWithoutConfirmedFencesFailsRatherThanReleasingOwners)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    RhiExecutorTargets targets;
    targets.signalGpuFences = [](auto, auto&) { return RHI::RHIQueueOperationResult {}; };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    ASSERT_TRUE(executor.RunOnSubmitLane("UnfencedNative", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> probe = resource;
    const auto completed = executor.DeferRelease({std::move(resource)});
    EXPECT_TRUE(completed.IsComplete());
    EXPECT_FALSE(completed.Succeeded());
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 0u);
    EXPECT_FALSE(probe.expired());
    executor.Shutdown();
    executor.ResetForTesting();
    EXPECT_TRUE(probe.expired());
}

TEST(RhiCommandListExecutorTests, ShutdownWithoutGpuCompletionKeepsResourceOwners)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    auto fence = std::make_shared<StubFence>();
    RhiExecutorTargets targets;
    targets.signalGpuFences = [fence](auto, auto& output)
    {
        output.push_back(fence);
        return RHI::RHIQueueOperationResult { RHI::RHIQueueOperationStatusCode::Success, {}, true, true };
    };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    ASSERT_TRUE(executor.RunOnSubmitLane("Scene", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> probe = resource;
    const auto completed = executor.DeferRelease({std::move(resource)});
    executor.Shutdown(RhiFlushFlags::Submit);
    EXPECT_TRUE(completed.WaitFor(100ms));
    EXPECT_FALSE(completed.Succeeded());
    EXPECT_FALSE(probe.expired());
    EXPECT_EQ(executor.GetQuarantinedResourceReleaseCount(), 1u);
    fence->signaled.store(true);
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 0u);
    EXPECT_FALSE(probe.expired());
    executor.ResetForTesting();
    EXPECT_TRUE(probe.expired());
}

TEST(RhiCommandListExecutorTests, GpuFlushCoversNativeWorkAndPermitsLateResourceDestruction)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    auto fence = std::make_shared<StubFence>();
    std::atomic_uint markerCalls { 0u };
    RhiExecutorTargets targets;
    targets.signalGpuFences = [fence, &markerCalls](auto, auto& output)
    {
        output.push_back(fence);
        ++markerCalls;
        return RHI::RHIQueueOperationResult { RHI::RHIQueueOperationStatusCode::Success, {}, true, true };
    };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    ASSERT_TRUE(executor.RunOnSubmitLane("NativeReadback", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    auto flushing = std::async(std::launch::async, [&executor] { return executor.Flush(RhiFlushFlags::Gpu); });
    const bool markerSubmitted = WaitUntil([&] { return markerCalls.load() == 1u; }, 2s);
    EXPECT_TRUE(markerSubmitted);
    EXPECT_EQ(flushing.wait_for(20ms), std::future_status::timeout);
    fence->signaled.store(true);
    EXPECT_TRUE(flushing.get());
    executor.Shutdown(RhiFlushFlags::Gpu);
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> probe = resource;
    const auto completed = executor.DeferRelease({std::move(resource)});
    EXPECT_TRUE(completed.Succeeded());
    EXPECT_TRUE(probe.expired());
    EXPECT_EQ(executor.GetQuarantinedResourceReleaseCount(), 0u);
    EXPECT_EQ(markerCalls.load(), 1u);
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, MissingRetirementTargetWithPriorWorkQuarantinesResources)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    executor.Initialize(RhiThreadMode::None, {});
    ASSERT_TRUE(executor.RunOnSubmitLane("NativeUpload", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> probe = resource;
    const auto completed = executor.DeferRelease({std::move(resource)});
    EXPECT_TRUE(completed.IsComplete());
    EXPECT_FALSE(completed.Succeeded());
    EXPECT_TRUE(executor.HasFailed());
    EXPECT_FALSE(probe.expired());
    EXPECT_EQ(executor.GetQuarantinedResourceReleaseCount(), 1u);
    executor.Shutdown();
    executor.ResetForTesting();
    EXPECT_TRUE(probe.expired());
}

TEST(RhiCommandListExecutorTests, ShutdownReleasesDeviceTargetsAfterGpuRetirement)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    auto deviceOwner = std::make_shared<int>(42);
    std::weak_ptr<int> probe = deviceOwner;
    RhiExecutorTargets targets;
    targets.signalGpuFences = [deviceOwner](auto, auto& output)
    {
        auto fence = std::make_shared<StubFence>();
        fence->signaled.store(true);
        output.push_back(std::move(fence));
        return RHI::RHIQueueOperationResult { RHI::RHIQueueOperationStatusCode::Success, {}, true, true };
    };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    deviceOwner.reset();
    ASSERT_TRUE(executor.RunOnSubmitLane("Native", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    EXPECT_FALSE(probe.expired());
    executor.Shutdown(RhiFlushFlags::Gpu);
    EXPECT_TRUE(probe.expired());
    executor.ResetForTesting();
}

TEST(RhiCommandListExecutorTests, UnconfirmedGpuShutdownQuarantinesDeviceTargets)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    auto deviceOwner = std::make_shared<int>(42);
    std::weak_ptr<int> probe = deviceOwner;
    RhiExecutorTargets targets;
    targets.signalGpuFences = [deviceOwner](auto, auto& output)
    {
        output.push_back(std::make_shared<StubFence>());
        return RHI::RHIQueueOperationResult { RHI::RHIQueueOperationStatusCode::Success, {}, true, true };
    };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    deviceOwner.reset();
    ASSERT_TRUE(executor.RunOnSubmitLane("Native", [] { return RHI::RHIQueueOperationResult {}; }).Succeeded());
    const auto retirement = executor.InsertGpuFence();
    executor.Shutdown(RhiFlushFlags::Submit);
    EXPECT_TRUE(retirement.IsComplete());
    EXPECT_FALSE(retirement.Succeeded());
    EXPECT_FALSE(probe.expired());
    executor.ResetForTesting();
    EXPECT_TRUE(probe.expired());
}

TEST(RhiCommandListExecutorTests, GlobalGpuCompletionRetiresEarlierListWithoutReusingFrameFenceState)
{
    using namespace NLS::Render;
    auto& executor = RhiCommandListExecutor::Get();
    executor.ResetForTesting();
    auto frameFence = std::make_shared<StubFence>();
    auto globalFence = std::make_shared<StubFence>();
    globalFence->signaled.store(true);
    size_t retiredLists = 0u;
    RhiExecutorTargets targets;
    targets.acquireCommandBuffer = [](auto, auto) { return std::make_shared<StubCommandBuffer>(); };
    targets.submit = [](const auto&) { return RHI::RHIQueueOperationResult {}; };
    targets.retireCommandBuffer = [&retiredLists](const auto&) { ++retiredLists; };
    targets.signalGpuFences = [globalFence](auto, auto& output)
    {
        output.push_back(globalFence);
        return RHI::RHIQueueOperationResult { RHI::RHIQueueOperationStatusCode::Success, {}, true, true };
    };
    executor.Initialize(RhiThreadMode::None, std::move(targets));
    auto resource = std::make_shared<int>(42);
    std::weak_ptr<int> probe = resource;
    auto list = MakeList("EarlierGpuOwners");
    const auto completed = list->GetGpuFence();
    list->Record("OwnedResource", [resource](auto&) {});
    list->SetSignalFence(frameFence);
    list->Close();
    ASSERT_TRUE(list->Submit());
    resource.reset();
    list.reset();
    EXPECT_FALSE(probe.expired());
    EXPECT_FALSE(completed.IsComplete());
    EXPECT_EQ(executor.ReleaseRetiredBatches(), 0u);
    ASSERT_TRUE(executor.Flush(RhiFlushFlags::Gpu | RhiFlushFlags::ReleaseResources));
    EXPECT_FALSE(frameFence->IsSignaled());
    EXPECT_TRUE(completed.Succeeded());
    EXPECT_TRUE(probe.expired());
    EXPECT_EQ(retiredLists, 1u);
    executor.Shutdown();
    executor.ResetForTesting();
}
