#pragma once

#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Settings/ERenderPassOrder.h"
#include "RenderDef.h"

namespace NLS::Render::Core
{
class CompositeRenderer;

/**
 * Represents a rendering step in the graphics pipeline.
 * Subclasses of this class define specific rendering passes.
 */
class NLS_RENDER_API ARenderPass
{
public:
    using PipelineState = Data::PipelineState;

    ARenderPass(Core::CompositeRenderer& p_renderer);
    virtual ~ARenderPass() = default;

    void SetEnabled(bool p_enabled);
    bool IsEnabled() const;
    virtual bool RequiresLegacyExecution() const;
    virtual bool BlocksExplicitRecording() const;

protected:
    virtual void OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor);
    virtual void OnEndFrame();
    virtual void Draw(PipelineState p_pso) = 0;

protected:
    Core::CompositeRenderer& m_renderer;
    bool m_enabled = true;

    friend class Core::CompositeRenderer;
};
}
