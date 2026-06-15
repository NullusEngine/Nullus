#include "Rendering/RHI/Utils/RHIUIBridge.h"

#include "Debug/Logger.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/RHI/Utils/RHIUIBridgeInternal.h"

#include <algorithm>

namespace NLS::Render::RHI
{
    bool RHIUICurrentFrameTextureRetirementTracker::RetireCurrentFrameUse(
        const RHIUITextureHandleUse& use,
        const bool referencedByCurrentFrame)
    {
        if (!referencedByCurrentFrame || use.textureViewKey == 0u || use.descriptorIndex == 0u)
            return false;

        if (!IsRetiredCurrentFrameUse(use))
            m_retiredCurrentFrameUses.push_back(use);
        return true;
    }

    bool RHIUICurrentFrameTextureRetirementTracker::IsRetiredCurrentFrameUse(
        const RHIUITextureHandleUse& use) const
    {
        return std::find(
            m_retiredCurrentFrameUses.begin(),
            m_retiredCurrentFrameUses.end(),
            use) != m_retiredCurrentFrameUses.end();
    }

    void RHIUICurrentFrameTextureRetirementTracker::RemoveViewKey(const uintptr_t textureViewKey)
    {
        m_retiredCurrentFrameUses.erase(
            std::remove_if(
                m_retiredCurrentFrameUses.begin(),
                m_retiredCurrentFrameUses.end(),
                [textureViewKey](const RHIUITextureHandleUse& use)
                {
                    return use.textureViewKey == textureViewKey;
                }),
            m_retiredCurrentFrameUses.end());
    }

    std::vector<RHIUITextureHandleUse> RHIUICurrentFrameTextureRetirementTracker::DiscardCurrentFrame()
    {
        auto uses = std::move(m_retiredCurrentFrameUses);
        m_retiredCurrentFrameUses.clear();
        return uses;
    }

    void RHIUICurrentFrameTextureRetirementTracker::RetainCurrentFrame()
    {
        m_retiredCurrentFrameUses.clear();
    }

    namespace
    {
        class NullUIBridge final : public RHIUIBridge
        {
        public:
            NativeBackendType GetNativeBackendType() const override { return NativeBackendType::None; }
            bool HasRendererBackend() const override { return false; }
            void BeginFrame() override {}
            void RenderDrawData(ImDrawData*, uint32_t, const WaitSemaphoreResolver& = {}) override {}
            NativeHandle ResolveTextureView(const std::shared_ptr<RHITextureView>&) override { return {}; }
            void ReleaseTextureViewHandle(const std::shared_ptr<RHITextureView>&) override {}
            void RetireTextureViewHandle(const std::shared_ptr<RHITextureView>&) override {}
            void SetWaitSemaphore(NativeHandle) override {}
            void SetSignalSemaphore(NativeHandle) override {}
            NativeHandle GetUISignalSemaphore() override { return {}; }
            uint64_t GetUISignalValue() const override { return 0u; }
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
