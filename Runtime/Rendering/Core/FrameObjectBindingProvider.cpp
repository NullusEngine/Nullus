#include "Rendering/Core/FrameObjectBindingProvider.h"

#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Resources/Material.h"

namespace NLS::Render::Core
{
FrameObjectBindingProvider::FrameObjectBindingProvider(CompositeRenderer& renderer)
    : m_renderer(renderer)
{
}

void FrameObjectBindingProvider::BeginFrame(const Data::FrameDescriptor& frameDescriptor)
{
    m_framePrepared = true;
    m_objectPrepared = false;
    m_preparedDrawCount = 0u;
    m_preparedMaterial = nullptr;
    m_preparedShader = nullptr;
    OnBeginFrame(frameDescriptor);
}

void FrameObjectBindingProvider::EndFrame()
{
    OnEndFrame();
    m_framePrepared = false;
    m_objectPrepared = false;
    m_preparedDrawCount = 0u;
    m_preparedMaterial = nullptr;
    m_preparedShader = nullptr;
}

bool FrameObjectBindingProvider::TryReservePreparedFrameResources()
{
    return OnTryReservePreparedFrameResources();
}

void FrameObjectBindingProvider::ReleaseReservedPreparedFrameResources()
{
    OnReleaseReservedPreparedFrameResources();
}

bool FrameObjectBindingProvider::HasReservedPreparedFrameResources() const
{
    return OnHasReservedPreparedFrameResources();
}

bool FrameObjectBindingProvider::PrepareDraw(PipelineState& pso, const Entities::Drawable& drawable)
{
    if (drawable.material != nullptr)
        return PrepareDraw(pso, drawable, *drawable.material);

    m_preparedMaterial = nullptr;
    m_preparedShader = nullptr;
    m_objectPrepared = OnPrepareDraw(pso, drawable);
    return m_objectPrepared;
}

bool FrameObjectBindingProvider::PrepareDraw(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    const Resources::Material& effectiveMaterial)
{
    const auto* effectiveShader = effectiveMaterial.GetShader();
    m_preparedMaterial = &effectiveMaterial;
    m_preparedShader = effectiveShader;
    m_objectPrepared = OnPrepareDraw(pso, drawable);
    return m_objectPrepared;
}

bool FrameObjectBindingProvider::PrepareDraw(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    const Resources::Material& effectiveMaterial,
    const Resources::Shader& effectiveShader)
{
    m_preparedMaterial = &effectiveMaterial;
    m_preparedShader = &effectiveShader;
    m_objectPrepared = OnPrepareDraw(pso, drawable);
    return m_objectPrepared;
}

void FrameObjectBindingProvider::PrepareExplicitDraw(
    RHI::RHICommandBuffer& commandBuffer,
    PipelineState& pso,
    const Entities::Drawable& drawable)
{
    OnPrepareExplicitDraw(commandBuffer, pso, drawable);
    ++m_preparedDrawCount;
}

bool FrameObjectBindingProvider::CaptureFrameBindingSet(
    std::shared_ptr<RHI::RHIBindingSet>& outBindingSet)
{
    outBindingSet.reset();
    if (!m_framePrepared)
        return false;

    return OnCaptureFrameBindingSet(outBindingSet) && outBindingSet != nullptr;
}

bool FrameObjectBindingProvider::CapturePreparedBindingSets(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    PreparedBindingSets& outBindings)
{
    return OnCapturePreparedBindingSets(pso, drawable, outBindings);
}

bool FrameObjectBindingProvider::CapturePreparedObjectBindingSet(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    PreparedBindingSets& outBindings)
{
    outBindings.frameBindingSet.reset();
    return OnCapturePreparedObjectBindingSet(pso, drawable, outBindings);
}

bool FrameObjectBindingProvider::IsFramePrepared() const
{
    return m_framePrepared;
}

bool FrameObjectBindingProvider::IsObjectPrepared() const
{
    return m_objectPrepared;
}

uint64_t FrameObjectBindingProvider::GetPreparedDrawCount() const
{
    return m_preparedDrawCount;
}

const Resources::Material* FrameObjectBindingProvider::GetPreparedMaterial() const
{
    return m_preparedMaterial;
}

const Resources::Shader* FrameObjectBindingProvider::GetPreparedShader() const
{
    return m_preparedShader;
}

void FrameObjectBindingProvider::OnBeginFrame(const Data::FrameDescriptor&)
{
}

void FrameObjectBindingProvider::OnEndFrame()
{
}

bool FrameObjectBindingProvider::OnTryReservePreparedFrameResources()
{
    return true;
}

void FrameObjectBindingProvider::OnReleaseReservedPreparedFrameResources()
{
}

bool FrameObjectBindingProvider::OnHasReservedPreparedFrameResources() const
{
    return false;
}

bool FrameObjectBindingProvider::OnPrepareDraw(PipelineState&, const Entities::Drawable&)
{
    return true;
}

void FrameObjectBindingProvider::OnPrepareExplicitDraw(RHI::RHICommandBuffer&, PipelineState&, const Entities::Drawable&)
{
}

bool FrameObjectBindingProvider::OnCaptureFrameBindingSet(std::shared_ptr<RHI::RHIBindingSet>&)
{
    return false;
}

bool FrameObjectBindingProvider::OnCapturePreparedObjectBindingSet(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    PreparedBindingSets& outBindings)
{
    return OnCapturePreparedBindingSets(pso, drawable, outBindings);
}

bool FrameObjectBindingProvider::OnCapturePreparedBindingSets(
    PipelineState&,
    const Entities::Drawable&,
    PreparedBindingSets&)
{
    return false;
}
}
