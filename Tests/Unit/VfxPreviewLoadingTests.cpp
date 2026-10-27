#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <Guid.h>
#include <Jobs/JobSystem.h>
#include <Jobs/BackgroundJobQueue.h>
#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/Settings/DriverSettings.h>
#include "VFX/Editor/VfxSession.h"
#include "VFX/Editor/VfxSystemPreview.h"

TEST(VfxPreviewLoading, SerialRenderLaneDoesNotWaitForPreviewCompilationOrClosing)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "The startup regression uses a DX12 device";
#else
    using namespace NLS;
    namespace context = Render::Context;
    namespace jobs = Base::Jobs;
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized() || jobs::IsJobSystemInitialized() || context::RenderCommandDispatcher::GetTaskGraph())
        GTEST_SKIP() << "Another fixture owns the rendering or job runtime";
    jobs::JobSystemConfig config; config.workerCount = 1; config.backgroundWorkerCount = 1;
    ASSERT_TRUE(jobs::InitializeJobSystem(config));
    struct ShutdownJobs { ~ShutdownJobs() { jobs::ShutdownJobSystem(); } } shutdownJobs;
    executor.Initialize(context::RhiThreadMode::None,{});
    struct ShutdownRhi { context::RhiCommandListExecutor& executor; ~ShutdownRhi() { executor.Shutdown(); } } shutdownRhi{executor};
    Render::Settings::DriverSettings settings; settings.graphicsBackend = Render::Settings::EGraphicsBackend::DX12; settings.debugMode = true;
    auto device = Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "DX12 device unavailable";
    executor.Initialize(context::RhiThreadMode::None,context::RhiCommandListExecutor::CreateDeviceTargets(device));
    const auto root = std::filesystem::temp_directory_path()/("NullusVfxOpen_"+Guid::New().ToString());
    struct Remove { std::filesystem::path path; ~Remove() { std::error_code error; std::filesystem::remove_all(path,error); } } remove{root};
    Editor::VFXTools::VfxSession session;
    ASSERT_TRUE(session.Rebuild());

    // Hold the actual compiler lane. A frame and preview close must progress
    // before this gate opens; a sleep-based timing assertion cannot prove it.
    struct Gate
    {
        std::mutex mutex; std::condition_variable changed;
        bool entered = false, released = false;
        void Release() { { std::lock_guard lock(mutex); released = true; } changed.notify_all(); }
    } gate;
    jobs::BackgroundJobDesc blocker;
    blocker.function = [](void* data)
    {
        auto& gate = *static_cast<Gate*>(data); std::unique_lock lock(gate.mutex);
        gate.entered = true; gate.changed.notify_all();
        gate.changed.wait(lock,[&] { return gate.released; });
    };
    blocker.userData = &gate; blocker.debugName = "VfxPreviewLoading::CompilerGate";
    auto handle = jobs::ScheduleBackgroundJob(blocker); ASSERT_NE(handle.id,0u);
    struct Release
    {
        Gate& gate; jobs::JobHandle& handle;
        ~Release() { gate.Release(); jobs::Complete(handle); jobs::ShutdownJobSystem(); }
    } release{gate,handle};
    {
        std::unique_lock lock(gate.mutex);
        ASSERT_TRUE(gate.changed.wait_for(lock,std::chrono::seconds(5),[&] { return gate.entered; }));
    }
    Editor::VFXTools::VfxSystemPreview preview;
    Editor::VFXTools::VfxSystemPreviewInput input; input.width = 64; input.height = 64;
    const auto start = std::chrono::steady_clock::now();
    preview.Update(device,session.Program(),session.TakeFrames(),root,input);
    context::RenderCommandEvent frame;
    context::RenderCommandDispatcher::Enqueue([frame] { frame.Signal(); });
    context::RenderCommandDispatcher::Dispatch();
    RecordProperty("opening_frame_ms",std::to_string(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()));
    EXPECT_TRUE(frame.Succeeded());
    EXPECT_EQ(preview.Status(),"Compiling shaders...");
    EXPECT_FALSE(preview.CanAdvance());
    EXPECT_FALSE(preview.View());
    gate.Release(); jobs::Complete(handle);
    std::shared_ptr<Render::RHI::RHITextureView> loaded;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        preview.Update(device,session.Program(),{},root,input);
        context::RenderCommandDispatcher::Dispatch();
        loaded = preview.View(); if (loaded) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(loaded) << preview.Status();
    // Recompile with a new generation, then close while the work is queued.
    {
        std::lock_guard lock(gate.mutex); gate.entered = false; gate.released = false;
    }
    handle = jobs::ScheduleBackgroundJob(blocker); ASSERT_NE(handle.id,0u);
    {
        std::unique_lock lock(gate.mutex);
        ASSERT_TRUE(gate.changed.wait_for(lock,std::chrono::seconds(5),[&] { return gate.entered; }));
    }
    ASSERT_TRUE(session.Rebuild(true));
    preview.Update(device,session.Program(),session.TakeFrames(),root,input);
    EXPECT_EQ(preview.View(),loaded);
    EXPECT_EQ(preview.Status(),"Compiling shaders...");
    preview.Clear();
    EXPECT_FALSE(preview.View());
    EXPECT_EQ(preview.Status(),"Preview requires a rendering device");
    // Closing cancels publication; releasing queued work must not resurrect it.
    gate.Release(); jobs::Complete(handle); jobs::ShutdownJobSystem();
    context::RenderCommandDispatcher::FlushToDrain();
    EXPECT_FALSE(preview.View());
#endif
}
