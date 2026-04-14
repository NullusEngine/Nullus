#pragma once

#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIPipeline.h"

#if defined(_WIN32)
#include <d3d12.h>
#undef CreateSemaphore
#endif

namespace NLS::Render::RHI::DX12
{
#if defined(_WIN32)
    struct NLS_RENDER_API DX12OwnedInputLayout
    {
        std::vector<std::string> semanticNames;
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    };

    NLS_RENDER_API DX12OwnedInputLayout BuildDX12OwnedInputLayout(const RHIGraphicsPipelineDesc& desc);
#endif
}
