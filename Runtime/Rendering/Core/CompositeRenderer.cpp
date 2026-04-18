#include <functional>
#include <filesystem>
#include <fstream>

#include "Rendering/Core/CompositeRenderer.h"

namespace NLS::Render::Core
{
CompositeRenderer::CompositeRenderer(Context::Driver& p_driver)
    : ABaseRenderer(p_driver)
{
}

void CompositeRenderer::BeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
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
    DrawRegisteredPasses();
}

void CompositeRenderer::ExecutePass(Core::ARenderPass& pass)
{
    ExecutePass(pass, CreatePipelineState());
}

void CompositeRenderer::ExecutePass(Core::ARenderPass& pass, PipelineState pso)
{
    const auto commandBuffer = GetActiveExplicitCommandBuffer();
    if (commandBuffer != nullptr && !pass.ManagesOwnRenderPass())
    {
        const bool startedRenderPass = BeginOutputRenderPass(
            m_frameDescriptor.renderWidth,
            m_frameDescriptor.renderHeight,
            false,
            false,
            false);

        pass.Draw(pso);

        EndOutputRenderPass(startedRenderPass);

        return;
    }

    pass.Draw(pso);
}

void CompositeRenderer::DrawRegisteredPasses()
{
    DrawRegisteredPasses(CreatePipelineState());
}

void CompositeRenderer::DrawRegisteredPasses(PipelineState pso)
{
    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled())
            ExecutePass(*pass.second, pso);
    }
}

void CompositeRenderer::EndFrame()
{
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

    m_rendererStats.EndFrame();
    ClearDescriptors();
    ABaseRenderer::EndFrame();
}

void CompositeRenderer::DrawEntity(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable
)
{
    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->PrepareDraw(p_pso, p_drawable);

    PreparedRecordedDraw preparedDraw;
    if (PrepareRecordedDraw(p_pso, p_drawable, preparedDraw))
    {
        BindPreparedGraphicsPipeline(preparedDraw);

        if (preparedDraw.commandBuffer != nullptr)
        {
            if (m_frameObjectBindingProvider != nullptr)
                m_frameObjectBindingProvider->PrepareExplicitDraw(*preparedDraw.commandBuffer, p_pso, p_drawable);
        }

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
    auto effectivePso = CreatePipelineState();

    if (m_frameObjectBindingProvider != nullptr)
        m_frameObjectBindingProvider->PrepareDraw(effectivePso, p_drawable);

    PreparedRecordedDraw preparedDraw;
    if (PrepareRecordedDraw(p_drawable, pipelineOverrides, depthCompareOverride, preparedDraw))
    {
        BindPreparedGraphicsPipeline(preparedDraw);

        if (preparedDraw.commandBuffer != nullptr)
        {
            if (m_frameObjectBindingProvider != nullptr)
                m_frameObjectBindingProvider->PrepareExplicitDraw(*preparedDraw.commandBuffer, effectivePso, p_drawable);
        }

        BindPreparedMaterialBindingSet(preparedDraw);
        SubmitPreparedDraw(preparedDraw);
        m_rendererStats.RecordSubmittedDraw(p_drawable, preparedDraw.instanceCount);
    }
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

}
