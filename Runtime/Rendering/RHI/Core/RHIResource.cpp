#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI
{
    bool RHITexture::RequiresExternalClearValueMessageFilter() const
    {
        return false;
    }
}
