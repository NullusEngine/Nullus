#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <filesystem>
#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/RHI/Backends/DX12/DX12QueueSynchronization.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "TaskGraph/TaskGraph.h"

#include <chrono>
#include <d3d12.h>
#include <limits>
#include <wrl/client.h>
#endif

#include "Rendering/RHI/Backends/DX12/DX12PresentPolicy.h"
#include "UI/ImGuiExtensions/TimelineProfiler/Profiler.h"
#include "UI/Profiling/TimelineProfilerLimits.h"
#include "UI/Profiling/TimelineProfilerSink.h"

namespace
{
#if defined(_WIN32)
class ScopedProfilerSubmitLane final
{
public:
    explicit ScopedProfilerSubmitLane(
        const NLS::Render::Context::RhiThreadMode mode = NLS::Render::Context::RhiThreadMode::None)
    {
        auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
        executor.ResetForTesting();
        EXPECT_TRUE(m_graph.Start());
        executor.Initialize(mode, {}, &m_graph);
        if (mode == NLS::Render::Context::RhiThreadMode::DedicatedThread)
        {
            m_rhiThread = std::thread([this, &executor]
            {
                m_graph.RegisterCurrentThread(NLS::Base::TaskGraph::NamedThread::Rhi);
                while (!m_stop.load(std::memory_order_acquire))
                {
                    m_graph.Pump(NLS::Base::TaskGraph::NamedThread::Rhi);
                    executor.Pump();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                m_graph.UnregisterCurrentThread(NLS::Base::TaskGraph::NamedThread::Rhi);
            });
        }
    }
    ~ScopedProfilerSubmitLane()
    {
        auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
        executor.Shutdown();
        m_stop.store(true, std::memory_order_release);
        if (m_rhiThread.joinable())
            m_rhiThread.join();
        m_graph.Shutdown();
        executor.ResetForTesting();
    }
private:
    NLS::Base::TaskGraph::TaskGraph m_graph{{1u, false, true}};
    std::atomic_bool m_stop{false};
    std::thread m_rhiThread;
};
#endif

std::string ReadSourceFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
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

TEST(DX12PresentPolicyTests, TearingRequiresVsyncOffAndPlatformSupport)
{
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12PresentFlags(true, true), 0u);
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12PresentFlags(false, false), 0u);
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12PresentFlags(false, true), 0x00000200u);
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12SwapchainFlags(false), 0u);
    EXPECT_EQ(NLS::Render::Backend::ResolveDX12SwapchainFlags(true), 0x00000800u);
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

TEST(TimelineProfilerGpuLifecycleTests, ClearingGpuContextReleasesBeforeSubmitLaneShutdown)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    for (const auto mode : {NLS::Render::Context::RhiThreadMode::None,
        NLS::Render::Context::RhiThreadMode::DedicatedThread, NLS::Render::Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ScopedProfilerSubmitLane submitLane(mode);
        const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
        if (!resources.IsValid())
            GTEST_SKIP() << "DX12 device unavailable on this test machine";
        NLS::Base::Profiling::TimelineProfilerSink timeline;
        timeline.SetRecordingEnabled(true);
        NLS::Base::Profiling::ProfilerGpuContextEvent context;
        context.nativeDevice = resources.device.Get();
        context.nativeCommandQueues = {resources.graphicsQueue.Get()};
        context.frameLatency = 2u;
        timeline.InitializeGpuContext(context);
        ASSERT_NE(timeline.GetState().capabilities &
            NLS::Base::Profiling::ProfilerCapability_GPUScopes, 0u);

        int unrelatedDevice = 0;
        timeline.ClearGpuContext(&unrelatedDevice);
        EXPECT_NE(timeline.GetState().capabilities &
            NLS::Base::Profiling::ProfilerCapability_GPUScopes, 0u);
        timeline.ClearGpuContext(resources.device.Get());
        EXPECT_EQ(timeline.GetState().capabilities &
            NLS::Base::Profiling::ProfilerCapability_GPUScopes, 0u);

        timeline.InitializeGpuContext(context);
        EXPECT_NE(timeline.GetState().capabilities &
            NLS::Base::Profiling::ProfilerCapability_GPUScopes, 0u);
        timeline.ClearGpuContext(nullptr);
        EXPECT_EQ(timeline.GetState().capabilities &
            NLS::Base::Profiling::ProfilerCapability_GPUScopes, 0u);
        NLS::Render::Context::RhiCommandListExecutor::Get().Shutdown();
    }
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, UnsupportedComputeQueueDoesNotRecordGpuEvents)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    ScopedProfilerSubmitLane submitLane;
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
    ScopedProfilerSubmitLane submitLane;
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
    ScopedProfilerSubmitLane submitLane;
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

