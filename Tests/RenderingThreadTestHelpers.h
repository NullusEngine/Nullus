#pragma once

#include <future>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Context/RenderingThreadSystem.h>
#include <Rendering/Context/FrameSubmissionTelemetry.h>
#include <Rendering/RenderScene.h>

namespace NLS::Tests
{
    inline std::future<std::pair<Render::Context::RenderFramePacket, Render::Context::RenderScenePackage>>
        ObserveNextRenderFrameForTest(Render::Context::Driver& driver)
    {
        using Observation = std::pair<Render::Context::RenderFramePacket, Render::Context::RenderScenePackage>;
        const auto result = std::make_shared<std::promise<Observation>>();
        auto future = result->get_future();
        Render::Context::DriverTestAccess::SetRenderSceneObserver(driver,
            [result, observed = false](const auto& packet, const auto& package) mutable
            {
                if (!observed)
                {
                    result->set_value({packet, package});
                    observed = true;
                }
            });
        return future;
    }

    inline std::future<Render::Context::RenderScenePackage> ObserveNextRenderSceneForTest(
        Render::Context::Driver& driver, const uint64_t frameId = 0u)
    {
        const auto result = std::make_shared<std::promise<Render::Context::RenderScenePackage>>();
        auto future = result->get_future();
        Render::Context::DriverTestAccess::SetRenderSceneObserver(driver,
            [result, frameId, observed = false](const auto& packet, const auto& package) mutable
            {
                if (!observed && (frameId == 0u || packet.frameId == frameId))
                {
                    result->set_value(package);
                    observed = true;
                }
            });
        return future;
    }

    inline std::future<Render::Context::RhiSubmissionFrame> ObserveNextRhiFrameForTest(
        Render::Context::Driver& driver, const uint64_t frameId = 0u)
    {
        const auto result = std::make_shared<std::promise<Render::Context::RhiSubmissionFrame>>();
        auto future = result->get_future();
        Render::Context::DriverTestAccess::SetRhiFrameObserver(driver,
            [result, frameId, observed = false](const auto& frame) mutable
            {
                if (!observed && (frameId == 0u || frame.frameId == frameId))
                {
                    result->set_value(frame);
                    observed = true;
                }
            });
        return future;
    }

    class ScopedRenderFailureReset final
    {
    public:
        ScopedRenderFailureReset() { Render::Context::ResetRenderFatalErrorForTesting(); }
        ~ScopedRenderFailureReset() { Render::Context::ResetRenderFatalErrorForTesting(); }
    };

    template <typename Function>
    auto RunOnRenderThread(Function&& function) -> std::invoke_result_t<Function>
    {
        using namespace NLS::Render::Context;
        if (RenderingThreadSystem::IsInRenderingThread())
            return std::forward<Function>(function)();

        // Headless fixtures and explicitly paused drivers have joined their lane.
        // Running drivers must execute the same command on their dedicated thread.
        std::unique_ptr<ScopedRenderThreadForTesting> joinedLane;
        if (!RenderingThreadSystem::HasActiveRenderThread())
            joinedLane = std::make_unique<ScopedRenderThreadForTesting>();
        auto task = std::make_shared<std::packaged_task<std::invoke_result_t<Function>()>>(
            std::forward<Function>(function));
        auto result = task->get_future();
        RenderCommandEvent completion;
        EnqueueRenderCommand<struct RenderThreadTestCommand>([task] { (*task)(); }, completion);
        if (joinedLane != nullptr)
            (void)RenderCommandDispatcher::Dispatch();
        completion.Wait();
        if (!completion.Succeeded())
            throw std::runtime_error(completion.GetDiagnostic());
        return result.get();
    }

    inline Engine::Rendering::RenderSceneSyncStats CompleteSceneSynchronizationForTest(
        Engine::Rendering::RenderScene& renderScene,
        Engine::Rendering::RenderSceneSyncRequest request,
        const Engine::Rendering::RenderSceneSyncOptions& options = {})
    {
        return RunOnRenderThread([&] { return renderScene.CompleteSynchronize(std::move(request), options); });
    }

    inline Engine::Rendering::RenderSceneSyncStats SynchronizeSceneForTest(
        Engine::Rendering::RenderScene& renderScene,
        Engine::SceneSystem::Scene& scene,
        const Engine::Rendering::RenderSceneSyncOptions& options = {})
    {
        return CompleteSceneSynchronizationForTest(renderScene, renderScene.BeginSynchronize(scene, options), options);
    }

    inline Engine::Rendering::SceneRenderStoreApplyStats ApplyDroppedSceneCaptureForTest(
        Engine::Rendering::RenderScene& renderScene,
        const Engine::Rendering::RenderSceneSyncRequest& request)
    {
        return RunOnRenderThread([&] { return renderScene.ApplyDroppedSynchronizeCapture(request); });
    }
}

#define NLS_RENDER_THREAD_TEST(suite, name) \
    static void suite##_##name##_RenderBody(); \
    TEST(suite, name) \
    { \
        NLS::Tests::RunOnRenderThread(suite##_##name##_RenderBody); \
    } \
    static void suite##_##name##_RenderBody()
