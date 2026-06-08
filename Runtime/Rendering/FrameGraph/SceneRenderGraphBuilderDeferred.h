#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "Rendering/Buffers/MultiFramebuffer.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderLightGrid.h"
#include "Rendering/Resources/Texture2D.h"

namespace NLS::Render::RHI
{
    class RHIBindingSet;
    class RHIBuffer;
    class RHIComputePipeline;
}

namespace NLS::Render::FrameGraph
{
    using DeferredScenePassDescriptor = ThreadedRenderSceneGraphPassDescriptor<>;

    inline constexpr size_t kDeferredGBufferColorAttachmentCount = 3u;
    inline constexpr size_t kDeferredGBufferDepthTextureIndex = kDeferredGBufferColorAttachmentCount;
    inline constexpr size_t kDeferredGBufferTextureCount = kDeferredGBufferColorAttachmentCount + 1u;
    inline constexpr NLS::Render::RHI::TextureFormat kDeferredGBufferDepthFormat =
        NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    inline constexpr NLS::Render::RHI::TextureUsageFlags kDeferredGBufferColorUsage =
        NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;
    inline constexpr NLS::Render::RHI::TextureUsageFlags kDeferredGBufferDepthUsage =
        NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment |
        NLS::Render::RHI::TextureUsageFlags::Sampled;

    enum class DeferredGBufferColorSlotRole : uint8_t
    {
        Albedo = 0,
        Normal,
        Material
    };

    struct DeferredGBufferColorSlotDesc
    {
        DeferredGBufferColorSlotRole role;
        NLS::Render::RHI::TextureFormat format;
        NLS::Render::RHI::TextureUsageFlags usage;
        const char* graphResourceName;
        const char* graphViewName;
        const char* capturedViewName;
        const char* debugName;
    };

    inline constexpr std::array<DeferredGBufferColorSlotDesc, kDeferredGBufferColorAttachmentCount> kDeferredGBufferColorSlots = {
        DeferredGBufferColorSlotDesc{
            DeferredGBufferColorSlotRole::Albedo,
            NLS::Render::RHI::TextureFormat::RGBA8,
            kDeferredGBufferColorUsage,
            "DeferredGBufferAlbedo",
            "DeferredGBufferAlbedoView",
            "GBufferAlbedoView",
            "GBufferAlbedo"
        },
        DeferredGBufferColorSlotDesc{
            DeferredGBufferColorSlotRole::Normal,
            NLS::Render::RHI::TextureFormat::RGBA8,
            kDeferredGBufferColorUsage,
            "DeferredGBufferNormal",
            "DeferredGBufferNormalView",
            "GBufferNormalView",
            "GBufferNormal"
        },
        DeferredGBufferColorSlotDesc{
            DeferredGBufferColorSlotRole::Material,
            NLS::Render::RHI::TextureFormat::RGBA8,
            kDeferredGBufferColorUsage,
            "DeferredGBufferMaterial",
            "DeferredGBufferMaterialView",
            "GBufferMaterialView",
            "GBufferMaterial"
        }
    };

    static_assert(kDeferredGBufferColorSlots.size() == kDeferredGBufferColorAttachmentCount);

    constexpr size_t FindDeferredGBufferColorSlotIndex(const DeferredGBufferColorSlotRole role)
    {
        for (size_t i = 0u; i < kDeferredGBufferColorSlots.size(); ++i)
        {
            if (kDeferredGBufferColorSlots[i].role == role)
                return i;
        }
        return kDeferredGBufferColorAttachmentCount;
    }

    inline constexpr size_t kDeferredGBufferAlbedoColorAttachmentIndex =
        FindDeferredGBufferColorSlotIndex(DeferredGBufferColorSlotRole::Albedo);
    static_assert(kDeferredGBufferAlbedoColorAttachmentIndex < kDeferredGBufferColorAttachmentCount);

    constexpr std::array<NLS::Render::RHI::TextureFormat, kDeferredGBufferColorAttachmentCount>
        BuildDeferredGBufferColorFormats()
    {
        std::array<NLS::Render::RHI::TextureFormat, kDeferredGBufferColorAttachmentCount> formats{};
        for (size_t i = 0u; i < kDeferredGBufferColorAttachmentCount; ++i)
            formats[i] = kDeferredGBufferColorSlots[i].format;
        return formats;
    }

