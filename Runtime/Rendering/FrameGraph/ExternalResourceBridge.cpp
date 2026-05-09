#include "Rendering/FrameGraph/ExternalResourceBridge.h"

#include <algorithm>

#include "Rendering/Context/DriverAccess.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"

namespace NLS::Render::FrameGraph
{
    namespace
    {
        bool ApplyExternalSceneOutputAttachmentsToPassInput(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const NLS::Render::FrameGraph::ExternalSceneOutputAttachments& attachments)
        {
            bool applied = false;
            if (passInput.usesColorAttachment &&
                attachments.colorView != nullptr &&
                passInput.colorAttachmentViews.empty())
            {
                passInput.colorAttachmentViews = { attachments.colorView };
                applied = true;
            }

            if (passInput.usesDepthStencilAttachment &&
                attachments.depthStencilView != nullptr &&
                passInput.depthStencilAttachmentView == nullptr)
            {
                passInput.depthStencilAttachmentView = attachments.depthStencilView;
                applied = true;
            }

            return applied;
        }

        void AppendUniqueTexture(
            std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>>& textures,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
        {
            if (texture == nullptr)
                return;

            const auto found = std::find(textures.begin(), textures.end(), texture);
            if (found == textures.end())
                textures.push_back(texture);
        }

        std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>> CollectExternalSceneOutputTextures(
            const NLS::Render::Context::RenderScenePackage& renderScenePackage)
        {
            std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>> textures;
            const auto isKnownExternalTexture =
                [&renderScenePackage](const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
            {
                return texture != nullptr &&
                    texture != renderScenePackage.preferredReadbackTexture &&
                    std::find(
                        renderScenePackage.extractedTextures.begin(),
                        renderScenePackage.extractedTextures.end(),
                        texture) != renderScenePackage.extractedTextures.end();
            };

            for (const auto& passInput : renderScenePackage.passCommandInputs)
            {
                for (const auto& colorView : passInput.colorAttachmentViews)
                {
                    if (colorView != nullptr && isKnownExternalTexture(colorView->GetTexture()))
                        AppendUniqueTexture(textures, colorView->GetTexture());
                }

                if (passInput.depthStencilAttachmentView != nullptr &&
                    isKnownExternalTexture(passInput.depthStencilAttachmentView->GetTexture()))
                {
                    AppendUniqueTexture(textures, passInput.depthStencilAttachmentView->GetTexture());
                }
            }

            for (const auto& texture : renderScenePackage.extractedTextures)
            {
                if (texture != renderScenePackage.preferredReadbackTexture)
                    AppendUniqueTexture(textures, texture);
            }

            return textures;
        }
    }

    FrameGraphTexture::Desc MakeSceneColorTargetDesc(uint16_t width, uint16_t height)
    {
        FrameGraphTexture::Desc desc;
        desc.extent.width = width;
        desc.extent.height = height;
        desc.extent.depth = 1u;
        desc.format = NLS::Render::RHI::TextureFormat::RGB8;
        desc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment | NLS::Render::RHI::TextureUsageFlags::Sampled;
        desc.debugName = "SceneColor";
        return desc;
    }

    FrameGraphTexture::Desc MakeSceneDepthTargetDesc(uint16_t width, uint16_t height)
    {
        FrameGraphTexture::Desc desc;
        desc.extent.width = width;
        desc.extent.height = height;
        desc.extent.depth = 1u;
        desc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
        desc.usage = NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment;
        desc.debugName = "SceneDepth";
        return desc;
    }

    bool FrameTargetsSwapchain(const NLS::Render::Data::FrameDescriptor& frame)
    {
        return ResolveExternalSceneOutputFramebuffer(frame) == nullptr &&
            frame.outputColorTexture == nullptr &&
            frame.outputDepthStencilTexture == nullptr &&
            frame.outputColorView == nullptr &&
            frame.outputDepthStencilView == nullptr;
    }

    NLS::Render::Buffers::Framebuffer* ResolveExternalSceneOutputFramebuffer(
        const NLS::Render::Data::FrameDescriptor& frame)
    {
        return frame.outputBuffer;
    }

    void SetExternalSceneOutputFramebuffer(
        NLS::Render::Data::FrameDescriptor& frame,
        NLS::Render::Buffers::Framebuffer* framebuffer)
    {
        frame.outputBuffer = framebuffer;
        frame.outputColorTexture.reset();
        frame.outputDepthStencilTexture.reset();
        frame.outputColorView.reset();
        frame.outputDepthStencilView.reset();

        if (framebuffer == nullptr)
            return;

        frame.outputColorTexture = framebuffer->GetExplicitTextureHandle();
        frame.outputDepthStencilTexture = framebuffer->GetExplicitDepthStencilTextureHandle();
        frame.outputColorView = framebuffer->GetOrCreateExplicitColorView("SceneOutputColorView");
        frame.outputDepthStencilView = framebuffer->GetOrCreateExplicitDepthStencilView("SceneOutputDepthView");
    }

    NLS::Render::Data::FrameDescriptor CaptureExternalSceneOutputSnapshot(
        const NLS::Render::Data::FrameDescriptor& frame)
    {
        auto snapshot = frame;
        auto* outputBuffer = ResolveExternalSceneOutputFramebuffer(frame);
        if (outputBuffer == nullptr)
            return snapshot;

        if (snapshot.outputColorTexture == nullptr)
            snapshot.outputColorTexture = outputBuffer->GetExplicitTextureHandle();
        if (snapshot.outputDepthStencilTexture == nullptr)
            snapshot.outputDepthStencilTexture = outputBuffer->GetExplicitDepthStencilTextureHandle();
        if (snapshot.outputColorView == nullptr)
            snapshot.outputColorView = outputBuffer->GetOrCreateExplicitColorView("SceneOutputColorView");
        if (snapshot.outputDepthStencilView == nullptr)
            snapshot.outputDepthStencilView = outputBuffer->GetOrCreateExplicitDepthStencilView("SceneOutputDepthView");

        snapshot.outputBuffer = nullptr;
        return snapshot;
    }

    void ImportSceneRenderTargets(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const NLS::Render::Data::FrameDescriptor& frame,
        const char* colorResourceName,
        const char* depthResourceName)
    {
        auto* outputBuffer = ResolveExternalSceneOutputFramebuffer(frame);
        if (outputBuffer == nullptr &&
            frame.outputColorTexture == nullptr &&
            frame.outputDepthStencilTexture == nullptr)
            return;

        SceneRenderTargetsData targets;
        const auto colorTexture =
            frame.outputColorTexture != nullptr
                ? frame.outputColorTexture
                : (outputBuffer != nullptr ? outputBuffer->GetExplicitTextureHandle() : nullptr);
        const auto depthTexture =
            frame.outputDepthStencilTexture != nullptr
                ? frame.outputDepthStencilTexture
                : (outputBuffer != nullptr ? outputBuffer->GetExplicitDepthStencilTextureHandle() : nullptr);
        const auto colorView =
            frame.outputColorView != nullptr
                ? frame.outputColorView
                : (outputBuffer != nullptr ? outputBuffer->GetOrCreateExplicitColorView("SceneOutputColorView") : nullptr);
        const auto depthView =
            frame.outputDepthStencilView != nullptr
                ? frame.outputDepthStencilView
                : (outputBuffer != nullptr ? outputBuffer->GetOrCreateExplicitDepthStencilView("SceneOutputDepthView") : nullptr);

        if (colorTexture != nullptr)
        {
            targets.color = frameGraph.import<FrameGraphTexture>(
                colorResourceName,
                MakeSceneColorTargetDesc(frame.renderWidth, frame.renderHeight),
                FrameGraphTexture::WrapExternal(
                    colorTexture,
                    colorView));
        }
        if (depthTexture != nullptr)
        {
            targets.depth = frameGraph.import<FrameGraphTexture>(
                depthResourceName,
                MakeSceneDepthTargetDesc(frame.renderWidth, frame.renderHeight),
                FrameGraphTexture::WrapExternal(
                    depthTexture,
                    depthView));
        }

        blackboard.add<SceneRenderTargetsData>(targets);
    }

    ExternalSceneOutputAttachments ResolveExternalSceneOutputAttachments(
        const NLS::Render::Data::FrameDescriptor& frame,
        const char* colorViewName,
        const char* depthViewName)
    {
        auto* outputBuffer = ResolveExternalSceneOutputFramebuffer(frame);
        if (outputBuffer == nullptr &&
            frame.outputColorView == nullptr &&
            frame.outputDepthStencilView == nullptr)
            return {};

        ExternalSceneOutputAttachments attachments;
        attachments.colorView =
            frame.outputColorView != nullptr
                ? frame.outputColorView
                : (outputBuffer != nullptr ? outputBuffer->GetOrCreateExplicitColorView(colorViewName) : nullptr);
        attachments.depthStencilView =
            frame.outputDepthStencilView != nullptr
                ? frame.outputDepthStencilView
                : (outputBuffer != nullptr ? outputBuffer->GetOrCreateExplicitDepthStencilView(depthViewName) : nullptr);
        return attachments;
    }

    SceneRenderTargetsData ResolveImportedSceneRenderTargets(const FrameGraphBlackboard& blackboard)
    {
        if (const auto* targets = blackboard.try_get<SceneRenderTargetsData>())
            return *targets;
        return {};
    }

    bool ApplyExternalSceneOutputAttachments(
        NLS::Render::Context::RenderScenePackage& package,
        const ExternalSceneOutputAttachments& attachments,
        std::initializer_list<NLS::Render::Context::RenderPassCommandKind> passKinds)
    {
        if (package.targetsSwapchain || (attachments.colorView == nullptr && attachments.depthStencilView == nullptr))
            return false;

        auto matchesPassKind = [&passKinds](NLS::Render::Context::RenderPassCommandKind kind)
        {
            for (const auto expectedKind : passKinds)
            {
                if (expectedKind == kind)
                    return true;
            }

            return false;
        };

        bool applied = false;
        for (auto& passInput : package.passCommandInputs)
        {
            if (!matchesPassKind(passInput.kind))
                continue;

            applied = ApplyExternalSceneOutputAttachmentsToPassInput(passInput, attachments) || applied;
        }

        for (auto& workUnit : package.parallelCommandWorkUnits)
        {
            if (!matchesPassKind(workUnit.commandInput.kind))
                continue;

            applied = ApplyExternalSceneOutputAttachmentsToPassInput(workUnit.commandInput, attachments) || applied;
        }

        return applied;
    }

    bool ApplyExternalSceneOutputAttachments(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frame,
        const char* colorViewName,
        const char* depthViewName,
        std::initializer_list<NLS::Render::Context::RenderPassCommandKind> passKinds)
    {
        return ApplyExternalSceneOutputAttachments(
            package,
            ResolveExternalSceneOutputAttachments(frame, colorViewName, depthViewName),
            passKinds);
    }

    bool RegisterExternalSceneOutputExtractions(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frame)
    {
        auto* outputBuffer = ResolveExternalSceneOutputFramebuffer(frame);
        if (package.targetsSwapchain ||
            (outputBuffer == nullptr &&
                frame.outputColorTexture == nullptr &&
                frame.outputDepthStencilTexture == nullptr))
            return false;

        bool registered = false;
        const auto appendIfMissing = [&package, &registered](const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
        {
            if (texture == nullptr)
                return;

            const auto found = std::find(
                package.extractedTextures.begin(),
                package.extractedTextures.end(),
                texture);
            if (found == package.extractedTextures.end())
            {
                package.extractedTextures.push_back(texture);
                registered = true;
            }
        };

        appendIfMissing(
            frame.outputColorTexture != nullptr
                ? frame.outputColorTexture
                : (outputBuffer != nullptr ? outputBuffer->GetExplicitTextureHandle() : nullptr));
        return registered;
    }

    bool RegisterPreferredReadbackTexture(
        NLS::Render::Context::RenderScenePackage& package,
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
    {
        if (texture == nullptr)
            return false;

        bool changed = package.preferredReadbackTexture != texture;
        package.preferredReadbackTexture = texture;

        const auto found = std::find(
            package.extractedTextures.begin(),
            package.extractedTextures.end(),
            texture);
        if (found == package.extractedTextures.begin())
            return changed;

        if (found != package.extractedTextures.end())
        {
            package.extractedTextures.erase(found);
            package.extractedTextures.insert(package.extractedTextures.begin(), texture);
            return true;
        }

        package.extractedTextures.insert(package.extractedTextures.begin(), texture);
        return true;
    }

    ExternalSceneOutputSummary BuildExternalSceneOutputSummary(
        const NLS::Render::Data::FrameDescriptor& frame)
    {
        ExternalSceneOutputSummary summary;
        summary.targetsSwapchain = FrameTargetsSwapchain(frame);
        summary.textureCount = CountExternalSceneOutputTextures(frame);
        summary.hasExternalOutput = !summary.targetsSwapchain && summary.textureCount > 0u;
        return summary;
    }

    ExternalSceneOutputSummary BuildExternalSceneOutputSummary(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage)
    {
        ExternalSceneOutputSummary summary;
        summary.targetsSwapchain = renderScenePackage.targetsSwapchain;
        summary.textureCount = CountExternalSceneOutputTextures(renderScenePackage);
        summary.hasExternalOutput = !summary.targetsSwapchain && summary.textureCount > 0u;
        return summary;
    }

    uint32_t CountExternalSceneOutputTextures(const NLS::Render::Data::FrameDescriptor& frame)
    {
        auto* outputBuffer = ResolveExternalSceneOutputFramebuffer(frame);
        if (outputBuffer == nullptr &&
            frame.outputColorTexture == nullptr &&
            frame.outputDepthStencilTexture == nullptr)
            return 0u;

        uint32_t textureCount = 0u;
        if ((frame.outputColorTexture != nullptr) ||
            (outputBuffer != nullptr && outputBuffer->GetExplicitTextureHandle() != nullptr))
            ++textureCount;
        if ((frame.outputDepthStencilTexture != nullptr) ||
            (outputBuffer != nullptr && outputBuffer->GetExplicitDepthStencilTextureHandle() != nullptr))
            ++textureCount;
        return textureCount;
    }

    uint32_t CountExternalSceneOutputTextures(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage)
    {
        return static_cast<uint32_t>(CollectExternalSceneOutputTextures(renderScenePackage).size());
    }

    uint32_t CountExternalSceneOutputSampledTextures(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage)
    {
        return static_cast<uint32_t>(std::count_if(
            renderScenePackage.extractedTextures.begin(),
            renderScenePackage.extractedTextures.end(),
            [&renderScenePackage](const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
            {
                return texture != nullptr &&
                    texture != renderScenePackage.preferredReadbackTexture &&
                    NLS::Render::RHI::HasTextureUsage(
                        texture->GetDesc().usage,
                        NLS::Render::RHI::TextureUsageFlags::Sampled);
            }));
    }

    bool HasExternalSceneOutput(const NLS::Render::Context::RenderScenePackage& renderScenePackage)
    {
        return BuildExternalSceneOutputSummary(renderScenePackage).hasExternalOutput;
    }

    std::shared_ptr<NLS::Render::RHI::RHITexture> ResolveFrameReadbackTexture(
        const NLS::Render::Context::RenderScenePackage* renderScenePackage,
        const NLS::Render::RHI::RHIFrameContext* frameContext)
    {
        if (renderScenePackage != nullptr)
        {
            if (renderScenePackage->preferredReadbackTexture != nullptr)
                return renderScenePackage->preferredReadbackTexture;

            for (const auto& texture : renderScenePackage->extractedTextures)
            {
                if (texture != nullptr)
                    return texture;
            }
        }

        return ResolveActiveExplicitReadbackTexture(frameContext);
    }

    std::shared_ptr<NLS::Render::RHI::RHITexture> ResolveActiveExplicitReadbackTexture(
        const NLS::Render::RHI::RHIFrameContext* frameContext)
    {
        if (frameContext == nullptr)
            return nullptr;

        if (frameContext->explicitReadbackTexture != nullptr)
            return frameContext->explicitReadbackTexture;

        if (frameContext->swapchainBackbufferView == nullptr)
            return nullptr;

        return frameContext->swapchainBackbufferView->GetTexture();
    }

    void TransitionExternalSceneOutputToShaderRead(
        NLS::Render::Context::Driver& driver,
        const NLS::Render::Data::FrameDescriptor& frame)
    {
        auto* outputBuffer = ResolveExternalSceneOutputFramebuffer(frame);
        if (outputBuffer == nullptr && frame.outputColorTexture == nullptr)
            return;

        const auto texture =
            frame.outputColorTexture != nullptr
                ? frame.outputColorTexture
                : outputBuffer->GetExplicitTextureHandle();
        if (texture == nullptr)
            return;

        auto executionContext = NLS::Render::Context::DriverRendererAccess::CreateFrameGraphExecutionContext(driver);
        if (!executionContext.CanTrackExplicitResourceState())
            return;

        NLS::Render::RHI::RHISubresourceRange fullRange;
        fullRange.baseMipLevel = 0u;
        fullRange.mipLevelCount = texture->GetDesc().mipLevels;
        fullRange.baseArrayLayer = 0u;
        fullRange.arrayLayerCount = texture->GetDesc().arrayLayers;

        NLS::Render::RHI::RHIBarrierDesc barrierDesc;
        barrierDesc.textureBarriers.push_back({
            texture,
            NLS::Render::RHI::ResourceState::Unknown,
            NLS::Render::RHI::ResourceState::ShaderRead,
            fullRange,
            NLS::Render::RHI::PipelineStageMask::AllCommands,
            NLS::Render::RHI::PipelineStageMask::AllGraphics | NLS::Render::RHI::PipelineStageMask::ComputeShader,
            NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
            NLS::Render::RHI::AccessMask::ShaderRead
        });
        executionContext.RecordResourceBarriers(barrierDesc);
    }

    NLS::Render::Context::RenderPassCommandInput BuildExtractionVisibilityPassInput(
        const NLS::Render::Context::RenderScenePackage& renderScenePackage)
    {
        NLS::Render::Context::RenderPassCommandInput passInput;
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
        passInput.debugName = "PostPassExtractionVisibility";
        passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
        passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
        passInput.requiresDependencyVisibility = !renderScenePackage.extractedTextures.empty();
        passInput.textureVisibilityTransitions.reserve(renderScenePackage.extractedTextures.size());

        for (const auto& texture : renderScenePackage.extractedTextures)
        {
            if (texture == nullptr)
                continue;

            NLS::Render::RHI::RHISubresourceRange fullRange;
            fullRange.baseMipLevel = 0u;
            fullRange.mipLevelCount = texture->GetDesc().mipLevels;
            fullRange.baseArrayLayer = 0u;
            fullRange.arrayLayerCount = texture->GetDesc().arrayLayers;
            passInput.textureVisibilityTransitions.push_back({
                texture,
                fullRange,
                NLS::Render::RHI::ResourceState::Unknown,
                NLS::Render::RHI::ResourceState::ShaderRead,
                NLS::Render::RHI::PipelineStageMask::AllGraphics,
                NLS::Render::RHI::PipelineStageMask::FragmentShader | NLS::Render::RHI::PipelineStageMask::ComputeShader,
                NLS::Render::RHI::AccessMask::ColorAttachmentWrite | NLS::Render::RHI::AccessMask::DepthStencilWrite,
                NLS::Render::RHI::AccessMask::ShaderRead
            });
        }

        return passInput;
    }
}
