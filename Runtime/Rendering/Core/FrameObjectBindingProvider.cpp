#include "Rendering/Core/FrameObjectBindingProvider.h"

#include "Rendering/Core/CompositeRenderer.h"
#include "Rendering/Resources/Material.h"

namespace NLS::Render::Core
{
FrameObjectBindingProvider::FrameObjectBindingProvider(CompositeRenderer& renderer)
    : m_renderer(renderer)
{
}

FrameObjectBindingProvider::PreparationState& FrameObjectBindingProvider::ActivePreparationState() const
{
    return m_preparationState;
}

void FrameObjectBindingProvider::BeginFrame(const Data::FrameDescriptor& frameDescriptor)
{
    std::lock_guard lock(m_stateMutex);
    m_preparationState = {};
    m_preparationState.framePrepared = true;
    OnBeginFrame(frameDescriptor);
}

void FrameObjectBindingProvider::EndFrame()
{
    std::lock_guard lock(m_stateMutex);
    OnEndFrame();
    ActivePreparationState().framePrepared = false;
    ActivePreparationState().objectPrepared = false;
    ActivePreparationState().preparedDrawCount = 0u;
    ActivePreparationState().preparedMaterial = nullptr;
    ActivePreparationState().preparedShader = nullptr;
}

bool FrameObjectBindingProvider::TryReservePreparedFrameResources()
{
    return OnTryReservePreparedFrameResources();
}

void FrameObjectBindingProvider::ReleaseReservedPreparedFrameResources()
{
    std::lock_guard lock(m_stateMutex);
    OnReleaseReservedPreparedFrameResources();
}

bool FrameObjectBindingProvider::HasReservedPreparedFrameResources() const
{
    std::lock_guard lock(m_stateMutex);
    return OnHasReservedPreparedFrameResources();
}

void FrameObjectBindingProvider::BindCaptureFrameSlot(const size_t)
{
    std::lock_guard lock(m_stateMutex);
}

void FrameObjectBindingProvider::UnbindCaptureFrameSlot()
{
    std::lock_guard lock(m_stateMutex);
}

std::function<void(size_t)> FrameObjectBindingProvider::FreezeCaptureFrame()
{
    return [this](size_t slotIndex) { BindCaptureFrameSlot(slotIndex); };
}

bool FrameObjectBindingProvider::PrepareDraw(PipelineState& pso, const Entities::Drawable& drawable)
{
    std::lock_guard lock(m_stateMutex);
    if (drawable.material != nullptr)
        return PrepareDraw(pso, drawable, *drawable.material);

    ActivePreparationState().preparedMaterial = nullptr;
    ActivePreparationState().preparedShader = nullptr;
    ActivePreparationState().objectPrepared = OnPrepareDraw(pso, drawable);
    return ActivePreparationState().objectPrepared;
}

bool FrameObjectBindingProvider::PrepareDraw(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    const Resources::Material& effectiveMaterial)
{
    std::lock_guard lock(m_stateMutex);
    const auto* effectiveShader = effectiveMaterial.GetShader();
    ActivePreparationState().preparedMaterial = &effectiveMaterial;
    ActivePreparationState().preparedShader = effectiveShader;
    ActivePreparationState().objectPrepared = OnPrepareDraw(pso, drawable);
    return ActivePreparationState().objectPrepared;
}

bool FrameObjectBindingProvider::PrepareDraw(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    const Resources::Material& effectiveMaterial,
    const Resources::Shader& effectiveShader)
{
    std::lock_guard lock(m_stateMutex);
    ActivePreparationState().preparedMaterial = &effectiveMaterial;
    ActivePreparationState().preparedShader = &effectiveShader;
    ActivePreparationState().objectPrepared = OnPrepareDraw(pso, drawable);
    return ActivePreparationState().objectPrepared;
}

void FrameObjectBindingProvider::PrepareExplicitDraw(
    RHI::RHICommandBuffer& commandBuffer,
    PipelineState& pso,
    const Entities::Drawable& drawable)
{
    std::lock_guard lock(m_stateMutex);
    OnPrepareExplicitDraw(commandBuffer, pso, drawable);
    ++ActivePreparationState().preparedDrawCount;
}

bool FrameObjectBindingProvider::CaptureFrameBindingSet(
    std::shared_ptr<RHI::RHIBindingSet>& outBindingSet)
{
    std::lock_guard lock(m_stateMutex);
    outBindingSet.reset();
    if (!ActivePreparationState().framePrepared)
        return false;

    return OnCaptureFrameBindingSet(outBindingSet) && outBindingSet != nullptr;
}

bool FrameObjectBindingProvider::CapturePreparedBindingSets(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    PreparedBindingSets& outBindings)
{
    std::lock_guard lock(m_stateMutex);
    return OnCapturePreparedBindingSets(pso, drawable, outBindings);
}

bool FrameObjectBindingProvider::CapturePreparedObjectBindingSet(
    PipelineState& pso,
    const Entities::Drawable& drawable,
    PreparedBindingSets& outBindings)
{
    std::lock_guard lock(m_stateMutex);
    outBindings.frameBindingSet.reset();
    return OnCapturePreparedObjectBindingSet(pso, drawable, outBindings);
}

bool FrameObjectBindingProvider::IsFramePrepared() const
{
    std::lock_guard lock(m_stateMutex);
    return ActivePreparationState().framePrepared;
}

bool FrameObjectBindingProvider::IsObjectPrepared() const
{
    std::lock_guard lock(m_stateMutex);
    return ActivePreparationState().objectPrepared;
}

uint64_t FrameObjectBindingProvider::GetPreparedDrawCount() const
{
    std::lock_guard lock(m_stateMutex);
    return ActivePreparationState().preparedDrawCount;
}

const Resources::Material* FrameObjectBindingProvider::GetPreparedMaterial() const
{
    std::lock_guard lock(m_stateMutex);
    return ActivePreparationState().preparedMaterial;
}

const Resources::Shader* FrameObjectBindingProvider::GetPreparedShader() const
{
    std::lock_guard lock(m_stateMutex);
    return ActivePreparationState().preparedShader;
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
