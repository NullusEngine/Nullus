#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <Guid.h>
#include "Rendering/ShaderCompiler/ShaderCompiler.h"
#include <array>
#include <cstdint>
#include <memory>

#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Core/RHISync.h"
#endif

namespace
{
#if defined(__APPLE__)
    std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateTestMetalDevice()
    {
        auto device = NLS::Render::Backend::CreateMetalRhiDevice(false);
        if (device == nullptr || !device->IsBackendReady())
            return nullptr;
        return device;
    }

    /**
     * Native Metal resources and queue submissions funnel through the global
     * command-list executor, so a device test must own its lane instead of
     * depending on whatever state an earlier test left behind.
     */
    class ScopedMetalSubmitLane final
    {
    public:
        ScopedMetalSubmitLane()
        {
            auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
            executor.ResetForTesting();
            executor.Initialize(NLS::Render::Context::RhiThreadMode::None, {});
        }

        ~ScopedMetalSubmitLane()
        {
            auto& executor = NLS::Render::Context::RhiCommandListExecutor::Get();
            executor.Shutdown();
            executor.ResetForTesting();
        }

        ScopedMetalSubmitLane(const ScopedMetalSubmitLane&) = delete;
        ScopedMetalSubmitLane& operator=(const ScopedMetalSubmitLane&) = delete;
    };
#endif
}

#if defined(__APPLE__)
TEST(MetalRhiParityTests, MetalRejectsSamplerStateThatItCannotRepresent)
{
    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    NLS::Render::RHI::SamplerDesc mipBiasDesc;
    mipBiasDesc.mipLodBias = 0.25f;
    EXPECT_EQ(device->CreateSampler(mipBiasDesc, "UnsupportedMipBias"), nullptr);

    NLS::Render::RHI::SamplerDesc customBorderDesc;
    customBorderDesc.wrapU = NLS::Render::RHI::TextureWrap::ClampToBorder;
    customBorderDesc.borderColor = { 0.25f, 0.5f, 0.75f, 1.0f };
    EXPECT_EQ(device->CreateSampler(customBorderDesc, "UnsupportedBorderColor"), nullptr);

    customBorderDesc.wrapU = NLS::Render::RHI::TextureWrap::ClampToEdge;
    EXPECT_NE(device->CreateSampler(customBorderDesc, "UnusedCustomBorderColor"), nullptr)
        << "An unused border color does not change sampling and should remain valid.";
}

TEST(MetalRhiParityTests, MetalCreatesEveryNativeBorderColor)
{
    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    const std::array<std::array<float, 4>, 3> supportedColors = {
        std::array<float, 4> { 0.0f, 0.0f, 0.0f, 0.0f },
        std::array<float, 4> { 0.0f, 0.0f, 0.0f, 1.0f },
        std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f }
    };

    for (size_t index = 0u; index < supportedColors.size(); ++index)
    {
        NLS::Render::RHI::SamplerDesc desc;
        desc.wrapU = NLS::Render::RHI::TextureWrap::ClampToBorder;
        desc.borderColor = supportedColors[index];
        EXPECT_NE(device->CreateSampler(desc, "SupportedBorderColor" + std::to_string(index)), nullptr);
    }
}

