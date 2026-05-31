#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgiformat.h>
#endif

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    NLS_RENDER_API DXGI_FORMAT ToDXGIFormat(
        TextureFormat format,
        TextureColorSpace colorSpace = TextureColorSpace::Linear);
    NLS_RENDER_API DXGI_FORMAT ToDX12ResourceFormat(
        TextureFormat format,
        TextureColorSpace colorSpace = TextureColorSpace::Linear);
    NLS_RENDER_API DXGI_FORMAT ToDX12OptimizedClearFormat(
        TextureFormat format,
        TextureColorSpace colorSpace = TextureColorSpace::Linear);
    NLS_RENDER_API uint32_t GetDXGIFormatBytesPerPixel(DXGI_FORMAT format);
    NLS_RENDER_API bool IsDepthStencilFormat(TextureFormat format);
    NLS_RENDER_API TextureFormatCapability BuildDX12TextureFormatCapability(
        TextureFormat format,
        D3D12_FORMAT_SUPPORT1 support1,
        D3D12_FORMAT_SUPPORT2 support2,
        bool supportsSrgbView = false,
        std::string diagnosticReason = {});
#endif
}
