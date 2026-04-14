#pragma once

#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Settings/EGraphicsBackend.h"

struct GLFWwindow;
struct ImDrawData;

namespace NLS::Render::RHI
{
    class RHITextureView;
    struct NativeHandle;

    class NLS_RENDER_API RHIUIBridge
    {
    public:
        virtual ~RHIUIBridge() = default;

        virtual bool HasRendererBackend() const = 0;
        virtual void BeginFrame() = 0;
        virtual void RenderDrawData(ImDrawData* drawData, uint32_t currentImageIndex) = 0;
        virtual NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>& textureView) = 0;
        virtual void NotifySwapchainWillResize() {}

        // Synchronization - wait semaphore from previous rendering stage
        virtual void SetWaitSemaphore(void* semaphore) = 0;
        // Synchronization - semaphore to signal when UI rendering is complete
        virtual void SetSignalSemaphore(void* semaphore) = 0;
        // Get the UI's signal semaphore for Driver to wait on during present
        virtual void* GetUISignalSemaphore() = 0;
        // Submit the UI command buffer to the GPU
        virtual void SubmitCommandBuffer(uint32_t currentImageIndex) = 0;
    };

    NLS_RENDER_API std::unique_ptr<RHIUIBridge> CreateRHIUIBridge(
        GLFWwindow* window,
        NLS::Render::Settings::EGraphicsBackend backend,
        const std::string& glslVersion,
        const NativeRenderDeviceInfo* nativeDeviceInfo);
}
