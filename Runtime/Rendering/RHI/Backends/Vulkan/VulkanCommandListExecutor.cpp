// Runtime/Rendering/RHI/Backends/Vulkan/VulkanCommandListExecutor.cpp
#include "VulkanCommandListExecutor.h"
#include "Rendering/RHI/Core/RHICommandList.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::RHI::Vulkan {

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

// Resource state to Vulkan image layout translation
static VkImageLayout ToVulkanImageLayout(NLS::Render::RHI::ResourceState state)
{
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopySrc))
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::CopyDst))
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    // Note: VertexBuffer, IndexBuffer, and UniformBuffer are buffer states, not image states
    // For these, we return UNDEFINED as buffers don't have image layouts
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::VertexBuffer))
        return VK_IMAGE_LAYOUT_UNDEFINED;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::IndexBuffer))
        return VK_IMAGE_LAYOUT_UNDEFINED;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::UniformBuffer))
        return VK_IMAGE_LAYOUT_UNDEFINED;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderRead))
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::ShaderWrite))
        return VK_IMAGE_LAYOUT_GENERAL;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::RenderTarget))
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::DepthRead))
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::DepthWrite))
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (static_cast<uint32_t>(state) & static_cast<uint32_t>(NLS::Render::RHI::ResourceState::Present))
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

// Access mask to Vulkan access flags translation
static VkAccessFlags ToVulkanAccessFlags(NLS::Render::RHI::AccessMask access)
{
    VkAccessFlags result = 0;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::CopyRead))
        result |= VK_ACCESS_TRANSFER_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::CopyWrite))
        result |= VK_ACCESS_TRANSFER_WRITE_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::VertexRead))
        result |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::IndexRead))
        result |= VK_ACCESS_INDEX_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::UniformRead))
        result |= VK_ACCESS_UNIFORM_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ShaderRead))
        result |= VK_ACCESS_SHADER_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ShaderWrite))
        result |= VK_ACCESS_SHADER_WRITE_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ColorAttachmentRead))
        result |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::ColorAttachmentWrite))
        result |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::DepthStencilRead))
        result |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::DepthStencilWrite))
        result |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::HostRead))
        result |= VK_ACCESS_HOST_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::HostWrite))
        result |= VK_ACCESS_HOST_WRITE_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::MemoryRead))
        result |= VK_ACCESS_MEMORY_READ_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::MemoryWrite))
        result |= VK_ACCESS_MEMORY_WRITE_BIT;
    if (static_cast<uint32_t>(access) & static_cast<uint32_t>(NLS::Render::RHI::AccessMask::Present))
        result |= VK_ACCESS_MEMORY_READ_BIT;
    return result;
}

// Pipeline stage mask to Vulkan pipeline stage flags translation
static VkPipelineStageFlags ToVulkanPipelineStageFlags(NLS::Render::RHI::PipelineStageMask stage)
{
    VkPipelineStageFlags result = 0;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::Copy))
        result |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::VertexInput))
        result |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::VertexShader))
        result |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::FragmentShader))
        result |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::ComputeShader))
        result |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::RenderTarget))
        result |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::DepthStencil))
        result |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::Present))
        result |= VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::Host))
        result |= VK_PIPELINE_STAGE_HOST_BIT;
    if (static_cast<uint32_t>(stage) & static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::AllGraphics))
        result |= VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    if (static_cast<uint32_t>(stage) == static_cast<uint32_t>(NLS::Render::RHI::PipelineStageMask::AllCommands))
        result |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    return result;
}

VulkanCommandListExecutor::VulkanCommandListExecutor(VkDevice device, VkCommandPool commandPool, VkQueue graphicsQueue)
    : device_(device), commandPool_(commandPool), graphicsQueue_(graphicsQueue)
{
    if (device_ && commandPool_)
    {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkResult result = vkAllocateCommandBuffers(device_, &allocInfo, &currentCmdBuffer_);
        if (result != VK_SUCCESS)
        {
            currentCmdBuffer_ = VK_NULL_HANDLE;
        }
    }
}

