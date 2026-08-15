#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <array>
#include <cstdint>
#include <memory>

#include "Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/Core/RHISync.h"
#endif

namespace
{
    std::filesystem::path RepoPath(std::string_view relativePath)
    {
        return std::filesystem::path(NLS_ROOT_DIR) / std::filesystem::path(relativePath);
    }

    std::string ReadSourceText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        EXPECT_TRUE(input.is_open()) << "Failed to open source file: " << path.string();
        return std::string(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
    }

    std::string ExtractFunctionBody(const std::string& source, std::string_view functionNeedle)
    {
        const auto begin = source.find(functionNeedle);
        EXPECT_NE(begin, std::string::npos) << "Missing function: " << functionNeedle;
        if (begin == std::string::npos)
            return {};

        const auto bodyBegin = source.find('{', begin);
        EXPECT_NE(bodyBegin, std::string::npos) << "Missing function body: " << functionNeedle;
        if (bodyBegin == std::string::npos)
            return {};

        size_t depth = 0u;
        for (size_t offset = bodyBegin; offset < source.size(); ++offset)
        {
            if (source[offset] == '{')
            {
                ++depth;
            }
            else if (source[offset] == '}')
            {
                --depth;
                if (depth == 0u)
                    return source.substr(bodyBegin, offset - bodyBegin + 1u);
            }
        }

        ADD_FAILURE() << "Unterminated function body: " << functionNeedle;
        return {};
    }

    void ExpectNoNeedles(
        const std::string& source,
        const std::vector<std::string_view>& needles,
        std::string_view context)
    {
        for (const auto needle : needles)
        {
            EXPECT_EQ(source.find(needle), std::string::npos)
                << context << " must not contain " << needle;
        }
    }

#if defined(__APPLE__)
    std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateTestMetalDevice()
    {
        auto device = NLS::Render::Backend::CreateMetalRhiDevice(false);
        if (device == nullptr || !device->IsBackendReady())
            return nullptr;
        return device;
    }
#endif
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerRenderDoesNotCallLegacyBridgeDrawData)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));
    const auto renderBody = ExtractFunctionBody(source, "void UIManager::Render(");

    const auto publishBranch = renderBody.find("ShouldPublishUiSnapshotToFrameGraph()");
    const auto publishCall = renderBody.find("PublishCurrentUiSnapshotToFrameGraph()", publishBranch);

    ASSERT_NE(publishBranch, std::string::npos);
    ASSERT_NE(publishCall, std::string::npos);
    EXPECT_EQ(renderBody.find("m_uiBridge->RenderDrawData"), std::string::npos)
        << "DX12 legacy UI direct-submit rendering must not remain reachable from UIManager::Render.";
    EXPECT_EQ(renderBody.find("ImGui_ImplDX12_RenderDrawData"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerBeginFrameAllowsNullRendererBridgeOnFrameGraphOverlayPath)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));
    const auto beginFrameBody = ExtractFunctionBody(source, "void UIManager::BeginFrame(");

    const auto bridgeBackendCheck = beginFrameBody.find("!m_uiBridge->HasRendererBackend()");
    const auto overlayCapabilityCheck = beginFrameBody.find("!ShouldPublishUiSnapshotToFrameGraph()", bridgeBackendCheck);
    ASSERT_NE(bridgeBackendCheck, std::string::npos);
    ASSERT_NE(overlayCapabilityCheck, std::string::npos)
        << "A null/unsupported legacy renderer bridge must not prevent ImGui::NewFrame when "
           "UIOverlayFrameGraph owns migrated rendering.";
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerDoesNotWarnForNullBridgeWhenFrameGraphOverlayOwnsRendering)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));
    const auto constructorBody = ExtractFunctionBody(source, "UIManager::UIManager(");

    const auto officialBackendWarning = constructorBody.find("runtime initialization did not complete");
    ASSERT_NE(officialBackendWarning, std::string::npos);

    const auto overlayCapabilityCheck = constructorBody.rfind(
        "ShouldPublishUiSnapshotToFrameGraph()",
        officialBackendWarning);
    ASSERT_NE(overlayCapabilityCheck, std::string::npos)
        << "Migrated UIOverlayFrameGraph rendering intentionally uses the null bridge, so the legacy "
           "official-backend warning must be gated by frame-graph ownership.";
}

