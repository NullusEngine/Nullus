// Runtime/Rendering/RHI/Core/RHICommandList.cpp
#include "Rendering/RHI/Core/RHICommandList.h"

namespace NLS::Render::RHI
{

// Command struct constructors - properly initialize base Command class
BeginRenderPassCmd::BeginRenderPassCmd() : Command(Type::BeginRenderPass) {}
EndRenderPassCmd::EndRenderPassCmd() : Command(Type::EndRenderPass) {}
SetViewportCmd::SetViewportCmd() : Command(Type::SetViewport) {}
SetScissorCmd::SetScissorCmd() : Command(Type::SetScissor) {}
BindGraphicsPipelineCmd::BindGraphicsPipelineCmd() : Command(Type::BindGraphicsPipeline) {}
BindComputePipelineCmd::BindComputePipelineCmd() : Command(Type::BindComputePipeline) {}
BindBindingSetCmd::BindBindingSetCmd() : Command(Type::BindBindingSet) {}
PushConstantsCmd::PushConstantsCmd() : Command(Type::PushConstants) {}
BindVertexBufferCmd::BindVertexBufferCmd() : Command(Type::BindVertexBuffer) {}
BindIndexBufferCmd::BindIndexBufferCmd() : Command(Type::BindIndexBuffer) {}
DrawCmd::DrawCmd() : Command(Type::Draw) {}
DrawIndexedCmd::DrawIndexedCmd() : Command(Type::DrawIndexed) {}
DrawInstancedCmd::DrawInstancedCmd() : Command(Type::DrawInstanced) {}
DrawIndexedInstancedCmd::DrawIndexedInstancedCmd() : Command(Type::DrawIndexedInstanced) {}
DispatchCmd::DispatchCmd() : Command(Type::Dispatch) {}
DispatchIndirectCmd::DispatchIndirectCmd() : Command(Type::DispatchIndirect) {}
SetStencilRefCmd::SetStencilRefCmd() : Command(Type::SetStencilRef) {}
SetBlendFactorCmd::SetBlendFactorCmd() : Command(Type::SetBlendFactor) {}
UAVBarrierCmd::UAVBarrierCmd() : Command(Type::UAVBarrier) {}
AliasBarrierCmd::AliasBarrierCmd() : Command(Type::AliasBarrier) {}
CopyBufferCmd::CopyBufferCmd() : Command(Type::CopyBuffer) {}
CopyBufferToTextureCmd::CopyBufferToTextureCmd() : Command(Type::CopyBufferToTexture) {}
CopyTextureCmd::CopyTextureCmd() : Command(Type::CopyTexture) {}
BarrierCmd::BarrierCmd() : Command(Type::Barrier) {}

// DefaultRHICommandList 方法实现

void DefaultRHICommandList::BeginRenderPass(const RenderPassDesc& desc)
{
    state_ = State::Recording;
    auto cmd = std::make_shared<BeginRenderPassCmd>();
    cmd->desc = desc;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::EndRenderPass()
{
    commands_.push_back(std::make_shared<EndRenderPassCmd>());
}

void DefaultRHICommandList::SetViewport(const Viewport& viewport)
{
    auto cmd = std::make_shared<SetViewportCmd>();
    cmd->viewport = viewport;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::SetScissor(const RHIRect2D& rect)
{
    auto cmd = std::make_shared<SetScissorCmd>();
    cmd->rect = rect;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::BindGraphicsPipeline(const std::shared_ptr<RHIGraphicsPipeline>& pipeline)
{
    auto cmd = std::make_shared<BindGraphicsPipelineCmd>();
    cmd->pipeline = pipeline;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::BindComputePipeline(const std::shared_ptr<RHIComputePipeline>& pipeline)
{
    auto cmd = std::make_shared<BindComputePipelineCmd>();
    cmd->pipeline = pipeline;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::BindBindingSet(uint32_t setIndex, const std::shared_ptr<RHIBindingSet>& bindingSet)
{
    auto cmd = std::make_shared<BindBindingSetCmd>();
    cmd->setIndex = setIndex;
    cmd->bindingSet = bindingSet;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::PushConstants(ShaderStageMask stageMask, uint32_t offset, uint32_t size, const void* data)
{
    auto cmd = std::make_shared<PushConstantsCmd>();
    cmd->stageMask = stageMask;
    cmd->offset = offset;
    cmd->size = size;
    if (data && size > 0)
    {
        const uint8_t* byteData = static_cast<const uint8_t*>(data);
        cmd->data.assign(byteData, byteData + size);
    }
    commands_.push_back(cmd);
}

void DefaultRHICommandList::BindVertexBuffer(uint32_t slot, const RHIVertexBufferView& view)
{
    auto cmd = std::make_shared<BindVertexBufferCmd>();
    cmd->slot = slot;
    cmd->view = view;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::BindIndexBuffer(const RHIIndexBufferView& view)
{
    auto cmd = std::make_shared<BindIndexBufferCmd>();
    cmd->view = view;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::Draw(const DrawArguments& args)
{
    auto cmd = std::make_shared<DrawCmd>();
    cmd->args = args;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::DrawIndexed(const DrawIndexedArguments& args)
{
    auto cmd = std::make_shared<DrawIndexedCmd>();
    cmd->args = args;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::DrawInstanced(const DrawInstancedArguments& args)
{
    auto cmd = std::make_shared<DrawInstancedCmd>();
    cmd->args = args;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::DrawIndexedInstanced(const DrawIndexedInstancedArguments& args)
{
    auto cmd = std::make_shared<DrawIndexedInstancedCmd>();
    cmd->args = args;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::Dispatch(const DispatchArguments& args)
{
    auto cmd = std::make_shared<DispatchCmd>();
    cmd->args = args;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::DispatchIndirect(RHIBuffer* indirectBuffer, uint64_t offset)
{
    auto cmd = std::make_shared<DispatchIndirectCmd>();
    cmd->indirectBuffer = indirectBuffer;
    cmd->offset = offset;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::SetStencilRef(uint32_t stencilRef)
{
    auto cmd = std::make_shared<SetStencilRefCmd>();
    cmd->stencilRef = stencilRef;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::SetBlendFactor(const float blendFactor[4])
{
    auto cmd = std::make_shared<SetBlendFactorCmd>();
    if (blendFactor)
    {
        cmd->blendFactor[0] = blendFactor[0];
        cmd->blendFactor[1] = blendFactor[1];
        cmd->blendFactor[2] = blendFactor[2];
        cmd->blendFactor[3] = blendFactor[3];
    }
    commands_.push_back(cmd);
}

void DefaultRHICommandList::UAVBarrier(IRHIResource* resource)
{
    auto cmd = std::make_shared<UAVBarrierCmd>();
    cmd->resource = resource;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::AliasBarrier(IRHIResource* before, IRHIResource* after)
{
    auto cmd = std::make_shared<AliasBarrierCmd>();
    cmd->before = before;
    cmd->after = after;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::CopyBuffer(const CopyBufferArguments& args)
{
    auto cmd = std::make_shared<CopyBufferCmd>();
    cmd->args = args;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::CopyBufferToTexture(const CopyBufferToTextureArguments& args)
{
    auto cmd = std::make_shared<CopyBufferToTextureCmd>();
    cmd->args = args;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::CopyTexture(const CopyTextureArguments& args)
{
    auto cmd = std::make_shared<CopyTextureCmd>();
    cmd->args = args;
    commands_.push_back(cmd);
}

void DefaultRHICommandList::Barrier(const BarrierDesc& barrier)
{
    auto cmd = std::make_shared<BarrierCmd>();
    cmd->barrier = barrier;
    commands_.push_back(cmd);
}

} // namespace NLS::Render::RHI