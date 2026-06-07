#pragma once

#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderForward.h"

namespace NLS::Engine::Rendering
{
    using ForwardScenePassPipelineKind = NLS::Render::FrameGraph::ForwardScenePassPipelineKind;
    using ForwardScenePassExecutionKind = NLS::Render::FrameGraph::ForwardScenePassExecutionKind;
    using DeferredScenePassExecutionKind = NLS::Render::FrameGraph::DeferredScenePassExecutionKind;

    inline ForwardScenePassExecutionKind GetForwardScenePassExecutionKind(
        const NLS::Render::Context::RenderPassCommandKind kind)
    {
        return NLS::Render::FrameGraph::GetForwardScenePassExecutionKind(kind);
    }

    inline ForwardScenePassPipelineKind GetForwardScenePassPipelineKind(
        const NLS::Render::Context::RenderPassCommandKind kind)
    {
        return NLS::Render::FrameGraph::GetForwardScenePassPipelineKind(kind);
    }

    inline DeferredScenePassExecutionKind GetDeferredScenePassExecutionKind(
        const NLS::Render::Context::RenderPassCommandKind kind)
    {
        return NLS::Render::FrameGraph::GetDeferredScenePassExecutionKind(kind);
    }
}
