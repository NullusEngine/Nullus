#pragma once

#include <cstdint>

#include "Guid.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/ShaderLab/ShaderLabVariant.h"

namespace NLS::Render::ShaderLab
{
    struct NLS_RENDER_API ShaderLabProgramId
    {
        NLS::Guid shaderGuid = NLS::Guid::Empty();
        uint16_t subShaderIndex = 0;
        uint16_t passIndex = 0;
        uint64_t keywordHash = 0;
        uint64_t vertexArtifactHash = 0;
        uint64_t fragmentArtifactHash = 0;
        uint64_t computeArtifactHash = 0;
    };

    struct NLS_RENDER_API ShaderLabGraphicsPipelineKey
    {
        ShaderLabProgramId program;
        uint64_t renderStateHash = 0;
        uint64_t vertexLayoutHash = 0;
        uint64_t renderTargetLayoutHash = 0;
        NLS::Render::RHI::PrimitiveTopology topology =
            NLS::Render::RHI::PrimitiveTopology::TriangleList;
        uint8_t sampleCount = 1;

        [[nodiscard]] uint64_t Hash() const;
    };
}
