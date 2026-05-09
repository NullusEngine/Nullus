#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include "Core/ServiceLocator.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "Rendering/Resources/Texture.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include <Image.h>

namespace
{
    class TestAdapter final : public NLS::Render::RHI::RHIAdapter
    {
    public:
        std::string_view GetDebugName() const override { return "TextureResourceLifecycleTestsAdapter"; }
        NLS::Render::RHI::NativeBackendType GetBackendType() const override { return NLS::Render::RHI::NativeBackendType::DX12; }
        std::string_view GetVendor() const override { return "TestVendor"; }
        std::string_view GetHardware() const override { return "TestHardware"; }
    };

    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit TestTexture(NLS::Render::RHI::RHITextureDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return NLS::Render::RHI::ResourceState::Unknown; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc {};
    };

    class TestTextureView final : public NLS::Render::RHI::RHITextureView
    {
    public:
        TestTextureView(
            std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
            NLS::Render::RHI::RHITextureViewDesc desc)
            : m_texture(std::move(texture))
            , m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
        const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }

    private:
        std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
        NLS::Render::RHI::RHITextureViewDesc m_desc {};
    };

    class TestExplicitDevice final : public NLS::Render::RHI::RHIDevice
    {
    public:
        using NLS::Render::RHI::RHIDevice::CreateBuffer;
        using NLS::Render::RHI::RHIDevice::CreateTexture;

        TestExplicitDevice()
            : m_adapter(std::make_shared<TestAdapter>())
        {
            m_nativeDeviceInfo.backend = NLS::Render::RHI::NativeBackendType::DX12;
            m_capabilities.backendReady = true;
            m_capabilities.supportsGraphics = true;
            m_capabilities.supportsCurrentSceneRenderer = true;
        }

        std::string_view GetDebugName() const override { return "TextureResourceLifecycleTestsDevice"; }
        const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
        const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
        NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override { return m_nativeDeviceInfo; }
        bool IsBackendReady() const override { return true; }
        std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(const NLS::Render::RHI::SwapchainDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
            const NLS::Render::RHI::RHIBufferDesc&,
            const NLS::Render::RHI::RHIBufferUploadDesc&) override
        {
            return nullptr;
        }
        std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
            const NLS::Render::RHI::RHITextureDesc& desc,
            const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override
        {
            ++textureCreateCalls;
            lastTextureUploadDesc = uploadDesc;
            if (failTextureCreateCall != 0u && textureCreateCalls == failTextureCreateCall)
                return nullptr;

            return std::make_shared<TestTexture>(desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
            const NLS::Render::RHI::RHITextureViewDesc& desc) override
        {
            return std::make_shared<TestTextureView>(texture, desc);
        }
        std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(const NLS::Render::RHI::SamplerDesc&, std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(const NLS::Render::RHI::RHIBindingLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(const NLS::Render::RHI::RHIBindingSetDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(const NLS::Render::RHI::RHIPipelineLayoutDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(const NLS::Render::RHI::RHIShaderModuleDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(const NLS::Render::RHI::RHIGraphicsPipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(NLS::Render::RHI::QueueType, std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string = {}) override { return nullptr; }
        std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string = {}) override { return nullptr; }
        NLS::Render::RHI::RHIUpdateResult UpdateTexture(const NLS::Render::RHI::RHITextureUpdateDesc& desc) override
        {
            ++textureUpdateCalls;
            lastTextureUpdateDesc = desc;
            return textureUpdateResult;
        }
        void ReadPixels(
            const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
            uint32_t,
            uint32_t,
            uint32_t,
            uint32_t,
            NLS::Render::Settings::EPixelDataFormat,
            NLS::Render::Settings::EPixelDataType,
            void*) override {}

        size_t textureCreateCalls = 0u;
        size_t failTextureCreateCall = 0u;
        size_t textureUpdateCalls = 0u;
        NLS::Render::RHI::RHITextureUploadDesc lastTextureUploadDesc {};
        NLS::Render::RHI::RHITextureUpdateDesc lastTextureUpdateDesc {};
        NLS::Render::RHI::RHIUpdateResult textureUpdateResult {
            NLS::Render::RHI::RHIUpdateStatusCode::Success,
            {}
        };

    private:
        std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
        NLS::Render::RHI::NativeRenderDeviceInfo m_nativeDeviceInfo {};
        NLS::Render::RHI::RHIDeviceCapabilities m_capabilities {};
    };

    class TestTextureResource final : public NLS::Render::Resources::Texture
    {
    public:
        using NLS::Render::Resources::Texture::Texture;

        bool RecreateForTest(uint32_t width, uint32_t height, const void* initialData)
        {
            const size_t initialDataSize = initialData != nullptr
                ? static_cast<size_t>(width) * static_cast<size_t>(height) * sizeof(uint32_t)
                : 0u;
            return RecreateRHITextureIfNeeded(
                width,
                height,
                NLS::Render::RHI::TextureFormat::RGBA8,
                NLS::Render::RHI::TextureFilter::Nearest,
                NLS::Render::RHI::TextureFilter::Nearest,
                NLS::Render::RHI::TextureWrap::Repeat,
                NLS::Render::RHI::TextureWrap::Repeat,
                false,
                initialData,
                initialDataSize);
        }
    };
}

TEST(TextureResourceLifecycleTests, FailedRecreateKeepsPreviousTextureAndView)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    TestTextureResource texture;
    const auto previousTexture = texture.GetTextureHandle();
    const auto previousView = texture.GetOrCreateExplicitTextureView("PreviousTextureView");
    ASSERT_NE(previousTexture, nullptr);
    ASSERT_NE(previousView, nullptr);

    const uint32_t pixel = 0xffffffffu;
    explicitDevice->failTextureCreateCall = explicitDevice->textureCreateCalls + 1u;
    texture.RecreateForTest(2u, 2u, &pixel);

    ASSERT_EQ(texture.GetTextureHandle(), previousTexture);
    ASSERT_EQ(texture.GetOrCreateExplicitTextureView("AfterFailedRecreateView"), previousView);
    EXPECT_EQ(texture.GetTextureHandle()->GetDesc().extent.width, 1u);
    EXPECT_EQ(texture.GetTextureHandle()->GetDesc().extent.height, 1u);
}

TEST(TextureResourceLifecycleTests, SameSizeTextureDataUpdateUsesInPlaceUploadInsteadOfRecreate)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    TestTextureResource texture;
    const uint32_t firstPixels[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
    ASSERT_TRUE(texture.RecreateForTest(2u, 2u, firstPixels));
    const auto createdTexture = texture.GetTextureHandle();
    const size_t createCallsAfterFirstUpload = explicitDevice->textureCreateCalls;

    const uint32_t updatedPixels[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };
    ASSERT_TRUE(texture.RecreateForTest(2u, 2u, updatedPixels));

    EXPECT_EQ(texture.GetTextureHandle(), createdTexture);
    EXPECT_EQ(explicitDevice->textureCreateCalls, createCallsAfterFirstUpload);
    ASSERT_EQ(explicitDevice->textureUpdateCalls, 1u);
    EXPECT_EQ(explicitDevice->lastTextureUpdateDesc.texture, createdTexture);
    EXPECT_EQ(explicitDevice->lastTextureUpdateDesc.extent.width, 2u);
    EXPECT_EQ(explicitDevice->lastTextureUpdateDesc.extent.height, 2u);
    EXPECT_EQ(explicitDevice->lastTextureUpdateDesc.rowPitch, 8u);
}

TEST(TextureResourceLifecycleTests, FailedCreateFromMemoryUploadKeepsTexture2DMetadataAlignedWithFallbackTexture)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    uint8_t pixels[2u * 2u * 4u] {};
    explicitDevice->failTextureCreateCall = explicitDevice->textureCreateCalls + 2u;

    std::unique_ptr<NLS::Render::Resources::Texture2D> texture(
        NLS::Render::Resources::Loaders::TextureLoader::CreateFromMemory(
            pixels,
            2u,
            2u,
            NLS::Render::Settings::ETextureFilteringMode::NEAREST,
            NLS::Render::Settings::ETextureFilteringMode::NEAREST,
            false));

    ASSERT_NE(texture, nullptr);
    ASSERT_NE(texture->GetTextureHandle(), nullptr);
    EXPECT_EQ(texture->GetTextureHandle()->GetDesc().extent.width, 1u);
    EXPECT_EQ(texture->GetTextureHandle()->GetDesc().extent.height, 1u);
    EXPECT_EQ(texture->width, 1u);
    EXPECT_EQ(texture->height, 1u);
}

TEST(TextureResourceLifecycleTests, FailedTextureCubeUploadReportsFailureAndKeepsFallbackTexture)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::vector<NLS::Image> images;
    images.reserve(NLS::Render::RHI::GetTextureLayerCount(NLS::Render::RHI::TextureDimension::TextureCube));
    std::vector<const NLS::Image*> imagePtrs;
    imagePtrs.reserve(NLS::Render::RHI::GetTextureLayerCount(NLS::Render::RHI::TextureDimension::TextureCube));
    for (uint32_t i = 0; i < NLS::Render::RHI::GetTextureLayerCount(NLS::Render::RHI::TextureDimension::TextureCube); ++i)
    {
        images.emplace_back(2, 2, 4);
        imagePtrs.push_back(&images.back());
    }

    NLS::Render::Resources::TextureCube cubeMap;
    const auto previousTexture = cubeMap.GetTextureHandle();
    ASSERT_NE(previousTexture, nullptr);

    explicitDevice->failTextureCreateCall = explicitDevice->textureCreateCalls + 1u;
    EXPECT_FALSE(cubeMap.SetTextureResource(imagePtrs));
    EXPECT_EQ(cubeMap.GetTextureHandle(), previousTexture);
    EXPECT_EQ(cubeMap.GetTextureHandle()->GetDesc().extent.width, 1u);
    EXPECT_EQ(cubeMap.GetTextureHandle()->GetDesc().extent.height, 1u);
}

TEST(TextureResourceLifecycleTests, CreateCubeMapReturnsNullWhenSourceImagesCannotLoad)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    const std::vector<std::string> filePaths {
        "__missing_cubemap_px__.png",
        "__missing_cubemap_nx__.png",
        "__missing_cubemap_py__.png",
        "__missing_cubemap_ny__.png",
        "__missing_cubemap_pz__.png",
        "__missing_cubemap_nz__.png"
    };

    std::unique_ptr<NLS::Render::Resources::TextureCube> cubeMap(
        NLS::Render::Resources::Loaders::TextureLoader::CreateCubeMap(filePaths));

    EXPECT_EQ(cubeMap, nullptr);
}
