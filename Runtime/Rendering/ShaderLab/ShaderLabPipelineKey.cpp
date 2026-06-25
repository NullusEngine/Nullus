#include "Rendering/ShaderLab/ShaderLabPipelineKey.h"

namespace NLS::Render::ShaderLab
{
    uint64_t ShaderLabGraphicsPipelineKey::Hash() const
    {
        uint64_t hash = 0;
        for (const uint8_t byte : program.shaderGuid.GetBytes())
            HashShaderLabCombine(hash, byte);
        HashShaderLabCombine(hash, program.subShaderIndex);
        HashShaderLabCombine(hash, program.passIndex);
        HashShaderLabCombine(hash, program.keywordHash);
        HashShaderLabCombine(hash, program.vertexArtifactHash);
        HashShaderLabCombine(hash, program.fragmentArtifactHash);
        HashShaderLabCombine(hash, program.computeArtifactHash);
        HashShaderLabCombine(hash, renderStateHash);
        HashShaderLabCombine(hash, vertexLayoutHash);
        HashShaderLabCombine(hash, renderTargetLayoutHash);
        HashShaderLabCombine(hash, static_cast<uint64_t>(topology));
        HashShaderLabCombine(hash, sampleCount);
        return hash;
    }
}
