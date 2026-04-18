#include <Debug/Logger.h>
#include <Rendering/Core/ABaseRenderer.h>
#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>

#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"

using namespace NLS;

namespace
{
    bool ShouldLogFrameConstantDiagnostics()
    {
        static const bool enabled = []()
        {
            return NLS::Render::Settings::IsEnvironmentFlagEnabled("NLS_LOG_RENDER_DRAW_PATH");
        }();
        return enabled;
    }
}

namespace NLS::Engine::Rendering
{
EngineFrameObjectBindingProvider::EngineFrameObjectBindingProvider(NLS::Render::Core::CompositeRenderer& renderer)
    : FrameObjectBindingProvider(renderer)
{
    m_engineBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
        sizeof(Maths::Matrix4) +
        sizeof(Maths::Matrix4) +
        sizeof(Maths::Matrix4) +
        sizeof(Maths::Vector3) +
        sizeof(float) +
        sizeof(Maths::Matrix4),
        0,
        0,
        NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

    m_hlslFrameBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
        sizeof(Maths::Matrix4) + sizeof(Maths::Vector3) + sizeof(float) + sizeof(Maths::Matrix4),
        NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kFrameBindingSpace, 0),
        0,
        NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

    m_hlslObjectBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
        sizeof(Maths::Matrix4),
        NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0),
        0,
        NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

    m_startTime = std::chrono::high_resolution_clock::now();
}

void EngineFrameObjectBindingProvider::OnBeginFrame(const NLS::Render::Data::FrameDescriptor& frameDescriptor)
{
    auto currentTime = std::chrono::high_resolution_clock::now();
    auto elapsedTime = std::chrono::duration_cast<std::chrono::duration<float>>(currentTime - m_startTime);

    size_t offset = sizeof(Maths::Matrix4);
    m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(frameDescriptor.camera->GetViewMatrix()), std::ref(offset));
    m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(frameDescriptor.camera->GetProjectionMatrix()), std::ref(offset));
    m_engineBuffer->SetSubData(frameDescriptor.camera->GetPosition(), std::ref(offset));
    m_engineBuffer->SetSubData(elapsedTime.count(), std::ref(offset));
    m_engineBuffer->Bind(0);

    size_t hlslFrameOffset = 0;
    const auto viewProjection = frameDescriptor.camera->GetProjectionMatrix() * frameDescriptor.camera->GetViewMatrix();
    auto viewMatrixNoTranslation = frameDescriptor.camera->GetViewMatrix();
    viewMatrixNoTranslation(0, 3) = 0.0f;
    viewMatrixNoTranslation(1, 3) = 0.0f;
    viewMatrixNoTranslation(2, 3) = 0.0f;
    const auto viewProjectionNoTranslation = frameDescriptor.camera->GetProjectionMatrix() * viewMatrixNoTranslation;

    if (ShouldLogFrameConstantDiagnostics())
    {
        const auto& cameraPos = frameDescriptor.camera->GetPosition();
        const auto& clearColor = frameDescriptor.camera->GetClearColor();
        NLS_LOG_INFO(
            "[FrameConstants] renderSize=" +
            std::to_string(frameDescriptor.renderWidth) + "x" + std::to_string(frameDescriptor.renderHeight) +
            " cameraPos=(" + std::to_string(cameraPos.x) + "," + std::to_string(cameraPos.y) + "," + std::to_string(cameraPos.z) + ")" +
            " clearColor=(" + std::to_string(clearColor.x) + "," + std::to_string(clearColor.y) + "," + std::to_string(clearColor.z) + ")" +
            " vpNoTrans_row0=(" + std::to_string(viewProjectionNoTranslation.data[0]) + "," + std::to_string(viewProjectionNoTranslation.data[1]) + "," + std::to_string(viewProjectionNoTranslation.data[2]) + "," + std::to_string(viewProjectionNoTranslation.data[3]) + ")" +
            " vpNoTrans_row1=(" + std::to_string(viewProjectionNoTranslation.data[4]) + "," + std::to_string(viewProjectionNoTranslation.data[5]) + "," + std::to_string(viewProjectionNoTranslation.data[6]) + "," + std::to_string(viewProjectionNoTranslation.data[7]) + ")");
    }

    m_hlslFrameBuffer->SetSubData(Maths::Matrix4::Transpose(viewProjection), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(frameDescriptor.camera->GetPosition(), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(elapsedTime.count(), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->SetSubData(Maths::Matrix4::Transpose(viewProjectionNoTranslation), std::ref(hlslFrameOffset));
    m_hlslFrameBuffer->Bind(NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kFrameBindingSpace, 0));
    m_explicitFrameBindingSetDirty = true;
}

void EngineFrameObjectBindingProvider::OnEndFrame()
{
    m_engineBuffer->Unbind();
    m_hlslFrameBuffer->Unbind();
    m_hlslObjectBuffer->Unbind();
}

void EngineFrameObjectBindingProvider::OnPrepareDraw(
    PipelineState&,
    const NLS::Render::Entities::Drawable& drawable)
{
    NLS::Render::Data::DrawableObjectDescriptor descriptor;
    if (drawable.TryGetDescriptor<NLS::Render::Data::DrawableObjectDescriptor>(descriptor))
    {
        m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(descriptor.modelMatrix), 0);
        m_engineBuffer->SetSubData(
            descriptor.userMatrix,
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Matrix4) +
            sizeof(Maths::Vector3) +
            sizeof(float));

        m_hlslObjectBuffer->SetSubData(Maths::Matrix4::Transpose(descriptor.modelMatrix), 0);
        m_hlslObjectBuffer->Bind(NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0));
        m_explicitObjectBindingSetDirty = true;
    }
}

