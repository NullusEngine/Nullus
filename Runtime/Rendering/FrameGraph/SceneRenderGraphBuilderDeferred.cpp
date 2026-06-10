#include "Rendering/FrameGraph/SceneRenderGraphBuilderDeferred.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

#include "Profiling/Profiler.h"
#include "Rendering/FrameGraph/FrameGraphExecutionPlan.h"
#include "Rendering/FrameGraph/SceneRenderGraphBuilderInternal.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"
#include "Rendering/RHI/BindingPointMap.h"

namespace NLS::Render::FrameGraph
{
    namespace
    {
        bool IsDeferredScenePassKind(const NLS::Render::Context::RenderPassCommandKind kind)
        {
            return GetDeferredScenePassExecutionKind(kind) != DeferredScenePassExecutionKind::Unknown;
        }

        struct DeferredGBufferGraphPassData
        {
            FrameGraphResource albedo = -1;
            FrameGraphResource normal = -1;
            FrameGraphResource material = -1;
            FrameGraphResource depth = -1;
        };

        struct DeferredLightingGraphPassData
        {
            FrameGraphResource albedo = -1;
            FrameGraphResource normal = -1;
            FrameGraphResource material = -1;
            FrameGraphResource depth = -1;
            FrameGraphResource outputColor = -1;
            FrameGraphResource outputDepth = -1;
        };

        struct DeferredTransparentGraphPassData
        {
            FrameGraphResource gbufferDepth = -1;
            FrameGraphResource outputColor = -1;
            FrameGraphResource outputDepth = -1;
        };

        struct DeferredScenePassDescriptorRange
        {
            std::array<DeferredScenePassDescriptor, 4u> descriptors{};
            size_t count = 0u;

            const DeferredScenePassDescriptor* begin() const
            {
                return descriptors.data();
            }

            const DeferredScenePassDescriptor* end() const
            {
                return descriptors.data() + count;
            }

            size_t size() const
            {
                return count;
            }
        };

        bool TextureWrapperMatchesHandle(
            const NLS::Render::Resources::Texture2D* texture,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& expectedHandle)
        {
            return texture != nullptr &&
                expectedHandle != nullptr &&
                texture->GetExplicitRHITextureHandle() == expectedHandle;
        }

        bool TextureDescHasRequiredUsage(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::TextureUsageFlags requiredUsage)
        {
            return (static_cast<uint32_t>(desc.usage) & static_cast<uint32_t>(requiredUsage)) ==
                static_cast<uint32_t>(requiredUsage);
        }

        bool TextureWrapperMatchesHandleFormatAndUsage(
            const NLS::Render::Resources::Texture2D* texture,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& expectedHandle,
            const NLS::Render::RHI::TextureFormat expectedFormat,
            const NLS::Render::RHI::TextureUsageFlags requiredUsage)
        {
            if (!TextureWrapperMatchesHandle(texture, expectedHandle))
                return false;

            const auto& desc = expectedHandle->GetDesc();
            return desc.format == expectedFormat &&
                TextureDescHasRequiredUsage(desc, requiredUsage);
        }

        bool TextureDescMatchesFrameDescriptor(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor)
        {
            return desc.extent.width == frameDescriptor.renderWidth &&
                desc.extent.height == frameDescriptor.renderHeight &&
                desc.extent.depth == 1u;
        }

        bool TextureWrapperMatchesHandleAndFrameDesc(
            const NLS::Render::Resources::Texture2D* texture,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& expectedHandle,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const NLS::Render::RHI::TextureFormat expectedFormat,
            const NLS::Render::RHI::TextureUsageFlags requiredUsage)
        {
            if (!TextureWrapperMatchesHandleFormatAndUsage(
                    texture,
                    expectedHandle,
                    expectedFormat,
                    requiredUsage))
            {
                return false;
            }

            const auto& desc = expectedHandle->GetDesc();
            return TextureDescMatchesFrameDescriptor(desc, frameDescriptor) &&
                texture->width == frameDescriptor.renderWidth &&
                texture->height == frameDescriptor.renderHeight;
        }

        bool HasCompleteDeferredPreparedSceneResourceRequest(
            const DeferredPreparedSceneResourceRequest& request)
        {
            if (request.gBuffer == nullptr)
                return false;

            const auto& colorHandles = request.gBuffer->GetExplicitColorTextureHandles();
            if (colorHandles.size() < kDeferredGBufferColorAttachmentCount)
                return false;

            const std::array<const NLS::Render::Resources::Texture2D*, kDeferredGBufferColorAttachmentCount> colorWrappers = {
                request.gbufferAlbedoTexture,
                request.gbufferNormalTexture,
                request.gbufferMaterialTexture
            };

            for (size_t i = 0u; i < kDeferredGBufferColorAttachmentCount; ++i)
            {
                const auto& slot = kDeferredGBufferColorSlots[i];
                if (!TextureWrapperMatchesHandleFormatAndUsage(
                        colorWrappers[i],
                        colorHandles[i],
                        slot.format,
                        slot.usage))
                {
                    return false;
                }
            }

            return TextureWrapperMatchesHandleFormatAndUsage(
                request.gbufferDepthTexture,
                request.gBuffer->GetExplicitDepthTextureHandle(),
                kDeferredGBufferDepthSlot.format,
                kDeferredGBufferDepthSlot.usage);
        }

        bool HasDeferredPreparedSceneResourceRequestMatchingFrameDesc(
            const DeferredPreparedSceneResourceRequest& request,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor)
        {
            if (request.gBuffer == nullptr)
                return false;

            const auto& colorHandles = request.gBuffer->GetExplicitColorTextureHandles();
            if (colorHandles.size() < kDeferredGBufferColorAttachmentCount)
                return false;

            const std::array<const NLS::Render::Resources::Texture2D*, kDeferredGBufferColorAttachmentCount> colorWrappers = {
                request.gbufferAlbedoTexture,
                request.gbufferNormalTexture,
                request.gbufferMaterialTexture
            };

            for (size_t i = 0u; i < kDeferredGBufferColorAttachmentCount; ++i)
            {
                const auto& slot = kDeferredGBufferColorSlots[i];
                if (!TextureWrapperMatchesHandleAndFrameDesc(
                        colorWrappers[i],
                        colorHandles[i],
                        frameDescriptor,
                        slot.format,
                        slot.usage))
                {
                    return false;
                }
            }

            return TextureWrapperMatchesHandleAndFrameDesc(
                request.gbufferDepthTexture,
                request.gBuffer->GetExplicitDepthTextureHandle(),
                frameDescriptor,
                kDeferredGBufferDepthSlot.format,
                kDeferredGBufferDepthSlot.usage);
        }

        bool DeferredPreparedSceneResourceTextureMatchesFrameDesc(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const NLS::Render::RHI::TextureFormat expectedFormat,
            const NLS::Render::RHI::TextureUsageFlags requiredUsage)
        {
            if (texture == nullptr)
                return false;

            const auto& desc = texture->GetDesc();
            return TextureDescMatchesFrameDescriptor(desc, frameDescriptor) &&
                desc.format == expectedFormat &&
                TextureDescHasRequiredUsage(desc, requiredUsage);
        }

        bool DeferredPreparedSceneResourceViewMatchesSlot(
            const std::shared_ptr<NLS::Render::RHI::RHITextureView>& view,
            const NLS::Render::RHI::TextureFormat expectedFormat)
        {
            if (view == nullptr || view->GetTexture() == nullptr)
                return false;

            const auto& desc = view->GetDesc();
            if (desc.format != expectedFormat ||
                (desc.viewType != NLS::Render::RHI::TextureViewType::Auto &&
                    desc.viewType != NLS::Render::RHI::TextureViewType::Texture2D))
            {
                return false;
            }

            const auto normalizedRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                view->GetTexture()->GetDesc(),
                desc.subresourceRange);
            return normalizedRange.has_value() &&
                NLS::Render::RHI::IsFullTextureSubresourceRange(
                    view->GetTexture()->GetDesc(),
                    *normalizedRange);
        }

        bool HasCompleteDeferredPreparedSceneResources(const DeferredPreparedSceneResources& resources)
        {
            if (resources.gbufferColorViews.size() != kDeferredGBufferColorAttachmentCount ||
                resources.gbufferTextures.size() != kDeferredGBufferTextureCount ||
                resources.gbufferDepthView == nullptr)
            {
                return false;
            }

            for (size_t i = 0u; i < kDeferredGBufferColorAttachmentCount; ++i)
            {
                if (resources.gbufferColorViews[i] == nullptr ||
                    resources.gbufferTextures[i] == nullptr ||
                    resources.gbufferColorViews[i]->GetTexture() != resources.gbufferTextures[i] ||
                    !DeferredPreparedSceneResourceViewMatchesSlot(
                        resources.gbufferColorViews[i],
                        kDeferredGBufferColorSlots[i].format))
                {
                    return false;
                }
            }

            return resources.gbufferTextures[kDeferredGBufferDepthTextureIndex] != nullptr &&
                resources.gbufferDepthView->GetTexture() == resources.gbufferTextures[kDeferredGBufferDepthTextureIndex] &&
                DeferredPreparedSceneResourceViewMatchesSlot(
                    resources.gbufferDepthView,
                    kDeferredGBufferDepthSlot.format);
        }

        bool HasCompleteDeferredPreparedSceneResources(
            const DeferredPreparedSceneResources& resources,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor)
        {
            if (!HasCompleteDeferredPreparedSceneResources(resources))
                return false;

            for (size_t i = 0u; i < kDeferredGBufferColorAttachmentCount; ++i)
            {
                const auto& slot = kDeferredGBufferColorSlots[i];
                if (!DeferredPreparedSceneResourceTextureMatchesFrameDesc(
                        resources.gbufferTextures[i],
                        frameDescriptor,
                        slot.format,
                        slot.usage))
                {
                    return false;
                }
            }

            return DeferredPreparedSceneResourceTextureMatchesFrameDesc(
                resources.gbufferTextures[kDeferredGBufferDepthTextureIndex],
                frameDescriptor,
                kDeferredGBufferDepthSlot.format,
                kDeferredGBufferDepthSlot.usage);
        }

