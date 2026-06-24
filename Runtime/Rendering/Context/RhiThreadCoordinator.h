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
        static bool DrainPendingThreadedSubmissions(
            Driver& driver,
            RhiSubmissionAttribution attribution,
            bool applyPendingSwapchainResize = true);
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
