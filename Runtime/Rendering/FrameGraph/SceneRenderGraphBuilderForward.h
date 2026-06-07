#pragma once

#include <array>
#include <functional>

#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderLightGrid.h"

namespace NLS::Render::FrameGraph
{
    using ForwardScenePassDescriptor = ThreadedRenderSceneGraphPassDescriptor<>;

    enum class ForwardScenePassPipelineKind : uint8_t
    {
        Default = 0,
        Skybox
    };

    enum class ForwardScenePassExecutionKind : uint8_t
    {
        Unknown = 0,
        Opaque,
        Decal,
        Skybox,
        Transparent
    };

    struct PreparedForwardSceneGraph
    {
        PreparedSceneGraphExecution execution;
    };

    struct ForwardSceneGraphExecutionCallbacks
    {
        std::function<bool(const OutputRenderPassExecutionDesc&)> beginOutputPass;
        std::function<void(const CompiledThreadedRenderSceneGraphPass&)> executePass;
        std::function<void(bool, const OutputRenderPassExecutionDesc&)> endOutputPass;
    };

    NLS_RENDER_API void ReserveForwardSceneGraph(
        ::FrameGraph& frameGraph,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor);

    NLS_RENDER_API std::array<ForwardScenePassDescriptor, 4> GetForwardScenePassDescriptors();

    NLS_RENDER_API PreparedForwardSceneGraph PrepareForwardSceneGraph(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const LightGridCompileContext& lightGridContext,
        const char* colorResourceName = "ForwardOutputColor",
        const char* depthResourceName = "ForwardOutputDepth");

    NLS_RENDER_API void ExecutePreparedForwardSceneGraph(
        ::FrameGraph& frameGraph,
        const PreparedForwardSceneGraph& preparedGraph,
        const ForwardSceneGraphExecutionCallbacks& callbacks);

    NLS_RENDER_API CompiledThreadedRenderSceneExecution CompileAndApplyPreparedForwardLightGridSceneExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& lightGridContext);

    NLS_RENDER_API void FinalizePreparedForwardScenePackage(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor);

    NLS_RENDER_API ForwardScenePassExecutionKind GetForwardScenePassExecutionKind(
        NLS::Render::Context::RenderPassCommandKind kind);

    NLS_RENDER_API ForwardScenePassPipelineKind GetForwardScenePassPipelineKind(
        NLS::Render::Context::RenderPassCommandKind kind);
}
