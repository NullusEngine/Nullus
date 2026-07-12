#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <memory>
#include <string_view>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/ArtifactManifest.h"
#include "Guid.h"
#include "Rendering/Assets/TextureArtifact.h"
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
    size_t CountTelemetryStage(
        const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
        const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
    {
        return static_cast<size_t>(std::count_if(
            records.begin(),
            records.end(),
            [stage](const auto& record)
            {
                return record.stage == stage;
            }));
    }

    void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

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

TEST(TextureResourceLifecycleTests, FailedCreateFromMemoryUploadKeepsFallbackTextureCompatibility)
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
    EXPECT_EQ(explicitDevice->textureCreateCalls, 2u);
    EXPECT_EQ(texture->GetTextureHandle()->GetDesc().extent.width, 1u);
    EXPECT_EQ(texture->GetTextureHandle()->GetDesc().extent.height, 1u);
    EXPECT_EQ(texture->width, 1u);
    EXPECT_EQ(texture->height, 1u);
}

TEST(TextureResourceLifecycleTests, CreateFromMemoryPreservesFallbackTextureCompatibility)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    uint8_t pixels[3u * 2u * 4u] {};
    std::unique_ptr<NLS::Render::Resources::Texture2D> texture(
        NLS::Render::Resources::Loaders::TextureLoader::CreateFromMemory(
            pixels,
            3u,
            2u,
            NLS::Render::Settings::ETextureFilteringMode::NEAREST,
            NLS::Render::Settings::ETextureFilteringMode::NEAREST,
            false));

    ASSERT_NE(texture, nullptr);
    ASSERT_NE(texture->GetTextureHandle(), nullptr);
    EXPECT_EQ(explicitDevice->textureCreateCalls, 2u)
        << "The generic image loader keeps its 1x1 fallback texture compatibility for older callers.";
    EXPECT_EQ(texture->GetTextureHandle()->GetDesc().extent.width, 3u);
    EXPECT_EQ(texture->GetTextureHandle()->GetDesc().extent.height, 2u);
    EXPECT_EQ(texture->width, 3u);
    EXPECT_EQ(texture->height, 2u);
}

TEST(TextureResourceLifecycleTests, CreateFromRgba8MemoryUploadsDirectly)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    uint8_t pixels[3u * 2u * 4u] {};
    std::unique_ptr<NLS::Render::Resources::Texture2D> texture(
        NLS::Render::Resources::Loaders::TextureLoader::CreateFromRgba8Memory(
            pixels,
            sizeof(pixels),
            3u,
            2u,
            NLS::Render::Settings::ETextureFilteringMode::NEAREST,
            NLS::Render::Settings::ETextureFilteringMode::NEAREST,
            false));

    ASSERT_NE(texture, nullptr);
    ASSERT_NE(texture->GetTextureHandle(), nullptr);
    EXPECT_EQ(explicitDevice->textureCreateCalls, 1u);
    EXPECT_EQ(explicitDevice->lastTextureUploadDesc.data, pixels);
    EXPECT_EQ(explicitDevice->lastTextureUploadDesc.dataSize, sizeof(pixels));
    EXPECT_EQ(explicitDevice->lastTextureUploadDesc.rowPitch, 12u);
    EXPECT_EQ(explicitDevice->lastTextureUploadDesc.slicePitch, sizeof(pixels));
    EXPECT_EQ(texture->width, 3u);
    EXPECT_EQ(texture->height, 2u);
}

TEST(TextureResourceLifecycleTests, FailedCreateFromRgba8MemoryUploadReturnsNullWithoutFallbackTexture)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    uint8_t pixels[2u * 2u * 4u] {};
    explicitDevice->failTextureCreateCall = explicitDevice->textureCreateCalls + 1u;

    std::unique_ptr<NLS::Render::Resources::Texture2D> texture(
        NLS::Render::Resources::Loaders::TextureLoader::CreateFromRgba8Memory(
            pixels,
            sizeof(pixels),
            2u,
            2u,
            NLS::Render::Settings::ETextureFilteringMode::NEAREST,
            NLS::Render::Settings::ETextureFilteringMode::NEAREST,
            false));

    EXPECT_EQ(texture, nullptr);
    EXPECT_EQ(explicitDevice->textureCreateCalls, 1u);

    texture.reset(NLS::Render::Resources::Loaders::TextureLoader::CreateFromRgba8Memory(
        pixels,
        sizeof(pixels),
        UINT32_MAX,
        UINT32_MAX,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false));
    EXPECT_EQ(texture, nullptr);
    EXPECT_EQ(explicitDevice->textureCreateCalls, 1u);
}

TEST(TextureResourceLifecycleTests, FailedTextureArtifactUploadReturnsNull)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        std::vector<uint8_t>(16u, 255u)
    });

    explicitDevice->failTextureCreateCall = explicitDevice->textureCreateCalls + 1u;
    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreateFromArtifact(
        artifact,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    EXPECT_EQ(texture, nullptr);
}

