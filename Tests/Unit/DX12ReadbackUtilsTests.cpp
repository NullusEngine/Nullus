#include <gtest/gtest.h>

#if defined(_WIN32)
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

#include <Windows.h>

#include "Rendering/RHI/Backends/DX12/DX12Device.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/RHI/Backends/DX12/DX12ExplicitDeviceFactory.h"
#include "Rendering/RHI/Backends/DX12/DX12ReadbackUtils.h"
#include "Rendering/RHI/Backends/DX12/DX12Resource.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Core/RHIResource.h"

namespace
{
    class ScopedReadbackSubmitLane final
    {
    public:
        ScopedReadbackSubmitLane()
        {
            auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
            executor.ResetForTesting();
            executor.Initialize(NLS::Render::Context::RhiThreadMode::None, {});
        }
        ~ScopedReadbackSubmitLane()
        {
            auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
            executor.Shutdown();
            executor.ResetForTesting();
        }
    };
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

TEST(DX12ReadbackUtilsTests, ReusesDeviceOwnedInitialUploadCommandObjectsAcrossSequentialTextures)
{
    ScopedReadbackSubmitLane submitLane;
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
    textureDesc.debugName = "InitialUploadCommandReuseTexture";

    NLS::Render::RHI::RHITextureUploadDesc uploadDesc{};
    uploadDesc.data = sourcePixel.data();
    uploadDesc.dataSize = sourcePixel.size();
    uploadDesc.extent = textureDesc.extent;
    uploadDesc.debugName = "InitialUploadCommandReuseTextureUpload";

    NLS::Render::Backend::DX12InitialUploadCommandObjects commandObjects;
    auto firstTexture = NLS::Render::Backend::CreateNativeDX12Texture(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        &commandObjects,
        textureDesc,
        uploadDesc);
    ASSERT_NE(firstTexture, nullptr);
    ASSERT_NE(commandObjects.allocator, nullptr);
    ASSERT_NE(commandObjects.commandList, nullptr);
    ID3D12CommandAllocator* const firstAllocator = commandObjects.allocator.Get();
    ID3D12GraphicsCommandList* const firstCommandList = commandObjects.commandList.Get();

    auto secondTexture = NLS::Render::Backend::CreateNativeDX12Texture(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        &commandObjects,
        textureDesc,
        uploadDesc);
    ASSERT_NE(secondTexture, nullptr);
    EXPECT_EQ(commandObjects.allocator.Get(), firstAllocator);
    EXPECT_EQ(commandObjects.commandList.Get(), firstCommandList);
}

TEST(DX12ReadbackUtilsTests, PollingCompletedAsyncReadbackFinalizesAndAllowsNextReadback)
{
    ScopedReadbackSubmitLane submitLane;
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
        nullptr,
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

TEST(DX12ReadbackUtilsTests, AllowsThreeConcurrentAsyncReadbacksBeforeApplyingBackpressure)
{
    ScopedReadbackSubmitLane submitLane;
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
    textureDesc.debugName = "ConcurrentReadbackTexture";

    NLS::Render::RHI::RHITextureUploadDesc uploadDesc{};
    uploadDesc.data = sourcePixel.data();
    uploadDesc.dataSize = sourcePixel.size();
    uploadDesc.extent = textureDesc.extent;
    uploadDesc.debugName = "ConcurrentReadbackTextureUpload";

    auto texture = NLS::Render::Backend::CreateNativeDX12Texture(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        nullptr,
        textureDesc,
        uploadDesc);
    ASSERT_NE(texture, nullptr);

    NLS::Render::RHI::DX12::DX12ReadbackContext context;
    std::array<std::array<uint8_t, 4>, 3> pixels{};
    std::array<NLS::Render::RHI::DX12::DX12ReadbackResult, 3> readbacks{};
    for (size_t index = 0u; index < readbacks.size(); ++index)
    {
        readbacks[index] = context.Begin(
            resources.device.Get(),
            resources.graphicsQueue.Get(),
            texture,
            0u,
            0u,
            1u,
            1u,
            NLS::Render::Settings::EPixelDataFormat::RGBA,
            NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
            pixels[index].data());
        ASSERT_TRUE(readbacks[index].Succeeded()) << readbacks[index].message;
        ASSERT_NE(readbacks[index].completion, nullptr);
    }

    std::array<uint8_t, 4> fourthPixel{};
    const auto fourth = context.Begin(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        fourthPixel.data());
    EXPECT_FALSE(fourth.Succeeded());
    EXPECT_NE(fourth.message.find("previous async readback"), std::string::npos);

    for (auto& readback : readbacks)
    {
        NLS::Render::RHI::RHICompletionStatus status{};
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            status = readback.completion->GetStatus();
            if (status.IsComplete())
                break;
            Sleep(1);
        }
        ASSERT_EQ(status.code, NLS::Render::RHI::RHICompletionStatusCode::Success) << status.message;
    }
    for (const auto& pixel : pixels)
        EXPECT_EQ(pixel, sourcePixel);

    std::array<uint8_t, 4> reusablePixel{};
    const auto reusable = context.Begin(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        texture,
        0u,
        0u,
        1u,
        1u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        reusablePixel.data());
    EXPECT_TRUE(reusable.Succeeded()) << reusable.message;
}

TEST(DX12ReadbackUtilsTests, DroppingAsyncReadbackCompletionKeepsResourcesUntilFenceRetires)
{
    ScopedReadbackSubmitLane submitLane;
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
    textureDesc.debugName = "ReadbackAbandonedTexture";

    NLS::Render::RHI::RHITextureUploadDesc uploadDesc{};
    uploadDesc.data = sourcePixel.data();
    uploadDesc.dataSize = sourcePixel.size();
    uploadDesc.extent = textureDesc.extent;
    uploadDesc.debugName = "ReadbackAbandonedTextureUpload";

    auto texture = NLS::Render::Backend::CreateNativeDX12Texture(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        nullptr,
        textureDesc,
        uploadDesc);
    ASSERT_NE(texture, nullptr);
    std::weak_ptr<NLS::Render::RHI::RHITexture> weakTexture = texture;

    std::array<uint8_t, 4> abandonedReadback{};
    {
        NLS::Render::RHI::DX12::DX12ReadbackContext context;
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
            abandonedReadback.data());
        ASSERT_TRUE(first.Succeeded()) << first.message;
        ASSERT_NE(first.completion, nullptr);

        first.completion.reset();
        texture.reset();
        EXPECT_NE(weakTexture.lock(), nullptr);
    }
    EXPECT_NE(weakTexture.lock(), nullptr);

