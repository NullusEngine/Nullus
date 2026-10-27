#pragma once

#include <fg/FrameGraph.hpp>
#include "GpuEmitter.h"
#include "SpriteRenderer.h"

namespace NLS::VFX
{
struct GpuSimulationPass
{
    FrameGraphResource attributes = -1, ids = -1, counters = -1;
    std::shared_ptr<GpuEmitter> emitter;
    std::shared_ptr<const GpuFramePacket> frame;
    // The graph and this result must survive recording; retain frame through the GPU fence.
    bool Commit() { return emitter && emitter->CommitFrame(frame); }
};

struct SpriteRenderPass
{
    FrameGraphResource color = -1, depth = -1;
    std::shared_ptr<const SpriteDrawResources> draw;
};

// Execute with the engine's FrameGraphExecutionContext. Simulation is added once per world frame;
// any number of view passes can consume its output. Recording failures abort the graph.
NLS_VFX_API std::shared_ptr<GpuSimulationPass> AddGpuSimulationPass(::FrameGraph& graph,
    std::shared_ptr<GpuEmitter> emitter, uint64_t frameId, double deltaTime);
NLS_VFX_API std::shared_ptr<SpriteRenderPass> AddSpriteRenderPass(::FrameGraph& graph,
    std::shared_ptr<GpuSimulationPass> simulation, std::shared_ptr<const SpriteRenderer> renderer,
    SpriteView view, FrameGraphResource color, FrameGraphResource depth = -1,
    Render::RHI::LoadOp load = Render::RHI::LoadOp::Load, SpriteDrawContext context = {});
}
