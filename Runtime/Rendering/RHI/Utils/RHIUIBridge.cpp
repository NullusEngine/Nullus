#include "Rendering/RHI/Utils/RHIUIBridge.h"

#include "Debug/Logger.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/RHI/Utils/RHIUIBridgeInternal.h"

namespace NLS::Render::RHI
{
    namespace
    {
        class NullUIBridge final : public RHIUIBridge
        {
        public:
            NativeBackendType GetNativeBackendType() const override { return NativeBackendType::None; }
            bool HasRendererBackend() const override { return false; }
            void BeginFrame() override {}
            void RenderDrawData(ImDrawData*, uint32_t) override {}
            NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>&) override { return {}; }
            void SetWaitSemaphore(void*) override {}
            void SetSignalSemaphore(void*) override {}
            void* GetUISignalSemaphore() override { return nullptr; }
            void SubmitCommandBuffer(uint32_t) override {}
        };
    }

    std::unique_ptr<RHIUIBridge> CreateRHIUIBridge(
        GLFWwindow*,
        const std::string&,
        const NativeRenderDeviceInfo* nativeDeviceInfo)
    {
        NativeRenderDeviceInfo resolvedNativeInfo;
        if (nativeDeviceInfo != nullptr)
        {
            resolvedNativeInfo = *nativeDeviceInfo;
            NLS_LOG_INFO("CreateRHIUIBridge: using provided native device info (device=" +
                std::to_string(reinterpret_cast<uintptr_t>(resolvedNativeInfo.device)) +
                ", swapchain=" + std::to_string(reinterpret_cast<uintptr_t>(resolvedNativeInfo.swapchain)) + ")");
        }
        else
        {
            resolvedNativeInfo = ResolveUIDriverNativeDeviceInfo();
            NLS_LOG_INFO("CreateRHIUIBridge: resolved native device info from driver");
        }

        NLS_LOG_INFO("CreateRHIUIBridge: resolved native backend=" +
            std::to_string(static_cast<int>(resolvedNativeInfo.backend)));

        const auto resolvedGraphicsBackend = Render::Settings::ToGraphicsBackend(resolvedNativeInfo.backend);
        if (!Render::Settings::SupportsImGuiRendererBackend(resolvedGraphicsBackend))
        {
            NLS_LOG_WARNING(
                "CreateRHIUIBridge: phase-1 selector rejected backend " +
                std::string(Render::Settings::ToString(resolvedGraphicsBackend)) +
                "; returning null UI bridge.");
            return std::make_unique<NullUIBridge>();
        }

#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
        if (resolvedNativeInfo.backend == NativeBackendType::DX12)
        {
            return CreateDX12RHIUIBridge(resolvedNativeInfo);
        }
#endif

        return std::make_unique<NullUIBridge>();
    }
}