TEST(TextureResourceLifecycleTests, TextureArtifactUploadCreatesOnlyFinalTexture)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        std::vector<uint8_t>(16u, 255u)
    });

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreateFromArtifact(
        artifact,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(explicitDevice->textureCreateCalls, 1u);
    EXPECT_EQ(texture->GetTextureHandle()->GetDesc().extent.width, 2u);
    EXPECT_EQ(texture->GetTextureHandle()->GetDesc().extent.height, 2u);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
}

TEST(TextureResourceLifecycleTests, WrapExternalTexture2DSkipsFallbackTextureCreation)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::RHI::RHITextureDesc desc{};
    desc.extent.width = 4u;
    desc.extent.height = 4u;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    auto externalTexture = std::make_shared<TestTexture>(desc);

    auto texture = NLS::Render::Resources::Texture2D::WrapExternal(externalTexture, 4u, 4u);

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->GetTextureHandle(), externalTexture);
    EXPECT_EQ(explicitDevice->textureCreateCalls, 0u)
        << "Wrapping renderer-thread thumbnail textures must not create a UI-thread fallback RHI texture first.";
    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(TextureResourceLifecycleTests, TextureLoaderCreateParsesNativeTextureArtifactOnce)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_texture_loader_single_parse_" + NLS::Guid::New().ToString());

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        std::vector<uint8_t>(16u, 255u)
    });
    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    const auto texturePath = root /
        "Library" /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(
            NLS::Core::Assets::BuildArtifactStorageFileName(bytes.data(), bytes.size()));
    WriteBytes(texturePath, bytes);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        texturePath.string(),
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);
    const auto records = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();

    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(
        CountTelemetryStage(records, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeContainerParseHash),
        1u)
        << "TextureLoader::Create must not parse/hash the native container once to detect it and again to load it.";
    EXPECT_EQ(
        CountTelemetryStage(records, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactLowCopyView),
        1u);
    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
    std::filesystem::remove_all(root);
}

TEST(TextureResourceLifecycleTests, TextureLoaderCreateSkipsArtifactProbeForNonArtifactPath)
{
    NLS::Core::Assets::ClearArtifactLoadTelemetry();

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::Create(
        "Assets/Textures/Missing.png",
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    EXPECT_EQ(texture, nullptr);
    const auto records = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    EXPECT_EQ(
        CountTelemetryStage(records, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead),
        0u)
        << "Ordinary source image paths must not pay native texture artifact load/probe cost.";
    EXPECT_EQ(
        CountTelemetryStage(records, NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeContainerParseHash),
        0u);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
}

TEST(TextureResourceLifecycleTests, LoadTextureArtifactUsesBackedPixelViews)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_texture_loader_pixel_views_" + NLS::Guid::New().ToString());
    const auto texturePath = root / "texture-artifact";

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        std::vector<uint8_t>(16u, 255u)
    });

    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    WriteBytes(texturePath, bytes);

    const auto loaded = NLS::Render::Assets::LoadTextureArtifact(texturePath);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_NE(loaded->backingBytes, nullptr);
    ASSERT_EQ(loaded->mips.size(), 1u);
    EXPECT_TRUE(loaded->mips[0].pixels.empty());
    ASSERT_TRUE(loaded->mips[0].HasPixels());
    EXPECT_EQ(loaded->mips[0].PixelSize(), 16u);
    const auto* backingBegin = loaded->backingBytes->data();
    const auto* backingEnd = backingBegin + loaded->backingBytes->size();
    EXPECT_GE(loaded->mips[0].PixelData(), backingBegin);
    EXPECT_LT(loaded->mips[0].PixelData(), backingEnd);

    const auto decoded = NLS::Render::Assets::DeserializeTextureArtifact(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->backingBytes, nullptr);
    ASSERT_EQ(decoded->mips.size(), 1u);
    EXPECT_FALSE(decoded->mips[0].pixels.empty());
    EXPECT_TRUE(decoded->mips[0].pixelView.empty());

    std::filesystem::remove_all(root);
}

TEST(TextureResourceLifecycleTests, LoadTextureArtifactRecordsReadAndDeserializeTelemetry)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_texture_loader_telemetry_" + NLS::Guid::New().ToString());
    const auto texturePath = root / "texture-artifact";

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        std::vector<uint8_t>(16u, 255u)
    });

    const auto bytes = NLS::Render::Assets::SerializeTextureArtifact(artifact);
    WriteBytes(texturePath, bytes);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    const auto loaded = NLS::Render::Assets::LoadTextureArtifact(texturePath);
    const auto records = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();

    ASSERT_TRUE(loaded.has_value());
    auto findStage = [&records](const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
        -> const NLS::Core::Assets::ArtifactLoadTelemetryRecord*
    {
        const auto found = std::find_if(
            records.begin(),
            records.end(),
            [stage](const auto& record)
            {
                return record.stage == stage;
            });
        return found == records.end() ? nullptr : &*found;
    };

    const auto* fileRead = findStage(NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead);
    ASSERT_NE(fileRead, nullptr);
    EXPECT_EQ(fileRead->byteCount, bytes.size());
    EXPECT_GT(fileRead->elapsed.count(), 0);

    const auto* cpuDeserialize = findStage(NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize);
    ASSERT_NE(cpuDeserialize, nullptr);
    EXPECT_EQ(cpuDeserialize->byteCount, bytes.size());
    EXPECT_GT(cpuDeserialize->elapsed.count(), 0);

    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    std::filesystem::remove_all(root);
}