TEST(RHIUiOverlaySourceGuardTests, EditorAndLauncherDoNotSubmitLegacyUiRendering)
{
    const auto editorSource = ReadSourceText(RepoPath("Project/Editor/Core/Editor.cpp"));
    const auto launcherSource = ReadSourceText(RepoPath("Project/Launcher/Core/Launcher.cpp"));
    const auto renderEditorUiBody = ExtractFunctionBody(
        editorSource,
        "void Editor::Core::Editor::RenderEditorUI(");

    EXPECT_EQ(renderEditorUiBody.find("ResolveUISignalSemaphore()"), std::string::npos);
    EXPECT_EQ(renderEditorUiBody.find("SetUICompositionSignal"), std::string::npos);
    EXPECT_EQ(renderEditorUiBody.find("SubmitUIRendering()"), std::string::npos);

    const auto launcherRunBody = ExtractFunctionBody(launcherSource, "LauncherRunResult Launcher::Run(");
    EXPECT_EQ(launcherRunBody.find("SubmitUIRendering()"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, EditorDoesNotSpecialCaseMetalOutOfThreadedRenderingOrPresentation)
{
    const auto contextSource = ReadSourceText(RepoPath("Project/Editor/Core/Context.cpp"));
    const auto contextConstructor = ExtractFunctionBody(
        contextSource,
        "Editor::Core::Context::Context(");
    EXPECT_NE(
        contextConstructor.find(
            "driverSettings.enableThreadedRendering = runtimeSettings.enableThreadedRendering"),
        std::string::npos)
        << "Metal and DX12 must use the same configured threaded rendering lifecycle.";
    EXPECT_EQ(
        contextConstructor.find(
            "graphicsBackend != Render::Settings::EGraphicsBackend::METAL"),
        std::string::npos)
        << "Editor startup must not disable threaded rendering specifically for Metal.";

    const auto editorSource = ReadSourceText(RepoPath("Project/Editor/Core/Editor.cpp"));
    const auto postUpdateBody = ExtractFunctionBody(
        editorSource,
        "void Editor::Core::Editor::PostUpdate(");
    EXPECT_NE(
        postUpdateBody.find("DriverUIAccess::PresentSwapchain(*m_context.driver)"),
        std::string::npos)
        << "Metal and DX12 must enter the same FrameGraph UI presentation path.";
    EXPECT_EQ(
        postUpdateBody.find("EGraphicsBackend::METAL"),
        std::string::npos)
        << "Editor presentation must not skip Metal after the FrameGraph overlay capability is enabled.";
}

TEST(RHIUiOverlaySourceGuardTests, MetalSwapchainPresentCompletionRetainsSignalFence)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto presentBody = ExtractFunctionBody(
        source,
        "bool Present(");

    EXPECT_NE(
        presentBody.find("const auto completionFence = std::move(signalFence)"),
        std::string::npos)
        << "The asynchronous Metal completion block must own the fence after Present returns.";
    EXPECT_NE(presentBody.find("completionFence->Signal()"), std::string::npos);
    EXPECT_EQ(presentBody.find("signalFence->Signal()"), std::string::npos)
        << "Capturing the Present parameter directly can leave the completion block with a dangling reference.";
}

TEST(RHIUiOverlaySourceGuardTests, MetalSwapchainUsesDrawableCountAndValidatedLogicalImageIndices)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto constructorBody = ExtractFunctionBody(
        source,
        "MetalSwapchain(id<MTLDevice> device");
    const auto acquireBody = ExtractFunctionBody(source, "AcquireNextImage(");
    const auto getBackbufferBody = ExtractFunctionBody(source, "GetBackbufferView(uint32_t index)");
    const auto presentBody = ExtractFunctionBody(source, "bool Present(");

    EXPECT_NE(constructorBody.find("ResolveMetalSwapchainImageCount"), std::string::npos);
    EXPECT_NE(constructorBody.find("m_layer.maximumDrawableCount = m_imageCount"), std::string::npos);
    EXPECT_NE(acquireBody.find("m_currentImageIndex = m_nextImageIndex"), std::string::npos);
    EXPECT_NE(acquireBody.find("m_nextImageIndex + 1u"), std::string::npos);
    EXPECT_NE(getBackbufferBody.find("index != *m_currentImageIndex"), std::string::npos);
    EXPECT_NE(presentBody.find("imageIndex != *m_currentImageIndex"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, MetalPixelReadbackUsesAnAsynchronousCompletionToken)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto beginBody = ExtractFunctionBody(source, "RHIReadbackResult BeginReadPixels(");

    EXPECT_NE(beginBody.find("MetalPixelReadbackCompletionToken"), std::string::npos);
    EXPECT_NE(beginBody.find("[readbackCommands commit]"), std::string::npos);
    EXPECT_EQ(beginBody.find("waitUntilCompleted"), std::string::npos)
        << "BeginReadPixels must return without synchronously waiting for the GPU.";
    EXPECT_NE(source.find("GetMetalReadbackBytesPerPixel"), std::string::npos);
    EXPECT_NE(source.find("MTLPixelFormatRGBA16Float"), std::string::npos);
    EXPECT_NE(source.find("MTLPixelFormatR32Float"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, MetalGraphicsPipelineMapsDx12EquivalentFixedFunctionState)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto createPipelineBody = ExtractFunctionBody(
        source,
        "std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(");
    const auto bindPipelineBody = ExtractFunctionBody(
        source,
        "void BindGraphicsPipeline(");

    EXPECT_NE(
        createPipelineBody.find(
            "descriptor.alphaToCoverageEnabled = desc.blendState.alphaToCoverageEnable"),
        std::string::npos)
        << "Metal must transfer the RHI alpha-to-coverage state into the native pipeline descriptor.";
    EXPECT_NE(
        createPipelineBody.find("desc.blendState.independentBlendEnable"),
        std::string::npos);
    EXPECT_NE(
        createPipelineBody.find("const size_t blendStateIndex"),
        std::string::npos)
        << "When independent blending is disabled, Metal must apply render target zero's blend state to every attachment.";
    EXPECT_NE(
        bindPipelineBody.find("setStencilReferenceValue:"),
        std::string::npos)
        << "Metal's dynamic stencil reference must be bound with the graphics pipeline, matching DX12 OMSetStencilRef.";
    EXPECT_NE(
        bindPipelineBody.find("depthStencilState.stencilReference"),
        std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, MetalSamplerMapsOnlyNativeDx12EquivalentState)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto createSamplerBody = ExtractFunctionBody(source, "CreateSampler(");

    EXPECT_NE(createSamplerBody.find("desc.mipLodBias != 0.0f"), std::string::npos)
        << "Metal has no native sampler LOD-bias state, so a non-zero value must not be silently ignored.";
    EXPECT_NE(createSamplerBody.find("UsesMetalBorderColor(desc)"), std::string::npos);
    EXPECT_NE(createSamplerBody.find("!borderColor.has_value()"), std::string::npos)
        << "Custom DX12 border colors that Metal cannot represent must be rejected.";
    EXPECT_NE(createSamplerBody.find("descriptor.borderColor = *borderColor"), std::string::npos);
    EXPECT_EQ(createSamplerBody.find("(void)desc.mipLodBias"), std::string::npos);

    EXPECT_NE(source.find("MTLSamplerBorderColorTransparentBlack"), std::string::npos);
    EXPECT_NE(source.find("MTLSamplerBorderColorOpaqueBlack"), std::string::npos);
    EXPECT_NE(source.find("MTLSamplerBorderColorOpaqueWhite"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, MetalSupportsDx12CompressedTextureAssetFormats)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto createTextureBody = ExtractFunctionBody(source, "CreateTexture(");
    const auto updateTextureBody = ExtractFunctionBody(source, "UpdateTexture(");
    const auto capabilitiesBody = ExtractFunctionBody(source, "CreateCapabilities(id<MTLDevice> device)");

    for (const auto formatMapping : {
        "MTLPixelFormatBC1_RGBA",
        "MTLPixelFormatBC3_RGBA",
        "MTLPixelFormatBC5_RGUnorm",
        "MTLPixelFormatBC7_RGBAUnorm" })
    {
        EXPECT_NE(source.find(formatMapping), std::string::npos)
            << "Metal must map every BC format produced by the DirectXTex asset encoder.";
    }
    EXPECT_NE(createTextureBody.find("CalculateTextureRowPitch"), std::string::npos);
    EXPECT_NE(createTextureBody.find("CalculateTextureSlicePitch"), std::string::npos)
        << "Initial compressed uploads must use block geometry instead of bytes-per-pixel math.";
    EXPECT_NE(updateTextureBody.find("formatInfo->blockHeight"), std::string::npos);
    EXPECT_NE(updateTextureBody.find("alignedOrTouchesRightEdge"), std::string::npos);
    EXPECT_NE(updateTextureBody.find("alignedOrTouchesBottomEdge"), std::string::npos)
        << "Compressed partial updates must enforce Metal's block alignment rules.";
    EXPECT_NE(
        updateTextureBody.find("RHIUpdateStatusCode::Success"),
        std::string::npos)
        << "A completed Metal replaceRegion call must not return the default Unsupported status.";
    EXPECT_NE(source.find("ResolveMetalBufferToTextureCopyLayout"), std::string::npos);
    EXPECT_NE(source.find("minimumSlicePitch"), std::string::npos);
    EXPECT_NE(source.find("source buffer span is smaller"), std::string::npos)
        << "Metal buffer-to-texture copies must validate compressed block layout and source bounds.";
    EXPECT_NE(capabilitiesBody.find("device.supportsBCTextureCompression"), std::string::npos);
    EXPECT_NE(capabilitiesBody.find("capabilities.SynchronizeLegacyFields()"), std::string::npos)
        << "Structured limits must stay synchronized with the legacy capability fields.";
}

TEST(RHIUiOverlaySourceGuardTests, MetalValidatesResourceCopiesBeforeEncoding)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto bufferValidationBody = ExtractFunctionBody(source, "ValidateMetalBufferCopyRegion(");
    const auto textureValidationBody = ExtractFunctionBody(source, "ValidateMetalTextureCopyDesc(");
    const auto copyBufferBody = ExtractFunctionBody(source, "void CopyBuffer(");
    const auto copyTextureBody = ExtractFunctionBody(source, "void CopyTexture(");

    EXPECT_NE(bufferValidationBody.find("sourceDesc.size - region.srcOffset"), std::string::npos);
    EXPECT_NE(bufferValidationBody.find("destinationDesc.size - region.dstOffset"), std::string::npos)
        << "Buffer copy offsets and sizes must be checked without integer overflow.";
    EXPECT_NE(copyBufferBody.find("ValidateMetalBufferCopyRegion"), std::string::npos);
    EXPECT_LT(
        copyBufferBody.find("ValidateMetalBufferCopyRegion"),
        copyBufferBody.find("BeginBlitEncoder"))
        << "Invalid buffer copies must be rejected before creating a Metal encoder.";

    EXPECT_NE(textureValidationBody.find("mipLevelCount != 1u"), std::string::npos);
    EXPECT_NE(textureValidationBody.find("baseMipLevel >= sourceDesc.mipLevels"), std::string::npos);
    EXPECT_NE(textureValidationBody.find("baseArrayLayer >= sourceLayerCount"), std::string::npos);
    EXPECT_NE(textureValidationBody.find("sourceFormat != destinationFormat"), std::string::npos);
    EXPECT_NE(textureValidationBody.find("isInsideBounds(sourceBounds)"), std::string::npos);
    EXPECT_NE(textureValidationBody.find("isBlockAligned(sourceBounds)"), std::string::npos);
    EXPECT_NE(textureValidationBody.find("isBlockAligned(destinationBounds)"), std::string::npos);
    EXPECT_NE(copyTextureBody.find("ValidateMetalTextureCopyDesc"), std::string::npos);
    EXPECT_LT(
        copyTextureBody.find("ValidateMetalTextureCopyDesc"),
        copyTextureBody.find("BeginBlitEncoder"))
        << "Invalid texture copies must be rejected before creating a Metal encoder.";
}

TEST(RHIUiOverlaySourceGuardTests, MetalPreservesPartialPushConstantWrites)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto pushConstantsBody = ExtractFunctionBody(source, "void PushConstants(");
    const auto createLayoutBody = ExtractFunctionBody(source, "CreatePipelineLayout(");

    EXPECT_NE(pushConstantsBody.find("m_pushConstantData.data() + offset"), std::string::npos)
        << "Metal must apply the RHI push constant byte offset instead of replacing the buffer at byte zero.";
    EXPECT_NE(pushConstantsBody.find("m_pushConstantDataSize"), std::string::npos)
        << "A later partial write must upload the preceding push constant bytes too.";
    EXPECT_NE(pushConstantsBody.find("m_boundPushConstantRanges"), std::string::npos)
        << "Metal push constant writes must be constrained by the currently bound pipeline layout.";
    EXPECT_NE(pushConstantsBody.find("rangeIt->registerSpace"), std::string::npos);
    EXPECT_NE(pushConstantsBody.find("rangeIt->shaderRegister"), std::string::npos);
    EXPECT_NE(pushConstantsBody.find("atIndex:*bufferIndex"), std::string::npos)
        << "Metal push constants must use the buffer index declared by the pipeline layout, not one hard-coded slot.";
    EXPECT_NE(pushConstantsBody.find("size % sizeof(uint32_t)"), std::string::npos);
    EXPECT_NE(pushConstantsBody.find("offset % sizeof(uint32_t)"), std::string::npos)
        << "Metal and DX12 must enforce the same root-constant alignment.";
    EXPECT_NE(createLayoutBody.find("kRHIMaxPushConstantBytes"), std::string::npos)
        << "Invalid Metal pipeline layouts must be rejected before command recording.";
}

TEST(RHIUiOverlaySourceGuardTests, MetalUsesPrivateGpuOnlyResourcesAndStagingCopies)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto createBufferBody = ExtractFunctionBody(source, "CreateBuffer(");
    const auto createTextureBody = ExtractFunctionBody(source, "CreateTexture(");
    const auto updateTextureBody = ExtractFunctionBody(source, "UpdateTexture(");
    const auto readBufferBody = ExtractFunctionBody(source, "BeginReadBuffer(");

    EXPECT_NE(createBufferBody.find("MTLResourceStorageModePrivate"), std::string::npos);
    EXPECT_NE(createBufferBody.find("newBufferWithBytes:uploadDesc.data"), std::string::npos)
        << "GPUOnly initial buffer data must be uploaded through a CPU-visible staging allocation.";
    EXPECT_NE(createBufferBody.find("copyFromBuffer:staging"), std::string::npos);
    EXPECT_NE(readBufferBody.find("source->GetBuffer().storageMode == MTLStorageModePrivate"), std::string::npos);
    EXPECT_NE(readBufferBody.find("copyFromBuffer:source->GetBuffer()"), std::string::npos)
        << "Private Metal buffer readback requires a GPU copy into shared staging memory.";

    EXPECT_NE(createTextureBody.find("desc.memoryUsage != NLS::Render::RHI::MemoryUsage::GPUOnly"), std::string::npos);
    EXPECT_NE(createTextureBody.find("MTLStorageModePrivate"), std::string::npos);
    EXPECT_NE(createTextureBody.find("MTLStorageModeShared"), std::string::npos);
    EXPECT_NE(createTextureBody.find("copyFromTexture:stagingTexture"), std::string::npos)
        << "GPUOnly texture initial data must use a shared staging texture and a GPU blit.";
    EXPECT_NE(updateTextureBody.find("copyFromTexture:stagingTexture"), std::string::npos)
        << "Private Metal texture updates must stage rather than calling replaceRegion on the destination.";
}

