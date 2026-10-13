#include <gtest/gtest.h>

#if defined(_WIN32)
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/Context/RhiCommandListRecorder.h"
#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Settings/DriverSettings.h"
#include "TaskGraph/TaskGraph.h"
#include <d3d12sdklayers.h>

#undef CreateSemaphore

namespace
{
    using namespace NLS::Render;
    using namespace std::chrono_literals;

    class ScopedDx12SubmitLane final
    {
    public:
        explicit ScopedDx12SubmitLane(const Context::RhiThreadMode mode)
            : graph({1u, false, true})
        {
            auto& executor = Context::RhiCommandListExecutor::Get();
            executor.ResetForTesting();
            ready = graph.Start();
            if (!ready)
                return;
            executor.Initialize(mode, {}, &graph);
            Context::RenderingThreadInitParams params;
            params.rhiThreadMode = mode;
            params.taskGraph = &graph;
            ready = threads.InitRenderingThreads(params);
        }

        ~ScopedDx12SubmitLane()
        {
            auto& executor = Context::RhiCommandListExecutor::Get();
            executor.Shutdown();
            threads.ShutdownRenderingThreads();
            graph.Shutdown();
            executor.ResetForTesting();
        }

        NLS::Base::TaskGraph::TaskGraph graph;
        Context::RenderingThreadSystem threads;
        bool ready = false;
    };

    auto CreateExplicitDevice(const Backend::DX12DeviceResources& resources)
    {
        return Backend::CreateNativeDX12ExplicitDevice(
            resources.device.Get(), resources.graphicsQueue.Get(), resources.computeQueue.Get(),
            resources.factory.Get(), resources.adapter.Get(), resources.capabilities,
            resources.vendor, resources.hardware);
    }
}

TEST(DX12SubmitLaneTests, RejectedDescriptorInitializationDoesNotQuarantineUnsubmittedGpuWork)
{
    Settings::DriverSettings settings;
    settings.graphicsBackend = Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    Context::Driver driver(settings);
    Context::DriverTestAccess::PauseThreadedRenderingWorkers(driver);
    struct ScopedDriverRegistration final
    {
        explicit ScopedDriverRegistration(Context::Driver& driver)
        {
            NLS::Core::ServiceLocator::Provide(driver);
        }
        ~ScopedDriverRegistration()
        {
            NLS::Core::ServiceLocator::Remove<Context::Driver>();
        }
    } registration(driver);
    auto& executor = Context::RhiCommandListExecutor::Get();
    executor.Shutdown(Context::RhiFlushFlags::Dispatch | Context::RhiFlushFlags::Translate |
        Context::RhiFlushFlags::Submit, &driver);
    const auto sequence = executor.GetDispatchedSequence();
    const auto resources = Backend::CreateDX12DeviceResources(true);
    ASSERT_TRUE(resources.IsValid());
    auto device = CreateExplicitDevice(resources);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(executor.GetDispatchedSequence(), sequence);
    const auto* impl = Context::DriverTestAccess::GetImplForTesting(driver);
    EXPECT_FALSE(impl->unsafeGpuWorkQuarantined.load());
    EXPECT_FALSE(impl->deviceLostDetected.load());
    EXPECT_EQ(impl->queueOperationFailureCount, 0u);
    EXPECT_TRUE(SUCCEEDED(resources.device->GetDeviceRemovedReason()));
}

