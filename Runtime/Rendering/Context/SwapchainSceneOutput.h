#pragma once

#include <cstdint>
#include <memory>

#include "Rendering/Context/RenderScenePackage.h"
#include "Rendering/RHI/Core/RHISwapchain.h"

namespace NLS::Render::Resources { class Shader; }

namespace NLS::Render::Context
{
    class Driver;

    class SwapchainSceneOutput final
    {
    public:
        void Prepare(Driver& driver, RHI::RHIFrameContext& frame, RenderScenePackage& package,
            RHI::TextureFormat outputFormat);

    private:
        void EnsurePipeline(const std::shared_ptr<RHI::RHIDevice>& device, RHI::TextureFormat format);

        uint64_t m_deviceIdentity = 0u;
        RHI::TextureFormat m_outputFormat = RHI::TextureFormat::RGBA8;
        std::shared_ptr<RHI::RHIBindingLayout> m_bindingLayout;
        std::shared_ptr<RHI::RHISampler> m_sampler;
        std::shared_ptr<RHI::RHIGraphicsPipeline> m_pipeline;
        std::shared_ptr<Resources::Shader> m_ownedShader;
    };
}
