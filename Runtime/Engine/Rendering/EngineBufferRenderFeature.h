
#pragma once

#include <map>
#include <chrono>

#include <Rendering/Features/ARenderFeature.h>
#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Entities/Camera.h>
#include "EngineDef.h"
namespace NLS::Engine::Rendering
{
/**
 * Render feature handling engine buffer (UBO) updates
 */
class NLS_ENGINE_API EngineBufferRenderFeature : public NLS::Rendering::Features::ARenderFeature
{
public:
    /**
     * Constructor
     * @param p_renderer
     */
    EngineBufferRenderFeature(NLS::Rendering::Core::CompositeRenderer& p_renderer);

protected:
    virtual void OnBeginFrame(const NLS::Rendering::Data::FrameDescriptor& p_frameDescriptor) override;
    virtual void OnEndFrame() override;
    virtual void OnBeforeDraw(NLS::Rendering::Data::PipelineState& p_pso, const NLS::Rendering::Entities::Drawable& p_drawable) override;

protected:
    std::chrono::high_resolution_clock::time_point m_startTime;
    std::unique_ptr<NLS::Rendering::Buffers::UniformBuffer> m_engineBuffer;
};
} // namespace NLS::Engine::Rendering