TEST(DX12SubmitLaneTests, DirectAndNestedQueueSubmitsShareTheConfiguredFifo)
{
    for (const auto mode : {Context::RhiThreadMode::None, Context::RhiThreadMode::DedicatedThread,
        Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ScopedDx12SubmitLane lane(mode);
        ASSERT_TRUE(lane.ready);
        const auto resources = Backend::CreateDX12DeviceResources(true);
        if (!resources.IsValid())
            GTEST_SKIP() << "DX12 device unavailable on this test machine";
        auto device = CreateExplicitDevice(resources);
        ASSERT_NE(device, nullptr);
        auto& executor = Context::RhiCommandListExecutor::Get();

        for (const auto type : {RHI::QueueType::Graphics, RHI::QueueType::Compute, RHI::QueueType::Copy})
        {
            SCOPED_TRACE(static_cast<int>(type));
            auto queue = device->GetQueue(type);
            auto fence = device->CreateFence("DX12DirectSubmitFence");
            ASSERT_NE(queue, nullptr);
            ASSERT_NE(fence, nullptr);
            RHI::RHISubmitDesc submit;
            submit.signalFence = fence;
            const auto sequence = executor.GetDispatchedSequence();
            const auto direct = queue->SubmitChecked(submit);
            ASSERT_TRUE(direct.Succeeded()) << direct.message;
            EXPECT_TRUE(direct.frameFenceSignalQueued);
            EXPECT_EQ(executor.GetDispatchedSequence(), sequence + 1u);
            EXPECT_EQ(executor.GetSubmitSequence(), sequence + 1u);
            ASSERT_TRUE(fence->Wait(5'000'000'000ull));

            const auto nested = executor.RunOnSubmitLane("DX12 nested native submit", [&]
            {
                EXPECT_TRUE(Context::RenderingThreadSystem::IsInRhiThread());
                return executor.SubmitNative(queue, submit);
            });
            ASSERT_TRUE(nested.Succeeded()) << nested.message;
            EXPECT_EQ(executor.GetDispatchedSequence(), sequence + 2u);
            EXPECT_EQ(executor.GetSubmitSequence(), sequence + 2u);
            ASSERT_TRUE(fence->Wait(5'000'000'000ull));

            // The only worker must assist its own queued submit in Tasks mode.
            const auto worker = lane.graph.Submit([&]
            {
                const auto result = queue->SubmitChecked(submit);
                EXPECT_TRUE(result.Succeeded()) << result.message;
            });
            ASSERT_TRUE(lane.graph.WaitFor(worker, 5s));
            EXPECT_EQ(lane.graph.GetCompletion(worker).GetStatus(), NLS::Base::TaskGraph::TaskStatus::Succeeded);
            EXPECT_EQ(executor.GetDispatchedSequence(), sequence + 3u);
            EXPECT_EQ(executor.GetSubmitSequence(), sequence + 3u);
            ASSERT_TRUE(fence->Wait(5'000'000'000ull));
        }
        EXPECT_FALSE(executor.HasFailed()) << executor.GetFailureDiagnostic();
    }
}

TEST(DX12SubmitLaneTests, GraphicsComputeGraphicsBarriersPreserveStatesAndPixels)
{
    for (const auto mode : {Context::RhiThreadMode::None, Context::RhiThreadMode::DedicatedThread,
        Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ScopedDx12SubmitLane lane(mode);
        ASSERT_TRUE(lane.ready);
        const auto resources = Backend::CreateDX12DeviceResources(true);
        ASSERT_TRUE(resources.IsValid());
        auto device = CreateExplicitDevice(resources);
        ASSERT_NE(device, nullptr);
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
        ASSERT_TRUE(SUCCEEDED(resources.device.As(&messages)));
        auto& executor = Context::RhiCommandListExecutor::Get();
        executor.SetTargets(Context::RhiCommandListExecutor::CreateDeviceTargets(device));

        const std::array<uint8_t, 4> source{23u, 97u, 181u, 255u};
        RHI::RHITextureDesc textureDesc;
        textureDesc.extent = {1u, 1u, 1u};
        textureDesc.format = RHI::TextureFormat::RGBA8;
        textureDesc.usage = RHI::TextureUsageFlags::Sampled | RHI::TextureUsageFlags::Storage |
            RHI::TextureUsageFlags::CopySrc | RHI::TextureUsageFlags::CopyDst;
        textureDesc.debugName = "DX12CrossQueueTexture";
        RHI::RHITextureUploadDesc upload;
        upload.data = source.data();
        upload.dataSize = source.size();
        upload.rowPitch = upload.slicePitch = 4u;
        auto texture = device->CreateTexture(textureDesc, upload);
        ASSERT_NE(texture, nullptr);

        for (uint32_t iteration = 0u; iteration < 2u; ++iteration)
        {
            SCOPED_TRACE(iteration);
            auto tracker = RHI::CreateDefaultResourceStateTracker();
            const std::array<RHI::QueueType, 3> queues{
                RHI::QueueType::Graphics, RHI::QueueType::Compute, RHI::QueueType::Graphics};
            std::array<std::shared_ptr<RHI::RHICommandPool>, 3> pools;
            std::array<std::shared_ptr<RHI::RHICommandBuffer>, 3> native;
            std::array<std::shared_ptr<Context::RhiCommandList>, 3> lists;
            auto fence = device->CreateFence("DX12CrossQueueFence");
            auto released = device->CreateSemaphore("DX12GraphicsReleased");
            auto computed = device->CreateSemaphore("DX12ComputeCompleted");
            ASSERT_NE(fence, nullptr);
            ASSERT_NE(released, nullptr);
            ASSERT_NE(computed, nullptr);
            for (size_t index = 0u; index < lists.size(); ++index)
            {
                pools[index] = device->CreateCommandPool(queues[index], "DX12CrossQueuePool");
                ASSERT_NE(pools[index], nullptr);
                native[index] = pools[index]->CreateCommandBuffer("DX12CrossQueueList");
                ASSERT_NE(native[index], nullptr);
                lists[index] = std::make_shared<Context::RhiCommandList>();
                lists[index]->SetQueueType(queues[index]);
                lists[index]->SetSignalFence(fence);
            }
            Context::RhiCommandListRecorder release(*lists[0], *native[0], tracker);
            release.Begin();
            Context::RhiCommandListRecorder compute(*lists[1], *native[1], tracker,
                [&](const auto& barrier) { return release.BarrierChecked(barrier); });
            compute.Begin();
            RHI::RHIBarrierDesc barrier;
            barrier.textureBarriers.push_back({texture, RHI::ResourceState::Unknown,
                RHI::ResourceState::ShaderWrite});
            ASSERT_TRUE(compute.BarrierChecked(barrier).Succeeded());
            barrier.textureBarriers.front().after = RHI::ResourceState::ShaderRead;
            ASSERT_TRUE(compute.BarrierChecked(barrier).Succeeded());
            ASSERT_EQ(tracker->GetTextureState(texture, {}).value().state, RHI::ResourceState::ShaderReadNonPixel);
            compute.End();
            release.End();
            ASSERT_GT(lists[0]->GetCommandCount(), 0u);
            Context::RhiCommandListRecorder graphics(*lists[2], *native[2], tracker);
            graphics.Begin();
            ASSERT_TRUE(graphics.BarrierChecked(barrier).Succeeded());
            graphics.End();

            messages->ClearStoredMessages();
            Context::RhiCommandListBatch batch;
            batch.commandLists.assign(lists.begin(), lists.end());
            batch.acquireCommandBuffer = [native](const size_t index, auto, auto) { return native[index]; };
            batch.retirementFence = fence;
            batch.submitSignalsFences = false;
            batch.submit = [&](const auto& translated)
            {
                for (size_t index = 0u; index < translated.size(); ++index)
                {
                    RHI::RHISubmitDesc submit;
                    submit.commandBuffers.push_back(translated[index].commandBuffer);
                    if (index == 0u)
                        submit.signalSemaphores.push_back(released);
                    else if (index == 1u)
                    {
                        submit.waitSemaphores.push_back(released);
                        submit.signalSemaphores.push_back(computed);
                    }
                    else
                    {
                        submit.waitSemaphores.push_back(computed);
                        submit.signalFence = fence;
                    }
                    const auto result = executor.SubmitNative(device->GetQueue(queues[index]), submit);
                    if (!result.Succeeded())
                        return result;
                }
                return RHI::RHIQueueOperationResult{RHI::RHIQueueOperationStatusCode::Success, {}, true, true};
            };
            Context::RhiSubmitEvent completed;
            ASSERT_TRUE(executor.DispatchBatch(std::move(batch), &completed));
            ASSERT_TRUE(executor.Flush(Context::RhiFlushFlags::Gpu)) << executor.GetFailureDiagnostic();
            ASSERT_TRUE(completed.Succeeded()) << completed.GetDiagnostic();
            std::array<uint8_t, 4> pixels{};
            const auto readback = device->ReadPixelsChecked(texture, 0u, 0u, 1u, 1u,
                Settings::EPixelDataFormat::RGBA, Settings::EPixelDataType::UNSIGNED_BYTE, pixels.data());
            ASSERT_TRUE(readback.Succeeded()) << readback.message;
            EXPECT_EQ(pixels, source);
            for (UINT64 index = 0u; index < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
            {
                SIZE_T size = 0u;
                ASSERT_TRUE(SUCCEEDED(messages->GetMessage(index, nullptr, &size)));
                std::vector<std::byte> bytes(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                ASSERT_TRUE(SUCCEEDED(messages->GetMessage(index, message, &size)));
                EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_ERROR) << message->pDescription;
                EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
            }
        }
    }
}

TEST(DX12SubmitLaneTests, FullTextureUpdatesPreserveReadableLayoutAndPixels)
{
    for (const auto mode : {Context::RhiThreadMode::None, Context::RhiThreadMode::DedicatedThread,
        Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ScopedDx12SubmitLane lane(mode);
        ASSERT_TRUE(lane.ready);
        const auto resources = Backend::CreateDX12DeviceResources(true);
        ASSERT_TRUE(resources.IsValid());
        auto device = CreateExplicitDevice(resources);
        ASSERT_NE(device, nullptr);
        Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
        ASSERT_TRUE(SUCCEEDED(resources.device.As(&messages)));
        auto& executor = Context::RhiCommandListExecutor::Get();

        std::array<uint8_t, 64u * 64u * 4u> source{};
        source.fill(37u);
        RHI::RHITextureDesc textureDesc;
        textureDesc.extent = {64u, 64u, 1u};
        textureDesc.format = RHI::TextureFormat::RGBA8;
        textureDesc.usage = RHI::TextureUsageFlags::Sampled | RHI::TextureUsageFlags::CopySrc |
            RHI::TextureUsageFlags::CopyDst;
        textureDesc.debugName = "DX12FullUpdateTexture";
        RHI::RHITextureUploadDesc upload;
        upload.data = source.data();
        upload.dataSize = source.size();
        upload.extent = textureDesc.extent;
        upload.rowPitch = 64u * 4u;
        upload.slicePitch = source.size();
        auto texture = device->CreateTexture(textureDesc, upload);
        ASSERT_NE(texture, nullptr);
        ASSERT_EQ(texture->GetState(), RHI::ResourceState::ShaderRead);
        messages->ClearStoredMessages();

        for (uint8_t value : {71u, 193u})
        {
            SCOPED_TRACE(value);
            source.fill(value);
            RHI::RHITextureUpdateDesc update;
            update.texture = texture;
            update.data = source.data();
            update.dataSize = source.size();
            update.extent = textureDesc.extent;
            update.rowPitch = upload.rowPitch;
            update.slicePitch = upload.slicePitch;
            update.debugName = "DX12FullUpdate";
            const auto sequence = executor.GetDispatchedSequence();
            const bool batched = value == 193u;
            if (batched)
                ASSERT_TRUE(device->BeginInitialTextureUploadBatch());
            const auto updated = device->UpdateTexture(update);
            if (batched)
            {
                const auto recordedSequence = executor.GetDispatchedSequence();
                const bool ended = device->EndInitialTextureUploadBatch();
                EXPECT_EQ(recordedSequence, sequence);
                ASSERT_TRUE(ended);
                ASSERT_EQ(device->WaitForInitialUploads(), 0u);
            }
            ASSERT_TRUE(updated.Succeeded()) << updated.message;
            EXPECT_EQ(executor.GetDispatchedSequence(), sequence + 1u);
            EXPECT_EQ(executor.GetSubmitSequence(), sequence + 1u);
            EXPECT_EQ(texture->GetState(), RHI::ResourceState::ShaderRead);

            std::array<uint8_t, 64u * 64u * 4u> pixels{};
            const auto readback = device->ReadPixelsChecked(texture, 0u, 0u, 64u, 64u,
                Settings::EPixelDataFormat::RGBA, Settings::EPixelDataType::UNSIGNED_BYTE, pixels.data());
            ASSERT_TRUE(readback.Succeeded()) << readback.message;
            EXPECT_EQ(pixels, source);
        }
        for (UINT64 index = 0u; index < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
        {
            SIZE_T size = 0u;
            ASSERT_TRUE(SUCCEEDED(messages->GetMessage(index, nullptr, &size)));
            std::vector<std::byte> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            ASSERT_TRUE(SUCCEEDED(messages->GetMessage(index, message, &size)));
            EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_ERROR) << message->pDescription;
            EXPECT_NE(message->Severity, D3D12_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
        }
        EXPECT_FALSE(executor.HasFailed()) << executor.GetFailureDiagnostic();
    }
}

// GPU-based validation judges draws that do not repeat a texture transition against
// the resource's creation layout. Creating native textures in COPY_DEST made UI and
// material draws report an incompatible layout even though the upload had already
// transitioned them, so texture creation has to stay in the promotable COMMON state.
TEST(DX12SubmitLaneTests, NativeTextureCreationUsesPromotableCommonState)
{
    std::ifstream input(
        std::filesystem::path(NLS_ROOT_DIR) /
            "Runtime/Rendering/RHI/Backends/DX12/DX12Resource.cpp",
        std::ios::binary);
    ASSERT_TRUE(input.is_open()) << "DX12Resource.cpp is unavailable to the source guard";
    const std::string source(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    ASSERT_FALSE(source.empty());

    const auto constructorBegin = source.find("NativeDX12Texture::NativeDX12Texture(");
    ASSERT_NE(constructorBegin, std::string::npos);
    const auto constructorEnd =
        source.find("NativeDX12Texture::~NativeDX12Texture", constructorBegin);
    ASSERT_NE(constructorEnd, std::string::npos);
    const std::string constructor =
        source.substr(constructorBegin, constructorEnd - constructorBegin);

    EXPECT_NE(constructor.find("D3D12_RESOURCE_STATE_COMMON"), std::string::npos)
        << "NativeDX12Texture must create resources in COMMON and record the upload "
           "transition explicitly.";
    EXPECT_EQ(constructor.find("D3D12_RESOURCE_STATE_COPY_DEST"), std::string::npos)
        << "Native texture creation must not start in COPY_DEST; GPU-based validation "
           "would then judge draws that do not repeat the transition against it.";
}

TEST(DX12SubmitLaneTests, MismatchedNativeListTypeIsRejectedBeforeQueueingGpuWork)
{
    for (const auto mode : {Context::RhiThreadMode::None, Context::RhiThreadMode::DedicatedThread,
        Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ScopedDx12SubmitLane lane(mode);
        ASSERT_TRUE(lane.ready);
        const auto resources = Backend::CreateDX12DeviceResources(true);
        ASSERT_TRUE(resources.IsValid());
        auto device = CreateExplicitDevice(resources);
        auto pool = device->CreateCommandPool(RHI::QueueType::Compute, "MismatchedComputePool");
        ASSERT_NE(pool, nullptr);
        auto buffer = pool->CreateCommandBuffer("MismatchedComputeList");
        ASSERT_NE(buffer, nullptr);
        buffer->Begin();
        buffer->End();
        ASSERT_TRUE(buffer->IsClosedForSubmission());
        RHI::RHISubmitDesc submit;
        submit.commandBuffers.push_back(buffer);
        submit.signalFence = device->CreateFence("RejectedSubmitFence");
        const auto rejected = device->GetQueue(RHI::QueueType::Graphics)->SubmitChecked(submit);
        EXPECT_EQ(rejected.code, RHI::RHIQueueOperationStatusCode::InvalidArgument);
        EXPECT_NE(rejected.message.find("command list type does not match queue"), std::string::npos);
        EXPECT_FALSE(rejected.mayHaveQueuedGpuWork);
        EXPECT_FALSE(rejected.frameFenceSignalQueued);
        EXPECT_TRUE(SUCCEEDED(resources.device->GetDeviceRemovedReason()));
    }
}

TEST(DX12SubmitLaneTests, FailedPresentReleasesTheSequenceAndRejectsLaterWork)
{
    for (const auto mode : {Context::RhiThreadMode::None, Context::RhiThreadMode::DedicatedThread,
        Context::RhiThreadMode::Tasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        ScopedDx12SubmitLane lane(mode);
        ASSERT_TRUE(lane.ready);
        const auto resources = Backend::CreateDX12DeviceResources(false);
        if (!resources.IsValid())
            GTEST_SKIP() << "DX12 device unavailable on this test machine";
        auto device = CreateExplicitDevice(resources);
        ASSERT_NE(device, nullptr);
        auto queue = device->GetQueue(RHI::QueueType::Graphics);
        ASSERT_NE(queue, nullptr);
        auto& executor = Context::RhiCommandListExecutor::Get();
        const auto sequence = executor.GetDispatchedSequence();
        const auto present = queue->PresentChecked({});
        EXPECT_EQ(present.code, RHI::RHIQueueOperationStatusCode::InvalidArgument);
        EXPECT_FALSE(present.mayHaveQueuedGpuWork);
        EXPECT_TRUE(executor.HasFailed());
        EXPECT_EQ(executor.GetPresentRequestSequence(), sequence + 1u);
        EXPECT_EQ(executor.GetPresentSequence(), sequence + 1u);
        EXPECT_FALSE(executor.Flush(Context::RhiFlushFlags::Swapchain));

        RHI::RHISubmitDesc submit;
        submit.signalFence = device->CreateFence("DX12RejectedSubmitFence");
        const auto rejected = queue->SubmitChecked(submit);
        EXPECT_FALSE(rejected.Succeeded());
        EXPECT_FALSE(rejected.mayHaveQueuedGpuWork);
        EXPECT_FALSE(rejected.frameFenceSignalQueued);
        EXPECT_EQ(executor.GetDispatchedSequence(), sequence + 1u);
    }
}

TEST(DX12SubmitLaneTests, DeviceRemovalNeverCountsAsFenceOrSemaphoreCompletion)
{
    ScopedDx12SubmitLane lane(Context::RhiThreadMode::None);
    ASSERT_TRUE(lane.ready);
    const auto resources = Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable on this test machine";
    Microsoft::WRL::ComPtr<ID3D12Device5> removableDevice;
    if (FAILED(resources.device.As(&removableDevice)))
        GTEST_SKIP() << "ID3D12Device5 is required for the device-removal test";
    auto device = CreateExplicitDevice(resources);
    ASSERT_NE(device, nullptr);
    auto fence = device->CreateFence("DX12RemovedDeviceFence");
    auto semaphore = device->CreateSemaphore("DX12RemovedDeviceSemaphore");
    ASSERT_NE(fence, nullptr);
    ASSERT_NE(semaphore, nullptr);
    RHI::RHISubmitDesc signal;
    signal.signalFence = fence;
    signal.signalSemaphores.push_back(semaphore);
    const auto submitted = device->GetQueue(RHI::QueueType::Graphics)->SubmitChecked(signal);
    ASSERT_TRUE(submitted.Succeeded()) << submitted.message;
    ASSERT_TRUE(fence->Wait(5'000'000'000ull));
    ASSERT_TRUE(semaphore->IsSignaled());
    removableDevice->RemoveDevice();
    EXPECT_FALSE(fence->IsSignaled());
    EXPECT_FALSE(fence->Wait(0u));
    fence->Reset();
    EXPECT_FALSE(fence->IsSignaled());
    EXPECT_FALSE(fence->Wait(1'000'000ull));
    EXPECT_FALSE(semaphore->IsSignaled());
}
#endif
