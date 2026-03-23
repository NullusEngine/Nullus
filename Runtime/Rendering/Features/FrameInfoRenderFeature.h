#pragma once

#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Data/FrameInfo.h"
#include "RenderDef.h"

namespace NLS::Render::Features
{
class NLS_RENDER_API FrameInfoRenderFeature : public ARenderFeature
{
public:
    using FrameInfo = Data::FrameInfo;
    using Drawable = Entities::Drawable;

    FrameInfoRenderFeature(Core::CompositeRenderer& p_renderer);
    virtual ~FrameInfoRenderFeature();

    const FrameInfo& GetFrameInfo() const;

protected:
    virtual void OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor) override;
    virtual void OnEndFrame() override;
    virtual void OnAfterDraw(const Drawable& p_drawable) override;

private:
    bool m_isFrameInfoDataValid;
    FrameInfo m_frameInfo;
    NLS::ListenerID m_postDrawListener;
};
}
