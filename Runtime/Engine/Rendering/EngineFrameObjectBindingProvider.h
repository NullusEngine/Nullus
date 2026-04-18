#pragma once

#include <chrono>
#include <memory>

#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Core/FrameObjectBindingProvider.h>
#include <Rendering/RHI/Core/RHIBinding.h>

#include "EngineDef.h"

namespace NLS::Engine::Rendering
{
class NLS_ENGINE_API EngineFrameObjectBindingProvider final : public NLS::Render::Core::FrameObjectBindingProvider
{
public:
    explicit EngineFrameObjectBindingProvider(NLS::Render::Core::CompositeRenderer& renderer);

protected:
    void OnBeginFrame(const NLS::Render::Data::FrameDescriptor& frameDescriptor) override;
    void OnEndFrame() override;
    void OnPrepareDraw(PipelineState& pso, const NLS::Render::Entities::Drawable& drawable) override;
    void OnPrepareExplicitDraw(
        NLS::Render::RHI::RHICommandBuffer& commandBuffer,
        PipelineState& pso,
        const NLS::Render::Entities::Drawable& drawable) override;

private:
    void RefreshExplicitFrameBindingSet();
    void RefreshExplicitObjectBindingSet();

    std::chrono::high_resolution_clock::time_point m_startTime;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_engineBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslFrameBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslObjectBuffer;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_explicitFrameBindingSet;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_explicitObjectBindingSet;
    bool m_explicitFrameBindingSetDirty = true;
    bool m_explicitObjectBindingSetDirty = true;
};
}
