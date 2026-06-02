#include <gtest/gtest.h>

#if defined(_WIN32)
#include <array>

#include <Windows.h>

#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace
{
    class TestTexture final : public NLS::Render::RHI::RHITexture
    {
    public:
        explicit TestTexture(NLS::Render::RHI::ResourceState state)
            : m_state(state)
        {
        }

        std::string_view GetDebugName() const override { return "DX12ReadbackUtilsTestsTexture"; }
        const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
        NLS::Render::RHI::ResourceState GetState() const override { return m_state; }

    private:
        NLS::Render::RHI::RHITextureDesc m_desc{};
        NLS::Render::RHI::ResourceState m_state = NLS::Render::RHI::ResourceState::Unknown;
    };
}

TEST(DX12ReadbackUtilsTests, AlignsSinglePixelReadbackRowsToDx12PitchRequirement)
{
    const auto layout = NLS::Render::RHI::DX12::BuildDX12ReadbackLayout(DXGI_FORMAT_R8G8B8A8_UNORM, 1u, 1u);

    EXPECT_EQ(layout.bytesPerPixel, 4u);
    EXPECT_EQ(layout.rowPitch, 256u);
    EXPECT_EQ(layout.readbackSize, 256u);
}

TEST(DX12ReadbackUtilsTests, PreservesAlreadyAlignedRowPitch)
{
    const auto layout = NLS::Render::RHI::DX12::BuildDX12ReadbackLayout(DXGI_FORMAT_R8G8B8A8_UNORM, 64u, 2u);

    EXPECT_EQ(layout.bytesPerPixel, 4u);
    EXPECT_EQ(layout.rowPitch, 256u);
    EXPECT_EQ(layout.readbackSize, 512u);
}

TEST(DX12ReadbackUtilsTests, UsesFormatByteSizeWhenComputingReadbackFootprint)
{
    const auto layout = NLS::Render::RHI::DX12::BuildDX12ReadbackLayout(DXGI_FORMAT_R16G16B16A16_FLOAT, 3u, 1u);

    EXPECT_EQ(layout.bytesPerPixel, 8u);
    EXPECT_EQ(layout.rowPitch, 256u);
    EXPECT_EQ(layout.readbackSize, 256u);
}

TEST(DX12ReadbackUtilsTests, PlansScratchReadbackResourceReuseUntilCapacityIsExceeded)
{
    const auto firstUse = NLS::Render::RHI::DX12::BuildDX12ReadbackScratchResourcePlan(0u, 512u);
    EXPECT_TRUE(firstUse.needsNewResource);
    EXPECT_EQ(firstUse.committedCapacity, 512u);

    const auto reuse = NLS::Render::RHI::DX12::BuildDX12ReadbackScratchResourcePlan(512u, 256u);
    EXPECT_FALSE(reuse.needsNewResource);
    EXPECT_EQ(reuse.committedCapacity, 512u);

    const auto grow = NLS::Render::RHI::DX12::BuildDX12ReadbackScratchResourcePlan(512u, 1024u);
    EXPECT_TRUE(grow.needsNewResource);
    EXPECT_EQ(grow.committedCapacity, 1024u);
}

TEST(DX12ReadbackUtilsTests, PreservesTrackedSourceStateAroundCopy)
{
    const auto states = NLS::Render::RHI::DX12::BuildDX12ReadbackBarrierStates(
        NLS::Render::RHI::ResourceState::RenderTarget);

    EXPECT_EQ(states.beforeCopy, NLS::Render::RHI::ResourceState::RenderTarget);
    EXPECT_EQ(states.afterCopy, NLS::Render::RHI::ResourceState::RenderTarget);
}

TEST(DX12ReadbackUtilsTests, UnknownSourceStateFallsBackToCommonEquivalent)
{
    const auto states = NLS::Render::RHI::DX12::BuildDX12ReadbackBarrierStates(
        NLS::Render::RHI::ResourceState::Unknown);

    EXPECT_EQ(states.beforeCopy, NLS::Render::RHI::ResourceState::Unknown);
    EXPECT_EQ(states.afterCopy, NLS::Render::RHI::ResourceState::Unknown);
}

TEST(DX12ReadbackUtilsTests, ValidatesSupportedRgbAndRgbaUnsignedByteRequests)
{
    const auto rgb = NLS::Render::RHI::DX12::ValidateDX12ReadbackRequest(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        NLS::Render::Settings::EPixelDataFormat::RGB,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE);
    EXPECT_TRUE(rgb.supported);
    EXPECT_EQ(rgb.destinationBytesPerPixel, 3u);
    EXPECT_TRUE(rgb.reason.empty());

    const auto rgba = NLS::Render::RHI::DX12::ValidateDX12ReadbackRequest(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE);
    EXPECT_TRUE(rgba.supported);
    EXPECT_EQ(rgba.destinationBytesPerPixel, 4u);
    EXPECT_TRUE(rgba.reason.empty());
}

