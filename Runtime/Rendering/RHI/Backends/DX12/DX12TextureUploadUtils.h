#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI::DX12
{
    struct NLS_RENDER_API DX12TextureUploadSubresource
    {
        uint32_t mipLevel = 0;
        uint32_t arrayLayer = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;
        size_t dataOffset = 0;
        size_t rowPitch = 0;
        size_t slicePitch = 0;
    };

    struct NLS_RENDER_API DX12TextureUploadPlan
    {
        std::vector<DX12TextureUploadSubresource> subresources;
        size_t totalBytes = 0;
    };

    NLS_RENDER_API DX12TextureUploadPlan BuildDX12TextureUploadPlan(const RHITextureDesc& desc);
}