TEST(MetalRhiParityTests, MetalSubmitsParallelRecordedCommandBuffersInArrayOrder)
{
    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    auto queue = device->GetQueue(NLS::Render::RHI::QueueType::Graphics);
    auto firstPool = device->CreateCommandPool(
        NLS::Render::RHI::QueueType::Graphics,
        "OrderedSubmissionPool0");
    auto secondPool = device->CreateCommandPool(
        NLS::Render::RHI::QueueType::Graphics,
        "OrderedSubmissionPool1");
    ASSERT_NE(queue, nullptr);
    ASSERT_NE(firstPool, nullptr);
    ASSERT_NE(secondPool, nullptr);

    auto firstCommandBuffer = firstPool->CreateCommandBuffer("OrderedSubmission0");
    auto secondCommandBuffer = secondPool->CreateCommandBuffer("OrderedSubmission1");
    ASSERT_NE(firstCommandBuffer, nullptr);
    ASSERT_NE(secondCommandBuffer, nullptr);
    firstCommandBuffer->Begin();
    ASSERT_TRUE(firstCommandBuffer->IsRecording());
    firstCommandBuffer->End();
    secondCommandBuffer->Begin();
    ASSERT_TRUE(secondCommandBuffer->IsRecording());
    secondCommandBuffer->End();

    auto completionFence = device->CreateFence("OrderedSubmissionFence");
    ASSERT_NE(completionFence, nullptr);
    NLS::Render::RHI::RHISubmitDesc submitDesc;
    submitDesc.commandBuffers = { firstCommandBuffer, secondCommandBuffer };
    submitDesc.signalFence = completionFence;
    const auto result = queue->SubmitChecked(submitDesc);

    EXPECT_TRUE(result.Succeeded()) << result.message;
    EXPECT_TRUE(result.mayHaveQueuedGpuWork);
    EXPECT_TRUE(result.frameFenceSignalQueued);
    EXPECT_TRUE(completionFence->Wait(5'000'000'000ull))
        << "The fence attached to the final ordered command buffer did not complete.";
}

TEST(MetalRhiParityTests, MetalExecutesNonZeroOffsetPushConstantWrites)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    constexpr std::string_view shaderSource = R"(
#include <metal_stdlib>
using namespace metal;

kernel void PushConstantOffsetTest(
    constant uint4& values [[buffer(8)]],
    device uint4* output [[buffer(9)]])
{
    output[0] = values;
}
)";

    RHIShaderModuleDesc shaderDesc;
    shaderDesc.stage = ShaderStage::Compute;
    shaderDesc.entryPoint = "PushConstantOffsetTest";
    shaderDesc.bytecode.assign(shaderSource.begin(), shaderSource.end());
    shaderDesc.debugName = "MetalPushConstantOffsetShader";
    auto shader = device->CreateShaderModule(shaderDesc);
    ASSERT_NE(shader, nullptr);

    RHIBindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.entries.push_back({
        "Output",
        BindingType::StorageBuffer,
        0u,
        1u,
        1u,
        ShaderStageMask::Compute,
        0u,
        sizeof(uint32_t) * 4u
    });
    auto bindingLayout = device->CreateBindingLayout(bindingLayoutDesc);
    ASSERT_NE(bindingLayout, nullptr);

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.bindingLayouts.push_back(bindingLayout);
    pipelineLayoutDesc.pushConstants.push_back({ ShaderStageMask::Compute, 0u, 16u, 0u, 0u });
    pipelineLayoutDesc.debugName = "MetalPushConstantOffsetLayout";
    auto pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout, nullptr);

    RHIComputePipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = pipelineLayout;
    pipelineDesc.computeShader = shader;
    pipelineDesc.debugName = "MetalPushConstantOffsetPipeline";
    auto pipeline = device->CreateComputePipeline(pipelineDesc);
    ASSERT_NE(pipeline, nullptr);

    RHIBufferDesc outputDesc;
    outputDesc.size = sizeof(uint32_t) * 4u;
    outputDesc.usage = BufferUsageFlags::Storage | BufferUsageFlags::CopySrc;
    outputDesc.memoryUsage = MemoryUsage::GPUOnly;
    outputDesc.debugName = "MetalPushConstantOffsetOutput";
    auto output = device->CreateBuffer(outputDesc);
    ASSERT_NE(output, nullptr);

    RHIBindingSetDesc bindingSetDesc;
    bindingSetDesc.layout = bindingLayout;
    bindingSetDesc.entries.push_back({
        1u,
        BindingType::StorageBuffer,
        output,
        0u,
        outputDesc.size,
        sizeof(uint32_t) * 4u,
        nullptr,
        nullptr
    });
    bindingSetDesc.debugName = "MetalPushConstantOffsetBindingSet";
    auto bindingSet = device->CreateBindingSet(bindingSetDesc);
    ASSERT_NE(bindingSet, nullptr);

    auto queue = device->GetQueue(QueueType::Compute);
    auto commandPool = device->CreateCommandPool(QueueType::Compute, "MetalPushConstantOffsetPool");
    ASSERT_NE(queue, nullptr);
    ASSERT_NE(commandPool, nullptr);
    auto commandBuffer = commandPool->CreateCommandBuffer("MetalPushConstantOffsetCommands");
    ASSERT_NE(commandBuffer, nullptr);

    const std::array<uint32_t, 2u> firstValues = { 0x11223344u, 0x55667788u };
    const std::array<uint32_t, 2u> secondValues = { 0x99AABBCCu, 0xDDEEFF00u };
    const std::array<uint32_t, 2u> rejectedValues = { 0u, 0u };
    commandBuffer->Begin();
    ASSERT_TRUE(commandBuffer->IsRecording());
    commandBuffer->BindComputePipeline(pipeline);
    commandBuffer->BindBindingSet(0u, bindingSet);
    commandBuffer->PushConstants(ShaderStageMask::Compute, 0u, 8u, firstValues.data());
    commandBuffer->PushConstants(ShaderStageMask::Compute, 12u, 8u, rejectedValues.data());
    commandBuffer->PushConstants(ShaderStageMask::Compute, 8u, 8u, secondValues.data());
    commandBuffer->Dispatch(1u, 1u, 1u);
    commandBuffer->End();

    auto completionFence = device->CreateFence("MetalPushConstantOffsetFence");
    ASSERT_NE(completionFence, nullptr);
    RHISubmitDesc submitDesc;
    submitDesc.commandBuffers = { commandBuffer };
    submitDesc.signalFence = completionFence;
    const auto submitResult = queue->SubmitChecked(submitDesc);
    ASSERT_TRUE(submitResult.Succeeded()) << submitResult.message;
    ASSERT_TRUE(completionFence->Wait(5'000'000'000ull));

    std::array<uint32_t, 4u> readback{};
    RHIBufferReadbackDesc readbackDesc;
    readbackDesc.source = output;
    readbackDesc.sourceState = ResourceState::ShaderWrite;
    readbackDesc.size = sizeof(readback);
    readbackDesc.data = readback.data();
    readbackDesc.debugName = "MetalPushConstantOffsetReadback";
    const auto readbackResult = device->BeginReadBuffer(readbackDesc);
    ASSERT_TRUE(readbackResult.Succeeded()) << readbackResult.message;
    ASSERT_NE(readbackResult.completion, nullptr);
    const auto readbackStatus = readbackResult.completion->Wait(5'000'000'000ull);
    ASSERT_TRUE(readbackStatus.Succeeded()) << readbackStatus.message;

    const std::array<uint32_t, 4u> expected = {
        firstValues[0], firstValues[1], secondValues[0], secondValues[1]
    };
    EXPECT_EQ(readback, expected);
}

TEST(MetalRhiParityTests, MetalExecutesDxcShiftedBindingsAndIndexedObjectConstants)
{
    using namespace NLS::Render::RHI;
    namespace shader = NLS::Render::ShaderCompiler;
    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";
    ScopedMetalSubmitLane submitLane;

    const auto root = std::filesystem::temp_directory_path() / ("NullusMetalHlsl_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    const auto source = root / "bindings.hlsl";
    {
        std::ofstream file(source);
        file << R"(
cbuffer Constants : register(b0, space3) { uint Value; };
StructuredBuffer<uint4> Input : register(t0, space1);
RWStructuredBuffer<uint4> Output : register(u1, space1);
struct NLSObjectDrawConstants { uint objectIndex; uint3 padding; };
[[vk::push_constant]] ConstantBuffer<NLSObjectDrawConstants> g_NLSObjectDrawConstants;
Texture2D<float4> SampledImage : register(t0, space2);
SamplerState ImageSampler : register(s0, space2);
RWTexture2D<float4> StorageImage : register(u0, space0);
[numthreads(1, 1, 1)] void Main()
{
    Output[0] = Input[0] + Value + g_NLSObjectDrawConstants.objectIndex;
}
[numthreads(1, 1, 1)] void Sampling()
{
    StorageImage[uint2(0, 0)] = SampledImage.SampleLevel(ImageSampler, float2(0.5, 0.5), 0);
}
)";
    }
    shader::ShaderCompiler compiler;
    shader::ShaderCompilationInput input;
    input.assetPath = source.string();
    input.stage = shader::ShaderStage::Compute;
    input.options.targetPlatform = shader::ShaderTargetPlatform::SPIRV;
    input.options.targetProfile = "cs_6_0";
    input.options.entryPoint = "Main";
    input.options.artifactDirectory = (root / "cache").string();
    const auto compiled = compiler.Compile(input);
    ASSERT_EQ(compiled.status, shader::ShaderCompilationStatus::Succeeded) << compiled.diagnostics;
    RHIShaderModuleDesc moduleDesc;
    moduleDesc.stage = ShaderStage::Compute;
    moduleDesc.targetBackend = NativeBackendType::Metal;
    moduleDesc.entryPoint = "Main";
    moduleDesc.bytecode = compiled.bytecode;
    auto module = device->CreateShaderModule(moduleDesc);
    ASSERT_NE(module, nullptr);
    input.options.entryPoint = "Sampling";
    const auto sampled = compiler.Compile(input);
    ASSERT_EQ(sampled.status, shader::ShaderCompilationStatus::Succeeded) << sampled.diagnostics;
    moduleDesc.entryPoint = "Sampling";
    moduleDesc.bytecode = sampled.bytecode;
    ASSERT_NE(device->CreateShaderModule(moduleDesc), nullptr)
        << "Texture, sampler and storage-image registers must use the same logical binding map.";

    RHIBindingLayoutDesc layoutDesc;
    layoutDesc.entries = {
        {"Constants", BindingType::UniformBuffer, 0u, 0u, 1u, ShaderStageMask::Compute, 3u, 0u},
        {"Input", BindingType::StructuredBuffer, 0u, 0u, 1u, ShaderStageMask::Compute, 1u, 16u},
        {"Output", BindingType::StorageBuffer, 0u, 1u, 1u, ShaderStageMask::Compute, 1u, 16u}
    };
    auto layout = device->CreateBindingLayout(layoutDesc);
    ASSERT_NE(layout, nullptr);
    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.bindingLayouts = {layout};
    pipelineLayoutDesc.pushConstants.push_back({ShaderStageMask::Compute, 0u, 16u, 1u, 3u});
    auto pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
    ASSERT_NE(pipelineLayout, nullptr);
    RHIComputePipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = pipelineLayout;
    pipelineDesc.computeShader = module;
    auto pipeline = device->CreateComputePipeline(pipelineDesc);
    ASSERT_NE(pipeline, nullptr);

    const std::array<uint32_t, 4> values{10u, 0u, 0u, 0u}, inputs{1u, 2u, 3u, 4u};
    RHIBufferDesc bufferDesc;
    bufferDesc.size = sizeof(values);
    bufferDesc.usage = BufferUsageFlags::Uniform;
    bufferDesc.memoryUsage = MemoryUsage::CPUToGPU;
    RHIBufferUploadDesc upload;
    upload.data = values.data();
    upload.dataSize = sizeof(values);
    auto constants = device->CreateBuffer(bufferDesc, upload);
    bufferDesc.usage = BufferUsageFlags::Storage;
    upload.data = inputs.data();
    auto sourceBuffer = device->CreateBuffer(bufferDesc, upload);
    bufferDesc.usage = BufferUsageFlags::Storage | BufferUsageFlags::CopySrc;
    bufferDesc.memoryUsage = MemoryUsage::GPUOnly;
    auto output = device->CreateBuffer(bufferDesc);
    ASSERT_TRUE(constants && sourceBuffer && output);
    RHIBindingSetDesc setDesc;
    setDesc.layout = layout;
    setDesc.entries = {
        {0u, BindingType::UniformBuffer, constants, 0u, 16u, 0u},
        {0u, BindingType::StructuredBuffer, sourceBuffer, 0u, 16u, 16u},
        {1u, BindingType::StorageBuffer, output, 0u, 16u, 16u}
    };
    auto set = device->CreateBindingSet(setDesc);
    ASSERT_NE(set, nullptr);
    auto queue = device->GetQueue(QueueType::Compute);
    auto pool = device->CreateCommandPool(QueueType::Compute, "DxcBindings");
    ASSERT_TRUE(queue && pool);
    auto commands = pool->CreateCommandBuffer("DxcBindings");
    ASSERT_NE(commands, nullptr);
    commands->Begin();
    commands->BindComputePipeline(pipeline);
    commands->BindBindingSet(0u, set);
    const std::array<uint32_t, 4> objectConstants{7u, 0u, 0u, 0u};
    commands->PushConstants(ShaderStageMask::Compute, 0u, 16u, objectConstants.data());
    commands->Dispatch(1u, 1u, 1u);
    commands->End();
    auto fence = device->CreateFence("DxcBindings");
    ASSERT_NE(fence, nullptr);
    RHISubmitDesc submit;
    submit.commandBuffers = {commands};
    submit.signalFence = fence;
    const auto submitted = queue->SubmitChecked(submit);
    ASSERT_TRUE(submitted.Succeeded()) << submitted.message;
    ASSERT_TRUE(fence->Wait(5'000'000'000ull));
    std::array<uint32_t, 4> result{};
    RHIBufferReadbackDesc readback;
    readback.source = output;
    readback.sourceState = ResourceState::ShaderWrite;
    readback.size = sizeof(result);
    readback.data = result.data();
    const auto read = device->BeginReadBuffer(readback);
    ASSERT_TRUE(read.Succeeded()) << read.message;
    ASSERT_NE(read.completion, nullptr);
    ASSERT_TRUE(read.completion->Wait(5'000'000'000ull).Succeeded());
    const std::array<uint32_t, 4> expected{18u, 19u, 20u, 21u};
    EXPECT_EQ(result, expected);
}

TEST(MetalRhiParityTests, MetalEnforcesDx12EquivalentResourceMemoryUsage)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    const std::array<uint32_t, 4u> initialData = { 1u, 2u, 3u, 4u };
    RHIBufferUploadDesc initialUpload;
    initialUpload.data = initialData.data();
    initialUpload.dataSize = sizeof(initialData);

    RHIBufferDesc gpuOnlyDesc;
    gpuOnlyDesc.size = sizeof(initialData);
    gpuOnlyDesc.usage = BufferUsageFlags::Storage | BufferUsageFlags::CopySrc;
    gpuOnlyDesc.memoryUsage = MemoryUsage::GPUOnly;
    gpuOnlyDesc.debugName = "MetalPrivateMemoryContractBuffer";
    auto gpuOnlyBuffer = device->CreateBuffer(gpuOnlyDesc, initialUpload);
    ASSERT_NE(gpuOnlyBuffer, nullptr);
    EXPECT_EQ(gpuOnlyBuffer->GetDesc().memoryUsage, MemoryUsage::GPUOnly);
    EXPECT_EQ(gpuOnlyBuffer->UpdateData(initialUpload).code, RHIUpdateStatusCode::Unsupported);

    std::array<uint32_t, 4u> gpuOnlyReadback{};
    RHIBufferReadbackDesc readbackDesc;
    readbackDesc.source = gpuOnlyBuffer;
    readbackDesc.sourceState = ResourceState::CopySrc;
    readbackDesc.size = sizeof(gpuOnlyReadback);
    readbackDesc.data = gpuOnlyReadback.data();
    auto readbackResult = device->BeginReadBuffer(readbackDesc);
    ASSERT_TRUE(readbackResult.Succeeded()) << readbackResult.message;
    ASSERT_NE(readbackResult.completion, nullptr);
    const auto completionStatus = readbackResult.completion->Wait(5'000'000'000ull);
    ASSERT_TRUE(completionStatus.Succeeded()) << completionStatus.message;
    EXPECT_EQ(gpuOnlyReadback, initialData);

    RHIBufferDesc cpuToGpuDesc;
    cpuToGpuDesc.size = sizeof(initialData);
    cpuToGpuDesc.usage = BufferUsageFlags::ShaderRead;
    cpuToGpuDesc.memoryUsage = MemoryUsage::CPUToGPU;
    cpuToGpuDesc.debugName = "MetalSharedMemoryContractBuffer";
    auto cpuToGpuBuffer = device->CreateBuffer(cpuToGpuDesc);
    ASSERT_NE(cpuToGpuBuffer, nullptr);
    EXPECT_TRUE(cpuToGpuBuffer->UpdateData(initialUpload).Succeeded());

    auto invalidUploadDesc = cpuToGpuDesc;
    invalidUploadDesc.usage = BufferUsageFlags::Storage;
    EXPECT_EQ(device->CreateBuffer(invalidUploadDesc), nullptr);

    auto invalidReadbackDesc = cpuToGpuDesc;
    invalidReadbackDesc.usage = BufferUsageFlags::CopySrc;
    invalidReadbackDesc.memoryUsage = MemoryUsage::GPUToCPU;
    EXPECT_EQ(device->CreateBuffer(invalidReadbackDesc), nullptr);

    RHITextureDesc invalidTextureDesc;
    invalidTextureDesc.extent = { 1u, 1u, 1u };
    invalidTextureDesc.format = TextureFormat::RGBA8;
    invalidTextureDesc.memoryUsage = MemoryUsage::CPUToGPU;
    invalidTextureDesc.debugName = "MetalInvalidCpuTexture";
    EXPECT_EQ(device->CreateTexture(invalidTextureDesc), nullptr);
}

TEST(MetalRhiParityTests, MetalTracksDx12EquivalentResourceStatesAndBindingBounds)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    RHIBufferDesc gpuBufferDesc;
    gpuBufferDesc.size = 64u;
    gpuBufferDesc.usage = BufferUsageFlags::Storage | BufferUsageFlags::CopyDst;
    gpuBufferDesc.memoryUsage = MemoryUsage::GPUOnly;
    gpuBufferDesc.debugName = "MetalTrackedGpuBuffer";
    auto gpuBuffer = device->CreateBuffer(gpuBufferDesc);
    ASSERT_NE(gpuBuffer, nullptr);
    EXPECT_EQ(gpuBuffer->GetState(), ResourceState::Unknown);

    RHIBufferDesc uniformBufferDesc;
    uniformBufferDesc.size = 64u;
    uniformBufferDesc.usage = BufferUsageFlags::Uniform;
    uniformBufferDesc.memoryUsage = MemoryUsage::GPUOnly;
    uniformBufferDesc.debugName = "MetalTrackedUniformBuffer";
    auto uniformBuffer = device->CreateBuffer(uniformBufferDesc);
    ASSERT_NE(uniformBuffer, nullptr);
    EXPECT_EQ(uniformBuffer->GetDesc().memoryUsage, MemoryUsage::CPUToGPU);
    EXPECT_EQ(uniformBuffer->GetState(), ResourceState::GenericRead);

    RHITextureDesc textureDesc;
    textureDesc.extent = { 4u, 4u, 1u };
    textureDesc.format = TextureFormat::RGBA8;
    textureDesc.mipLevels = 2u;
    textureDesc.usage = TextureUsageFlags::Sampled | TextureUsageFlags::ColorAttachment;
    textureDesc.memoryUsage = MemoryUsage::GPUOnly;
    textureDesc.debugName = "MetalTrackedTexture";
    auto texture = device->CreateTexture(textureDesc);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->GetState(), ResourceState::Unknown);

    auto commandPool = device->CreateCommandPool(QueueType::Graphics, "MetalTrackedStatePool");
    ASSERT_NE(commandPool, nullptr);
    auto commandBuffer = commandPool->CreateCommandBuffer("MetalTrackedStateCommands");
    ASSERT_NE(commandBuffer, nullptr);
    commandBuffer->Begin();

    RHIBarrierDesc bufferBarrier;
    bufferBarrier.bufferBarriers.push_back({
        gpuBuffer,
        ResourceState::Unknown,
        ResourceState::CopyDst
    });
    EXPECT_TRUE(commandBuffer->BarrierChecked(bufferBarrier).Succeeded());
    EXPECT_EQ(gpuBuffer->GetState(), ResourceState::CopyDst);

    RHIBarrierDesc illegalCpuVisibleBarrier;
    illegalCpuVisibleBarrier.bufferBarriers.push_back({
        uniformBuffer,
        ResourceState::GenericRead,
        ResourceState::CopyDst
    });
    EXPECT_FALSE(commandBuffer->BarrierChecked(illegalCpuVisibleBarrier).Succeeded());
    EXPECT_EQ(uniformBuffer->GetState(), ResourceState::GenericRead);

    RHIBarrierDesc fullTextureBarrier;
    fullTextureBarrier.textureBarriers.push_back({
        texture,
        ResourceState::Unknown,
        ResourceState::RenderTarget,
        GetFullTextureSubresourceRange(textureDesc)
    });
    EXPECT_TRUE(commandBuffer->BarrierChecked(fullTextureBarrier).Succeeded());
    EXPECT_EQ(texture->GetState(), ResourceState::RenderTarget);

    RHIBarrierDesc partialTextureBarrier;
    partialTextureBarrier.textureBarriers.push_back({
        texture,
        ResourceState::RenderTarget,
        ResourceState::ShaderRead,
        RHISubresourceRange { 1u, 1u, 0u, 1u }
    });
    EXPECT_TRUE(commandBuffer->BarrierChecked(partialTextureBarrier).Succeeded());
    EXPECT_EQ(texture->GetState(), ResourceState::RenderTarget)
        << "A partial transition must not overwrite the tracked whole-resource state.";

    RHIBarrierDesc invalidTextureBarrier;
    invalidTextureBarrier.textureBarriers.push_back({
        texture,
        ResourceState::Unknown,
        ResourceState::ShaderRead,
        RHISubresourceRange { 9u, 1u, 0u, 1u }
    });
    EXPECT_FALSE(commandBuffer->BarrierChecked(invalidTextureBarrier).Succeeded());
    commandBuffer->End();

    RHIBindingLayoutDesc invalidCountLayoutDesc;
    invalidCountLayoutDesc.entries.push_back({
        "InvalidCount",
        BindingType::StorageBuffer,
        0u,
        0u,
        0u,
        ShaderStageMask::Compute,
        0u,
        sizeof(uint32_t)
    });
    EXPECT_EQ(device->CreateBindingLayout(invalidCountLayoutDesc), nullptr);

    RHIBindingLayoutDesc storageLayoutDesc;
    storageLayoutDesc.entries.push_back({
        "Storage",
        BindingType::StorageBuffer,
        0u,
        1u,
        1u,
        ShaderStageMask::Compute,
        0u,
        sizeof(uint32_t)
    });
    auto storageLayout = device->CreateBindingLayout(storageLayoutDesc);
    ASSERT_NE(storageLayout, nullptr);

    RHIBindingSetDesc invalidRangeSetDesc;
    invalidRangeSetDesc.layout = storageLayout;
    invalidRangeSetDesc.entries.push_back({
        1u,
        BindingType::StorageBuffer,
        gpuBuffer,
        60u,
        8u,
        sizeof(uint32_t),
        nullptr,
        nullptr
    });
    EXPECT_EQ(device->CreateBindingSet(invalidRangeSetDesc), nullptr);

    RHIBindingSetDesc missingOptionalSetDesc;
    missingOptionalSetDesc.layout = storageLayout;
    EXPECT_NE(device->CreateBindingSet(missingOptionalSetDesc), nullptr)
        << "Missing entries map to DX12 null descriptors and must remain a valid binding set.";
}

TEST(MetalRhiParityTests, MetalOrdersGpuWorkAcrossNativeQueuesWithSharedEvents)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    EXPECT_TRUE(device->GetCapabilities().GetFeature(RHIDeviceFeature::DedicatedComputeQueue).supported);
    EXPECT_TRUE(device->GetCapabilities().GetFeature(RHIDeviceFeature::CopyQueue).supported);

    const std::array<uint32_t, 4u> sourceData = {
        0x10203040u, 0x50607080u, 0x90A0B0C0u, 0xD0E0F000u
    };
    RHIBufferUploadDesc sourceUpload;
    sourceUpload.data = sourceData.data();
    sourceUpload.dataSize = sizeof(sourceData);

    RHIBufferDesc sourceDesc;
    sourceDesc.size = sizeof(sourceData);
    sourceDesc.usage = BufferUsageFlags::CopySrc;
    sourceDesc.memoryUsage = MemoryUsage::GPUOnly;
    sourceDesc.debugName = "MetalCrossQueueSource";
    auto source = device->CreateBuffer(sourceDesc, sourceUpload);

    RHIBufferDesc intermediateDesc;
    intermediateDesc.size = sizeof(sourceData);
    intermediateDesc.usage = BufferUsageFlags::CopySrc | BufferUsageFlags::CopyDst;
    intermediateDesc.memoryUsage = MemoryUsage::GPUOnly;
    intermediateDesc.debugName = "MetalCrossQueueIntermediate";
    auto intermediate = device->CreateBuffer(intermediateDesc);

    RHIBufferDesc destinationDesc = intermediateDesc;
    destinationDesc.debugName = "MetalCrossQueueDestination";
    auto destination = device->CreateBuffer(destinationDesc);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(intermediate, nullptr);
    ASSERT_NE(destination, nullptr);

    auto copyPool = device->CreateCommandPool(QueueType::Copy, "MetalCrossQueueProducerPool");
    auto graphicsPool = device->CreateCommandPool(QueueType::Graphics, "MetalCrossQueueConsumerPool");
    auto copyQueue = device->GetQueue(QueueType::Copy);
    auto graphicsQueue = device->GetQueue(QueueType::Graphics);
    ASSERT_NE(copyPool, nullptr);
    ASSERT_NE(graphicsPool, nullptr);
    ASSERT_NE(copyQueue, nullptr);
    ASSERT_NE(graphicsQueue, nullptr);

    auto producer = copyPool->CreateCommandBuffer("MetalCrossQueueProducer");
    auto consumer = graphicsPool->CreateCommandBuffer("MetalCrossQueueConsumer");
    ASSERT_NE(producer, nullptr);
    ASSERT_NE(consumer, nullptr);
    producer->Begin();
    producer->CopyBuffer(source, intermediate, { 0u, 0u, sizeof(sourceData) });
    producer->End();
    consumer->Begin();
    consumer->CopyBuffer(intermediate, destination, { 0u, 0u, sizeof(sourceData) });
    consumer->End();

    auto handoff = device->CreateSemaphore("MetalCrossQueueHandoff");
    auto completionFence = device->CreateFence("MetalCrossQueueFence");
    ASSERT_NE(handoff, nullptr);
    ASSERT_NE(completionFence, nullptr);

    RHISubmitDesc producerSubmit;
    producerSubmit.commandBuffers = { producer };
    producerSubmit.signalSemaphores = { handoff };
    const auto producerResult = copyQueue->SubmitChecked(producerSubmit);
    ASSERT_TRUE(producerResult.Succeeded()) << producerResult.message;

    RHISubmitDesc consumerSubmit;
    consumerSubmit.commandBuffers = { consumer };
    consumerSubmit.waitSemaphores = { handoff };
    consumerSubmit.signalFence = completionFence;
    const auto consumerResult = graphicsQueue->SubmitChecked(consumerSubmit);
    ASSERT_TRUE(consumerResult.Succeeded()) << consumerResult.message;
    ASSERT_TRUE(completionFence->Wait(5'000'000'000ull));
    EXPECT_TRUE(handoff->IsSignaled());

    std::array<uint32_t, 4u> readback{};
    RHIBufferReadbackDesc readbackDesc;
    readbackDesc.source = destination;
    readbackDesc.sourceState = ResourceState::CopyDst;
    readbackDesc.size = sizeof(readback);
    readbackDesc.data = readback.data();
    const auto readbackResult = device->BeginReadBuffer(readbackDesc);
    ASSERT_TRUE(readbackResult.Succeeded()) << readbackResult.message;
    ASSERT_NE(readbackResult.completion, nullptr);
    const auto readbackStatus = readbackResult.completion->Wait(5'000'000'000ull);
    ASSERT_TRUE(readbackStatus.Succeeded()) << readbackStatus.message;
    EXPECT_EQ(readback, sourceData);
}

TEST(MetalRhiParityTests, MetalCreatesAndUpdatesAdvertisedCompressedTextures)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    const auto& capabilities = device->GetCapabilities();
    EXPECT_EQ(capabilities.limits.maxTextureDimension2D, capabilities.maxTextureDimension2D);
    EXPECT_EQ(capabilities.limits.maxColorAttachments, capabilities.maxColorAttachments);

    auto queue = device->GetQueue(QueueType::Graphics);
    auto commandPool = device->CreateCommandPool(QueueType::Graphics, "CompressedCopyPool");
    ASSERT_NE(queue, nullptr);
    ASSERT_NE(commandPool, nullptr);

    const std::array formats = {
        TextureFormat::BC1,
        TextureFormat::BC3,
        TextureFormat::BC5,
        TextureFormat::BC7,
        TextureFormat::BC6H,
        TextureFormat::ASTC4x4,
        TextureFormat::ETC2RGBA8
    };
    for (const auto format : formats)
    {
        const auto& capability = capabilities.GetTextureFormatCapability(format);
        const bool requiredDx12AssetFormat =
            format == TextureFormat::BC1 || format == TextureFormat::BC3 ||
            format == TextureFormat::BC5 || format == TextureFormat::BC7;
        if (!capability.sampled || !capability.upload)
        {
            EXPECT_FALSE(requiredDx12AssetFormat) << GetTextureFormatName(format);
            continue;
        }
        EXPECT_TRUE(capability.supportsUnalignedBlockTextures) << GetTextureFormatName(format);

        RHITextureDesc textureDesc;
        textureDesc.extent = { 7u, 5u, 1u };
        textureDesc.dimension = TextureDimension::Texture2D;
        textureDesc.format = format;
        const auto* formatDescriptor = GetTextureFormatDescriptor(format);
        ASSERT_NE(formatDescriptor, nullptr);
        textureDesc.colorSpace = formatDescriptor->supportsSrgbView
            ? TextureColorSpace::SRGB
            : TextureColorSpace::Linear;
        textureDesc.mipLevels = 3u;
        textureDesc.usage = TextureUsageFlags::Sampled | TextureUsageFlags::CopyDst;
        textureDesc.debugName = "CompressedMetalTexture";

        std::array<std::vector<uint8_t>, 3u> mipData;
        for (uint32_t mipLevel = 0u; mipLevel < textureDesc.mipLevels; ++mipLevel)
        {
            const uint32_t mipWidth = (std::max)(textureDesc.extent.width >> mipLevel, 1u);
            const uint32_t mipHeight = (std::max)(textureDesc.extent.height >> mipLevel, 1u);
            mipData[mipLevel].resize(CalculateTextureSlicePitch(format, mipWidth, mipHeight));
        }

        RHITextureUploadDesc uploadDesc;
        for (const auto& mip : mipData)
            uploadDesc.subresources.push_back({ mip.data(), mip.size() });

        auto texture = device->CreateTexture(textureDesc, uploadDesc);
        ASSERT_NE(texture, nullptr) << GetTextureFormatName(format);

        RHITextureViewDesc viewDesc;
        viewDesc.viewType = TextureViewType::Texture2D;
        viewDesc.format = format;
        viewDesc.colorSpace = textureDesc.colorSpace;
        viewDesc.subresourceRange = { 0u, textureDesc.mipLevels, 0u, 1u };
        EXPECT_NE(device->CreateTextureView(texture, viewDesc), nullptr)
            << GetTextureFormatName(format);

        const uint32_t updateRowPitch = CalculateTextureRowPitch(format, 3u);
        std::vector<uint8_t> updateData(updateRowPitch);
        RHITextureUpdateDesc updateDesc;
        updateDesc.texture = texture;
        updateDesc.data = updateData.data();
        updateDesc.dataSize = updateData.size();
        updateDesc.x = 4u;
        updateDesc.extent = { 3u, 4u, 1u };
        updateDesc.rowPitch = updateRowPitch;
        EXPECT_TRUE(device->UpdateTexture(updateDesc).Succeeded())
            << GetTextureFormatName(format);

        updateDesc.x = 1u;
        updateDesc.extent.width = 3u;
        EXPECT_EQ(
            device->UpdateTexture(updateDesc).code,
            RHIUpdateStatusCode::InvalidArgument)
            << GetTextureFormatName(format);

        RHIBufferDesc stagingDesc;
        stagingDesc.size = mipData[0].size();
        stagingDesc.usage = BufferUsageFlags::CopySrc;
        stagingDesc.memoryUsage = MemoryUsage::CPUToGPU;
        stagingDesc.debugName = "CompressedCopySource";
        RHIBufferUploadDesc stagingUpload;
        stagingUpload.data = mipData[0].data();
        stagingUpload.dataSize = mipData[0].size();
        auto stagingBuffer = device->CreateBuffer(stagingDesc, stagingUpload);
        ASSERT_NE(stagingBuffer, nullptr) << GetTextureFormatName(format);

        auto commandBuffer = commandPool->CreateCommandBuffer("CompressedCopyCommands");
        ASSERT_NE(commandBuffer, nullptr);
        commandBuffer->Begin();
        ASSERT_TRUE(commandBuffer->IsRecording());
        RHIBufferToTextureCopyDesc copyDesc;
        copyDesc.source = stagingBuffer;
        copyDesc.destination = texture;
        copyDesc.extent = textureDesc.extent;
        copyDesc.rowPitch = CalculateTextureRowPitch(format, textureDesc.extent.width);
        commandBuffer->CopyBufferToTexture(copyDesc);
        commandBuffer->End();

        auto copyFence = device->CreateFence("CompressedCopyFence");
        ASSERT_NE(copyFence, nullptr);
        RHISubmitDesc submitDesc;
        submitDesc.commandBuffers = { commandBuffer };
        submitDesc.signalFence = copyFence;
        const auto submitResult = queue->SubmitChecked(submitDesc);
        ASSERT_TRUE(submitResult.Succeeded()) << submitResult.message;
        EXPECT_TRUE(copyFence->Wait(5'000'000'000ull))
            << GetTextureFormatName(format);
    }
}

TEST(MetalRhiParityTests, MetalRejectsInvalidResourceCopiesAndExecutesValidCopies)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    auto queue = device->GetQueue(QueueType::Graphics);
    auto commandPool = device->CreateCommandPool(QueueType::Graphics, "ValidatedCopyPool");
    ASSERT_NE(queue, nullptr);
    ASSERT_NE(commandPool, nullptr);

    const std::array<uint8_t, 16u> sourceBufferData = {
        0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u
    };
    const std::array<uint8_t, 16u> emptyBufferData{};
    RHIBufferDesc sourceBufferDesc;
    sourceBufferDesc.size = sourceBufferData.size();
    sourceBufferDesc.usage = BufferUsageFlags::CopySrc;
    sourceBufferDesc.memoryUsage = MemoryUsage::CPUToGPU;
    sourceBufferDesc.debugName = "ValidatedCopySourceBuffer";
    RHIBufferUploadDesc sourceBufferUpload;
    sourceBufferUpload.data = sourceBufferData.data();
    sourceBufferUpload.dataSize = sourceBufferData.size();
    auto sourceBuffer = device->CreateBuffer(sourceBufferDesc, sourceBufferUpload);

    RHIBufferDesc destinationBufferDesc;
    destinationBufferDesc.size = emptyBufferData.size();
    destinationBufferDesc.usage = BufferUsageFlags::CopyDst;
    destinationBufferDesc.memoryUsage = MemoryUsage::GPUOnly;
    destinationBufferDesc.debugName = "ValidatedCopyDestinationBuffer";
    RHIBufferUploadDesc destinationBufferUpload;
    destinationBufferUpload.data = emptyBufferData.data();
    destinationBufferUpload.dataSize = emptyBufferData.size();
    auto destinationBuffer = device->CreateBuffer(destinationBufferDesc, destinationBufferUpload);
    ASSERT_NE(sourceBuffer, nullptr);
    ASSERT_NE(destinationBuffer, nullptr);

    std::array<uint8_t, 4u * 4u * 4u> sourceTextureData{};
    for (uint32_t y = 0u; y < 4u; ++y)
    {
        for (uint32_t x = 0u; x < 4u; ++x)
        {
            const size_t pixel = (static_cast<size_t>(y) * 4u + x) * 4u;
            sourceTextureData[pixel + 0u] = static_cast<uint8_t>(10u + x);
            sourceTextureData[pixel + 1u] = static_cast<uint8_t>(20u + y);
            sourceTextureData[pixel + 2u] = 30u;
            sourceTextureData[pixel + 3u] = 255u;
        }
    }
    const std::array<uint8_t, 4u * 4u * 4u> emptyTextureData{};

    RHITextureDesc sourceTextureDesc;
    sourceTextureDesc.extent = { 4u, 4u, 1u };
    sourceTextureDesc.format = TextureFormat::RGBA8;
    sourceTextureDesc.usage = TextureUsageFlags::CopySrc;
    sourceTextureDesc.debugName = "ValidatedCopySourceTexture";
    RHITextureUploadDesc sourceTextureUpload;
    sourceTextureUpload.subresources.push_back({ sourceTextureData.data(), sourceTextureData.size() });
    auto sourceTexture = device->CreateTexture(sourceTextureDesc, sourceTextureUpload);

    RHITextureDesc destinationTextureDesc = sourceTextureDesc;
    destinationTextureDesc.usage = TextureUsageFlags::CopyDst | TextureUsageFlags::CopySrc;
    destinationTextureDesc.debugName = "ValidatedCopyDestinationTexture";
    RHITextureUploadDesc destinationTextureUpload;
    destinationTextureUpload.subresources.push_back({ emptyTextureData.data(), emptyTextureData.size() });
    auto destinationTexture = device->CreateTexture(destinationTextureDesc, destinationTextureUpload);

    RHITextureDesc incompatibleTextureDesc = destinationTextureDesc;
    incompatibleTextureDesc.format = TextureFormat::R8;
    incompatibleTextureDesc.debugName = "ValidatedCopyIncompatibleTexture";
    const std::array<uint8_t, 4u * 4u> emptyR8Data{};
    RHITextureUploadDesc incompatibleTextureUpload;
    incompatibleTextureUpload.subresources.push_back({ emptyR8Data.data(), emptyR8Data.size() });
    auto incompatibleTexture = device->CreateTexture(incompatibleTextureDesc, incompatibleTextureUpload);
    ASSERT_NE(sourceTexture, nullptr);
    ASSERT_NE(destinationTexture, nullptr);
    ASSERT_NE(incompatibleTexture, nullptr);

    auto commandBuffer = commandPool->CreateCommandBuffer("ValidatedCopyCommands");
    ASSERT_NE(commandBuffer, nullptr);
    commandBuffer->Begin();
    ASSERT_TRUE(commandBuffer->IsRecording());

    commandBuffer->CopyBuffer(sourceBuffer, destinationBuffer, { 14u, 0u, 4u });
    commandBuffer->CopyBuffer(sourceBuffer, destinationBuffer, { 0u, 15u, 4u });
    commandBuffer->CopyBuffer(sourceBuffer, destinationBuffer, { 4u, 8u, 4u });

    RHITextureCopyDesc textureCopy;
    textureCopy.source = sourceTexture;
    textureCopy.destination = destinationTexture;
    textureCopy.extent = { 1u, 1u, 1u };
    textureCopy.sourceRange.baseMipLevel = 1u;
    commandBuffer->CopyTexture(textureCopy);
    textureCopy.sourceRange.baseMipLevel = 0u;
    textureCopy.destinationRange.baseArrayLayer = 1u;
    commandBuffer->CopyTexture(textureCopy);
    textureCopy.destinationRange.baseArrayLayer = 0u;
    textureCopy.sourceOffset.x = -1;
    commandBuffer->CopyTexture(textureCopy);
    textureCopy.sourceOffset.x = 0;
    textureCopy.destination = incompatibleTexture;
    commandBuffer->CopyTexture(textureCopy);

    textureCopy.destination = destinationTexture;
    textureCopy.sourceOffset = { 1, 1, 0 };
    textureCopy.destinationOffset = { 2, 0, 0 };
    textureCopy.extent = { 2u, 2u, 1u };
    commandBuffer->CopyTexture(textureCopy);
    commandBuffer->End();

    auto copyFence = device->CreateFence("ValidatedCopyFence");
    ASSERT_NE(copyFence, nullptr);
    RHISubmitDesc submitDesc;
    submitDesc.commandBuffers = { commandBuffer };
    submitDesc.signalFence = copyFence;
    const auto submitResult = queue->SubmitChecked(submitDesc);
    ASSERT_TRUE(submitResult.Succeeded()) << submitResult.message;
    ASSERT_TRUE(copyFence->Wait(5'000'000'000ull));

    std::array<uint8_t, 16u> bufferReadback{};
    RHIBufferReadbackDesc bufferReadbackDesc;
    bufferReadbackDesc.source = destinationBuffer;
    bufferReadbackDesc.sourceState = ResourceState::CopyDst;
    bufferReadbackDesc.size = bufferReadback.size();
    bufferReadbackDesc.data = bufferReadback.data();
    bufferReadbackDesc.debugName = "ValidatedCopyBufferReadback";
    const auto bufferReadbackResult = device->BeginReadBuffer(bufferReadbackDesc);
    ASSERT_TRUE(bufferReadbackResult.Succeeded()) << bufferReadbackResult.message;
    ASSERT_NE(bufferReadbackResult.completion, nullptr);
    const auto bufferReadbackStatus = bufferReadbackResult.completion->Wait(5'000'000'000ull);
    ASSERT_TRUE(bufferReadbackStatus.Succeeded()) << bufferReadbackStatus.message;

    auto expectedBufferData = emptyBufferData;
    std::copy_n(sourceBufferData.begin() + 4u, 4u, expectedBufferData.begin() + 8u);
    EXPECT_EQ(bufferReadback, expectedBufferData);

    std::array<uint8_t, 4u * 4u * 4u> textureReadback{};
    const auto textureReadbackResult = device->ReadPixelsChecked(
        destinationTexture,
        0u,
        0u,
        4u,
        4u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        textureReadback.data());
    ASSERT_TRUE(textureReadbackResult.Succeeded()) << textureReadbackResult.message;

    auto expectedTextureData = emptyTextureData;
    for (uint32_t row = 0u; row < 2u; ++row)
    {
        const size_t sourceOffset = (static_cast<size_t>(row + 1u) * 4u + 1u) * 4u;
        const size_t destinationOffset = (static_cast<size_t>(row) * 4u + 2u) * 4u;
        std::copy_n(
            sourceTextureData.begin() + sourceOffset,
            2u * 4u,
            expectedTextureData.begin() + destinationOffset);
    }
    EXPECT_EQ(textureReadback, expectedTextureData);
}

TEST(MetalRhiParityTests, MetalPixelReadbackIsAsyncAndSupportsDx12WideSourceLayouts)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

    ScopedMetalSubmitLane submitLane;

    EXPECT_TRUE(device->GetCapabilities().GetFeature(RHIDeviceFeature::AsyncReadback).supported);

    const std::array<uint8_t, 16u> rgbaPixels = {
        1u, 2u, 3u, 4u,
        5u, 6u, 7u, 8u,
        9u, 10u, 11u, 12u,
        13u, 14u, 15u, 16u
    };
    RHITextureDesc rgbaDesc;
    rgbaDesc.extent = { 2u, 2u, 1u };
    rgbaDesc.format = TextureFormat::RGBA8;
    rgbaDesc.usage = TextureUsageFlags::CopySrc;
    rgbaDesc.debugName = "AsyncMetalRgbaReadback";
    RHITextureUploadDesc rgbaUpload;
    rgbaUpload.subresources.push_back({ rgbaPixels.data(), rgbaPixels.size() });
    auto rgbaTexture = device->CreateTexture(rgbaDesc, rgbaUpload);
    ASSERT_NE(rgbaTexture, nullptr);

    std::array<uint8_t, rgbaPixels.size()> rgbaReadback{};
    auto rgbaResult = device->BeginReadPixels(
        rgbaTexture,
        0u,
        0u,
        2u,
        2u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        rgbaReadback.data());
    ASSERT_TRUE(rgbaResult.Succeeded()) << rgbaResult.message;
    ASSERT_NE(rgbaResult.completion, nullptr);
    const auto rgbaCompletion = rgbaResult.completion->Wait(5'000'000'000ull);
    ASSERT_TRUE(rgbaCompletion.Succeeded()) << rgbaCompletion.message;
    EXPECT_EQ(rgbaReadback, rgbaPixels);

    const std::array<uint8_t, 16u> rg16Pixels = {
        0x00u, 0x3Cu, 0x00u, 0x38u,
        0x00u, 0x34u, 0x00u, 0x3Cu,
        0x00u, 0x00u, 0x00u, 0x3Cu,
        0x00u, 0x38u, 0x00u, 0x34u
    };
    RHITextureDesc rg16Desc = rgbaDesc;
    rg16Desc.format = TextureFormat::RG16F;
    rg16Desc.debugName = "AsyncMetalRg16Readback";
    RHITextureUploadDesc rg16Upload;
    rg16Upload.subresources.push_back({ rg16Pixels.data(), rg16Pixels.size() });
    auto rg16Texture = device->CreateTexture(rg16Desc, rg16Upload);
    ASSERT_NE(rg16Texture, nullptr);

    std::array<uint8_t, rg16Pixels.size()> rg16Readback{};
    auto rg16Result = device->BeginReadPixels(
        rg16Texture,
        0u,
        0u,
        2u,
        2u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        rg16Readback.data());
    ASSERT_TRUE(rg16Result.Succeeded()) << rg16Result.message;
    ASSERT_NE(rg16Result.completion, nullptr);
    const auto rg16Completion = rg16Result.completion->Wait(5'000'000'000ull);
    ASSERT_TRUE(rg16Completion.Succeeded()) << rg16Completion.message;
    EXPECT_EQ(rg16Readback, rg16Pixels);

    RHITextureDesc r8Desc = rgbaDesc;
    r8Desc.format = TextureFormat::R8;
    r8Desc.debugName = "RejectedMetalR8Readback";
    const std::array<uint8_t, 4u> r8Pixels = { 1u, 2u, 3u, 4u };
    RHITextureUploadDesc r8Upload;
    r8Upload.subresources.push_back({ r8Pixels.data(), r8Pixels.size() });
    auto r8Texture = device->CreateTexture(r8Desc, r8Upload);
    ASSERT_NE(r8Texture, nullptr);

    std::array<uint8_t, 16u> rejectedReadback{};
    const auto rejectedResult = device->BeginReadPixels(
        r8Texture,
        0u,
        0u,
        2u,
        2u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        rejectedReadback.data());
    EXPECT_EQ(rejectedResult.code, RHIReadbackStatusCode::UnsupportedFormat);
    EXPECT_EQ(rejectedResult.completion, nullptr);

    const auto& rgb8Capability = device->GetCapabilities().GetTextureFormatCapability(TextureFormat::RGB8);
    EXPECT_TRUE(rgb8Capability.sampled);
    EXPECT_TRUE(rgb8Capability.upload);
    const std::array<uint8_t, 12u> rgb8Pixels = {
        10u, 20u, 30u,
        40u, 50u, 60u,
        70u, 80u, 90u,
        100u, 110u, 120u
    };
    RHITextureDesc rgb8Desc = rgbaDesc;
    rgb8Desc.format = TextureFormat::RGB8;
    rgb8Desc.debugName = "ExpandedMetalRgb8Readback";
    RHITextureUploadDesc rgb8Upload;
    rgb8Upload.subresources.push_back({ rgb8Pixels.data(), rgb8Pixels.size() });
    auto rgb8Texture = device->CreateTexture(rgb8Desc, rgb8Upload);
    ASSERT_NE(rgb8Texture, nullptr);

    const std::array<uint8_t, 3u> replacementPixel = { 7u, 8u, 9u };
    RHITextureUpdateDesc rgb8Update;
    rgb8Update.texture = rgb8Texture;
    rgb8Update.data = replacementPixel.data();
    rgb8Update.dataSize = replacementPixel.size();
    rgb8Update.x = 1u;
    rgb8Update.y = 0u;
    rgb8Update.extent = { 1u, 1u, 1u };
    rgb8Update.debugName = "ExpandedMetalRgb8Update";
    const auto rgb8UpdateResult = device->UpdateTexture(rgb8Update);
    ASSERT_TRUE(rgb8UpdateResult.Succeeded()) << rgb8UpdateResult.message;

    std::array<uint8_t, 16u> rgb8Readback{};
    const auto rgb8ReadbackResult = device->ReadPixelsChecked(
        rgb8Texture,
        0u,
        0u,
        2u,
        2u,
        NLS::Render::Settings::EPixelDataFormat::RGBA,
        NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        rgb8Readback.data());
    ASSERT_TRUE(rgb8ReadbackResult.Succeeded()) << rgb8ReadbackResult.message;
    EXPECT_EQ(rgb8Readback, (std::array<uint8_t, 16u>{
        10u, 20u, 30u, 255u,
        7u, 8u, 9u, 255u,
        70u, 80u, 90u, 255u,
        100u, 110u, 120u, 255u
    }));
}
#endif