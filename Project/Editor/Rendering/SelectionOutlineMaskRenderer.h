#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <Math/Matrix4.h>
#include <Math/Vector4.h>
#include <Rendering/Buffers/MultiFramebuffer.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>
#include <Rendering/Data/FrameDescriptor.h>
#include <Rendering/Data/PipelineState.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Rendering/RHI/Core/RHIResource.h>
#include <Rendering/Resources/Material.h>

#include "Rendering/DebugGameObjectSelectionCollector.h"

namespace NLS::Render::Core { class CompositeRenderer; }
namespace NLS::Render::Resources
{
    class Mesh;
    class Texture2D;
}

namespace NLS::Editor::Rendering
{
    enum class SelectionOutlineFallbackReason : uint8_t
    {
#define NLS_SELECTION_OUTLINE_FALLBACK_REASON(name, action) name,
#include "Rendering/SelectionOutlineFallbackReasons.def"
#undef NLS_SELECTION_OUTLINE_FALLBACK_REASON
    };

    enum class SelectionOutlineFallbackAction : uint8_t
    {
        SkipFrame,
        LegacyShell
    };

    inline constexpr SelectionOutlineFallbackAction ResolveSelectionOutlineFallbackAction(
        const SelectionOutlineFallbackReason reason)
    {
        switch (reason)
        {
#define NLS_SELECTION_OUTLINE_FALLBACK_REASON(name, action) \
        case SelectionOutlineFallbackReason::name: \
            return SelectionOutlineFallbackAction::action;
#include "Rendering/SelectionOutlineFallbackReasons.def"
#undef NLS_SELECTION_OUTLINE_FALLBACK_REASON
        }

        return SelectionOutlineFallbackAction::SkipFrame;
    }

    inline constexpr const char* SelectionOutlineFallbackReasonToString(
        const SelectionOutlineFallbackReason reason)
    {
        switch (reason)
        {
#define NLS_SELECTION_OUTLINE_FALLBACK_REASON(name, action) \
        case SelectionOutlineFallbackReason::name: \
            return #name;
#include "Rendering/SelectionOutlineFallbackReasons.def"
#undef NLS_SELECTION_OUTLINE_FALLBACK_REASON
        }

        return "Unknown";
    }

    inline constexpr bool SelectionOutlineMaskSampleCountsAreSupported(
        const uint32_t outputSampleCount,
        const uint32_t depthSampleCount)
    {
        return outputSampleCount == 1u && depthSampleCount == outputSampleCount;
    }

    inline uint32_t SelectionOutlineMipExtent(const uint32_t extent, const uint32_t mipLevel)
    {
        const uint32_t shiftedExtent = mipLevel < 31u ? (extent >> mipLevel) : 0u;
        return shiftedExtent != 0u ? shiftedExtent : 1u;
    }

    inline bool SelectionOutlineTextureViewMatchesRenderExtent(
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>& view,
        const uint32_t renderWidth,
        const uint32_t renderHeight)
    {
        if (renderWidth == 0u || renderHeight == 0u || view == nullptr || view->GetTexture() == nullptr)
            return false;

        const auto& textureDesc = view->GetTexture()->GetDesc();
        const uint32_t mipLevel = view->GetDesc().subresourceRange.baseMipLevel;
        return SelectionOutlineMipExtent(textureDesc.extent.width, mipLevel) == renderWidth &&
            SelectionOutlineMipExtent(textureDesc.extent.height, mipLevel) == renderHeight;
    }

    inline bool SelectionOutlineFrameAttachmentsMatchRenderExtent(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>& sceneDepthViewOverride = nullptr)
    {
        const auto depthView = sceneDepthViewOverride != nullptr
            ? sceneDepthViewOverride
            : frameDescriptor.outputDepthStencilView;
        return SelectionOutlineTextureViewMatchesRenderExtent(
                frameDescriptor.outputColorView,
                frameDescriptor.renderWidth,
                frameDescriptor.renderHeight) &&
            SelectionOutlineTextureViewMatchesRenderExtent(
                depthView,
                frameDescriptor.renderWidth,
                frameDescriptor.renderHeight);
    }

