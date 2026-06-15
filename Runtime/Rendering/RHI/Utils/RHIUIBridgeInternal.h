#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Rendering/Context/DriverAccess.h"
#include "RenderDef.h"
#include "Rendering/RHI/Utils/RHIUIBridge.h"

namespace NLS::Render::RHI
{
    struct NLS_RENDER_API RHIUITextureHandleUse
    {
        uintptr_t textureViewKey = 0u;
        uint32_t descriptorIndex = 0u;

        bool operator==(const RHIUITextureHandleUse& other) const
        {
            return textureViewKey == other.textureViewKey &&
                descriptorIndex == other.descriptorIndex;
        }
    };

    class NLS_RENDER_API RHIUICurrentFrameTextureRetirementTracker
    {
    public:
        bool RetireCurrentFrameUse(const RHIUITextureHandleUse& use, bool referencedByCurrentFrame);
        bool IsRetiredCurrentFrameUse(const RHIUITextureHandleUse& use) const;
        void RemoveViewKey(uintptr_t textureViewKey);
        std::vector<RHIUITextureHandleUse> DiscardCurrentFrame();
        void RetainCurrentFrame();

    private:
        std::vector<RHIUITextureHandleUse> m_retiredCurrentFrameUses;
    };

    inline NLS::Render::Context::Driver* ResolveUIDriver()
    {
        return NLS::Render::Context::TryGetLocatedDriver();
    }

    inline NativeRenderDeviceInfo ResolveUIDriverNativeDeviceInfo()
    {
        if (const auto* driver = ResolveUIDriver(); driver != nullptr)
        {
            if (NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(*driver))
                return NLS::Render::Context::DriverUIAccess::GetNativeDeviceInfo(*driver);
        }

        return {};
    }

#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
    std::unique_ptr<RHIUIBridge> CreateDX12RHIUIBridge(const NativeRenderDeviceInfo& nativeInfo);
#endif
}
