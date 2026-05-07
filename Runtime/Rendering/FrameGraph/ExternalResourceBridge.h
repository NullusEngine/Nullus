#pragma once

#include <fg/Blackboard.hpp>
#include <fg/FrameGraph.hpp>

#include <initializer_list>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/FrameGraph/FrameGraphTexture.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "RenderDef.h"

namespace NLS::Render::FrameGraph
{
    struct NLS_RENDER_API SceneRenderTargetsData
    {
        FrameGraphResource color = -1;
        FrameGraphResource depth = -1;
    };

    struct NLS_RENDER_API ExternalSceneOutputAttachments
    {
        std::shared_ptr<NLS::Render::RHI::RHITextureView> colorView;
        std::shared_ptr<NLS::Render::RHI::RHITextureView> depthStencilView;
    };

    struct NLS_RENDER_API ExternalSceneOutputSummary
    {
        bool targetsSwapchain = true;
        bool hasExternalOutput = false;
        uint32_t textureCount = 0u;
    };

    NLS_RENDER_API FrameGraphTexture::Desc MakeSceneColorTargetDesc(uint16_t width, uint16_t height);
    NLS_RENDER_API FrameGraphTexture::Desc MakeSceneDepthTargetDesc(uint16_t width, uint16_t height);
    NLS_RENDER_API bool FrameTargetsSwapchain(const NLS::Render::Data::FrameDescriptor& frame);
    NLS_RENDER_API NLS::Render::Buffers::Framebuffer* ResolveExternalSceneOutputFramebuffer(
        const NLS::Render::Data::FrameDescriptor& frame);
    NLS_RENDER_API void SetExternalSceneOutputFramebuffer(
        NLS::Render::Data::FrameDescriptor& frame,
        NLS::Render::Buffers::Framebuffer* framebuffer);
    NLS_RENDER_API NLS::Render::Data::FrameDescriptor CaptureExternalSceneOutputSnapshot(
        const NLS::Render::Data::FrameDescriptor& frame);

    NLS_RENDER_API void ImportSceneRenderTargets(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const NLS::Render::Data::FrameDescriptor& frame,
        const char* colorResourceName,
        const char* depthResourceName);

    NLS_RENDER_API ExternalSceneOutputAttachments ResolveExternalSceneOutputAttachments(
        const NLS::Render::Data::FrameDescriptor& frame,
        const char* colorViewName,
        const char* depthViewName);

    NLS_RENDER_API SceneRenderTargetsData ResolveImportedSceneRenderTargets(
        const FrameGraphBlackboard& blackboard);

    NLS_RENDER_API bool ApplyExternalSceneOutputAttachments(
        NLS::Render::Context::RenderScenePackage& package,
        const ExternalSceneOutputAttachments& attachments,
        std::initializer_list<NLS::Render::Context::RenderPassCommandKind> passKinds);

    NLS_RENDER_API bool ApplyExternalSceneOutputAttachments(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frame,
        const char* colorViewName,
        const char* depthViewName,
        std::initializer_list<NLS::Render::Context::RenderPassCommandKind> passKinds);

    NLS_RENDER_API bool RegisterExternalSceneOutputExtractions(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frame);
    NLS_RENDER_API bool RegisterPreferredReadbackTexture(
        NLS::Render::Context::RenderScenePackage& package,
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture);
    NLS_RENDER_API ExternalSceneOutputSummary BuildExternalSceneOutputSummary(
        const NLS::Render::Data::FrameDescriptor& frame);
    NLS_RENDER_API ExternalSceneOutputSummary BuildExternalSceneOutputSummary(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage);
    NLS_RENDER_API uint32_t CountExternalSceneOutputTextures(
        const NLS::Render::Data::FrameDescriptor& frame);
    NLS_RENDER_API uint32_t CountExternalSceneOutputTextures(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage);
    NLS_RENDER_API uint32_t CountExternalSceneOutputSampledTextures(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage);
    NLS_RENDER_API bool HasExternalSceneOutput(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage);

    NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHITexture> ResolveFrameReadbackTexture(
        const NLS::Render::Context::RenderScenePackage* renderScenePackage,
        const NLS::Render::RHI::RHIFrameContext* frameContext);

    NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHITexture> ResolveActiveExplicitReadbackTexture(
        const NLS::Render::RHI::RHIFrameContext* frameContext);
    NLS_RENDER_API void TransitionExternalSceneOutputToShaderRead(
        NLS::Render::Context::Driver& driver,
        const NLS::Render::Data::FrameDescriptor& frame);

    NLS_RENDER_API NLS::Render::Context::RenderPassCommandInput BuildExtractionVisibilityPassInput(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage);
}
