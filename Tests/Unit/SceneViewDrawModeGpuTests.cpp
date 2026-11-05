#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <Guid.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/Debug/SceneDisplayDebug.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/Settings/DriverSettings.h>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>
#include <Rendering/ShaderLab/ShaderLabParser.h>
#include <MaterialCompiler/MaterialShaderCompiler.h>
#include <MaterialCompiler/MaterialGraph.h>
#include <Core/ServiceLocator.h>
#include <Core/ResourceManagement/MeshManager.h>
#include <Core/ResourceManagement/ShaderManager.h>
#include <Engine/Rendering/DeferredSceneRenderer.h>
#include <Engine/Components/SkyBoxComponent.h>
#include <Engine/Components/MeshFilter.h>
#include <Engine/Components/MeshRenderer.h>
#include <Engine/SceneSystem/Scene.h>
#include <Rendering/Context/Driver.h>
#include <Rendering/Context/DriverAccess.h>

namespace
{
namespace R = NLS::Render;
namespace G = R::RHI;
using Vec = std::array<float,4>;
struct SceneDebugTestSurface { Vec color{0.18f,0.18f,0.18f,1}; Vec uv{1,1,32,32}; Vec arguments{0,0,0,0}; };
class SceneViewDrawModeGpuTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& executor = R::Context::RhiCommandListExecutor::Get();
        if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
        executor.Initialize(R::Context::RhiThreadMode::None,{}); ownsExecutor = true;
        R::Settings::DriverSettings settings; settings.graphicsBackend = R::Settings::EGraphicsBackend::DX12;
        settings.debugMode = true; device = R::Backend::CreateRhiDevice(settings);
        if (!device || !device->IsBackendReady()) GTEST_SKIP() << "DX12 unavailable";
        executor.SetTargets(R::Context::RhiCommandListExecutor::CreateDeviceTargets(device));
        root = std::filesystem::temp_directory_path() / ("NullusSceneDebug_" + NLS::Guid::New().ToString());
        std::filesystem::create_directories(root);
        const auto shaderRoot = (std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders").generic_string();
        std::ofstream(root / "Surface.hlsl") << "#include \"" << shaderRoot << R"(/NullusShaderLibrary/SceneDebug.hlsl"
cbuffer TestSurface : register(b0,space2) { float4 TestColor; float4 TestUv; float4 TestArguments; };
struct Output {float4 position:SV_Position;float2 uv:TEXCOORD0;};
Output VSMain(uint id:SV_VertexID) { Output o; o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o; }
float4 PSMain(Output input):SV_Target0 {
if(TestArguments.z>0.5)clip(TestColor.a-TestArguments.y);
return NLSSceneDebug(TestColor.rgb,TestArguments.x,TestColor.a,NLSSceneTextureLod(input.uv*TestUv.xy,TestUv.zw));}
)";
        std::ofstream(root / "Resolve.hlsl") << "#include \"" << shaderRoot << R"(/SceneDebugResolve.hlsl"
Output FullScreenVS(uint id:SV_VertexID) {Output o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
)";
        surfaceVertex = Compile(root / "Surface.hlsl", false, "VSMain");
        surfacePixel = Compile(root / "Surface.hlsl", true, "PSMain");
        resolveVertex = Compile(root / "Resolve.hlsl", false, "FullScreenVS");
        resolvePixel = Compile(root / "Resolve.hlsl", true, "PSMain");
        ASSERT_TRUE(surfaceVertex && surfacePixel && resolveVertex && resolvePixel);
    }
    void TearDown() override
    {
        surfaceVertex.reset(); surfacePixel.reset(); resolveVertex.reset(); resolvePixel.reset();
        if (ownsExecutor) R::Context::RhiCommandListExecutor::Get().Shutdown();
        device.reset();
        std::error_code error; if (!root.empty()) std::filesystem::remove_all(root,error);
    }
    std::shared_ptr<G::RHIShaderModule> Compile(const std::filesystem::path& path, bool pixel, const char* entry)
    {
        R::ShaderCompiler::ShaderCompilationInput input; input.assetPath = path.string();
        input.stage = pixel ? R::ShaderCompiler::ShaderStage::Pixel : R::ShaderCompiler::ShaderStage::Vertex;
        input.options.targetPlatform = R::ShaderCompiler::ShaderTargetPlatform::DXIL;
        input.options.entryPoint = entry; input.options.targetProfile = pixel ? "ps_6_0" : "vs_6_0";
        input.options.artifactDirectory = (root / "Artifacts").string();
        const auto result = R::ShaderCompiler::ShaderCompiler{}.Compile(input);
        EXPECT_EQ(result.status,R::ShaderCompiler::ShaderCompilationStatus::Succeeded) << result.diagnostics;
        if (result.status != R::ShaderCompiler::ShaderCompilationStatus::Succeeded) return {};
        G::RHIShaderModuleDesc desc; desc.stage = pixel ? G::ShaderStage::Fragment : G::ShaderStage::Vertex;
        desc.targetBackend = device->GetAdapter()->GetBackendType(); desc.bytecode = result.bytecode; desc.entryPoint = entry;
        return device->CreateShaderModule(desc);
    }
    template<class T> std::shared_ptr<G::RHIBuffer> Constants(const T& value)
    {
        G::RHIBufferDesc desc; desc.size = sizeof(T); desc.usage = G::BufferUsageFlags::Uniform; desc.memoryUsage = G::MemoryUsage::CPUToGPU;
        G::RHIBufferUploadDesc upload; upload.data = &value; upload.dataSize = sizeof(T); return device->CreateBuffer(desc,upload);
    }
    using Surface = SceneDebugTestSurface;
    std::array<uint8_t,4> Render(int mode, Surface surface = {}, int layers = 1, bool forward = false, bool pbr = true, bool texture = true, int size = 32, bool supported = true)
    {
        constexpr auto set = G::BindingPointMap::kMaterialDescriptorSet;
        G::RHITextureDesc textureDesc; textureDesc.extent = {uint32_t(size),uint32_t(size),1}; textureDesc.format = G::TextureFormat::RGBA16F;
        textureDesc.usage = G::TextureUsageFlags::ColorAttachment | G::TextureUsageFlags::Sampled;
        auto accumulator = device->CreateTexture(textureDesc); G::RHITextureViewDesc viewDesc; viewDesc.format = textureDesc.format;
        auto accumulatorView = device->CreateTextureView(accumulator,viewDesc);
        textureDesc.format = G::TextureFormat::RGBA8; textureDesc.usage = G::TextureUsageFlags::ColorAttachment | G::TextureUsageFlags::CopySrc;
        auto output = device->CreateTexture(textureDesc); viewDesc.format = textureDesc.format;
        auto outputView = device->CreateTextureView(output,viewDesc);
        G::RHIBindingLayoutDesc layoutDesc;
        layoutDesc.entries = {{"TestSurface",G::BindingType::UniformBuffer,set,0,1,G::ShaderStageMask::Fragment,2,0},
            {"SceneDebugConstants",G::BindingType::UniformBuffer,set,1,1,G::ShaderStageMask::Fragment,2,0}};
        auto layout = device->CreateBindingLayout(layoutDesc);
        struct Debug {Vec options,albedo,specular,wire;};
        auto values = Constants(surface);
        auto debug = Constants(Debug{{float(mode),float(forward ? 3 : 0),supported ? (pbr ? 1.0f : 0.0f) : -1.0f,texture ? 1.0f : 0.0f},
            {50,243,186,255},{40,75,155,255},{0.8f,0.8f,0.8f,1}});
        G::RHIBindingSetDesc bindingsDesc; bindingsDesc.layout = layout;
        G::RHIBindingSetEntry base; base.type = G::BindingType::UniformBuffer; base.binding = 0; base.buffer = values; base.bufferRange = sizeof(Surface);
        auto parameters = base; parameters.binding = 1; parameters.buffer = debug; parameters.bufferRange = sizeof(Debug);
        bindingsDesc.entries = {base,parameters}; auto bindings = device->CreateBindingSet(bindingsDesc);
        G::RHIPipelineLayoutDesc pipelineLayoutDesc; pipelineLayoutDesc.bindingLayouts.resize(set+1); pipelineLayoutDesc.bindingLayouts[set] = layout;
        G::RHIGraphicsPipelineDesc pipelineDesc; pipelineDesc.pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
        pipelineDesc.vertexShader = surfaceVertex; pipelineDesc.fragmentShader = surfacePixel; pipelineDesc.renderTargetLayout.colorFormats = {G::TextureFormat::RGBA16F};
        pipelineDesc.rasterState.cullEnabled = false; pipelineDesc.depthStencilState.depthTest = false; pipelineDesc.depthStencilState.depthWrite = false;
        G::RHIRenderTargetBlendStateDesc blend; blend.blendEnable = mode == 6 || mode == 7;
        if (mode == 7) blend.srcColor = blend.dstColor = blend.srcAlpha = blend.dstAlpha = G::RHIBlendFactor::One;
        pipelineDesc.blendState.enabled = blend.blendEnable; pipelineDesc.blendState.renderTargets = {blend};
        auto pipeline = device->CreateGraphicsPipeline(pipelineDesc);
        layoutDesc.entries = {{"MaterialConstants",G::BindingType::UniformBuffer,set,0,1,G::ShaderStageMask::Fragment,2,0},
            {"u_SceneDebugTexture",G::BindingType::Texture,set,0,1,G::ShaderStageMask::Fragment,2,0},
            {"u_SceneDebugSampler",G::BindingType::Sampler,set,0,1,G::ShaderStageMask::Fragment,2,0}};
        auto resolveLayout = device->CreateBindingLayout(layoutDesc); pipelineLayoutDesc.bindingLayouts[set] = resolveLayout;
        auto resolveDesc = pipelineDesc; resolveDesc.pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
        resolveDesc.vertexShader = resolveVertex; resolveDesc.fragmentShader = resolvePixel; resolveDesc.renderTargetLayout.colorFormats = {G::TextureFormat::RGBA8};
        resolveDesc.blendState.enabled = false; resolveDesc.blendState.renderTargets = {G::RHIRenderTargetBlendStateDesc{}};
        auto resolvePipeline = device->CreateGraphicsPipeline(resolveDesc);
        struct Resolve {int mode; float max; float padding[2];}; auto resolveConstants = Constants(Resolve{mode,16,{0,0}});
        bindingsDesc.layout = resolveLayout; base.buffer = resolveConstants; base.bufferRange = sizeof(Resolve);
        G::RHIBindingSetEntry sampled; sampled.binding = 0; sampled.type = G::BindingType::Texture; sampled.textureView = accumulatorView;
        G::RHIBindingSetEntry sampler; sampler.binding = 0; sampler.type = G::BindingType::Sampler; sampler.sampler = device->CreateSampler({});
        bindingsDesc.entries = {base,sampled,sampler}; auto resolveBindings = device->CreateBindingSet(bindingsDesc);
        EXPECT_TRUE(accumulatorView && outputView && pipeline && resolvePipeline && bindings && resolveBindings);
        if (!accumulatorView || !outputView || !pipeline || !resolvePipeline || !bindings || !resolveBindings) return {};
        auto pool = device->CreateCommandPool(G::QueueType::Graphics,"SceneDebugGpuTest"); auto commands = pool->CreateCommandBuffer("SceneDebugGpuTest");
        commands->Begin();
        auto barrier = [&](const auto& view, G::ResourceState state, G::PipelineStageMask stage, G::AccessMask access)
        {
            G::RHIBarrierDesc desc; desc.textureBarriers.push_back({view->GetTexture(),G::ResourceState::Unknown,state,view->GetDesc().subresourceRange,
                G::PipelineStageMask::AllCommands,stage,G::AccessMask::MemoryRead|G::AccessMask::MemoryWrite,access}); EXPECT_TRUE(commands->BarrierChecked(desc).Succeeded());
        };
        barrier(accumulatorView,G::ResourceState::RenderTarget,G::PipelineStageMask::RenderTarget,G::AccessMask::ColorAttachmentWrite);
        G::RHIRenderPassDesc pass; pass.renderArea = {0,0,uint32_t(size),uint32_t(size)};
        G::RHIRenderPassColorAttachmentDesc attachment; attachment.view = accumulatorView; attachment.clearValue = {0,0,0,0}; pass.colorAttachments = {attachment};
        commands->BeginRenderPass(pass); commands->SetViewport({0,0,float(size),float(size),0,1}); commands->SetScissor(pass.renderArea);
        commands->BindGraphicsPipeline(pipeline); commands->BindBindingSet(set,bindings);
        for (int i = 0; i < layers; ++i) commands->Draw(3,1,0,0);
        commands->EndRenderPass();
        barrier(accumulatorView,G::ResourceState::ShaderRead,G::PipelineStageMask::FragmentShader,G::AccessMask::ShaderRead);
        barrier(outputView,G::ResourceState::RenderTarget,G::PipelineStageMask::RenderTarget,G::AccessMask::ColorAttachmentWrite);
        pass.colorAttachments[0].view = outputView; commands->BeginRenderPass(pass);
        commands->SetViewport({0,0,float(size),float(size),0,1}); commands->SetScissor(pass.renderArea);
        commands->BindGraphicsPipeline(resolvePipeline); commands->BindBindingSet(set,resolveBindings); commands->Draw(3,1,0,0);
        commands->EndRenderPass(); commands->End();
        auto fence = device->CreateFence("SceneDebugGpuTest"); fence->Reset(); G::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        auto accepted = device->GetQueue(G::QueueType::Graphics)->SubmitChecked(submit); EXPECT_TRUE(accepted.Succeeded()) << accepted.message;
        if (!accepted.Succeeded()) return {};
        EXPECT_TRUE(fence->Wait(30000000000ull));
        std::array<uint8_t,4> result{};
        auto read = device->BeginReadPixels(output,size/2,size/2,1,1,R::Settings::EPixelDataFormat::RGBA,R::Settings::EPixelDataType::UNSIGNED_BYTE,result.data());
        EXPECT_TRUE(read.Succeeded()) << read.message; if (read.completion) EXPECT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        return result;
    }
    bool ownsExecutor = false;
    std::filesystem::path root;
    std::shared_ptr<G::RHIDevice> device;
    std::shared_ptr<G::RHIShaderModule> surfaceVertex,surfacePixel,resolveVertex,resolvePixel;
};