    inline bool SelectionOutlineLegacyShellFallbackIsAttachmentCompatible(
        const NLS::Render::Data::FrameDescriptor& frameDescriptor,
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>& sceneDepthViewOverride = nullptr)
    {
        if (!SelectionOutlineFrameAttachmentsMatchRenderExtent(frameDescriptor, sceneDepthViewOverride))
            return false;

        const auto outputTexture = frameDescriptor.outputColorView != nullptr
            ? frameDescriptor.outputColorView->GetTexture()
            : nullptr;
        const auto depthView = sceneDepthViewOverride != nullptr
            ? sceneDepthViewOverride
            : frameDescriptor.outputDepthStencilView;
        const auto depthTexture = depthView != nullptr
            ? depthView->GetTexture()
            : nullptr;
        if (outputTexture == nullptr || depthTexture == nullptr)
            return false;

        return SelectionOutlineMaskSampleCountsAreSupported(
            outputTexture->GetDesc().sampleCount,
            depthTexture->GetDesc().sampleCount);
    }

    struct SelectionOutlineFallbackDecision
    {
        SelectionOutlineFallbackReason reason = SelectionOutlineFallbackReason::None;
        uint64_t selectedItemCount = 0u;
    };

    enum class SelectionOutlineMaskPassKind : uint8_t
    {
        CaptureMask = 0,
        Composite,
        Count
    };

    inline constexpr size_t SelectionOutlineMaskPassKindCount =
        static_cast<size_t>(SelectionOutlineMaskPassKind::Count);

    inline constexpr bool SelectionOutlineMaskPassPropagatesColorOutput(
        const SelectionOutlineMaskPassKind kind)
    {
        return kind == SelectionOutlineMaskPassKind::Composite;
    }

    inline NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata BuildSelectionOutlineLegacyShellMetadata(
        const NLS::Render::Context::RenderPassCommandInput& passInput)
    {
        NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata entry;
        entry.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
        entry.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
        entry.queueType = NLS::Render::RHI::QueueType::Graphics;
        entry.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
        entry.visibleDrawCountContribution = passInput.drawCount;
        entry.propagatesColorOutput = true;
        entry.propagatesDepthOutput = true;
        NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(entry, passInput.debugName);
        return entry;
    }

    struct SelectionOutlinePreparedOutput
    {
        std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
        std::vector<SelectionOutlineMaskPassKind> passKinds;
        std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> metadata;
        std::optional<SelectionOutlineFallbackDecision> fallbackDecision;
        uint64_t selectedItemCount = 0u;

        bool HasScreenSpacePasses() const
        {
            return !passInputs.empty() &&
                !fallbackDecision.has_value() &&
                passInputs.size() == passKinds.size() &&
                passInputs.size() == metadata.size();
        }
    };

    enum class SelectionOutlineMaskChannel : uint8_t
    {
#define NLS_SELECTION_OUTLINE_MASK_CHANNEL(name, swizzle, index) name = index,
#include "Rendering/SelectionOutlineMaskChannels.def"
#undef NLS_SELECTION_OUTLINE_MASK_CHANNEL
    };

    struct SelectionOutlineMaskChannelDesc
    {
        const char* name = nullptr;
        char swizzle = 0;
        uint8_t index = 0u;
    };

    struct SelectionMaskCaptureGroup
    {
        NLS::Render::Resources::Mesh* mesh = nullptr;
        float selectionGroupId = 1.0f;
        float selectionClassification = kSelectionOutlineParentClassification;
        std::vector<Maths::Matrix4> instanceModelMatrices;
    };

    class SelectionOutlineMaskRenderer
    {
    public:
        explicit SelectionOutlineMaskRenderer(NLS::Render::Core::CompositeRenderer& renderer);

        static constexpr std::array<const char*, 2> kPassNames = {
            "SelectionOutlineMask::CaptureMask",
            "SelectionOutlineMask::Composite"
        };
        static_assert(kPassNames.size() == SelectionOutlineMaskPassKindCount,
            "Selection outline pass-name metadata must match SelectionOutlineMaskPassKind.");

