#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>
#include <utility>

#include "Profiling/Profiler.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"

namespace NLS::Render::Core
{
namespace
{
    struct RenderPreparationCounterSnapshot
    {
        uint64_t uniformBindingSetCreationCount = 0u;
        uint64_t uniformSnapshotBufferCreationCount = 0u;
        uint64_t materialBindingSetCreationCount = 0u;
        uint64_t materialSnapshotBufferCreationCount = 0u;
    };

    RenderPreparationCounterSnapshot CaptureRenderPreparationCounterSnapshot(
        const CompositeRenderer& renderer,
        const Entities::Drawable& drawable)
    {
        RenderPreparationCounterSnapshot snapshot;
        snapshot.uniformBindingSetCreationCount = renderer.GetExplicitUniformBindingSetCreationCount();
        snapshot.uniformSnapshotBufferCreationCount = renderer.GetExplicitUniformSnapshotBufferCreationCount();
        if (drawable.material != nullptr)
        {
            snapshot.materialBindingSetCreationCount = drawable.material->GetExplicitBindingSetCreationCount();
            snapshot.materialSnapshotBufferCreationCount = drawable.material->GetExplicitSnapshotBufferCreationCount();
        }
        return snapshot;
    }

    std::pair<uint64_t, uint64_t> CalculateRenderPreparationCounterDelta(
        const CompositeRenderer& renderer,
        const Entities::Drawable& drawable,
        const RenderPreparationCounterSnapshot& snapshot)
    {
        const auto materialBindingSetDelta = drawable.material != nullptr
            ? drawable.material->GetExplicitBindingSetCreationCount() - snapshot.materialBindingSetCreationCount
            : 0u;
        const auto materialSnapshotBufferDelta = drawable.material != nullptr
            ? drawable.material->GetExplicitSnapshotBufferCreationCount() - snapshot.materialSnapshotBufferCreationCount
            : 0u;

        return {
            (renderer.GetExplicitUniformBindingSetCreationCount() - snapshot.uniformBindingSetCreationCount) +
                materialBindingSetDelta,
            (renderer.GetExplicitUniformSnapshotBufferCreationCount() - snapshot.uniformSnapshotBufferCreationCount) +
                materialSnapshotBufferDelta
        };
    }
}

CompositeRenderer::CompositeRenderer(Context::Driver& p_driver)
    : ABaseRenderer(p_driver)
{
}

void CompositeRenderer::BeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
    NLS_PROFILE_SCOPE();
    m_rendererStats.BeginFrame();
    ABaseRenderer::BeginFrame(p_frameDescriptor);
    m_compositeFrameActive = IsFrameActive();
    if (!m_compositeFrameActive)
        return;

    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->BeginFrame(p_frameDescriptor);

    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled())
        {
            pass.second->OnBeginFrame(p_frameDescriptor);
        }
    }
}

void CompositeRenderer::BeginFrameForBackgroundPreview(const Data::FrameDescriptor& p_frameDescriptor)
{
    NLS_PROFILE_SCOPE();
    m_rendererStats.BeginFrame();
    ABaseRenderer::BeginFrameForBackgroundPreview(p_frameDescriptor);
    m_compositeFrameActive = IsFrameActive();
    if (!m_compositeFrameActive)
        return;

    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->BeginFrame(p_frameDescriptor);

    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled())
        {
            pass.second->OnBeginFrame(p_frameDescriptor);
        }
    }
}

void CompositeRenderer::DrawFrame()
{
    NLS_PROFILE_SCOPE();
    if (!m_compositeFrameActive)
        return;

    DrawRegisteredPasses();
}

void CompositeRenderer::ExecutePass(Core::ARenderPass& pass)
{
    if (!m_compositeFrameActive)
        return;

    ExecutePass(pass, CreatePipelineState());
}

void CompositeRenderer::ExecutePass(Core::ARenderPass& pass, PipelineState pso)
{
    if (!m_compositeFrameActive)
        return;

    std::string passName = "CompositeRenderPass";
    for (const auto& [_, registeredPass] : m_passes)
    {
        if (registeredPass.second.get() == &pass)
        {
            passName = registeredPass.first;
            break;
        }
    }
    NLS_PROFILE_NAMED_SCOPE(passName.c_str());

    const auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer != nullptr && !pass.ManagesOwnRenderPass())
    {
        const bool startedRenderPass = BeginOutputRenderPass(
            m_frameDescriptor.renderWidth,
            m_frameDescriptor.renderHeight,
            false,
            false,
            false);
        if (!startedRenderPass)
            return;

        pass.Draw(pso);

        EndOutputRenderPass(startedRenderPass);

        return;
    }

    pass.Draw(pso);
}

