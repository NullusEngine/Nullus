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

bool CompositeRenderer::CanRecordExplicitFrame() const
{
    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled() && pass.second->BlocksExplicitRecording())
            return false;
    }

    return true;
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
    auto pso = CreatePipelineState();
    DrawRegisteredPasses(pso);
}

void CompositeRenderer::DrawRegisteredPasses(PipelineState pso)
{
    for (const auto& [_, pass] : m_passes)
    {
        if (pass.second->IsEnabled())
        {
            const auto commandBuffer = GetActiveExplicitCommandBuffer();
            if (commandBuffer != nullptr && !pass.second->RequiresLegacyExecution())
            {
                const bool startedRenderPass = BeginRecordedRenderPass(
                    m_frameDescriptor.outputBuffer,
                    m_frameDescriptor.renderWidth,
                    m_frameDescriptor.renderHeight,
                    false,
                    false,
                    false);

                pass.second->Draw(pso);

                if (startedRenderPass)
                    EndRecordedRenderPass();
            }
            else
            {
                const bool usesLegacyExecution = commandBuffer != nullptr && pass.second->RequiresLegacyExecution();
                if (usesLegacyExecution)
                    BeginLegacyDrawSection();

                pass.second->Draw(pso);

                if (usesLegacyExecution)
                    EndLegacyDrawSection();
            }
        }
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
    const auto commandBuffer = GetActiveExplicitCommandBuffer();
    for (const auto& [_, feature] : m_features)
    {
        if (feature->IsEnabled())
        {
            feature->OnBeforeDraw(p_pso, p_drawable);
            if (commandBuffer != nullptr)
                feature->OnPrepareExplicitDraw(*commandBuffer, p_pso, p_drawable);
        }
    }

    ABaseRenderer::DrawEntity(p_pso, p_drawable);

    for (const auto& [_, feature] : m_features)
    {
        if (feature->IsEnabled())
        {
            feature->OnAfterDraw(p_drawable);
        }
    }
}
}
