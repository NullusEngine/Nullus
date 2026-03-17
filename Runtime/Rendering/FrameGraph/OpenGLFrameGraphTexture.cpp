#include "Rendering/FrameGraph/OpenGLFrameGraphTexture.h"

namespace NLS::Render::FrameGraph
{
    void OpenGLFrameGraphTexture::create(const Desc& desc, void*)
    {
        if (id != 0)
            return;

        glGenTextures(1, &id);
        glBindTexture(desc.target, id);
        glTexParameteri(desc.target, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(desc.minFilter));
        glTexParameteri(desc.target, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(desc.magFilter));
        glTexParameteri(desc.target, GL_TEXTURE_WRAP_S, static_cast<GLint>(desc.wrapS));
        glTexParameteri(desc.target, GL_TEXTURE_WRAP_T, static_cast<GLint>(desc.wrapT));
        glTexImage2D(desc.target, 0, desc.internalFormat, desc.width, desc.height, 0, desc.format, desc.type, nullptr);
        glBindTexture(desc.target, 0);
    }

    void OpenGLFrameGraphTexture::destroy(const Desc&, void*)
    {
        if (id == 0)
            return;

        glDeleteTextures(1, &id);
        id = 0;
    }

    void OpenGLFrameGraphTexture::preRead(const Desc& desc, uint32_t flags, void*)
    {
        if (id == 0)
            return;

        const auto slot = static_cast<GLenum>(flags == 0xFFFFFFFFu ? 0u : flags);
        glActiveTexture(GL_TEXTURE0 + slot);
        glBindTexture(desc.target, id);
    }

    void OpenGLFrameGraphTexture::preWrite(const Desc&, uint32_t, void*)
    {
        // OpenGL FBO attachment routing is handled by the pass context.
    }

    std::string OpenGLFrameGraphTexture::toString(const Desc& desc)
    {
        return std::to_string(desc.width) + "x" + std::to_string(desc.height) + " fmt=" + std::to_string(desc.internalFormat);
    }
}
