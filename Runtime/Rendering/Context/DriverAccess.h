#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "RenderDef.h"

namespace NLS::Maths
{
    class Vector4;
}

namespace NLS::Render::Data
{
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
    class IRHIBuffer;
    class IRHITexture;
    class RHIBuffer;
    class RHICommandBuffer;
    class RHIBindingLayout;
    class RHIBindingSet;
    class RHIDevice;
    class RHITexture;
    class RHITextureView;
    enum class TextureDimension : uint8_t;
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

    NLS_RENDER_API Driver* TryGetLocatedDriver();
    NLS_RENDER_API Driver& RequireLocatedDriver(std::string_view ownerName = {});
    NLS_RENDER_API std::optional<Settings::EGraphicsBackend> TryGetLocatedActiveGraphicsBackend();

    struct DriverRendererAccess final
    {
        static bool HasExplicitRHI(const Driver& driver);
        static void BeginExplicitFrame(Driver& driver, bool acquireSwapchainImage = true);
        static void EndExplicitFrame(Driver& driver, bool presentSwapchain = true);
        static std::shared_ptr<RHI::RHIDevice> GetExplicitDevice(const Driver& driver);

        static void SetViewport(
            Driver& driver,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height);
        static Data::PipelineState CreatePipelineState(const Driver& driver);
        static bool SupportsEditorPickingReadback(const Driver& driver);
        static bool SupportsFramebufferReadback(const Driver& driver);
        static FrameGraph::FrameGraphExecutionContext CreateFrameGraphExecutionContext(const Driver& driver);
        static std::shared_ptr<RHI::RHICommandBuffer> GetActiveExplicitCommandBuffer(const Driver& driver);
        static std::shared_ptr<RHI::RHITextureView> GetSwapchainBackbufferView(const Driver& driver);
        static void TransitionTextureToShaderRead(
            Driver& driver,
            const std::shared_ptr<RHI::RHITexture>& texture);

        static std::shared_ptr<RHI::RHIBindingLayout> CreateExplicitBindingLayout(
            const Driver& driver,
            const RHI::RHIBindingLayoutDesc& desc);

        static std::shared_ptr<RHI::RHIBindingSet> CreateExplicitBindingSet(
            const Driver& driver,
            const RHI::RHIBindingSetDesc& desc);

        static void ReadPixels(
            const Driver& driver,
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
        static void BindDefaultCompatibilityFramebuffer(Driver& driver);
        static bool SubmitMaterialDraw(
            Driver& driver,
            const Resources::Material& material,
            Data::PipelineState pipelineState,
            const Resources::MaterialPipelineStateOverrides& overrides,
            const Resources::IMesh& mesh,
            Settings::EPrimitiveMode primitiveMode,
            Settings::EComparaisonAlgorithm depthCompare,
            uint32_t instances,
            bool allowExplicitRecording);
    };

    struct DriverUIAccess final
    {
        static RHI::NativeRenderDeviceInfo GetNativeDeviceInfo(const Driver& driver);
        static void* GetUITextureHandle(
            const Driver& driver,
            const std::shared_ptr<RHI::RHITextureView>& textureView);
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

        // Vulkan UI synchronization - get the semaphore UI should wait on before rendering
        static void* GetRenderFinishedSemaphore(Driver& driver);
        // Vulkan UI synchronization - set the semaphore UI signals after rendering (Present waits on this)
        static void SetUISignalSemaphore(Driver& driver, void* semaphore);
    };

    struct DriverTestAccess final
    {
        static void SetExplicitDevice(Driver& driver, std::shared_ptr<RHI::RHIDevice> explicitDevice);
        static RHI::RHIFrameContext& EnsureFrameContext(Driver& driver, size_t index);
        static const RHI::RHIFrameContext* PeekFrameContext(const Driver& driver, size_t index);
        static void SetExplicitFrameActive(Driver& driver, bool active);
    };
}