TEST(TextureResourceLifecycleTests, TextureLoaderCreateFromArtifactRecordsRuntimeAndGpuTelemetry)
{
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 2u;
    artifact.height = 2u;
    artifact.format = NLS::Render::RHI::TextureFormat::RGBA8;
    artifact.mips.push_back({
        0u,
        2u,
        2u,
        8u,
        16u,
        std::vector<uint8_t>(16u, 255u)
    });

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreateFromArtifact(
        artifact,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);
    const auto records = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();

    ASSERT_NE(texture, nullptr);
    auto findStage = [&records](const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
        -> const NLS::Core::Assets::ArtifactLoadTelemetryRecord*
    {
        const auto found = std::find_if(
            records.begin(),
            records.end(),
            [stage](const auto& record)
            {
                return record.stage == stage;
            });
        return found == records.end() ? nullptr : &*found;
    };

    const auto* runtimeCreation = findStage(NLS::Core::Assets::ArtifactLoadTelemetryStage::RuntimeResourceCreation);
    ASSERT_NE(runtimeCreation, nullptr);
    EXPECT_EQ(runtimeCreation->byteCount, 16u);
    EXPECT_GT(runtimeCreation->elapsed.count(), 0);

    const auto* gpuUpload = findStage(NLS::Core::Assets::ArtifactLoadTelemetryStage::GpuUpload);
    ASSERT_NE(gpuUpload, nullptr);
    EXPECT_EQ(gpuUpload->byteCount, 16u);
    EXPECT_GT(gpuUpload->elapsed.count(), 0);

    EXPECT_TRUE(NLS::Render::Resources::Loaders::TextureLoader::Destroy(texture));
    NLS::Core::Assets::ClearArtifactLoadTelemetry();
    NLS::Core::ServiceLocator::Remove<NLS::Render::Context::Driver>();
}

TEST(TextureResourceLifecycleTests, DefaultTextureViewCoversUploadedMipChain)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Resources::Texture texture;
    NLS::Render::RHI::RHITextureDesc desc{};
    desc.extent.width = 8u;
    desc.extent.height = 8u;
    desc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    desc.mipLevels = 4u;
    desc.arrayLayers = 1u;
    texture.SetRHITexture(std::make_shared<TestTexture>(desc));

    const auto view = texture.GetOrCreateExplicitTextureView("MipChainView");
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->GetDesc().subresourceRange.mipLevelCount, 4u);
    EXPECT_EQ(view->GetDesc().subresourceRange.arrayLayerCount, 1u);
}

TEST(TextureResourceLifecycleTests, CompressedTextureArtifactFailsClosedWhenDeviceLacksFormatCapability)
{
    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    NLS::Render::Assets::TextureArtifactData artifact;
    artifact.width = 4u;
    artifact.height = 4u;
    artifact.format = NLS::Render::RHI::TextureFormat::BC1;
    artifact.mips.push_back({
        0u,
        4u,
        4u,
        NLS::Render::RHI::CalculateTextureRowPitch(NLS::Render::RHI::TextureFormat::BC1, 4u),
        NLS::Render::RHI::CalculateTextureSlicePitch(NLS::Render::RHI::TextureFormat::BC1, 4u, 4u, 1u),
        std::vector<uint8_t>(8u, 0u)
    });

    auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreateFromArtifact(
        artifact,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        NLS::Render::Settings::ETextureFilteringMode::NEAREST,
        false);

    EXPECT_EQ(texture, nullptr);
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

TEST(TextureResourceLifecycleTests, CreateCubeMapRejectsRuntimeSourceImages)
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_cubemap_source_reject_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);

    std::vector<std::string> filePaths;
    filePaths.reserve(6u);
    for (const char* name : {"px.png", "nx.png", "py.png", "ny.png", "pz.png", "nz.png"})
    {
        const auto path = root / name;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "source image bytes";
        filePaths.push_back(path.string());
    }

    NLS::Render::Settings::DriverSettings settings;
    settings.graphicsBackend = NLS::Render::Settings::EGraphicsBackend::NONE;
    settings.enableExplicitRHI = false;

    NLS::Render::Context::Driver driver(settings);
    NLS::Core::ServiceLocator::Provide(driver);
    auto explicitDevice = std::make_shared<TestExplicitDevice>();
    NLS::Render::Context::DriverTestAccess::SetExplicitDevice(driver, explicitDevice);

    std::unique_ptr<NLS::Render::Resources::TextureCube> cubeMap(
        NLS::Render::Resources::Loaders::TextureLoader::CreateCubeMap(filePaths));

    EXPECT_EQ(cubeMap, nullptr);

    std::filesystem::remove_all(root);
}