    auto secondTexture = NLS::Render::Backend::CreateNativeDX12Texture(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        nullptr,
        textureDesc,
        uploadDesc);
    ASSERT_NE(secondTexture, nullptr);

    std::array<uint8_t, 4> secondReadback{};
    NLS::Render::RHI::DX12::DX12ReadbackResult second;
    NLS::Render::RHI::DX12::DX12ReadbackContext secondContext;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        second = secondContext.Begin(
            resources.device.Get(),
            resources.graphicsQueue.Get(),
            secondTexture,
            0u,
            0u,
            1u,
            1u,
            NLS::Render::Settings::EPixelDataFormat::RGBA,
            NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
            secondReadback.data());
        if (second.Succeeded())
            break;
        Sleep(1);
    }

    EXPECT_TRUE(second.Succeeded()) << second.message;
    EXPECT_EQ(weakTexture.lock(), nullptr);
}

TEST(DX12ReadbackUtilsTests, PollingCompletedAsyncBufferReadbackCopiesGpuBufferBytes)
{
    ScopedReadbackSubmitLane submitLane;
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
    {
        GTEST_SKIP() << "DX12 device unavailable on this test machine";
    }

    const std::array<uint32_t, 4u> sourceData{ 11u, 22u, 33u, 44u };
    NLS::Render::RHI::RHIBufferDesc bufferDesc;
    bufferDesc.size = sizeof(sourceData);
    bufferDesc.usage = NLS::Render::RHI::BufferUsageFlags::CopySrc |
        NLS::Render::RHI::BufferUsageFlags::ShaderRead;
    bufferDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
    bufferDesc.debugName = "DX12AsyncBufferReadbackSource";

    NLS::Render::RHI::RHIBufferUploadDesc uploadDesc;
    uploadDesc.data = sourceData.data();
    uploadDesc.dataSize = sizeof(sourceData);
    uploadDesc.debugName = "DX12AsyncBufferReadbackSourceUpload";

    auto device = NLS::Render::Backend::CreateNativeDX12ExplicitDevice(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        resources.computeQueue.Get(),
        resources.factory.Get(),
        resources.adapter.Get(),
        resources.capabilities,
        resources.vendor,
        resources.hardware);
    ASSERT_NE(device, nullptr);

    auto sourceBuffer = device->CreateBuffer(bufferDesc, uploadDesc);
    ASSERT_NE(sourceBuffer, nullptr);
    ASSERT_TRUE(sourceBuffer->GetNativeBufferHandle().IsValid());

    std::array<uint32_t, 4u> readbackData{};
    NLS::Render::RHI::RHIBufferReadbackDesc readbackDesc;
    readbackDesc.source = sourceBuffer;
    readbackDesc.sourceState = NLS::Render::RHI::ResourceState::CopySrc;
    readbackDesc.size = sizeof(readbackData);
    readbackDesc.data = readbackData.data();
    readbackDesc.debugName = "DX12AsyncBufferReadback";

    NLS::Render::RHI::DX12::DX12ReadbackContext context;
    auto result = context.BeginBuffer(
        resources.device.Get(),
        resources.graphicsQueue.Get(),
        readbackDesc);
    ASSERT_TRUE(result.Succeeded()) << result.message;
    ASSERT_NE(result.completion, nullptr);
    sourceBuffer.reset();
    readbackDesc.source.reset();

    NLS::Render::RHI::RHICompletionStatus status{};
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        status = result.completion->Poll();
        if (status.IsComplete())
            break;
        Sleep(1);
    }

    ASSERT_EQ(status.code, NLS::Render::RHI::RHICompletionStatusCode::Success) << status.message;
    EXPECT_EQ(readbackData, sourceData);
}