        static std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> BuildMetadata(
            const std::vector<NLS::Render::Context::RenderPassCommandInput>& passInputs,
            const std::vector<SelectionOutlineMaskPassKind>& passKinds);

        SelectionOutlinePreparedOutput BuildPreparedOutput(
            const DebugGameObjectDebugDrawItems& debugDrawItems,
            const Maths::Vector4& parentColor,
            const Maths::Vector4& childColor,
            NLS::Render::Data::PipelineState basePipelineState,
            std::shared_ptr<NLS::Render::RHI::RHITextureView> sceneDepthViewOverride = nullptr);

        SelectionOutlineFallbackReason GetLastFallbackReason() const { return m_lastFallbackReason; }
        void CommitPendingCachedMask(uint64_t publishedFrameId);
        void DiscardPendingCachedMask();

    private:
        struct ResourceIdentity
        {
            uint32_t width = 0u;
            uint32_t height = 0u;
            uint32_t sceneDepthExtentWidth = 0u;
            uint32_t sceneDepthExtentHeight = 0u;
            uint32_t outputColorExtentWidth = 0u;
            uint32_t outputColorExtentHeight = 0u;
            NLS::Render::RHI::TextureFormat maskFormat = NLS::Render::RHI::TextureFormat::RGBA8;
            uint32_t sampleCount = 1u;
            uint32_t depthSampleCount = 1u;
            std::shared_ptr<NLS::Render::RHI::RHITexture> sceneDepthTexture;
            std::shared_ptr<NLS::Render::RHI::RHITexture> outputColorTexture;
            NLS::Render::RHI::RHITextureViewDesc sceneDepthViewDesc;
            NLS::Render::RHI::RHITextureViewDesc outputColorViewDesc;

            bool Matches(const ResourceIdentity& other) const;
        };

        struct PreparedResources
        {
            std::shared_ptr<NLS::Render::RHI::RHITextureView> maskView;
        };

        struct MaskReuseSignature
        {
            ResourceIdentity resourceIdentity;
            uint64_t selectedItemCount = 0u;
            uint64_t groupCount = 0u;
            uint64_t instanceCount = 0u;
            uint64_t selectedTreeHash = 0u;
            uint64_t meshContentRevisionHash = 0u;
            NLS::Render::Entities::Camera* camera = nullptr;
            Maths::Matrix4 viewMatrix = Maths::Matrix4::Identity;
            Maths::Matrix4 projectionMatrix = Maths::Matrix4::Identity;

            bool Matches(const MaskReuseSignature& other) const;
        };

        struct SelectionSourceSignature
        {
            uint64_t selectedItemCount = 0u;
            uint64_t selectionMeshItemCount = 0u;
            uint64_t cameraItemCount = 0u;
            bool hasCameraMesh = false;
            uint64_t cameraMeshContentRevision = 0u;
            uint64_t selectedTreeHash = 0u;
            uint64_t meshContentRevisionHash = 0u;

            bool Matches(const SelectionSourceSignature& other) const;
        };

        struct PreparedSelectionCaptureGroups
        {
            std::vector<SelectionMaskCaptureGroup> groups;
            std::optional<MaskReuseSignature> maskReuseSignature;
            uint64_t instanceCount = 0u;
        };

