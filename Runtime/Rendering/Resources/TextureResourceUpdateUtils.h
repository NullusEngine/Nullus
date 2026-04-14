#pragma once

#include <cstdint>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::Resources
{
    NLS_RENDER_API bool ShouldRecreateRHITexture(
        const RHI::RHITextureDesc& existingDesc,
        uint32_t width,
        uint32_t height,
        RHI::TextureFormat format,
        const void* initialData);
}
