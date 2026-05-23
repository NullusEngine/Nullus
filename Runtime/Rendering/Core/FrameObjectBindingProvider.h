#pragma once

#include <cstdint>
#include <memory>

#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

namespace NLS::Render::RHI
{
    class RHICommandBuffer;
    class RHIBindingSet;
}

namespace NLS::Render::Core
{
class CompositeRenderer;

class NLS_RENDER_API FrameObjectBindingProvider
{
public:
    using PipelineState = Data::PipelineState;
    struct PreparedBindingSets
    {
        std::shared_ptr<RHI::RHIBindingSet> frameBindingSet;
        std::shared_ptr<RHI::RHIBindingSet> objectBindingSet;
        uint32_t objectIndex = 0u;
        bool usesObjectIndex = false;
    };

    explicit FrameObjectBindingProvider(CompositeRenderer& renderer);
    virtual ~FrameObjectBindingProvider() = default;

    void BeginFrame(const Data::FrameDescriptor& frameDescriptor);
    void EndFrame();
    bool PrepareDraw(PipelineState& pso, const Entities::Drawable& drawable);
    void PrepareExplicitDraw(RHI::RHICommandBuffer& commandBuffer, PipelineState& pso, const Entities::Drawable& drawable);
    bool CapturePreparedBindingSets(PipelineState& pso, const Entities::Drawable& drawable, PreparedBindingSets& outBindings);

    bool IsFramePrepared() const;
    bool IsObjectPrepared() const;
    uint64_t GetPreparedDrawCount() const;

protected:
    virtual void OnBeginFrame(const Data::FrameDescriptor& frameDescriptor);
    virtual void OnEndFrame();
    virtual bool OnPrepareDraw(PipelineState& pso, const Entities::Drawable& drawable);
    virtual void OnPrepareExplicitDraw(RHI::RHICommandBuffer& commandBuffer, PipelineState& pso, const Entities::Drawable& drawable);
    virtual bool OnCapturePreparedBindingSets(PipelineState& pso, const Entities::Drawable& drawable, PreparedBindingSets& outBindings);

    CompositeRenderer& m_renderer;

private:
    bool m_framePrepared = false;
    bool m_objectPrepared = false;
    uint64_t m_preparedDrawCount = 0u;
};
}
