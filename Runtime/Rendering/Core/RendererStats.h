#pragma once

#include <optional>

#include "Rendering/Data/FrameInfo.h"
#include "Rendering/Entities/Drawable.h"
#include "RenderDef.h"

namespace NLS::Render::Context
{
    struct ThreadedFrameTelemetry;
}

namespace NLS::Render::Data
{
    struct DrawCallOptimizationStats;
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
    void RecordGBufferMaterialResolve(bool hit);
    void RecordPreparedRecordedDrawStaticBaseCache(bool hit);
    void RecordRenderBindingSetCreation(uint64_t count = 1u);
    void RecordRenderSnapshotBufferCreation(uint64_t count = 1u);
    void RecordDrawCallOptimizationStats(const NLS::Render::Data::DrawCallOptimizationStats& stats);
    void RecordLargeSceneTelemetry(const NLS::Render::Data::LargeSceneTelemetry& telemetry);
    void RecordPickingDiagnostics(const NLS::Render::Data::PickingDiagnostics& diagnostics);
    void SetThreadedFrameTelemetry(const NLS::Render::Context::ThreadedFrameTelemetry& telemetry);
    bool ReuseLastThreadedFrameTelemetry();
    static void ApplyThreadedFrameTelemetry(
        const NLS::Render::Context::ThreadedFrameTelemetry& telemetry,
        Data::FrameInfo& frameInfo);

    const Data::FrameInfo& GetFrameInfo() const;
    bool IsFrameInfoValid() const;

private:
    bool m_isFrameInfoValid = true;
    Data::FrameInfo m_frameInfo;
    std::optional<Data::FrameInfo> m_lastThreadedFrameInfoTelemetry;
};
}
