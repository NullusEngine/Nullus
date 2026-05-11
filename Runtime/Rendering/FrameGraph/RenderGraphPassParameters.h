#pragma once

#include <memory>

#include "Rendering/RenderDef.h"

namespace NLS::Render::FrameGraph
{
    class NLS_RENDER_API RenderGraphPassParameterAllocator
    {
    public:
        template <typename TParameters>
        [[nodiscard]] std::unique_ptr<TParameters> AllocParameters() const
        {
            return std::make_unique<TParameters>();
        }
    };
}
