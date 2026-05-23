#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

#if defined(_WIN32)
#include <dxgiformat.h>
#endif

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    NLS_RENDER_API DXGI_FORMAT ToDXGIFormat(TextureFormat format);
    NLS_RENDER_API DXGI_FORMAT ToDX12ResourceFormat(TextureFormat format);
    NLS_RENDER_API DXGI_FORMAT ToDX12OptimizedClearFormat(TextureFormat format);
    NLS_RENDER_API uint32_t GetDXGIFormatBytesPerPixel(DXGI_FORMAT format);
    NLS_RENDER_API bool IsDepthStencilFormat(TextureFormat format);
#endif
}
