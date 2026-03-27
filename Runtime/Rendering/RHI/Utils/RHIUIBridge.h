#pragma once

#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/Settings/EGraphicsBackend.h"

struct GLFWwindow;
struct ImDrawData;

namespace NLS::Render::RHI
{
    class NLS_RENDER_API RHIUIBridge
    {
    public:
        virtual ~RHIUIBridge() = default;

        virtual bool HasRendererBackend() const = 0;
        virtual void BeginFrame() = 0;
        virtual void RenderDrawData(ImDrawData* drawData) = 0;
        virtual void* ResolveTextureID(uint32_t textureId) = 0;
        virtual void NotifySwapchainWillResize() {}
    };

    NLS_RENDER_API std::unique_ptr<RHIUIBridge> CreateRHIUIBridge(
        GLFWwindow* window,
        NLS::Render::Settings::EGraphicsBackend backend,
        const NativeRenderDeviceInfo& nativeDeviceInfo,
        const std::string& glslVersion);
}
