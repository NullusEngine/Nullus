#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace NLS::Render::Context
{
    class Driver;
}

namespace NLS::Render::Entities
{
    class Camera;
}

namespace NLS::Render::RHI
{
    class RHITextureView;
}

namespace NLS::Editor::Rendering
{
    // A self-contained Metal fallback for editor viewports while the generic scene RHI is incomplete.
    class MetalViewportRenderer final
    {
    public:
        ~MetalViewportRenderer();

        MetalViewportRenderer(const MetalViewportRenderer&) = delete;
        MetalViewportRenderer& operator=(const MetalViewportRenderer&) = delete;

        bool Render(uint16_t width, uint16_t height, const NLS::Render::Entities::Camera* camera);
        const std::shared_ptr<NLS::Render::RHI::RHITextureView>& GetOutputTextureView() const;

    private:
        struct Impl;

        explicit MetalViewportRenderer(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_impl;

        friend std::unique_ptr<MetalViewportRenderer> CreateMetalViewportRenderer(
            NLS::Render::Context::Driver& driver,
            std::string debugName);
    };

    std::unique_ptr<MetalViewportRenderer> CreateMetalViewportRenderer(
        NLS::Render::Context::Driver& driver,
        std::string debugName);
}
