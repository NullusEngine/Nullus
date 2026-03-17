#include "Rendering/Buffers/MultiFramebuffer.h"

namespace NLS::Render::Buffers
{
    MultiFramebuffer::MultiFramebuffer(uint16_t width, uint16_t height, const std::vector<AttachmentDesc>& colorAttachments, bool withDepth)
    {
        Init(width, height, colorAttachments, withDepth);
    }

    MultiFramebuffer::~MultiFramebuffer()
    {
        Release();
    }

    void MultiFramebuffer::Init(uint16_t width, uint16_t height, const std::vector<AttachmentDesc>& colorAttachments, bool withDepth)
    {
        Release();
        m_width = width;
        m_height = height;
        m_withDepth = withDepth;
        m_attachmentDescs = colorAttachments;
        Allocate();
    }

    void MultiFramebuffer::Resize(uint16_t width, uint16_t height)
    {
        if (width == m_width && height == m_height)
            return;

        m_width = width;
        m_height = height;
        Allocate();
    }

    void MultiFramebuffer::Bind() const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_bufferId);
    }

    void MultiFramebuffer::Unbind() const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void MultiFramebuffer::Release()
    {
        if (!m_colorTextures.empty())
        {
            glDeleteTextures(static_cast<GLsizei>(m_colorTextures.size()), m_colorTextures.data());
            m_colorTextures.clear();
        }

        if (m_depthTexture != 0)
        {
            glDeleteTextures(1, &m_depthTexture);
            m_depthTexture = 0;
        }

        if (m_bufferId != 0)
        {
            glDeleteFramebuffers(1, &m_bufferId);
            m_bufferId = 0;
        }
    }

    void MultiFramebuffer::Allocate()
    {
        if (m_bufferId == 0)
            glGenFramebuffers(1, &m_bufferId);

        Bind();

        if (!m_colorTextures.empty())
        {
            glDeleteTextures(static_cast<GLsizei>(m_colorTextures.size()), m_colorTextures.data());
            m_colorTextures.clear();
        }

        if (!m_attachmentDescs.empty())
        {
            m_colorTextures.resize(m_attachmentDescs.size(), 0);
            glGenTextures(static_cast<GLsizei>(m_colorTextures.size()), m_colorTextures.data());

            std::vector<GLenum> drawBuffers;
            drawBuffers.reserve(m_attachmentDescs.size());

            for (size_t i = 0; i < m_attachmentDescs.size(); ++i)
            {
                glBindTexture(GL_TEXTURE_2D, m_colorTextures[i]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, m_attachmentDescs[i].internalFormat, m_width, m_height, 0, m_attachmentDescs[i].format, m_attachmentDescs[i].type, nullptr);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i), GL_TEXTURE_2D, m_colorTextures[i], 0);
                drawBuffers.push_back(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i));
            }

            glDrawBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
        }

        if (m_withDepth)
        {
            if (m_depthTexture == 0)
                glGenTextures(1, &m_depthTexture);

            glBindTexture(GL_TEXTURE_2D, m_depthTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, m_width, m_height, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_depthTexture, 0);
        }

        glBindTexture(GL_TEXTURE_2D, 0);
        Unbind();
    }
}
