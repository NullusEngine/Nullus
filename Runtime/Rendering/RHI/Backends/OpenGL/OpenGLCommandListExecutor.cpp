// Runtime/Rendering/RHI/Backends/OpenGL/OpenGLCommandListExecutor.cpp
#include "Rendering/RHI/Backends/OpenGL/OpenGLCommandListExecutor.h"

#include <glad/glad.h>

#include "Rendering/RHI/Core/RHICommandList.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHIEnums.h"

namespace NLS::Render::RHI::OpenGL {

OpenGLCommandListExecutor::OpenGLCommandListExecutor()
{
}

OpenGLCommandListExecutor::~OpenGLCommandListExecutor() = default;

void OpenGLCommandListExecutor::Reset(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    hasRecordedBarriers_ = false;
}

void OpenGLCommandListExecutor::BeginRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    hasRecordedBarriers_ = false;
}

void OpenGLCommandListExecutor::EndRecording(RHICommandList* cmdList)
{
    currentCommandList_ = cmdList;
    // OpenGL command lists are recorded to the immediate context directly
    // No explicit close needed - commands are executed immediately or deferred
}

void OpenGLCommandListExecutor::Execute(RHICommandList* cmdList)
{
    if (!cmdList)
        return;

    currentCommandList_ = cmdList;

    auto* defaultCmdList = static_cast<DefaultRHICommandList*>(cmdList);
    const auto& commands = defaultCmdList->GetCommands();

    // Execute commands in order - OpenGL uses immediate mode rendering
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
            // OpenGL render pass ends when next pass begins or on explicit End
            break;

        case Command::Type::SetViewport:
        {
            auto* viewportCmd = static_cast<SetViewportCmd*>(cmd.get());
            glViewport(static_cast<GLint>(viewportCmd->viewport.x),
                       static_cast<GLint>(viewportCmd->viewport.y),
                       static_cast<GLsizei>(viewportCmd->viewport.width),
                       static_cast<GLsizei>(viewportCmd->viewport.height));
            break;
        }

        case Command::Type::SetScissor:
        {
            auto* scissorCmd = static_cast<SetScissorCmd*>(cmd.get());
            glScissor(static_cast<GLint>(scissorCmd->rect.x),
                      static_cast<GLint>(scissorCmd->rect.y),
                      static_cast<GLsizei>(scissorCmd->rect.width),
                      static_cast<GLsizei>(scissorCmd->rect.height));
            break;
        }

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
        {
            auto* vbCmd = static_cast<BindVertexBufferCmd*>(cmd.get());
            GLuint bufferId = 0;
            if (vbCmd->view.buffer)
            {
                auto nativeHandle = vbCmd->view.buffer->GetNativeBufferHandle();
                if (nativeHandle.backend == BackendType::OpenGL && nativeHandle.handle != nullptr)
                {
                    bufferId = static_cast<GLuint>(reinterpret_cast<uintptr_t>(nativeHandle.handle));
                }
            }
            glBindBuffer(GL_ARRAY_BUFFER, bufferId);
            break;
        }

        case Command::Type::BindIndexBuffer:
        {
            auto* ibCmd = static_cast<BindIndexBufferCmd*>(cmd.get());
            GLuint bufferId = 0;
            if (ibCmd->view.buffer)
            {
                auto nativeHandle = ibCmd->view.buffer->GetNativeBufferHandle();
                if (nativeHandle.backend == BackendType::OpenGL && nativeHandle.handle != nullptr)
                {
                    bufferId = static_cast<GLuint>(reinterpret_cast<uintptr_t>(nativeHandle.handle));
                }
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, bufferId);
            break;
        }

        case Command::Type::Draw:
        {
            auto* drawCmd = static_cast<DrawCmd*>(cmd.get());
            if (drawCmd->args.instanceCount > 1)
            {
                glDrawArraysInstanced(GL_TRIANGLES,
                                      static_cast<GLint>(drawCmd->args.firstVertex),
                                      static_cast<GLsizei>(drawCmd->args.vertexCount),
                                      static_cast<GLsizei>(drawCmd->args.instanceCount));
            }
            else
            {
                glDrawArrays(GL_TRIANGLES,
                             static_cast<GLint>(drawCmd->args.firstVertex),
                             static_cast<GLsizei>(drawCmd->args.vertexCount));
            }
            break;
        }

        case Command::Type::DrawIndexed:
        {
            auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get());
            GLenum indexType = (drawIndexedCmd->args.firstIndex != 0) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
            // Use the indexType from the index buffer view if available
            // For now, default to unsigned int
            indexType = GL_UNSIGNED_INT;

            if (drawIndexedCmd->args.instanceCount > 1)
            {
                // glDrawElementsInstanced requires 5 params: mode, count, type, indices, primcount
                glDrawElementsInstanced(GL_TRIANGLES,
                                        static_cast<GLsizei>(drawIndexedCmd->args.indexCount),
                                        indexType,
                                        nullptr,
                                        static_cast<GLsizei>(drawIndexedCmd->args.instanceCount));
            }
            else
            {
                glDrawElements(GL_TRIANGLES,
                               static_cast<GLsizei>(drawIndexedCmd->args.indexCount),
                               indexType,
                               nullptr);
            }
            break;
        }

        case Command::Type::DrawInstanced:
        {
            auto* drawInstancedCmd = static_cast<DrawInstancedCmd*>(cmd.get());
            glDrawArraysInstanced(GL_TRIANGLES,
                                  static_cast<GLint>(drawInstancedCmd->args.firstVertex),
                                  static_cast<GLsizei>(drawInstancedCmd->args.vertexCount),
                                  static_cast<GLsizei>(drawInstancedCmd->args.instanceCount));
            break;
        }

        case Command::Type::DrawIndexedInstanced:
        {
            auto* drawIndexedInstancedCmd = static_cast<DrawIndexedInstancedCmd*>(cmd.get());
            GLenum indexType = GL_UNSIGNED_INT;
            glDrawElementsInstanced(GL_TRIANGLES,
                                    static_cast<GLsizei>(drawIndexedInstancedCmd->args.indexCount),
                                    indexType,
                                    nullptr,
                                    static_cast<GLsizei>(drawIndexedInstancedCmd->args.instanceCount));
            break;
        }

        case Command::Type::Dispatch:
        {
            auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get());
            glDispatchCompute(dispatchCmd->args.threadGroupX,
                              dispatchCmd->args.threadGroupY,
                              dispatchCmd->args.threadGroupZ);
            break;
        }

        case Command::Type::DispatchIndirect:
        {
            // OpenGL supports GL_DRAW_INDIRECT_BUFFER
            // Would need to bind the indirect buffer and call glDispatchComputeIndirect
            break;
        }

        case Command::Type::SetStencilRef:
        {
            auto* stencilCmd = static_cast<SetStencilRefCmd*>(cmd.get());
            glStencilFunc(GL_ALWAYS, static_cast<GLint>(stencilCmd->stencilRef), 0xFFFFFFFF);
            break;
        }

        case Command::Type::SetBlendFactor:
        {
            auto* blendCmd = static_cast<SetBlendFactorCmd*>(cmd.get());
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBlendColor(blendCmd->blendFactor[0], blendCmd->blendFactor[1],
                         blendCmd->blendFactor[2], blendCmd->blendFactor[3]);
            break;
        }

        case Command::Type::Barrier:
        {
            // OpenGL does NOT support explicit barriers - record for compatibility only
            auto* barrierCmd = static_cast<BarrierCmd*>(cmd.get());
            RecordBarrierState(barrierCmd->barrier);
            // No-op: OpenGL uses implicit synchronization
            break;
        }

        case Command::Type::UAVBarrier:
        {
            // OpenGL does NOT support explicit UAV barriers
            RecordBarrierState(BarrierDesc{});
            // No-op: OpenGL GPU ensures ordering between draw/dispatch calls
            break;
        }

        case Command::Type::AliasBarrier:
        {
            // OpenGL does NOT support explicit alias barriers
            RecordBarrierState(BarrierDesc{});
            // No-op: OpenGL resource transitions are implicit
            break;
        }

        case Command::Type::CopyBuffer:
        {
            // OpenGL buffer copy via glCopyBufferSubData
            // Requires binding copy read/write targets
            break;
        }

        case Command::Type::CopyBufferToTexture:
        {
            // Would need staging resources - OpenGL texture loading
            break;
        }

        case Command::Type::CopyTexture:
        {
            // Would need staging resources - OpenGL texture copy
            break;
        }

        default:
            break;
        }
    }
}

