#pragma once

#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/GraphicsPipelineDesc.h"
#include "Rendering/Resources/BindingSetInstance.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/EPrimitiveMode.h"
#include "RenderDef.h"

namespace NLS::Render::Context
{
    class Driver;
}

namespace NLS::Render::RHI
{
    class IRenderDevice;
}

namespace NLS::Render::Resources
{
    class Material;

    class NLS_RENDER_API MaterialCompatibilityDrawState final
    {
    public:
        const Data::PipelineState& GetPipelineState() const;

        void Bind(Context::Driver& driver) const;
        void Bind(RHI::IRenderDevice& renderDevice) const;

    private:
        friend MaterialCompatibilityDrawState BuildMaterialCompatibilityDrawState(
            const Material& material,
            Data::PipelineState pipelineState,
            Settings::EPrimitiveMode primitiveMode,
            Settings::EComparaisonAlgorithm depthCompare);

        Data::PipelineState m_pipelineState{};
        RHI::GraphicsPipelineDesc m_pipelineDesc{};
        BindingSetInstance m_bindingSetStorage{};
        bool m_hasBindingSet = false;
    };

    NLS_RENDER_API MaterialCompatibilityDrawState BuildMaterialCompatibilityDrawState(
        const Material& material,
        Data::PipelineState pipelineState,
        Settings::EPrimitiveMode primitiveMode,
        Settings::EComparaisonAlgorithm depthCompare);
}