TEST_F(SceneViewDrawModeGpuTests, Dx12RenderPathsAndUnsupportedBuffersHaveDistinctColors)
{
    const auto deferred = Render(5), forward = Render(5,{},1,true), unavailable = Render(4);
    EXPECT_GT(deferred[2],240); EXPECT_LT(deferred[0],50);
    EXPECT_GT(forward[0],240); EXPECT_GT(forward[1],200); EXPECT_LT(forward[2],5);
    EXPECT_NEAR(unavailable[0],89,2); EXPECT_EQ(unavailable[0],unavailable[1]); EXPECT_EQ(unavailable[0],unavailable[2]);
}

TEST(SceneViewRendererGpuTests, Dx12SkyboxReturnsAfterWireframe)
{
#if defined(_WIN32)
    namespace R = NLS::Render;
    namespace E = NLS::Engine;
    namespace C = R::Context;
    R::Settings::DriverSettings settings;
    settings.graphicsBackend = R::Settings::EGraphicsBackend::DX12;
    settings.debugMode = true;
    C::Driver driver(settings);
    ASSERT_TRUE(C::DriverRendererAccess::GetExplicitDevice(driver));
    NLS::Core::ServiceLocator::Provide(driver);
    NLS::Core::ResourceManagement::ShaderManager shaders;
    NLS::Core::ResourceManagement::MeshManager meshes;
    NLS::Core::ServiceLocator::Provide(shaders);
    NLS::Core::ServiceLocator::Provide(meshes);
    struct MeshPaths : NLS::Core::ResourceManagement::MeshManager
    {
        static const std::string& EngineRoot() { return GetEngineAssetsPath(); }
    };
    const auto engineAssets = (std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine").string() + "/";
    const auto previousShaderProject = shaders.ProjectAssetsRoot(), previousShaderEngine = shaders.EngineAssetsRoot();
    const auto previousMeshProject = meshes.ProjectAssetsRoot(), previousMeshEngine = MeshPaths::EngineRoot();
    shaders.ProvideAssetPaths("", engineAssets);
    meshes.ProvideAssetPaths("", engineAssets);
    struct Services
    {
        std::string shaderProject, shaderEngine, meshProject, meshEngine;
        ~Services()
        {
            NLS::Core::ResourceManagement::ShaderManager::ProvideAssetPaths(shaderProject, shaderEngine);
            NLS::Core::ResourceManagement::MeshManager::ProvideAssetPaths(meshProject, meshEngine);
            NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MeshManager>();
            NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::ShaderManager>();
            NLS::Core::ServiceLocator::Remove<C::Driver>();
        }
    } services{previousShaderProject, previousShaderEngine, previousMeshProject, previousMeshEngine};

    // Deferred sky rendering only needs the sky proxy to carry a valid mesh;
    // supply one directly so this regression does not depend on model import.
    std::vector<R::Geometry::Vertex> vertices(3);
    vertices[0].position[0] = -1.0f;
    vertices[0].position[1] = -1.0f;
    vertices[1].position[1] = 1.0f;
    vertices[2].position[0] = 1.0f;
    vertices[2].position[1] = -1.0f;
    auto* mesh = meshes.RegisterResource(":Models/SkyCube.obj", new R::Resources::Mesh(
        vertices, {}, 0u, R::Resources::MeshBufferUploadMode::CpuToGpu, {{0, 0, 0}, 2.0f}));
    E::SceneSystem::Scene scene;
    auto* sky = scene.CreateGameObject("Skybox").AddComponent<E::Components::SkyBoxComponent>();
    ASSERT_TRUE(sky && sky->GetMesh() && sky->GetMaterial());
    R::Buffers::Framebuffer output(128u, 96u);
    E::Rendering::DeferredSceneRenderer renderer(driver);
    R::Resources::Material surface(E::Rendering::DeferredSceneRendererTestAccess::GetGBufferShader(renderer));
    auto& object = scene.CreateGameObject("Wireframe triangle");
    object.AddComponent<E::Components::MeshFilter>()->SetMesh(mesh);
    object.AddComponent<E::Components::MeshRenderer>()->FillWithMaterial(surface);
    object.GetTransform()->SetWorldPosition({0, 0, -6});
    R::Entities::Camera camera;
    R::Data::FrameDescriptor frame;
    frame.camera = &camera;
    frame.renderWidth = 128u;
    frame.renderHeight = 96u;
    frame.outputBuffer = &output;
    const auto render = [&](E::Rendering::SceneGeometryDrawMode mode, bool skybox)
    {
        E::Rendering::BaseSceneRenderer::SceneDescriptor descriptor{scene, std::nullopt, nullptr};
        descriptor.includeSkyboxes = skybox;
        descriptor.viewOptions.geometryMode = mode;
        descriptor.viewOptions.backgroundColor = {0, 0, 0, 1};
        descriptor.suppressHZBOcclusion = true;
        renderer.AddDescriptor<E::Rendering::BaseSceneRenderer::SceneDescriptor>(std::move(descriptor));
        renderer.BeginFrame(frame);
        renderer.DrawFrame();
        renderer.EndFrame();
        EXPECT_TRUE(C::DriverRendererAccess::TryDrainThreadedRendering(driver));
        std::array<uint8_t, 4> pixel{};
        auto read = C::DriverRendererAccess::BeginReadPixels(driver, output.GetExplicitTextureHandle(),
            5, 5, 1, 1, R::Settings::EPixelDataFormat::RGBA, R::Settings::EPixelDataType::UNSIGNED_BYTE, pixel.data());
        EXPECT_TRUE(read.Succeeded()) << read.message;
        if (read.completion) EXPECT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
        return pixel;
    };
    using Mode = E::Rendering::SceneGeometryDrawMode;
    const auto shaded = render(Mode::Shaded, true);
    EXPECT_GT(shaded[0] + shaded[1] + shaded[2], 100);
    const auto wire = render(Mode::Wireframe, true);
    EXPECT_LT(wire[0] + wire[1] + wire[2], 5);
    E::Rendering::SceneRenderOptions wireOptions;
    wireOptions.geometryMode = Mode::Wireframe;
    auto& diagnostic = E::Rendering::DeferredSceneRendererTestAccess::ResolveSceneDebugMaterialForTesting(
        renderer, surface, wireOptions, false, false, true);
    const auto diagnosticId = diagnostic.GetInstanceId();
    const auto device = C::DriverRendererAccess::GetExplicitDevice(driver);
    const auto wireLayout = diagnostic.GetExplicitPipelineLayout(device);
    ASSERT_TRUE(wireLayout);
    EXPECT_EQ(render(Mode::Wireframe, true), wire);
    auto& reusedDiagnostic = E::Rendering::DeferredSceneRendererTestAccess::ResolveSceneDebugMaterialForTesting(
        renderer, surface, wireOptions, false, false, true);
    EXPECT_EQ(reusedDiagnostic.GetInstanceId(), diagnosticId);
    EXPECT_EQ(reusedDiagnostic.GetExplicitPipelineLayout(device), wireLayout)
        << "The next wireframe frame must reuse its native pipeline layout.";
    const auto restored = render(Mode::Shaded, true);
    EXPECT_EQ(restored, shaded);
#else
    GTEST_SKIP() << "Windows DX12 acceptance test.";
#endif
}
TEST_F(SceneViewDrawModeGpuTests, Dx12AlphaCompositesCoverageAndPreservesClipping)
{
    Surface surface; surface.color[3] = 0.5f;
    EXPECT_EQ(Render(6,surface,0,true)[0],0);
    EXPECT_NEAR(Render(6,surface,1,true)[0],128,2);
    EXPECT_NEAR(Render(6,surface,2,true)[0],191,2);
    EXPECT_EQ(Render(6,surface,1,false)[0],255);
    surface.arguments = {0,0.75f,1,0}; EXPECT_EQ(Render(6,surface,2,true)[0],0);
}
TEST_F(SceneViewDrawModeGpuTests, Dx12OverdrawAccumulatesBeyondUnormAndResolvesHeat)
{
    const auto empty = Render(7,{},0), one = Render(7), sixteen = Render(7,{},16), thirtyTwo = Render(7,{},32);
    EXPECT_EQ(empty[0]+empty[1]+empty[2],0);
    EXPECT_GT(one[2],one[0]); EXPECT_GT(sixteen[0],200); EXPECT_LT(sixteen[2],5);
    EXPECT_EQ(sixteen,thirtyTwo);
}
TEST_F(SceneViewDrawModeGpuTests, Dx12MipDensityTracksProjectedUvAndViewportSize)
{
    Surface surface; auto matched = Render(8,surface); EXPECT_GT(matched[1],150); EXPECT_EQ(matched[0],0);
    surface.uv = {1,1,512,512}; auto oversized = Render(8,surface); EXPECT_GT(oversized[0],240);
    // Compare viewport sizes while the density tint is still unsaturated.
    surface.uv = {1,1,256,256}; const auto moderate = Render(8,surface);
    auto resized = Render(8,surface,1,false,true,true,64); EXPECT_LT(resized[0],moderate[0]);
    surface.uv = {1,1,1,1}; auto small = Render(8,surface); EXPECT_GT(small[2],200);
    const auto absent = Render(8,surface,1,false,true,false); EXPECT_EQ(absent[0],absent[1]);
}
TEST_F(SceneViewDrawModeGpuTests, Dx12MaterialValidationUsesLinearValuesBeforeClamping)
{
    Surface surface; surface.color = {0.001f,0.001f,0.001f,1}; EXPECT_GT(Render(9,surface)[0],240);
    surface.color = {2,2,2,1}; auto high = Render(9,surface); EXPECT_GT(high[2],240); EXPECT_LT(high[0],5);
    surface.color = {0.18f,0.18f,0.18f,1}; auto valid = Render(9,surface); EXPECT_EQ(valid[0],valid[1]); EXPECT_GT(valid[0],20);
    surface.arguments[0] = 1; auto metal = Render(10,surface); EXPECT_GT(metal[0],240); EXPECT_LT(metal[2],5);
    surface.color = {0.8f,0.8f,0.8f,1}; auto metalValid = Render(10,surface); EXPECT_EQ(metalValid[0],metalValid[1]);
    auto unlit = Render(9,surface,1,false,false); EXPECT_NEAR(unlit[0],89,2);
}

TEST_F(SceneViewDrawModeGpuTests, Dx12AlbedoThresholdsInterpolateInLinearSpace)
{
    const auto linear = [](float value) { value /= 255; return value <= 0.04045f ? value/12.92f : std::pow((value+0.055f)/1.055f,2.4f); };
    for (float metal : {0.0f,0.5f,1.0f})
    {
        SCOPED_TRACE(metal);
        const float low = std::lerp(linear(50),linear(186),metal);
        const float high = std::lerp(linear(243),linear(255),metal);
        Surface surface; surface.arguments[0] = metal;
        surface.color = {low-0.001f,low-0.001f,low-0.001f,1};
        const auto below = Render(9,surface); EXPECT_GT(below[0],240); EXPECT_LT(below[2],5);
        surface.color = {low+0.001f,low+0.001f,low+0.001f,1};
        const auto inside = Render(9,surface); EXPECT_EQ(inside[0],inside[2]);
        surface.color = {high+0.001f,high+0.001f,high+0.001f,1};
        const auto above = Render(9,surface); EXPECT_GT(above[2],240); EXPECT_LT(above[0],5);
    }
}

TEST_F(SceneViewDrawModeGpuTests, BuiltinDiagnosticPassesCompileWithSkinningVariants)
{
    namespace M = NLS::MaterialCompiler;
    const auto fallback = std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/SceneDebugFallback.hlsl";
    ASSERT_TRUE(Compile(fallback,false,"VSMain"));
    ASSERT_TRUE(Compile(fallback,true,"PSMain"));
    for (const char* name : {"StandardPBR","UnlitTexture","UnlitColor","AlphaTest","Transparent"})
    {
        const auto path = std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/ShaderLab" / (std::string(name)+".shader");
        std::ifstream file(path); const std::string source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        const auto parsed = R::ShaderLab::ParseShaderLabSource(source,path.string());
        ASSERT_TRUE(parsed.Succeeded()) << parsed.DiagnosticsToString();
        const auto& passes = parsed.asset.subShaders.front().passes;
        const auto found = std::find_if(passes.begin(),passes.end(),[](const auto& pass) { return pass.name == "SceneDebug"; });
        ASSERT_NE(found,passes.end()) << name;
        M::MaterialShaderCompileOptions options;
        options.sourcePath = path; options.sourceIdentity = path.generic_string(); options.subAssetKey = "SceneDebug";
        options.generatedPath = root / (std::string(name)+"Debug.hlsl"); options.targets = {R::ShaderCompiler::ShaderTargetPlatform::DXIL};
        options.includeDirectories = {(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders").string()};
        R::ShaderLab::ShaderLabKeywordSet skinned; skinned.Enable("NLS_SKINNED"); options.variants = {{},skinned}; options.generateGlsl = false;
        if (std::string_view(name) == "StandardPBR" || std::string_view(name) == "AlphaTest")
        {
            R::ShaderLab::ShaderLabKeywordSet masked; masked.Enable("_ALPHATEST_ON");
            auto maskedSkin = masked; maskedSkin.Enable("NLS_SKINNED"); options.variants.push_back(masked); options.variants.push_back(maskedSkin);
        }
        const auto artifact = M::CompileMaterialPass(*found,options,M::BuildMaterialParameterSchema(parsed.asset,path.generic_string()));
        ASSERT_FALSE(artifact.stages.empty());
        EXPECT_EQ(artifact.materialInterface.unlit, std::string_view(name) != "StandardPBR");
        if (std::string_view(name) == "StandardPBR")
            for (const auto& variant : artifact.variantReflections)
            {
                const auto& properties = variant.reflection.properties;
                EXPECT_TRUE(std::any_of(properties.begin(),properties.end(),[](const auto& property) { return property.name == "ObjectData"; }));
                EXPECT_TRUE(std::any_of(properties.begin(),properties.end(),[](const auto& property) { return property.name == "InstanceObjectIndices"; }));
            }
        for (const auto& stage : artifact.stages)
            EXPECT_EQ(stage.output.status,R::ShaderCompiler::ShaderCompilationStatus::Succeeded) << name << ": " << stage.output.diagnostics;
    }
}

TEST_F(SceneViewDrawModeGpuTests, Dx12UnsupportedShadersAreGrayInCoverageDiagnostics)
{
    for (int mode : {6,7})
    {
        const auto gray = Render(mode,{},1,true,true,true,32,false);
        EXPECT_NEAR(gray[0],89,2); EXPECT_EQ(gray[0],gray[1]); EXPECT_EQ(gray[0],gray[2]);
    }
}
TEST_F(SceneViewDrawModeGpuTests, GraphDiagnosticPassRetainsDisplacementMaskAndSkinning)
{
    namespace M = NLS::MaterialCompiler;
    auto graph = M::CreateDefaultMaterialGraph("Diagnostic graph");
    graph.blend = M::MaterialGraphBlend::Masked; graph.maximumWorldDisplacement = 0.25f;
    const auto offset = M::AddMaterialGraphNode(graph,"Constant",M::MaterialGraphType::Float3);
    graph.nodes.back().value = {0.1f,0,0,0}; graph.outputs["WorldPositionOffset"] = {offset,0};
    // Texture derivatives must stay inside each static branch's UV scope.
    graph.parameters.push_back({901,"Main texture","",M::MaterialGraphType::Texture2D,std::string{}});
    graph.parameters.push_back({902,"Use texture","",M::MaterialGraphType::Bool,true,false,true});
    const auto texture = M::AddMaterialGraphNode(graph,"Parameter",M::MaterialGraphType::Texture2D);
    graph.nodes.back().parameter = 901;
    const auto uv = M::AddMaterialGraphNode(graph,"UV",M::MaterialGraphType::Float2);
    const auto scale = M::AddMaterialGraphNode(graph,"Constant",M::MaterialGraphType::Float2);
    graph.nodes.back().value = {2,3,0,0};
    const auto scaled = M::AddMaterialGraphNode(graph,"Multiply",M::MaterialGraphType::Float2);
    graph.nodes.back().inputs = {{"A",{uv,0}},{"B",{scale,0}}};
    const auto sample = M::AddMaterialGraphNode(graph,"SampleTexture",M::MaterialGraphType::Float4);
    graph.nodes.back().inputs = {{"Texture",{texture,0}},{"UV",{scaled,0}}};
    const auto branch = M::AddMaterialGraphNode(graph,"StaticSwitch",M::MaterialGraphType::Float4);
    graph.nodes.back().parameter = 902; graph.nodes.back().inputs = {{"True",{sample,0}},{"False",graph.outputs.at("BaseColor")}};
    graph.outputs["BaseColor"] = {branch,0};
    const auto generated = M::GenerateMaterialGraphShader(graph);
    ASSERT_TRUE(generated.Succeeded()) << (generated.diagnostics.empty() ? "" : generated.diagnostics.front().message);
    const auto& passes = generated.shader.subShaders.front().passes;
    auto pass = std::find_if(passes.begin(),passes.end(),[](const auto& value) { return value.name == "SceneDebug"; });
    ASSERT_NE(pass,passes.end()); M::MaterialShaderCompileOptions options;
    options.sourcePath = root / "Graph.shadergraph"; options.sourceIdentity = "Assets/Diagnostic.shadergraph";
    options.generatedPath = root / "Graph.hlsl"; options.subAssetKey = "SceneDebug";
    options.includeDirectories = {(std::filesystem::path(NLS_ROOT_DIR)/"App/Assets/Engine/Shaders").string()};
    options.targets = {R::ShaderCompiler::ShaderTargetPlatform::DXIL}; options.generateGlsl = false;
    const auto artifact = M::CompileMaterialPass(*pass,options,generated.schema);
    EXPECT_FALSE(artifact.materialInterface.unlit);
    EXPECT_FLOAT_EQ(artifact.materialInterface.maximumWorldDisplacement,0.25f);
    EXPECT_GE(artifact.variantReflections.size(),2u);
    for (const auto& stage : artifact.stages)
        EXPECT_EQ(stage.output.status,R::ShaderCompiler::ShaderCompilationStatus::Succeeded) << stage.output.diagnostics;
}
}
