// Runtime/Rendering/RHI/Backends/DX11/DX11CommandListExecutor.cpp
#include "DX11CommandListExecutor.h"
#include "Rendering/RHI/Core/RHICommandList.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI::DX11 {

// 使用来自RHICommandList.h的正式Command类型别名
using Command = NLS::Render::RHI::Command;
using SetViewportCmd = NLS::Render::RHI::SetViewportCmd;
using SetScissorCmd = NLS::Render::RHI::SetScissorCmd;
using BindGraphicsPipelineCmd = NLS::Render::RHI::BindGraphicsPipelineCmd;
using BindComputePipelineCmd = NLS::Render::RHI::BindComputePipelineCmd;
using BindBindingSetCmd = NLS::Render::RHI::BindBindingSetCmd;
using PushConstantsCmd = NLS::Render::RHI::PushConstantsCmd;
using BindVertexBufferCmd = NLS::Render::RHI::BindVertexBufferCmd;
using BindIndexBufferCmd = NLS::Render::RHI::BindIndexBufferCmd;
using DrawCmd = NLS::Render::RHI::DrawCmd;
using DrawIndexedCmd = NLS::Render::RHI::DrawIndexedCmd;
using DrawInstancedCmd = NLS::Render::RHI::DrawInstancedCmd;
using DrawIndexedInstancedCmd = NLS::Render::RHI::DrawIndexedInstancedCmd;
using DispatchCmd = NLS::Render::RHI::DispatchCmd;
using DispatchIndirectCmd = NLS::Render::RHI::DispatchIndirectCmd;
using SetStencilRefCmd = NLS::Render::RHI::SetStencilRefCmd;
using SetBlendFactorCmd = NLS::Render::RHI::SetBlendFactorCmd;
using UAVBarrierCmd = NLS::Render::RHI::UAVBarrierCmd;
using AliasBarrierCmd = NLS::Render::RHI::AliasBarrierCmd;
using CopyBufferCmd = NLS::Render::RHI::CopyBufferCmd;
using CopyBufferToTextureCmd = NLS::Render::RHI::CopyBufferToTextureCmd;
using CopyTextureCmd = NLS::Render::RHI::CopyTextureCmd;
using BarrierCmd = NLS::Render::RHI::BarrierCmd;
using BeginRenderPassCmd = NLS::Render::RHI::BeginRenderPassCmd;
using EndRenderPassCmd = NLS::Render::RHI::EndRenderPassCmd;

DX11CommandListExecutor::DX11CommandListExecutor(ID3D11Device* device, ID3D11DeviceContext* context)
    : device_(device), context_(context)
{
}

DX11CommandListExecutor::~DX11CommandListExecutor() = default;

void DX11CommandListExecutor::Reset(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    hasRecordedBarriers_ = false;
}

void DX11CommandListExecutor::BeginRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    hasRecordedBarriers_ = false;
}

void DX11CommandListExecutor::EndRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    // DX11 command lists are recorded to the immediate context directly
    // No explicit close needed - commands are executed immediately or deferred
}

