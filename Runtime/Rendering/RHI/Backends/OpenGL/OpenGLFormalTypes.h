#pragma once

#include <cstdint>
#include <memory>

#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Backend
{
    // Forward declarations
    class NativeOpenGLTexture;

    // Native OpenGL texture view for Formal RHI
    // OpenGL uses GLuint as native handles
    class NLS_RENDER_API NativeOpenGLTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        NativeOpenGLTextureView(std::shared_ptr<NLS::Render::RHI::RHITexture> texture, uint32_t viewId,
            const NLS::Render::RHI::RHITextureViewDesc& desc);

        NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override;

        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override;
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override;

        uint32_t GetViewId() const { return m_viewId; }

    private:
        uint32_t m_viewId = 0;
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc;
    };

    // Native OpenGL texture for Formal RHI
    class NLS_RENDER_API NativeOpenGLTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit NativeOpenGLTexture(uint32_t textureId);
        explicit NativeOpenGLTexture(uint32_t textureId, const NLS::Render::RHI::RHITextureDesc& desc);

        uint32_t GetTextureId() const { return m_textureId; }

        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override;
        NLS::Render::RHI::ResourceState GetState() const override;
        NLS::Render::RHI::NativeHandle GetNativeImageHandle() override;

    private:
        uint32_t m_textureId = 0;
        NLS::Render::RHI::RHITextureDesc m_desc;
        NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
    };

    // Native OpenGL buffer for Formal RHI
    class NLS_RENDER_API NativeOpenGLBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        explicit NativeOpenGLBuffer(uint32_t bufferId);
        explicit NativeOpenGLBuffer(uint32_t bufferId, const NLS::Render::RHI::RHIBufferDesc& desc);

        uint32_t GetBufferId() const { return m_bufferId; }

        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override;
        NLS::Render::RHI::ResourceState GetState() const override;
        uint64_t GetGPUAddress() const override;
        NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override;

    private:
        uint32_t m_bufferId = 0;
        NLS::Render::RHI::RHIBufferDesc m_desc;
        NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
    };
}