        void AddFullTextureResourceAccess(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::Context::ResourceAccessMode mode,
            const NLS::Render::RHI::ResourceState state,
            const NLS::Render::RHI::PipelineStageMask stages,
            const NLS::Render::RHI::AccessMask access)
        {
            if (texture == nullptr)
                return;

            NLS::Render::RHI::RHISubresourceRange fullRange;
            fullRange.baseMipLevel = 0u;
            fullRange.mipLevelCount = texture->GetDesc().mipLevels;
            fullRange.baseArrayLayer = 0u;
            fullRange.arrayLayerCount = texture->GetDesc().arrayLayers;
            passInput.textureResourceAccesses.push_back({
                texture,
                fullRange,
                mode,
                state,
                stages,
                access
            });
        }

        bool HasTextureWriteResourceAccess(
            const NLS::Render::Context::RenderPassCommandInput& passInput,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHISubresourceRange& subresourceRange)
        {
            if (texture == nullptr)
                return true;

            return std::any_of(
                passInput.textureResourceAccesses.begin(),
                passInput.textureResourceAccesses.end(),
                [&texture, &subresourceRange](const NLS::Render::Context::TextureResourceAccess& access)
                {
                    if (access.texture != texture ||
                        access.mode != NLS::Render::Context::ResourceAccessMode::Write)
                    {
                        return false;
                    }

                    const auto accessRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        access.subresourceRange);
                    const auto requestedRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        subresourceRange);
                    return accessRange.has_value() &&
                        requestedRange.has_value() &&
                        NLS::Render::RHI::DoesSubresourceRangeCover(*accessRange, *requestedRange);
                });
        }

        bool HasTextureResourceAccess(
            const NLS::Render::Context::RenderPassCommandInput& passInput,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHISubresourceRange& subresourceRange,
            const NLS::Render::Context::ResourceAccessMode mode)
        {
            if (texture == nullptr)
                return true;

            return std::any_of(
                passInput.textureResourceAccesses.begin(),
                passInput.textureResourceAccesses.end(),
                [&texture, &subresourceRange, mode](const NLS::Render::Context::TextureResourceAccess& access)
                {
                    if (access.texture != texture || access.mode != mode)
                        return false;

                    const auto accessRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        access.subresourceRange);
                    const auto requestedRange = NLS::Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        subresourceRange);
                    return accessRange.has_value() &&
                        requestedRange.has_value() &&
                        NLS::Render::RHI::DoesSubresourceRangeCover(*accessRange, *requestedRange);
                });
        }

        void AddTextureResourceAccess(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHISubresourceRange& subresourceRange,
            const NLS::Render::Context::ResourceAccessMode mode,
            const NLS::Render::RHI::ResourceState state,
            const NLS::Render::RHI::PipelineStageMask stages,
            const NLS::Render::RHI::AccessMask access)
        {
            if (texture == nullptr)
                return;

            passInput.textureResourceAccesses.push_back({
                texture,
                subresourceRange,
                mode,
                state,
                stages,
                access
            });
        }

        void RemoveTextureResourceAccesses(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture)
        {
            if (texture == nullptr)
                return;

            passInput.textureResourceAccesses.erase(
                std::remove_if(
                    passInput.textureResourceAccesses.begin(),
                    passInput.textureResourceAccesses.end(),
                    [&texture](const NLS::Render::Context::TextureResourceAccess& access)
                    {
                        return access.texture == texture;
                    }),
                passInput.textureResourceAccesses.end());
        }

        void AddDecalColorAttachmentResourceAccesses(
            NLS::Render::Context::RenderPassCommandInput& passInput);

        void AddColorAttachmentWriteResourceAccesses(
            NLS::Render::Context::RenderPassCommandInput& passInput)
        {
            if (passInput.kind == NLS::Render::Context::RenderPassCommandKind::Decal)
            {
                AddDecalColorAttachmentResourceAccesses(passInput);
                return;
            }

            for (const auto& colorView : passInput.colorAttachmentViews)
            {
                const auto& texture = colorView != nullptr ? colorView->GetTexture() : nullptr;
                const auto subresourceRange = colorView != nullptr
                    ? colorView->GetDesc().subresourceRange
                    : NLS::Render::RHI::RHISubresourceRange{};
                if (HasTextureWriteResourceAccess(passInput, texture, subresourceRange))
                    continue;

                AddTextureResourceAccess(
                    passInput,
                    texture,
                    subresourceRange,
                    NLS::Render::Context::ResourceAccessMode::Write,
                    NLS::Render::RHI::ResourceState::RenderTarget,
                    NLS::Render::RHI::PipelineStageMask::RenderTarget,
                    NLS::Render::RHI::AccessMask::ColorAttachmentRead |
                        NLS::Render::RHI::AccessMask::ColorAttachmentWrite);
            }
        }

        void AddDecalColorAttachmentResourceAccesses(
            NLS::Render::Context::RenderPassCommandInput& passInput)
        {
            // The pass binds the full GBuffer MRT set; current color decals write albedo
            // only through their PSO color-write masks.
            for (size_t index = 0u; index < passInput.colorAttachmentViews.size(); ++index)
            {
                const auto& colorView = passInput.colorAttachmentViews[index];
                const auto& texture = colorView != nullptr ? colorView->GetTexture() : nullptr;
                const auto subresourceRange = colorView != nullptr
                    ? colorView->GetDesc().subresourceRange
                    : NLS::Render::RHI::RHISubresourceRange{};
                if (HasTextureWriteResourceAccess(passInput, texture, subresourceRange))
                    continue;

                AddTextureResourceAccess(
                    passInput,
                    texture,
                    subresourceRange,
                    NLS::Render::Context::ResourceAccessMode::Write,
                    NLS::Render::RHI::ResourceState::RenderTarget,
                    NLS::Render::RHI::PipelineStageMask::RenderTarget,
                    NLS::Render::RHI::AccessMask::ColorAttachmentRead |
                        NLS::Render::RHI::AccessMask::ColorAttachmentWrite);
            }
        }

        void AttachDeferredGBufferDepthForPostLightingPasses(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const DeferredPreparedSceneResources& resources)
        {
            const bool needsDeferredDepth =
                passInput.kind == NLS::Render::Context::RenderPassCommandKind::Transparent ||
                passInput.kind == NLS::Render::Context::RenderPassCommandKind::Helper;
            if (!needsDeferredDepth ||
                !passInput.usesDepthStencilAttachment ||
                resources.gbufferDepthView == nullptr)
            {
                return;
            }

            if (passInput.depthStencilAttachmentView != nullptr &&
                (passInput.clearDepth || passInput.clearStencil))
            {
                return;
            }

            const auto previousDepthTexture = passInput.depthStencilAttachmentView != nullptr
                ? passInput.depthStencilAttachmentView->GetTexture()
                : nullptr;
            const auto gbufferDepthTexture = resources.gbufferDepthView->GetTexture();
            RemoveTextureResourceAccesses(passInput, previousDepthTexture);
            RemoveTextureResourceAccesses(passInput, gbufferDepthTexture);

            const auto accessMode = passInput.writesDepthStencilAttachment
                ? NLS::Render::Context::ResourceAccessMode::Write
                : NLS::Render::Context::ResourceAccessMode::Read;
            const auto resourceState = passInput.writesDepthStencilAttachment
                ? NLS::Render::RHI::ResourceState::DepthWrite
                : NLS::Render::RHI::ResourceState::DepthRead;
            const auto accessMask = passInput.writesDepthStencilAttachment
                ? (NLS::Render::RHI::AccessMask::DepthStencilRead |
                    NLS::Render::RHI::AccessMask::DepthStencilWrite)
                : NLS::Render::RHI::AccessMask::DepthStencilRead;

            passInput.depthStencilAttachmentView = resources.gbufferDepthView;
            AddFullTextureResourceAccess(
                passInput,
                gbufferDepthTexture,
                accessMode,
                resourceState,
                NLS::Render::RHI::PipelineStageMask::DepthStencil,
                accessMask);
        }

        NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeGBufferColorDesc(
            uint16_t width,
            uint16_t height,
            size_t attachmentIndex,
            std::string debugName)
        {
            NLS::Render::FrameGraph::FrameGraphTexture::Desc desc;
            desc.extent.width = width;
            desc.extent.height = height;
            desc.extent.depth = 1u;
            const auto& slot = NLS::Render::FrameGraph::kDeferredGBufferColorSlots[attachmentIndex];
            desc.format = slot.format;
            desc.usage = slot.usage;
            desc.debugName = std::move(debugName);
            return desc;
        }

        NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeGBufferDepthDesc(uint16_t width, uint16_t height)
        {
            NLS::Render::FrameGraph::FrameGraphTexture::Desc desc;
            desc.extent.width = width;
            desc.extent.height = height;
            desc.extent.depth = 1u;
            desc.format = NLS::Render::FrameGraph::kDeferredGBufferDepthSlot.format;
            desc.usage = NLS::Render::FrameGraph::kDeferredGBufferDepthSlot.usage;
            desc.debugName = NLS::Render::FrameGraph::kDeferredGBufferDepthSlot.debugName;
            return desc;
        }

        template<typename TExecuteFn>
        const DeferredGBufferGraphPassData& AddDeferredCompiledGBufferPass(
            ::FrameGraph& frameGraph,
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            const FrameGraphResource gbufferAlbedo,
            const FrameGraphResource gbufferNormal,
            const FrameGraphResource gbufferMaterial,
            const FrameGraphResource gbufferDepth,
            TExecuteFn&& execute)
        {
            return AddExecutingMetadataPass<DeferredGBufferGraphPassData>(
                frameGraph,
                compiledPass,
                [=](::FrameGraph::Builder& builder, DeferredGBufferGraphPassData& data)
                {
                    data.albedo = builder.write(gbufferAlbedo);
                    data.normal = builder.write(gbufferNormal);
                    data.material = builder.write(gbufferMaterial);
                    data.depth = builder.write(gbufferDepth);
                },
                std::forward<TExecuteFn>(execute));
        }

        template<typename TExecuteFn>
        const DeferredGBufferGraphPassData& AddDeferredCompiledDecalPass(
            ::FrameGraph& frameGraph,
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            const DeferredGBufferGraphPassData* gBufferPassData,
            TExecuteFn&& execute)
        {
            return AddExecutingMetadataPass<DeferredGBufferGraphPassData>(
                frameGraph,
                compiledPass,
                [gBufferPassData](::FrameGraph::Builder& builder, DeferredGBufferGraphPassData& data)
                {
                    if (gBufferPassData == nullptr)
                        return;

                    data.albedo = builder.write(gBufferPassData->albedo);
                    data.normal = builder.write(gBufferPassData->normal);
                    data.material = builder.write(gBufferPassData->material);
                    data.depth = builder.read(gBufferPassData->depth);
                },
                std::forward<TExecuteFn>(execute));
        }

        template<typename TExecuteFn>
        void AddDeferredCompiledLightingPass(
            ::FrameGraph& frameGraph,
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            ScenePassOutputResourceState& outputState,
            const DeferredGBufferGraphPassData* gBufferPassData,
            TExecuteFn&& execute)
        {
            AddExecutingSceneOutputPass<
                DeferredLightingGraphPassData,
                &DeferredLightingGraphPassData::outputColor,
                &DeferredLightingGraphPassData::outputDepth>(
                frameGraph,
                compiledPass,
                outputState,
                [gBufferPassData](::FrameGraph::Builder& builder, DeferredLightingGraphPassData& data)
                {
                    if (gBufferPassData == nullptr)
                        return;
                    data.albedo = builder.read(gBufferPassData->albedo);
                    data.normal = builder.read(gBufferPassData->normal);
                    data.material = builder.read(gBufferPassData->material);
                    data.depth = builder.read(gBufferPassData->depth);
                },
                std::forward<TExecuteFn>(execute));
        }

        template<typename TExecuteFn>
        void AddDeferredCompiledTransparentPass(
            ::FrameGraph& frameGraph,
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            ScenePassOutputResourceState& outputState,
            const DeferredGBufferGraphPassData* gBufferPassData,
            TExecuteFn&& execute)
        {
            AddExecutingSceneOutputPass<
                DeferredTransparentGraphPassData,
                &DeferredTransparentGraphPassData::outputColor,
                &DeferredTransparentGraphPassData::outputDepth>(
                frameGraph,
                compiledPass,
                outputState,
                [gBufferPassData](::FrameGraph::Builder& builder, DeferredTransparentGraphPassData& data)
                {
                    if (gBufferPassData == nullptr)
                        return;
                    data.gbufferDepth = builder.read(gBufferPassData->depth);
                },
                std::forward<TExecuteFn>(execute));
        }

        template<
            typename TAddGBufferPassFn,
            typename TAddDecalPassFn,
            typename TAddLightingPassFn,
            typename TAddTransparentPassFn>
        void DispatchDeferredCompiledGraphPasses(
            ::FrameGraph& frameGraph,
            const PreparedComputeDispatchSource& preparedComputeSource,
            const std::vector<CompiledThreadedRenderSceneGraphPass>& compiledPasses,
            TAddGBufferPassFn&& addGBufferPass,
            TAddDecalPassFn&& addDecalPass,
            TAddLightingPassFn&& addLightingPass,
            TAddTransparentPassFn&& addTransparentPass)
        {
            const DeferredGBufferGraphPassData* gBufferPassData = nullptr;
            ScenePassOutputResourceState outputState;
            for (const auto& compiledPass : compiledPasses)
            {
                if (TryAddPreparedComputeDispatchCompiledGraphPass(
                    frameGraph,
                    preparedComputeSource,
                    compiledPass))
                {
                    continue;
                }

                SeedScenePassOutputResourceState(outputState, compiledPass.outputChain);

                switch (GetDeferredScenePassExecutionKind(compiledPass.metadata.commandKind))
                {
                case DeferredScenePassExecutionKind::GBuffer:
                    gBufferPassData = &addGBufferPass(compiledPass);
                    break;
                case DeferredScenePassExecutionKind::Decal:
                    if (gBufferPassData != nullptr)
                        gBufferPassData = &addDecalPass(compiledPass, gBufferPassData);
                    break;
                case DeferredScenePassExecutionKind::Lighting:
                    addLightingPass(compiledPass, outputState, gBufferPassData);
                    break;
                case DeferredScenePassExecutionKind::Transparent:
                    addTransparentPass(compiledPass, outputState, gBufferPassData);
                    break;
                default:
                    break;
                }
            }
        }

        DeferredGraphSceneResources ImportDeferredSceneGraphResources(
            const DeferredGraphSceneResourceRequest& request)
        {
            DeferredGraphSceneResources resources;
            if (request.frameGraph == nullptr || request.frameDescriptor == nullptr)
                return resources;

            if (request.blackboard != nullptr)
            {
                ImportSceneRenderTargets(
                    *request.frameGraph,
                    *request.blackboard,
                    *request.frameDescriptor,
                    request.outputColorResourceName,
                    request.outputDepthResourceName);
                resources.sceneTargets = ResolveImportedSceneRenderTargets(*request.blackboard);
            }

            if (!HasDeferredPreparedSceneResourceRequestMatchingFrameDesc(
                    request.preparedResources,
                    *request.frameDescriptor))
            {
                return resources;
            }

            const auto width = request.frameDescriptor->renderWidth;
            const auto height = request.frameDescriptor->renderHeight;
            const auto& colorHandles = request.preparedResources.gBuffer->GetExplicitColorTextureHandles();

            const std::array<const NLS::Render::Resources::Texture2D*, kDeferredGBufferColorAttachmentCount> colorWrappers = {
                request.preparedResources.gbufferAlbedoTexture,
                request.preparedResources.gbufferNormalTexture,
                request.preparedResources.gbufferMaterialTexture
            };
            std::array<FrameGraphResource*, kDeferredGBufferColorAttachmentCount> importedColorResources = {
                &resources.gbufferAlbedo,
                &resources.gbufferNormal,
                &resources.gbufferMaterial
            };
            for (size_t i = 0u; i < kDeferredGBufferColorAttachmentCount; ++i)
            {
                if (colorWrappers[i] == nullptr || colorHandles[i] == nullptr)
                    continue;

                const auto& slot = kDeferredGBufferColorSlots[i];
                *importedColorResources[i] = request.frameGraph->import<NLS::Render::FrameGraph::FrameGraphTexture>(
                    slot.graphResourceName,
                    MakeGBufferColorDesc(width, height, i, slot.debugName),
                    NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
                        colorHandles[i],
                        request.preparedResources.gBuffer->GetOrCreateExplicitColorView(
                            static_cast<uint32_t>(i),
                            slot.graphViewName)));
            }

            const auto depthHandle = request.preparedResources.gBuffer->GetExplicitDepthTextureHandle();
            if (request.preparedResources.gbufferDepthTexture != nullptr && depthHandle != nullptr)
            {
                resources.gbufferDepth = request.frameGraph->import<NLS::Render::FrameGraph::FrameGraphTexture>(
                    kDeferredGBufferDepthSlot.graphResourceName,
                    MakeGBufferDepthDesc(width, height),
                    NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
                        depthHandle,
                        request.preparedResources.gBuffer->GetOrCreateExplicitDepthView(
                            kDeferredGBufferDepthSlot.graphViewName)));
            }

            return resources;
        }

        bool HasCompleteDeferredGraphGBufferResources(const DeferredGraphSceneResources& resources)
        {
            return resources.gbufferAlbedo >= 0 &&
                resources.gbufferNormal >= 0 &&
                resources.gbufferMaterial >= 0 &&
                resources.gbufferDepth >= 0;
        }

        const NLS::Render::Context::RenderPassCommandInput* FindPackagePassInputByKind(
            const NLS::Render::Context::RenderScenePackage& package,
            const NLS::Render::Context::RenderPassCommandKind kind)
        {
            const auto passInput = std::find_if(
                package.passCommandInputs.begin(),
                package.passCommandInputs.end(),
                [kind](const NLS::Render::Context::RenderPassCommandInput& input)
                {
                    return input.kind == kind;
                });
            return passInput != package.passCommandInputs.end() ? &(*passInput) : nullptr;
        }

        bool HasTypedRecordedDrawCommands(
            const NLS::Render::Context::RenderPassCommandInput* passInput)
        {
            return passInput != nullptr && !passInput->recordedDrawCommands.empty();
        }

        const NLS::Render::Context::RenderPassCommandInput* FindDeferredPreparedScenePassInput(
            const NLS::Render::Context::RenderScenePackage& package,
            const DeferredScenePassExecutionKind executionKind)
        {
            switch (executionKind)
            {
            case DeferredScenePassExecutionKind::GBuffer:
            {
                const auto* gbufferInput = FindPackagePassInputByKind(
                    package,
                    NLS::Render::Context::RenderPassCommandKind::GBuffer);
                const auto* opaqueInput = FindPackagePassInputByKind(
                    package,
                    NLS::Render::Context::RenderPassCommandKind::Opaque);
                if (HasTypedRecordedDrawCommands(gbufferInput) || opaqueInput == nullptr)
                    return gbufferInput;
                return opaqueInput;
            }
            case DeferredScenePassExecutionKind::Decal:
                return FindPackagePassInputByKind(package, NLS::Render::Context::RenderPassCommandKind::Decal);
            case DeferredScenePassExecutionKind::Lighting:
                return FindPackagePassInputByKind(package, NLS::Render::Context::RenderPassCommandKind::Lighting);
            case DeferredScenePassExecutionKind::Transparent:
                return FindPackagePassInputByKind(package, NLS::Render::Context::RenderPassCommandKind::Transparent);
            default:
                return nullptr;
            }
        }

        void ValidateDeferredTypedRecordedDrawSource(
            const char* passName,
            const NLS::Render::Context::RenderPassCommandInput& typedPassInput,
            const uint64_t expectedDrawCount)
        {
            if (typedPassInput.drawCount != expectedDrawCount ||
                static_cast<uint64_t>(typedPassInput.recordedDrawCommands.size()) != expectedDrawCount)
            {
                throw std::invalid_argument(
                    std::string("Deferred typed pass input recorded draw count does not match declared drawCount for ") +
                    passName + ".");
            }
        }

        void AssignDeferredRecordedDrawCommands(
            NLS::Render::Context::RenderPassCommandInput& passInput,
            const NLS::Render::Context::RenderPassCommandInput* typedPassInput,
            const std::vector<NLS::Render::Context::RecordedDrawCommandInput>& fallbackRecordedDrawCommands,
            const uint64_t fallbackBeginOffset,
            const uint64_t drawCount,
            const char* passName)
        {
            if (HasTypedRecordedDrawCommands(typedPassInput))
            {
                ValidateDeferredTypedRecordedDrawSource(
                    passName,
                    *typedPassInput,
                    drawCount);
                passInput.recordedDrawCommands = typedPassInput->recordedDrawCommands;
                return;
            }

            if (fallbackRecordedDrawCommands.size() <= fallbackBeginOffset || drawCount == 0u)
                return;

            const auto begin =
                fallbackRecordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(fallbackBeginOffset);
            const auto endOffset = static_cast<size_t>(
                std::min<uint64_t>(
                    fallbackBeginOffset + drawCount,
                    static_cast<uint64_t>(fallbackRecordedDrawCommands.size())));
            passInput.recordedDrawCommands.assign(
                begin,
                fallbackRecordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(endOffset));
        }

        PreparedComputeDispatchSource MergePreparedComputeDispatchSources(
            const PreparedComputeDispatchSource& lightGridSource,
            const PreparedComputeDispatchSource& hzbSource)
        {
            PreparedComputeDispatchSource mergedSource;
            mergedSource.dispatchInputs.reserve(
                lightGridSource.dispatchInputs.size() + hzbSource.dispatchInputs.size());
            mergedSource.dispatchInputs.insert(
                mergedSource.dispatchInputs.end(),
                lightGridSource.dispatchInputs.begin(),
                lightGridSource.dispatchInputs.end());
            mergedSource.dispatchInputs.insert(
                mergedSource.dispatchInputs.end(),
                hzbSource.dispatchInputs.begin(),
                hzbSource.dispatchInputs.end());
            mergedSource.metadata = BuildPreparedComputeDispatchPassMetadata(mergedSource.dispatchInputs);
            return mergedSource;
        }

        std::vector<ThreadedRenderScenePassMetadata> BuildDeferredSceneGraphMetadata(
            const PreparedComputeDispatchSource& lightGridSource,
            const PreparedComputeDispatchSource& hzbSource)
        {
            const auto deferredPassDescriptors = GetDeferredScenePassDescriptors();
            std::vector<ThreadedRenderScenePassMetadata> metadata;
            metadata.reserve(
                lightGridSource.metadata.size() +
                hzbSource.metadata.size() +
                deferredPassDescriptors.size());

            metadata.insert(
                metadata.end(),
                lightGridSource.metadata.begin(),
                lightGridSource.metadata.end());
            metadata.push_back(deferredPassDescriptors[0].metadata);
            metadata.insert(
                metadata.end(),
                hzbSource.metadata.begin(),
                hzbSource.metadata.end());
            metadata.push_back(deferredPassDescriptors[1].metadata);

            PromotePreparedComputeConsumerDependencies(metadata);
            return metadata;
        }

        NLS::Render::Context::RenderPassCommandInput BuildDeferredScenePassInput(
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            const NLS::Render::Context::RenderScenePackage& package,
            const PreparedComputeDispatchSource& preparedComputeSource,
            const uint64_t opaqueDrawCount,
            const uint64_t decalDrawCount,
            const uint64_t lightingDrawCount,
            const uint64_t transparentDrawCount,
            const DeferredPreparedSceneResources& resources)
        {
            NLS::Render::Context::RenderPassCommandInput passInput;
            if (TryBuildPreparedComputeDispatchThreadedPassInput(
                preparedComputeSource,
                compiledPass,
                passInput))
            {
                return passInput;
            }

            passInput = MakeCompiledThreadedRenderPassCommandInput(compiledPass);
            const auto executionKind = GetDeferredScenePassExecutionKind(compiledPass.metadata.commandKind);
            const auto* typedPassInput = FindDeferredPreparedScenePassInput(package, executionKind);
            switch (executionKind)
            {
            case DeferredScenePassExecutionKind::GBuffer:
                passInput.drawCount = opaqueDrawCount;
                AssignDeferredRecordedDrawCommands(
                    passInput,
                    typedPassInput,
                    package.recordedDrawCommands,
                    0u,
                    opaqueDrawCount,
                    "GBuffer");
                passInput.requiresFrameData = true;
                passInput.requiresObjectData = true;
                passInput.targetsSwapchain = false;
                passInput.usesColorAttachment = true;
                passInput.usesDepthStencilAttachment = true;
                passInput.writesDepthStencilAttachment = true;
                passInput.gbufferTextures = resources.gbufferTextures;
                passInput.colorAttachmentViews = resources.gbufferColorViews;
                passInput.depthStencilAttachmentView = resources.gbufferDepthView;
                for (const auto& colorView : resources.gbufferColorViews)
                {
                    const auto& texture = colorView != nullptr ? colorView->GetTexture() : nullptr;
                    AddFullTextureResourceAccess(
                        passInput,
                        texture,
                        NLS::Render::Context::ResourceAccessMode::Write,
                        NLS::Render::RHI::ResourceState::RenderTarget,
                        NLS::Render::RHI::PipelineStageMask::AllCommands,
                        NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite);
                }
                if (resources.gbufferDepthView != nullptr &&
                    resources.gbufferDepthView->GetTexture() != nullptr)
                {
                    AddFullTextureResourceAccess(
                        passInput,
                        resources.gbufferDepthView->GetTexture(),
                        NLS::Render::Context::ResourceAccessMode::Write,
                        NLS::Render::RHI::ResourceState::DepthWrite,
                        NLS::Render::RHI::PipelineStageMask::AllCommands,
                        NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite);
                }
                break;
            case DeferredScenePassExecutionKind::Decal:
            {
                passInput.drawCount = decalDrawCount;
                const auto decalBeginOffset = opaqueDrawCount;
                AssignDeferredRecordedDrawCommands(
                    passInput,
                    typedPassInput,
                    package.recordedDrawCommands,
                    decalBeginOffset,
                    decalDrawCount,
                    "Decal");
                passInput.requiresFrameData = true;
                passInput.requiresObjectData = true;
                passInput.targetsSwapchain = false;
                passInput.usesColorAttachment = true;
                passInput.usesDepthStencilAttachment = true;
                passInput.writesDepthStencilAttachment = false;
                passInput.gbufferTextures = resources.gbufferTextures;
                passInput.colorAttachmentViews = resources.gbufferColorViews;
                passInput.depthStencilAttachmentView = resources.gbufferDepthView;
                AddDecalColorAttachmentResourceAccesses(passInput);
                if (resources.gbufferDepthView != nullptr &&
                    resources.gbufferDepthView->GetTexture() != nullptr)
                {
                    AddFullTextureResourceAccess(
                        passInput,
                        resources.gbufferDepthView->GetTexture(),
                        NLS::Render::Context::ResourceAccessMode::Read,
                        NLS::Render::RHI::ResourceState::DepthRead,
                        NLS::Render::RHI::PipelineStageMask::DepthStencil,
                        NLS::Render::RHI::AccessMask::DepthStencilRead);
                }
                break;
            }
            case DeferredScenePassExecutionKind::Lighting:
            {
                passInput.drawCount = lightingDrawCount;
                const auto lightingBeginOffset = opaqueDrawCount + decalDrawCount;
                AssignDeferredRecordedDrawCommands(
                    passInput,
                    typedPassInput,
                    package.recordedDrawCommands,
                    lightingBeginOffset,
                    lightingDrawCount,
                    "Lighting");
                passInput.requiresFrameData = true;
                passInput.requiresLightingData = true;
                passInput.targetsSwapchain = package.targetsSwapchain;
                passInput.usesColorAttachment = true;
                passInput.usesDepthStencilAttachment = false;
                passInput.gbufferTextures = resources.gbufferTextures;
                for (const auto& texture : resources.gbufferTextures)
                {
                    AddFullTextureResourceAccess(
                        passInput,
                        texture,
                        NLS::Render::Context::ResourceAccessMode::Read,
                        NLS::Render::RHI::ResourceState::ShaderRead,
                        NLS::Render::RHI::PipelineStageMask::FragmentShader | NLS::Render::RHI::PipelineStageMask::ComputeShader,
                        NLS::Render::RHI::AccessMask::ShaderRead);
                }
                break;
            }
            case DeferredScenePassExecutionKind::Transparent:
            {
                passInput.drawCount = transparentDrawCount;
                const auto transparentBeginOffset = opaqueDrawCount + decalDrawCount + lightingDrawCount;
                AssignDeferredRecordedDrawCommands(
                    passInput,
                    typedPassInput,
                    package.recordedDrawCommands,
                    transparentBeginOffset,
                    transparentDrawCount,
                    "Transparent");
                passInput.requiresFrameData = true;
                passInput.requiresObjectData = true;
                passInput.targetsSwapchain = package.targetsSwapchain;
                passInput.usesColorAttachment = true;
                passInput.usesDepthStencilAttachment = true;
                passInput.writesDepthStencilAttachment = false;
                passInput.depthStencilAttachmentView = resources.gbufferDepthView;
                if (resources.gbufferDepthView != nullptr &&
                    resources.gbufferDepthView->GetTexture() != nullptr)
                {
                    AddFullTextureResourceAccess(
                        passInput,
                        resources.gbufferDepthView->GetTexture(),
                        NLS::Render::Context::ResourceAccessMode::Read,
                        NLS::Render::RHI::ResourceState::DepthRead,
                        NLS::Render::RHI::PipelineStageMask::DepthStencil,
                        NLS::Render::RHI::AccessMask::DepthStencilRead);
                }
                break;
            }
            default:
                break;
            }

            return passInput;
        }

        std::vector<NLS::Render::Context::RenderPassCommandInput> BuildDeferredScenePassInputs(
            const std::vector<CompiledThreadedRenderSceneGraphPass>& compiledPasses,
            const NLS::Render::Context::RenderScenePackage& package,
            const PreparedComputeDispatchSource& preparedComputeSource,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const uint64_t opaqueDrawCount,
            const uint64_t decalDrawCount,
            const uint64_t lightingDrawCount,
            const uint64_t transparentDrawCount,
            const DeferredPreparedSceneResources& resources)
        {
            std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
            if (!HasCompleteDeferredPreparedSceneResources(resources, frameDescriptor))
                return passInputs;

            passInputs.reserve(compiledPasses.size());

            for (const auto& compiledPass : compiledPasses)
            {
                passInputs.push_back(BuildDeferredScenePassInput(
                    compiledPass,
                    package,
                    preparedComputeSource,
                    opaqueDrawCount,
                    decalDrawCount,
                    lightingDrawCount,
                    transparentDrawCount,
                    resources));
            }

            return passInputs;
        }

        std::optional<NLS::Render::Context::RenderPassCommandInput> BuildDeferredAggregateHelperPassInput(
            const NLS::Render::Context::RenderScenePackage& package,
            const uint64_t opaqueDrawCount,
            const uint64_t decalDrawCount,
            const uint64_t lightingDrawCount,
            const uint64_t transparentDrawCount,
            const bool canSlicePostLightingRecordedDraws)
        {
            const auto* typedHelperInput = FindPackagePassInputByKind(
                package,
                NLS::Render::Context::RenderPassCommandKind::Helper);
            if (HasTypedRecordedDrawCommands(typedHelperInput))
            {
                ValidateDeferredTypedRecordedDrawSource(
                    "Helper",
                    *typedHelperInput,
                    package.helperDrawCount);
                return *typedHelperInput;
            }

            if (!canSlicePostLightingRecordedDraws)
                return std::nullopt;

            const auto helperStart = static_cast<size_t>(
                std::min<uint64_t>(
                    opaqueDrawCount + decalDrawCount + lightingDrawCount + transparentDrawCount,
                    static_cast<uint64_t>(package.recordedDrawCommands.size())));
            const auto helperDrawCount =
                package.recordedDrawCommands.size() > helperStart
                    ? package.recordedDrawCommands.size() - helperStart
                    : 0u;
            if (helperDrawCount == 0u)
                return std::nullopt;

            NLS::Render::Context::RenderPassCommandInput passInput;
            passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
            passInput.debugName = "EditorHelperPass";
            passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
            passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            passInput.drawCount = static_cast<uint64_t>(helperDrawCount);
            passInput.requiresFrameData = true;
            passInput.requiresObjectData = true;
            passInput.targetsSwapchain = package.targetsSwapchain;
            passInput.renderWidth = package.renderWidth;
            passInput.renderHeight = package.renderHeight;
            passInput.usesColorAttachment = true;
            passInput.usesDepthStencilAttachment = true;
            passInput.recordedDrawCommands.assign(
                package.recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(helperStart),
                package.recordedDrawCommands.end());
            return passInput;
        }

        uint64_t ResolveDeferredLightingDrawCount(
            const NLS::Render::Context::RenderScenePackage& package,
            const uint64_t opaqueDrawCount,
            const uint64_t decalDrawCount,
            const std::optional<uint64_t> queuedLightingDrawCount = std::nullopt)
        {
            (void)package;
            (void)opaqueDrawCount;
            (void)decalDrawCount;
            if (!queuedLightingDrawCount.has_value())
                return 0u;

            return *queuedLightingDrawCount;
        }

        void ValidateDeferredRecordedDrawSliceSize(
            const char* passName,
            const uint64_t recordedDrawCount,
            const uint64_t sliceBegin,
            const uint64_t sliceDrawCount)
        {
            if (sliceBegin > recordedDrawCount ||
                sliceDrawCount > recordedDrawCount - sliceBegin)
            {
                throw std::invalid_argument(
                    std::string("Deferred recorded draw slice is shorter than declared drawCount for ") +
                    passName + ".");
            }
        }

        void ValidateDeferredRecordedDrawSource(
            const char* passName,
            const NLS::Render::Context::RenderPassCommandInput* typedPassInput,
            const uint64_t expectedDrawCount,
            const uint64_t recordedDrawCount,
            const uint64_t sliceBegin)
        {
            if (HasTypedRecordedDrawCommands(typedPassInput))
            {
                ValidateDeferredTypedRecordedDrawSource(
                    passName,
                    *typedPassInput,
                    expectedDrawCount);
                return;
            }

            ValidateDeferredRecordedDrawSliceSize(
                passName,
                recordedDrawCount,
                sliceBegin,
                expectedDrawCount);
        }

        void ValidateDeferredRecordedDrawSliceBoundaries(
            const NLS::Render::Context::RenderScenePackage& package,
            const uint64_t opaqueDrawCount,
            const uint64_t decalDrawCount,
            const uint64_t lightingDrawCount,
            const uint64_t transparentDrawCount,
            const DeferredPreparedQueuedDrawCounts& queuedDrawCounts,
            const bool canSliceHelperDraws)
        {
            const auto recordedDrawCount = static_cast<uint64_t>(package.recordedDrawCommands.size());
            const auto* gbufferInput = FindDeferredPreparedScenePassInput(
                package,
                DeferredScenePassExecutionKind::GBuffer);
            const auto* decalInput = FindDeferredPreparedScenePassInput(
                package,
                DeferredScenePassExecutionKind::Decal);
            const auto* lightingInput = FindDeferredPreparedScenePassInput(
                package,
                DeferredScenePassExecutionKind::Lighting);
            const auto* transparentInput = FindDeferredPreparedScenePassInput(
                package,
                DeferredScenePassExecutionKind::Transparent);
            const auto* helperInput = FindPackagePassInputByKind(
                package,
                NLS::Render::Context::RenderPassCommandKind::Helper);

            ValidateDeferredRecordedDrawSource(
                "GBuffer",
                gbufferInput,
                opaqueDrawCount,
                recordedDrawCount,
                0u);

            const auto decalBegin = opaqueDrawCount;
            ValidateDeferredRecordedDrawSource(
                "Decal",
                decalInput,
                decalDrawCount,
                recordedDrawCount,
                decalBegin);

            const auto lightingBegin = decalBegin + decalDrawCount;
            if (recordedDrawCount > lightingBegin &&
                !queuedDrawCounts.lightingDrawCount.has_value() &&
                !HasTypedRecordedDrawCommands(lightingInput))
            {
                throw std::invalid_argument(
                    "Deferred recorded draw slicing is ambiguous without queuedDrawCounts.lightingDrawCount.");
            }
            ValidateDeferredRecordedDrawSource(
                "Lighting",
                lightingInput,
                lightingDrawCount,
                recordedDrawCount,
                lightingBegin);

            const auto transparentBegin = lightingBegin + lightingDrawCount;
            if (recordedDrawCount > transparentBegin &&
                package.transparentDrawCount > 0u &&
                !queuedDrawCounts.transparentDrawCount.has_value() &&
                !HasTypedRecordedDrawCommands(transparentInput))
            {
                throw std::invalid_argument(
                    "Deferred recorded draw slicing is ambiguous without queuedDrawCounts.transparentDrawCount.");
            }
            ValidateDeferredRecordedDrawSource(
                "Transparent",
                transparentInput,
                transparentDrawCount,
                recordedDrawCount,
                transparentBegin);

            const auto helperBegin = transparentBegin + transparentDrawCount;
            if (HasTypedRecordedDrawCommands(helperInput))
            {
                ValidateDeferredTypedRecordedDrawSource(
                    "Helper",
                    *helperInput,
                    package.helperDrawCount);
                return;
            }

            const auto remainingHelperDraws =
                recordedDrawCount > helperBegin
                    ? recordedDrawCount - helperBegin
                    : 0u;
            if (remainingHelperDraws > 0u &&
                (!canSliceHelperDraws || remainingHelperDraws > package.helperDrawCount))
            {
                throw std::invalid_argument(
                    "Deferred recorded draw slicing leaves commands outside declared scene pass counts.");
            }
        }

        uint64_t ResolveDeferredDecalDrawCount(
            const NLS::Render::Context::RenderScenePackage& package,
            const uint64_t opaqueDrawCount,
            const std::optional<uint64_t> queuedDecalDrawCount = std::nullopt)
        {
            (void)opaqueDrawCount;
            const auto requestedDecalDraws = queuedDecalDrawCount.has_value()
                ? *queuedDecalDrawCount
                : package.decalDrawCount;
            return requestedDecalDraws;
        }

        uint64_t ResolveDeferredTransparentDrawCount(
            const NLS::Render::Context::RenderScenePackage& package,
            const uint64_t opaqueDrawCount,
            const uint64_t decalDrawCount,
            const uint64_t lightingDrawCount,
            const std::optional<uint64_t> queuedTransparentDrawCount,
            const bool canSlicePostLightingRecordedDraws)
        {
            (void)opaqueDrawCount;
            (void)decalDrawCount;
            (void)lightingDrawCount;
            if (!canSlicePostLightingRecordedDraws)
                return 0u;

            const auto requestedTransparentDraws = queuedTransparentDrawCount.has_value()
                ? *queuedTransparentDrawCount
                : package.transparentDrawCount;
            return requestedTransparentDraws;
        }

        void AppendDeferredHelperPassInputForMetadata(
            std::vector<NLS::Render::Context::RenderPassCommandInput>& passInputs,
            const NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata& metadata,
            std::vector<NLS::Render::Context::RenderPassCommandInput>& appendedPassInputs,
            const std::optional<NLS::Render::Context::RenderPassCommandInput>& aggregateHelperPassInput)
        {
            if (metadata.commandKind != NLS::Render::Context::RenderPassCommandKind::Helper)
                return;

            const auto metadataName = metadata.graphPassName != nullptr
                ? std::string_view(metadata.graphPassName)
                : std::string_view{};
            for (auto appendedPassInput = appendedPassInputs.begin();
                appendedPassInput != appendedPassInputs.end();
                ++appendedPassInput)
            {
                if (!metadataName.empty() && appendedPassInput->debugName != metadataName)
                    continue;

                passInputs.push_back(std::move(*appendedPassInput));
                appendedPassInputs.erase(appendedPassInput);
                return;
            }

            if (aggregateHelperPassInput.has_value() &&
                (metadataName.empty() || aggregateHelperPassInput->debugName == metadataName))
            {
                passInputs.push_back(*aggregateHelperPassInput);
            }
        }

        DeferredScenePassDescriptor BuildDeferredTransparentPassDescriptor()
        {
            return {
                {
                    NLS::Render::Context::RenderPassCommandKind::Transparent,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Transparent,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "DeferredTransparent",
                    NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
                    true,
                    false
                }
            };
        }

        DeferredScenePassDescriptor BuildDeferredDecalPassDescriptor()
        {
            return {
                {
                    NLS::Render::Context::RenderPassCommandKind::Decal,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Decal,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "DeferredDecal",
                    NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
                    false,
                    false
                }
            };
        }

        DeferredScenePassDescriptorRange BuildDeferredScenePassDescriptors(
            const bool includeTransparentPass,
            const bool includeDecalPass)
        {
            const auto baseDescriptors = GetDeferredScenePassDescriptors();
            DeferredScenePassDescriptorRange range;
            range.descriptors[range.count++] = baseDescriptors[0];
            if (includeDecalPass)
                range.descriptors[range.count++] = BuildDeferredDecalPassDescriptor();
            range.descriptors[range.count++] = baseDescriptors[1];
            if (includeTransparentPass)
                range.descriptors[range.count++] = BuildDeferredTransparentPassDescriptor();
            return range;
        }
    }

    DeferredScenePassExecutionKind GetDeferredScenePassExecutionKind(
        NLS::Render::Context::RenderPassCommandKind kind)
    {
        switch (kind)
        {
        case NLS::Render::Context::RenderPassCommandKind::GBuffer:
            return DeferredScenePassExecutionKind::GBuffer;
        case NLS::Render::Context::RenderPassCommandKind::Decal:
            return DeferredScenePassExecutionKind::Decal;
        case NLS::Render::Context::RenderPassCommandKind::Lighting:
            return DeferredScenePassExecutionKind::Lighting;
        case NLS::Render::Context::RenderPassCommandKind::Transparent:
            return DeferredScenePassExecutionKind::Transparent;
        default:
            return DeferredScenePassExecutionKind::Unknown;
        }
    }

    PreparedComputeDispatchSource BuildHZBPreparedComputeDispatchSource(
        const HZBFrameResourceRequest& request)
    {
        NLS_PROFILE_SCOPE();
        if (!request.opaqueDepthEligible ||
            request.opaqueDepthTexture == nullptr ||
            request.hzbTexture == nullptr)
        {
            return {};
        }

        const auto hzbMipCount = (std::min<uint32_t>)(
            request.hzbMipCount,
            request.hzbTexture->GetDesc().mipLevels);
        if (hzbMipCount == 0u ||
            request.hzbBuildBindingSets.size() < hzbMipCount ||
            request.hzbBuildGroupCountsByMip.size() < hzbMipCount)
            return {};

        auto makeRange = [](const uint32_t baseMipLevel, const uint32_t mipLevelCount)
        {
            NLS::Render::RHI::RHISubresourceRange range;
            range.baseMipLevel = baseMipLevel;
            range.mipLevelCount = mipLevelCount;
            range.baseArrayLayer = 0u;
            range.arrayLayerCount = 1u;
            return range;
        };

        const auto opaqueDepthRange = NLS::Render::RHI::GetFullTextureSubresourceRange(
            request.opaqueDepthTexture->GetDesc());
        std::vector<NLS::Render::Context::RecordedComputeDispatchInput> dispatches;
        dispatches.reserve(static_cast<size_t>(hzbMipCount) + 1u);
        for (uint32_t mip = 0u; mip < hzbMipCount; ++mip)
        {
            NLS::Render::Context::RecordedComputeDispatchInput buildHZB;
            buildHZB.debugName = "HZBBuildMip" + std::to_string(mip);
            buildHZB.pipeline = request.hzbBuildPipeline;
            if (request.hzbBuildBindingSets[mip] != nullptr)
            {
                buildHZB.bindingSets.push_back({
                    NLS::Render::RHI::BindingPointMap::kPassDescriptorSet,
                    request.hzbBuildBindingSets[mip]
                });
            }
            buildHZB.groupCountX = request.hzbBuildGroupCountsByMip[mip][0];
            buildHZB.groupCountY = request.hzbBuildGroupCountsByMip[mip][1];
            buildHZB.groupCountZ = request.hzbBuildGroupCountsByMip[mip][2];
            if (mip == 0u)
            {
                buildHZB.textureResourceAccesses.push_back({
                    request.opaqueDepthTexture,
                    opaqueDepthRange,
                    NLS::Render::Context::ResourceAccessMode::Read,
                    NLS::Render::RHI::ResourceState::ShaderRead,
                    NLS::Render::RHI::PipelineStageMask::ComputeShader,
                    NLS::Render::RHI::AccessMask::ShaderRead
                });
                buildHZB.textureVisibilityTransitionsBefore.push_back({
                    request.opaqueDepthTexture,
                    opaqueDepthRange,
                    NLS::Render::RHI::ResourceState::Unknown,
                    NLS::Render::RHI::ResourceState::ShaderRead,
                    NLS::Render::RHI::PipelineStageMask::AllCommands,
                    NLS::Render::RHI::PipelineStageMask::ComputeShader,
                    NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
                    NLS::Render::RHI::AccessMask::ShaderRead
                });
            }
            else
            {
                const auto previousMipRange = makeRange(mip - 1u, 1u);
                buildHZB.textureResourceAccesses.push_back({
                    request.hzbTexture,
                    previousMipRange,
                    NLS::Render::Context::ResourceAccessMode::Read,
                    NLS::Render::RHI::ResourceState::ShaderRead,
                    NLS::Render::RHI::PipelineStageMask::ComputeShader,
                    NLS::Render::RHI::AccessMask::ShaderRead
                });
                buildHZB.textureVisibilityTransitionsBefore.push_back({
                    request.hzbTexture,
                    previousMipRange,
                    NLS::Render::RHI::ResourceState::ShaderWrite,
                    NLS::Render::RHI::ResourceState::ShaderRead,
                    NLS::Render::RHI::PipelineStageMask::ComputeShader,
                    NLS::Render::RHI::PipelineStageMask::ComputeShader,
                    NLS::Render::RHI::AccessMask::ShaderWrite,
                    NLS::Render::RHI::AccessMask::ShaderRead
                });
            }
            const auto mipRange = makeRange(mip, 1u);
            buildHZB.textureResourceAccesses.push_back({
                request.hzbTexture,
                mipRange,
                NLS::Render::Context::ResourceAccessMode::Write,
                NLS::Render::RHI::ResourceState::ShaderWrite,
                NLS::Render::RHI::PipelineStageMask::ComputeShader,
                NLS::Render::RHI::AccessMask::ShaderWrite
            });
            buildHZB.textureVisibilityTransitionsBefore.push_back({
                request.hzbTexture,
                mipRange,
                NLS::Render::RHI::ResourceState::Unknown,
                NLS::Render::RHI::ResourceState::ShaderWrite,
                NLS::Render::RHI::PipelineStageMask::AllCommands,
                NLS::Render::RHI::PipelineStageMask::ComputeShader,
                NLS::Render::RHI::AccessMask::MemoryRead | NLS::Render::RHI::AccessMask::MemoryWrite,
                NLS::Render::RHI::AccessMask::ShaderWrite
            });
            buildHZB.exportedTextureVisibilityTransitions.push_back({
                request.hzbTexture,
                mipRange,
                NLS::Render::RHI::ResourceState::ShaderWrite,
                NLS::Render::RHI::ResourceState::ShaderRead,
                NLS::Render::RHI::PipelineStageMask::ComputeShader,
                NLS::Render::RHI::PipelineStageMask::ComputeShader,
                NLS::Render::RHI::AccessMask::ShaderWrite,
                NLS::Render::RHI::AccessMask::ShaderRead
            });
            dispatches.push_back(std::move(buildHZB));
        }

        NLS::Render::Context::RecordedComputeDispatchInput occlusion;
        occlusion.debugName = "HZBOcclusion";
        occlusion.pipeline = request.occlusionPipeline;
        if (request.occlusionBindingSet != nullptr)
        {
            occlusion.bindingSets.push_back({
                NLS::Render::RHI::BindingPointMap::kPassDescriptorSet,
                request.occlusionBindingSet
            });
        }
        occlusion.groupCountX = request.occlusionGroupCounts[0];
        occlusion.groupCountY = request.occlusionGroupCounts[1];
        occlusion.groupCountZ = request.occlusionGroupCounts[2];
        const auto hzbReadRange = makeRange(0u, hzbMipCount);
        occlusion.textureResourceAccesses.push_back({
            request.hzbTexture,
            hzbReadRange,
            NLS::Render::Context::ResourceAccessMode::Read,
            NLS::Render::RHI::ResourceState::ShaderRead,
            NLS::Render::RHI::PipelineStageMask::ComputeShader,
            NLS::Render::RHI::AccessMask::ShaderRead
        });
        if (request.occlusionPrimitiveInputBuffer != nullptr)
        {
            const bool inputBufferIsCpuToGpu =
                request.occlusionPrimitiveInputBuffer->GetDesc().memoryUsage ==
                NLS::Render::RHI::MemoryUsage::CPUToGPU;
            const auto inputBufferState = inputBufferIsCpuToGpu
                ? NLS::Render::RHI::ResourceState::GenericRead
                : NLS::Render::RHI::ResourceState::ShaderRead;
            if (!inputBufferIsCpuToGpu)
                occlusion.shaderReadBuffersBefore.push_back(request.occlusionPrimitiveInputBuffer);
            occlusion.bufferResourceAccesses.push_back({
                request.occlusionPrimitiveInputBuffer,
                NLS::Render::Context::ResourceAccessMode::Read,
                inputBufferState,
                NLS::Render::RHI::PipelineStageMask::ComputeShader,
                NLS::Render::RHI::AccessMask::ShaderRead
            });
        }
        if (request.occlusionConstantsBuffer != nullptr)
        {
            occlusion.bufferResourceAccesses.push_back({
                request.occlusionConstantsBuffer,
                NLS::Render::Context::ResourceAccessMode::Read,
                NLS::Render::RHI::ResourceState::GenericRead,
                NLS::Render::RHI::PipelineStageMask::ComputeShader,
                NLS::Render::RHI::AccessMask::ShaderRead
            });
        }
        if (request.occlusionPrimitiveResultBuffer != nullptr)
        {
            occlusion.shaderWriteBuffersBefore.push_back(request.occlusionPrimitiveResultBuffer);
            occlusion.uavBarrierBuffersAfter.push_back(request.occlusionPrimitiveResultBuffer);
            occlusion.bufferResourceAccesses.push_back({
                request.occlusionPrimitiveResultBuffer,
                NLS::Render::Context::ResourceAccessMode::Write,
                NLS::Render::RHI::ResourceState::ShaderWrite,
                NLS::Render::RHI::PipelineStageMask::ComputeShader,
                NLS::Render::RHI::AccessMask::ShaderWrite
            });
        }
        dispatches.push_back(std::move(occlusion));
        return BuildPreparedComputeDispatchSource(std::move(dispatches));
    }

    DeferredPreparedSceneResources CaptureDeferredPreparedSceneResources(
        const DeferredPreparedSceneResourceRequest& request)
    {
        NLS_PROFILE_SCOPE();
        DeferredPreparedSceneResources resources;
        if (!HasCompleteDeferredPreparedSceneResourceRequest(request))
            return resources;

        const std::array<const NLS::Render::Resources::Texture2D*, kDeferredGBufferColorAttachmentCount> colorWrappers = {
            request.gbufferAlbedoTexture,
            request.gbufferNormalTexture,
            request.gbufferMaterialTexture
        };
        resources.gbufferColorViews.reserve(kDeferredGBufferColorAttachmentCount);
        for (size_t i = 0u; i < kDeferredGBufferColorAttachmentCount; ++i)
        {
            resources.gbufferColorViews.push_back(request.gBuffer->GetOrCreateExplicitColorView(
                static_cast<uint32_t>(i),
                kDeferredGBufferColorSlots[i].capturedViewName));
        }
        resources.gbufferDepthView = request.gBuffer->GetOrCreateExplicitDepthView(
            kDeferredGBufferDepthSlot.capturedViewName);

        resources.gbufferTextures.reserve(kDeferredGBufferTextureCount);
        for (const auto* texture : colorWrappers)
            resources.gbufferTextures.push_back(texture->GetExplicitRHITextureHandle());
        resources.gbufferTextures.push_back(request.gbufferDepthTexture->GetExplicitRHITextureHandle());
        if (!HasCompleteDeferredPreparedSceneResources(resources))
            return {};

        return resources;
    }

    DeferredGraphSceneResourceRequest BuildDeferredGraphSceneResourceRequest(
        ::FrameGraph& frameGraph,
        FrameGraphBlackboard& blackboard,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const DeferredPreparedSceneResourceRequest& preparedResources)
    {
        DeferredGraphSceneResourceRequest request;
        request.frameGraph = &frameGraph;
        request.blackboard = &blackboard;
        request.frameDescriptor = &frameDescriptor;
        request.preparedResources = preparedResources;
        return request;
    }

    std::array<DeferredScenePassDescriptor, 2> GetDeferredScenePassDescriptors()
    {
        return { {
            {
                {
                    NLS::Render::Context::RenderPassCommandKind::GBuffer,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Opaque,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "DeferredGBuffer",
                    NLS::Render::FrameGraph::kThreadedPlanUsePassDrawCount,
                    false,
                    false,
                    {
                        false,
                        true,
                        true,
                        true,
                        NLS::Maths::Vector4{ 0.0f, 0.0f, 0.0f, 1.0f }
                    }
                }
            },
            {
                {
                    NLS::Render::Context::RenderPassCommandKind::Lighting,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary,
                    NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Output,
                    NLS::Render::RHI::QueueType::Graphics,
                    NLS::Render::Context::QueueDependencyPolicy::Previous,
                    "DeferredLighting",
                    1u,
                    true,
                    true,
                    {
                        true,
                        false,
                        false,
                        false,
                        NLS::Maths::Vector4::Zero
                    }
                }
            }
        } };
    }

    void ReserveDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const DeferredGraphSceneResourceRequest& resourceRequest)
    {
        NLS_PROFILE_SCOPE();
        const bool hasExternalOutput =
            resourceRequest.frameDescriptor != nullptr &&
            ResolveExternalSceneOutputFramebuffer(*resourceRequest.frameDescriptor) != nullptr;
        frameGraph.reserve(4, hasExternalOutput ? 7 : 4);
    }

    PreparedDeferredSceneGraph PrepareDeferredSceneGraph(
        const DeferredGraphSceneResourceRequest& resourceRequest,
        const LightGridCompileContext& lightGridContext,
        const bool includeTransparentPass,
        const bool includeDecalPass)
    {
        NLS_PROFILE_SCOPE();
        PreparedDeferredSceneGraph preparedGraph;
        preparedGraph.resources = ImportDeferredSceneGraphResources(resourceRequest);
        if (!HasCompleteDeferredGraphGBufferResources(preparedGraph.resources))
            return preparedGraph;

        const auto scenePassDescriptors = BuildDeferredScenePassDescriptors(
            includeTransparentPass,
            includeDecalPass);
        const auto lightGridSource = Detail::BuildPreparedLightGridDispatchSource(lightGridContext);
        const auto hzbSource = BuildHZBPreparedComputeDispatchSource(resourceRequest.hzbResources);
        preparedGraph.execution.preparedComputeSource = MergePreparedComputeDispatchSources(
            lightGridSource,
            hzbSource);
        std::vector<ThreadedRenderScenePassMetadata> metadata;
        metadata.reserve(
            lightGridSource.metadata.size() +
            scenePassDescriptors.size() +
            hzbSource.metadata.size());
        metadata.insert(metadata.end(), lightGridSource.metadata.begin(), lightGridSource.metadata.end());
        if (scenePassDescriptors.size() > 0u)
            metadata.push_back(scenePassDescriptors.descriptors[0].metadata);
        if (scenePassDescriptors.size() > 1u &&
            scenePassDescriptors.descriptors[1].metadata.commandKind == NLS::Render::Context::RenderPassCommandKind::Decal)
        {
            metadata.push_back(scenePassDescriptors.descriptors[1].metadata);
        }
        metadata.insert(metadata.end(), hzbSource.metadata.begin(), hzbSource.metadata.end());
        for (size_t i = 1u; i < scenePassDescriptors.size(); ++i)
        {
            if (scenePassDescriptors.descriptors[i].metadata.commandKind != NLS::Render::Context::RenderPassCommandKind::Decal)
                metadata.push_back(scenePassDescriptors.descriptors[i].metadata);
        }
        PromotePreparedComputeConsumerDependencies(metadata);
        preparedGraph.execution.compiledExecution = CompileThreadedRenderSceneExecution(
            lightGridContext.frameDescriptor,
            preparedGraph.resources.sceneTargets.color,
            preparedGraph.resources.sceneTargets.depth,
            metadata);
        return preparedGraph;
    }

    void ExecutePreparedDeferredSceneGraph(
        ::FrameGraph& frameGraph,
        const PreparedDeferredSceneGraph& preparedGraph,
        const DeferredSceneGraphExecutionCallbacks& callbacks)
    {
        NLS_PROFILE_SCOPE();
        const auto capturedCallbacks = callbacks;
        const auto addCompiledGBufferPass = [&frameGraph, &preparedGraph, capturedCallbacks](
            const CompiledThreadedRenderSceneGraphPass& compiledPass) -> const DeferredGBufferGraphPassData&
        {
            return AddDeferredCompiledGBufferPass(
                frameGraph,
                compiledPass,
                preparedGraph.resources.gbufferAlbedo,
                preparedGraph.resources.gbufferNormal,
                preparedGraph.resources.gbufferMaterial,
                preparedGraph.resources.gbufferDepth,
                [capturedCallbacks](const DeferredGBufferGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
                {
                    ExecuteRecordedRenderPass(
                        desc,
                        [&capturedCallbacks](const auto& beginDesc) -> bool
                        {
                            return capturedCallbacks.beginGBufferPass ? capturedCallbacks.beginGBufferPass(beginDesc) : false;
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.executeGBufferPass)
                                capturedCallbacks.executeGBufferPass();
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.endGBufferPass)
                                capturedCallbacks.endGBufferPass();
                        });
                });
        };
        const auto addCompiledLightingPass = [&frameGraph, capturedCallbacks](
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            ScenePassOutputResourceState& outputState,
            const DeferredGBufferGraphPassData* gBufferPassData)
        {
            AddDeferredCompiledLightingPass(
                frameGraph,
                compiledPass,
                outputState,
                gBufferPassData,
                [capturedCallbacks](const DeferredLightingGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
                {
                    ExecuteOutputRenderPass(
                        desc,
                        [&capturedCallbacks](const auto& beginDesc) -> bool
                        {
                            return capturedCallbacks.beginLightingPass ? capturedCallbacks.beginLightingPass(beginDesc) : false;
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.executeLightingPass)
                                capturedCallbacks.executeLightingPass();
                        },
                        [&capturedCallbacks](bool startedRenderPass, const auto& endDesc)
                        {
                            if (capturedCallbacks.endLightingPass)
                                capturedCallbacks.endLightingPass(startedRenderPass, endDesc);
                        });
                });
        };
        const auto addCompiledDecalPass = [&frameGraph, capturedCallbacks](
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            const DeferredGBufferGraphPassData* gBufferPassData) -> const DeferredGBufferGraphPassData&
        {
            return AddDeferredCompiledDecalPass(
                frameGraph,
                compiledPass,
                gBufferPassData,
                [capturedCallbacks](const DeferredGBufferGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
                {
                    ExecuteRecordedRenderPass(
                        desc,
                        [&capturedCallbacks](const auto& beginDesc) -> bool
                        {
                            return capturedCallbacks.beginDecalPass ? capturedCallbacks.beginDecalPass(beginDesc) : false;
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.executeDecalPass)
                                capturedCallbacks.executeDecalPass();
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.endDecalPass)
                                capturedCallbacks.endDecalPass();
                        });
                });
        };
        const auto addCompiledTransparentPass = [&frameGraph, capturedCallbacks](
            const CompiledThreadedRenderSceneGraphPass& compiledPass,
            ScenePassOutputResourceState& outputState,
            const DeferredGBufferGraphPassData* gBufferPassData)
        {
            AddDeferredCompiledTransparentPass(
                frameGraph,
                compiledPass,
                outputState,
                gBufferPassData,
                [capturedCallbacks](const DeferredTransparentGraphPassData&, FrameGraphPassResources&, void*, const auto& desc)
                {
                    ExecuteOutputRenderPass(
                        desc,
                        [&capturedCallbacks](const auto& beginDesc) -> bool
                        {
                            return capturedCallbacks.beginTransparentPass ? capturedCallbacks.beginTransparentPass(beginDesc) : false;
                        },
                        [&capturedCallbacks]()
                        {
                            if (capturedCallbacks.executeTransparentPass)
                                capturedCallbacks.executeTransparentPass();
                        },
                        [&capturedCallbacks](bool startedRenderPass, const auto& endDesc)
                        {
                            if (capturedCallbacks.endTransparentPass)
                                capturedCallbacks.endTransparentPass(startedRenderPass, endDesc);
                        });
                });
        };
        DispatchDeferredCompiledGraphPasses(
            frameGraph,
            preparedGraph.execution.preparedComputeSource,
            preparedGraph.execution.compiledExecution.graphPasses,
            addCompiledGBufferPass,
            addCompiledDecalPass,
            addCompiledLightingPass,
            addCompiledTransparentPass);
    }

    CompiledThreadedRenderSceneExecution CompileAndApplyPreparedDeferredLightGridSceneExecution(
        NLS::Render::Context::RenderScenePackage& package,
        const LightGridCompileContext& lightGridContext,
        const DeferredPreparedSceneResources& resources,
        std::vector<NLS::Render::Context::RenderPassCommandInput>&& appendedPassInputs,
        const std::vector<ThreadedRenderScenePassMetadata>& appendedPassMetadata,
        const DeferredPreparedQueuedDrawCounts queuedDrawCounts,
        const PreparedComputeDispatchSource& hzbSource)
    {
        NLS_PROFILE_SCOPE();
        const auto opaqueDrawCount = package.opaqueDrawCount;
        const auto decalDrawCount = ResolveDeferredDecalDrawCount(
            package,
            opaqueDrawCount,
            queuedDrawCounts.decalDrawCount);
        const auto lightingDrawCount = ResolveDeferredLightingDrawCount(
            package,
            opaqueDrawCount,
            decalDrawCount,
            queuedDrawCounts.lightingDrawCount);
        const bool canSlicePostLightingRecordedDraws = true;
        const auto transparentDrawCount = ResolveDeferredTransparentDrawCount(
            package,
            opaqueDrawCount,
            decalDrawCount,
            lightingDrawCount,
            queuedDrawCounts.transparentDrawCount,
            canSlicePostLightingRecordedDraws);
        const bool canSliceHelperDraws = std::any_of(
            appendedPassMetadata.begin(),
            appendedPassMetadata.end(),
            [](const ThreadedRenderScenePassMetadata& metadata)
            {
                return metadata.commandKind == NLS::Render::Context::RenderPassCommandKind::Helper;
            });
        if (HasCompleteDeferredPreparedSceneResources(resources, lightGridContext.frameDescriptor))
        {
            ValidateDeferredRecordedDrawSliceBoundaries(
                package,
                opaqueDrawCount,
                decalDrawCount,
                lightingDrawCount,
                transparentDrawCount,
                queuedDrawCounts,
                canSliceHelperDraws);
        }
        const auto lightGridSource = Detail::BuildPreparedLightGridDispatchSource(lightGridContext);
        const auto mergedComputeSource = MergePreparedComputeDispatchSources(lightGridSource, hzbSource);
        const auto deferredPassDescriptors = BuildDeferredScenePassDescriptors(
            transparentDrawCount > 0u,
            decalDrawCount > 0u);
        std::vector<ThreadedRenderScenePassMetadata> scenePassMetadata;
        scenePassMetadata.reserve(
            lightGridSource.metadata.size() +
            deferredPassDescriptors.size() +
            hzbSource.metadata.size() +
            appendedPassMetadata.size());
        scenePassMetadata.insert(scenePassMetadata.end(), lightGridSource.metadata.begin(), lightGridSource.metadata.end());
        if (deferredPassDescriptors.size() > 0u)
            scenePassMetadata.push_back(deferredPassDescriptors.descriptors[0].metadata);
        if (deferredPassDescriptors.size() > 1u &&
            deferredPassDescriptors.descriptors[1].metadata.commandKind == NLS::Render::Context::RenderPassCommandKind::Decal)
        {
            scenePassMetadata.push_back(deferredPassDescriptors.descriptors[1].metadata);
        }
        scenePassMetadata.insert(scenePassMetadata.end(), hzbSource.metadata.begin(), hzbSource.metadata.end());
        for (size_t i = 1u; i < deferredPassDescriptors.size(); ++i)
        {
            if (deferredPassDescriptors.descriptors[i].metadata.commandKind != NLS::Render::Context::RenderPassCommandKind::Decal)
                scenePassMetadata.push_back(deferredPassDescriptors.descriptors[i].metadata);
        }
        PromotePreparedComputeConsumerDependencies(scenePassMetadata);
        const auto appendedMetadataBegin = scenePassMetadata.size();
        scenePassMetadata.insert(
            scenePassMetadata.end(),
            appendedPassMetadata.begin(),
            appendedPassMetadata.end());
        scenePassMetadata.erase(
            std::remove_if(
                scenePassMetadata.begin() + static_cast<std::ptrdiff_t>(appendedMetadataBegin),
                scenePassMetadata.end(),
                [](const ThreadedRenderScenePassMetadata& metadata)
                {
                    return IsDeferredScenePassKind(metadata.commandKind);
                }),
            scenePassMetadata.end());

        Detail::ResolvePreparedLightGridPassBindings(package, lightGridContext);
        return CompileAndApplyThreadedRenderSceneExecution(
            package,
            lightGridContext.frameDescriptor,
            -1,
            -1,
            scenePassMetadata,
            [&package, &resources, &appendedPassInputs, &appendedPassMetadata, &mergedComputeSource, &lightGridContext, opaqueDrawCount, decalDrawCount, lightingDrawCount, transparentDrawCount, canSlicePostLightingRecordedDraws](const auto& compiledPasses) mutable
            {
                std::vector<CompiledThreadedRenderSceneGraphPass> deferredCompiledPasses;
                deferredCompiledPasses.reserve(compiledPasses.size());
                for (const auto& compiledPass : compiledPasses)
                {
                    if (FindPreparedComputeDispatchByName(
                            mergedComputeSource,
                            compiledPass.metadata.graphPassName) != nullptr ||
                        GetDeferredScenePassExecutionKind(compiledPass.metadata.commandKind) != DeferredScenePassExecutionKind::Unknown)
                    {
                        deferredCompiledPasses.push_back(compiledPass);
                    }
                }
                const bool hasValidDeferredResources = HasCompleteDeferredPreparedSceneResources(
                    resources,
                    lightGridContext.frameDescriptor);
                auto passInputs = BuildDeferredScenePassInputs(
                    deferredCompiledPasses,
                    package,
                    mergedComputeSource,
                    lightGridContext.frameDescriptor,
                    opaqueDrawCount,
                    decalDrawCount,
                    lightingDrawCount,
                    transparentDrawCount,
                    resources);
                const auto aggregateHelperPassInput =
                    BuildDeferredAggregateHelperPassInput(
                        package,
                        opaqueDrawCount,
                        decalDrawCount,
                        lightingDrawCount,
                        transparentDrawCount,
                        canSlicePostLightingRecordedDraws);
                for (const auto& metadata : appendedPassMetadata)
                {
                    AppendDeferredHelperPassInputForMetadata(
                        passInputs,
                        metadata,
                        appendedPassInputs,
                        aggregateHelperPassInput);
                }
                if (hasValidDeferredResources)
                {
                    for (auto& passInput : passInputs)
                        AttachDeferredGBufferDepthForPostLightingPasses(passInput, resources);
                }
                if (!package.targetsSwapchain)
                {
                    ApplyExternalSceneOutputAttachments(
                        passInputs,
                        ResolveExternalSceneOutputAttachments(
                            lightGridContext.frameDescriptor,
                            "DeferredOutputColorView",
                            "DeferredOutputDepthView"),
                        {
                            NLS::Render::Context::RenderPassCommandKind::Lighting,
                            NLS::Render::Context::RenderPassCommandKind::Transparent,
                            NLS::Render::Context::RenderPassCommandKind::Helper
                        });
                    for (auto& passInput : passInputs)
                        AddColorAttachmentWriteResourceAccesses(passInput);
                }
                return passInputs;
            });
    }

    void FinalizePreparedDeferredScenePackage(
        NLS::Render::Context::RenderScenePackage& package,
        const NLS::Render::Data::FrameDescriptor& frameDescriptor)
    {
        NLS_PROFILE_SCOPE();
        ApplyExternalSceneOutputAttachments(
            package,
            frameDescriptor,
            "DeferredOutputColorView",
            "DeferredOutputDepthView",
            {
                NLS::Render::Context::RenderPassCommandKind::Lighting,
                NLS::Render::Context::RenderPassCommandKind::Transparent
            });
        RegisterExternalSceneOutputExtractions(package, frameDescriptor);
    }
}
