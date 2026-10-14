#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h"
#include "Rendering/Settings/DriverSettings.h"

#if defined(_WIN32) && NLS_HAS_VULKAN
#include "Windowing/Context/Device.h"
#include "Windowing/Settings/WindowSettings.h"
#include "Windowing/Window.h"
#endif

TEST(VulkanSubmitLaneTests, UploadReadbackAndFenceOnlyRetirementUseTheConfiguredLane)
{
#if defined(_WIN32) && NLS_HAS_VULKAN
    using namespace NLS::Render;
    NLS::Context::Device windowDevice(NLS::Windowing::Settings::DeviceSettings{});
    NLS::Windowing::Settings::WindowSettings windowSettings;
    windowSettings.title = "Vulkan Submit Lane Validation";
    windowSettings.width = 64u;
    windowSettings.height = 64u;
    windowSettings.focused = false;
    windowSettings.clientAPI = NLS::Windowing::Settings::WindowClientAPI::NoAPI;
    NLS::Windowing::Window window(windowDevice, windowSettings);

    for (const auto mode : {Settings::RhiThreadModePreference::ForceNone,
        Settings::RhiThreadModePreference::ForceDedicatedThread, Settings::RhiThreadModePreference::ForceTasks})
    {
        SCOPED_TRACE(static_cast<int>(mode));
        auto& executor = Context::RhiCommandListExecutor::Get();
        executor.ResetForTesting();
        Settings::DriverSettings settings;
        settings.graphicsBackend = Settings::EGraphicsBackend::VULKAN;
        settings.rhiThreadMode = mode;
        settings.platformWindow = window.GetGlfwWindow();
        Context::Driver driver(settings);
        auto device = Context::DriverRendererAccess::GetExplicitDevice(driver);
        if (device == nullptr)
            GTEST_SKIP() << "Vulkan device creation failed in this environment";
        EXPECT_TRUE(device->GetCapabilities().supportsRhiTasksMode);

        const std::array<uint8_t, 4> source{12u, 87u, 201u, 255u};
        RHI::RHITextureDesc desc;
        desc.extent = {1u, 1u, 1u};
        desc.format = RHI::TextureFormat::RGBA8;
        desc.usage = RHI::TextureUsageFlags::Sampled | RHI::TextureUsageFlags::CopySrc | RHI::TextureUsageFlags::CopyDst;
        desc.debugName = "VulkanSubmitLanePixel";
        RHI::RHITextureUploadDesc upload;
        upload.data = source.data();
        upload.dataSize = source.size();
        upload.rowPitch = 4u;
        upload.slicePitch = 4u;
        const auto beforeUpload = executor.GetDispatchedSequence();
        auto texture = device->CreateTexture(desc, upload);
        ASSERT_NE(texture, nullptr);
        EXPECT_GT(executor.GetSubmitSequence(), beforeUpload);

        std::array<uint8_t, 4> pixels{};
        const auto beforeReadback = executor.GetDispatchedSequence();
        const auto readback = device->ReadPixelsChecked(texture, 0u, 0u, 1u, 1u,
            Settings::EPixelDataFormat::RGBA, Settings::EPixelDataType::UNSIGNED_BYTE, pixels.data());
        ASSERT_TRUE(readback.Succeeded()) << readback.message;
        EXPECT_EQ(pixels, source);
        EXPECT_GT(executor.GetSubmitSequence(), beforeReadback);

        auto queue = device->GetQueue(RHI::QueueType::Graphics);
        auto nativeFence = device->CreateFence("VulkanFenceOnlyRetirement");
        ASSERT_NE(queue, nullptr);
        ASSERT_NE(nativeFence, nullptr);
        nativeFence->Reset();
        RHI::RHISubmitDesc fenceOnly;
        fenceOnly.signalFence = nativeFence;
        const auto submitted = queue->SubmitChecked(fenceOnly);
        ASSERT_TRUE(submitted.Succeeded()) << submitted.message;
        EXPECT_TRUE(submitted.frameFenceSignalQueued);
        EXPECT_TRUE(nativeFence->Wait(5'000'000'000ull));

        auto owner = std::make_shared<int>(7);
        std::weak_ptr<int> weakOwner = owner;
        const auto retired = executor.DeferRelease({owner, texture});
        owner.reset();
        texture.reset();
        ASSERT_TRUE(executor.Flush(Context::RhiFlushFlags::Gpu | Context::RhiFlushFlags::ReleaseResources))
            << executor.GetFailureDiagnostic();
        EXPECT_TRUE(retired.Succeeded()) << retired.GetDiagnostic();
        EXPECT_TRUE(weakOwner.expired());
        EXPECT_EQ(executor.GetPendingResourceReleaseCount(), 0u);
        EXPECT_EQ(executor.GetQuarantinedResourceReleaseCount(), 0u);
    }
#else
    GTEST_SKIP() << "The Windows Vulkan backend is not compiled.";
#endif
}

TEST(VulkanSubmitLaneTests, ShaderReadBuffersAndDepthPassTransitionsSatisfyValidation)
{
#if defined(_WIN32) && NLS_HAS_VULKAN && defined(NLS_ENABLE_TEST_HOOKS)
    using namespace NLS::Render;
    const auto validationBefore = Backend::GetVulkanValidationErrorCountForTesting();
    {
        NLS::Context::Device windowDevice(NLS::Windowing::Settings::DeviceSettings{});
        NLS::Windowing::Settings::WindowSettings windowSettings;
        windowSettings.width = 64u;
        windowSettings.height = 64u;
        windowSettings.focused = false;
        windowSettings.clientAPI = NLS::Windowing::Settings::WindowClientAPI::NoAPI;
        NLS::Windowing::Window window(windowDevice, windowSettings);
        for (const auto mode : {Settings::RhiThreadModePreference::ForceNone,
            Settings::RhiThreadModePreference::ForceDedicatedThread, Settings::RhiThreadModePreference::ForceTasks})
        {
            SCOPED_TRACE(static_cast<int>(mode));
            Settings::DriverSettings settings;
            settings.graphicsBackend = Settings::EGraphicsBackend::VULKAN;
            settings.rhiThreadMode = mode;
            settings.platformWindow = window.GetGlfwWindow();
            Context::Driver driver(settings);
            auto device = Context::DriverRendererAccess::GetExplicitDevice(driver);
            if (device == nullptr)
                GTEST_SKIP() << "Vulkan device creation failed in this environment";

            RHI::RHIBufferDesc bufferDesc;
            bufferDesc.size = 64u;
            bufferDesc.usage = RHI::BufferUsageFlags::ShaderRead;
            auto buffer = device->CreateBuffer(bufferDesc);
            ASSERT_NE(buffer, nullptr);
            RHI::RHIBindingLayoutDesc layoutDesc;
            RHI::RHIBindingLayoutEntry layoutEntry;
            layoutEntry.type = RHI::BindingType::StructuredBuffer;
            layoutEntry.elementStride = 4u;
            layoutEntry.stageMask = RHI::ShaderStageMask::AllGraphics;
            layoutDesc.entries.push_back(layoutEntry);
            auto layout = device->CreateBindingLayout(layoutDesc);
            ASSERT_NE(layout, nullptr);
            RHI::RHIBindingSetDesc bindingDesc;
            bindingDesc.layout = layout;
            RHI::RHIBindingSetEntry bindingEntry;
            bindingEntry.type = RHI::BindingType::StructuredBuffer;
            bindingEntry.buffer = buffer;
            bindingEntry.bufferRange = bufferDesc.size;
            bindingEntry.elementStride = 4u;
            bindingDesc.entries.push_back(bindingEntry);
            ASSERT_NE(device->CreateBindingSet(bindingDesc), nullptr);

            RHI::RHITextureDesc depthDesc;
            depthDesc.extent = {64u, 64u, 1u};
            depthDesc.format = RHI::TextureFormat::Depth32F;
            depthDesc.usage = RHI::TextureUsageFlags::DepthStencilAttachment;
            auto depth = device->CreateTexture(depthDesc);
            ASSERT_NE(depth, nullptr);
            RHI::RHITextureViewDesc viewDesc;
            viewDesc.format = depthDesc.format;
            auto depthView = device->CreateTextureView(depth, viewDesc);
            ASSERT_NE(depthView, nullptr);
            auto pool = device->CreateCommandPool(RHI::QueueType::Graphics, "DepthLayoutValidation");
            ASSERT_NE(pool, nullptr);
            RHI::RHIRenderPassDesc pass;
            pass.renderArea = {0, 0, 64u, 64u};
            pass.attachmentsRequireExternalStateTransitions = true;
            RHI::RHIRenderPassDepthStencilAttachmentDesc attachment;
            attachment.view = depthView;
            attachment.depthLoadOp = RHI::LoadOp::Clear;
            attachment.depthStoreOp = RHI::StoreOp::Store;
            attachment.stencilLoadOp = RHI::LoadOp::DontCare;
            attachment.stencilStoreOp = RHI::StoreOp::DontCare;
            pass.depthStencilAttachment = attachment;
            auto fence = device->CreateFence("DepthLayoutValidationComplete");
            ASSERT_NE(fence, nullptr);
            auto precedingState = RHI::ResourceState::Unknown;
            for (uint32_t passIndex = 0u; passIndex < 3u; ++passIndex)
            {
                auto commands = pool->CreateCommandBuffer("DepthWriteReadWrite");
                ASSERT_NE(commands, nullptr);
                commands->Begin();
                RHI::RHIBarrierDesc barrier;
                barrier.statesAreExplicit = true;
                RHI::RHITextureBarrier transition;
                transition.texture = depth;
                transition.before = precedingState;
                transition.after = passIndex == 0u ? RHI::ResourceState::DepthWrite : precedingState;
                barrier.textureBarriers.push_back(transition);
                commands->Barrier(barrier);
                // Keep the preceding layout distinct from the subpass layout so
                // validation checks both implicit read/write transitions.
                pass.depthStencilAttachment->readOnlyDepthStencil = passIndex == 1u;
                pass.depthStencilAttachment->depthLoadOp = passIndex == 0u ? RHI::LoadOp::Clear : RHI::LoadOp::Load;
                commands->BeginRenderPass(pass);
                commands->EndRenderPass();
                commands->End();
                fence->Reset();
                RHI::RHISubmitDesc submission;
                submission.commandBuffers.push_back(commands);
                submission.signalFence = fence;
                const auto submitted = device->GetQueue(RHI::QueueType::Graphics)->SubmitChecked(submission);
                ASSERT_TRUE(submitted.Succeeded()) << submitted.message;
                ASSERT_TRUE(fence->Wait(5'000'000'000ull));
                precedingState = passIndex == 1u ? RHI::ResourceState::DepthRead : RHI::ResourceState::DepthWrite;
            }
        }
    }
    EXPECT_EQ(Backend::GetVulkanValidationErrorCountForTesting(), validationBefore);
#else
    GTEST_SKIP() << "The Windows Vulkan backend with test hooks is not compiled.";
#endif
}