void CompositeRenderer::DrawRegisteredPasses()
{
    NLS_PROFILE_SCOPE();
    if (!m_compositeFrameActive)
        return;

    DrawRegisteredPasses(CreatePipelineState());
}

void CompositeRenderer::DrawRegisteredPasses(PipelineState pso)
{
    NLS_PROFILE_SCOPE();
    if (!m_compositeFrameActive)
        return;

    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled())
            ExecutePass(*pass.second, pso);
    }
}

void CompositeRenderer::EndFrame()
{
    NLS_PROFILE_SCOPE();
    if (!m_compositeFrameActive)
    {
        ABaseRenderer::EndFrame();
        ClearDescriptors();
        m_compositeFrameActive = false;
        return;
    }

    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled())
        {
            pass.second->OnEndFrame();
        }
    }

    if (m_debugDrawService != nullptr)
        m_debugDrawService->EndFrame();
    ABaseRenderer::EndFrame();
    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->EndFrame();
    ClearDescriptors();
    if (Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver))
    {
        if (const auto telemetry = Context::DriverRendererAccess::TryGetThreadedFrameTelemetry(m_driver);
            telemetry.has_value())
        {
            m_rendererStats.SetThreadedFrameTelemetry(telemetry.value());
        }
        else
        {
            m_rendererStats.ReuseLastThreadedFrameTelemetry();
        }
    }
    m_rendererStats.EndFrame();
    m_compositeFrameActive = false;
}

RendererStats* CompositeRenderer::GetMutableRendererStats() const
{
    return const_cast<RendererStats*>(&m_rendererStats);
}

void CompositeRenderer::OnThreadedFramePublishFailed()
{
    ABaseRenderer::OnThreadedFramePublishFailed();
    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->ReleaseReservedPreparedFrameResources();
}

void CompositeRenderer::DrawEntity(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable,
    std::string_view lightMode
)
{
    NLS_PROFILE_SCOPE();
    if (!m_compositeFrameActive)
        return;

    const bool usesThreadedRendering = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

    const bool frameObjectPrepared = m_frameObjectBindingProvider == nullptr ||
        m_frameObjectBindingProvider->PrepareDraw(p_pso, p_drawable);

    const auto renderPreparationCounterSnapshot = CaptureRenderPreparationCounterSnapshot(*this, p_drawable);
    const auto recordRenderPreparationCounters =
        [&]()
        {
            const auto [bindingSetCreationCount, snapshotBufferCreationCount] =
                CalculateRenderPreparationCounterDelta(*this, p_drawable, renderPreparationCounterSnapshot);
            m_rendererStats.RecordRenderBindingSetCreation(bindingSetCreationCount);
            m_rendererStats.RecordRenderSnapshotBufferCreation(snapshotBufferCreationCount);
    };

    if (!frameObjectPrepared)
    {
        recordRenderPreparationCounters();
        return;
    }

    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_pso, p_drawable, lightMode, preparedDraw))
    {
        recordRenderPreparationCounters();
        return;
    }

    if (preparedDraw.commandBuffer == nullptr && usesThreadedRendering && m_frameObjectBindingProvider != nullptr)
    {
        FrameObjectBindingProvider::PreparedBindingSets bindingSets;
        if (m_frameObjectBindingProvider->CapturePreparedBindingSets(p_pso, p_drawable, bindingSets))
        {
            preparedDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
            preparedDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
            preparedDraw.objectConstants = bindingSets.objectConstants;
            preparedDraw.usesObjectIndex = bindingSets.usesObjectIndex;
        }
    }

    if (preparedDraw.commandBuffer == nullptr)
    {
        recordRenderPreparationCounters();
        if (usesThreadedRendering && QueueThreadedRecordedDraw(preparedDraw))
            m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
        return;
    }

    BindPreparedGraphicsPipeline(preparedDraw);
    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->PrepareExplicitDraw(*preparedDraw.commandBuffer, p_pso, p_drawable);

    recordRenderPreparationCounters();
    BindPreparedMaterialBindingSet(preparedDraw);
    SubmitPreparedDraw(preparedDraw);
    m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
}

