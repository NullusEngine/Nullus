#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>

#include "Profiling/Profiler.h"
#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"

namespace NLS::Render::Core
{
CompositeRenderer::CompositeRenderer(Context::Driver& p_driver)
    : ABaseRenderer(p_driver)
{
}

void CompositeRenderer::BeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
    NLS_PROFILE_SCOPE();
    ABaseRenderer::BeginFrame(p_frameDescriptor);
    m_rendererStats.BeginFrame();

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
    DrawRegisteredPasses();
}

void CompositeRenderer::ExecutePass(Core::ARenderPass& pass)
{
    ExecutePass(pass, CreatePipelineState());
}

void CompositeRenderer::ExecutePass(Core::ARenderPass& pass, PipelineState pso)
{
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
    DrawRegisteredPasses(CreatePipelineState());
}

void CompositeRenderer::DrawRegisteredPasses(PipelineState pso)
{
    NLS_PROFILE_SCOPE();
    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled())
            ExecutePass(*pass.second, pso);
    }
}

void CompositeRenderer::EndFrame()
{
    NLS_PROFILE_SCOPE();
    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled())
        {
            pass.second->OnEndFrame();
        }
    }

    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->EndFrame();
    if (m_debugDrawService != nullptr)
        m_debugDrawService->EndFrame();
    ABaseRenderer::EndFrame();
    ClearDescriptors();
    const auto telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(m_driver);
    m_rendererStats.SetThreadedFrameTelemetry(telemetry);
    m_rendererStats.EndFrame();
}

void CompositeRenderer::DrawEntity(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable
)
{
    NLS_PROFILE_SCOPE();
    const bool usesThreadedRendering = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->PrepareDraw(p_pso, p_drawable);

    PreparedRecordedDraw preparedDraw;
    if (PrepareRecordedDraw(p_pso, p_drawable, preparedDraw))
    {
        if (preparedDraw.commandBuffer == nullptr && usesThreadedRendering && m_frameObjectBindingProvider != nullptr)
        {
            FrameObjectBindingProvider::PreparedBindingSets bindingSets;
            if (m_frameObjectBindingProvider->CapturePreparedBindingSets(p_pso, p_drawable, bindingSets))
            {
                preparedDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
                preparedDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
            }
        }

        if (preparedDraw.commandBuffer == nullptr)
        {
            if (usesThreadedRendering && QueueThreadedRecordedDraw(preparedDraw))
                m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
            return;
        }

        BindPreparedGraphicsPipeline(preparedDraw);
        if (m_frameObjectBindingProvider != nullptr)
            m_frameObjectBindingProvider->PrepareExplicitDraw(*preparedDraw.commandBuffer, p_pso, p_drawable);

        BindPreparedMaterialBindingSet(preparedDraw);
        SubmitPreparedDraw(preparedDraw);
        m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
    }
}

void CompositeRenderer::DrawEntity(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride)
{
    NLS_PROFILE_SCOPE();
    auto effectivePso = CreatePipelineState();
    const bool usesThreadedRendering = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->PrepareDraw(effectivePso, p_drawable);

    PreparedRecordedDraw preparedDraw;
    if (PrepareRecordedDraw(p_drawable, pipelineOverrides, depthCompareOverride, preparedDraw))
    {
        if (preparedDraw.commandBuffer == nullptr && usesThreadedRendering && m_frameObjectBindingProvider != nullptr)
        {
            FrameObjectBindingProvider::PreparedBindingSets bindingSets;
            if (m_frameObjectBindingProvider->CapturePreparedBindingSets(effectivePso, p_drawable, bindingSets))
            {
                preparedDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
                preparedDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
            }
        }

        if (preparedDraw.commandBuffer == nullptr)
        {
            if (usesThreadedRendering && QueueThreadedRecordedDraw(preparedDraw))
                m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
            return;
        }

        BindPreparedGraphicsPipeline(preparedDraw);
        if (m_frameObjectBindingProvider != nullptr)
            m_frameObjectBindingProvider->PrepareExplicitDraw(*preparedDraw.commandBuffer, effectivePso, p_drawable);

        BindPreparedMaterialBindingSet(preparedDraw);
        SubmitPreparedDraw(preparedDraw);
        m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
    }
}

bool CompositeRenderer::CaptureRecordedDrawCommand(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable,
    Context::RecordedDrawCommandInput& outDraw)
{
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
        return true;
    };

    const bool usesThreadedRendering = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->PrepareDraw(p_pso, p_drawable);

    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_pso, p_drawable, preparedDraw))
        return false;

    if (preparedDraw.commandBuffer == nullptr && usesThreadedRendering && m_frameObjectBindingProvider != nullptr)
    {
        FrameObjectBindingProvider::PreparedBindingSets bindingSets;
        if (m_frameObjectBindingProvider->CapturePreparedBindingSets(p_pso, p_drawable, bindingSets))
        {
            preparedDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
            preparedDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
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
        return true;
    };

    auto effectivePso = CreatePipelineState();
    const bool usesThreadedRendering = Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->PrepareDraw(effectivePso, p_drawable);

    PreparedRecordedDraw preparedDraw;
    if (!PrepareRecordedDraw(p_drawable, pipelineOverrides, depthCompareOverride, preparedDraw))
        return false;

    if (preparedDraw.commandBuffer == nullptr && usesThreadedRendering && m_frameObjectBindingProvider != nullptr)
    {
        FrameObjectBindingProvider::PreparedBindingSets bindingSets;
        if (m_frameObjectBindingProvider->CapturePreparedBindingSets(effectivePso, p_drawable, bindingSets))
        {
            preparedDraw.frameBindingSet = std::move(bindingSets.frameBindingSet);
            preparedDraw.objectBindingSet = std::move(bindingSets.objectBindingSet);
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
    if (Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver))
    {
        const auto telemetry = Context::DriverRendererAccess::GetThreadedFrameTelemetry(m_driver);
        const_cast<RendererStats&>(m_rendererStats).SetThreadedFrameTelemetry(telemetry);
    }

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

}
