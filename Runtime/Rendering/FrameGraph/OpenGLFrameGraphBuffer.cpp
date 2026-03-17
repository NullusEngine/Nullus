#include "Rendering/FrameGraph/OpenGLFrameGraphBuffer.h"

namespace NLS::Render::FrameGraph
{
    void OpenGLFrameGraphBuffer::create(const Desc& desc, void*)
    {
        if (id != 0)
            return;

        glGenBuffers(1, &id);
        glBindBuffer(desc.target, id);
        glBufferData(desc.target, static_cast<GLsizeiptr>(desc.size), initialData.empty() ? nullptr : initialData.data(), desc.usage);
        glBindBuffer(desc.target, 0);
    }

    void OpenGLFrameGraphBuffer::destroy(const Desc&, void*)
    {
        if (id == 0)
            return;

        glDeleteBuffers(1, &id);
        id = 0;
    }

    void OpenGLFrameGraphBuffer::preRead(const Desc& desc, uint32_t flags, void*)
    {
        if (id == 0)
            return;

        glBindBufferBase(desc.target, flags == 0xFFFFFFFFu ? 0u : flags, id);
    }

    void OpenGLFrameGraphBuffer::preWrite(const Desc& desc, uint32_t flags, void*)
    {
        preRead(desc, flags, nullptr);
    }

    std::string OpenGLFrameGraphBuffer::toString(const Desc& desc)
    {
        return std::to_string(desc.size) + " bytes";
    }
}
