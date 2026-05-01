#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::Buffers
{
/**
 * Backend-neutral offscreen render-target wrapper over the formal RHI surface.
 */
class NLS_RENDER_API Framebuffer
{
public:
    /**
     * Create the framebuffer
     * @param p_width
     * @param p_height
     */
    Framebuffer(uint16_t p_width = 0, uint16_t p_height = 0);

    /**
     * Destructor
     */
    ~Framebuffer();

    /**
     * Defines a new size for the framebuffer
     * @param p_width
     * @param p_height
     * @param p_forceUpdate Force the resizing operation even if the width and height didn't change
     */
    void Resize(uint16_t p_width, uint16_t p_height, bool p_forceUpdate = false);
    void SetOptimizedColorClearValue(float r, float g, float b, float a = 1.0f);

    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetExplicitTextureHandle() const;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitColorView(const std::string& debugName = {}) const;

    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetExplicitDepthStencilTextureHandle() const;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitDepthStencilView(const std::string& debugName = {}) const;

private:
    void Release();

    uint16_t m_width = 0;
    uint16_t m_height = 0;
    NLS::Render::RHI::RHITextureDesc::OptimizedClearValue m_colorOptimizedClearValue{ true };

    std::shared_ptr<NLS::Render::RHI::RHITexture> m_explicitRenderTexture;
    std::shared_ptr<NLS::Render::RHI::RHITexture> m_explicitDepthStencilTexture;
    mutable std::shared_ptr<NLS::Render::RHI::RHITextureView> m_explicitRenderTextureView;
    mutable std::shared_ptr<NLS::Render::RHI::RHITextureView> m_explicitDepthStencilTextureView;
};
} // namespace NLS::Render::Buffers
