#include <gtest/gtest.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Engine/Rendering/ForwardSceneRenderer.h>
#include <Engine/SceneSystem/Scene.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/RHI/Core/RHIResource.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Rendering/UI/UIDrawSnapshotAdapter.h>
#include <Engine/Components/UICanvasComponent.h>
#include <ImGui/imgui.h>
#include <array>
#include <filesystem>

TEST(PlayOutputGpuTests, Dx12CameraCompositionClipsClearsAndAddsUiToTheOutput)
{
#if defined(_WIN32)
    namespace R=NLS::Render;namespace E=NLS::Engine;namespace C=R::Context;
    R::Settings::DriverSettings settings;settings.graphicsBackend=R::Settings::EGraphicsBackend::DX12;settings.debugMode=true;
    C::Driver driver(settings);const auto device=C::DriverRendererAccess::GetExplicitDevice(driver);
    ASSERT_TRUE(device&&device->IsBackendReady())<<"DX12 device is required for Play acceptance.";
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Core::ResourceManagement::ShaderManager shaders;NLS::Core::ServiceLocator::Provide(shaders);
    const auto oldProject=shaders.ProjectAssetsRoot(),oldEngine=shaders.EngineAssetsRoot();
    shaders.ProvideAssetPaths("",(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine").string()+"/");
    struct Services{std::string project,engine;~Services(){NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(project,engine);NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();NLS::Core::ServiceLocator::Remove<C::Driver>();}} services{oldProject,oldEngine};
    IMGUI_CHECKVERSION();auto* context=ImGui::CreateContext();ImGui::GetIO().Fonts->AddFontDefault();
    struct ImGuiCleanup{ImGuiContext* context;~ImGuiCleanup(){ImGui::DestroyContext(context);}} imgui{context};
    E::SceneSystem::Scene scene;R::Buffers::Framebuffer output;output.Resize(3840,2160);
    E::Rendering::ForwardSceneRenderer background(driver),overlay(driver);
    const auto render=[&](auto& renderer,NLS::Maths::Vector3 color,bool clear,NLS::Maths::Vector4 viewport,std::shared_ptr<const R::UI::UiDrawDataSnapshot> ui={})
    {
        R::Entities::Camera camera;camera.SetClearColor(color);camera.SetClearColorBuffer(clear);camera.SetClearDepthBuffer(true);camera.SetClearStencilBuffer(true);
        camera.CacheMatrices(uint16_t(viewport.z),uint16_t(viewport.w));
        E::Rendering::BaseSceneRenderer::SceneDescriptor sceneDescriptor{scene,std::nullopt,nullptr};sceneDescriptor.includeSkyboxes=false;
        renderer.template AddDescriptor<E::Rendering::BaseSceneRenderer::SceneDescriptor>(std::move(sceneDescriptor));
        R::Data::FrameDescriptor frame;frame.camera=&camera;frame.renderWidth=uint16_t(viewport.z);frame.renderHeight=uint16_t(viewport.w);
        frame.outputViewport=viewport;frame.playSessionId=50;frame.simulationFrame=7;frame.gameTime=2;frame.gameUiSnapshot=std::move(ui);
        R::FrameGraph::SetExternalSceneOutputFramebuffer(frame,&output);
        renderer.BeginFrame(frame);renderer.DrawFrame();renderer.EndFrame();
    };
    const auto pixel=[&](int x,int y)
    {
        std::array<uint8_t,4> value{};
        auto read=C::DriverRendererAccess::BeginReadPixels(driver,output.GetExplicitTextureHandle(),x,y,1,1,R::Settings::EPixelDataFormat::RGBA,R::Settings::EPixelDataType::UNSIGNED_BYTE,value.data());
        EXPECT_TRUE(read.Succeeded())<<read.message;if(read.completion)EXPECT_TRUE(read.completion->Wait(30000000000ull).Succeeded());return value;
    };
    render(background,{0,0,1},true,{0,0,3840,2160});
    ASSERT_TRUE(C::DriverRendererAccess::TryDrainThreadedRendering(driver));
    EXPECT_GT(pixel(500,1000)[2],245)<<"First camera did not clear the output";
    render(overlay,{1,0,0},true,{1920,0,1920,2160});
    ASSERT_TRUE(C::DriverRendererAccess::TryDrainThreadedRendering(driver));
    const auto left=pixel(500,1000),right=pixel(3000,1000);
    EXPECT_GT(left[2],245);EXPECT_LT(left[0],10);EXPECT_GT(right[0],245);EXPECT_LT(right[2],10);
    render(overlay,{0,1,0},false,{1920,0,1920,2160});
    ASSERT_TRUE(C::DriverRendererAccess::TryDrainThreadedRendering(driver));EXPECT_EQ(pixel(3000,1000),right);
    E::Components::UICanvasComponent canvas;
    ASSERT_TRUE(canvas.LoadDocumentJson(R"json({"version":1,"root":{"type":"Panel","children":[{"type":"Panel","layout":{"size":[200,200]},"style":{"backgroundColor":[0,1,0,1]}}]}})json"));
    canvas.Tick(0,{3840,2160},1,7);
    const auto ui=R::UI::AdaptUIDrawSnapshot(canvas.BuildDrawSnapshot(7,3840,2160,1));
    ASSERT_TRUE(ui);ASSERT_TRUE(ui->hasVisibleDraws);ASSERT_GT(ui->totalVertexCount,0u);
    render(overlay,{0,0,0},false,{1920,0,1920,2160},ui);
    ASSERT_TRUE(C::DriverRendererAccess::TryDrainThreadedRendering(driver));
    const auto panel=pixel(100,100);EXPECT_GT(panel[1],245)
        <<"UI vertex count="<<ui->totalVertexCount<<" first="<<ui->drawLists.front().vertices.front().position[0]<<","<<ui->drawLists.front().vertices.front().position[1]
        <<" clip="<<ui->drawLists.front().commands.front().clipRect[0]<<","<<ui->drawLists.front().commands.front().clipRect[1]<<","<<ui->drawLists.front().commands.front().clipRect[2]<<","<<ui->drawLists.front().commands.front().clipRect[3];
    EXPECT_LT(panel[2],10);EXPECT_EQ(pixel(3000,1000),right);
#else
    GTEST_SKIP()<<"Windows DX12 acceptance test.";
#endif
}