TEST(RHIUiOverlaySourceGuardTests, MetalUsesNativeGpuQueueSynchronization)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto submitBody = ExtractFunctionBody(
        source,
        "NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(");
    const auto presentBody = ExtractFunctionBody(
        source,
        "NLS::Render::RHI::RHIQueueOperationResult PresentChecked(");
    const auto readBufferBody = ExtractFunctionBody(source, "BeginReadBuffer(");

    EXPECT_NE(source.find("[device newSharedEvent]"), std::string::npos);
    EXPECT_NE(submitBody.find("encodeWaitForEvent:"), std::string::npos);
    EXPECT_NE(submitBody.find("encodeSignalEvent:"), std::string::npos)
        << "Metal queue dependencies must execute on the GPU rather than completion-handler atomics.";
    EXPECT_EQ(submitBody.find("cannot submit before its CPU-visible wait semaphore"), std::string::npos);
    EXPECT_NE(presentBody.find("waitSemaphores"), std::string::npos)
        << "Swapchain presentation must wait for render-finished GPU work.";
    EXPECT_NE(readBufferBody.find("encodeWaitForEvent:"), std::string::npos)
        << "Async readback must honor compute/graphics queue handoff semaphores.";
    EXPECT_NE(source.find("m_computeQueue([m_device newCommandQueue])"), std::string::npos);
    EXPECT_NE(source.find("m_copyQueue([m_device newCommandQueue])"), std::string::npos)
        << "Graphics, compute, and copy must not all alias one serial Metal command queue.";
    EXPECT_EQ(source.find("isIndexedObjectDataPushConstant"), std::string::npos)
        << "Object constants must use the same register/space mapping as every other push constant range.";
}

