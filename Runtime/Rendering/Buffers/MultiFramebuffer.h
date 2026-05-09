#pragma once

#include <deque>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/RHITypes.h"

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
        bool IsInitialized() const;

        const std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>>& GetExplicitColorTextureHandles() const { return m_explicitColorTextures; }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitColorView(size_t index, const std::string& debugName = {}) const;
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetExplicitDepthTextureHandle() const { return m_explicitDepthTexture; }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitDepthView(const std::string& debugName = {}) const;

    private:
        struct RetiredResourceSet
        {
            std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>> colorTextures;
            std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> colorTextureViews;
            std::shared_ptr<NLS::Render::RHI::RHITexture> depthTexture;
            std::shared_ptr<NLS::Render::RHI::RHITextureView> depthTextureView;

            bool HasResources() const;
            void Reset();
        };

        void Release();
        void ResetCurrentResources();
        void RetireCurrentResources();
        void PruneRetiredResources();
        bool Allocate(uint16_t width, uint16_t height);

    private:
        uint16_t m_width = 0;
        uint16_t m_height = 0;
        bool m_withDepth = true;
        std::vector<AttachmentDesc> m_attachmentDescs;
        std::vector<std::shared_ptr<NLS::Render::RHI::RHITexture>> m_explicitColorTextures;
        mutable std::vector<std::shared_ptr<NLS::Render::RHI::RHITextureView>> m_explicitColorTextureViews;
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_explicitDepthTexture;
        mutable std::shared_ptr<NLS::Render::RHI::RHITextureView> m_explicitDepthTextureView;
        std::deque<RetiredResourceSet> m_retiredResourceSets;
    };
}
