#pragma once

#include <optional>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHICommand.h"

namespace NLS::Render::RHI
{
    struct NLS_RENDER_API TrackedBufferState
    {
        std::shared_ptr<RHIBuffer> buffer;
        ResourceState state = ResourceState::Unknown;
        PipelineStageMask stageMask = PipelineStageMask::None;
        AccessMask accessMask = AccessMask::None;
    };

    struct NLS_RENDER_API TrackedTextureState
    {
        std::shared_ptr<RHITexture> texture;
        RHISubresourceRange subresourceRange{};
        ResourceState state = ResourceState::Unknown;
        PipelineStageMask stageMask = PipelineStageMask::None;
        AccessMask accessMask = AccessMask::None;
    };

    class NLS_RENDER_API ResourceStateTracker
    {
    public:
        virtual ~ResourceStateTracker() = default;
        virtual void Reset() = 0;
        virtual std::optional<TrackedBufferState> GetBufferState(const std::shared_ptr<RHIBuffer>& buffer) const = 0;
        virtual std::optional<TrackedTextureState> GetTextureState(
            const std::shared_ptr<RHITexture>& texture,
            const RHISubresourceRange& subresourceRange) const = 0;
        virtual RHIBarrierDesc BuildTransitionBarriers(
            const std::vector<RHIBufferBarrier>& bufferBarriers,
            const std::vector<RHITextureBarrier>& textureBarriers) const = 0;
        virtual void Commit(const RHIBarrierDesc& barriers) = 0;
    };

    NLS_RENDER_API std::shared_ptr<ResourceStateTracker> CreateDefaultResourceStateTracker();
}
