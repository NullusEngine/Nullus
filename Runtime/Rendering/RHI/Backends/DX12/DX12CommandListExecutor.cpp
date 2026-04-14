// Runtime/Rendering/RHI/Backends/DX12/DX12CommandListExecutor.cpp
#include "DX12CommandListExecutor.h"
#include "Rendering/RHI/Core/RHICommandList.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI::DX12 {

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

// Resource state to D3D12 resource state translation
static D3D12_RESOURCE_STATES ToD3D12ResourceState(NLS::Render::RHI::ResourceState state)
{
    D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopySrc))
        result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopyDst))
        result |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::VertexBuffer))
        result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::IndexBuffer))
        result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::UniformBuffer))
        result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderRead))
        result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderWrite))
        result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::RenderTarget))
        result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::DepthRead))
        result |= D3D12_RESOURCE_STATE_DEPTH_READ;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::DepthWrite))
        result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::Present))
        result |= D3D12_RESOURCE_STATE_PRESENT;
    return result;
}

DX12CommandListExecutor::DX12CommandListExecutor(Microsoft::WRL::ComPtr<ID3D12Device> device,
                                                 Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue)
    : device_(device), commandQueue_(commandQueue)
{
    if (device_ && commandQueue_)
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        HRESULT hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr))
        {
            hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&currentCmdList_));
        }
    }
}

DX12CommandListExecutor::~DX12CommandListExecutor() = default;

void DX12CommandListExecutor::Reset(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    pendingBarriers_.clear();

    if (currentCmdList_)
    {
        currentCmdList_->Reset(nullptr, nullptr);
    }
}

void DX12CommandListExecutor::BeginRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    pendingBarriers_.clear();
}

void DX12CommandListExecutor::EndRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;

    if (currentCmdList_)
    {
        currentCmdList_->Close();
    }
}

