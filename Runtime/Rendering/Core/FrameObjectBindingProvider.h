#pragma once

#include <cstdint>

#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

namespace NLS::Render::RHI
{
    class RHICommandBuffer;
}

namespace NLS::Render::Core
{
class CompositeRenderer;

class NLS_RENDER_API FrameObjectBindingProvider
{
public:
    using PipelineState = Data::PipelineState;

    explicit FrameObjectBindingProvider(CompositeRenderer& renderer);
    virtual ~FrameObjectBindingProvider() = default;

    void BeginFrame(const Data::FrameDescriptor& frameDescriptor);
    void EndFrame();
    void PrepareDraw(PipelineState& pso, const Entities::Drawable& drawable);
    void PrepareExplicitDraw(RHI::RHICommandBuffer& commandBuffer, PipelineState& pso, const Entities::Drawable& drawable);

    bool IsFramePrepared() const;
    bool IsObjectPrepared() const;
    uint64_t GetPreparedDrawCount() const;

protected:
    virtual void OnBeginFrame(const Data::FrameDescriptor& frameDescriptor);
    virtual void OnEndFrame();
    virtual void OnPrepareDraw(PipelineState& pso, const Entities::Drawable& drawable);
    virtual void OnPrepareExplicitDraw(RHI::RHICommandBuffer& commandBuffer, PipelineState& pso, const Entities::Drawable& drawable);

    CompositeRenderer& m_renderer;

private:
    bool m_framePrepared = false;
    bool m_objectPrepared = false;
    uint64_t m_preparedDrawCount = 0u;
};
}
