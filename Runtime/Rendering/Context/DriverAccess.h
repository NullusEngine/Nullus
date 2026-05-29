#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "RenderDef.h"

namespace NLS::Maths
{
    class Vector4;
}

namespace NLS::Render::Data
{
    enum class FramePublishState : uint8_t;
    struct PipelineState;
}

namespace NLS::Render::FrameGraph
{
    struct FrameGraphExecutionContext;
}

namespace NLS::Render::Settings
{
    enum class EGraphicsBackend : uint8_t;
    enum class EComparaisonAlgorithm : uint8_t;
    enum class EPixelDataFormat : uint8_t;
    enum class EPixelDataType : uint8_t;
    enum class EPrimitiveMode : uint8_t;
    enum class ERasterizationMode : uint8_t;
    struct EngineDiagnosticsSettings;
}

namespace NLS::Render::Resources
{
    class IMesh;
    class Material;
    struct MaterialPipelineStateOverrides;
}

namespace NLS::Render::RHI
{
    enum class BufferType : uint8_t;
    enum class BufferUsage : uint8_t;
    class RHIBuffer;
    class RHICommandBuffer;
    class RHIBindingLayout;
    class RHIBindingSet;
    class DescriptorAllocator;
    class RHIDevice;
    class PipelineCache;
    class RHITexture;
    class RHITextureView;
    class RHISwapchain;
    enum class TextureDimension : uint8_t;
    struct RHIReadbackResult;
    struct NativeRenderDeviceInfo;
    struct FramebufferDesc;
    struct RHIBufferDesc;
    struct RHIBindingLayoutDesc;
    struct RHIBindingSetDesc;
    struct RHIFrameContext;
    struct RHITextureViewDesc;
}

namespace NLS::Render::Context
{
    class Driver;
    class ThreadedRenderingLifecycle;
    struct FrameSnapshot;
    struct RenderScenePackage;
    struct ThreadedFrameTelemetry;

    NLS_RENDER_API Driver* TryGetLocatedDriver();
    NLS_RENDER_API Driver& RequireLocatedDriver(std::string_view ownerName = {});
    NLS_RENDER_API std::optional<Settings::EGraphicsBackend> TryGetLocatedActiveGraphicsBackend();

    struct NLS_RENDER_API DriverRendererAccess final
    {
        static bool HasExplicitRHI(const Driver& driver);
        static bool IsThreadedRenderingEnabled(const Driver& driver);
        static bool IsLightGridEnabled(const Driver& driver);
        static std::shared_ptr<RHI::RHIDevice> GetExplicitDevice(const Driver& driver);
        static bool TryPublishPreparedFrameBuilder(
            Driver& driver,
            const FrameSnapshot& snapshot,
            PreparedRenderSceneBuilder renderSceneBuilder,
            size_t* publishedSlotIndex = nullptr,
            uint64_t* publishedFrameId = nullptr);
        static bool TryDrainThreadedRendering(Driver& driver);
        static void DrainThreadedRendering(Driver& driver);
        static ThreadedFrameTelemetry GetThreadedFrameTelemetry(const Driver& driver);
        static std::optional<ThreadedFrameTelemetry> TryGetThreadedFrameTelemetry(const Driver& driver);

        static void SetViewport(
            Driver& driver,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height);
        static Data::PipelineState CreatePipelineState(const Driver& driver);
        static bool SupportsEditorPickingReadback(const Driver& driver);
        static FrameGraph::FrameGraphExecutionContext CreateFrameGraphExecutionContext(const Driver& driver);
        static std::optional<size_t> GetActiveFrameContextSlotIndex(const Driver& driver);
        static size_t GetFrameContextSlotCount(const Driver& driver);
        static size_t GetLifecycleFrameSlotCount(const Driver& driver);
        static std::optional<size_t> ReserveReusableFrameContextSlotIndex(Driver& driver);
        static bool ReleaseReservedFrameContextSlotIndex(Driver& driver, size_t slotIndex);
        static std::optional<size_t> GetReservedFrameContextSlotIndex(const Driver& driver);
        static std::shared_ptr<RHI::RHICommandBuffer> GetActiveExplicitCommandBuffer(const Driver& driver);
        static std::shared_ptr<RHI::RHITextureView> GetSwapchainBackbufferView(const Driver& driver);
        static std::shared_ptr<RHI::RHITextureView> GetSwapchainDepthStencilView(const Driver& driver);
        static std::shared_ptr<RHI::RHITexture> ResolveReadbackTexture(const Driver& driver);
        static bool HasCompletedReadbackTexture(
            const Driver& driver,
            const std::shared_ptr<RHI::RHITexture>& texture);
        static std::shared_ptr<RHI::PipelineCache> GetPipelineCache(const Driver& driver);
        static std::shared_ptr<RHI::DescriptorAllocator> GetActiveDescriptorAllocator(const Driver& driver);

        static std::shared_ptr<RHI::RHIBindingLayout> CreateExplicitBindingLayout(
            const Driver& driver,
            const RHI::RHIBindingLayoutDesc& desc);

