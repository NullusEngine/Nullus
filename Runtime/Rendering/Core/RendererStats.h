#pragma once

#include <mutex>
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
    void RecordPreparedRecordedDrawStaticBaseFastPath(bool hit);
    void RecordObjectDataRevisionReuse(bool hit);
    void RecordObjectDataRevisionFallback(
        NLS::Render::Data::ObjectDataRevisionFallbackReason reason);
    void RecordObjectDataFastPath(bool hit);
    void RecordDrawCommandCopy(uint64_t count);
    void RecordDrawCommandMove(uint64_t count);
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

    /**
     * Returns a copy rather than a reference.
     *
     * Recording and reading legitimately happen on different threads: the
     * threaded prepared builder records draw and material telemetry on the render
     * thread while the editor panel reads the same counters on the game thread.
     * A reference into mutex-protected storage cannot be handed out safely, so the
     * accessors copy under the lock instead.
     */
    [[nodiscard]] Data::FrameInfo GetFrameInfo() const;
    [[nodiscard]] Data::FrameInfo GetCumulativeFrameInfo() const;
    bool IsFrameInfoValid() const;

private:
    // Guards every member below. All public methods lock for their whole body;
    // they are flat (none calls another public method), so a non-recursive mutex
    // is sufficient.
    mutable std::mutex m_statsMutex;
    bool m_isFrameInfoValid = true;
    Data::FrameInfo m_frameInfo;
    Data::FrameInfo m_cumulativeFrameInfo;
    std::optional<Data::FrameInfo> m_lastThreadedFrameInfoTelemetry;
};
}
