#include <Debug/Logger.h>
#include <Rendering/Core/ABaseRenderer.h>
#include <Rendering/Core/CompositeRenderer.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Settings/DriverSettings.h>

#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/EngineFrameObjectBindingProvider.h"

using namespace NLS;

namespace
{
    bool ShouldLogFrameConstantDiagnostics(const Render::Context::Driver& driver)
    {
        return Render::Context::DriverRendererAccess::GetDiagnosticsSettings(driver).logRenderDrawPath;
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

    m_hlslObjectBufferAlt = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
        sizeof(Maths::Matrix4),
        NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0),
        0,
        NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

    m_startTime = std::chrono::high_resolution_clock::now();
}

void EngineFrameObjectBindingProvider::PrepareRenderScenePackage(
    const NLS::Render::Context::FrameSnapshot&,
    NLS::Render::Context::RenderScenePackage& package) const
{
    package.frameDataReady = true;
    package.objectDataReady = true;
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

    size_t hlslFrameOffset = 0;
    const auto viewProjection = frameDescriptor.camera->GetProjectionMatrix() * frameDescriptor.camera->GetViewMatrix();
    auto viewMatrixNoTranslation = frameDescriptor.camera->GetViewMatrix();
    viewMatrixNoTranslation(0, 3) = 0.0f;
    viewMatrixNoTranslation(1, 3) = 0.0f;
    viewMatrixNoTranslation(2, 3) = 0.0f;
    const auto viewProjectionNoTranslation = frameDescriptor.camera->GetProjectionMatrix() * viewMatrixNoTranslation;

    if (ShouldLogFrameConstantDiagnostics(m_renderer.GetDriver()))
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
    m_explicitFrameBindingSetDirty = true;
}

void EngineFrameObjectBindingProvider::OnEndFrame()
{
    m_useAltObjectBuffer = !m_useAltObjectBuffer;

    OnDeferredReset();
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

        auto& writeBuffer = m_useAltObjectBuffer ? *m_hlslObjectBufferAlt : *m_hlslObjectBuffer;
        writeBuffer.SetSubData(Maths::Matrix4::Transpose(descriptor.modelMatrix), 0);
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

bool EngineFrameObjectBindingProvider::OnCapturePreparedBindingSets(
    PipelineState&,
    const NLS::Render::Entities::Drawable&,
    PreparedBindingSets& outBindings)
{
    RefreshExplicitFrameBindingSet();
    RefreshExplicitObjectBindingSet();
    outBindings.frameBindingSet = m_explicitFrameBindingSet;
    outBindings.objectBindingSet = m_explicitObjectBindingSet;
    return outBindings.frameBindingSet != nullptr || outBindings.objectBindingSet != nullptr;
}

void EngineFrameObjectBindingProvider::RefreshExplicitFrameBindingSet()
{
    if (!m_explicitFrameBindingSetDirty)
        return;

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
    auto newBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(*m_hlslFrameBuffer, bindingDesc);

    m_deferredFrameBindingSet = std::move(m_explicitFrameBindingSet);
    m_explicitFrameBindingSet = std::move(newBindingSet);
    m_explicitFrameBindingSetDirty = false;
}

void EngineFrameObjectBindingProvider::RefreshExplicitObjectBindingSet()
{
    if (!m_explicitObjectBindingSetDirty)
        return;

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

    auto& writeBuffer = m_useAltObjectBuffer ? *m_hlslObjectBufferAlt : *m_hlslObjectBuffer;
    auto newBindingSet = m_renderer.CreateExplicitUniformBufferBindingSet(writeBuffer, bindingDesc);

    m_deferredObjectBindingSet = std::move(m_explicitObjectBindingSet);
    m_explicitObjectBindingSet = std::move(newBindingSet);
    m_explicitObjectBindingSetDirty = false;
}

void EngineFrameObjectBindingProvider::OnDeferredReset()
{
    m_deferredFrameBindingSet.reset();
    m_deferredObjectBindingSet.reset();
}
}