#if defined(__APPLE__)
TEST(RHIUiOverlaySourceGuardTests, MetalRejectsSamplerStateThatItCannotRepresent)
{
    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalCreatesEveryNativeBorderColor)
{
    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalSubmitsParallelRecordedCommandBuffersInArrayOrder)
{
    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalExecutesNonZeroOffsetPushConstantWrites)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalEnforcesDx12EquivalentResourceMemoryUsage)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalBindingSetsClearMissingSlotsAndValidatePipelineCompatibility)
{
    const auto source = ReadSourceText(
        RepoPath("Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.mm"));
    const auto bindBindingSetBody = ExtractFunctionBody(source, "void BindBindingSet(");
    const auto createBindingLayoutBody = ExtractFunctionBody(source, "CreateBindingLayout(");
    const auto createBindingSetBody = ExtractFunctionBody(source, "CreateBindingSet(");

    EXPECT_NE(bindBindingSetBody.find("expectedEntriesPresent"), std::string::npos);
    EXPECT_NE(bindBindingSetBody.find("actualEntriesCompatible"), std::string::npos)
        << "Metal binding sets must be checked against the currently bound pipeline layout.";
    EXPECT_NE(bindBindingSetBody.find("nativeBuffer = buffer != nullptr ? buffer->GetBuffer() : nil"), std::string::npos);
    EXPECT_NE(bindBindingSetBody.find("nativeTexture = textureView != nullptr"), std::string::npos);
    EXPECT_NE(bindBindingSetBody.find("nativeSampler = sampler != nullptr ? sampler->GetSampler() : nil"), std::string::npos)
        << "Missing Metal descriptors must clear the corresponding native slot instead of inheriting a previous draw's resource.";
    EXPECT_NE(createBindingLayoutBody.find("ValidateMetalBindingIndexSpan"), std::string::npos);
    EXPECT_NE(createBindingLayoutBody.find("overlap the same Metal resource index"), std::string::npos);
    EXPECT_NE(createBindingSetBody.find("out-of-range buffer binding"), std::string::npos);
    EXPECT_NE(createBindingSetBody.find("incompatible usage"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, MetalTracksDx12EquivalentResourceStatesAndBindingBounds)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalOrdersGpuWorkAcrossNativeQueuesWithSharedEvents)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalCreatesAndUpdatesAdvertisedCompressedTextures)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalRejectsInvalidResourceCopiesAndExecutesValidCopies)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, MetalPixelReadbackIsAsyncAndSupportsDx12WideSourceLayouts)
{
    using namespace NLS::Render::RHI;

    auto device = CreateTestMetalDevice();
    if (device == nullptr)
        GTEST_SKIP() << "No Metal device is available.";

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

TEST(RHIUiOverlaySourceGuardTests, LauncherAlwaysEnablesThreadedLifecycleForUiOnlyFrameGraphRendering)
{
    const auto launcherSource = ReadSourceText(RepoPath("Project/Launcher/Core/Launcher.cpp"));
    const auto setupContextBody = ExtractFunctionBody(launcherSource, "void Launcher::SetupContext(");

    EXPECT_NE(
        setupContextBody.find("driverSettings.enableThreadedRendering = true"),
        std::string::npos)
        << "Launcher has no scene renderer, so its migrated UI-only frame must always create the "
           "threaded lifecycle used by PublishUiOnlyFrame.";
    EXPECT_EQ(
        setupContextBody.find("IsEnvironmentFlagEnabled(\"NLS_ENABLE_THREADED_RENDERING\")"),
        std::string::npos)
        << "The Launcher UI presentation path must not depend on an opt-in environment variable.";
}

TEST(RHIUiOverlaySourceGuardTests, LauncherUsesOnlyRegisteredProjectEditorBinding)
{
    const auto launcherSource = ReadSourceText(RepoPath("Project/Launcher/Core/Launcher.cpp"));
    const auto openProjectBody = ExtractFunctionBody(launcherSource, "void OpenProject(");

    EXPECT_NE(
        openProjectBody.find("m_settings.IsRegisteredEngineExecutablePath(boundEditorExecutablePath)"),
        std::string::npos)
        << "A project-bound Editor must still belong to the Launcher's current installation list.";
    EXPECT_EQ(
        openProjectBody.find("LauncherSettings::IsValidEngineExecutablePath(boundEditorExecutablePath)"),
        std::string::npos)
        << "An existing stale Editor executable must not override the configured default installation.";
}

TEST(RHIUiOverlaySourceGuardTests, LauncherProvidesShaderManagerForUiOverlayBuiltInShader)
{
    const auto launcherSource = ReadSourceText(RepoPath("Project/Launcher/Core/Launcher.cpp"));
    const auto launcherHeader = ReadSourceText(RepoPath("Project/Launcher/Core/Launcher.h"));
    const auto setupContextBody = ExtractFunctionBody(launcherSource, "void Launcher::SetupContext(");
    const auto constructorBody = ExtractFunctionBody(launcherSource, "Launcher::Launcher(");
    const auto destructorBody = ExtractFunctionBody(launcherSource, "Launcher::~Launcher(");

    EXPECT_NE(
        setupContextBody.find("const auto engineAssetsRoot = EnsureTrailingPathSeparator(assetsRoot / \"Engine\")"),
        std::string::npos)
        << "Built-in resource virtual paths are appended to the configured root, so Launcher must "
           "preserve the trailing path separator contract used by Editor and Game.";
    EXPECT_NE(
        setupContextBody.find("ShaderManager::ProvideAssetPaths({}, engineAssetsRoot)"),
        std::string::npos)
        << "The Launcher must authorize its deployed Assets/Engine root before loading the built-in "
           "RHIImGuiOverlay HLSL shader.";
    EXPECT_EQ(
        setupContextBody.find("ServiceLocator::Provide<ShaderManager>(m_shaderManager)"),
        std::string::npos)
        << "Registering during SetupContext leaves a dangling service if later Launcher construction throws.";
    const auto existingServiceGuard = constructorBody.find("ServiceLocator::Contains<ShaderManager>()");
    const auto addPanel = constructorBody.find("m_canvas.AddPanel(*m_mainPanel)");
    const auto provideService = constructorBody.find("ServiceLocator::Provide<ShaderManager>(m_shaderManager)");
    EXPECT_NE(existingServiceGuard, std::string::npos)
        << "Launcher must not overwrite a ShaderManager owned by another context.";
    EXPECT_NE(provideService, std::string::npos)
        << "RHIImGuiOverlayRenderer resolves the overlay shader through the ShaderManager service.";
    EXPECT_LT(existingServiceGuard, provideService);
    EXPECT_LT(addPanel, provideService)
        << "Service registration must be the final potentially persistent action in Launcher construction.";
    EXPECT_NE(launcherHeader.find("ShaderManager m_shaderManager"), std::string::npos)
        << "The ShaderManager service must outlive all Launcher UI frame recording.";
    const auto threadedShutdown = destructorBody.find("m_driver->ShutdownThreadedRendering()");
    const auto shaderUnload = destructorBody.find("m_shaderManager.UnloadResources()");
    EXPECT_NE(threadedShutdown, std::string::npos)
        << "Launcher must stop the RHI worker before releasing its overlay shader resources.";
    EXPECT_NE(shaderUnload, std::string::npos)
        << "Launcher-owned shader resources must be released before process teardown.";
    const auto ownedServiceCheck = destructorBody.find(
        "&Core::ServiceLocator::Get<ShaderManager>() == &m_shaderManager");
    const auto removeService = destructorBody.find("ServiceLocator::Remove<ShaderManager>()");
    EXPECT_NE(ownedServiceCheck, std::string::npos)
        << "Launcher must not remove a ShaderManager that replaced its own service registration.";
    EXPECT_NE(removeService, std::string::npos)
        << "Launcher must unregister its ShaderManager before the member is destroyed.";
    EXPECT_LT(ownedServiceCheck, removeService);
    EXPECT_LT(threadedShutdown, shaderUnload)
        << "The RHI worker may still reference the UI overlay shader until threaded rendering stops.";
}

TEST(RHIUiOverlaySourceGuardTests, OverlayRendererFontAtlasAndTextureRegistryDoNotOwnNativeDx12QueueWork)
{
    const std::vector<std::filesystem::path> migratedFiles = {
        RepoPath("Runtime/Rendering/UI/RHIImGuiOverlayRenderer.cpp"),
        RepoPath("Runtime/Rendering/UI/RHIImGuiFontAtlas.cpp"),
        RepoPath("Runtime/Rendering/UI/RHIImGuiTextureRegistry.cpp"),
    };

    for (const auto& path : migratedFiles)
    {
        ASSERT_TRUE(std::filesystem::exists(path)) << "Missing migrated UI file: " << path.string();
        const auto source = ReadSourceText(path);
        ExpectNoNeedles(
            source,
            {
                "ExecuteCommandLists(",
                "->ExecuteCommandLists",
                "Signal(",
                "->Signal(",
                "PresentInternal",
                "->Present(",
                ".Present(",
                "ImGui_ImplDX12_RenderDrawData",
            },
            path.generic_string());
    }
}

TEST(RHIUiOverlaySourceGuardTests, FontAtlasUploadAvoidsImmediateCreateTextureInitialData)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/UI/RHIImGuiFontAtlas.cpp"));

    EXPECT_EQ(source.find("CreateTexture(textureDesc, uploadDesc)"), std::string::npos)
        << "Font atlas upload must not use immediate RHIDevice::CreateTexture(initialData) because the "
           "DX12 implementation owns a private command list/fence wait on that path.";
    EXPECT_NE(source.find("CreateTexture(textureDesc)"), std::string::npos);
    EXPECT_NE(source.find("CopyBufferToTexture"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, AssetBrowserThumbnailUploadAvoidsImmediateTextureInitialData)
{
    const auto source = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto uploadBody = ExtractFunctionBody(
        source,
        "bool Editor::Panels::AssetBrowser::LoadDecodedCachedThumbnailTexture(");

    EXPECT_EQ(uploadBody.find("CreateFromRgba8Memory"), std::string::npos)
        << "Asset Browser cached thumbnail upload must not use immediate TextureLoader RGBA uploads "
           "because the RHI initial-data path can synchronously wait on GPU work and stall UI scrolling.";
    EXPECT_NE(uploadBody.find("RequestUiRgba8TextureUpload"), std::string::npos)
        << "Cached thumbnail RGBA pixels should be submitted to the renderer-owned upload queue.";
}

TEST(RHIUiOverlaySourceGuardTests, OverlayRendererDoesNotExposeRecordConveniencePath)
{
    const auto header = ReadSourceText(RepoPath("Runtime/Rendering/UI/RHIImGuiOverlayRenderer.h"));

    EXPECT_EQ(header.find("Record("), std::string::npos)
        << "UI overlay recording must be split into PrepareFrameResources before the render pass "
           "and RecordPrepared inside the render pass; public Record(...) convenience paths can bypass "
           "dynamic-buffer preparation or record upload barriers inside an active render pass.";
}

TEST(RHIUiOverlaySourceGuardTests, DX12LegacyBridgeImplementationIsRemoved)
{
    const auto path = RepoPath("Runtime/Rendering/RHI/Backends/DX12/DX12UIBridge.cpp");
    if (!std::filesystem::exists(path))
        return;

    const auto source = ReadSourceText(path);
    ExpectNoNeedles(
        source,
        {
            "ImGui_ImplDX12_RenderDrawData",
            "WaitForBackbufferReuse",
            "WaitForAllocatorReuse",
            "DX12UIBridge::ExecuteCommandLists",
            "ID3D12CommandQueue",
            "SubmitCommandBuffer",
            "CreateDX12RHIUIBridge",
        },
        path.generic_string());
}

TEST(RHIUiOverlaySourceGuardTests, RHIUIBridgeFactoryNeverCreatesLegacyDX12Bridge)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/RHI/Utils/RHIUIBridge.cpp"));
    const auto factoryBody = ExtractFunctionBody(source, "std::unique_ptr<RHIUIBridge> CreateRHIUIBridge(");

    const auto capabilityCheck = factoryBody.find("GetUIOverlayFrameGraphFeature");
    const auto nullBridgeReturn = factoryBody.find("return std::make_unique<NullUIBridge>()", capabilityCheck);

    ASSERT_NE(capabilityCheck, std::string::npos)
        << "The UI bridge factory must inspect UIOverlayFrameGraph capability before selecting a legacy bridge.";
    ASSERT_NE(nullBridgeReturn, std::string::npos)
        << "When UIOverlayFrameGraph is supported, the factory must choose the null bridge and let "
           "the RHI overlay renderer own UI work.";
    EXPECT_EQ(factoryBody.find("CreateDX12RHIUIBridge"), std::string::npos)
        << "The legacy DX12 UI bridge must not remain as a runtime fallback.";
    EXPECT_NE(factoryBody.find("overlayFeature.reason"), std::string::npos)
        << "Unsupported UIOverlayFrameGraph state must log the capability reason instead of silently "
           "choosing a legacy renderer bridge.";
}

TEST(RHIUiOverlaySourceGuardTests, RhiThreadPrepareUiRenderDoesNotStartStandaloneFrameWhenOverlayIsSupported)
{
    const auto source = ReadSourceText(RepoPath("Runtime/Rendering/Context/RhiThreadCoordinator.cpp"));
    const auto prepareBody = ExtractFunctionBody(source, "bool RhiThreadCoordinator::PrepareUIRender(");
    const auto presentBody = ExtractFunctionBody(source, "void RhiThreadCoordinator::PresentSwapchain(");

    const auto capabilityCheck = prepareBody.find("GetUIOverlayFrameGraphFeature");
    const auto standaloneBegin = prepareBody.find("BeginStandaloneUiExplicitFrame");
    ASSERT_NE(standaloneBegin, std::string::npos);
    ASSERT_NE(capabilityCheck, std::string::npos)
        << "PrepareUIRender must inspect UIOverlayFrameGraph before considering the legacy "
           "standalone UI explicit frame path.";
    EXPECT_LT(capabilityCheck, standaloneBegin)
        << "Migrated UI overlay devices must fail closed before standalone UI explicit frame startup.";
    EXPECT_EQ(prepareBody.find("PublishUiOnlyFrame"), std::string::npos)
        << "PrepareUIRender must not consume the pending UI snapshot before a scene package can attach it.";
    EXPECT_NE(presentBody.find("PublishUiOnlyFrame"), std::string::npos)
        << "PresentSwapchain owns the UI-only fallback after scene publication had a chance to consume the snapshot.";
    EXPECT_EQ(presentBody.find("NotifyThreadedWorkers"), std::string::npos)
        << "Frame publication already wakes the workers; PresentSwapchain must not issue a redundant hot-path wake.";
    EXPECT_EQ(presentBody.find("TryDrainThreadedRendering"), std::string::npos)
        << "UE-style threaded Present must not synchronously drain render or RHI work on the caller thread.";
    const auto publishUiOnly = presentBody.find("DriverUIAccess::PublishUiOnlyFrame(");
    ASSERT_NE(publishUiOnly, std::string::npos);
    const auto publishUiOnlyEnd = presentBody.find(");", publishUiOnly);
    ASSERT_NE(publishUiOnlyEnd, std::string::npos);
    const auto publishUiOnlyCall = presentBody.substr(
        publishUiOnly,
        publishUiOnlyEnd - publishUiOnly);
    EXPECT_NE(publishUiOnlyCall.find("false"), std::string::npos)
        << "The UI-only fallback must use the nonblocking frame-slot publication path.";
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerResolveTextureViewUsesPackedUiTextureIdentityOnMigratedPath)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));
    const auto resolveBody = ExtractFunctionBody(
        source,
        "NLS::Render::RHI::NativeHandle UIManager::ResolveTextureView(");

    ASSERT_NE(resolveBody.find("ShouldPublishUiSnapshotToFrameGraph()"), std::string::npos);
    EXPECT_NE(resolveBody.find("DriverUIAccess::RegisterUiTextureView"), std::string::npos);
    EXPECT_NE(resolveBody.find("PackUiTextureIdForImGui"), std::string::npos);
    EXPECT_NE(resolveBody.find("nativeHandle.value"), std::string::npos);
    EXPECT_NE(resolveBody.find("nativeHandle.handle"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, ImageWidgetsPreferPackedUiTextureIdentityWhenAvailable)
{
    const auto imageSource = ReadSourceText(RepoPath("Runtime/UI/Widgets/Visual/Image.cpp"));
    const auto imageBody = ExtractFunctionBody(imageSource, "void Image::_Draw_Impl()");
    ASSERT_NE(imageBody.find("ResolveTextureId(textureView)"), std::string::npos);
    EXPECT_EQ(imageBody.find("nativeHandle."), std::string::npos);

    const auto buttonSource = ReadSourceText(RepoPath("Runtime/UI/Widgets/Buttons/ButtonImage.cpp"));
    const auto buttonBody = ExtractFunctionBody(buttonSource, "void ButtonImage::_Draw_Impl()");
    ASSERT_NE(buttonBody.find("ResolveTextureId(textureView)"), std::string::npos);
    EXPECT_EQ(buttonBody.find("nativeHandle."), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, ButtonImageHonorsDisabledState)
{
    const auto buttonSource = ReadSourceText(RepoPath("Runtime/UI/Widgets/Buttons/ButtonImage.cpp"));
    const auto buttonBody = ExtractFunctionBody(buttonSource, "void ButtonImage::_Draw_Impl()");

    EXPECT_NE(buttonBody.find("const bool wasDisabled = disabled"), std::string::npos);
    EXPECT_NE(buttonBody.find("ImGui::BeginDisabled()"), std::string::npos);
    EXPECT_NE(buttonBody.find("ImGui::EndDisabled()"), std::string::npos);
    EXPECT_LT(buttonBody.find("ImGui::BeginDisabled()"), buttonBody.find("ImGui::ImageButton"));
    EXPECT_LT(buttonBody.find("ImGui::ImageButton"), buttonBody.find("ImGui::EndDisabled()"));
}

TEST(RHIUiOverlaySourceGuardTests, DirectImageCallSitesUseUnifiedTextureIdResolver)
{
    const auto editorTopBarSource = ReadSourceText(RepoPath("Project/Editor/Panels/EditorTopBar.cpp"));
    const auto editorResolveBody = ExtractFunctionBody(editorTopBarSource, "void* ResolveTextureId(");
    EXPECT_NE(editorResolveBody.find("ResolveTextureId(p_textureView)"), std::string::npos);
    EXPECT_EQ(editorResolveBody.find("IsValid()"), std::string::npos);

    const auto launcherSource = ReadSourceText(RepoPath("Project/Launcher/Core/Launcher.cpp"));
    const auto drawTextureBody = ExtractFunctionBody(launcherSource, "void DrawTexture(");
    EXPECT_NE(drawTextureBody.find("ResolveTextureId(textureView)"), std::string::npos);
    EXPECT_EQ(drawTextureBody.find("IsValid()"), std::string::npos);

    const auto assetBrowserSource = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto assetBrowserHeader = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.h"));
    const auto assetBrowserResolveBody = ExtractFunctionBody(
        assetBrowserSource,
        "void* Editor::Panels::AssetBrowser::ResolveAssetBrowserTextureHandle(");
    EXPECT_EQ(
        assetBrowserResolveBody.find("ResolveTextureId(textureView)"),
        std::string::npos)
        << "AssetBrowser fixed icon draw is a hot path; texture ids should be cached per icon/debug-name pair.";
    EXPECT_NE(assetBrowserResolveBody.find("m_assetBrowserTextureHandleCache"), std::string::npos);
    EXPECT_NE(assetBrowserResolveBody.find("return found->second.textureId"), std::string::npos);
    EXPECT_NE(assetBrowserResolveBody.find("ResolveTextureId(resolvedTextureView)"), std::string::npos);
    EXPECT_LT(
        assetBrowserResolveBody.find("return found->second.textureId"),
        assetBrowserResolveBody.find("ResolveTextureId(resolvedTextureView)"));
    EXPECT_EQ(assetBrowserHeader.find("std::string debugName;"), std::string::npos)
        << "Texture2D currently owns a single explicit texture view, so debug-name keyed UI id cache entries "
           "can duplicate release/retire calls for the same view.";
    EXPECT_NE(
        ExtractFunctionBody(assetBrowserSource, "void Editor::Panels::AssetBrowser::Clear(")
            .find("ReleaseAssetBrowserTextureHandleCache(false)"),
        std::string::npos)
        << "Fixed-icon UI texture ids must be invalidated when the browser clears refresh-owned texture state.";
    EXPECT_EQ(assetBrowserResolveBody.find("ResolveTextureView(textureView)"), std::string::npos);
    EXPECT_EQ(assetBrowserResolveBody.find("IsValid()"), std::string::npos);

    const auto thumbnailResolveBody = ExtractFunctionBody(
        assetBrowserSource,
        "Editor::Panels::AssetBrowser::ThumbnailTextureHandle Editor::Panels::AssetBrowser::ResolveCachedThumbnailTextureHandle(");
    EXPECT_EQ(
        thumbnailResolveBody.find("ResolveTextureId(found->second.textureView)"),
        std::string::npos)
        << "AssetBrowser thumbnail draw is a hot path; texture ids should be resolved once when the cached thumbnail texture is loaded.";
    EXPECT_NE(thumbnailResolveBody.find("found->second.textureId"), std::string::npos);
    EXPECT_EQ(thumbnailResolveBody.find("ResolveTextureView(found->second.textureView)"), std::string::npos);
    EXPECT_EQ(thumbnailResolveBody.find("IsValid()"), std::string::npos);

    const auto thumbnailLoadBody = ExtractFunctionBody(
        assetBrowserSource,
        "bool Editor::Panels::AssetBrowser::LoadDecodedCachedThumbnailTexture(");
    EXPECT_EQ(thumbnailLoadBody.find("ResolveTextureId("), std::string::npos)
        << "Decoded cached thumbnails should submit renderer-owned uploads; UI texture ids are resolved after upload completion.";

    const auto thumbnailConsumeBody = ExtractFunctionBody(
        assetBrowserSource,
        "void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()");
    EXPECT_NE(thumbnailConsumeBody.find("ResolveTextureId(result.textureView)"), std::string::npos);
    EXPECT_NE(thumbnailConsumeBody.find("textureId"), std::string::npos);
    EXPECT_EQ(thumbnailLoadBody.find("ResolveTextureView(textureView)"), std::string::npos);
    EXPECT_EQ(thumbnailLoadBody.find("IsValid()"), std::string::npos);

    const auto thumbnailServiceSource = ReadSourceText(
        RepoPath("Project/Editor/Assets/AssetThumbnailService.cpp"));
    const std::string gpuPreviewNeedle =
        "std::optional<AssetThumbnailServiceResult> AssetThumbnailService::GenerateNextThumbnail(";
    size_t gpuPreviewBegin = std::string::npos;
    for (size_t searchBegin = 0u;; searchBegin += gpuPreviewNeedle.size())
    {
        const auto candidateBegin = thumbnailServiceSource.find(gpuPreviewNeedle, searchBegin);
        if (candidateBegin == std::string::npos)
            break;

        const auto candidateBodyBegin = thumbnailServiceSource.find('{', candidateBegin);
        ASSERT_NE(candidateBodyBegin, std::string::npos);
        const auto candidateSignature =
            thumbnailServiceSource.substr(candidateBegin, candidateBodyBegin - candidateBegin);
        if (candidateSignature.find("IEditorThumbnailPreviewRenderer& previewRenderer") != std::string::npos)
        {
            gpuPreviewBegin = candidateBegin;
            break;
        }
        searchBegin = candidateBegin;
    }
    ASSERT_NE(gpuPreviewBegin, std::string::npos);
    const auto completedReadbackGuard = thumbnailServiceSource.find(
        "!preview.completedPendingReadback",
        gpuPreviewBegin);
    const auto renderTelemetry = thumbnailServiceSource.find(
        "ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender",
        gpuPreviewBegin);
    ASSERT_NE(completedReadbackGuard, std::string::npos);
    ASSERT_NE(renderTelemetry, std::string::npos);
    EXPECT_LT(completedReadbackGuard, renderTelemetry)
        << "GPU preview readback polling must not be counted as a second ThumbnailGpuPreviewRender sample.";
}

TEST(RHIUiOverlaySourceGuardTests, UIManagerResourceNotificationsRouteThroughFrameGraphOverlayResources)
{
    const auto source = ReadSourceText(RepoPath("Runtime/UI/UIManager.cpp"));

    const auto notifyResizeBody = ExtractFunctionBody(source, "void UIManager::NotifySwapchainWillResize()");
    EXPECT_NE(notifyResizeBody.find("ShouldPublishUiSnapshotToFrameGraph()"), std::string::npos);
    EXPECT_NE(notifyResizeBody.find("DriverUIAccess::NotifyUiOverlaySwapchainWillResize"), std::string::npos);

    const auto releaseTextureBody = ExtractFunctionBody(source, "void UIManager::ReleaseTextureViewHandle(");
    EXPECT_NE(releaseTextureBody.find("ShouldPublishUiSnapshotToFrameGraph()"), std::string::npos);
    EXPECT_NE(releaseTextureBody.find("DriverUIAccess::ReleaseUiTextureView"), std::string::npos);

    const auto rebuildFontsBody = ExtractFunctionBody(source, "void UIManager::RebuildFonts()");
    EXPECT_NE(rebuildFontsBody.find("NotifyFontAtlasChanged()"), std::string::npos);

    const auto notifyFontsBody = ExtractFunctionBody(source, "void UIManager::NotifyFontAtlasChanged()");
    EXPECT_NE(notifyFontsBody.find("ShouldPublishUiSnapshotToFrameGraph()"), std::string::npos);
    EXPECT_NE(notifyFontsBody.find("DriverUIAccess::NotifyUiOverlayFontAtlasChanged"), std::string::npos);

    const auto loadFontBody = ExtractFunctionBody(source, "bool UIManager::LoadFont(");
    EXPECT_NE(loadFontBody.find("NotifyFontAtlasChanged()"), std::string::npos);

    const auto unloadFontBody = ExtractFunctionBody(source, "bool UIManager::UnloadFont(");
    EXPECT_NE(unloadFontBody.find("RebuildFonts()"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, DriverUIAccessExposesUiOverlayResourceLifecycleHooks)
{
    const auto header = ReadSourceText(RepoPath("Runtime/Rendering/Context/DriverAccess.h"));
    EXPECT_NE(header.find("NotifyUiOverlayFontAtlasChanged"), std::string::npos);
    EXPECT_NE(header.find("NotifyUiOverlaySwapchainWillResize"), std::string::npos);

    const auto driverSource = ReadSourceText(RepoPath("Runtime/Rendering/Context/Driver.cpp"));
    const auto fontBody = ExtractFunctionBody(
        driverSource,
        "void DriverUIAccess::NotifyUiOverlayFontAtlasChanged(");
    EXPECT_NE(fontBody.find("uiOverlayRenderer"), std::string::npos);
    EXPECT_NE(fontBody.find("InvalidateFontAtlas"), std::string::npos);

    const auto resizeBody = ExtractFunctionBody(
        driverSource,
        "void DriverUIAccess::NotifyUiOverlaySwapchainWillResize(");
    EXPECT_EQ(resizeBody.find("ReleaseRetiredResources"), std::string::npos);
    EXPECT_EQ(resizeBody.find("ReleaseRetiredTextureViews"), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, AssetBrowserProjectCopyRegeneratesMetaIdentity)
{
    const auto assetBrowserSource = ReadSourceText(RepoPath("Project/Editor/Panels/AssetBrowser.cpp"));
    const auto copyFileBody = ExtractFunctionBody(assetBrowserSource, "bool CopyAssetFileWithMeta(");
    const auto copyFolderBody = ExtractFunctionBody(assetBrowserSource, "bool CopyAssetFolderRecursively(");

    EXPECT_NE(copyFileBody.find("AssetId::New()"), std::string::npos);
    EXPECT_NE(copyFileBody.find("meta.Save(destinationMeta)"), std::string::npos);
    EXPECT_EQ(copyFileBody.find("std::filesystem::copy_file(sourceMeta"), std::string::npos);
    EXPECT_NE(copyFolderBody.find("CopyAssetFileWithMeta(entry.path(), destination / relative)"), std::string::npos);
    EXPECT_EQ(copyFolderBody.find("std::filesystem::copy("), std::string::npos);
}

TEST(RHIUiOverlaySourceGuardTests, ToolbarUsesLegacyImageButtonsAndRawTextureIds)
{
    const auto toolbarSource = ReadSourceText(RepoPath("Project/Editor/Panels/Toolbar.cpp"));
    const auto toolbarHeader = ReadSourceText(RepoPath("Project/Editor/Panels/Toolbar.h"));
    const auto editorResourcesSource = ReadSourceText(RepoPath("Project/Editor/Core/EditorResources.cpp"));

    EXPECT_NE(toolbarHeader.find("ButtonImage.h"), std::string::npos);
    EXPECT_NE(toolbarHeader.find("ButtonImage*"), std::string::npos);
    EXPECT_NE(toolbarSource.find("makeToolbarTextureView"), std::string::npos);
    EXPECT_NE(toolbarSource.find("Button_Play"), std::string::npos);
    EXPECT_NE(toolbarSource.find("Button_Pause"), std::string::npos);
    EXPECT_NE(toolbarSource.find("Button_Stop"), std::string::npos);
    EXPECT_NE(toolbarSource.find("Button_Next"), std::string::npos);
    EXPECT_NE(toolbarSource.find("Button_Refresh"), std::string::npos);
    EXPECT_NE(toolbarSource.find("Maths::Vector2{ 20, 20 }"), std::string::npos);

    EXPECT_NE(editorResourcesSource.find("BUTTON_PLAY"), std::string::npos);
    EXPECT_NE(editorResourcesSource.find("BUTTON_PAUSE"), std::string::npos);
    EXPECT_NE(editorResourcesSource.find("BUTTON_STOP"), std::string::npos);
    EXPECT_NE(editorResourcesSource.find("BUTTON_NEXT"), std::string::npos);
    EXPECT_NE(editorResourcesSource.find("BUTTON_REFRESH"), std::string::npos);

    ExpectNoNeedles(
        toolbarSource,
        {
            "ICON_FA_PLAY",
            "ICON_FA_PAUSE",
            "ICON_FA_STOP",
            "ICON_FA_STEP_FORWARD",
            "ICON_FA_REFRESH",
            "EnsureFontAwesomeIconFontLoaded",
        },
        "Project/Editor/Panels/Toolbar.cpp");
}

TEST(RHIUiOverlaySourceGuardTests, EditorCursorImagesUseCurrentEditorCursorResourceDirectory)
{
    const auto deviceSource = ReadSourceText(RepoPath("Runtime/Platform/Windowing/Context/Device.cpp"));

    EXPECT_NE(deviceSource.find("\"Editor\" / \"Cursors\" / \"windows\""), std::string::npos);
    EXPECT_NE(deviceSource.find("\"FPSView.png\""), std::string::npos);
    EXPECT_NE(deviceSource.find("\"PanView.png\""), std::string::npos);
    EXPECT_NE(deviceSource.find("\"OrbitView.png\""), std::string::npos);
    EXPECT_NE(deviceSource.find("\"SlideArrow.png\""), std::string::npos);
    EXPECT_EQ(deviceSource.find("\"Icon\" / \"cursors\""), std::string::npos);

    const std::vector<std::filesystem::path> cursorFiles = {
        RepoPath("App/Assets/Editor/Cursors/windows/FPSView.png"),
        RepoPath("App/Assets/Editor/Cursors/windows/PanView.png"),
        RepoPath("App/Assets/Editor/Cursors/windows/OrbitView.png"),
        RepoPath("App/Assets/Editor/Cursors/windows/SlideArrow.png"),
    };

    for (const auto& cursorFile : cursorFiles)
        EXPECT_TRUE(std::filesystem::is_regular_file(cursorFile)) << "Missing editor cursor image: " << cursorFile.string();
}
