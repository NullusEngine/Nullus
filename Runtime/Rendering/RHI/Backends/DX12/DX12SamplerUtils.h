#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

#if defined(_WIN32)
#include <d3d12.h>
#endif

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    NLS_RENDER_API D3D12_FILTER ToD3D12Filter(
        TextureFilter minFilter,
        TextureFilter magFilter,
        TextureMipFilter mipFilter,
        uint32_t maxAnisotropy,
        bool comparison);
    NLS_RENDER_API D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(TextureWrap wrap);
    NLS_RENDER_API D3D12_COMPARISON_FUNC ToD3D12ComparisonFunc(
        NLS::Render::Settings::EComparaisonAlgorithm algorithm);
    NLS_RENDER_API D3D12_SAMPLER_DESC BuildDX12SamplerDesc(const SamplerDesc& desc);
#endif
}
