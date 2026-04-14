#pragma once

#include <cstdint>

#include "RenderDef.h"

#if defined(_WIN32)
#include <dxgiformat.h>
#endif

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    struct NLS_RENDER_API DX12ReadbackLayout
    {
        uint32_t bytesPerPixel = 0;
        uint32_t rowPitch = 0;
        uint64_t readbackSize = 0;
    };

    NLS_RENDER_API uint32_t GetDX12ReadbackBytesPerPixel(DXGI_FORMAT format);
    NLS_RENDER_API DX12ReadbackLayout BuildDX12ReadbackLayout(DXGI_FORMAT format, uint32_t width, uint32_t height);
#endif
}