TEST(DX12ReadbackUtilsTests, ReportsUnsupportedReadbackRequestReason)
{
    const auto unsupportedType = NLS::Render::RHI::DX12::ValidateDX12ReadbackRequest(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::FLOAT);
    EXPECT_FALSE(unsupportedType.supported);
    EXPECT_NE(unsupportedType.reason.find("UNSIGNED_BYTE"), std::string::npos);

    const auto unsupportedSource = NLS::Render::RHI::DX12::ValidateDX12ReadbackRequest(
        DXGI_FORMAT_UNKNOWN,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE);
    EXPECT_FALSE(unsupportedSource.supported);
    EXPECT_NE(unsupportedSource.reason.find("source format"), std::string::npos);
}

TEST(DX12ReadbackUtilsTests, ValidateReadPixelsInputsReturnsStatusAndMessage)
{
    auto texture = std::make_shared<TestTexture>(NLS::Render::RHI::ResourceState::ShaderRead);
    std::array<uint8_t, 16> data{};

    const auto ok = NLS::Render::RHI::DX12::ValidateDX12ReadPixelsInputs(
        texture,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        data.data());
    EXPECT_TRUE(ok.Succeeded());

    const auto invalid = NLS::Render::RHI::DX12::ValidateDX12ReadPixelsInputs(
        nullptr,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        data.data());
    EXPECT_EQ(invalid.code, NLS::Render::RHI::DX12::DX12ReadbackStatusCode::InvalidArgument);
    EXPECT_NE(invalid.message.find("texture"), std::string::npos);
}

TEST(DX12ReadbackUtilsTests, DeviceRemovedReadbackFailureMessageIncludesReason)
{
    const auto expected = NLS::Render::RHI::DX12::DX12ReadbackStatusCode::DeviceLost;
    const auto result = NLS::Render::RHI::DX12::BuildDX12DeviceRemovedReadbackFailure(
        DXGI_ERROR_DEVICE_HUNG,
        "ReadPixels preflight");

    EXPECT_EQ(result.code, expected);
    EXPECT_NE(result.message.find("ReadPixels preflight"), std::string::npos);
    EXPECT_NE(result.message.find("device removed"), std::string::npos);
    EXPECT_NE(result.message.find(std::to_string(static_cast<long>(DXGI_ERROR_DEVICE_HUNG))), std::string::npos);
}

TEST(DX12ReadbackUtilsTests, PositiveNanosecondTimeoutsRoundUpToAtLeastOneMillisecond)
{
    EXPECT_EQ(NLS::Render::RHI::DX12::ConvertDX12WaitTimeoutNanosecondsToMilliseconds(1u), 1u);
    EXPECT_EQ(NLS::Render::RHI::DX12::ConvertDX12WaitTimeoutNanosecondsToMilliseconds(999999u), 1u);
    EXPECT_EQ(NLS::Render::RHI::DX12::ConvertDX12WaitTimeoutNanosecondsToMilliseconds(1000000u), 1u);
    EXPECT_EQ(NLS::Render::RHI::DX12::ConvertDX12WaitTimeoutNanosecondsToMilliseconds(1000001u), 2u);
}

TEST(DX12ReadbackUtilsTests, ZeroNanosecondTimeoutMeansInfiniteWait)
{
    EXPECT_EQ(
        NLS::Render::RHI::DX12::ConvertDX12WaitTimeoutNanosecondsToMilliseconds(0u),
        INFINITE);
}

TEST(DX12ReadbackUtilsTests, PollingCompletedAsyncReadbackFinalizesAndAllowsNextReadback)
{
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
    {
        GTEST_SKIP() << "DX12 device unavailable on this test machine";
    }

    const std::array<uint8_t, 4> sourcePixel{ 16u, 32u, 48u, 255u };
    NLS::Render::RHI::RHITextureDesc textureDesc{};
    textureDesc.extent = { 1u, 1u, 1u };
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::Sampled;
    textureDesc.debugName = "ReadbackPollingTexture";

    NLS::Render::RHI::RHITextureUploadDesc uploadDesc{};
    uploadDesc.data = sourcePixel.data();
    uploadDesc.dataSize = sourcePixel.size();
    uploadDesc.extent = textureDesc.extent;
    uploadDesc.debugName = "ReadbackPollingTextureUpload";

    auto texture = NLS::Render::Backend::CreateNativeDX12Texture(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        textureDesc,
        uploadDesc);
    ASSERT_NE(texture, nullptr);

    NLS::Render::RHI::DX12::DX12ReadbackContext context;
    std::array<uint8_t, 4> firstReadback{};
    auto first = context.Begin(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        firstReadback.data());
    ASSERT_TRUE(first.Succeeded()) << first.message;
    ASSERT_NE(first.completion, nullptr);

    NLS::Render::RHI::RHICompletionStatus status{};
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        status = first.completion->GetStatus();
        if (status.IsComplete())
            break;
        Sleep(1);
    }
    ASSERT_EQ(status.code, NLS::Render::RHI::RHICompletionStatusCode::Success) << status.message;
    EXPECT_EQ(firstReadback, sourcePixel);

    std::array<uint8_t, 4> secondReadback{};
    auto second = context.Begin(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        secondReadback.data());
    EXPECT_TRUE(second.Succeeded()) << second.message;
}
#endif
