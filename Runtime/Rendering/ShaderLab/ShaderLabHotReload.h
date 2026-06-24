#pragma once

#include <memory>
#include <vector>

#include "Rendering/ShaderLab/ShaderLabAsset.h"
#include "Rendering/ShaderLab/ShaderLabMaterial.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "Rendering/ShaderLab/ShaderLabVariant.h"

namespace NLS::Render::ShaderLab
{
    struct NLS_RENDER_API ShaderLabReloadValidationResult
    {
        bool succeeded = false;
        std::vector<ShaderLabDiagnostic> diagnostics;
    };

    class NLS_RENDER_API IShaderLabReloadValidator
    {
    public:
        virtual ~IShaderLabReloadValidator() = default;
        virtual ShaderLabReloadValidationResult Validate(
            const ShaderLabAssetDesc& candidate,
            const std::vector<ShaderLabVariantKey>& requestedVariants) = 0;
    };

    struct NLS_RENDER_API ShaderLabReloadRequest
    {
        std::shared_ptr<ShaderLabAsset> shader;
        ShaderLabAssetDesc candidate;
        std::vector<ShaderLabMaterialReloadDependency> dependentMaterials;
        std::vector<ShaderLabVariantKey> requestedVariants;
    };

    struct NLS_RENDER_API ShaderLabReloadResult
    {
        bool succeeded = false;
        uint64_t invalidatedPipelineGeneration = 0;
        std::vector<ShaderLabDiagnostic> diagnostics;

        [[nodiscard]] std::string DiagnosticsToString() const { return FormatShaderLabDiagnostics(diagnostics); }
    };

    class NLS_RENDER_API ShaderLabHotReloadService
    {
    public:
        explicit ShaderLabHotReloadService(IShaderLabReloadValidator& validator);

        [[nodiscard]] ShaderLabReloadResult Reload(const ShaderLabReloadRequest& request) const;

    private:
        IShaderLabReloadValidator& m_validator;
    };
}