VulkanCommandListExecutor::~VulkanCommandListExecutor()
{
    if (device_ != VK_NULL_HANDLE)
    {
        if (currentCmdBuffer_ != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(device_, commandPool_, 1, &currentCmdBuffer_);
        }
        if (commandPool_ != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        }
    }
}

void VulkanCommandListExecutor::Reset(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    bufferBarriers_.clear();
    imageBarriers_.clear();

    if (currentCmdBuffer_ != VK_NULL_HANDLE)
    {
        vkResetCommandBuffer(currentCmdBuffer_, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    }
}

void VulkanCommandListExecutor::BeginRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    bufferBarriers_.clear();
    imageBarriers_.clear();

    if (currentCmdBuffer_ != VK_NULL_HANDLE)
    {
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(currentCmdBuffer_, &beginInfo);
    }
}

void VulkanCommandListExecutor::EndRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;

    if (currentCmdBuffer_ != VK_NULL_HANDLE)
    {
        vkEndCommandBuffer(currentCmdBuffer_);
    }
}

void VulkanCommandListExecutor::Execute(RHICommandList* cmdList)
{
    if (!cmdList || currentCmdBuffer_ == VK_NULL_HANDLE)
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
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
                vkCmdEndRenderPass(currentCmdBuffer_);
            break;
        case Command::Type::SetViewport:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* viewportCmd = static_cast<SetViewportCmd*>(cmd.get());
                VkViewport vkViewport = {};
                vkViewport.width = viewportCmd->viewport.width;
                vkViewport.height = viewportCmd->viewport.height;
                vkViewport.minDepth = viewportCmd->viewport.minDepth;
                vkViewport.maxDepth = viewportCmd->viewport.maxDepth;
                vkViewport.x = viewportCmd->viewport.x;
                vkViewport.y = viewportCmd->viewport.y;
                vkCmdSetViewport(currentCmdBuffer_, 0, 1, &vkViewport);
            }
            break;
        case Command::Type::SetScissor:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* scissorCmd = static_cast<SetScissorCmd*>(cmd.get());
                VkRect2D vkScissor = {};
                vkScissor.offset.x = scissorCmd->rect.x;
                vkScissor.offset.y = scissorCmd->rect.y;
                vkScissor.extent.width = scissorCmd->rect.width;
                vkScissor.extent.height = scissorCmd->rect.height;
                vkCmdSetScissor(currentCmdBuffer_, 0, 1, &vkScissor);
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
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* drawCmd = static_cast<DrawCmd*>(cmd.get());
                vkCmdDraw(
                    currentCmdBuffer_,
                    drawCmd->args.vertexCount,
                    drawCmd->args.instanceCount,
                    drawCmd->args.firstVertex,
                    drawCmd->args.firstInstance);
            }
            break;
        case Command::Type::DrawIndexed:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get());
                vkCmdDrawIndexed(
                    currentCmdBuffer_,
                    drawIndexedCmd->args.indexCount,
                    drawIndexedCmd->args.instanceCount,
                    drawIndexedCmd->args.firstIndex,
                    drawIndexedCmd->args.vertexOffset,
                    drawIndexedCmd->args.firstInstance);
            }
            break;
        case Command::Type::DrawInstanced:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* drawInstancedCmd = static_cast<DrawInstancedCmd*>(cmd.get());
                vkCmdDraw(
                    currentCmdBuffer_,
                    drawInstancedCmd->args.vertexCount,
                    drawInstancedCmd->args.instanceCount,
                    drawInstancedCmd->args.firstVertex,
                    drawInstancedCmd->args.firstInstance);
            }
            break;
        case Command::Type::DrawIndexedInstanced:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* drawIndexedInstancedCmd = static_cast<DrawIndexedInstancedCmd*>(cmd.get());
                vkCmdDrawIndexed(
                    currentCmdBuffer_,
                    drawIndexedInstancedCmd->args.indexCount,
                    drawIndexedInstancedCmd->args.instanceCount,
                    drawIndexedInstancedCmd->args.firstIndex,
                    drawIndexedInstancedCmd->args.vertexOffset,
                    drawIndexedInstancedCmd->args.firstInstance);
            }
            break;
        case Command::Type::Dispatch:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get());
                vkCmdDispatch(
                    currentCmdBuffer_,
                    dispatchCmd->args.threadGroupX,
                    dispatchCmd->args.threadGroupY,
                    dispatchCmd->args.threadGroupZ);
            }
            break;
        case Command::Type::DispatchIndirect:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* dispatchIndirectCmd = static_cast<DispatchIndirectCmd*>(cmd.get());
                if (dispatchIndirectCmd->indirectBuffer)
                {
                    auto nativeHandle = dispatchIndirectCmd->indirectBuffer->GetNativeBufferHandle();
                    if (nativeHandle.backend == BackendType::Vulkan)
                    {
                        VkBuffer buffer = static_cast<VkBuffer>(nativeHandle.handle);
                        vkCmdDispatchIndirect(currentCmdBuffer_, buffer, dispatchIndirectCmd->offset);
                    }
                }
            }
            break;
        case Command::Type::SetStencilRef:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* stencilCmd = static_cast<SetStencilRefCmd*>(cmd.get());
                vkCmdSetStencilReference(currentCmdBuffer_, VK_STENCIL_FACE_FRONT_AND_BACK, stencilCmd->stencilRef);
            }
            break;
        case Command::Type::SetBlendFactor:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* blendCmd = static_cast<SetBlendFactorCmd*>(cmd.get());
                vkCmdSetBlendConstants(currentCmdBuffer_, blendCmd->blendFactor);
            }
            break;
        case Command::Type::Barrier:
        {
            auto* barrierCmd = static_cast<BarrierCmd*>(cmd.get());
            TranslateBarrier(barrierCmd->barrier);
            break;
        }
        case Command::Type::UAVBarrier:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* uavBarrierCmd = static_cast<UAVBarrierCmd*>(cmd.get());
                if (uavBarrierCmd->resource)
                {
                    VkMemoryBarrier barrier = {};
                    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(
                        currentCmdBuffer_,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        0,
                        1, &barrier,
                        0, nullptr,
                        0, nullptr);
                }
            }
            break;
        case Command::Type::AliasBarrier:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                VkMemoryBarrier barrier = {};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                vkCmdPipelineBarrier(
                    currentCmdBuffer_,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    0,
                    1, &barrier,
                    0, nullptr,
                    0, nullptr);
            }
            break;
        case Command::Type::CopyBuffer:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* copyBufferCmd = static_cast<CopyBufferCmd*>(cmd.get());
                auto srcHandle = copyBufferCmd->args.source->GetNativeBufferHandle();
                auto dstHandle = copyBufferCmd->args.destination->GetNativeBufferHandle();
                if (srcHandle.backend == BackendType::Vulkan && dstHandle.backend == BackendType::Vulkan)
                {
                    VkBufferCopy region = {};
                    region.srcOffset = copyBufferCmd->args.region.srcOffset;
                    region.dstOffset = copyBufferCmd->args.region.dstOffset;
                    region.size = copyBufferCmd->args.region.size;
                    vkCmdCopyBuffer(
                        currentCmdBuffer_,
                        static_cast<VkBuffer>(srcHandle.handle),
                        static_cast<VkBuffer>(dstHandle.handle),
                        1,
                        &region);
                }
            }
            break;
        case Command::Type::CopyBufferToTexture:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* copyCmd = static_cast<CopyBufferToTextureCmd*>(cmd.get());
                auto srcHandle = copyCmd->args.desc.source->GetNativeBufferHandle();
                auto dstHandle = copyCmd->args.desc.destination->GetNativeImageHandle();
                if (srcHandle.backend == BackendType::Vulkan && dstHandle.backend == BackendType::Vulkan)
                {
                    VkBufferImageCopy region = {};
                    region.bufferOffset = copyCmd->args.desc.bufferOffset;
                    region.bufferRowLength = copyCmd->args.desc.extent.width;
                    region.bufferImageHeight = copyCmd->args.desc.extent.height;
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.mipLevel = copyCmd->args.desc.mipLevel;
                    region.imageSubresource.baseArrayLayer = copyCmd->args.desc.arrayLayer;
                    region.imageSubresource.layerCount = 1;
                    region.imageOffset.x = copyCmd->args.desc.textureOffset.x;
                    region.imageOffset.y = copyCmd->args.desc.textureOffset.y;
                    region.imageOffset.z = copyCmd->args.desc.textureOffset.z;
                    region.imageExtent.width = copyCmd->args.desc.extent.width;
                    region.imageExtent.height = copyCmd->args.desc.extent.height;
                    region.imageExtent.depth = copyCmd->args.desc.extent.depth;

                    vkCmdCopyBufferToImage(
                        currentCmdBuffer_,
                        static_cast<VkBuffer>(srcHandle.handle),
                        static_cast<VkImage>(dstHandle.handle),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1,
                        &region);
                }
            }
            break;
        case Command::Type::CopyTexture:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* copyCmd = static_cast<CopyTextureCmd*>(cmd.get());
                auto srcHandle = copyCmd->args.desc.source->GetNativeImageHandle();
                auto dstHandle = copyCmd->args.desc.destination->GetNativeImageHandle();
                if (srcHandle.backend == BackendType::Vulkan && dstHandle.backend == BackendType::Vulkan)
                {
                    VkImageCopy region = {};
                    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.srcSubresource.mipLevel = copyCmd->args.desc.sourceRange.baseMipLevel;
                    region.srcSubresource.baseArrayLayer = copyCmd->args.desc.sourceRange.baseArrayLayer;
                    region.srcSubresource.layerCount = copyCmd->args.desc.sourceRange.arrayLayerCount;
                    region.srcOffset.x = copyCmd->args.desc.sourceOffset.x;
                    region.srcOffset.y = copyCmd->args.desc.sourceOffset.y;
                    region.srcOffset.z = copyCmd->args.desc.sourceOffset.z;
                    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.dstSubresource.mipLevel = copyCmd->args.desc.destinationRange.baseMipLevel;
                    region.dstSubresource.baseArrayLayer = copyCmd->args.desc.destinationRange.baseArrayLayer;
                    region.dstSubresource.layerCount = copyCmd->args.desc.destinationRange.arrayLayerCount;
                    region.dstOffset.x = copyCmd->args.desc.destinationOffset.x;
                    region.dstOffset.y = copyCmd->args.desc.destinationOffset.y;
                    region.dstOffset.z = copyCmd->args.desc.destinationOffset.z;
                    region.extent.width = copyCmd->args.desc.extent.width;
                    region.extent.height = copyCmd->args.desc.extent.height;
                    region.extent.depth = copyCmd->args.desc.extent.depth;

                    vkCmdCopyImage(
                        currentCmdBuffer_,
                        static_cast<VkImage>(srcHandle.handle),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        static_cast<VkImage>(dstHandle.handle),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1,
                        &region);
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

void VulkanCommandListExecutor::ExecuteDrawCommands(RHICommandList* cmdList)
{
    if (!cmdList || currentCmdBuffer_ == VK_NULL_HANDLE)
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
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* drawCmd = static_cast<DrawCmd*>(cmd.get());
                vkCmdDraw(
                    currentCmdBuffer_,
                    drawCmd->args.vertexCount,
                    drawCmd->args.instanceCount,
                    drawCmd->args.firstVertex,
                    drawCmd->args.firstInstance);
            }
            break;
        case Command::Type::DrawIndexed:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get());
                vkCmdDrawIndexed(
                    currentCmdBuffer_,
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

void VulkanCommandListExecutor::ExecuteComputeCommands(RHICommandList* cmdList)
{
    if (!cmdList || currentCmdBuffer_ == VK_NULL_HANDLE)
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
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get());
                vkCmdDispatch(
                    currentCmdBuffer_,
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

void VulkanCommandListExecutor::ExecuteCopyCommands(RHICommandList* cmdList)
{
    if (!cmdList || currentCmdBuffer_ == VK_NULL_HANDLE)
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
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* copyBufferCmd = static_cast<CopyBufferCmd*>(cmd.get());
                auto srcHandle = copyBufferCmd->args.source->GetNativeBufferHandle();
                auto dstHandle = copyBufferCmd->args.destination->GetNativeBufferHandle();
                if (srcHandle.backend == BackendType::Vulkan && dstHandle.backend == BackendType::Vulkan)
                {
                    VkBufferCopy region = {};
                    region.srcOffset = copyBufferCmd->args.region.srcOffset;
                    region.dstOffset = copyBufferCmd->args.region.dstOffset;
                    region.size = copyBufferCmd->args.region.size;
                    vkCmdCopyBuffer(
                        currentCmdBuffer_,
                        static_cast<VkBuffer>(srcHandle.handle),
                        static_cast<VkBuffer>(dstHandle.handle),
                        1,
                        &region);
                }
            }
            break;
        case Command::Type::CopyBufferToTexture:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* copyCmd = static_cast<CopyBufferToTextureCmd*>(cmd.get());
                auto srcHandle = copyCmd->args.desc.source->GetNativeBufferHandle();
                auto dstHandle = copyCmd->args.desc.destination->GetNativeImageHandle();
                if (srcHandle.backend == BackendType::Vulkan && dstHandle.backend == BackendType::Vulkan)
                {
                    VkBufferImageCopy region = {};
                    region.bufferOffset = copyCmd->args.desc.bufferOffset;
                    region.bufferRowLength = copyCmd->args.desc.extent.width;
                    region.bufferImageHeight = copyCmd->args.desc.extent.height;
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.mipLevel = copyCmd->args.desc.mipLevel;
                    region.imageSubresource.baseArrayLayer = copyCmd->args.desc.arrayLayer;
                    region.imageSubresource.layerCount = 1;
                    region.imageOffset.x = copyCmd->args.desc.textureOffset.x;
                    region.imageOffset.y = copyCmd->args.desc.textureOffset.y;
                    region.imageOffset.z = copyCmd->args.desc.textureOffset.z;
                    region.imageExtent.width = copyCmd->args.desc.extent.width;
                    region.imageExtent.height = copyCmd->args.desc.extent.height;
                    region.imageExtent.depth = copyCmd->args.desc.extent.depth;

                    vkCmdCopyBufferToImage(
                        currentCmdBuffer_,
                        static_cast<VkBuffer>(srcHandle.handle),
                        static_cast<VkImage>(dstHandle.handle),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1,
                        &region);
                }
            }
            break;
        case Command::Type::CopyTexture:
            if (currentCmdBuffer_ != VK_NULL_HANDLE)
            {
                auto* copyCmd = static_cast<CopyTextureCmd*>(cmd.get());
                auto srcHandle = copyCmd->args.desc.source->GetNativeImageHandle();
                auto dstHandle = copyCmd->args.desc.destination->GetNativeImageHandle();
                if (srcHandle.backend == BackendType::Vulkan && dstHandle.backend == BackendType::Vulkan)
                {
                    VkImageCopy region = {};
                    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.srcSubresource.mipLevel = copyCmd->args.desc.sourceRange.baseMipLevel;
                    region.srcSubresource.baseArrayLayer = copyCmd->args.desc.sourceRange.baseArrayLayer;
                    region.srcSubresource.layerCount = copyCmd->args.desc.sourceRange.arrayLayerCount;
                    region.srcOffset.x = copyCmd->args.desc.sourceOffset.x;
                    region.srcOffset.y = copyCmd->args.desc.sourceOffset.y;
                    region.srcOffset.z = copyCmd->args.desc.sourceOffset.z;
                    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.dstSubresource.mipLevel = copyCmd->args.desc.destinationRange.baseMipLevel;
                    region.dstSubresource.baseArrayLayer = copyCmd->args.desc.destinationRange.baseArrayLayer;
                    region.dstSubresource.layerCount = copyCmd->args.desc.destinationRange.arrayLayerCount;
                    region.dstOffset.x = copyCmd->args.desc.destinationOffset.x;
                    region.dstOffset.y = copyCmd->args.desc.destinationOffset.y;
                    region.dstOffset.z = copyCmd->args.desc.destinationOffset.z;
                    region.extent.width = copyCmd->args.desc.extent.width;
                    region.extent.height = copyCmd->args.desc.extent.height;
                    region.extent.depth = copyCmd->args.desc.extent.depth;

                    vkCmdCopyImage(
                        currentCmdBuffer_,
                        static_cast<VkImage>(srcHandle.handle),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        static_cast<VkImage>(dstHandle.handle),
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1,
                        &region);
                }
            }
            break;
        default:
            break;
        }
    }
}

