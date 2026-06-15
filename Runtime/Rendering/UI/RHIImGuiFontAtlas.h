#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RenderDef.h"

namespace NLS::Render::RHI
{
    class RHIBuffer;
    class RHIBindingLayout;
    class RHIBindingSet;
    class RHICommandBuffer;
    class RHIDevice;
    class RHISampler;
    class RHITexture;
    class RHITextureView;
}

namespace NLS::Render::UI
{
    struct NLS_RENDER_API RHIImGuiFontAtlasRetiredResources
    {
        uint64_t retireFrameId = 0u;
        std::shared_ptr<RHI::RHITexture> texture;
        std::shared_ptr<RHI::RHITextureView> textureView;
        std::shared_ptr<RHI::RHISampler> sampler;
        std::shared_ptr<RHI::RHIBindingSet> bindingSet;
        std::shared_ptr<RHI::RHIBuffer> uploadStagingBuffer;
    };

    class NLS_RENDER_API RHIImGuiFontAtlas
    {
    public:
        bool EnsureUploaded(
            RHI::RHIDevice& device,
            RHI::RHICommandBuffer& commandBuffer,
            const std::shared_ptr<RHI::RHIBindingLayout>& bindingLayout,
            std::string& errorMessage);
        void Invalidate(uint64_t retireFrameId);
        void ReleaseRetiredResourcesUpTo(uint64_t completedFrameId);
        [[nodiscard]] uint64_t GetGeneration() const { return m_generation; }
        [[nodiscard]] bool IsUploaded() const { return m_uploaded; }
        [[nodiscard]] const std::shared_ptr<RHI::RHITextureView>& TextureView() const { return m_textureView; }
        [[nodiscard]] const std::shared_ptr<RHI::RHISampler>& Sampler() const { return m_sampler; }
        [[nodiscard]] const std::shared_ptr<RHI::RHIBindingSet>& BindingSet() const { return m_bindingSet; }

#if defined(NLS_ENABLE_TEST_HOOKS)
        void SetUploadedResourcesForTesting(
            std::shared_ptr<RHI::RHITexture> texture,
            std::shared_ptr<RHI::RHITextureView> textureView,
            std::shared_ptr<RHI::RHISampler> sampler,
            std::shared_ptr<RHI::RHIBindingSet> bindingSet);
        [[nodiscard]] size_t GetRetiredResourceCountForTesting() const { return m_retiredResources.size(); }
#endif

    private:
        void RetireCurrentResources(uint64_t retireFrameId);
        void ClearCurrentResources();

        uint64_t m_generation = 1u;
        bool m_uploaded = false;
        uint64_t m_deviceCacheIdentity = 0u;
        std::shared_ptr<RHI::RHITexture> m_texture;
        std::shared_ptr<RHI::RHITextureView> m_textureView;
        std::shared_ptr<RHI::RHISampler> m_sampler;
        std::shared_ptr<RHI::RHIBindingSet> m_bindingSet;
        std::shared_ptr<RHI::RHIBuffer> m_uploadStagingBuffer;
        std::vector<RHIImGuiFontAtlasRetiredResources> m_retiredResources;
    };
}
