#pragma once

#include <cstdint>
#include <memory>

#include "RenderDef.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"

#if defined(_WIN32)
#include <dxgiformat.h>
#endif

namespace NLS::Render::RHI
{
    class RHITexture;
}

struct ID3D12CommandQueue;
struct ID3D12Device;

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
    NLS_RENDER_API void ExecuteDX12ReadPixels(
        ID3D12Device* device,
        ID3D12CommandQueue* graphicsQueue,
        const std::shared_ptr<RHITexture>& texture,
        uint32_t x,
        uint32_t y,
        uint32_t width,
        uint32_t height,
        NLS::Render::Settings::EPixelDataFormat format,
        NLS::Render::Settings::EPixelDataType type,
        void* data);
#endif
}