void CompositeRenderer::DrawEntity(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride,
    std::string_view lightMode)
{
    NLS_PROFILE_SCOPE();
    if (!m_compositeFrameActive)
        return;

    auto effectivePso = CreatePipelineState();
    const bool usesThreadedRendering = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

    const bool frameObjectPrepared = m_frameObjectBindingProvider == nullptr ||
        m_frameObjectBindingProvider->PrepareDraw(effectivePso, p_drawable);

    const auto renderPreparationCounterSnapshot = CaptureRenderPreparationCounterSnapshot(*this, p_drawable);
    const auto recordRenderPreparationCounters =
        [&]()
        {
            const auto [bindingSetCreationCount, snapshotBufferCreationCount] =
                CalculateRenderPreparationCounterDelta(*this, p_drawable, renderPreparationCounterSnapshot);
            m_rendererStats.RecordRenderBindingSetCreation(bindingSetCreationCount);
            m_rendererStats.RecordRenderSnapshotBufferCreation(snapshotBufferCreationCount);
    };

    if (!frameObjectPrepared)
    {
        recordRenderPreparationCounters();
        return;
    }

    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_drawable, pipelineOverrides, depthCompareOverride, lightMode, preparedDraw))
    {
        recordRenderPreparationCounters();
        return;
    }

    if (preparedDraw.commandBuffer == nullptr && usesThreadedRendering && m_frameObjectBindingProvider != nullptr)
    {
        FrameObjectBindingProvider::PreparedBindingSets bindingSets;
        if (m_frameObjectBindingProvider->CapturePreparedBindingSets(effectivePso, p_drawable, bindingSets))
        {
            preparedDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
            preparedDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
            preparedDraw.objectConstants = bindingSets.objectConstants;
            preparedDraw.usesObjectIndex = bindingSets.usesObjectIndex;
        }
    }

    if (preparedDraw.commandBuffer == nullptr)
    {
        recordRenderPreparationCounters();
        if (usesThreadedRendering && QueueThreadedRecordedDraw(preparedDraw))
            m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
        return;
    }

    BindPreparedGraphicsPipeline(preparedDraw);
    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->PrepareExplicitDraw(*preparedDraw.commandBuffer, effectivePso, p_drawable);

    recordRenderPreparationCounters();
    BindPreparedMaterialBindingSet(preparedDraw);
    SubmitPreparedDraw(preparedDraw);
    m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
}

bool CompositeRenderer::CaptureRecordedDrawCommand(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable,
    Context::RecordedDrawCommandInput& outDraw,
    std::string_view lightMode)
{
    if (!m_compositeFrameActive)
        return false;

    const auto populateRecordedDrawCommandInput =
        [&outDraw](const PreparedRecordedDraw& preparedDraw)
    {
        if (preparedDraw.pipeline == nullptr ||
            preparedDraw.materialBindingSet == nullptr ||
            preparedDraw.mesh == nullptr ||
            preparedDraw.instanceCount == 0u)
        {
            return false;
        }

        outDraw.pipeline = preparedDraw.pipeline;
        outDraw.frameBindingSet = preparedDraw.frameBindingSet;
        outDraw.objectBindingSet = preparedDraw.objectBindingSet;
        outDraw.passBindingSet = preparedDraw.passBindingSet;
        outDraw.materialBindingSet = preparedDraw.materialBindingSet;
        outDraw.mesh = preparedDraw.mesh;
        outDraw.instanceCount = preparedDraw.instanceCount;
        outDraw.vertexStart = preparedDraw.vertexStart;
        outDraw.vertexCount = preparedDraw.vertexCount;
        outDraw.objectConstants = preparedDraw.objectConstants;
        outDraw.usesObjectIndex = preparedDraw.usesObjectIndex;
        return true;
    };

    const bool usesThreadedRendering = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

    if (m_frameObjectBindingProvider != nullptr &&
        !m_frameObjectBindingProvider->PrepareDraw(p_pso, p_drawable))
    {
        return false;
    }

    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_pso, p_drawable, lightMode, preparedDraw))
        return false;

    if (preparedDraw.commandBuffer == nullptr && usesThreadedRendering && m_frameObjectBindingProvider != nullptr)
    {
        FrameObjectBindingProvider::PreparedBindingSets bindingSets;
        if (m_frameObjectBindingProvider->CapturePreparedBindingSets(p_pso, p_drawable, bindingSets))
        {
            preparedDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
            preparedDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
            preparedDraw.objectConstants = bindingSets.objectConstants;
            preparedDraw.usesObjectIndex = bindingSets.usesObjectIndex;
        }
    }

    return populateRecordedDrawCommandInput(preparedDraw);
}

