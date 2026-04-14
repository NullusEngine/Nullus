
#pragma once

#include <map>
#include <chrono>

#include <Rendering/Features/ARenderFeature.h>
#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Entities/Camera.h>
#include <Rendering/RHI/Core/RHIBinding.h>
#include <Rendering/RHI/RHITypes.h>
#include "EngineDef.h"
namespace NLS::Engine::Rendering
{
/**
 * Render feature handling engine buffer (UBO) updates
 */
class NLS_ENGINE_API EngineBufferRenderFeature : public NLS::Render::Features::ARenderFeature
{
public:
    /**
     * Constructor
     * @param p_renderer
     */
    EngineBufferRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer);

protected:
    virtual void OnBeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
    virtual void OnEndFrame() override;
    virtual void OnBeforeDraw(NLS::Render::Data::PipelineState& p_pso, const NLS::Render::Entities::Drawable& p_drawable) override;
    virtual void OnPrepareExplicitDraw(NLS::Render::RHI::RHICommandBuffer& commandBuffer, NLS::Render::Data::PipelineState& p_pso, const NLS::Render::Entities::Drawable& p_drawable) override;

private:
    void RefreshExplicitFrameBindingSet();
    void RefreshExplicitObjectBindingSet();

protected:
    std::chrono::high_resolution_clock::time_point m_startTime;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_engineBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslFrameBuffer;
    std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_hlslObjectBuffer;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_explicitFrameBindingSet;
    std::shared_ptr<NLS::Render::RHI::RHIBindingSet> m_explicitObjectBindingSet;
    bool m_explicitFrameBindingSetDirty = true;
    bool m_explicitObjectBindingSetDirty = true;
};
} // namespace NLS::Engine::Rendering