void VulkanCommandListExecutor::ExecuteBarriers(RHICommandList* cmdList)
{
    (void)cmdList;

    if (currentCmdBuffer_ == VK_NULL_HANDLE)
        return;

    // 执行image barriers
    if (!imageBarriers_.empty())
    {
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        vkCmdPipelineBarrier(
            currentCmdBuffer_,
            srcStage,
            dstStage,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<uint32_t>(imageBarriers_.size()),
            imageBarriers_.data());
        imageBarriers_.clear();
    }

    // 执行buffer barriers
    if (!bufferBarriers_.empty())
    {
        VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        vkCmdPipelineBarrier(
            currentCmdBuffer_,
            srcStage,
            dstStage,
            0,
            0,
            nullptr,
            static_cast<uint32_t>(bufferBarriers_.size()),
            bufferBarriers_.data(),
            0,
            nullptr);
        bufferBarriers_.clear();
    }
}

void VulkanCommandListExecutor::ExecuteRenderPass(RHICommandList* cmdList)
{
    if (!cmdList || currentCmdBuffer_ == VK_NULL_HANDLE)
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

            // Note: Vulkan render pass handling would require creating VkRenderPass and VkFramebuffer
            // For now, this is a placeholder for the actual render pass implementation
            // Real implementation would need to:
            // 1. Create or retrieve VkRenderPass based on desc
            // 2. Create VkFramebuffer with the attachment images
            // 3. Call vkCmdBeginRenderPass with the framebuffer
            (void)desc;
        }
    }
}

