#include "Rendering/ShaderLab/ShaderLabHotReload.h"

#include "Rendering/ShaderLab/ShaderLabMaterial.h"
#include "Rendering/ShaderLab/ShaderLabReloadBarrier.h"

#include <mutex>

namespace NLS::Render::ShaderLab
{
    ShaderLabHotReloadService::ShaderLabHotReloadService(IShaderLabReloadValidator& validator)
        : m_validator(validator)
    {
    }

    ShaderLabReloadResult ShaderLabHotReloadService::Reload(const ShaderLabReloadRequest& request) const
    {
        ShaderLabReloadResult result;
        if (request.shader == nullptr)
        {
            result.diagnostics.push_back({ "missing shader", {} });
            return result;
        }

        const auto validation = m_validator.Validate(request.candidate, request.requestedVariants);
        if (!validation.succeeded)
        {
            request.shader->SetLastDiagnostics(validation.diagnostics);
            result.diagnostics = validation.diagnostics;
            return result;
        }

        {
            const std::unique_lock barrier(GetShaderLabReloadBarrier());
            const bool replaced = request.shader->ReplaceSnapshot(request.candidate, validation.diagnostics);
            if (!replaced)
            {
                result.diagnostics.push_back({ "shader replacement failed", {} });
                return result;
            }
            for (const auto& material : request.dependentMaterials)
            {
                auto liveMaterial = material.Resolve();
                if (liveMaterial == nullptr)
                    continue;

                const auto dependencyShader = material.shader.lock();
                if (dependencyShader != request.shader || material.shaderGuid != request.shader->GetGuidUnderBarrier())
                {
                    result.diagnostics.push_back({
                        "skipped material reload dependency because it belongs to a different shader",
                        {}
                    });
                    continue;
                }

                if (liveMaterial->IsBoundToShaderUnderBarrier(request.shader))
                {
                    liveMaterial->ReloadShaderUnderBarrier(request.shader, request.candidate);
                }
                else
                {
                    result.diagnostics.push_back({
                        "skipped material reload dependency because the material shader changed",
                        {}
                    });
                }
            }
            result.invalidatedPipelineGeneration = request.shader->GetGenerationUnderBarrier();
            result.succeeded = true;
            request.shader->SetLastDiagnostics({});
        }

        return result;
    }
}
