#pragma once

#include <functional>
#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIEnums.h"

struct GLFWwindow;
struct ImDrawData;

namespace NLS::Render::RHI
{
    class RHITextureView;
    struct NativeHandle;
    using WaitSemaphoreResolver = std::function<NativeHandle()>;

    class NLS_RENDER_API RHIUIBridge
    {
    public:
        virtual ~RHIUIBridge() = default;

        virtual NativeBackendType GetNativeBackendType() const = 0;
        virtual bool HasRendererBackend() const = 0;
        virtual void BeginFrame() = 0;
        virtual void RenderDrawData(
            ImDrawData* drawData,
            uint32_t currentImageIndex,
            const WaitSemaphoreResolver& resolveWaitSemaphore = {}) = 0;
        virtual NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>& textureView) = 0;
        // After this returns, the caller may destroy the texture/view. Backends must retire
        // any UI descriptor or submitted draw work that can still reference textureView.
        virtual void ReleaseTextureViewHandle(const std::shared_ptr<RHITextureView>& textureView) = 0;
        // Non-blocking release for transient UI textures. Backends must keep textureView
        // alive until submitted UI work no longer references its descriptor; the caller may
        // only release its own reference after this returns.
        virtual void RetireTextureViewHandle(const std::shared_ptr<RHITextureView>& textureView)
        {
            ReleaseTextureViewHandle(textureView);
        }
        virtual void NotifySwapchainWillResize() {}
        virtual void NotifyFontAtlasChanged() {}

        // Synchronization - wait semaphore from previous rendering stage
        virtual void SetWaitSemaphore(NativeHandle semaphore) = 0;
        // Synchronization - semaphore to signal when UI rendering is complete
        virtual void SetSignalSemaphore(NativeHandle semaphore) = 0;
        // Get the UI's signal semaphore for Driver to wait on during present
        virtual NativeHandle GetUISignalSemaphore() = 0;
        virtual uint64_t GetUISignalValue() const = 0;
        // Submit the UI command buffer to the GPU
        virtual void SubmitCommandBuffer(uint32_t currentImageIndex) = 0;
    };

    NLS_RENDER_API std::unique_ptr<RHIUIBridge> CreateRHIUIBridge(
        GLFWwindow* window,
        const std::string& glslVersion,
        const NativeRenderDeviceInfo* nativeDeviceInfo);
}
