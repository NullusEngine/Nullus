#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <array>
#include <vector>

#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/Resources/Texture2D.h"

namespace NLS::Render::FrameGraph
{
    using ForwardScenePassDescriptor = ThreadedRenderSceneGraphPassDescriptor<>;
    using DeferredScenePassDescriptor = ThreadedRenderSceneGraphPassDescriptor<>;

    enum class ForwardScenePassPipelineKind : uint8_t
    {
        Default = 0,
        Skybox
    };

    enum class ForwardScenePassExecutionKind : uint8_t
    {
        Unknown = 0,
        Opaque,
        Skybox,
        Transparent
    };

    enum class DeferredScenePassExecutionKind : uint8_t
    {
        Unknown = 0,
        GBuffer,
        Lighting
    };

    struct LightGridCompileContext
    {
        NLS::Render::Data::FrameDescriptor frameDescriptor{};
        PreparedComputeDispatchSource preparedComputeSource;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> graphicsPassBindingSet;
    };

    struct PreparedSceneGraphExecution
    {
        PreparedComputeDispatchSource preparedComputeSource;
        CompiledThreadedRenderSceneExecution compiledExecution;
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

    struct DeferredPreparedSceneResources
    {
        std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> gbufferColorViews;
        std::shared_ptr<NLS::Render::RHI::RHITextureView> gbufferDepthView;
        std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>> gbufferTextures;
    };

    struct DeferredPreparedSceneResourceRequest
    {
        const NLS::Render::Buffers::MultiFramebuffer* gBuffer = nullptr;
        const NLS::Render::Resources::Texture2D* gbufferAlbedoTexture = nullptr;
        const NLS::Render::Resources::Texture2D* gbufferNormalTexture = nullptr;
        const NLS::Render::Resources::Texture2D* gbufferMaterialTexture = nullptr;
        const NLS::Render::Resources::Texture2D* gbufferDepthTexture = nullptr;
    };

    struct DeferredGraphSceneResourceRequest
    {
        ::FrameGraph* frameGraph = nullptr;
        FrameGraphBlackboard* blackboard = nullptr;
        const NLS::Render::Data::FrameDescriptor* frameDescriptor = nullptr;
        DeferredPreparedSceneResourceRequest preparedResources;
        const char* outputColorResourceName = "DeferredOutputColor";
        const char* outputDepthResourceName = "DeferredOutputDepth";
        const char* gbufferAlbedoResourceName = "DeferredGBufferAlbedo";
        const char* gbufferNormalResourceName = "DeferredGBufferNormal";
        const char* gbufferMaterialResourceName = "DeferredGBufferMaterial";
        const char* gbufferDepthResourceName = "DeferredGBufferDepth";
        const char* gbufferAlbedoViewName = "DeferredGBufferAlbedoView";
        const char* gbufferNormalViewName = "DeferredGBufferNormalView";
        const char* gbufferMaterialViewName = "DeferredGBufferMaterialView";
        const char* gbufferDepthViewName = "DeferredGBufferDepthView";
    };

    struct DeferredGraphSceneResources
    {
        SceneRenderTargetsData sceneTargets;
        FrameGraphResource gbufferAlbedo = -1;
        FrameGraphResource gbufferNormal = -1;
        FrameGraphResource gbufferMaterial = -1;
        FrameGraphResource gbufferDepth = -1;
    };

    struct PreparedDeferredSceneGraph
    {
        PreparedSceneGraphExecution execution;
        DeferredGraphSceneResources resources;
    };

    struct DeferredSceneGraphExecutionCallbacks
    {
        std::function<bool(const RecordedRenderPassExecutionDesc&)> beginGBufferPass;
        std::function<void()> executeGBufferPass;
        std::function<void()> endGBufferPass;
        std::function<bool(const OutputRenderPassExecutionDesc&)> beginLightingPass;
        std::function<void()> executeLightingPass;
        std::function<void(bool, const OutputRenderPassExecutionDesc&)> endLightingPass;
    };

    NLS_RENDER_API LightGridCompileContext BuildLightGridCompileContext(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        PreparedComputeDispatchSource preparedComputeSource,
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> graphicsPassBindingSet);

    NLS_RENDER_API void ReserveForwardSceneGraph(
        ::FrameGraph& frameGraph,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor);

    NLS_RENDER_API std::array<ForwardScenePassDescriptor, 3> GetForwardScenePassDescriptors();

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

    NLS_RENDER_API DeferredScenePassExecutionKind GetDeferredScenePassExecutionKind(
        NLS::Render::Context::RenderPassCommandKind kind);

    NLS_RENDER_API DeferredPreparedSceneResources CaptureDeferredPreparedSceneResources(
        const DeferredPreparedSceneResourceRequest& request);

    NLS_RENDER_API DeferredGraphSceneResourceRequest BuildDeferredGraphSceneResourceRequest(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const DeferredPreparedSceneResourceRequest& preparedResources);

    NLS_RENDER_API std::array<DeferredScenePassDescriptor, 2> GetDeferredScenePassDescriptors();

    NLS_RENDER_API void ReserveDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const DeferredGraphSceneResourceRequest& resourceRequest);

    NLS_RENDER_API PreparedDeferredSceneGraph PrepareDeferredSceneGraph(
        const DeferredGraphSceneResourceRequest& resourceRequest,
        const LightGridCompileContext& lightGridContext);

    NLS_RENDER_API void ExecutePreparedDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const PreparedDeferredSceneGraph& preparedGraph,
        const DeferredSceneGraphExecutionCallbacks& callbacks);

    NLS_RENDER_API CompiledThreadedRenderSceneExecution CompileAndApplyPreparedDeferredLightGridSceneExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& lightGridContext,
        const DeferredPreparedSceneResources& resources);

    NLS_RENDER_API void FinalizePreparedDeferredScenePackage(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor);
}
