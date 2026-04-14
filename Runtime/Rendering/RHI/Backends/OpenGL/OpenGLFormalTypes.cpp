#include "Rendering/RHI/Backends/OpenGL/OpenGLFormalTypes.h"

#include "Debug/Logger.h"

namespace NLS::Render::Backend
{
    // NativeOpenGLTextureView
    NativeOpenGLTextureView::NativeOpenGLTextureView(std::shared_ptr<NLS::Render::RHI::RHITexture> texture, uint32_t viewId,
        const NLS::Render::RHI::RHITextureViewDesc& desc)
        : m_viewId(viewId)
        , m_texture(std::move(texture))
        , m_desc(desc)
    {
    }

    const NLS::Render::RHI::RHITextureViewDesc& NativeOpenGLTextureView::GetDesc() const
    {
        return m_desc;
    }

    const std::shared_ptr<NLS::Render::RHI::RHITexture>& NativeOpenGLTextureView::GetTexture() const
    {
        return m_texture;
    }

    NLS::Render::RHI::NativeHandle NativeOpenGLTextureView::GetNativeShaderResourceView()
    {
        // For OpenGL, the "view" is just the texture ID itself
        // Return as a typed handle
        return {NLS::Render::RHI::BackendType::OpenGL, reinterpret_cast<void*>(static_cast<uintptr_t>(m_viewId))};
    }

    // NativeOpenGLTexture
    NativeOpenGLTexture::NativeOpenGLTexture(uint32_t textureId)
        : m_textureId(textureId)
    {
    }

    NativeOpenGLTexture::NativeOpenGLTexture(uint32_t textureId, const NLS::Render::RHI::RHITextureDesc& desc)
        : m_textureId(textureId)
        , m_desc(desc)
    {
    }

    const NLS::Render::RHI::RHITextureDesc& NativeOpenGLTexture::GetDesc() const
    {
        return m_desc;
    }

    NLS::Render::RHI::ResourceState NativeOpenGLTexture::GetState() const
    {
        return m_state;
    }

    NLS::Render::RHI::NativeHandle NativeOpenGLTexture::GetNativeImageHandle()
    {
        return {NLS::Render::RHI::BackendType::OpenGL, reinterpret_cast<void*>(static_cast<uintptr_t>(m_textureId))};
    }

    // NativeOpenGLBuffer
    NativeOpenGLBuffer::NativeOpenGLBuffer(uint32_t bufferId)
        : m_bufferId(bufferId)
    {
    }

    NativeOpenGLBuffer::NativeOpenGLBuffer(uint32_t bufferId, const NLS::Render::RHI::RHIBufferDesc& desc)
        : m_bufferId(bufferId)
        , m_desc(desc)
    {
    }

    const NLS::Render::RHI::RHIBufferDesc& NativeOpenGLBuffer::GetDesc() const
    {
        return m_desc;
    }

    NLS::Render::RHI::ResourceState NativeOpenGLBuffer::GetState() const
    {
        return m_state;
    }

    uint64_t NativeOpenGLBuffer::GetGPUAddress() const
    {
        return static_cast<uint64_t>(m_bufferId);
    }

    NLS::Render::RHI::NativeHandle NativeOpenGLBuffer::GetNativeBufferHandle()
    {
        return {NLS::Render::RHI::BackendType::OpenGL, reinterpret_cast<void*>(static_cast<uintptr_t>(m_bufferId))};
    }
}
