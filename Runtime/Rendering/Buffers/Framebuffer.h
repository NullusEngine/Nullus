#pragma once

#include <deque>
#include <memory>
#include <string>

#include "Rendering/RHI/Core/RHIResource.h"

namespace NLS::Render::Buffers
{
/**
 * Backend-neutral offscreen render-target wrapper over the formal RHI surface.
 */
class NLS_RENDER_API Framebuffer
{
public:
    /**
     * Create the framebuffer
     * @param p_width
     * @param p_height
     */
    Framebuffer(
        uint16_t p_width = 0,
        uint16_t p_height = 0,
        NLS::Render::RHI::TextureColorSpace colorSpace =
            NLS::Render::RHI::TextureColorSpace::Linear);

    /**
     * Destructor
     */
    ~Framebuffer();

    /**
     * Defines a new size for the framebuffer
     * @param p_width
     * @param p_height
     * @param p_forceUpdate Force the resizing operation even if the width and height didn't change
     */
    void Resize(uint16_t p_width, uint16_t p_height, bool p_forceUpdate = false);
    void SetOptimizedColorClearValue(float r, float g, float b, float a = 1.0f);

    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetExplicitTextureHandle() const;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitColorView(const std::string& debugName = {}) const;

    const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetExplicitDepthStencilTextureHandle() const;
    std::shared_ptr<NLS::Render::RHI::RHITextureView> GetOrCreateExplicitDepthStencilView(const std::string& debugName = {}) const;

private:
    struct RetiredResourceSet
    {
        std::shared_ptr<NLS::Render::RHI::RHITexture> colorTexture;
        std::shared_ptr<NLS::Render::RHI::RHITexture> depthStencilTexture;
        std::shared_ptr<NLS::Render::RHI::RHITextureView> colorView;
        std::shared_ptr<NLS::Render::RHI::RHITextureView> depthStencilView;

        bool HasResources() const;
        void Reset();
    };

    void Release();
    void ResetCurrentResources();
    void RetireCurrentResources();
    void PruneRetiredResources();

    uint16_t m_width = 0;
    uint16_t m_height = 0;
    NLS::Render::RHI::TextureColorSpace m_colorSpace =
        NLS::Render::RHI::TextureColorSpace::Linear;
    NLS::Render::RHI::RHITextureDesc::OptimizedClearValue m_colorOptimizedClearValue =
        NLS::Render::RHI::RHITextureDesc::OptimizedClearValue::Color();

    std::shared_ptr<NLS::Render::RHI::RHITexture> m_explicitRenderTexture;
    std::shared_ptr<NLS::Render::RHI::RHITexture> m_explicitDepthStencilTexture;
    mutable std::shared_ptr<NLS::Render::RHI::RHITextureView> m_explicitRenderTextureView;
    mutable std::shared_ptr<NLS::Render::RHI::RHITextureView> m_explicitDepthStencilTextureView;
    std::deque<RetiredResourceSet> m_retiredResourceSets;
};
} // namespace NLS::Render::Buffers
