#pragma once

#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

namespace NLS::Render::Core
{
class CompositeRenderer;
}

namespace NLS::Render::Features
{
/**
 * Extension of the composite renderer that provides new rendering capabilities
 */
class NLS_RENDER_API ARenderFeature
{
public:
    using PipelineState = Data::PipelineState;

    ARenderFeature(Core::CompositeRenderer& p_renderer);
    virtual ~ARenderFeature() = default;

    void SetEnabled(bool p_enabled);
    bool IsEnabled() const;

protected:
    virtual void OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor);
    virtual void OnEndFrame();
    virtual void OnBeforeDraw(PipelineState& p_pso, const Entities::Drawable& p_drawable);
    virtual void OnAfterDraw(const Entities::Drawable& p_drawable);

protected:
    Core::CompositeRenderer& m_renderer;
    bool m_enabled = true;

    friend class Core::CompositeRenderer;
};
}
