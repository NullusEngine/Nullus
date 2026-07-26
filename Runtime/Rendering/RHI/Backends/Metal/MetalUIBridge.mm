#include "Rendering/RHI/Backends/Metal/MetalUIBridge.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "Debug/Logger.h"
#include "ImGui/backends/imgui_impl_metal.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI
{
#if NLS_HAS_IMGUI_METAL_BACKEND
    namespace
    {
        class MetalUIBridge final : public RHIUIBridge
        {
        public:
            MetalUIBridge(id<MTLDevice> device, id<MTLCommandQueue> queue, CAMetalLayer* layer)
                : m_device([device retain])
                , m_queue([queue retain])
                , m_layer([layer retain])
                , m_renderPassDescriptor([[MTLRenderPassDescriptor alloc] init])
            {
                m_initialized = ImGui_ImplMetal_Init(m_device);
            }

            ~MetalUIBridge() override
            {
                ReleaseDrawable();
                if (m_initialized)
                    ImGui_ImplMetal_Shutdown();
                [m_renderPassDescriptor release];
                [m_layer release];
                [m_queue release];
                [m_device release];
            }

            NativeBackendType GetNativeBackendType() const override { return NativeBackendType::Metal; }
            bool HasRendererBackend() const override { return m_initialized; }
            void BeginFrame() override
            {
                if (!m_initialized || m_layer == nil)
                    return;

                ReleaseDrawable();
                id<CAMetalDrawable> drawable = [m_layer nextDrawable];
                if (drawable == nil)
                    return;

                m_drawable = [drawable retain];
                auto* colorAttachment = m_renderPassDescriptor.colorAttachments[0];
                colorAttachment.texture = m_drawable.texture;
                colorAttachment.loadAction = MTLLoadActionClear;
                colorAttachment.storeAction = MTLStoreActionStore;
                colorAttachment.clearColor = MTLClearColorMake(0.055, 0.067, 0.090, 1.0);
                ImGui_ImplMetal_NewFrame(m_renderPassDescriptor);
                m_frameReady = true;
            }
            void RenderDrawData(
                ImDrawData* drawData,
                uint32_t,
                const WaitSemaphoreResolver& = {}) override
            {
                if (!m_initialized || !m_frameReady || m_drawable == nil || drawData == nullptr)
                    return;

                id<MTLCommandBuffer> commandBuffer = [m_queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder =
                    [commandBuffer renderCommandEncoderWithDescriptor:m_renderPassDescriptor];
                ImGui_ImplMetal_RenderDrawData(drawData, commandBuffer, encoder);
                [encoder endEncoding];
                [commandBuffer presentDrawable:m_drawable];
                [commandBuffer commit];

                m_renderPassDescriptor.colorAttachments[0].texture = nil;
                ReleaseDrawable();
                m_frameReady = false;
            }
            NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>& textureView) override
            {
                if (textureView == nullptr)
                    return {};
                const auto handle = textureView->GetNativeShaderResourceView();
                return handle.backend == BackendType::Metal ? handle : NativeHandle{};
            }
            void ReleaseTextureViewHandle(const std::shared_ptr<RHITextureView>&) override {}
            void RetireTextureViewHandle(const std::shared_ptr<RHITextureView>&) override {}
            void NotifySwapchainWillResize() override
            {
                // The next drawable is acquired only after the Driver applies the new CAMetalLayer size.
                ReleaseDrawable();
                m_frameReady = false;
            }
            void SetWaitSemaphore(NativeHandle semaphore) override { m_waitSemaphore = semaphore; }
            void SetSignalSemaphore(NativeHandle semaphore) override { m_signalSemaphore = semaphore; }
            NativeHandle GetUISignalSemaphore() override { return m_signalSemaphore; }
            uint64_t GetUISignalValue() const override { return 0u; }
            void SubmitCommandBuffer(uint32_t) override
            {
                // RenderDrawData commits the Metal command buffer immediately because this path owns presentation.
            }

        private:
            void ReleaseDrawable()
            {
                [m_drawable release];
                m_drawable = nil;
            }

            id<MTLDevice> m_device = nil;
            id<MTLCommandQueue> m_queue = nil;
            CAMetalLayer* m_layer = nil;
            MTLRenderPassDescriptor* m_renderPassDescriptor = nil;
            id<CAMetalDrawable> m_drawable = nil;
            NativeHandle m_waitSemaphore{};
            NativeHandle m_signalSemaphore{};
            bool m_initialized = false;
            bool m_frameReady = false;
        };
    }
#endif

    std::unique_ptr<RHIUIBridge> CreateMetalRHIUIBridge(const NativeRenderDeviceInfo& nativeDeviceInfo)
    {
#if NLS_HAS_IMGUI_METAL_BACKEND
        if (nativeDeviceInfo.backend != NativeBackendType::Metal ||
            nativeDeviceInfo.device == nullptr ||
            nativeDeviceInfo.graphicsQueue == nullptr ||
            nativeDeviceInfo.swapchain == nullptr)
        {
            NLS_LOG_ERROR("CreateMetalRHIUIBridge: missing Metal device, command queue, or CAMetalLayer.");
            return nullptr;
        }

        auto bridge = std::make_unique<MetalUIBridge>(
            (__bridge id<MTLDevice>)nativeDeviceInfo.device,
            (__bridge id<MTLCommandQueue>)nativeDeviceInfo.graphicsQueue,
            (__bridge CAMetalLayer*)nativeDeviceInfo.swapchain);
        return bridge->HasRendererBackend() ? std::move(bridge) : nullptr;
#else
        (void)nativeDeviceInfo;
        return nullptr;
#endif
    }
}
