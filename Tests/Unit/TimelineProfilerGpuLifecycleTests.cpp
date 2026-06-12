#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12Device.h"

#include <chrono>
#include <d3d12.h>
#include <wrl/client.h>
#endif

#include "Rendering/RHI/Backends/DX12/DX12PresentPolicy.h"
#include "UI/ImGuiExtensions/TimelineProfiler/Profiler.h"
#include "UI/Profiling/TimelineProfilerLimits.h"
#include "UI/Profiling/TimelineProfilerSink.h"

namespace
{
std::string ReadSourceFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

size_t FindMatchingBrace(const std::string& source, const size_t openingBrace)
{
    size_t depth = 0u;
    for (size_t index = openingBrace; index < source.size(); ++index)
    {
        if (source[index] == '{')
        {
            ++depth;
        }
        else if (source[index] == '}')
        {
            if (depth == 0u)
                return std::string::npos;
            --depth;
            if (depth == 0u)
                return index;
        }
    }
    return std::string::npos;
}

}

TEST(TimelineProfilerGpuLifecycleTests, BuildConfigurationExposesExpectedProfilerHelpers)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EXPECT_FALSE(TimelineProfilerDetail::ShouldUnregisterCommandListDestructionCallback(true, 1u));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldUnregisterCommandListDestructionCallback(false, 1u));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldPublishGpuQueryPair(false));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldPublishGpuQueryPair(true));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldAdvanceGpuProfilerFrame(true, false));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldAdvanceGpuProfilerFrame(false, true));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldAdvanceGpuProfilerFrame(false, false));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldSubmitGpuProfilerReadback(0u));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldSubmitGpuProfilerReadback(1u));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldSubmitGpuProfilerReadback(64u));
    EXPECT_EQ(NLS::UI::Profiling::kTimelineProfilerInternalCpuStackDepth, 32u);
    EXPECT_EQ(NLS::UI::Profiling::kTimelineProfilerMaxCpuScopeDepth, 31u);
    EXPECT_EQ(Profiler::EventTrack::MAX_STACK_DEPTH,
              static_cast<int>(NLS::UI::Profiling::kTimelineProfilerInternalCpuStackDepth));
#if defined(_WIN32)
    EXPECT_EQ(
        TimelineProfilerDetail::ResolveGpuProfilerQueryHeapIndex(D3D12_COMMAND_LIST_TYPE_DIRECT),
        0u);
    EXPECT_EQ(
        TimelineProfilerDetail::ResolveGpuProfilerQueryHeapIndex(D3D12_COMMAND_LIST_TYPE_COPY),
        1u);
    EXPECT_TRUE(TimelineProfilerDetail::ShouldProfileGpuProfilerCommandListType(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        false));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldProfileGpuProfilerCommandListType(
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        true));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldProfileGpuProfilerCommandListType(
        D3D12_COMMAND_LIST_TYPE_COPY,
        false));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldProfileGpuProfilerCommandListType(
        D3D12_COMMAND_LIST_TYPE_COPY,
        true));
#endif
    EXPECT_GT(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(5.0f, 1.0f), 5.0f);
    EXPECT_LT(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(5.0f, -1.0f), 5.0f);
    EXPECT_FLOAT_EQ(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(1.0f, -20.0f), 1.0f);
    EXPECT_FLOAT_EQ(TimelineProfilerDetail::ComputeTimelineWheelZoomScale(100.0f, 20.0f), 100.0f);
#else
    SUCCEED() << "TimelineProfiler is disabled in this build.";
#endif
}

TEST(DX12PresentPolicyTests, VsyncControlsPresentSyncInterval)
{
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12PresentSyncInterval(true), 1u);
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12PresentSyncInterval(false), 0u);
}

