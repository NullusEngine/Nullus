#pragma once

#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::RHI
{
    enum class NLS_RENDER_API RenderSurfaceOrigin : uint8_t
    {
        TopLeft,
        BottomLeft
    };

    struct NLS_RENDER_API RenderSurfaceConvention
    {
        RenderSurfaceOrigin presentedTextureOrigin = RenderSurfaceOrigin::TopLeft;
        RenderSurfaceOrigin renderTargetOrigin = RenderSurfaceOrigin::TopLeft;

        constexpr bool RequiresPresentedTextureVerticalFlip() const
        {
            return presentedTextureOrigin == RenderSurfaceOrigin::BottomLeft;
        }

        constexpr bool UsesBottomLeftRenderTargetOrigin() const
        {
            return renderTargetOrigin == RenderSurfaceOrigin::BottomLeft;
        }
    };

    inline constexpr RenderSurfaceConvention GetRenderSurfaceConvention(const NativeBackendType backend)
    {
        switch (backend)
        {
        case NativeBackendType::OpenGL:
            return {
                RenderSurfaceOrigin::BottomLeft,
                RenderSurfaceOrigin::BottomLeft
            };
        case NativeBackendType::DX12:
        case NativeBackendType::DX11:
        case NativeBackendType::Vulkan:
        case NativeBackendType::Metal:
        case NativeBackendType::None:
        default:
            return {};
        }
    }
}
