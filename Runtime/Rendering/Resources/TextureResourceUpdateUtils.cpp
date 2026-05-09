#include "Rendering/Resources/TextureResourceUpdateUtils.h"

namespace NLS::Render::Resources
{
    bool ShouldRecreateRHITexture(
        const RHI::RHITextureDesc& existingDesc,
        uint32_t width,
        uint32_t height,
        RHI::TextureFormat format,
        const void* initialData)
    {
        if (existingDesc.extent.width != width || existingDesc.extent.height != height)
            return true;

        if (existingDesc.format != format)
            return true;

        return false;
    }

    bool CanUpdateRHITextureInPlace(
        const RHI::RHITextureDesc& existingDesc,
        uint32_t width,
        uint32_t height,
        RHI::TextureFormat format,
        const void* initialData)
    {
        return initialData != nullptr &&
            existingDesc.extent.width == width &&
            existingDesc.extent.height == height &&
            existingDesc.format == format;
    }
}
