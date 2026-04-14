#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"

#if defined(_WIN32)
#include <d3d12.h>
#endif

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    namespace
    {
        uint32_t AlignUp(uint32_t value, uint32_t alignment)
        {
            return alignment == 0u
                ? value
                : ((value + alignment - 1u) / alignment) * alignment;
        }
    }

    uint32_t GetDX12ReadbackBytesPerPixel(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return 4u;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return 8u;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return 16u;
        default:
            return 0u;
        }
    }

    DX12ReadbackLayout BuildDX12ReadbackLayout(DXGI_FORMAT format, uint32_t width, uint32_t height)
    {
        DX12ReadbackLayout layout{};
        layout.bytesPerPixel = GetDX12ReadbackBytesPerPixel(format);

        if (layout.bytesPerPixel == 0u || width == 0u || height == 0u)
            return layout;

        const uint32_t packedRowSize = width * layout.bytesPerPixel;
        layout.rowPitch = AlignUp(packedRowSize, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        layout.readbackSize = static_cast<uint64_t>(layout.rowPitch) * static_cast<uint64_t>(height);
        return layout;
    }
#endif
}
