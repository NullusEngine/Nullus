#pragma once

#include <string>
#include <string_view>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/Resources/Material.h"
#include "RenderDef.h"

namespace NLS::Render::Resources
{
    struct NLS_RENDER_API MaterialVariantIdentity
    {
        std::string stableKey;
    };

    struct NLS_RENDER_API MaterialPassVariantKey
    {
        std::string stableKey;
    };

    NLS_RENDER_API MaterialVariantIdentity BuildMaterialVariantIdentity(const Material& material);

    NLS_RENDER_API MaterialPassVariantKey BuildMaterialPassVariantKey(
        const Material& material,
        std::string_view passName,
        const Data::PipelineState& pipelineState,
        const MaterialPipelineStateOverrides& overrides);
}
