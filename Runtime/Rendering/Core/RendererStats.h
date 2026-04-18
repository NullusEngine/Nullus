#pragma once

#include "Rendering/Data/FrameInfo.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

namespace NLS::Render::Core
{
class NLS_RENDER_API RendererStats
{
public:
    void BeginFrame();
    void EndFrame();
    void RecordSubmittedDraw(const Entities::Drawable& drawable, uint32_t instanceCount);

    const Data::FrameInfo& GetFrameInfo() const;
    bool IsFrameInfoValid() const;

private:
    bool m_isFrameInfoValid = true;
    Data::FrameInfo m_frameInfo;
};
}