void EngineFrameObjectBindingProvider::OnPrepareExplicitDraw(
    NLS::Render::RHI::RHICommandBuffer& commandBuffer,
    PipelineState&,
    const NLS::Render::Entities::Drawable&)
{
    RefreshExplicitFrameBindingSet();
    RefreshExplicitObjectBindingSet();

    if (m_explicitFrameBindingSet != nullptr)
        commandBuffer.BindBindingSet(NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet, m_explicitFrameBindingSet);
    if (m_explicitObjectBindingSet != nullptr)
        commandBuffer.BindBindingSet(NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet, m_explicitObjectBindingSet);
}

void EngineFrameObjectBindingProvider::RefreshExplicitFrameBindingSet()
{
    if (!m_explicitFrameBindingSetDirty)
        return;

    m_explicitFrameBindingSet.reset();

    NLS::Render::Core::ABaseRenderer::ExplicitUniformBufferBindingDesc bindingDesc;
    bindingDesc.set = NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet;
    bindingDesc.registerSpace = NLS::Render::RHI::BindingPointMap::kFrameBindingSpace;
    bindingDesc.binding = 0u;
    bindingDesc.range = sizeof(Maths::Matrix4) + sizeof(Maths::Vector3) + sizeof(float) + sizeof(Maths::Matrix4);
    bindingDesc.entryName = "FrameConstants";
    bindingDesc.layoutDebugName = "EngineFrameBindingLayout";
    bindingDesc.setDebugName = "EngineFrameBindingSet";
    bindingDesc.snapshotDebugName = "EngineFrameConstantsSnapshot";
    bindingDesc.stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;
    m_explicitFrameBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(*m_hlslFrameBuffer, bindingDesc);
    m_explicitFrameBindingSetDirty = false;
}

void EngineFrameObjectBindingProvider::RefreshExplicitObjectBindingSet()
{
    if (!m_explicitObjectBindingSetDirty)
        return;

    m_explicitObjectBindingSet.reset();

    NLS::Render::Core::ABaseRenderer::ExplicitUniformBufferBindingDesc bindingDesc;
    bindingDesc.set = NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet;
    bindingDesc.registerSpace = NLS::Render::RHI::BindingPointMap::kObjectBindingSpace;
    bindingDesc.binding = 0u;
    bindingDesc.range = sizeof(Maths::Matrix4);
    bindingDesc.entryName = "ObjectConstants";
    bindingDesc.layoutDebugName = "EngineObjectBindingLayout";
    bindingDesc.setDebugName = "EngineObjectBindingSet";
    bindingDesc.snapshotDebugName = "EngineObjectConstantsSnapshot";
    bindingDesc.stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;
    m_explicitObjectBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(*m_hlslObjectBuffer, bindingDesc);
    m_explicitObjectBindingSetDirty = false;
}
}