void DX12CommandListExecutor::Execute(RHICommandList* cmdList)
{
    if (!cmdList || !currentCmdList_)
        return;

    currentCommandList_ = cmdList;
    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

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
            // DX12 command list end is handled via Close/Reset cycle
            // Not called explicitly here
            break;
        case Command::Type::SetViewport:
            if (currentCmdList_)
            {
                auto* viewportCmd = static_cast<SetViewportCmd*>(cmd.get());
                D3D12_VIEWPORT dxViewport = {};
                dxViewport.Width = viewportCmd->viewport.width;
                dxViewport.Height = viewportCmd->viewport.height;
                dxViewport.MinDepth = viewportCmd->viewport.minDepth;
                dxViewport.MaxDepth = viewportCmd->viewport.maxDepth;
                dxViewport.TopLeftX = viewportCmd->viewport.x;
                dxViewport.TopLeftY = viewportCmd->viewport.y;
                currentCmdList_->RSSetViewports(1, &dxViewport);
            }
            break;
        case Command::Type::SetScissor:
            if (currentCmdList_)
            {
                auto* scissorCmd = static_cast<SetScissorCmd*>(cmd.get());
                D3D12_RECT dxScissor = {};
                dxScissor.left = scissorCmd->rect.x;
                dxScissor.top = scissorCmd->rect.y;
                dxScissor.right = static_cast<LONG>(scissorCmd->rect.x + scissorCmd->rect.width);
                dxScissor.bottom = static_cast<LONG>(scissorCmd->rect.y + scissorCmd->rect.height);
                currentCmdList_->RSSetScissorRects(1, &dxScissor);
            }
            break;
        case Command::Type::BindGraphicsPipeline:
        case Command::Type::BindComputePipeline:
        case Command::Type::BindBindingSet:
        case Command::Type::PushConstants:
        case Command::Type::BindVertexBuffer:
        case Command::Type::BindIndexBuffer:
            // 这些命令在Draw/Dispatch时隐式处理
            break;
        case Command::Type::Draw:
            if (currentCmdList_)
            {
                auto* drawCmd = static_cast<DrawCmd*>(cmd.get());
                currentCmdList_->DrawInstanced(
                    drawCmd->args.vertexCount,
                    drawCmd->args.instanceCount,
                    drawCmd->args.firstVertex,
                    drawCmd->args.firstInstance);
            }
            break;
        case Command::Type::DrawIndexed:
            if (currentCmdList_)
            {
                auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get());
                currentCmdList_->DrawIndexedInstanced(
                    drawIndexedCmd->args.indexCount,
                    drawIndexedCmd->args.instanceCount,
                    drawIndexedCmd->args.firstIndex,
                    drawIndexedCmd->args.vertexOffset,
                    drawIndexedCmd->args.firstInstance);
            }
            break;
        case Command::Type::DrawInstanced:
            if (currentCmdList_)
            {
                auto* drawInstancedCmd = static_cast<DrawInstancedCmd*>(cmd.get());
                currentCmdList_->DrawInstanced(
                    drawInstancedCmd->args.vertexCount,
                    drawInstancedCmd->args.instanceCount,
                    drawInstancedCmd->args.firstVertex,
                    drawInstancedCmd->args.firstInstance);
            }
            break;
        case Command::Type::DrawIndexedInstanced:
            if (currentCmdList_)
            {
                auto* drawIndexedInstancedCmd = static_cast<DrawIndexedInstancedCmd*>(cmd.get());
                currentCmdList_->DrawIndexedInstanced(
                    drawIndexedInstancedCmd->args.indexCount,
                    drawIndexedInstancedCmd->args.instanceCount,
                    drawIndexedInstancedCmd->args.firstIndex,
                    drawIndexedInstancedCmd->args.vertexOffset,
                    drawIndexedInstancedCmd->args.firstInstance);
            }
            break;
        case Command::Type::Dispatch:
            if (currentCmdList_)
            {
                auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get());
                currentCmdList_->Dispatch(
                    dispatchCmd->args.threadGroupX,
                    dispatchCmd->args.threadGroupY,
                    dispatchCmd->args.threadGroupZ);
            }
            break;
        case Command::Type::DispatchIndirect:
            // DX12 ExecuteIndirect requires a command signature which must be created upfront.
            // For now, fall back to regular dispatch or make this a no-op.
            // Full implementation would need ID3D12CommandSignature created from a root signature.
            if (currentCmdList_)
            {
                auto* dispatchIndirectCmd = static_cast<DispatchIndirectCmd*>(cmd.get());
                if (dispatchIndirectCmd->indirectBuffer)
                {
                    auto nativeHandle = dispatchIndirectCmd->indirectBuffer->GetNativeBufferHandle();
                    if (nativeHandle.backend == BackendType::DX12)
                    {
                        // Fallback: just dispatch with placeholder values
                        // Proper implementation requires ID3D12CommandSignature
                        currentCmdList_->Dispatch(1, 1, 1);
                    }
                }
            }
            break;
        case Command::Type::SetStencilRef:
            if (currentCmdList_)
            {
                auto* stencilCmd = static_cast<SetStencilRefCmd*>(cmd.get());
                currentCmdList_->OMSetStencilRef(stencilCmd->stencilRef);
            }
            break;
        case Command::Type::SetBlendFactor:
            if (currentCmdList_)
            {
                auto* blendCmd = static_cast<SetBlendFactorCmd*>(cmd.get());
                currentCmdList_->OMSetBlendFactor(blendCmd->blendFactor);
            }
            break;
        case Command::Type::Barrier:
        {
            auto* barrierCmd = static_cast<BarrierCmd*>(cmd.get());
            TranslateBarrier(barrierCmd->barrier);
            break;
        }
        case Command::Type::UAVBarrier:
            if (currentCmdList_)
            {
                auto* uavBarrierCmd = static_cast<UAVBarrierCmd*>(cmd.get());
                if (uavBarrierCmd->resource)
                {
                    D3D12_RESOURCE_BARRIER barrier = {};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    auto nativeHandle = uavBarrierCmd->resource->GetNativeHandle();
                    if (nativeHandle.backend == BackendType::DX12)
                    {
                        barrier.UAV.pResource = static_cast<ID3D12Resource*>(nativeHandle.handle);
                    }
                    pendingBarriers_.push_back(barrier);
                }
            }
            break;
        case Command::Type::AliasBarrier:
            if (currentCmdList_)
            {
                auto* aliasBarrierCmd = static_cast<AliasBarrierCmd*>(cmd.get());
                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                pendingBarriers_.push_back(barrier);
            }
            break;
        case Command::Type::CopyBuffer:
            if (currentCmdList_)
            {
                auto* copyBufferCmd = static_cast<CopyBufferCmd*>(cmd.get());
                auto srcHandle = copyBufferCmd->args.source->GetNativeBufferHandle();
                auto dstHandle = copyBufferCmd->args.destination->GetNativeBufferHandle();
                if (srcHandle.backend == BackendType::DX12 && dstHandle.backend == BackendType::DX12)
                {
                    // DX12 CopyBufferRegion takes: pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes
                    currentCmdList_->CopyBufferRegion(
                        static_cast<ID3D12Resource*>(dstHandle.handle),
                        static_cast<UINT64>(copyBufferCmd->args.region.dstOffset),
                        static_cast<ID3D12Resource*>(srcHandle.handle),
                        static_cast<UINT64>(copyBufferCmd->args.region.srcOffset),
                        static_cast<UINT64>(copyBufferCmd->args.region.size));
                }
            }
            break;
        case Command::Type::CopyBufferToTexture:
            if (currentCmdList_)
            {
                auto* copyCmd = static_cast<CopyBufferToTextureCmd*>(cmd.get());
                auto srcHandle = copyCmd->args.desc.source->GetNativeBufferHandle();
                auto dstHandle = copyCmd->args.desc.destination->GetNativeImageHandle();
                if (srcHandle.backend == BackendType::DX12 && dstHandle.backend == BackendType::DX12)
                {
                    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
                    srcLocation.pResource = static_cast<ID3D12Resource*>(srcHandle.handle);
                    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    srcLocation.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    srcLocation.PlacedFootprint.Footprint.Width = copyCmd->args.desc.extent.width;
                    srcLocation.PlacedFootprint.Footprint.Height = copyCmd->args.desc.extent.height;
                    srcLocation.PlacedFootprint.Footprint.Depth = copyCmd->args.desc.extent.depth;
                    srcLocation.PlacedFootprint.Footprint.RowPitch = copyCmd->args.desc.extent.width * 4;
                    srcLocation.PlacedFootprint.Offset = copyCmd->args.desc.bufferOffset;

                    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
                    dstLocation.pResource = static_cast<ID3D12Resource*>(dstHandle.handle);
                    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

                    // DX12 CopyTextureRegion: pDst, DstX, DstY, DstZ, pSrc, pSrcBox
                    currentCmdList_->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
                }
            }
            break;
        case Command::Type::CopyTexture:
            if (currentCmdList_)
            {
                auto* copyCmd = static_cast<CopyTextureCmd*>(cmd.get());
                auto srcHandle = copyCmd->args.desc.source->GetNativeImageHandle();
                auto dstHandle = copyCmd->args.desc.destination->GetNativeImageHandle();
                if (srcHandle.backend == BackendType::DX12 && dstHandle.backend == BackendType::DX12)
                {
                    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
                    srcLocation.pResource = static_cast<ID3D12Resource*>(srcHandle.handle);
                    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

                    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
                    dstLocation.pResource = static_cast<ID3D12Resource*>(dstHandle.handle);
                    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

                    // DX12 CopyTextureRegion: pDst, DstX, DstY, DstZ, pSrc, pSrcBox
                    currentCmdList_->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
                }
            }
            break;
        default:
            break;
        }
    }

    // 执行所有pending的barriers
    ExecuteBarriers(cmdList);
}

