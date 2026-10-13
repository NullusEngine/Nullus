#pragma once

#include <cstddef>
#include <optional>

#include "Rendering/Context/FrameSubmissionTelemetry.h"
#include "RenderDef.h"

namespace NLS::Render::Context
{
    class Driver;

    struct NLS_RENDER_API RenderThreadCoordinator final
    {
        static bool IsThreadedRenderingEnabled(const Driver& driver);
        static bool BeginRendererFrame(
            Driver& driver, bool waitForRetirement = true, bool targetsSwapchain = true);
        static void EndRendererFrame(Driver& driver);
        static bool TryEnqueueRenderFrame(
            Driver& driver,
            const RenderFramePacket& packet,
            RenderSceneBuildCommand renderSceneBuilder,
            bool applyPendingSwapchainResize = true,
            size_t* publishedSlotIndex = nullptr,
            uint64_t* publishedFrameId = nullptr,
            bool backgroundPreview = false,
            // False performs a single non-blocking slot reservation attempt.
            bool waitForRetirement = true);
        static ThreadedFrameTelemetry GetThreadedFrameTelemetry(const Driver& driver);
        static std::optional<ThreadedFrameTelemetry> TryGetThreadedFrameTelemetry(const Driver& driver);

    private:
        static void EnqueuePublishedFrame(
            Driver& driver, size_t slotIndex, RenderFramePacket packet,
            std::shared_ptr<RenderSceneBuildCommand> renderSceneBuilder);
    };
}
