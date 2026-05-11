#include "Rendering/Resources/ShaderParameterMetadata.h"

namespace NLS::Render::Resources
{
    std::vector<ShaderParameterStruct> ShaderRootParameterMetadata::ToParameterStructs() const
    {
        return groups;
    }
}
