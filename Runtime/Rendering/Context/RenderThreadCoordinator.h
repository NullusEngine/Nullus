#pragma once

#include <cstddef>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "RenderDef.h"

namespace NLS::Render::Context
{
    class Driver;

    struct NLS_RENDER_API RenderThreadCoordinator final
    {
        static bool IsThreadedRenderingEnabled(const Driver& driver);
        static void BeginRendererFrame(Driver& driver, bool acquireSwapchainImage);
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
            size_t* publishedSlotIndex = nullptr);
        static bool DrainPendingRenderFrameBuildsSynchronously(Driver& driver);
        static ThreadedFrameTelemetry GetThreadedFrameTelemetry(const Driver& driver);
    };
}
