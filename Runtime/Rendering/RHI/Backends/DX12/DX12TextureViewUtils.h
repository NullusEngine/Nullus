#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"

#include <vector>

#if defined(_WIN32)
#include <d3d12.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    struct NLS_RENDER_API DX12TextureViewDescriptorSet
    {
        bool hasSrv = false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        bool hasRtv = false;
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        bool hasDsv = false;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    };

    NLS_RENDER_API DX12TextureViewDescriptorSet BuildDX12TextureViewDescriptorSet(
        const RHITextureDesc& textureDesc,
        const RHITextureViewDesc& viewDesc);

    NLS_RENDER_API bool DoesDX12BarrierRangeCoverWholeTexture(
        const RHITextureDesc& textureDesc,
        const RHISubresourceRange& subresourceRange);

    NLS_RENDER_API bool TryResolveDX12BarrierSubresourceIndex(
        const RHITextureDesc& textureDesc,
        const RHISubresourceRange& subresourceRange,
        UINT& outSubresourceIndex);

    NLS_RENDER_API UINT ResolveDX12BarrierSubresourceIndex(
        const RHITextureDesc& textureDesc,
        const RHISubresourceRange& subresourceRange);

    NLS_RENDER_API std::vector<UINT> BuildDX12BarrierSubresourceIndices(
        const RHITextureDesc& textureDesc,
        const RHISubresourceRange& subresourceRange);
#endif
}
