#pragma once

#include <cstddef>
#include <optional>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "RenderDef.h"

namespace NLS::Render::Context
{
    class Driver;

    struct NLS_RENDER_API RenderThreadCoordinator final
    {
        static bool IsThreadedRenderingEnabled(const Driver& driver);
        static bool BeginRendererFrame(Driver& driver, bool acquireSwapchainImage);
        static void EndRendererFrame(Driver& driver, bool presentSwapchain);
        static bool TryPublishHarnessFrameSnapshot(
            Driver& driver,
            const FrameSnapshot& snapshot,
            size_t* publishedSlotIndex = nullptr);
        static bool TryPublishHarnessPreparedFrame(
            Driver& driver,
            const FrameSnapshot& snapshot,
            const RenderScenePackage& renderScenePackage,
            size_t* publishedSlotIndex = nullptr);
        static bool TryPublishPreparedFrameBuilder(
            Driver& driver,
            const FrameSnapshot& snapshot,
            PreparedRenderSceneBuilder renderSceneBuilder,
            bool applyPendingSwapchainResize = true,
            size_t* publishedSlotIndex = nullptr,
            uint64_t* publishedFrameId = nullptr);
        static bool DrainPendingRenderFrameBuildsSynchronously(Driver& driver);
        static ThreadedFrameTelemetry GetThreadedFrameTelemetry(const Driver& driver);
        static std::optional<ThreadedFrameTelemetry> TryGetThreadedFrameTelemetry(const Driver& driver);
    };
}
