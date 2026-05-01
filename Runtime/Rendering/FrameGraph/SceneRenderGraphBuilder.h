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

#include "../../Engine/Rendering/LightGridPrepass.h"
#include "../../Engine/Rendering/ScenePassSchemas.h"

namespace NLS::Render::FrameGraph
{
    using ForwardScenePassDescriptor = ThreadedRenderSceneGraphPassDescriptor<>;
    using DeferredScenePassDescriptor = ThreadedRenderSceneGraphPassDescriptor<>;
    using ForwardScenePassExecutionKind = NLS::Engine::Rendering::ForwardScenePassExecutionKind;
    using ForwardScenePassPipelineKind = NLS::Engine::Rendering::ForwardScenePassPipelineKind;
    using DeferredScenePassExecutionKind = NLS::Engine::Rendering::DeferredScenePassExecutionKind;

    struct LightGridCompileContext
    {
        NLS::Render::Data::FrameDescriptor frameDescriptor{};
        std::shared_ptr<NLS::Engine::Rendering::LightGridPrepass> lightGridPrepass;
        std::optional<NLS::Engine::Rendering::LightGridPrepass::PreparedFrameInputs> preparedFrameInputs;
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

    LightGridCompileContext BuildLightGridCompileContext(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const std::shared_ptr<NLS::Engine::Rendering::LightGridPrepass>& lightGridPrepass,
        const std::optional<NLS::Engine::Rendering::LightGridPrepass::PreparedFrameInputs>& preparedFrameInputs);

    void ReserveForwardSceneGraph(
        ::FrameGraph& frameGraph,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor);

    std::array<ForwardScenePassDescriptor, 3> GetForwardScenePassDescriptors();

    PreparedForwardSceneGraph PrepareForwardSceneGraph(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const LightGridCompileContext& lightGridContext,
        const char* colorResourceName = "ForwardOutputColor",
        const char* depthResourceName = "ForwardOutputDepth");

    void ExecutePreparedForwardSceneGraph(
        ::FrameGraph& frameGraph,
        const PreparedForwardSceneGraph& preparedGraph,
        const ForwardSceneGraphExecutionCallbacks& callbacks);

    CompiledThreadedRenderSceneExecution CompileAndApplyPreparedForwardLightGridSceneExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& lightGridContext);

    void FinalizePreparedForwardScenePackage(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor);

    ForwardScenePassExecutionKind GetForwardScenePassExecutionKind(
        NLS::Render::Context::RenderPassCommandKind kind);

    ForwardScenePassPipelineKind GetForwardScenePassPipelineKind(
        NLS::Render::Context::RenderPassCommandKind kind);

    DeferredPreparedSceneResources CaptureDeferredPreparedSceneResources(
        const DeferredPreparedSceneResourceRequest& request);

    DeferredGraphSceneResourceRequest BuildDeferredGraphSceneResourceRequest(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const DeferredPreparedSceneResourceRequest& preparedResources);

    std::array<DeferredScenePassDescriptor, 2> GetDeferredScenePassDescriptors();

    void ReserveDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const DeferredGraphSceneResourceRequest& resourceRequest);

    PreparedDeferredSceneGraph PrepareDeferredSceneGraph(
        const DeferredGraphSceneResourceRequest& resourceRequest,
        const LightGridCompileContext& lightGridContext);

    void ExecutePreparedDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const PreparedDeferredSceneGraph& preparedGraph,
        const DeferredSceneGraphExecutionCallbacks& callbacks);

    CompiledThreadedRenderSceneExecution CompileAndApplyPreparedDeferredLightGridSceneExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& lightGridContext,
        const DeferredPreparedSceneResources& resources);

    void FinalizePreparedDeferredScenePackage(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor);
}