bool CompositeRenderer::CaptureRecordedDrawCommand(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride,
    Context::RecordedDrawCommandInput& outDraw)
{
    return CaptureRecordedDrawCommand(
        p_drawable,
        std::move(pipelineOverrides),
        depthCompareOverride,
        "Forward",
        outDraw);
}

bool CompositeRenderer::CaptureRecordedDrawCommand(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride,
    std::string_view lightMode,
    Context::RecordedDrawCommandInput& outDraw)
{
    if (!m_compositeFrameActive)
        return false;

    const auto populateRecordedDrawCommandInput =
        [&outDraw](const PreparedRecordedDraw& preparedDraw)
    {
        if (preparedDraw.pipeline == nullptr ||
            preparedDraw.materialBindingSet == nullptr ||
            preparedDraw.mesh == nullptr ||
            preparedDraw.instanceCount == 0u)
        {
            return false;
        }

        outDraw.pipeline = preparedDraw.pipeline;
        outDraw.frameBindingSet = preparedDraw.frameBindingSet;
        outDraw.objectBindingSet = preparedDraw.objectBindingSet;
        outDraw.passBindingSet = preparedDraw.passBindingSet;
        outDraw.materialBindingSet = preparedDraw.materialBindingSet;
        outDraw.mesh = preparedDraw.mesh;
        outDraw.instanceCount = preparedDraw.instanceCount;
        outDraw.vertexStart = preparedDraw.vertexStart;
        outDraw.vertexCount = preparedDraw.vertexCount;
        outDraw.objectConstants = preparedDraw.objectConstants;
        outDraw.usesObjectIndex = preparedDraw.usesObjectIndex;
        return true;
    };

    auto effectivePso = CreatePipelineState();
    const bool usesThreadedRendering = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

    if (m_frameObjectBindingProvider != nullptr &&
        !m_frameObjectBindingProvider->PrepareDraw(effectivePso, p_drawable))
    {
        return false;
    }

    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_drawable, pipelineOverrides, depthCompareOverride, lightMode, preparedDraw))
        return false;

    if (preparedDraw.commandBuffer == nullptr && usesThreadedRendering && m_frameObjectBindingProvider != nullptr)
    {
        FrameObjectBindingProvider::PreparedBindingSets bindingSets;
        if (m_frameObjectBindingProvider->CapturePreparedBindingSets(effectivePso, p_drawable, bindingSets))
        {
            preparedDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
            preparedDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
            preparedDraw.objectConstants = bindingSets.objectConstants;
            preparedDraw.usesObjectIndex = bindingSets.usesObjectIndex;
        }
    }

    return populateRecordedDrawCommandInput(preparedDraw);
}

void CompositeRenderer::SetFrameObjectBindingProvider(std::unique_ptr<FrameObjectBindingProvider> provider)
{
    NLS_ASSERT(!m_isDrawing, "You cannot swap the frame/object binding provider while drawing.");
    m_frameObjectBindingProvider = std::move(provider);
}

FrameObjectBindingProvider* CompositeRenderer::GetFrameObjectBindingProvider() const
{
    return m_frameObjectBindingProvider.get();
}

bool CompositeRenderer::HasFrameObjectBindingProvider() const
{
    return m_frameObjectBindingProvider != nullptr;
}

void CompositeRenderer::SetDebugDrawService(std::unique_ptr<Debug::DebugDrawService> service)
{
    NLS_ASSERT(!m_isDrawing, "You cannot swap the debug draw service while drawing.");
    m_debugDrawService = std::move(service);
}

Debug::DebugDrawService* CompositeRenderer::GetDebugDrawService() const
{
    return m_debugDrawService.get();
}

bool CompositeRenderer::HasDebugDrawService() const
{
    return m_debugDrawService != nullptr;
}

const Data::FrameInfo& CompositeRenderer::GetFrameInfo() const
{
    return m_rendererStats.GetFrameInfo();
}

bool CompositeRenderer::IsFrameInfoValid() const
{
    return m_rendererStats.IsFrameInfoValid();
}

void CompositeRenderer::ResetFrameStatistics()
{
    m_rendererStats.BeginFrame();
}

void CompositeRenderer::FinalizeFrameStatistics()
{
    m_rendererStats.EndFrame();
}

void CompositeRenderer::RecordPickingDiagnostics(const Data::PickingDiagnostics& diagnostics)
{
    m_rendererStats.RecordPickingDiagnostics(diagnostics);
}

}
