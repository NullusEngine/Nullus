#pragma once

#include <vector>

#include "Rendering/Context/Driver.h"

namespace NLS::Rendering::Buffers
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

    /**
     * Returns the ID of the OpenGL render buffer
     */
    uint32_t GetRenderBufferID() const;

private:
    uint16_t m_width = 0;
    uint16_t m_height = 0;

    uint32_t m_bufferID = 0;
    uint32_t m_renderTexture = 0;
    uint32_t m_depthStencilBuffer = 0;
};
} // namespace NLS::Rendering::Buffers