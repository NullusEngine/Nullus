#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Settings/DriverSettings.h"

namespace
{
    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        std::string_view GetDebugName() const override { return "TestTextureView"; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        NLS::Render::RHI::RHITextureViewDesc m_desc{};
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
    };

    class TestSwapchain final : public NLS::Render::RHI::RHISwapchain
    {
    public:
        explicit TestSwapchain(std::weak_ptr<NLS::Render::RHI::RHITextureView> trackedView)
            : m_trackedView(std::move(trackedView))
        {
        }

        std::string_view GetDebugName() const override { return "TestSwapchain"; }
        const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
        uint32_t GetImageCount() const override { return 2u; }

        std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
            const std::shared_ptr<NLS::Render::RHI::RHISemaphore>&,
            const std::shared_ptr<NLS::Render::RHI::RHIFence>&) override
        {
            return std::nullopt;
        }

        void Resize(uint32_t width, uint32_t height) override
        {
            resizeCalled = true;
            resizeWidth = width;
            resizeHeight = height;
            backbufferReleasedBeforeResize = m_trackedView.expired();
            m_desc.width = width;
            m_desc.height = height;
        }

        bool resizeCalled = false;
        bool backbufferReleasedBeforeResize = false;
        uint32_t resizeWidth = 0;
        uint32_t resizeHeight = 0;

    private:
        NLS::Render::RHI::SwapchainDesc m_desc{};
        std::weak_ptr<NLS::Render::RHI::RHITextureView> m_trackedView;
    };
}

TEST(DriverSwapchainResizeTests, ReleasesFrameContextBackbufferViewsBeforeResizingSwapchain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;
    settings.framesInFlight = 1;

    NLS::Render::Context::Driver driver(settings);

    auto view = std::make_shared<TestTextureView>();
    std::weak_ptr<NLS::Render::RHI::RHITextureView> weakView = view;
    auto swapchain = std::make_shared<TestSwapchain>(weakView);

    auto& frameContext = NLS::Render::Context::DriverTestAccess::EnsureFrameContext(driver, 0);
    frameContext.swapchainBackbufferView = view;
    view.reset();

    NLS::Render::Context::DriverTestAccess::SetExplicitSwapchain(driver, swapchain);
    driver.ResizePlatformSwapchain(1664u, 941u);

    EXPECT_TRUE(swapchain->resizeCalled);
    EXPECT_EQ(swapchain->resizeWidth, 1664u);
    EXPECT_EQ(swapchain->resizeHeight, 941u);
    EXPECT_TRUE(swapchain->backbufferReleasedBeforeResize);
    EXPECT_EQ(frameContext.swapchainBackbufferView, nullptr);
}
