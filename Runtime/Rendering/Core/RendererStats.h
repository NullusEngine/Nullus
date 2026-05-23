#pragma once

#include "Rendering/Data/FrameInfo.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

namespace NLS::Render::Context
{
    struct ThreadedFrameTelemetry;
}

namespace NLS::Render::Core
{
class NLS_RENDER_API RendererStats
{
public:
    void BeginFrame();
    void EndFrame();
    void RecordSubmittedDraw(const Entities::Drawable& drawable, uint32_t instanceCount);
    void RecordSceneParse(uint64_t opaqueCount, uint64_t transparentCount, uint64_t skyboxCount);
    void RecordGBufferMaterialSync();
    void RecordRenderBindingSetCreation(uint64_t count = 1u);
    void RecordRenderSnapshotBufferCreation(uint64_t count = 1u);
    void SetThreadedFrameTelemetry(const NLS::Render::Context::ThreadedFrameTelemetry& telemetry);

    const Data::FrameInfo& GetFrameInfo() const;
    bool IsFrameInfoValid() const;

private:
    bool m_isFrameInfoValid = true;
    Data::FrameInfo m_frameInfo;
};
}
