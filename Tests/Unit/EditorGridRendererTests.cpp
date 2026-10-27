#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <GLFW/glfw3.h>
#include <Guid.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>
#include "Rendering/EditorGridRenderer.h"
#include "VFX/Editor/VfxPreviewCamera.h"
#include "VFX/Editor/VfxSession.h"
#include "VFX/Editor/VfxSystemPreview.h"
#include <Rendering/Context/RenderCommandPipe.h>

namespace
{
void ValidateSharedGrid(bool vulkan,bool axes = false)
{
    using namespace NLS;
    namespace rhi = Render::RHI; namespace sc = Render::ShaderCompiler;
    auto& executor = Render::Context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(Render::Context::RhiThreadMode::None,{});
    struct Shutdown { Render::Context::RhiCommandListExecutor& value; ~Shutdown() { value.Shutdown(); } } shutdown{executor};
    struct Window { GLFWwindow* value = nullptr; ~Window() { if (value) { glfwDestroyWindow(value); glfwTerminate(); } } } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "Vulkan WSI unavailable";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.value = glfwCreateWindow(96,96,"Shared grid validation",nullptr,nullptr);
        if (!window.value) { glfwTerminate(); GTEST_SKIP() << "Vulkan WSI unavailable"; }
    }
    Render::Settings::DriverSettings settings; settings.debugMode = true; settings.platformWindow = window.value;
    settings.graphicsBackend = vulkan ? Render::Settings::EGraphicsBackend::VULKAN : Render::Settings::EGraphicsBackend::DX12;
    auto device = Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested GPU unavailable";
    executor.SetTargets(Render::Context::RhiCommandListExecutor::CreateDeviceTargets(device));
    const auto validation = Render::Backend::GetVulkanValidationErrorCountForTesting();
    const auto root = std::filesystem::temp_directory_path()/("NullusEditorGrid_"+Guid::New().ToString());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};
    auto shaders = std::make_shared<Editor::Rendering::EditorGridShaders>();
    sc::ShaderCompiler compiler;
    for (bool pixel : {false,true})
    {
        sc::ShaderCompilationInput input;
        input.assetPath = axes ? "App/Assets/Engine/Shaders/DebugPrimitive.hlsl" : "App/Assets/Editor/Shaders/Grid.hlsl";
        input.stage = pixel ? sc::ShaderStage::Pixel : sc::ShaderStage::Vertex;
        input.options.targetPlatform = vulkan ? sc::ShaderTargetPlatform::SPIRV : sc::ShaderTargetPlatform::DXIL;
        input.options.entryPoint = pixel ? "PSMain" : "VSMain"; input.options.targetProfile = pixel ? "ps_6_0" : "vs_6_0";
        input.options.artifactDirectory = root.string();
        const auto binary = compiler.Compile(input);
        ASSERT_EQ(binary.status,sc::ShaderCompilationStatus::Succeeded) << binary.diagnostics;
        auto& module = pixel ? shaders->fragment : shaders->vertex;
        module.stage = pixel ? rhi::ShaderStage::Fragment : rhi::ShaderStage::Vertex;
        module.targetBackend = device->GetAdapter()->GetBackendType(); module.entryPoint = input.options.entryPoint; module.bytecode = binary.bytecode;
    }
    rhi::RHITextureDesc colorDesc; colorDesc.extent = {96,96,1}; colorDesc.format = rhi::TextureFormat::RGBA8;
    colorDesc.usage = rhi::TextureUsageFlags::ColorAttachment|rhi::TextureUsageFlags::CopySrc;
    auto color = device->CreateTexture(colorDesc); rhi::RHITextureViewDesc viewDesc; viewDesc.format = colorDesc.format;
    auto colorView = device->CreateTextureView(color,viewDesc);
    auto depthDesc = colorDesc; depthDesc.format = rhi::TextureFormat::Depth32F; depthDesc.usage = rhi::TextureUsageFlags::DepthStencilAttachment;
    auto depth = device->CreateTexture(depthDesc); viewDesc.format = depthDesc.format;
    auto depthView = device->CreateTextureView(depth,viewDesc); ASSERT_TRUE(colorView && depthView);
    Editor::VFXTools::VfxPreviewCamera camera; camera.Camera().SetPosition({3,5,10});
    camera.Camera().SetRotation(Maths::Quaternion::LookAt(Maths::Vector3::Normalize({-3,-5,-10}),Maths::Vector3::Up));
    const auto cameraView = camera.MakeView(96,96); Maths::Matrix4 matrix;
    std::copy(cameraView.viewProjection.begin(),cameraView.viewProjection.end(),std::begin(matrix.data));
    rhi::RHIRenderTargetLayoutDesc targets; targets.colorFormats = {colorDesc.format}; targets.hasDepth = true; targets.depthFormat = depthDesc.format;
    Editor::Rendering::EditorGridRenderer renderer; std::string error;
    std::vector<Render::Context::RecordedDrawCommandInput> draws;
    if (axes)
    {
        auto prepared = renderer.PrepareAxes(device,shaders,targets,matrix,camera.Camera().GetPosition(),error);
        ASSERT_TRUE(prepared) << error;
        for (auto& draw : *prepared) draws.push_back(std::move(draw));
    }
    else
    {
        auto prepared = renderer.Prepare(device,shaders,targets,matrix,camera.Camera().GetPosition(),{0.5f,0.5f,0.5f},error);
        ASSERT_TRUE(prepared) << error; draws.push_back(std::move(*prepared));
    }
    for (const auto& draw : draws)
    {
        ASSERT_TRUE(draw.resourceOwner);
        EXPECT_FALSE(draw.pipeline->GetDesc().depthStencilState.depthWrite);
        EXPECT_TRUE(draw.pipeline->GetDesc().depthStencilState.depthTest);
        EXPECT_EQ(draw.pipeline->GetDesc().primitiveTopology,axes ? rhi::PrimitiveTopology::LineList : rhi::PrimitiveTopology::TriangleList);
    }
    const auto render = [&](float clearDepth)
    {
        std::vector<uint8_t> pixels(96*96*4);
        auto pool = device->CreateCommandPool(rhi::QueueType::Graphics,"EditorGridTest");
        auto commands = pool->CreateCommandBuffer("EditorGridTest"); commands->Begin();
        rhi::RHIRenderPassDesc pass; pass.renderArea = {0,0,96,96};
        rhi::RHIRenderPassColorAttachmentDesc attachment; attachment.view = colorView; attachment.clearValue = {0,0,0,1}; pass.colorAttachments = {attachment};
        rhi::RHIRenderPassDepthStencilAttachmentDesc depthAttachment; depthAttachment.view = depthView; depthAttachment.clearValue.depth = clearDepth; pass.depthStencilAttachment = depthAttachment;
        for (const auto& draw : draws)
        {
            Editor::Rendering::EditorGridRenderer::Record(*commands,draw,pass);
            pass.colorAttachments[0].loadOp = rhi::LoadOp::Load;
            pass.depthStencilAttachment->depthLoadOp = rhi::LoadOp::Load;
        }
        commands->End();
        auto fence = device->CreateFence("EditorGridTest"); fence->Reset(); rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        const auto accepted = device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit);
        EXPECT_TRUE(accepted.Succeeded()) << accepted.message;
        if (!accepted.Succeeded()) return pixels;
        EXPECT_TRUE(fence->Wait(30000000000ull));
        auto read = device->BeginReadPixels(color,0,0,96,96,Render::Settings::EPixelDataFormat::RGBA,Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
        EXPECT_TRUE(read.Succeeded()) << read.message;
        if (read.completion) EXPECT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        return pixels;
    };
    const auto visible = render(1); size_t visiblePixels = 0;
    for (size_t index = 0; index < visible.size(); index += 4) visiblePixels += visible[index] || visible[index+1] || visible[index+2];
    EXPECT_GT(visiblePixels,axes ? 20u : 100u) << "Scene helpers must draw visible lines in the preview camera";
    if (axes)
    {
        std::array<size_t,3> colors{};
        for (size_t pixel = 0; pixel < visible.size(); pixel += 4)
            for (size_t axis = 0; axis < colors.size(); ++axis)
                colors[axis] += visible[pixel+axis] > 200 && visible[pixel+(axis+1)%3] < 10 && visible[pixel+(axis+2)%3] < 10;
        for (size_t axis = 0; axis < colors.size(); ++axis) EXPECT_GT(colors[axis],5u) << "Missing axis " << axis;
    }
    const auto occluded = render(0); size_t occludedPixels = 0;
    for (size_t index = 0; index < occluded.size(); index += 4) occludedPixels += occluded[index] || occluded[index+1] || occluded[index+2];
    EXPECT_EQ(occludedPixels,0u) << "Grid lines must respect scene depth";
    // Exercise the same draw through the whole-system preview, including a
    // paused redraw when only the grid visibility changes.
    Editor::VFXTools::VfxSession session; ASSERT_TRUE(session.Rebuild()); session.playing = false;
    Editor::VFXTools::VfxSystemPreview preview;
    struct Drain { Editor::VFXTools::VfxSystemPreview& preview; ~Drain() { preview.Clear(); Render::Context::RenderCommandDispatcher::FlushToDrain(); } } drain{preview};
    Editor::VFXTools::VfxSystemPreviewInput input; input.width = 96; input.height = 96;
    input.camera = cameraView; input.gridEye = camera.Camera().GetPosition();
    input.gridColor = {0.5f,0.5f,0.5f}; input.background = {0,0,0,1};
    input.gridShaders = axes ? nullptr : shaders; input.showGrid = !axes;
    input.axesShaders = axes ? shaders : nullptr; input.showOriginAxis = axes;
    preview.Update(device,session.Program(),session.TakeFrames(),root,input);
    std::shared_ptr<rhi::RHITextureView> shown;
    const auto waitForView = [&](std::shared_ptr<rhi::RHITextureView> previous)
    {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(30);
        std::shared_ptr<rhi::RHITextureView> result;
        while (std::chrono::steady_clock::now() < deadline)
        {
            preview.Update(device,session.Program(),{},root,input); Render::Context::RenderCommandDispatcher::Dispatch();
            result = preview.View(); if (result && result != previous) return result;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::shared_ptr<rhi::RHITextureView>{};
    };
    shown = waitForView({}); ASSERT_TRUE(shown) << preview.Status();
    const auto readView = [&](const std::shared_ptr<rhi::RHITextureView>& view)
    {
        std::vector<uint8_t> pixels(96*96*4);
        auto read = device->BeginReadPixels(view->GetTexture(),0,0,96,96,Render::Settings::EPixelDataFormat::RGBA,Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
        EXPECT_TRUE(read.Succeeded()); if (read.completion) EXPECT_TRUE(read.completion->Wait(30000000000ull).Succeeded()); return pixels;
    };
    const auto gridPixels = readView(shown);
    size_t differentBytes = 0, previewLit = 0; int largestDifference = 0;
    for (size_t index = 0; index < gridPixels.size(); ++index)
    {
        differentBytes += gridPixels[index] != visible[index];
        largestDifference = (std::max)(largestDifference,std::abs(static_cast<int>(gridPixels[index])-visible[index]));
        if (index%4 == 0) previewLit += gridPixels[index] || gridPixels[index+1] || gridPixels[index+2];
    }
    if (const char* capture = std::getenv("NLS_VIEWPORT_CAPTURE_DIR"))
    {
        std::filesystem::create_directories(capture);
        for (bool preview : {false,true})
        {
            const auto& pixels = preview ? gridPixels : visible;
            const auto path = std::filesystem::path(capture)/((vulkan ? "vulkan" : "dx12")+std::string(axes ? "-axes" : "")+std::string(preview ? "-preview.ppm" : "-scene.ppm"));
            std::ofstream output(path,std::ios::binary); output << "P6\n96 96\n255\n";
            for (size_t index = 0; index < pixels.size(); index += 4) output.write(reinterpret_cast<const char*>(pixels.data()+index),3);
        }
    }
    EXPECT_EQ(gridPixels,visible) << "different bytes=" << differentBytes << " max difference=" << largestDifference << " preview lit=" << previewLit << " scene lit=" << visiblePixels;
    input.showGrid = false;
    input.showOriginAxis = false;
    const auto hidden = waitForView(shown); ASSERT_TRUE(hidden) << preview.Status();
    const auto hiddenPixels = readView(hidden);
    size_t hiddenCount = 0; for (size_t index = 0; index < hiddenPixels.size(); index += 4) hiddenCount += hiddenPixels[index] || hiddenPixels[index+1] || hiddenPixels[index+2];
    EXPECT_EQ(hiddenCount,0u); EXPECT_EQ(readView(shown),gridPixels);
    if (axes)
    {
        input.showOriginAxis = true;
        auto restored = waitForView(hidden); ASSERT_TRUE(restored) << preview.Status();
        EXPECT_EQ(readView(restored),visible) << "Axes must toggle without the grid or simulation advancing";
        EXPECT_EQ(session.Instance().Age(),0);
        // A later camera publication must neither mutate an earlier draw nor
        // leave a paused preview on the previous camera's axes.
        camera.Camera().SetPosition({12,4,7});
        camera.Camera().SetRotation(Maths::Quaternion::LookAt(Maths::Vector3::Normalize({-12,-4,-7}),Maths::Vector3::Up));
        input.camera = camera.MakeView(96,96); input.gridEye = camera.Camera().GetPosition();
        std::copy(input.camera.viewProjection.begin(),input.camera.viewProjection.end(),std::begin(matrix.data));
        auto moved = renderer.PrepareAxes(device,shaders,targets,matrix,input.gridEye,error); ASSERT_TRUE(moved) << error;
        draws.assign(moved->begin(),moved->end());
        const auto directMoved = render(1);
        const auto previewMoved = waitForView(restored); ASSERT_TRUE(previewMoved) << preview.Status();
        EXPECT_EQ(readView(previewMoved),directMoved); EXPECT_NE(directMoved,visible);
        EXPECT_EQ(readView(shown),visible);
    }
    if (vulkan) EXPECT_EQ(Render::Backend::GetVulkanValidationErrorCountForTesting(),validation);
}
}
TEST(EditorGridRenderer, Dx12SharedSceneGridProducesPixelsAndRespectsDepth) { ValidateSharedGrid(false); }
TEST(EditorGridRenderer, VulkanSharedSceneGridProducesPixelsAndRespectsDepth) { ValidateSharedGrid(true); }
TEST(EditorOriginAxes, Dx12SceneAndPreviewAxesMatchAndRespectDepth) { ValidateSharedGrid(false,true); }
TEST(EditorOriginAxes, VulkanSceneAndPreviewAxesMatchAndRespectDepth) { ValidateSharedGrid(true,true); }
TEST(EditorOriginAxes, SceneAxisGeometryRetainsColorsExtentAndDepthPolicy)
{
    using namespace NLS;
    const Maths::Vector3 eye{2,3,4};
    const auto axes = Editor::Rendering::BuildEditorOriginAxes(eye,10);
    for (size_t index = 0; index < axes.size(); ++index)
    {
        const auto& line = axes[index];
        EXPECT_EQ(line.type,Render::Debug::DebugDrawPrimitiveType::Line);
        EXPECT_EQ(line.options.category,Render::Debug::DebugDrawCategory::Grid);
        EXPECT_EQ(line.options.lifetime.mode,Render::Debug::DebugDrawLifetimeMode::Persistent);
        EXPECT_EQ(line.options.style.depthMode,Render::Debug::DebugDrawDepthMode::DepthTest);
        EXPECT_FLOAT_EQ(line.options.style.lineWidth,1);
        for (size_t component = 0; component < axes.size(); ++component)
        {
            EXPECT_FLOAT_EQ(line.options.style.color[component],component == index ? 1 : 0);
            EXPECT_FLOAT_EQ(line.points[0][component],component == index ? eye[component]-10 : 0);
            EXPECT_FLOAT_EQ(line.points[1][component],component == index ? eye[component]+10 : 0);
        }
    }
}
