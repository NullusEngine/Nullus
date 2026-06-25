#pragma once

#include <map>
#include <string>
#include <string_view>

#include "RenderDef.h"

namespace NLS::Render::Resources
{
    NLS_RENDER_API std::string EscapeMaterialField(std::string_view value);
    NLS_RENDER_API std::string UnescapeMaterialField(std::string_view value);
    NLS_RENDER_API std::map<std::string, std::string> ParseMaterialKeyValueTail(std::string_view tail);
}