void DX11CommandListExecutor::Execute(RHICommandList* cmdList)
{
    if (!cmdList)
        return;

    currentCommandList_ = cmdList;

    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

    // Execute commands in order - DX11 uses immediate mode rendering
    for (const auto& cmd : commands)
    {
        if (!cmd)
            continue;

        switch (cmd->type)
        {
        case Command::Type::BeginRenderPass:
            ExecuteRenderPass(cmdList);
            break;

        case Command::Type::EndRenderPass:
            // DX11 render pass ends when next pass begins or on explicit End
            break;

        case Command::Type::SetViewport:
            if (context_)
            {
                auto* viewportCmd = static_cast<SetViewportCmd*>(cmd.get());
                D3D11_VIEWPORT dxViewport = {};
                dxViewport.Width = viewportCmd->viewport.width;
                dxViewport.Height = viewportCmd->viewport.height;
                dxViewport.MinDepth = viewportCmd->viewport.minDepth;
                dxViewport.MaxDepth = viewportCmd->viewport.maxDepth;
                dxViewport.TopLeftX = viewportCmd->viewport.x;
                dxViewport.TopLeftY = viewportCmd->viewport.y;
                context_->RSSetViewports(1, &dxViewport);
            }
            break;

        case Command::Type::SetScissor:
            if (context_)
            {
                auto* scissorCmd = static_cast<SetScissorCmd*>(cmd.get());
                D3D11_RECT dxScissor = {};
                dxScissor.left = static_cast<LONG>(scissorCmd->rect.x);
                dxScissor.top = static_cast<LONG>(scissorCmd->rect.y);
                dxScissor.right = static_cast<LONG>(scissorCmd->rect.x + scissorCmd->rect.width);
                dxScissor.bottom = static_cast<LONG>(scissorCmd->rect.y + scissorCmd->rect.height);
                context_->RSSetScissorRects(1, &dxScissor);
            }
            break;

        case Command::Type::BindGraphicsPipeline:
            // Pipeline binding is handled at draw time through Material/DrawState
            break;

        case Command::Type::BindComputePipeline:
            // Compute binding handled at dispatch time
            break;

        case Command::Type::BindBindingSet:
            // Binding set binding is handled at draw/dispatch time
            break;

        case Command::Type::PushConstants:
            // Push constants handled through binding set or pipeline
            break;

        case Command::Type::BindVertexBuffer:
            if (context_)
            {
                auto* vbCmd = static_cast<BindVertexBufferCmd*>(cmd.get());
                ID3D11Buffer* dx11Buffer = nullptr;
                if (vbCmd->view.buffer)
                {
                    auto nativeHandle = vbCmd->view.buffer->GetNativeBufferHandle();
                    if (nativeHandle.handle != nullptr)
                    {
                        dx11Buffer = reinterpret_cast<ID3D11Buffer*>(nativeHandle.handle);
                    }
                }
                ID3D11Buffer* buffers[] = { dx11Buffer };
                UINT strides[] = { vbCmd->view.stride };
                UINT offsets[] = { static_cast<UINT>(vbCmd->view.offset) };
                context_->IASetVertexBuffers(vbCmd->slot, 1, buffers, strides, offsets);
            }
            break;

        case Command::Type::BindIndexBuffer:
            if (context_)
            {
                auto* ibCmd = static_cast<BindIndexBufferCmd*>(cmd.get());
                ID3D11Buffer* dx11Buffer = nullptr;
                if (ibCmd->view.buffer)
                {
                    auto nativeHandle = ibCmd->view.buffer->GetNativeBufferHandle();
                    if (nativeHandle.handle != nullptr)
                    {
                        dx11Buffer = reinterpret_cast<ID3D11Buffer*>(nativeHandle.handle);
                    }
                }
                DXGI_FORMAT format = (ibCmd->view.indexType == IndexType::UInt16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
                context_->IASetIndexBuffer(dx11Buffer, format, static_cast<UINT>(ibCmd->view.offset));
            }
            break;

        case Command::Type::Draw:
            if (context_)
            {
                auto* drawCmd = static_cast<DrawCmd*>(cmd.get());
                context_->Draw(drawCmd->args.vertexCount, drawCmd->args.firstVertex);
            }
            break;

        case Command::Type::DrawIndexed:
            if (context_)
            {
                auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get());
                context_->DrawIndexed(drawIndexedCmd->args.indexCount,
                                     drawIndexedCmd->args.firstIndex,
                                     drawIndexedCmd->args.vertexOffset);
            }
            break;

        case Command::Type::DrawInstanced:
            if (context_)
            {
                auto* drawInstancedCmd = static_cast<DrawInstancedCmd*>(cmd.get());
                context_->DrawInstanced(drawInstancedCmd->args.vertexCount,
                                       drawInstancedCmd->args.instanceCount,
                                       drawInstancedCmd->args.firstVertex,
                                       drawInstancedCmd->args.firstInstance);
            }
            break;

        case Command::Type::DrawIndexedInstanced:
            if (context_)
            {
                auto* drawIndexedInstancedCmd = static_cast<DrawIndexedInstancedCmd*>(cmd.get());
                context_->DrawIndexedInstanced(drawIndexedInstancedCmd->args.indexCount,
                                              drawIndexedInstancedCmd->args.instanceCount,
                                              drawIndexedInstancedCmd->args.firstIndex,
                                              drawIndexedInstancedCmd->args.vertexOffset,
                                              drawIndexedInstancedCmd->args.firstInstance);
            }
            break;

        case Command::Type::Dispatch:
            if (context_)
            {
                auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get());
                context_->Dispatch(dispatchCmd->args.threadGroupX,
                                  dispatchCmd->args.threadGroupY,
                                  dispatchCmd->args.threadGroupZ);
            }
            break;

        case Command::Type::DispatchIndirect:
            // DX11 does not support ExecuteIndirect for compute
            // This would need software fallback or be unsupported
            break;

        case Command::Type::SetStencilRef:
            // DX11: Stencil ref is set via OMSetDepthStencilState
            // For simplicity, we use the default depth stencil state with the ref value
            // A more complete implementation would cache/create depth stencil states as needed
            if (context_)
            {
                auto* stencilCmd = static_cast<SetStencilRefCmd*>(cmd.get());
                // DX11 doesn't have direct OMSetStencilRef - this is a no-op for now
                // Proper implementation would create a depthstencilstate with the ref
                (void)stencilCmd;
            }
            break;

        case Command::Type::SetBlendFactor:
            if (context_)
            {
                auto* blendCmd = static_cast<SetBlendFactorCmd*>(cmd.get());
                context_->OMSetBlendState(nullptr, blendCmd->blendFactor, 0xFFFFFFFF);
            }
            break;

        case Command::Type::Barrier:
        {
            // DX11 does NOT support explicit barriers - record for compatibility only
            auto* barrierCmd = static_cast<BarrierCmd*>(cmd.get());
            RecordBarrierState(barrierCmd->barrier);
            // No-op: DX11 uses implicit synchronization
            break;
        }

        case Command::Type::UAVBarrier:
        {
            // DX11 does NOT support explicit UAV barriers
            auto* uavBarrierCmd = static_cast<UAVBarrierCmd*>(cmd.get());
            RecordBarrierState(BarrierDesc{}); // Record that a barrier occurred
            // No-op: DX11 GPU ensures ordering between draw/dispatch calls
            (void)uavBarrierCmd;
            break;
        }

        case Command::Type::AliasBarrier:
        {
            // DX11 does NOT support explicit alias barriers
            auto* aliasBarrierCmd = static_cast<AliasBarrierCmd*>(cmd.get());
            RecordBarrierState(BarrierDesc{});
            // No-op: DX11 resource transitions are implicit
            (void)aliasBarrierCmd;
            break;
        }

        case Command::Type::CopyBuffer:
            // DX11 uses implicit copy through resource updates
            // For formal RHI compatibility, we execute copy via context
            break;

        case Command::Type::CopyBufferToTexture:
            // Would need staging resources - DX11 immediate context copy
            break;

        case Command::Type::CopyTexture:
            // Would need staging resources - DX11 immediate context copy
            break;

        default:
            break;
        }
    }
}