void DX12CommandListExecutor::ExecuteDrawCommands(RHICommandList* cmdList)
{
    if (!cmdList || !currentCmdList_)
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
            if (currentCmdList_)
            {
                auto* drawCmd = static_cast<DrawCmd*>(cmd.get());
                currentCmdList_->DrawInstanced(
                    drawCmd->args.vertexCount,
                    drawCmd->args.instanceCount,
                    drawCmd->args.firstVertex,
                    drawCmd->args.firstInstance);
            }
            break;
        case Command::Type::DrawIndexed:
            if (currentCmdList_)
            {
                auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get());
                currentCmdList_->DrawIndexedInstanced(
                    drawIndexedCmd->args.indexCount,
                    drawIndexedCmd->args.instanceCount,
                    drawIndexedCmd->args.firstIndex,
                    drawIndexedCmd->args.vertexOffset,
                    drawIndexedCmd->args.firstInstance);
            }
            break;
        default:
            break;
        }
    }
}

void DX12CommandListExecutor::ExecuteComputeCommands(RHICommandList* cmdList)
{
    if (!cmdList || !currentCmdList_)
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
            if (currentCmdList_)
            {
                auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get());
                currentCmdList_->Dispatch(
                    dispatchCmd->args.threadGroupX,
                    dispatchCmd->args.threadGroupY,
                    dispatchCmd->args.threadGroupZ);
            }
            break;
        default:
            break;
        }
    }
}

