#include "ParticleRenderGraph.h"

#include <stdexcept>
#include <Rendering/FrameGraph/FrameGraphBuffer.h>
#include <Rendering/FrameGraph/FrameGraphTexture.h>
#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>

namespace NLS::VFX
{
namespace
{
using namespace Render::FrameGraph;
Render::RHI::RHICommandBuffer& Commands(void* context)
{
    auto* execution = static_cast<FrameGraphExecutionContext*>(context);
    if (!execution || !execution->commandBuffer || !execution->device)
        throw std::runtime_error("Particle passes require an explicit FrameGraph execution context");
    return *execution->commandBuffer;
}
}

std::shared_ptr<GpuSimulationPass> AddGpuSimulationPass(::FrameGraph& graph,
    std::shared_ptr<GpuEmitter> emitter, uint64_t frameId, double deltaTime)
{
    if (!emitter || !emitter->Data().AttributeBuffer()) throw std::invalid_argument("Particle graph requires an initialized GPU emitter");
    auto result = std::make_shared<GpuSimulationPass>(); result->emitter = std::move(emitter);
    const auto import = [&](const char* name, const std::shared_ptr<Render::RHI::RHIBuffer>& buffer)
    {
        FrameGraphBuffer::Desc desc; desc.size = buffer->GetDesc().size; desc.debugName = name;
        return graph.import<FrameGraphBuffer>(name,desc,FrameGraphBuffer::WrapExternal(buffer));
    };
    result->attributes = import("VfxAttributes",result->emitter->Data().AttributeBuffer());
    result->ids = import("VfxIds",result->emitter->Data().IdBuffer());
    result->counters = import("VfxCounters",result->emitter->Data().CounterBuffer());
    graph.addCallbackPass("VfxSimulation",[&](::FrameGraph::Builder& builder, auto&)
    {
        result->attributes = builder.write(result->attributes,0);
        result->ids = builder.write(result->ids,0); result->counters = builder.write(result->counters,0);
    },[result,frameId,deltaTime](const auto&,auto& resources,void* context)
    {
        std::string error;
        result->frame = result->emitter->RecordFrame(Commands(context),frameId,deltaTime,error);
        if (!result->frame) throw std::runtime_error(error);
        // Growth and stages may publish a new physical generation. The imported
        // handles represent the emitter's logical output, rebound before view reads.
        resources.template get<FrameGraphBuffer>(result->attributes).explicitBuffer = result->frame->Data().AttributeBuffer();
        resources.template get<FrameGraphBuffer>(result->ids).explicitBuffer = result->frame->Data().IdBuffer();
    });
    return result;
}

std::shared_ptr<SpriteRenderPass> AddSpriteRenderPass(::FrameGraph& graph,
    std::shared_ptr<GpuSimulationPass> simulation, std::shared_ptr<const SpriteRenderer> renderer,
    SpriteView view, FrameGraphResource color, FrameGraphResource depth, Render::RHI::LoadOp load, SpriteDrawContext drawContext)
{
    if (!simulation || !renderer || color < 0 || !graph.isValid(color)) throw std::invalid_argument("Sprite pass requires simulation, renderer and a color target");
    if (depth >= 0 && !graph.isValid(depth)) throw std::invalid_argument("Sprite pass depth target is obsolete");
    auto result = std::make_shared<SpriteRenderPass>();
    graph.addCallbackPass("VfxSprites",[&](::FrameGraph::Builder& builder,auto&)
    {
        builder.read(simulation->attributes,0); builder.read(simulation->ids,0); builder.read(simulation->counters,FrameGraphBuffer::kIndirectRead);
        result->color = builder.write(color,0);
        if (depth >= 0) result->depth = renderer->WritesDepth() ? builder.write(depth,0) : builder.read(depth,0);
    },[result,simulation,renderer,view,depth,load,drawContext](const auto&,auto& resources,void* context) mutable
    {
        if (!simulation->frame) throw std::runtime_error("Sprite simulation pass did not produce a frame");
        const auto& target = resources.template get<FrameGraphTexture>(result->color);
        if (!target.explicitView) throw std::runtime_error("Sprite pass color view is unavailable");
        Render::RHI::RHIRenderPassDesc pass;
        Render::RHI::RHIRenderPassColorAttachmentDesc attachment;
        attachment.view = target.explicitView; attachment.loadOp = load;
        pass.colorAttachments.push_back(std::move(attachment));
        pass.renderArea = {0,0,target.explicitTexture->GetDesc().extent.width,target.explicitTexture->GetDesc().extent.height};
        pass.debugName = "VfxSprites";
        pass.attachmentsRequireExternalStateTransitions = static_cast<FrameGraphExecutionContext*>(context)->CanTrackExplicitResourceState();
        if (depth >= 0)
        {
            Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
            depthAttachment.view = resources.template get<FrameGraphTexture>(result->depth).explicitView;
            depthAttachment.depthLoadOp = Render::RHI::LoadOp::Load;
            depthAttachment.readOnlyDepthStencil = !renderer->WritesDepth(); pass.depthStencilAttachment = std::move(depthAttachment);
            if (!renderer->WritesDepth()) drawContext.sceneDepth = pass.depthStencilAttachment->view;
        }
        std::string error;
        result->draw = renderer->Record(Commands(context),simulation->frame->Data(),view,pass,error,drawContext);
        if (!result->draw) throw std::runtime_error(error);
    });
    return result;
}
}
