#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/IRHIResource.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::RHI
{
    class IRHITexture;
}

namespace NLS::Render::Buffers
{
    class NLS_RENDER_API MultiFramebuffer
    {
    public:
        struct AttachmentDesc
        {
            NLS::Render::RHI::TextureFormat format = NLS::Render::RHI::TextureFormat::RGBA8;
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
        const std::vector<std::shared_ptr<NLS::Render::RHI::IRHITexture>>& GetColorTextureResources() const { return m_colorTextureResources; }
        const std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>>& GetExplicitColorTextureHandles() const { return m_explicitColorTextures; }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitColorView(size_t index, const std::string& debugName = {}) const;
        const std::shared_ptr<NLS::Render::RHI::IRHITexture>& GetDepthTextureResource() const { return m_depthTextureResource; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetExplicitDepthTextureHandle() const { return m_explicitDepthTexture; }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitDepthView(const std::string& debugName = {}) const;

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
        std::vector<std::shared_ptr<NLS::Render::RHI::IRHITexture>> m_colorTextureResources;
        std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>> m_explicitColorTextures;
        mutable std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> m_explicitColorTextureViews;
        std::shared_ptr<NLS::Render::RHI::IRHITexture> m_depthTextureResource;
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_explicitDepthTexture;
        mutable std::shared_ptr<NLS::Render::RHI::RHITextureView> m_explicitDepthTextureView;
    };
}