void VulkanCommandListExecutor::TranslateBarrier(const BarrierDesc& barrier)
{
    // 转换buffer barriers
    for (const auto& bb : barrier.bufferBarriers)
    {
        VkBufferMemoryBarrier vkBarrier = {};
        vkBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        auto bbHandle = bb.buffer->GetNativeBufferHandle();
        vkBarrier.buffer = (bbHandle.backend == BackendType::Vulkan)
                              ? static_cast<VkBuffer>(bbHandle.handle)
                              : VK_NULL_HANDLE;
        vkBarrier.srcAccessMask = ToVulkanAccessFlags(bb.sourceAccessMask);
        vkBarrier.dstAccessMask = ToVulkanAccessFlags(bb.destinationAccessMask);
        vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.offset = 0;
        vkBarrier.size = VK_WHOLE_SIZE;
        bufferBarriers_.push_back(vkBarrier);
    }

    // 转换texture barriers
    for (const auto& tb : barrier.textureBarriers)
    {
        VkImageMemoryBarrier vkBarrier = {};
        vkBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        auto tbHandle = tb.texture->GetNativeImageHandle();
        vkBarrier.image = (tbHandle.backend == BackendType::Vulkan)
                              ? static_cast<VkImage>(tbHandle.handle)
                              : VK_NULL_HANDLE;
        vkBarrier.srcAccessMask = ToVulkanAccessFlags(tb.sourceAccessMask);
        vkBarrier.dstAccessMask = ToVulkanAccessFlags(tb.destinationAccessMask);
        vkBarrier.oldLayout = ToVulkanImageLayout(tb.before);
        vkBarrier.newLayout = ToVulkanImageLayout(tb.after);
        vkBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vkBarrier.subresourceRange.baseMipLevel = tb.subresourceRange.baseMipLevel;
        vkBarrier.subresourceRange.levelCount = tb.subresourceRange.mipLevelCount;
        vkBarrier.subresourceRange.baseArrayLayer = tb.subresourceRange.baseArrayLayer;
        vkBarrier.subresourceRange.layerCount = tb.subresourceRange.arrayLayerCount;
        imageBarriers_.push_back(vkBarrier);
    }
}

} // namespace NLS::Render::RHI::Vulkan
