#pragma once

#include <cstdint>
#include <memory>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIDevice.h"

namespace NLS::Render::Settings
{
    enum class EPixelDataFormat : uint8_t;
    enum class EPixelDataType : uint8_t;
}

namespace NLS::Render::RHI
{
    class RHITexture;
}

namespace NLS::Render::Context
{
    class Driver;
    enum class RhiSubmissionAttribution : uint8_t;

    struct NLS_RENDER_API RhiThreadCoordinator final
    {
        static bool CanBeginStandaloneExplicitFrame(const Driver& driver);
        static bool BeginStandaloneExplicitFrame(Driver& driver, bool acquireSwapchainImage);
        static void EndStandaloneExplicitFrame(Driver& driver, bool presentSwapchain);
        static bool TryExecuteNextThreadedSubmission(
            Driver& driver,
            RhiSubmissionAttribution attribution,
            bool applyPendingSwapchainResize = true);
        // Processes resource uploads even when no render-scene submission is queued.
        // Thumbnail preparation can enqueue runtime meshes between regular frames.
        static bool TryProcessPendingMeshRuntimeUploads(Driver& driver);
        static bool DrainPendingThreadedSubmissions(
            Driver& driver,
            RhiSubmissionAttribution attribution,
            bool applyPendingSwapchainResize = true);
        /**
         * Producer-side asynchronous submission. The calling thread owns the RHI
         * submission lane: it takes the next published frame and submits the stage
         * graph without waiting. Stage nodes then run on TaskGraph workers and on the
         * RHI named thread, which only pumps its own queue.
         */
        static bool TryScheduleNextThreadedSubmission(
            Driver& driver,
            RhiSubmissionAttribution attribution);
        /**
         * Schedules idle mesh runtime uploads on the RHI named thread when no frame is
         * in flight, so uploads still progress while the RHI worker only pumps.
         */
        static bool TryScheduleIdleMeshRuntimeUploads(Driver& driver);
        /**
         * Releases the producer lane once its scheduled work retired and optionally
         * applies a pending swapchain resize. Must run on the scheduling thread.
         */
        static void FinalizeThreadedSubmissionProduction(
            Driver& driver,
            bool applyPendingSwapchainResize);
        /**
         * Blocks until the producer lane is idle again. Called by the producing thread
         * on shutdown so an exiting thread never leaves the lane locked.
         */
        static void DrainProducerOwnedThreadedSubmission(Driver& driver);
        static void ReadPixels(
            const Driver& driver,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data);
        static void ReadPixels(
            const Driver& driver,
            const std::shared_ptr<RHI::RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data);
        static RHI::RHIReadbackResult ReadPixelsChecked(
            const Driver& driver,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data);
        static RHI::RHIReadbackResult ReadPixelsChecked(
            const Driver& driver,
            const std::shared_ptr<RHI::RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data);
        static RHI::RHIReadbackResult BeginReadPixels(
            const Driver& driver,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data);
        static RHI::RHIReadbackResult BeginReadPixels(
            const Driver& driver,
            const std::shared_ptr<RHI::RHITexture>& texture,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            Settings::EPixelDataFormat format,
            Settings::EPixelDataType type,
            void* data);
        static RHI::RHIReadbackResult PollReadbackCompletion(
            const Driver& driver,
            const RHI::RHIReadbackResult& readback);
        static bool PrepareUIRender(Driver& driver);
        static void PresentSwapchain(Driver& driver);
    };
}