void DX11CommandListExecutor::ExecuteDrawCommands(RHICommandList* cmdList)
{
    if (!cmdList)
        return;

    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

    for (const auto& cmd : commands)
    {
        if (!cmd)
            continue;

        switch (cmd->type)
        {
        case Command::Type::Draw:
            if (context_)
            {
                auto* drawCmd = static_cast<DrawCmd*>(cmd.get());
                context_->Draw(drawCmd->args.vertexCount, drawCmd->args.firstVertex);
            }
            break;
        case Command::Type::DrawIndexed:
            if (context_)
            {
                auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get());
                context_->DrawIndexed(drawIndexedCmd->args.indexCount,
                                     drawIndexedCmd->args.firstIndex,
                                     drawIndexedCmd->args.vertexOffset);
            }
            break;
        default:
            break;
        }
    }
}

void DX11CommandListExecutor::ExecuteComputeCommands(RHICommandList* cmdList)
{
    if (!cmdList)
        return;

    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

    for (const auto& cmd : commands)
    {
        if (!cmd)
            continue;

        switch (cmd->type)
        {
        case Command::Type::Dispatch:
            if (context_)
            {
                auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get());
                context_->Dispatch(dispatchCmd->args.threadGroupX,
                                  dispatchCmd->args.threadGroupY,
                                  dispatchCmd->args.threadGroupZ);
            }
            break;
        default:
            break;
        }
    }
}

void DX11CommandListExecutor::ExecuteCopyCommands(RHICommandList* cmdList)
{
    if (!cmdList)
        return;

    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

    for (const auto& cmd : commands)
    {
        if (!cmd)
            continue;

        // DX11 copy operations - in a full implementation these would use
        // staging resources and CopySubresourceRegion
        switch (cmd->type)
        {
        case Command::Type::CopyBuffer:
        case Command::Type::CopyBufferToTexture:
        case Command::Type::CopyTexture:
            // DX11 immediate context copy would go here
            break;
        default:
            break;
        }
    }
}

void DX11CommandListExecutor::ExecuteRenderPass(RHICommandList* cmdList)
{
    if (!cmdList || !context_)
        return;

    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

    for (const auto& cmd : commands)
    {
        if (!cmd)
            continue;

        if (cmd->type == Command::Type::BeginRenderPass)
        {
            auto* beginPassCmd = static_cast<BeginRenderPassCmd*>(cmd.get());
            const auto& desc = beginPassCmd->desc;

            // Set render targets from render pass description
            // Note: In DX11, we would get the RTV/DSV handles from the attachments
            // and call OMSetRenderTargets

            // For now, just iterate through attachments to verify the pass structure
            for (const auto& colorAtt : desc.colorAttachments)
            {
                (void)colorAtt;
                // Would bind RTV here if we had the DX11 RTV handle
            }

            if (desc.depthStencilAttachment.has_value())
            {
                (void)desc.depthStencilAttachment.value();
                // Would bind DSV here if we had the DX11 DSV handle
            }
        }
    }
}

void DX11CommandListExecutor::RecordBarrierState(const BarrierDesc& barrier)
{
    // Record that barriers were recorded even though they won't be executed
    // This is useful for debugging and maintaining API consistency
    hasRecordedBarriers_ = true;

    // In a debug build, we could log barrier information here
    // to help diagnose synchronization issues
    (void)barrier;
}

} // namespace NLS::Render::RHI::DX11