        static std::shared_ptr<RHI::RHIBindingSet> CreateExplicitBindingSet(
            const Driver& driver,
            const RHI::RHIBindingSetDesc& desc,
            RHI::DescriptorAllocationLifetime allocationLifetime =
                RHI::DescriptorAllocationLifetime::TransientFrame);

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

        static void Clear(
            Driver& driver,
            bool colorBuffer,
            bool depthBuffer,
            bool stencilBuffer,
            const Maths::Vector4& color);
        static const Settings::EngineDiagnosticsSettings& GetDiagnosticsSettings(const Driver& driver);
        static void SetDiagnosticsSettings(Driver& driver, const Settings::EngineDiagnosticsSettings& settings);
    };

    struct NLS_RENDER_API DriverUIAccess final
    {
        struct UICompositionSyncBoundary
        {
            RHI::NativeHandle sceneToUiWaitSemaphore;
            RHI::NativeHandle uiToPresentSignalSemaphore;
            uint64_t uiToPresentSignalValue = 0u;
        };

        static RHI::NativeRenderDeviceInfo GetNativeDeviceInfo(const Driver& driver);
        static bool PrepareUIRender(Driver& driver);
        static void ReleaseUITextureHandles(Driver& driver);
        static void PresentSwapchain(Driver& driver);
        static void SetPolygonMode(Driver& driver, Settings::ERasterizationMode mode);
        static bool IsRenderDocAvailable(const Driver& driver);
        static bool QueueRenderDocCapture(Driver& driver, const std::string& label = {});
        static bool OpenLatestRenderDocCapture(const Driver& driver);
        static std::string GetRenderDocCaptureDirectory(const Driver& driver);
        static bool GetRenderDocAutoOpenEnabled(const Driver& driver);
        static void SetRenderDocAutoOpenEnabled(Driver& driver, bool enabled);
        static void SetRenderDocEnabled(Driver& driver, bool enabled);
        static bool IsRenderDocEnabled(const Driver& driver);

        // UI synchronization - get the semaphore UI should wait on before rendering
        static RHI::NativeHandle GetRenderFinishedSemaphore(Driver& driver);
        static UICompositionSyncBoundary BuildUICompositionSyncBoundary(Driver& driver);
        static void SetUICompositionSignal(Driver& driver, RHI::NativeHandle semaphore, uint64_t value);
        // UI synchronization - set the semaphore UI signals after rendering (Present waits on this)
        static void SetUISignalSemaphore(Driver& driver, RHI::NativeHandle semaphore, uint64_t value);
    };

    struct NLS_RENDER_API DriverTestAccess final
    {
        static void SetExplicitDevice(Driver& driver, std::shared_ptr<RHI::RHIDevice> explicitDevice);
        static void RebuildExplicitFrameContexts(Driver& driver, size_t frameContextCount);
        static void SetExplicitSwapchain(Driver& driver, std::shared_ptr<RHI::RHISwapchain> explicitSwapchain);
        static RHI::RHIFrameContext& EnsureFrameContext(Driver& driver, size_t index);
        static const RHI::RHIFrameContext* PeekFrameContext(const Driver& driver, size_t index);
        static void SetCompletedReadbackTexture(
            Driver& driver,
            std::shared_ptr<RHI::RHITexture> texture);
        static void SetExplicitFrameActive(Driver& driver, bool active);
        static void AgePendingSwapchainResize(
            Driver& driver,
            std::chrono::steady_clock::duration age);
        static bool TryLockThreadedRhiSubmission(Driver& driver);
        static void UnlockThreadedRhiSubmission(Driver& driver);
        static void LockThreadedRhiSubmission(Driver& driver);
        static void SetUiStandaloneFramePending(Driver& driver, bool pending);
#if defined(NLS_ENABLE_TEST_HOOKS)
        static void ExpireUiStandaloneFramePendingLease(Driver& driver);
#endif
        static bool TryLockDriverTelemetry(Driver& driver);
        static void UnlockDriverTelemetry(Driver& driver);
        static ThreadedRenderingLifecycle* GetThreadedRenderingLifecycle(Driver& driver);
        static const ThreadedRenderingLifecycle* GetThreadedRenderingLifecycle(const Driver& driver);
        static bool CanBeginStandaloneExplicitFrame(const Driver& driver);
        static bool BeginStandaloneExplicitFrame(Driver& driver, bool acquireSwapchainImage = true);
        static void EndStandaloneExplicitFrame(Driver& driver, bool presentSwapchain = true);
        static void PauseThreadedRenderingWorkers(Driver& driver);
        static bool TryDrainThreadedRendering(Driver& driver);
        static void DrainThreadedRendering(Driver& driver);
        static bool TryPublishHarnessFrameSnapshot(
            Driver& driver,
            const FrameSnapshot& snapshot,
            size_t* publishedSlotIndex = nullptr);
        static bool TryPublishHarnessPreparedFrame(
            Driver& driver,
            const FrameSnapshot& snapshot,
            const RenderScenePackage& renderScenePackage,
            size_t* publishedSlotIndex = nullptr);
        static bool ResolveAndCompleteThreadedRenderScene(
            Driver& driver,
            size_t slotIndex);
    };
}
