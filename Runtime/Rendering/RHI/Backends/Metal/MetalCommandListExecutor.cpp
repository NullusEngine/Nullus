// Runtime/Rendering/RHI/Backends/Metal/MetalCommandListExecutor.cpp
#include "Rendering/RHI/Backends/Metal/MetalCommandListExecutor.h"

#if defined(__APPLE__)
#include <Metal/Metal.h>
#endif

#include "Rendering/RHI/Core/RHICommandList.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Debug/Logger.h"

namespace NLS::Render::RHI::Metal {

MetalCommandListExecutor::MetalCommandListExecutor()
{
}

MetalCommandListExecutor::~MetalCommandListExecutor() = default;

void MetalCommandListExecutor::Reset(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    hasRecordedBarriers_ = false;
}

void MetalCommandListExecutor::BeginRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    hasRecordedBarriers_ = false;
}

void MetalCommandListExecutor::EndRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    // Metal command lists are recorded to the command buffer directly
    // No explicit close needed - commands are executed immediately or deferred
}

void MetalCommandListExecutor::Execute(RHICommandList* cmdList)
{
    if (!cmdList)
        return;

    currentCommandList_ = cmdList;

    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

    // Execute commands in order - Metal uses explicit command buffer recording
    for (const auto& cmd : commands)
    {
        if (!cmd)
            continue;

        switch (cmd->type)
        {
        case Command::Type::SetViewport:
            // Metal viewport is set when encoding render pass
            break;
        case Command::Type::SetScissor:
            // Metal scissor is set when encoding render pass
            break;
        case Command::Type::BindGraphicsPipeline:
            // Metal pipeline binding is handled via render command encoder
            break;
        case Command::Type::BindComputePipeline:
            // Metal compute pipeline binding is handled via compute command encoder
            break;
        case Command::Type::BindBindingSet:
            // Metal binding set is handled via render/compute command encoders
            break;
        case Command::Type::PushConstants:
            // Metal push constants are handled via render command encoder
            break;
        case Command::Type::BindVertexBuffer:
            // Metal vertex buffer binding is handled via render command encoder
            break;
        case Command::Type::BindIndexBuffer:
            // Metal index buffer binding is handled via render command encoder
            break;
        case Command::Type::Draw:
        {
#if defined(__APPLE__)
            if (auto* drawCmd = static_cast<DrawCmd*>(cmd.get()))
            {
                // Metal draw calls would be encoded here using Metal command buffers
                // For now, this is a placeholder for the actual Metal implementation
                (void)drawCmd;
            }
#endif
            break;
        }
        case Command::Type::DrawIndexed:
        {
#if defined(__APPLE__)
            if (auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get()))
            {
                // Metal indexed draw calls would be encoded here using Metal command buffers
                (void)drawIndexedCmd;
            }
#endif
            break;
        }
        case Command::Type::DrawInstanced:
        {
#if defined(__APPLE__)
            if (auto* drawInstancedCmd = static_cast<DrawInstancedCmd*>(cmd.get()))
            {
                // Metal instanced draw calls would be encoded here
                (void)drawInstancedCmd;
            }
#endif
            break;
        }
        case Command::Type::DrawIndexedInstanced:
        {
#if defined(__APPLE__)
            if (auto* drawIndexedInstancedCmd = static_cast<DrawIndexedInstancedCmd*>(cmd.get()))
            {
                // Metal indexed instanced draw calls would be encoded here
                (void)drawIndexedInstancedCmd;
            }
#endif
            break;
        }
        case Command::Type::Dispatch:
        {
#if defined(__APPLE__)
            if (auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get()))
            {
                // Metal dispatch calls would be encoded here using compute command encoder
                (void)dispatchCmd;
            }
#endif
            break;
        }
        case Command::Type::DispatchIndirect:
        {
#if defined(__APPLE__)
            if (auto* dispatchIndirectCmd = static_cast<DispatchIndirectCmd*>(cmd.get()))
            {
                // Metal indirect dispatch would be encoded here
                (void)dispatchIndirectCmd;
            }
#endif
            break;
        }
        case Command::Type::SetStencilRef:
            // Metal stencil reference is set when encoding render pass
            break;
        case Command::Type::SetBlendFactor:
            // Metal blend factor is set when encoding render pass
            break;
        case Command::Type::Barrier:
        {
            auto* barrierCmd = static_cast<BarrierCmd*>(cmd.get());
            RecordBarrierState(barrierCmd->barrier);
            // Metal handles barriers implicitly through resource usage annotations
            break;
        }
        case Command::Type::UAVBarrier:
            // Metal UAV barriers are handled via fence synchronization
            break;
        case Command::Type::AliasBarrier:
            // Metal alias barriers are handled via resource aliasing
            break;
        case Command::Type::CopyBuffer:
        {
#if defined(__APPLE__)
            if (auto* copyBufferCmd = static_cast<CopyBufferCmd*>(cmd.get()))
            {
                // Metal buffer copy would be encoded via BlitCommandEncoder
                (void)copyBufferCmd;
            }
#endif
            break;
        }
        case Command::Type::CopyBufferToTexture:
        {
#if defined(__APPLE__)
            if (auto* copyCmd = static_cast<CopyBufferToTextureCmd*>(cmd.get()))
            {
                // Metal buffer to texture copy would be encoded via BlitCommandEncoder
                (void)copyCmd;
            }
#endif
            break;
        }
        case Command::Type::CopyTexture:
        {
#if defined(__APPLE__)
            if (auto* copyCmd = static_cast<CopyTextureCmd*>(cmd.get()))
            {
                // Metal texture copy would be encoded via BlitCommandEncoder
                (void)copyCmd;
            }
#endif
            break;
        }
        case Command::Type::BeginRenderPass:
        case Command::Type::EndRenderPass:
            // Metal render passes are handled differently - using render pass descriptors
            break;
        default:
            break;
        }
    }
}

void MetalCommandListExecutor::ExecuteDrawCommands(RHICommandList* cmdList)
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
        case Command::Type::DrawIndexed:
        case Command::Type::DrawInstanced:
        case Command::Type::DrawIndexedInstanced:
            // Metal draw calls would be encoded here
            break;
        default:
            break;
        }
    }
}

void MetalCommandListExecutor::ExecuteComputeCommands(RHICommandList* cmdList)
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
        case Command::Type::DispatchIndirect:
            // Metal compute calls would be encoded here
            break;
        default:
            break;
        }
    }
}

void MetalCommandListExecutor::ExecuteCopyCommands(RHICommandList* cmdList)
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
        case Command::Type::CopyBuffer:
        case Command::Type::CopyBufferToTexture:
        case Command::Type::CopyTexture:
            // Metal copy commands would be encoded here
            break;
        default:
            break;
        }
    }
}

void MetalCommandListExecutor::ExecuteRenderPass(RHICommandList* cmdList)
{
    if (!cmdList)
        return;

    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

    for (const auto& cmd : commands)
    {
        if (!cmd)
            continue;

        if (cmd->type == Command::Type::BeginRenderPass)
        {
            // Metal render passes are handled via MTLRenderPassDescriptor
            // This is a placeholder for actual implementation
        }
        else if (cmd->type == Command::Type::EndRenderPass)
        {
            // Metal render pass end is handled implicitly
        }
    }
}

void MetalCommandListExecutor::RecordBarrierState(const BarrierDesc& barrier)
{
    (void)barrier;
    hasRecordedBarriers_ = true;
    // Metal doesn't need explicit barrier recording as it uses
    // automatic synchronization through resource state tracking
}

} // namespace NLS::Render::RHI::Metal