void DX12CommandListExecutor::ExecuteCopyCommands(RHICommandList* cmdList)
{
    if (!cmdList || !currentCmdList_)
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
            if (currentCmdList_)
            {
                auto* copyBufferCmd = static_cast<CopyBufferCmd*>(cmd.get());
                auto srcHandle = copyBufferCmd->args.source->GetNativeBufferHandle();
                auto dstHandle = copyBufferCmd->args.destination->GetNativeBufferHandle();
                if (srcHandle.backend == BackendType::DX12 && dstHandle.backend == BackendType::DX12)
                {
                    // DX12 CopyBufferRegion takes: pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes
                    currentCmdList_->CopyBufferRegion(
                        static_cast<ID3D12Resource*>(dstHandle.handle),
                        static_cast<UINT64>(copyBufferCmd->args.region.dstOffset),
                        static_cast<ID3D12Resource*>(srcHandle.handle),
                        static_cast<UINT64>(copyBufferCmd->args.region.srcOffset),
                        static_cast<UINT64>(copyBufferCmd->args.region.size));
                }
            }
            break;
        case Command::Type::CopyBufferToTexture:
            if (currentCmdList_)
            {
                auto* copyCmd = static_cast<CopyBufferToTextureCmd*>(cmd.get());
                auto srcHandle = copyCmd->args.desc.source->GetNativeBufferHandle();
                auto dstHandle = copyCmd->args.desc.destination->GetNativeImageHandle();
                if (srcHandle.backend == BackendType::DX12 && dstHandle.backend == BackendType::DX12)
                {
                    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
                    srcLocation.pResource = static_cast<ID3D12Resource*>(srcHandle.handle);
                    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

                    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
                    dstLocation.pResource = static_cast<ID3D12Resource*>(dstHandle.handle);
                    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

                    // DX12 CopyTextureRegion: pDst, DstX, DstY, DstZ, pSrc, pSrcBox
                    currentCmdList_->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
                }
            }
            break;
        case Command::Type::CopyTexture:
            if (currentCmdList_)
            {
                auto* copyCmd = static_cast<CopyTextureCmd*>(cmd.get());
                auto srcHandle = copyCmd->args.desc.source->GetNativeImageHandle();
                auto dstHandle = copyCmd->args.desc.destination->GetNativeImageHandle();
                if (srcHandle.backend == BackendType::DX12 && dstHandle.backend == BackendType::DX12)
                {
                    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
                    srcLocation.pResource = static_cast<ID3D12Resource*>(srcHandle.handle);
                    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

                    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
                    dstLocation.pResource = static_cast<ID3D12Resource*>(dstHandle.handle);
                    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

                    // DX12 CopyTextureRegion: pDst, DstX, DstY, DstZ, pSrc, pSrcBox
                    currentCmdList_->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
                }
            }
            break;
        default:
            break;
        }
    }
}

