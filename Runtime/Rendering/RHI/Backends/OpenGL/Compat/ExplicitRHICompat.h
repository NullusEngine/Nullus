#pragma once

#include <memory>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/GraphicsPipelineDesc.h"
#include "Rendering/Settings/EPrimitiveMode.h"

namespace NLS::Render::Context
{
    class Driver;
}

namespace NLS::Render::Resources
{
    class IMesh;
    class BindingSetInstance;
}

namespace NLS::Render::RHI
{
    class IRenderDevice;
    class IRHIBuffer;
    class IRHITexture;

    NLS_RENDER_API std::shared_ptr<RHIDevice> CreateCompatibilityExplicitDevice(IRenderDevice& renderDevice);
    NLS_RENDER_API std::shared_ptr<RHISampler> CreateCompatibilitySampler(const SamplerDesc& desc, std::string debugName = {});
    NLS_RENDER_API std::shared_ptr<RHIBindingLayout> CreateCompatibilityBindingLayout(const RHIBindingLayoutDesc& desc);
    NLS_RENDER_API std::shared_ptr<RHIBindingSet> CreateCompatibilityBindingSet(const RHIBindingSetDesc& desc);
    NLS_RENDER_API std::shared_ptr<RHIPipelineLayout> CreateCompatibilityPipelineLayout(const RHIPipelineLayoutDesc& desc);
    NLS_RENDER_API std::shared_ptr<RHIGraphicsPipeline> CreateCompatibilityGraphicsPipeline(const GraphicsPipelineDesc& legacyDesc);
    NLS_RENDER_API std::shared_ptr<RHIBindingSet> WrapCompatibilityBindingSet(const NLS::Render::Resources::BindingSetInstance* legacyBindingSet);
    NLS_RENDER_API std::shared_ptr<RHIBuffer> WrapCompatibilityBuffer(const std::shared_ptr<IRHIBuffer>& legacyBuffer, std::string debugName = {});
    NLS_RENDER_API std::shared_ptr<RHITexture> WrapCompatibilityTexture(const std::shared_ptr<IRHITexture>& legacyTexture, std::string debugName = {});
    NLS_RENDER_API std::shared_ptr<RHITextureView> CreateCompatibilityTextureView(const std::shared_ptr<RHITexture>& texture, const RHITextureViewDesc& desc);
}