#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
namespace
{
bool WaitForTestGpuFence(ID3D12Fence* fence, const UINT64 value)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;)
    {
        const auto completed = fence->GetCompletedValue();
        if (completed == (std::numeric_limits<UINT64>::max)())
            return false;
        if (completed >= value)
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ValidateTimelineGpuPublication(const NLS::Render::Context::RhiThreadMode mode)
{
    ScopedProfilerSubmitLane submitLane(mode);
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
    const auto previousEventCount = timeline.CountRecordedEventsForTesting(kEventName, true);
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
        D3D12_RESOURCE_STATE_COMMON,
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
    Microsoft::WRL::ComPtr<ID3D12Fence> submitFence;
    ASSERT_TRUE(SUCCEEDED(resources.device->CreateFence(
        0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submitFence))));
    const auto submission = NLS::Render::RHI::DX12::SubmitDX12CommandLists(
        resources.graphicsQueue.Get(), submittedLists, submitFence.Get(), 1u,
        kEventName);
    const bool gpuComplete = submission.operation.frameFenceSignalQueued &&
        WaitForTestGpuFence(submitFence.Get(), 1u);
    if (submission.operation.mayHaveQueuedGpuWork && !gpuComplete)
    {
        // A failed test must also preserve objects whose GPU retirement is unknown.
        (void)sourceBuffer.Detach();
        (void)destinationBuffer.Detach();
        (void)commandList.Detach();
        (void)allocator.Detach();
        (void)submitFence.Detach();
        FAIL() << "GPU work did not retire: " << submission.operation.message;
    }
    ASSERT_TRUE(submission.Succeeded()) << submission.operation.message;
    ASSERT_TRUE(gpuComplete);

    NLS::Base::Profiling::ProfilerGpuCommandListSubmitEvent submitEvent;
    submitEvent.nativeCommandQueue = resources.graphicsQueue.Get();
    submitEvent.nativeCommandLists.push_back(submittedLists[0]);
    timeline.SubmitGpuCommandLists(submitEvent);
    EXPECT_EQ(timeline.GetPendingGpuProfilerCommandListQueryCountForTesting(), 0u);

    timeline.TickFrame();
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        timeline.TickFrame();
        if (timeline.CountRecordedEventsForTesting(kEventName, true) > previousEventCount)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EXPECT_GT(timeline.CountRecordedEventsForTesting(kEventName, true), previousEventCount)
        << "invalid-or-unresolved count="
        << timeline.CountRecordedEventsForTesting(kEventName, false);
}
}
#endif

TEST(TimelineProfilerGpuLifecycleTests, NonEmptyGpuFramePublishesResolvedEvent)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    for (const auto mode : {NLS::Render::Context::RhiThreadMode::None,
        NLS::Render::Context::RhiThreadMode::DedicatedThread, NLS::Render::Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ValidateTimelineGpuPublication(mode);
    }
#else
    GTEST_SKIP() << "TimelineProfiler or DX12 is not enabled in this build.";
#endif
}

TEST(TimelineProfilerGpuLifecycleTests, TickAllowsEarlierSubmitToFinishItsGpuScopes)
{
#if NLS_ENABLE_TIMELINE_PROFILER && defined(_WIN32)
    for (const auto mode : {NLS::Render::Context::RhiThreadMode::DedicatedThread,
        NLS::Render::Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ScopedProfilerSubmitLane submitLane(mode);
        const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
        if (!resources.IsValid())
            GTEST_SKIP() << "DX12 device unavailable on this test machine";
        NLS::Base::Profiling::TimelineProfilerSink timeline;
        timeline.SetRecordingEnabled(true);
        NLS::Base::Profiling::ProfilerGpuContextEvent context;
        context.nativeDevice = resources.device.Get();
        context.nativeCommandQueues = {resources.graphicsQueue.Get()};
        context.frameLatency = 2u;
        timeline.InitializeGpuContext(context);
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        ASSERT_TRUE(SUCCEEDED(resources.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
        ASSERT_TRUE(SUCCEEDED(resources.device->CreateCommandList(0u,
            D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList))));
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        ASSERT_TRUE(SUCCEEDED(resources.device->CreateFence(0u, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
        std::atomic_bool releaseEarlierSubmit = false;
        auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
        const auto earlier = executor.EnqueueOnSubmitLane("EarlierTimelineScopes", [&]
        {
            while (!releaseEarlierSubmit.load(std::memory_order_acquire))
                std::this_thread::yield();
            NLS::Base::Profiling::ProfilerGpuScopeEvent scope;
            scope.nativeCommandBuffer = commandList.Get();
            scope.name = "EarlierTimelineScopes";
            timeline.BeginGpuScope(scope);
            timeline.EndGpuScope(scope);
            EXPECT_TRUE(SUCCEEDED(commandList->Close()));
            NLS::Base::Profiling::ProfilerGpuCommandListSubmitEvent submitted;
            submitted.nativeCommandQueue = resources.graphicsQueue.Get();
            submitted.nativeCommandLists = {commandList.Get()};
            timeline.SubmitGpuCommandLists(submitted);
            ID3D12CommandList* lists[] = {commandList.Get()};
            resources.graphicsQueue->ExecuteCommandLists(1u, lists);
            EXPECT_TRUE(SUCCEEDED(resources.graphicsQueue->Signal(fence.Get(), 1u)));
            return NLS::Render::RHI::RHIQueueOperationResult{};
        });
        const auto sequence = executor.GetDispatchSequence();
        std::thread ticking([&] { timeline.TickFrame(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (executor.GetDispatchSequence() == sequence && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const bool tickQueued = executor.GetDispatchSequence() > sequence;
        releaseEarlierSubmit.store(true, std::memory_order_release);
        ticking.join();
        EXPECT_TRUE(tickQueued);
        EXPECT_TRUE(earlier.completion.Succeeded());
        EXPECT_FALSE(executor.HasFailed()) << executor.GetFailureDiagnostic();
        EXPECT_TRUE(executor.Flush(NLS::Render::Context::RhiFlushFlags::Submit));
        const auto gpuDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (fence->GetCompletedValue() < 1u && std::chrono::steady_clock::now() < gpuDeadline)
            std::this_thread::yield();
        EXPECT_GE(fence->GetCompletedValue(), 1u);
    }
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
