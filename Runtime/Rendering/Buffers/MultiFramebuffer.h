#pragma once

#include <cstdint>
#include <vector>

#include <glad/glad.h>

#include "RenderDef.h"

namespace NLS::Render::Buffers
{
    class NLS_RENDER_API MultiFramebuffer
    {
    public:
        struct AttachmentDesc
        {
            GLenum internalFormat = GL_RGBA8;
            GLenum format = GL_RGBA;
            GLenum type = GL_UNSIGNED_BYTE;
        };

        MultiFramebuffer() = default;
        MultiFramebuffer(uint16_t width, uint16_t height, const std::vector<AttachmentDesc>& colorAttachments, bool withDepth = true);
        ~MultiFramebuffer();

        MultiFramebuffer(const MultiFramebuffer&) = delete;
        MultiFramebuffer& operator=(const MultiFramebuffer&) = delete;

        void Init(uint16_t width, uint16_t height, const std::vector<AttachmentDesc>& colorAttachments, bool withDepth = true);
        void Resize(uint16_t width, uint16_t height);
        void Bind() const;
        void Unbind() const;

        uint32_t GetID() const { return m_bufferId; }
        const std::vector<uint32_t>& GetColorTextures() const { return m_colorTextures; }
        uint32_t GetDepthTexture() const { return m_depthTexture; }

    private:
        void Release();
        void Allocate();

    private:
        uint16_t m_width = 0;
        uint16_t m_height = 0;
        bool m_withDepth = true;
        uint32_t m_bufferId = 0;
        uint32_t m_depthTexture = 0;
        std::vector<AttachmentDesc> m_attachmentDescs;
        std::vector<uint32_t> m_colorTextures;
    };
}
