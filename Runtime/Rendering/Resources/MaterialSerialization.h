#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Resources
{
    NLS_RENDER_API std::string EscapeMaterialField(std::string_view value);
    NLS_RENDER_API std::string UnescapeMaterialField(std::string_view value);
    NLS_RENDER_API std::map<std::string, std::string> ParseMaterialKeyValueTail(std::string_view tail);
    NLS_RENDER_API std::string SerializeMaterialSampler(const RHI::SamplerDesc& sampler);
    NLS_RENDER_API std::optional<RHI::SamplerDesc> DeserializeMaterialSampler(std::string_view text);
    struct SerializedMaterialAssetReference
    {
        std::string parameter, path;
        bool shader = false;
    };
    NLS_RENDER_API std::vector<SerializedMaterialAssetReference> CollectSerializedMaterialAssetReferences(std::string_view payload);
}