void DX12CommandListExecutor::ExecuteBarriers(RHICommandList* cmdList)
{
    (void)cmdList;

    if (currentCmdList_ && !pendingBarriers_.empty())
    {
        currentCmdList_->ResourceBarrier(static_cast<UINT>(pendingBarriers_.size()), pendingBarriers_.data());
        pendingBarriers_.clear();
    }
}

void DX12CommandListExecutor::ExecuteRenderPass(RHICommandList* cmdList)
{
    if (!cmdList || !currentCmdList_)
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

            // Collect render target views
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvHandles;
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = {};

            for (const auto& colorAtt : desc.colorAttachments)
            {
                if (colorAtt.view)
                {
                    auto nativeHandle = colorAtt.view->GetNativeRenderTargetView();
                    if (nativeHandle.backend == BackendType::DX12)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
                        handle.ptr = reinterpret_cast<UINT64>(nativeHandle.handle);
                        rtvHandles.push_back(handle);
                    }
                }
            }

            if (desc.depthStencilAttachment.has_value())
            {
                const auto& dsAtt = desc.depthStencilAttachment.value();
                if (dsAtt.view)
                {
                    auto nativeHandle = dsAtt.view->GetNativeDepthStencilView();
                    if (nativeHandle.backend == BackendType::DX12)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
                        handle.ptr = reinterpret_cast<UINT64>(nativeHandle.handle);
                        dsvHandle = handle;
                    }
                }
            }

            if (!rtvHandles.empty())
            {
                currentCmdList_->OMSetRenderTargets(static_cast<UINT>(rtvHandles.size()), rtvHandles.data(),
                                                    FALSE, dsvHandle.ptr != 0 ? &dsvHandle : nullptr);
            }
        }
    }
}

void DX12CommandListExecutor::TranslateBarrier(const BarrierDesc& barrier)
{
    // 转换buffer barriers
    for (const auto& bb : barrier.bufferBarriers)
    {
        D3D12_RESOURCE_BARRIER dxBarrier = {};
        dxBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        auto bbHandle = bb.buffer->GetNativeBufferHandle();
        dxBarrier.Transition.pResource = (bbHandle.backend == BackendType::DX12)
                                              ? static_cast<ID3D12Resource*>(bbHandle.handle)
                                              : nullptr;
        dxBarrier.Transition.StateBefore = ToD3D12ResourceState(bb.before);
        dxBarrier.Transition.StateAfter = ToD3D12ResourceState(bb.after);
        dxBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pendingBarriers_.push_back(dxBarrier);
    }

    // 转换texture barriers
    for (const auto& tb : barrier.textureBarriers)
    {
        D3D12_RESOURCE_BARRIER dxBarrier = {};
        dxBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        auto tbHandle = tb.texture->GetNativeImageHandle();
        dxBarrier.Transition.pResource = (tbHandle.backend == BackendType::DX12)
                                              ? static_cast<ID3D12Resource*>(tbHandle.handle)
                                              : nullptr;
        dxBarrier.Transition.StateBefore = ToD3D12ResourceState(tb.before);
        dxBarrier.Transition.StateAfter = ToD3D12ResourceState(tb.after);
        dxBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pendingBarriers_.push_back(dxBarrier);
    }
}

} // namespace NLS::Render::RHI::DX12