        bool PrepareResources(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            PreparedResources& outResources);
        void InvalidateSelectionOutlineTextureBindings();
        bool BindScreenSpaceMaterialTextures(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const PreparedResources& resources);
        void ResetResources();
        std::optional<ResourceIdentity> BuildResourceIdentity(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor) const;
        std::optional<SelectionSourceSignature> BuildSelectionSourceSignature(
            const DebugGameObjectDebugDrawItems& debugDrawItems,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            NLS::Render::Resources::Mesh* cameraMesh) const;
        PreparedSelectionCaptureGroups BuildPreparedSelectionCaptureGroups(
            const DebugGameObjectDebugDrawItems& debugDrawItems,
            NLS::Render::Resources::Mesh* cameraMesh) const;
        const PreparedSelectionCaptureGroups& ResolveSelectionCaptureGroups(
            const DebugGameObjectDebugDrawItems& debugDrawItems,
            const std::optional<SelectionSourceSignature>& sourceSignature,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            NLS::Render::Resources::Mesh* cameraMesh);
        std::optional<MaskReuseSignature> BuildMaskReuseSignature(
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const std::vector<SelectionMaskCaptureGroup>& groups,
            uint64_t selectedItemCount) const;
        bool CanReuseCachedMask(const std::optional<MaskReuseSignature>& signature) const;
        bool ShouldKeepCachedMaskRetirementTarget(const std::optional<MaskReuseSignature>& signature) const;
        void MarkCachedMaskPending(const std::optional<MaskReuseSignature>& signature);
        void InvalidateCachedMask();
        uint64_t GetLatestRetiredThreadedFrameId() const;
        uint64_t GetLatestFailedRetiredThreadedFrameId() const;
        NLS::Render::Context::RenderPassCommandInput BuildPassInput(
            SelectionOutlineMaskPassKind kind,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            const PreparedResources& resources,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>&& maskDrawCommands) const;
        void CaptureMaskDrawCommands(
            const std::vector<SelectionMaskCaptureGroup>& groups,
            uint64_t selectedItemCount,
            NLS::Render::Data::PipelineState pso,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
        void CaptureMaskDrawCommandsForGroups(
            const std::vector<SelectionMaskCaptureGroup>& groups,
            NLS::Render::Resources::Material& material,
            int passMode,
            NLS::Render::Data::PipelineState pso,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
        void EnsureFullscreenResources();
        void RecordFullscreenCommand(
            NLS::Render::Resources::Material& material,
            const NLS::Render::Data::FrameDescriptor& frameDescriptor,
            std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands) const;
        void SetFallbackReason(SelectionOutlineFallbackReason reason);

        NLS::Render::Core::CompositeRenderer& m_renderer;
        NLS::Render::Resources::Material m_maskVisibleMaterial;
        NLS::Render::Resources::Material m_maskOccludedMaterial;
        NLS::Render::Resources::Material m_compositeMaterial;
        std::unique_ptr<NLS::Render::Resources::Mesh> m_fullscreenQuad;
        NLS::Render::Buffers::MultiFramebuffer m_intermediateFramebuffer;
        std::unique_ptr<NLS::Render::Resources::Texture2D> m_maskTexture;
        std::optional<ResourceIdentity> m_resourceIdentity;
        std::optional<MaskReuseSignature> m_cachedMaskSignature;
        std::optional<MaskReuseSignature> m_pendingCachedMaskSignature;
        std::optional<SelectionSourceSignature> m_cachedSelectionSourceSignature;
        PreparedSelectionCaptureGroups m_cachedSelectionCaptureGroups;
        std::optional<MaskReuseSignature> m_cachedSelectionMaskReuseSignature;
        uint64_t m_cachedMaskRetirementTarget = 0u;
        bool m_keepCachedMaskRetirementTargetForPending = false;
        SelectionOutlineFallbackReason m_lastFallbackReason = SelectionOutlineFallbackReason::None;
    };

    inline constexpr bool SelectionOutlineMaskPassNamesAreComplete()
    {
        for (const auto* passName : SelectionOutlineMaskRenderer::kPassNames)
        {
            if (passName == nullptr || passName[0] == '\0')
                return false;
        }
        return true;
    }

    static_assert(SelectionOutlineMaskPassNamesAreComplete(),
        "Selection outline pass-name metadata entries must be explicit and non-empty.");

    inline constexpr bool SelectionOutlineMaskPassKindIsValid(
        const SelectionOutlineMaskPassKind kind)
    {
        return static_cast<size_t>(kind) < SelectionOutlineMaskPassKindCount;
    }

    inline constexpr const char* SelectionOutlineMaskPassName(
        const SelectionOutlineMaskPassKind kind)
    {
        const size_t index = static_cast<size_t>(kind);
        return SelectionOutlineMaskPassKindIsValid(kind)
            && SelectionOutlineMaskRenderer::kPassNames[index] != nullptr
            ? SelectionOutlineMaskRenderer::kPassNames[index]
            : "SelectionOutlineMask::Invalid";
    }
}
