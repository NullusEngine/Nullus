#include "Rendering/Core/FrameObjectBindingProvider.h"

#include "Rendering/Core/CompositeRenderer.h"

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
    OnBeginFrame(frameDescriptor);
}

void FrameObjectBindingProvider::EndFrame()
{
    OnEndFrame();
    m_framePrepared = false;
    m_objectPrepared = false;
    m_preparedDrawCount = 0u;
}

void FrameObjectBindingProvider::PrepareDraw(PipelineState& pso, const Entities::Drawable& drawable)
{
    OnPrepareDraw(pso, drawable);
    m_objectPrepared = true;
}

void FrameObjectBindingProvider::PrepareExplicitDraw(
    RHI::RHICommandBuffer& commandBuffer,
    PipelineState& pso,
    const Entities::Drawable& drawable)
{
    OnPrepareExplicitDraw(commandBuffer, pso, drawable);
    ++m_preparedDrawCount;
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

void FrameObjectBindingProvider::OnBeginFrame(const Data::FrameDescriptor&)
{
}

void FrameObjectBindingProvider::OnEndFrame()
{
}

void FrameObjectBindingProvider::OnPrepareDraw(PipelineState&, const Entities::Drawable&)
{
}

void FrameObjectBindingProvider::OnPrepareExplicitDraw(RHI::RHICommandBuffer&, PipelineState&, const Entities::Drawable&)
{
}
}
