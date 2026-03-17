#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glad/glad.h>

#include "RenderDef.h"

namespace NLS::Render::FrameGraph
{
    struct NLS_RENDER_API OpenGLFrameGraphBuffer
    {
        struct Desc
        {
            size_t size = 0;
            GLenum target = GL_SHADER_STORAGE_BUFFER;
            GLenum usage = GL_DYNAMIC_DRAW;
        };

        GLuint id = 0;
        std::vector<std::byte> initialData;

        void create(const Desc& desc, void*);
        void destroy(const Desc&, void*);
        void preRead(const Desc& desc, uint32_t flags, void*);
        void preWrite(const Desc& desc, uint32_t flags, void*);

        static std::string toString(const Desc& desc);
    };
}
