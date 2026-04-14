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

    for (const auto& [_, feature] : m_features)
    {
        if (feature->IsEnabled())
        {
            feature->OnBeginFrame(p_frameDescriptor);
        }
    }

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

    for (const auto& [_, feature] : m_features)
    {
        if (feature->IsEnabled())
        {
            feature->OnEndFrame();
        }
    }

    ClearDescriptors();
    ABaseRenderer::EndFrame();
}

void CompositeRenderer::DrawEntity(
    PipelineState p_pso,
    const Entities::Drawable& p_drawable
)
{
    for (const auto& [_, feature] : m_features)
    {
        if (feature->IsEnabled())
        {
            feature->OnBeforeDraw(p_pso, p_drawable);
        }
    }

    PreparedRecordedDraw preparedDraw;
    if (PrepareRecordedDraw(p_pso, p_drawable, preparedDraw))
    {
        BindPreparedGraphicsPipeline(preparedDraw);

        if (preparedDraw.commandBuffer != nullptr)
        {
            for (const auto& [_, feature] : m_features)
            {
                if (feature->IsEnabled())
                    feature->OnPrepareExplicitDraw(*preparedDraw.commandBuffer, p_pso, p_drawable);
            }
        }

        BindPreparedMaterialBindingSet(preparedDraw);
        SubmitPreparedDraw(preparedDraw);
    }

    for (const auto& [_, feature] : m_features)
    {
        if (feature->IsEnabled())
        {
            feature->OnAfterDraw(p_drawable);
        }
    }
}

void CompositeRenderer::DrawEntity(
    const Entities::Drawable& p_drawable,
    Resources::MaterialPipelineStateOverrides pipelineOverrides,
    Settings::EComparaisonAlgorithm depthCompareOverride)
{
    auto effectivePso = CreatePipelineState();

    for (const auto& [_, feature] : m_features)
    {
        if (feature->IsEnabled())
        {
            feature->OnBeforeDraw(effectivePso, p_drawable);
        }
    }

    PreparedRecordedDraw preparedDraw;
    if (PrepareRecordedDraw(p_drawable, pipelineOverrides, depthCompareOverride, preparedDraw))
    {
        BindPreparedGraphicsPipeline(preparedDraw);

        if (preparedDraw.commandBuffer != nullptr)
        {
            for (const auto& [_, feature] : m_features)
            {
                if (feature->IsEnabled())
                    feature->OnPrepareExplicitDraw(*preparedDraw.commandBuffer, effectivePso, p_drawable);
            }
        }

        BindPreparedMaterialBindingSet(preparedDraw);
        SubmitPreparedDraw(preparedDraw);
    }

    for (const auto& [_, feature] : m_features)
    {
        if (feature->IsEnabled())
        {
            feature->OnAfterDraw(p_drawable);
        }
    }
}

}