TEST(DX12ReadbackUtilsTests, InitialBufferUploadBatchPreservesEveryGpuBuffer)
{
    ScopedReadbackSubmitLane submitLane;
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto device = NLS::Render::Backend::CreateNativeDX12ExplicitDevice(
        resources.device.Get(), resources.graphicsQueue.Get(), resources.computeQueue.Get(),
        resources.factory.Get(), resources.adapter.Get(), resources.capabilities,
        resources.vendor, resources.hardware);
    ASSERT_NE(device, nullptr);

    std::array<std::array<uint32_t, 4u>, 32u> sourceData{};
    std::array<std::shared_ptr<NLS::Render::RHI::RHIBuffer>, 32u> buffers{};
    ASSERT_TRUE(device->BeginInitialBufferUploadBatch(
        std::vector<uint64_t>(buffers.size(), sizeof(sourceData[0]))));
    for (size_t index = 0u; index < buffers.size(); ++index)
    {
        sourceData[index] = {static_cast<uint32_t>(index + 1u), 22u, 33u, 44u};
        NLS::Render::RHI::RHIBufferDesc desc;
        desc.size = sizeof(sourceData[index]);
        desc.usage = NLS::Render::RHI::BufferUsageFlags::CopySrc |
            NLS::Render::RHI::BufferUsageFlags::ShaderRead;
        desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
        desc.debugName = "DX12BatchedBuffer" + std::to_string(index);
        NLS::Render::RHI::RHIBufferUploadDesc upload;
        upload.data = sourceData[index].data();
        upload.dataSize = sizeof(sourceData[index]);
        buffers[index] = device->CreateBuffer(desc, upload);
        ASSERT_NE(buffers[index], nullptr);
    }
    ASSERT_TRUE(device->EndInitialBufferUploadBatch());

    const std::array<uint32_t, 4u> nextData{98u, 76u, 54u, 32u};
    NLS::Render::RHI::RHIBufferDesc nextDesc;
    nextDesc.size = sizeof(nextData);
    nextDesc.usage = NLS::Render::RHI::BufferUsageFlags::CopySrc |
        NLS::Render::RHI::BufferUsageFlags::ShaderRead;
    nextDesc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
    NLS::Render::RHI::RHIBufferUploadDesc nextUpload;
    nextUpload.data = nextData.data();
    nextUpload.dataSize = sizeof(nextData);
    ASSERT_TRUE(device->BeginInitialBufferUploadBatch({sizeof(nextData)}));
    auto nextBuffer = device->CreateBuffer(nextDesc, nextUpload);
    ASSERT_NE(nextBuffer, nullptr);
    ASSERT_TRUE(device->EndInitialBufferUploadBatch());

    NLS::Render::RHI::DX12::DX12ReadbackContext readback;
    for (size_t index = 0u; index < buffers.size(); ++index)
    {
        std::array<uint32_t, 4u> received{};
        NLS::Render::RHI::RHIBufferReadbackDesc desc;
        desc.source = buffers[index];
        desc.sourceState = NLS::Render::RHI::ResourceState::CopySrc;
        desc.size = sizeof(received);
        desc.data = received.data();
        auto result = readback.BeginBuffer(resources.device.Get(), resources.graphicsQueue.Get(), desc);
        ASSERT_TRUE(result.Succeeded()) << result.message;
        ASSERT_NE(result.completion, nullptr);
        const auto status = result.completion->Wait(5'000'000'000ull);
        ASSERT_EQ(status.code, NLS::Render::RHI::RHICompletionStatusCode::Success) << status.message;
        EXPECT_EQ(received, sourceData[index]);
    }

    std::array<uint32_t, 4u> nextReceived{};
    NLS::Render::RHI::RHIBufferReadbackDesc nextReadback;
    nextReadback.source = nextBuffer;
    nextReadback.sourceState = NLS::Render::RHI::ResourceState::CopySrc;
    nextReadback.size = sizeof(nextReceived);
    nextReadback.data = nextReceived.data();
    auto nextResult = readback.BeginBuffer(resources.device.Get(), resources.graphicsQueue.Get(), nextReadback);
    ASSERT_TRUE(nextResult.Succeeded()) << nextResult.message;
    ASSERT_NE(nextResult.completion, nullptr);
    const auto nextStatus = nextResult.completion->Wait(5'000'000'000ull);
    ASSERT_EQ(nextStatus.code, NLS::Render::RHI::RHICompletionStatusCode::Success) << nextStatus.message;
    EXPECT_EQ(nextReceived, nextData);
}

TEST(DX12ReadbackUtilsTests, InitialBufferUploadBatchRejectsOverflowWithoutSubmitting)
{
    ScopedReadbackSubmitLane submitLane;
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto device = NLS::Render::Backend::CreateNativeDX12ExplicitDevice(
        resources.device.Get(), resources.graphicsQueue.Get(), resources.computeQueue.Get(),
        resources.factory.Get(), resources.adapter.Get(), resources.capabilities,
        resources.vendor, resources.hardware);
    ASSERT_NE(device, nullptr);

    const uint32_t value = 123u;
    NLS::Render::RHI::RHIBufferDesc desc;
    desc.size = sizeof(value);
    desc.usage = NLS::Render::RHI::BufferUsageFlags::CopySrc;
    desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
    NLS::Render::RHI::RHIBufferUploadDesc upload;
    upload.data = &value;
    upload.dataSize = sizeof(value);

    ASSERT_TRUE(device->BeginInitialBufferUploadBatch(std::vector<uint64_t>(64u, sizeof(value))));
    for (size_t index = 0u; index < 64u; ++index)
        ASSERT_NE(device->CreateBuffer(desc, upload), nullptr);
    EXPECT_EQ(device->CreateBuffer(desc, upload), nullptr);
    EXPECT_FALSE(device->EndInitialBufferUploadBatch());

    ASSERT_TRUE(device->BeginInitialBufferUploadBatch({sizeof(value)}));
    ASSERT_NE(device->CreateBuffer(desc, upload), nullptr);
    EXPECT_TRUE(device->EndInitialBufferUploadBatch());
}

TEST(DX12ReadbackUtilsTests, SubmittedUploadOwnersSurviveWorkerExitAndDeviceShutdownDrainsThem)
{
    ScopedReadbackSubmitLane submitLane;
    const auto resources = NLS::Render::Backend::CreateDX12DeviceResources(false);
    if (!resources.IsValid())
        GTEST_SKIP() << "DX12 device unavailable on this test machine";

    auto device = NLS::Render::Backend::CreateNativeDX12ExplicitDevice(
        resources.device.Get(), resources.graphicsQueue.Get(), resources.computeQueue.Get(),
        resources.factory.Get(), resources.adapter.Get(), resources.capabilities,
        resources.vendor, resources.hardware);
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(NLS::Render::Backend::GetPendingDX12InitialUploadStagingBytes(resources.device.Get()), 0u);

    const std::array<uint32_t, 4u> values{12u, 34u, 56u, 78u};
    NLS::Render::RHI::RHIBufferDesc desc;
    desc.size = sizeof(values);
    desc.usage = NLS::Render::RHI::BufferUsageFlags::CopySrc;
    desc.memoryUsage = NLS::Render::RHI::MemoryUsage::GPUOnly;
    NLS::Render::RHI::RHIBufferUploadDesc upload;
    upload.data = values.data();
    upload.dataSize = sizeof(values);
    std::shared_ptr<NLS::Render::RHI::RHIBuffer> buffer;
    std::thread worker([&]
    {
        ASSERT_TRUE(device->BeginInitialBufferUploadBatch({sizeof(values)}));
        buffer = device->CreateBuffer(desc, upload);
        ASSERT_NE(buffer, nullptr);
        ASSERT_TRUE(device->EndInitialBufferUploadBatch());
    });
    worker.join();
    ASSERT_NE(buffer, nullptr);
    EXPECT_TRUE(device->HasPendingInitialUploads());
    EXPECT_EQ(NLS::Render::Backend::GetPendingDX12InitialUploadStagingBytes(resources.device.Get()), sizeof(values));

    std::array<uint32_t, 4u> received{};
    NLS::Render::RHI::DX12::DX12ReadbackContext readback;
    NLS::Render::RHI::RHIBufferReadbackDesc readDesc;
    readDesc.source = buffer;
    readDesc.sourceState = NLS::Render::RHI::ResourceState::CopySrc;
    readDesc.size = sizeof(received);
    readDesc.data = received.data();
    auto result = readback.BeginBuffer(resources.device.Get(), resources.graphicsQueue.Get(), readDesc);
    ASSERT_TRUE(result.Succeeded()) << result.message;
    ASSERT_NE(result.completion, nullptr);
    const auto status = result.completion->Wait(5'000'000'000ull);
    ASSERT_EQ(status.code, NLS::Render::RHI::RHICompletionStatusCode::Success) << status.message;
    EXPECT_EQ(received, values);

    device.reset();
    EXPECT_FALSE(NLS::Render::Backend::HasPendingDX12InitialUploads(resources.device.Get()));
    EXPECT_EQ(NLS::Render::Backend::GetPendingDX12InitialUploadStagingBytes(resources.device.Get()), 0u);
}
#endif