void OpenGLCommandListExecutor::ExecuteDrawCommands(RHICommandList* cmdList)
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
        {
            auto* drawCmd = static_cast<DrawCmd*>(cmd.get());
            glDrawArrays(GL_TRIANGLES,
                         static_cast<GLint>(drawCmd->args.firstVertex),
                         static_cast<GLsizei>(drawCmd->args.vertexCount));
            break;
        }
        case Command::Type::DrawIndexed:
        {
            auto* drawIndexedCmd = static_cast<DrawIndexedCmd*>(cmd.get());
            glDrawElements(GL_TRIANGLES,
                           static_cast<GLsizei>(drawIndexedCmd->args.indexCount),
                           GL_UNSIGNED_INT,
                           nullptr);
            break;
        }
        default:
            break;
        }
    }
}

void OpenGLCommandListExecutor::ExecuteComputeCommands(RHICommandList* cmdList)
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
        {
            auto* dispatchCmd = static_cast<DispatchCmd*>(cmd.get());
            glDispatchCompute(dispatchCmd->args.threadGroupX,
                              dispatchCmd->args.threadGroupY,
                              dispatchCmd->args.threadGroupZ);
            break;
        }
        default:
            break;
        }
    }
}

void OpenGLCommandListExecutor::ExecuteCopyCommands(RHICommandList* cmdList)
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
        {
            // OpenGL copy via glCopyBufferSubData
            // Would need staging resources
            break;
        }
        case Command::Type::CopyBufferToTexture:
        {
            // Would use glTexSubImage2D or similar
            break;
        }
        case Command::Type::CopyTexture:
        {
            // OpenGL texture copy would go here
            break;
        }
        default:
            break;
        }
    }
}

void OpenGLCommandListExecutor::ExecuteRenderPass(RHICommandList* cmdList)
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
            auto* beginPassCmd = static_cast<BeginRenderPassCmd*>(cmd.get());
            const auto& desc = beginPassCmd->desc;

            // Set render targets from render pass description
            // OpenGL uses framebuffer objects (FBOs)
            for (const auto& colorAtt : desc.colorAttachments)
            {
                (void)colorAtt;
                // Would bind FBO color attachment here if we had the OpenGL FBO handle
            }

            if (desc.depthStencilAttachment.has_value())
            {
                (void)desc.depthStencilAttachment.value();
                // Would bind FBO depth stencil attachment here
            }
        }
    }
}

void OpenGLCommandListExecutor::RecordBarrierState(const BarrierDesc& barrier)
{
    // Record that barriers were recorded even though they won't be executed
    // This is useful for debugging and maintaining API consistency
    hasRecordedBarriers_ = true;

    // In a debug build, we could log barrier information here
    // to help diagnose synchronization issues
    (void)barrier;
}

} // namespace NLS::Render::RHI::OpenGL