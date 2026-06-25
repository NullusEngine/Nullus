#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "Rendering/RenderDef.h"
#include "Rendering/ShaderLab/ShaderLabSourceLocation.h"

namespace NLS::Render::ShaderLab
{
    struct NLS_RENDER_API ShaderLabDiagnostic
    {
        std::string message;
        ShaderLabSourceLocation location;
    };

    inline std::string FormatShaderLabDiagnostics(const std::vector<ShaderLabDiagnostic>& diagnostics)
    {
        std::ostringstream stream;
        for (const auto& diagnostic : diagnostics)
        {
            stream << diagnostic.location.file << '('
                << diagnostic.location.line << ','
                << diagnostic.location.column << "): "
                << diagnostic.message << '\n';
        }
        return stream.str();
    }
}
