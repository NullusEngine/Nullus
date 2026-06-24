#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "Rendering/ShaderLab/ShaderLabTypes.h"

namespace NLS::Render::ShaderLab
{
    struct ShaderLabPassRuntime;

    struct NLS_RENDER_API ShaderLabParseResult
    {
        ShaderLabAssetDesc asset;
        std::vector<ShaderLabDiagnostic> diagnostics;

        [[nodiscard]] bool Succeeded() const { return diagnostics.empty(); }
        [[nodiscard]] std::string DiagnosticsToString() const { return FormatShaderLabDiagnostics(diagnostics); }
    };

    NLS_RENDER_API ShaderLabParseResult ParseShaderLabSource(
        std::string_view source,
        std::string filePath = {});

    NLS_RENDER_API std::string BuildShaderLabHlslForCompile(const ShaderLabPassDesc& pass);
    NLS_RENDER_API std::string BuildShaderLabHlslForCompile(const ShaderLabPassRuntime& pass);
}
