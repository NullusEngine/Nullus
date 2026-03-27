#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Rendering/Context/Driver.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/IRHIResource.h"

namespace NLS::Render::Buffers
{
/**
 * Wraps OpenGL EBO
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
     * Bind the framebuffer
     */
    void Bind() const;

    /**
     * Unbind the framebuffer
     */
    void Unbind() const;

    /**
     * Defines a new size for the framebuffer
     * @param p_width
     * @param p_height
     * @param p_forceUpdate Force the resizing operation even if the width and height didn't change
     */
    void Resize(uint16_t p_width, uint16_t p_height, bool p_forceUpdate = false);

    /**
     * Returns the ID of the OpenGL framebuffer
     */
    uint32_t GetID() const;

    /**
     * Returns the ID of the OpenGL render texture
     */
    uint32_t GetTextureID() const;
    const std::shared_ptr<NLS::Render::RHI::IRHITexture>& GetTextureResource() const;
    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetExplicitTextureHandle() const;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitColorView(const std::string& debugName = {}) const;

    /**
     * Returns the ID of the depth-stencil texture attached to this framebuffer.
     */
    uint32_t GetDepthStencilTextureID() const;
    const std::shared_ptr<NLS::Render::RHI::IRHITexture>& GetDepthStencilTextureResource() const;
    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetExplicitDepthStencilTextureHandle() const;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitDepthStencilView(const std::string& debugName = {}) const;

    /**
     * Legacy compatibility alias.
     */
    uint32_t GetRenderBufferID() const;

private:
    uint16_t m_width = 0;
    uint16_t m_height = 0;

    uint32_t m_bufferID = 0;
    uint32_t m_renderTexture = 0;
    uint32_t m_depthStencilTexture = 0;
    std::shared_ptr<NLS::Render::RHI::IRHITexture> m_renderTextureResource;
    std::shared_ptr<NLS::Render::RHI::IRHITexture> m_depthStencilTextureResource;
    std::shared_ptr<NLS::Render::RHI::RHITexture> m_explicitRenderTexture;
    std::shared_ptr<NLS::Render::RHI::RHITexture> m_explicitDepthStencilTexture;
    mutable std::shared_ptr<NLS::Render::RHI::RHITextureView> m_explicitRenderTextureView;
    mutable std::shared_ptr<NLS::Render::RHI::RHITextureView> m_explicitDepthStencilTextureView;
};
} // namespace NLS::Render::Buffers
