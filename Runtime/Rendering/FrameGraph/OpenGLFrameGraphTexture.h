#pragma once

#include <cstdint>
#include <string>

#include <glad/glad.h>

#include "RenderDef.h"

namespace NLS::Render::FrameGraph
{
    enum class TextureUsage : uint32_t
    {
        Sampled = 1u << 0,
        ColorAttachment = 1u << 1,
        DepthStencilAttachment = 1u << 2,
        Storage = 1u << 3
    };

    inline constexpr TextureUsage operator|(TextureUsage lhs, TextureUsage rhs)
    {
        return static_cast<TextureUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    struct NLS_RENDER_API OpenGLFrameGraphTexture
    {
        struct Desc
        {
            uint16_t width = 0;
            uint16_t height = 0;
            GLenum target = GL_TEXTURE_2D;
            GLint internalFormat = GL_RGBA8;
            GLenum format = GL_RGBA;
            GLenum type = GL_UNSIGNED_BYTE;
            GLenum minFilter = GL_NEAREST;
            GLenum magFilter = GL_NEAREST;
            GLenum wrapS = GL_CLAMP_TO_EDGE;
            GLenum wrapT = GL_CLAMP_TO_EDGE;
            TextureUsage usage = TextureUsage::Sampled;
        };

        GLuint id = 0;

        void create(const Desc& desc, void*);
        void destroy(const Desc&, void*);
        void preRead(const Desc&, uint32_t, void*);
        void preWrite(const Desc&, uint32_t, void*);

        static std::string toString(const Desc& desc);
    };
}