TEST(TimelineProfilerGpuLifecycleTests, GpuTickSkipsReadbackSubmissionForEmptyFrames)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    const auto profilerSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp");

    const auto tickFunction = profilerSource.find("void GPUProfiler::Tick()");
    ASSERT_NE(tickFunction, std::string::npos);
    const auto nextFunction = profilerSource.find("void GPUProfiler::ExecuteCommandLists", tickFunction);
    ASSERT_NE(nextFunction, std::string::npos);
    const auto tickBody = profilerSource.substr(tickFunction, nextFunction - tickFunction);

    const auto readbackDecision = tickBody.find("ShouldSubmitGpuProfilerReadback");
    const auto currentSlotWait = tickBody.find("PrepareFrameSlotForReuseUnlocked(m_FrameIndex, shouldSubmitReadback)");
    const auto emptySlotWait = tickBody.find("PrepareFrameSlotForReuseUnlocked(nextFrameIndex, shouldSubmitReadback)");
    const auto emptyFrameCompletion = tickBody.find("MarkFrameCompleteWithoutReadback");
    const auto resolve = tickBody.find("Resolve(m_FrameIndex)");
    const auto reusableSlotReset = tickBody.find("heap.ResetReusableFrameSlot(m_FrameIndex)");

    ASSERT_NE(readbackDecision, std::string::npos);
    ASSERT_NE(currentSlotWait, std::string::npos);
    ASSERT_NE(emptySlotWait, std::string::npos);
    ASSERT_NE(emptyFrameCompletion, std::string::npos);
    ASSERT_NE(resolve, std::string::npos);
    ASSERT_NE(reusableSlotReset, std::string::npos);
    EXPECT_LT(readbackDecision, currentSlotWait);
    EXPECT_LT(currentSlotWait, emptyFrameCompletion)
        << "The current readback slot must be reusable before an empty frame can overwrite its submitted state.";
    EXPECT_LT(currentSlotWait, resolve)
        << "The current readback slot must be reusable before a non-empty frame can resolve into it.";
    EXPECT_LT(readbackDecision, emptySlotWait);
    EXPECT_LT(emptySlotWait, emptyFrameCompletion)
        << "Empty GPU frames must not close the local command list before the next slot is reusable.";
    EXPECT_LT(readbackDecision, resolve);
    EXPECT_LT(emptyFrameCompletion, resolve);
    EXPECT_LT(resolve, reusableSlotReset)
        << "Empty GPU frames must still advance the local command-list allocator slot after skipping resolve.";
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, GpuProfilerResolveQueueOperationsUseSharedDx12QueueLock)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    const auto profilerSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp");

    EXPECT_NE(
        profilerSource.find("Rendering/RHI/Backends/DX12/DX12QueueSynchronization.h"),
        std::string::npos)
        << "TimelineProfiler's internal DX12 resolve submissions must participate in the same queue "
           "transaction lock as the RHI queue, UI bridge, uploads, and readbacks.";

    const auto drainQueueFunction = profilerSource.find("bool GPUProfiler::QueryHeap::DrainQueue");
    ASSERT_NE(drainQueueFunction, std::string::npos);
    const auto shutdownFunction = profilerSource.find("bool GPUProfiler::QueryHeap::Shutdown", drainQueueFunction);
    ASSERT_NE(shutdownFunction, std::string::npos);
    const auto drainQueueBody = profilerSource.substr(drainQueueFunction, shutdownFunction - drainQueueFunction);

    const auto drainQueueLock = drainQueueBody.find("ScopedDX12QueueLock");
    const auto drainQueueSignal = drainQueueBody.find("pQueue->Signal");
    ASSERT_NE(drainQueueSignal, std::string::npos);
    ASSERT_NE(drainQueueLock, std::string::npos);
    EXPECT_LT(drainQueueLock, drainQueueSignal)
        << "Shutdown/drain fence signals must be ordered under the shared DX12 queue lock.";
    const auto drainQueueWait = drainQueueBody.find("WaitForGpuProfilerFenceValue");
    ASSERT_NE(drainQueueWait, std::string::npos);
    const auto drainQueueLockBlockStart = drainQueueBody.rfind('{', drainQueueLock);
    ASSERT_NE(drainQueueLockBlockStart, std::string::npos);
    const auto drainQueueLockBlockEnd = FindMatchingBrace(drainQueueBody, drainQueueLockBlockStart);
    ASSERT_NE(drainQueueLockBlockEnd, std::string::npos);
    EXPECT_LT(drainQueueLock, drainQueueSignal);
    EXPECT_LT(drainQueueSignal, drainQueueLockBlockEnd)
        << "The drain fence signal must remain inside the shared DX12 queue lock scope.";
    EXPECT_LT(drainQueueLockBlockEnd, drainQueueWait)
        << "Profiler shutdown must not hold the shared DX12 queue lock while blocking on a CPU fence wait.";

    const auto resolveFunction = profilerSource.find("bool GPUProfiler::QueryHeap::Resolve");
    ASSERT_NE(resolveFunction, std::string::npos);
    const auto markFrameFunction = profilerSource.find(
        "void GPUProfiler::QueryHeap::MarkFrameCompleteWithoutReadback",
        resolveFunction);
    ASSERT_NE(markFrameFunction, std::string::npos);
    const auto resolveBody = profilerSource.substr(resolveFunction, markFrameFunction - resolveFunction);

    const auto resolveLock = resolveBody.find("ScopedDX12QueueLock");
    const auto resolveExecute = resolveBody.find("m_pResolveQueue->ExecuteCommandLists");
    const auto resolveSignal = resolveBody.find("m_pResolveQueue->Signal");
    ASSERT_NE(resolveExecute, std::string::npos);
    ASSERT_NE(resolveSignal, std::string::npos);
    ASSERT_NE(resolveLock, std::string::npos);
    EXPECT_LT(resolveLock, resolveExecute)
        << "Profiler resolve command lists must not race normal graphics/UI submissions.";
    EXPECT_LT(resolveExecute, resolveSignal)
        << "The resolve fence must still be signalled after the resolve command list is queued.";
    const auto resolveLockBlockStart = resolveBody.rfind('{', resolveLock);
    ASSERT_NE(resolveLockBlockStart, std::string::npos);
    const auto resolveLockBlockEnd = FindMatchingBrace(resolveBody, resolveLockBlockStart);
    ASSERT_NE(resolveLockBlockEnd, std::string::npos);
    EXPECT_LT(resolveExecute, resolveLockBlockEnd)
        << "The resolve ExecuteCommandLists call must be inside the shared DX12 queue lock scope.";
    EXPECT_LT(resolveSignal, resolveLockBlockEnd)
        << "The resolve fence Signal call must be inside the same shared DX12 queue lock scope.";
    EXPECT_EQ(
        resolveBody.find("gVerifyHR(m_pResolveQueue->Signal"),
        std::string::npos)
        << "Profiler resolve must not assert after queueing GPU work if the resolve fence Signal fails.";
    EXPECT_NE(
        resolveBody.find("FAILED(signalHr)"),
        std::string::npos)
        << "Profiler resolve must classify/retain failed post-execute Signal state instead of crashing.";
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, Dx12QueueSynchronizationRegistryIsProcessLifetimeAndExported)
{
#if defined(_WIN32)
    const auto synchronizationHeader = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12QueueSynchronization.h");
    const auto synchronizationSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12QueueSynchronization.cpp");

    EXPECT_NE(
        synchronizationHeader.find("NLS_RENDER_API std::mutex* ResolveQueueMutex"),
        std::string::npos)
        << "The queue mutex registry must be exported from NLS_Render so NLS_UI and NLS_Render share one registry.";
    EXPECT_NE(
        synchronizationHeader.find("NLS_RENDER_API void ReleaseQueueMutex"),
        std::string::npos)
        << "Queue mutex release must use the same exported registry as acquisition.";
    EXPECT_EQ(
        synchronizationHeader.find("static std::unordered_map"),
        std::string::npos)
        << "A header-local registry creates one mutex map per DLL and does not serialize profiler/RHI queue users.";
    EXPECT_EQ(
        synchronizationHeader.find("inline std::unordered_map"),
        std::string::npos)
        << "The registry must not be inline header state in the shared-library build.";
    EXPECT_NE(
        synchronizationSource.find("QueueMutexRegistry()"),
        std::string::npos)
        << "The exported registry implementation should live in the NLS_Render source file.";
    EXPECT_EQ(
        synchronizationSource.find(".erase(queue)"),
        std::string::npos)
        << "Queue lock state is handed out as raw pointers, so release must not destroy it while other "
           "DX12 queue/profiler users can still hold a lock object.";
    EXPECT_NE(
        synchronizationSource.find("process-lifetime"),
        std::string::npos)
        << "A non-erasing registry must document that the queue synchronization state intentionally lives "
           "until process shutdown.";
#else
    GTEST_SKIP() << "DX12 queue synchronization is only built on Windows.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, NativeDx12QueuePreservesProfilerSubmitOrderOutsideSharedQueueLock)
{
#if defined(_WIN32)
    const auto queueSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    const auto submitFunction = queueSource.find("NativeDX12Queue::SubmitChecked(");
    ASSERT_NE(submitFunction, std::string::npos);
    const auto presentFunction = queueSource.find("void NativeDX12Queue::Present", submitFunction);
    ASSERT_NE(presentFunction, std::string::npos);
    const auto submitBody = queueSource.substr(submitFunction, presentFunction - submitFunction);

    const auto queueLock = submitBody.find("ScopedDX12QueueLock queueLock(m_queue)");
    const auto profilerEnabled = submitBody.find("Profiler::IsEnabled()");
    const auto profilerSequence = submitBody.find("ReserveQueueProfilerSubmitSequence");
    const auto profilerOrder = submitBody.find("ScopedDX12QueueProfilerSubmissionOrder");
    const auto profilerSubmit = submitBody.find("Profiler::SubmitGpuCommandLists");
    ASSERT_NE(queueLock, std::string::npos);
    ASSERT_NE(profilerEnabled, std::string::npos);
    ASSERT_NE(profilerSequence, std::string::npos);
    ASSERT_NE(profilerOrder, std::string::npos);
    ASSERT_NE(profilerSubmit, std::string::npos);

    const auto queueLockBlockStart = submitBody.rfind('{', queueLock);
    ASSERT_NE(queueLockBlockStart, std::string::npos);
    const auto queueLockBlockEnd = FindMatchingBrace(submitBody, queueLockBlockStart);
    ASSERT_NE(queueLockBlockEnd, std::string::npos);
    EXPECT_LT(profilerEnabled, profilerSequence)
        << "Profiler-disabled DX12 submits should not pay the metadata ordering gate.";
    EXPECT_LT(queueLock, profilerSequence)
        << "Profiler submission order must be reserved while the shared DX12 queue transaction lock is held.";
    EXPECT_LT(profilerSequence, queueLockBlockEnd)
        << "The sequence token must reflect native queue execution order, not post-lock thread scheduling.";
    EXPECT_LT(queueLockBlockEnd, profilerOrder)
        << "Profiler callbacks take TimelineProfiler locks, so they must not run while the shared queue lock is held.";
    EXPECT_LT(profilerOrder, profilerSubmit)
        << "Profiler metadata must wait for its queue-order turn before mutating TimelineProfiler stacks.";
#else
    GTEST_SKIP() << "DX12 queue synchronization is only built on Windows.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, NativeDx12QueuePublishesProfilerMetadataAfterQueuedWorkFailures)
{
#if defined(_WIN32)
    const auto queueSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/RHI/Backends/DX12/DX12Queue.cpp");

    const auto submitFunction = queueSource.find("NativeDX12Queue::SubmitChecked(");
    ASSERT_NE(submitFunction, std::string::npos);
    const auto presentFunction = queueSource.find("void NativeDX12Queue::Present", submitFunction);
    ASSERT_NE(presentFunction, std::string::npos);
    const auto submitBody = queueSource.substr(submitFunction, presentFunction - submitFunction);

    const auto queueLock = submitBody.find("ScopedDX12QueueLock queueLock(m_queue)");
    const auto profilerEnabled = submitBody.find("Profiler::IsEnabled()");
    const auto execute = submitBody.find("m_queue->ExecuteCommandLists", queueLock);
    const auto profilerSequence = submitBody.find("ReserveQueueProfilerSubmitSequence", execute);
    const auto deviceStatusCheck = submitBody.find("device status after ExecuteCommandLists", execute);
    const auto signalSemaphoreFailure = submitBody.find("queue signal semaphore failed", execute);
    const auto signalFenceFailure = submitBody.find("queue signal fence failed", execute);
    const auto profilerSubmit = submitBody.find("Profiler::SubmitGpuCommandLists");
    ASSERT_NE(queueLock, std::string::npos);
    ASSERT_NE(profilerEnabled, std::string::npos);
    ASSERT_NE(execute, std::string::npos);
    ASSERT_NE(profilerSequence, std::string::npos);
    ASSERT_NE(deviceStatusCheck, std::string::npos);
    ASSERT_NE(signalSemaphoreFailure, std::string::npos);
    ASSERT_NE(signalFenceFailure, std::string::npos);
    ASSERT_NE(profilerSubmit, std::string::npos);
    EXPECT_LT(profilerEnabled, execute)
        << "The cheap profiler-enabled predicate should be captured before the queue transaction.";

    const auto queueLockBlockStart = submitBody.rfind('{', queueLock);
    ASSERT_NE(queueLockBlockStart, std::string::npos);
    const auto queueLockBlockEnd = FindMatchingBrace(submitBody, queueLockBlockStart);
    ASSERT_NE(queueLockBlockEnd, std::string::npos);

    EXPECT_LT(execute, profilerSequence)
        << "Profiler metadata order must be reserved immediately after native command-list execution.";
    EXPECT_LT(profilerSequence, deviceStatusCheck)
        << "Device-lost checks after ExecuteCommandLists must not return before profiler metadata can be published.";
    EXPECT_LT(profilerSequence, signalSemaphoreFailure)
        << "Semaphore signal failures after ExecuteCommandLists must not skip profiler metadata publication.";
    EXPECT_LT(profilerSequence, signalFenceFailure)
        << "Frame-fence signal failures after ExecuteCommandLists must not skip profiler metadata publication.";
    EXPECT_LT(profilerSequence, queueLockBlockEnd)
        << "The profiler sequence token must still reflect native queue execution order.";
    EXPECT_LT(queueLockBlockEnd, profilerSubmit)
        << "Profiler metadata publication must remain outside the shared native queue lock.";

    const auto afterExecuteLockBody = submitBody.substr(execute, queueLockBlockEnd - execute);
    EXPECT_EQ(afterExecuteLockBody.find("return "), std::string::npos)
        << "Once command lists have been queued, SubmitChecked must leave the queue lock and publish "
           "TimelineProfiler metadata before returning any failure result.";
#else
    GTEST_SKIP() << "DX12 queue synchronization is only built on Windows.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, GpuProfilerResolveSignalFailureQuarantinesUnfencedWork)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    const auto profilerHeader = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.h");
    const auto profilerSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp");

    EXPECT_NE(
        profilerHeader.find("bool   Resolve(uint32 frameIndex)"),
        std::string::npos)
        << "Resolve submission must return a status so Tick can stop advancing after a post-execute fence failure.";
    EXPECT_NE(
        profilerHeader.find("HasUnfencedSubmittedWork"),
        std::string::npos)
        << "The profiler must track accepted resolve work that has no retirement fence.";

    const auto tickFunction = profilerSource.find("void GPUProfiler::Tick()");
    ASSERT_NE(tickFunction, std::string::npos);
    const auto executeCommandListsFunction = profilerSource.find("void GPUProfiler::ExecuteCommandLists", tickFunction);
    ASSERT_NE(executeCommandListsFunction, std::string::npos);
    const auto tickBody = profilerSource.substr(tickFunction, executeCommandListsFunction - tickFunction);

    const auto resolveResultCheck = tickBody.find("!heap.Resolve(m_FrameIndex)");
    const auto disableHeap = tickBody.find("m_QueryHeapProfilingEnabled[heapIndex] = false", resolveResultCheck);
    const auto markGlobalQuarantine = tickBody.find("m_HasUnfencedSubmittedWork = true", resolveResultCheck);
    const auto tickReturn = tickBody.find("return;", markGlobalQuarantine);
    const auto frameAdvance = tickBody.find("m_FrameIndex = nextFrameIndex");
    ASSERT_NE(resolveResultCheck, std::string::npos);
    ASSERT_NE(disableHeap, std::string::npos);
    ASSERT_NE(markGlobalQuarantine, std::string::npos);
    ASSERT_NE(tickReturn, std::string::npos);
    ASSERT_NE(frameAdvance, std::string::npos);
    EXPECT_LT(resolveResultCheck, disableHeap);
    EXPECT_LT(disableHeap, markGlobalQuarantine);
    EXPECT_LT(markGlobalQuarantine, tickReturn);
    EXPECT_LT(tickReturn, frameAdvance)
        << "Tick must not advance/reset a GPU profiler frame after resolve work was queued without a fence.";

    const auto resolveFunction = profilerSource.find("bool GPUProfiler::QueryHeap::Resolve");
    ASSERT_NE(resolveFunction, std::string::npos);
    const auto markFrameFunction = profilerSource.find(
        "void GPUProfiler::QueryHeap::MarkFrameCompleteWithoutReadback",
        resolveFunction);
    ASSERT_NE(markFrameFunction, std::string::npos);
    const auto resolveBody = profilerSource.substr(resolveFunction, markFrameFunction - resolveFunction);

    const auto execute = resolveBody.find("m_pResolveQueue->ExecuteCommandLists");
    const auto signalFailure = resolveBody.find("FAILED(signalHr)", execute);
    const auto markHeapQuarantine = resolveBody.find("m_HasUnfencedSubmittedWork = true", signalFailure);
    const auto failedReturn = resolveBody.find("return false", markHeapQuarantine);
    ASSERT_NE(execute, std::string::npos);
    ASSERT_NE(signalFailure, std::string::npos);
    ASSERT_NE(markHeapQuarantine, std::string::npos);
    ASSERT_NE(failedReturn, std::string::npos);
    EXPECT_LT(execute, signalFailure);
    EXPECT_LT(signalFailure, markHeapQuarantine);
    EXPECT_LT(markHeapQuarantine, failedReturn);

    const auto shutdownFunction = profilerSource.find("bool GPUProfiler::ShutdownUnlocked()");
    ASSERT_NE(shutdownFunction, std::string::npos);
    const auto hasPendingFunction = profilerSource.find("bool GPUProfiler::HasPendingCommandListQueriesUnlocked", shutdownFunction);
    ASSERT_NE(hasPendingFunction, std::string::npos);
    const auto shutdownBody = profilerSource.substr(shutdownFunction, hasPendingFunction - shutdownFunction);
    const auto shutdownQuarantine = shutdownBody.find("HasUnfencedSubmittedWorkUnlocked()");
    const auto shutdownDrain = shutdownBody.find("DrainSubmittedFence");
    ASSERT_NE(shutdownQuarantine, std::string::npos);
    ASSERT_NE(shutdownDrain, std::string::npos);
    EXPECT_LT(shutdownQuarantine, shutdownDrain)
        << "Shutdown must retain unfenced resolve resources instead of waiting on a fence that was never queued.";

    const auto drainFunction = profilerSource.find("bool GPUProfiler::QueryHeap::DrainSubmittedFence()");
    ASSERT_NE(drainFunction, std::string::npos);
    const auto drainQueueFunction = profilerSource.find("bool GPUProfiler::QueryHeap::DrainQueue", drainFunction);
    ASSERT_NE(drainQueueFunction, std::string::npos);
    const auto drainBody = profilerSource.substr(drainFunction, drainQueueFunction - drainFunction);
    const auto drainQuarantine = drainBody.find("m_HasUnfencedSubmittedWork");
    const auto drainWait = drainBody.find("WaitForGpuProfilerFenceValue");
    ASSERT_NE(drainQuarantine, std::string::npos);
    ASSERT_NE(drainWait, std::string::npos);
    EXPECT_LT(drainQuarantine, drainWait)
        << "DrainSubmittedFence must not do a timed wait on an unfenced resolve submission.";

    const auto resetFunction = profilerSource.find("bool GPUProfiler::QueryHeap::ResetReusableFrameSlot");
    ASSERT_NE(resetFunction, std::string::npos);
    const auto resetEnd = profilerSource.find("bool GPUProfiler::QueryHeap::Reset(", resetFunction + 1u);
    ASSERT_NE(resetEnd, std::string::npos);
    const auto resetBody = profilerSource.substr(resetFunction, resetEnd - resetFunction);
    const auto resetQuarantine = resetBody.find("m_HasUnfencedSubmittedWork");
    const auto allocatorReset = resetBody.find("pAllocator->Reset()");
    ASSERT_NE(resetQuarantine, std::string::npos);
    ASSERT_NE(allocatorReset, std::string::npos);
    EXPECT_LT(resetQuarantine, allocatorReset)
        << "A query heap with unfenced submitted work must not reset its command allocator.";
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, GpuProfilerReadbackResourceStartsInCopyDestState)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    const auto profilerSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp");

    const auto initializeFunction = profilerSource.find("void GPUProfiler::QueryHeap::Initialize");
    ASSERT_NE(initializeFunction, std::string::npos);
    const auto drainSubmittedFenceFunction = profilerSource.find(
        "bool GPUProfiler::QueryHeap::DrainSubmittedFence",
        initializeFunction);
    ASSERT_NE(drainSubmittedFenceFunction, std::string::npos);
    const auto initializeBody = profilerSource.substr(
        initializeFunction,
        drainSubmittedFenceFunction - initializeFunction);

    const auto readbackHeap = initializeBody.find("D3D12_HEAP_TYPE_READBACK");
    const auto createReadback = initializeBody.find("CreateCommittedResource", readbackHeap);
    const auto copyDestState = initializeBody.find("D3D12_RESOURCE_STATE_COPY_DEST", createReadback);
    ASSERT_NE(readbackHeap, std::string::npos);
    ASSERT_NE(createReadback, std::string::npos);
    ASSERT_NE(copyDestState, std::string::npos)
        << "ResolveQueryData writes into the readback buffer, so the readback resource must start as COPY_DEST.";
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, UnsupportedGpuScopesAreFilteredBeforeCallbacks)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    const auto profilerSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp");

    const auto beginFunction = profilerSource.find("void GPUProfiler::BeginEvent(");
    ASSERT_NE(beginFunction, std::string::npos);
    const auto endFunction = profilerSource.find("void GPUProfiler::EndEvent(", beginFunction);
    ASSERT_NE(endFunction, std::string::npos);
    const auto executeFunction = profilerSource.find("void GPUProfiler::ExecuteCommandLists", endFunction);
    ASSERT_NE(executeFunction, std::string::npos);

    const auto beginBody = profilerSource.substr(beginFunction, endFunction - beginFunction);
    const auto endBody = profilerSource.substr(endFunction, executeFunction - endFunction);

    const auto beginQueueGate = beginBody.find("ShouldProfileGpuProfilerCommandListType");
    const auto beginCallback = beginBody.find("callbacks.OnEventBegin");
    const auto endQueueGate = endBody.find("ShouldProfileGpuProfilerCommandListType");
    const auto endCallback = endBody.find("callbacks.OnEventEnd");

    ASSERT_NE(beginQueueGate, std::string::npos);
    ASSERT_NE(beginCallback, std::string::npos);
    ASSERT_NE(endQueueGate, std::string::npos);
    ASSERT_NE(endCallback, std::string::npos);
    EXPECT_LT(beginQueueGate, beginCallback)
        << "Unsupported GPU command-list types should not trigger begin callbacks.";
    EXPECT_LT(endQueueGate, endCallback)
        << "Unsupported GPU command-list types should not trigger end callbacks.";
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, NullGpuCommandListsAreRejectedBeforeTypeQuery)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    const auto profilerSource = ReadSourceFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/UI/ImGuiExtensions/TimelineProfiler/Profiler.cpp");

    const auto beginFunction = profilerSource.find("void GPUProfiler::BeginEvent(");
    ASSERT_NE(beginFunction, std::string::npos);
    const auto endFunction = profilerSource.find("void GPUProfiler::EndEvent(", beginFunction);
    ASSERT_NE(endFunction, std::string::npos);
    const auto executeFunction = profilerSource.find("void GPUProfiler::ExecuteCommandLists", endFunction);
    ASSERT_NE(executeFunction, std::string::npos);

    const auto beginBody = profilerSource.substr(beginFunction, endFunction - beginFunction);
    const auto endBody = profilerSource.substr(endFunction, executeFunction - endFunction);

    const auto beginNullGuard = beginBody.find("pCmd == nullptr");
    const auto beginTypeQuery = beginBody.find("pCmd->GetType()");
    const auto endNullGuard = endBody.find("pCmd == nullptr");
    const auto endTypeQuery = endBody.find("pCmd->GetType()");

    ASSERT_NE(beginNullGuard, std::string::npos);
    ASSERT_NE(beginTypeQuery, std::string::npos);
    ASSERT_NE(endNullGuard, std::string::npos);
    ASSERT_NE(endTypeQuery, std::string::npos);
    EXPECT_LT(beginNullGuard, beginTypeQuery)
        << "BeginEvent must reject null command lists before querying the D3D12 type.";
    EXPECT_LT(endNullGuard, endTypeQuery)
        << "EndEvent must reject null command lists before querying the D3D12 type.";
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, GpuFrameRingSlotWaitsUntilPreviousReadbackIsDrained)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameSlotReusable(0u, 0u, 0u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameSlotReusable(0u, 2u, 0u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameSlotReusable(1u, 2u, 0u));
    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameSlotReusable(2u, 2u, 0u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameSlotReusable(2u, 2u, 1u));
    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameSlotReusable(3u, 2u, 1u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameSlotReusable(3u, 2u, 2u));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, TimelineSinkGpuProfilerOwnershipIsNonCopyableAndNonMovable)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    using NLS::Base::Profiling::TimelineProfilerSink;

    EXPECT_FALSE(std::is_copy_constructible_v<TimelineProfilerSink>);
    EXPECT_FALSE(std::is_copy_assignable_v<TimelineProfilerSink>);
    EXPECT_FALSE(std::is_move_constructible_v<TimelineProfilerSink>);
    EXPECT_FALSE(std::is_move_assignable_v<TimelineProfilerSink>);
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, UnsupportedComputeQueueDoesNotRecordGpuEvents)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0u;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> computeQueue;
    HRESULT hr = resources.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&computeQueue));
    ASSERT_TRUE(SUCCEEDED(hr)) << "Create compute queue failed: 0x" << std::hex << hr;

    NLS::Base::Profiling::TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);

    NLS::Base::Profiling::ProfilerGpuContextEvent gpuContext;
    gpuContext.nativeDevice = resources.device.Get();
    gpuContext.nativeCommandQueues.push_back(computeQueue.Get());
    gpuContext.frameLatency = 2u;
    timeline.InitializeGpuContext(gpuContext);
    const auto unsupportedState = timeline.GetState();
    EXPECT_EQ(
        unsupportedState.capabilities & NLS::Base::Profiling::ProfilerCapability_GPUScopes,
        NLS::Base::Profiling::ProfilerCapability_None)
        << "Unsupported/ambiguous GPU contexts should not advertise GPU scope routing.";
    EXPECT_NE(unsupportedState.lastError.find("GPU scopes are unavailable"), std::string::npos);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    hr = resources.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&allocator));
    ASSERT_TRUE(SUCCEEDED(hr)) << "Create compute allocator failed: 0x" << std::hex << hr;

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    hr = resources.device->CreateCommandList(
        0u,
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList));
    ASSERT_TRUE(SUCCEEDED(hr)) << "Create compute command list failed: 0x" << std::hex << hr;

    NLS::Base::Profiling::ProfilerGpuScopeEvent gpuScope;
    gpuScope.nativeCommandBuffer = commandList.Get();
    gpuScope.name = "UnsupportedComputeQueueScope";
    gpuScope.sourceFunction = __FUNCTION__;
    gpuScope.active = true;
    timeline.BeginGpuScope(gpuScope);
    timeline.EndGpuScope(gpuScope);

    EXPECT_EQ(timeline.GetPendingGpuProfilerEventCountForTesting(), 0u);
    EXPECT_EQ(timeline.GetPendingGpuProfilerCommandListQueryCountForTesting(), 0u);
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, EmptyGpuFramesDoNotSubmitReadbackAtRuntime)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    NLS::Base::Profiling::TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);

    NLS::Base::Profiling::ProfilerGpuContextEvent gpuContext;
    gpuContext.nativeDevice = resources.device.Get();
    gpuContext.nativeCommandQueues.push_back(resources.graphicsQueue.Get());
    gpuContext.frameLatency = 2u;
    timeline.InitializeGpuContext(gpuContext);
    ASSERT_NE(
        timeline.GetState().capabilities & NLS::Base::Profiling::ProfilerCapability_GPUScopes,
        NLS::Base::Profiling::ProfilerCapability_None);

    for (uint32_t frameIndex = 0u; frameIndex < 4u; ++frameIndex)
        timeline.TickFrame();

    EXPECT_EQ(timeline.GetSubmittedGpuProfilerReadbackCountForTesting(), 0u)
        << "Empty GPU profiler frames should not submit readback command lists or fences.";
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, MultipleDirectQueuesDoNotRecordAmbiguousSharedHeapEvents)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0u;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> secondGraphicsQueue;
    HRESULT hr = resources.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&secondGraphicsQueue));
    ASSERT_TRUE(SUCCEEDED(hr)) << "Create second graphics queue failed: 0x" << std::hex << hr;

    NLS::Base::Profiling::TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);

    NLS::Base::Profiling::ProfilerGpuContextEvent gpuContext;
    gpuContext.nativeDevice = resources.device.Get();
    gpuContext.nativeCommandQueues.push_back(resources.graphicsQueue.Get());
    gpuContext.nativeCommandQueues.push_back(secondGraphicsQueue.Get());
    gpuContext.frameLatency = 2u;
    timeline.InitializeGpuContext(gpuContext);
    const auto ambiguousState = timeline.GetState();
    EXPECT_EQ(
        ambiguousState.capabilities & NLS::Base::Profiling::ProfilerCapability_GPUScopes,
        NLS::Base::Profiling::ProfilerCapability_None)
        << "Ambiguous multi-queue contexts should not advertise GPU scope routing.";
    EXPECT_NE(ambiguousState.lastError.find("GPU scopes are unavailable"), std::string::npos);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    hr = resources.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ASSERT_TRUE(SUCCEEDED(hr)) << "Create direct allocator failed: 0x" << std::hex << hr;

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    hr = resources.device->CreateCommandList(
        0u,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList));
    ASSERT_TRUE(SUCCEEDED(hr)) << "Create direct command list failed: 0x" << std::hex << hr;

    NLS::Base::Profiling::ProfilerGpuScopeEvent gpuScope;
    gpuScope.nativeCommandBuffer = commandList.Get();
    gpuScope.name = "AmbiguousDirectQueueScope";
    gpuScope.sourceFunction = __FUNCTION__;
    gpuScope.active = true;
    timeline.BeginGpuScope(gpuScope);
    timeline.EndGpuScope(gpuScope);

    EXPECT_EQ(timeline.GetPendingGpuProfilerEventCountForTesting(), 0u);
    EXPECT_EQ(timeline.GetPendingGpuProfilerCommandListQueryCountForTesting(), 0u);
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, NonEmptyGpuFramePublishesResolvedEvent)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    NLS::Base::Profiling::TimelineProfilerSink timeline;
    timeline.SetRecordingEnabled(true);
    ASSERT_EQ(timeline.GetState().availability, NLS::Base::Profiling::ProfilerAvailability::Available);

    NLS::Base::Profiling::ProfilerGpuContextEvent gpuContext;
    gpuContext.nativeDevice = resources.device.Get();
    gpuContext.nativeCommandQueues.push_back(resources.graphicsQueue.Get());
    gpuContext.frameLatency = 2u;
    timeline.InitializeGpuContext(gpuContext);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT hr = resources.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator));
    ASSERT_TRUE(SUCCEEDED(hr)) << "CreateCommandAllocator failed: 0x" << std::hex << hr;

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    hr = resources.device->CreateCommandList(
        0u,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(),
        nullptr,
        IID_PPV_ARGS(&commandList));
    ASSERT_TRUE(SUCCEEDED(hr)) << "CreateCommandList failed: 0x" << std::hex << hr;

    constexpr const char* kEventName = "TimelineGpuPublishTest";
    NLS::Base::Profiling::ProfilerGpuScopeEvent gpuScope;
    gpuScope.nativeCommandBuffer = commandList.Get();
    gpuScope.name = kEventName;
    gpuScope.sourceFunction = __FUNCTION__;
    gpuScope.active = true;
    timeline.BeginGpuScope(gpuScope);

    constexpr UINT64 kBufferSize = 16u * 1024u * 1024u;
    constexpr uint32_t kCopyCount = 16u;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Alignment = 0u;
    bufferDesc.Width = kBufferSize;
    bufferDesc.Height = 1u;
    bufferDesc.DepthOrArraySize = 1u;
    bufferDesc.MipLevels = 1u;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1u;
    bufferDesc.SampleDesc.Quality = 0u;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    uploadHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    uploadHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    uploadHeap.CreationNodeMask = 0u;
    uploadHeap.VisibleNodeMask = 0u;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    defaultHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    defaultHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    defaultHeap.CreationNodeMask = 0u;
    defaultHeap.VisibleNodeMask = 0u;

    Microsoft::WRL::ComPtr<ID3D12Resource> sourceBuffer;
    hr = resources.device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&sourceBuffer));
    ASSERT_TRUE(SUCCEEDED(hr)) << "Create upload buffer failed: 0x" << std::hex << hr;

    Microsoft::WRL::ComPtr<ID3D12Resource> destinationBuffer;
    hr = resources.device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&destinationBuffer));
    ASSERT_TRUE(SUCCEEDED(hr)) << "Create destination buffer failed: 0x" << std::hex << hr;

    for (uint32_t copyIndex = 0u; copyIndex < kCopyCount; ++copyIndex)
        commandList->CopyBufferRegion(destinationBuffer.Get(), 0u, sourceBuffer.Get(), 0u, kBufferSize);
    timeline.EndGpuScope(gpuScope);
    EXPECT_EQ(timeline.GetPendingGpuProfilerEventCountForTesting(), 1u);
    EXPECT_EQ(timeline.GetPendingGpuProfilerCommandListQueryCountForTesting(), 2u);
    hr = commandList->Close();
    ASSERT_TRUE(SUCCEEDED(hr)) << "CommandList Close failed: 0x" << std::hex << hr;

    ID3D12CommandList* submittedLists[] = { static_cast<ID3D12CommandList*>(commandList.Get()) };
    resources.graphicsQueue->ExecuteCommandLists(1u, submittedLists);

    NLS::Base::Profiling::ProfilerGpuCommandListSubmitEvent submitEvent;
    submitEvent.nativeCommandQueue = resources.graphicsQueue.Get();
    submitEvent.nativeCommandLists.push_back(submittedLists[0]);
    timeline.SubmitGpuCommandLists(submitEvent);
    EXPECT_EQ(timeline.GetPendingGpuProfilerCommandListQueryCountForTesting(), 0u);

    timeline.TickFrame();
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        timeline.TickFrame();
        if (timeline.HasRecordedEventForTesting(kEventName))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_GT(timeline.CountRecordedEventsForTesting(kEventName, true), 0u)
        << "invalid-or-unresolved count="
        << timeline.CountRecordedEventsForTesting(kEventName, false);
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, ResolveFenceValuesTreatInitialFenceAsIncomplete)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    EXPECT_EQ(TimelineProfilerDetail::GetGpuProfilerResolveFenceValue(0u), 1u);
    EXPECT_EQ(TimelineProfilerDetail::GetGpuProfilerResolveFenceValue(4u), 5u);

    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameFenceComplete(0u, 0u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameFenceComplete(1u, 0u));
    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameFenceComplete(4u, 4u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameFenceComplete(5u, 4u));

    EXPECT_FALSE(TimelineProfilerDetail::ShouldWaitForGpuProfilerResolveFence(0u, 0u));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldWaitForGpuProfilerResolveFence(0u, 1u));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldWaitForGpuProfilerResolveFence(1u, 1u));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldReleaseGpuProfilerResolveResourcesAfterFenceWait(false, false));
    EXPECT_FALSE(TimelineProfilerDetail::ShouldReleaseGpuProfilerResolveResourcesAfterFenceWait(true, false));
    EXPECT_TRUE(TimelineProfilerDetail::ShouldReleaseGpuProfilerResolveResourcesAfterFenceWait(true, true));
    EXPECT_GT(TimelineProfilerDetail::GetGpuProfilerResolveFenceWaitTimeoutMilliseconds(), 0u);
    EXPECT_FALSE(TimelineProfilerDetail::CanReleaseGpuProfilerWithPendingCommandListQueries(true));
    EXPECT_TRUE(TimelineProfilerDetail::CanReleaseGpuProfilerWithPendingCommandListQueries(false));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, UnsubmittedGpuReadbackFramesAreCompleteWithoutFenceProgress)
{
#if NLS_ENABLE_TIMELINE_PROFILER
    const uint64_t unsubmittedFrame = TimelineProfilerDetail::GetGpuProfilerUnsubmittedReadbackSentinel();

    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerReadbackSubmitted(unsubmittedFrame));
    EXPECT_FALSE(TimelineProfilerDetail::DoesGpuProfilerReusableSlotDependOnFrame(unsubmittedFrame, 0u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameComplete(
        unsubmittedFrame,
        0u,
        0u));

    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerReadbackSubmitted(3u));
    EXPECT_TRUE(TimelineProfilerDetail::DoesGpuProfilerReusableSlotDependOnFrame(3u, 3u));
    EXPECT_FALSE(TimelineProfilerDetail::DoesGpuProfilerReusableSlotDependOnFrame(4u, 3u));
    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameComplete(
        3u,
        0u,
        3u));
    EXPECT_FALSE(TimelineProfilerDetail::IsGpuProfilerFrameComplete(
        4u,
        0u,
        3u));
    EXPECT_TRUE(TimelineProfilerDetail::IsGpuProfilerFrameComplete(
        3u,
        TimelineProfilerDetail::GetGpuProfilerResolveFenceValue(3u),
        3u));
#else
    GTEST_SKIP() << "TimelineProfiler is not enabled in this build.";
#endif
}