    inline constexpr auto kDeferredGBufferColorFormats = BuildDeferredGBufferColorFormats();
    static_assert(kDeferredGBufferColorFormats.size() == kDeferredGBufferColorAttachmentCount);

    struct DeferredGBufferDepthSlotDesc
    {
        NLS::Render::RHI::TextureFormat format;
        NLS::Render::RHI::TextureUsageFlags usage;
        const char* graphResourceName;
        const char* graphViewName;
        const char* capturedViewName;
        const char* debugName;
    };

    inline constexpr DeferredGBufferDepthSlotDesc kDeferredGBufferDepthSlot{
        kDeferredGBufferDepthFormat,
        kDeferredGBufferDepthUsage,
        "DeferredGBufferDepth",
        "DeferredGBufferDepthView",
        "GBufferDepthView",
        "GBufferDepth"
    };

    enum class DeferredScenePassExecutionKind : uint8_t
    {
        Unknown = 0,
        GBuffer,
        Decal,
        Lighting,
        Transparent
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

    struct HZBFrameResourceRequest
    {
        std::shared_ptr<NLS::Render::RHI::RHITexture> opaqueDepthTexture;
        std::shared_ptr<NLS::Render::RHI::RHITexture> hzbTexture;
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> occlusionPrimitiveInputBuffer;
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> occlusionPrimitiveResultBuffer;
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> hzbBuildPipeline;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> hzbBuildBindingSet;
        std::array<uint32_t, 3u> hzbBuildGroupCounts{ 1u, 1u, 1u };
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> occlusionPipeline;
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> occlusionBindingSet;
        std::array<uint32_t, 3u> occlusionGroupCounts{ 1u, 1u, 1u };
        bool opaqueDepthEligible = false;
        uint32_t hzbMipCount = 0u;
    };

    struct DeferredGraphSceneResourceRequest
    {
        ::FrameGraph* frameGraph = nullptr;
        FrameGraphBlackboard* blackboard = nullptr;
        const NLS::Render::Data::FrameDescriptor* frameDescriptor = nullptr;
        DeferredPreparedSceneResourceRequest preparedResources;
        HZBFrameResourceRequest hzbResources;
        const char* outputColorResourceName = "DeferredOutputColor";
        const char* outputDepthResourceName = "DeferredOutputDepth";
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

    struct DeferredPreparedQueuedDrawCounts
    {
        std::optional<uint64_t> lightingDrawCount;
        std::optional<uint64_t> decalDrawCount;
        std::optional<uint64_t> transparentDrawCount;
    };

    struct DeferredSceneGraphExecutionCallbacks
    {
        std::function<bool(const RecordedRenderPassExecutionDesc&)> beginGBufferPass;
        std::function<void()> executeGBufferPass;
        std::function<void()> endGBufferPass;
        std::function<bool(const OutputRenderPassExecutionDesc&)> beginLightingPass;
        std::function<void()> executeLightingPass;
        std::function<void(bool, const OutputRenderPassExecutionDesc&)> endLightingPass;
        std::function<bool(const OutputRenderPassExecutionDesc&)> beginTransparentPass;
        std::function<void()> executeTransparentPass;
        std::function<void(bool, const OutputRenderPassExecutionDesc&)> endTransparentPass;
        std::function<bool(const RecordedRenderPassExecutionDesc&)> beginDecalPass;
        std::function<void()> executeDecalPass;
        std::function<void()> endDecalPass;
    };

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
        const LightGridCompileContext& lightGridContext,
        bool includeTransparentPass = false,
        bool includeDecalPass = false);

    NLS_RENDER_API void ExecutePreparedDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const PreparedDeferredSceneGraph& preparedGraph,
        const DeferredSceneGraphExecutionCallbacks& callbacks);

    NLS_RENDER_API PreparedComputeDispatchSource BuildHZBPreparedComputeDispatchSource(
        const HZBFrameResourceRequest& request);

    NLS_RENDER_API CompiledThreadedRenderSceneExecution CompileAndApplyPreparedDeferredLightGridSceneExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& lightGridContext,
        const DeferredPreparedSceneResources& resources,
        std::vector<NLS::Render::Context::RenderPassCommandInput>&& appendedPassInputs = {},
        const std::vector<ThreadedRenderScenePassMetadata>& appendedPassMetadata = {},
        DeferredPreparedQueuedDrawCounts queuedDrawCounts = {},
        const PreparedComputeDispatchSource& hzbSource = {});

    NLS_RENDER_API void FinalizePreparedDeferredScenePackage(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor);
}
