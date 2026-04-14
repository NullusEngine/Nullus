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

        // The current Formal RHI texture API has no in-place data upload/update path,
        // so any call with initial data must recreate the underlying texture resource.
        return initialData != nullptr;
    }
}
