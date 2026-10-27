#include "VfxTestExecution.h"
#include <gtest/gtest.h>
#include <GLFW/glfw3.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/RHI/Backends/Vulkan/VulkanExplicitDeviceFactory.h>
#include <Rendering/Settings/DriverSettings.h>
#include <VFX/SystemRenderProxy.h>
#include <VFXCompiler/VfxShaderCompiler.h>
#include "VFX/Editor/VfxSession.h"
#include "VFX/Editor/VfxSystemPreview.h"
#include "Assets/AssetDatabaseFacade.h"
#include <Engine/Assets/ParticleSystemAsset.h>
#include <Engine/Components/ParticleSystemComponent.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/GameObject.h>
#include "VfxAuthoredStageTestAsset.h"
#include "VfxTypedTestAsset.h"
#include <chrono>
#include <thread>

namespace
{
void ValidateAuthoredStages(bool vulkan,bool dynamicControls = false)
{
    using namespace NLS;
    namespace author = VFX::Authoring;
    namespace rhi = Render::RHI;
    namespace context = Render::Context;
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};
    struct Window { GLFWwindow* handle = nullptr; ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } } } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP() << "GLFW initialization unavailable";
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(128,128,"Authored stages",nullptr,nullptr);
        if (!window.handle) { glfwTerminate(); GTEST_SKIP() << "Vulkan WSI window unavailable"; }
    }
    Render::Settings::DriverSettings settings; settings.debugMode = true; settings.platformWindow = window.handle;
    settings.graphicsBackend = vulkan ? Render::Settings::EGraphicsBackend::VULKAN : Render::Settings::EGraphicsBackend::DX12;
    auto device = Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "Requested device unavailable";
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    const auto validationBefore = Render::Backend::GetVulkanValidationErrorCountForTesting();
    auto fixture = Tests::CreateAuthoredStageTestAsset(true); author::SourceLibrary library; std::string error;
    if (dynamicControls)
    {
        fixture.system.parameters.push_back({Guid::New().ToString(),"User.StageEnabled",VFX::ValueType::Float,{1,0,0,0}});
        fixture.system.parameters.push_back({Guid::New().ToString(),"User.StageIterations",VFX::ValueType::Float,{2,0,0,0}});
        fixture.emitter.versions[0].emitter.stacks[4].bindings = {"User.StageEnabled","User.StageIterations"};
    }
    ASSERT_TRUE(library.Add(fixture.function,{},error)); ASSERT_TRUE(library.Add(fixture.module,{},error)); ASSERT_TRUE(library.Add(fixture.emitter,{},error));
    Editor::VFXTools::VfxSession session; session.SetLibrary(library); ASSERT_TRUE(session.document.Apply(fixture.system,"Inherited stages")); ASSERT_TRUE(session.Rebuild());
    auto compiled = std::make_shared<VFX::CompiledSystem>(*session.Program()); std::vector<VFX::Diagnostic> diagnostics;
    const auto root = std::filesystem::temp_directory_path()/("NullusAuthoredStages_"+Guid::New().ToString()); std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code error; std::filesystem::remove_all(root,error); } } cleanup{root};
    const auto backend = device->GetAdapter()->GetBackendType();
    ASSERT_TRUE(VFX::CompilePlatformPrograms(*compiled,std::span(&backend,1),root,diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    ASSERT_TRUE(VFX::DeserializeCompiledSystem(VFX::SerializeCompiledSystem(*compiled,false),*compiled,error)) << error;
    EXPECT_TRUE(compiled->emitters[0].stages[0].hlsl.empty()); EXPECT_TRUE(compiled->asset.emitters[0].simulationStages[0].graph.nodes.empty());
    VFX::SystemInstance world; ASSERT_TRUE(world.Initialize(compiled,error)); auto proxy = std::make_unique<VFX::SystemRenderProxy>(compiled);
    ASSERT_TRUE(proxy->Prepare(device,world.Capture(),error)) << error;
    std::vector<std::shared_ptr<const VFX::SystemFrame>> previewFrames{world.Capture()};
    const auto read = [&](const auto& buffer,void* data,size_t size)
    {
        rhi::RHIBufferReadbackDesc request; request.source = buffer; request.data = data; request.size = size;
        const auto result = device->BeginReadBuffer(request); EXPECT_TRUE(result.Succeeded()) << result.message;
        return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded());
    };
    const auto compare = [&](const std::shared_ptr<const VFX::SystemRenderFrame>& frame,float x,float size,float age)
    {
        ASSERT_TRUE(frame); const auto& data = frame->emitters[0]; ASSERT_TRUE(data); std::array<uint32_t,8> counters{};
        ASSERT_TRUE(read(data->counters,counters.data(),sizeof(counters))); ASSERT_EQ(counters[0],1u);
        std::vector<float> values(data->values->GetDesc().size/4); ASSERT_TRUE(read(data->values,values.data(),values.size()*4));
        size_t scalar = 0;
        for (const auto& attribute : compiled->asset.emitters[0].attributes)
        {
            if (attribute.name == "Particles.Position") EXPECT_NEAR(values[scalar*data->capacity],x,1e-5f);
            if (attribute.name == "Particles.Size") EXPECT_NEAR(values[scalar*data->capacity],size,1e-5f);
            if (attribute.name == "Particles.Age") EXPECT_NEAR(values[scalar*data->capacity],age,1e-5f);
            scalar += VFX::ComponentCount(attribute.type);
        }
    };
    ASSERT_TRUE(world.Tick(1.0/60)); previewFrames.push_back(world.Capture()); const auto early = proxy->Prepare(device,world.Capture(),error);
    compare(early,-0.44f,0.3f,1.0f/60);
    ASSERT_TRUE(world.SetParameter("User.Offset",{0.02f,0,0,0})); ASSERT_TRUE(proxy->Prepare(device,world.Capture(),error));
    ASSERT_TRUE(world.Tick(1.0/60)); compare(proxy->Prepare(device,world.Capture(),error),-0.32f,0.15f,2.0f/60);
    world.SetPaused(true); ASSERT_TRUE(world.Tick(0.2)); compare(proxy->Prepare(device,world.Capture(),error),-0.32f,0.15f,2.0f/60);
    world.SetPaused(false); world.Reset(); ASSERT_TRUE(proxy->Prepare(device,world.Capture(),error));
    ASSERT_TRUE(world.Tick(1.0/60)); compare(proxy->Prepare(device,world.Capture(),error),-0.38f,0.3f,1.0f/60);
    proxy.reset(); compare(early,-0.44f,0.3f,1.0f/60);
    if (dynamicControls)
    {
        VFX::SystemInstance controlled; ASSERT_TRUE(controlled.Initialize(compiled,error)); auto controlProxy = std::make_unique<VFX::SystemRenderProxy>(compiled);
        ASSERT_TRUE(controlProxy->Prepare(device,controlled.Capture(),error));
        ASSERT_TRUE(controlled.SetParameters({{"User.StageEnabled",{}},{"User.StageIterations",{4,0,0,0}}}));
        const auto disabled = controlled.Capture(); ASSERT_TRUE(controlled.Tick(1.0/60));
        auto prepared = controlProxy->Prepare(device,controlled.Capture(),error); compare(prepared,-0.47f,0.3f,1.0f/60);
        const auto retained = prepared;
        ASSERT_TRUE(controlled.SetParameters({{"User.StageEnabled",{1,0,0,0}},{"User.StageIterations",{3,0,0,0}}}));
        ASSERT_TRUE(controlled.Tick(1.0/60)); compare(controlProxy->Prepare(device,controlled.Capture(),error),-0.38f,0.15f,2.0f/60);
        ASSERT_TRUE(controlled.SetParameter("User.StageIterations",{})); ASSERT_TRUE(controlled.Tick(1.0/60)); compare(controlProxy->Prepare(device,controlled.Capture(),error),-0.35f,0.075f,3.0f/60);
        const auto beforeInvalid = controlled.Capture(); EXPECT_FALSE(controlled.SetParameter("User.StageIterations",{129,0,0,0}));
        EXPECT_FALSE(controlled.SetParameter("User.StageEnabled",{0.5f,0,0,0})); EXPECT_EQ(controlled.Capture(),beforeInvalid);
        ASSERT_TRUE(controlled.SetParameter("User.StageIterations",{-1,0,0,0})); ASSERT_TRUE(controlled.Tick(1.0/60)); compare(controlProxy->Prepare(device,controlled.Capture(),error),-0.32f,0.0375f,4.0f/60);
        controlled.Reset(); ASSERT_TRUE(controlProxy->Prepare(device,controlled.Capture(),error)); ASSERT_TRUE(controlled.SetParameter("User.StageIterations",{1,0,0,0}));
        ASSERT_TRUE(controlled.Tick(1.0/60)); compare(controlProxy->Prepare(device,controlled.Capture(),error),-0.46f,0.3f,1.0f/60);
        compare(retained,-0.47f,0.3f,1.0f/60); EXPECT_FLOAT_EQ(disabled->parameters.at("User.StageEnabled")[0],0);
        controlProxy.reset(); compare(retained,-0.47f,0.3f,1.0f/60);

        // Direct emitter recording captures controls before submission. Abandon
        // one recording, then change controls after recording the real frame.
        VFX::GpuEmitter direct; ASSERT_TRUE(direct.Initialize(device,compiled->asset.emitters[0],compiled->emitters[0],compiled->FindPlatform(backend)->emitters[0].simulation,error)) << error;
        auto pool = device->CreateCommandPool(rhi::QueueType::Graphics,"StageControlCapture"); ASSERT_TRUE(pool);
        {
            auto commands = pool->CreateCommandBuffer("AbandonedControls"); commands->Begin(); auto packet = direct.RecordFrame(*commands,1,1.0/60,error); ASSERT_TRUE(packet) << error;
            commands->End(); packet.reset(); EXPECT_DOUBLE_EQ(direct.Age(),0);
        }
        ASSERT_TRUE(direct.SetParameters({{"User.StageEnabled",{}},{"User.StageIterations",{4,0,0,0}}}));
        auto commands = pool->CreateCommandBuffer("CapturedControls"); commands->Begin(); auto packet = direct.RecordFrame(*commands,1,1.0/60,error); ASSERT_TRUE(packet) << error;
        const auto snapshot = packet->Data().RecordSnapshot(*commands,error); ASSERT_TRUE(snapshot); commands->End();
        ASSERT_TRUE(direct.SetParameters({{"User.StageEnabled",{1,0,0,0}},{"User.StageIterations",{3,0,0,0}}}));
        EXPECT_FALSE(direct.SetParameter("User.StageIterations",{2.5f,0,0,0}));
        auto fence = device->CreateFence("CapturedControlsComplete"); ASSERT_TRUE(fence); fence->Reset(); rhi::RHISubmitDesc submit; submit.commandBuffers = {commands}; submit.signalFence = fence;
        ASSERT_TRUE(device->GetQueue(rhi::QueueType::Graphics)->SubmitChecked(submit).Succeeded()); ASSERT_TRUE(direct.CommitFrame(packet)); ASSERT_TRUE(fence->Wait(30000000000ull));
        auto result = std::make_shared<VFX::SystemRenderFrame>(); result->emitters = {snapshot}; compare(result,-0.47f,0.3f,1.0f/60);
    }

    Editor::VFXTools::VfxSystemPreview preview; Editor::VFXTools::VfxSystemPreviewInput input; input.width = 128; input.height = 128;
    input.camera.viewProjection[10] = 0.01f; input.camera.viewProjection[11] = 0.5f;
    struct Drain { Editor::VFXTools::VfxSystemPreview& preview; ~Drain() { preview.Clear(); context::RenderCommandDispatcher::FlushToDrain(); (void)context::RhiCommandListExecutor::Get().Flush(context::RhiFlushFlags::Gpu|context::RhiFlushFlags::ReleaseResources); } } drain{preview};
    preview.Update(device,compiled,std::move(previewFrames),root,input); std::shared_ptr<rhi::RHITextureView> view;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(45);
    while (std::chrono::steady_clock::now() < deadline)
    {
        preview.Update(device,compiled,{},root,input); context::RenderCommandDispatcher::Dispatch(); view = preview.View(); if (view) break;
        const auto status = preview.Status(); if (!status.empty() && status != "Compiling shaders...") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(view) << preview.Status(); std::vector<uint8_t> pixels(128*128*4);
    auto image = device->BeginReadPixels(view->GetTexture(),0,0,128,128,Render::Settings::EPixelDataFormat::RGBA,Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
    ASSERT_TRUE(image.Succeeded()) << image.message; if (image.completion) ASSERT_TRUE(image.completion->Wait(30000000000ull).Succeeded());
    size_t green = 0; for (size_t i = 0; i < pixels.size(); i += 4) green += pixels[i+1] > 180 && pixels[i] < 50;
    EXPECT_GT(green,20u);
    preview.Clear(); context::RenderCommandDispatcher::FlushToDrain(); (void)executor.Flush(context::RhiFlushFlags::Gpu|context::RhiFlushFlags::ReleaseResources);
    if (vulkan) EXPECT_EQ(Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}
}
TEST(VfxSimulationAuthoringDevice, Dx12InheritedStagesExecuteAndPreviewFromStrippedPrograms)
{
#if defined(_WIN32)
    ValidateAuthoredStages(false);
#else
    GTEST_SKIP() << "DX12 requires Windows";
#endif
}
TEST(VfxSimulationAuthoringDevice, VulkanInheritedStagesExecuteAndPreviewFromStrippedPrograms)
{ ValidateAuthoredStages(true); }
TEST(VfxSimulationAuthoringDevice, Dx12DynamicStageControlsPreserveFrameSnapshots)
{
#if defined(_WIN32)
    ValidateAuthoredStages(false,true);
#else
    GTEST_SKIP() << "DX12 requires Windows";
#endif
}
TEST(VfxSimulationAuthoringDevice, VulkanDynamicStageControlsPreserveFrameSnapshots)
{ ValidateAuthoredStages(true,true); }
TEST(VfxSimulationAuthoringDevice, NativeImportRetainsStagesAndDependenciesWithoutSourceAssets)
{
    using namespace NLS; namespace author = VFX::Authoring;
    const auto root = std::filesystem::temp_directory_path()/("NullusStageImport_"+Guid::New().ToString()); std::filesystem::create_directories(root/"Assets");
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code error; std::filesystem::remove_all(root,error); } } cleanup{root};
    auto fixture = Tests::CreateAuthoredStageTestAsset(true); std::string error;
    fixture.system.timing = {true,0.125,2,0.2};
    fixture.system.parameters.push_back({Guid::New().ToString(),"User.StageEnabled",VFX::ValueType::Float,{1,0,0,0}});
    fixture.system.parameters.push_back({Guid::New().ToString(),"User.StageIterations",VFX::ValueType::Float,{2,0,0,0}});
    fixture.emitter.versions[0].emitter.stacks[4].bindings = {"User.StageEnabled","User.StageIterations"};
    ASSERT_TRUE(author::SaveScript(root/"Assets/Weight.vfxscript",fixture.function,error)); ASSERT_TRUE(author::SaveScript(root/"Assets/Offset.vfxscript",fixture.module,error));
    ASSERT_TRUE(author::SaveEmitterSource(root/"Assets/Emitter.vfxemitter",fixture.emitter,error)); ASSERT_TRUE(author::Save(root/"Assets/System.vfx",fixture.system,error));
    Editor::Assets::AssetDatabaseFacade database(Editor::Assets::MakeProjectEditorAssetRoots(root)); ASSERT_TRUE(database.Refresh());
    const auto imported = database.ImportAsset("Assets/System.vfx"); if (!imported) for (const auto& issue : database.GetDiagnostics()) ADD_FAILURE() << issue.message; ASSERT_TRUE(imported);
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/System.vfx"); ASSERT_TRUE(manifest); EXPECT_GE(manifest->dependencies.size(),3u);
    const auto artifact = database.ResolveArtifactPathAtPath("Assets/System.vfx",manifest->primarySubAssetKey); const auto hash = manifest->FindPrimaryArtifact()->contentHash;
    fixture.system.emitters[0].stackOverrides.push_back({Guid::New().ToString(),false,{},{}}); ASSERT_TRUE(author::Save(root/"Assets/System.vfx",fixture.system,error));
    EXPECT_FALSE(database.ImportAsset("Assets/System.vfx")); EXPECT_EQ(database.GetArtifactManifestForAssetPath("Assets/System.vfx")->FindPrimaryArtifact()->contentHash,hash);
    std::filesystem::rename(root/"Assets",root/"Sources.hidden"); Engine::Assets::ParticleSystemAsset runtime; ASSERT_TRUE(runtime.Reload(artifact,hash,error)) << error;
    ASSERT_TRUE(runtime.Program()); ASSERT_EQ(runtime.Program()->emitters[0].stages.size(),4u); EXPECT_EQ(runtime.Program()->asset.emitters[0].simulationStages[0].iterations,2u);
    EXPECT_EQ(runtime.Program()->asset.timing,fixture.system.timing);
    EXPECT_EQ(runtime.Program()->platforms.size(),3u); EXPECT_TRUE(runtime.Program()->emitters[0].stages[0].hlsl.empty()); EXPECT_TRUE(runtime.Program()->asset.emitters[0].simulationStages[0].graph.nodes.empty());
    EXPECT_EQ(runtime.Program()->asset.emitters[0].simulationStages[0].bindings.enabled,"User.StageEnabled"); EXPECT_EQ(runtime.Program()->asset.emitters[0].simulationStages[0].bindings.iterations,"User.StageIterations");
}
TEST(VfxSimulationAuthoringDevice, SceneComponentRejectsIncompatibleControlsDuringHotReload)
{
    using namespace NLS; namespace author = VFX::Authoring;
    auto fixture = Tests::CreateAuthoredStageTestAsset(); author::SourceLibrary library; std::string error;
    ASSERT_TRUE(library.Add(fixture.function,{},error)); ASSERT_TRUE(library.Add(fixture.module,{},error));
    fixture.system.parameters.push_back({Guid::New().ToString(),"User.Control",VFX::ValueType::Float,{0,0,0,0}});
    fixture.system.emitters[0].stacks[4].bindings.iterations = "User.Control";
    auto program = std::make_shared<VFX::CompiledSystem>(); std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(fixture.system,library,*program,dependencies,issues));
    Engine::SceneSystem::Scene scene; auto& object = scene.CreateGameObject("Controlled particles"); auto* component = object.AddComponent<Engine::Components::ParticleSystemComponent>(); ASSERT_TRUE(component);
    ASSERT_TRUE(component->SetCompiledSystem(program,error)); ASSERT_TRUE(component->SetFloatParameter("User.Control",3));
    component->OnUpdate(1.0f/60); component->SetPaused(true); component->Stop();
    const auto before = component->CaptureFrame(); const auto proxy = component->RenderProxy();
    fixture.system.emitters[0].stacks[4].bindings = {"User.Control",{}};
    auto replacement = std::make_shared<VFX::CompiledSystem>(); ASSERT_TRUE(author::Compile(fixture.system,library,*replacement,dependencies,issues));
    EXPECT_FALSE(component->SetCompiledSystem(replacement,error)); EXPECT_FALSE(error.empty()); EXPECT_EQ(component->CaptureFrame(),before); EXPECT_EQ(component->RenderProxy(),proxy);
    ASSERT_TRUE(component->SetFloatParameter("User.Control",1)); ASSERT_TRUE(component->SetCompiledSystem(replacement,error));
    EXPECT_TRUE(component->CaptureFrame()->paused); EXPECT_FALSE(component->CaptureFrame()->spawning); EXPECT_FLOAT_EQ(component->CaptureFrame()->parameters.at("User.Control")[0],1);
    const auto accepted = component->CaptureFrame(); EXPECT_FALSE(component->SetFloatParameter("User.Control",0.25f)); EXPECT_EQ(component->CaptureFrame(),accepted);
}

namespace
{
void ValidateTypedDevice(bool vulkan)
{
    using namespace NLS; using namespace VFX; namespace rhi = Render::RHI; namespace context = Render::Context;
    auto& executor = context::RhiCommandListExecutor::Get(); if (executor.IsInitialized()) GTEST_SKIP() << "RHI already in use";
    executor.Initialize(context::RhiThreadMode::None,{}); struct Shutdown { ~Shutdown() { context::RhiCommandListExecutor::Get().Shutdown(); } } shutdown;
    struct Window { GLFWwindow* handle = nullptr; ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } } } window;
    if (vulkan) { if (!glfwInit()) GTEST_SKIP(); glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE); window.handle = glfwCreateWindow(128,128,"Typed VFX",nullptr,nullptr); if (!window.handle) { glfwTerminate(); GTEST_SKIP(); } }
    Render::Settings::DriverSettings settings; settings.debugMode = true; settings.platformWindow = window.handle;
    settings.graphicsBackend = vulkan ? Render::Settings::EGraphicsBackend::VULKAN : Render::Settings::EGraphicsBackend::DX12;
    auto device = Render::Backend::CreateRhiDevice(settings); if (!device || !device->IsBackendReady()) GTEST_SKIP(); executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    const auto validationBefore = Render::Backend::GetVulkanValidationErrorCountForTesting();
    const auto root = std::filesystem::temp_directory_path()/("NullusTypedGpu_"+Guid::New().ToString()); std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code error; std::filesystem::remove_all(root,error); } } cleanup{root};
    auto source = Tests::CreateTypedParticleTestAsset(); auto& graph = source.emitters[0].update;
    const auto flag = AddNode(graph,Operation::Parameter,ValueType::Bool); FindNode(graph,flag)->name = "User.Enabled"; FindNode(graph,flag)->value = BoolValue(true);
    const auto negate = AddNode(graph,Operation::LogicalNot,ValueType::Bool); FindNode(graph,negate)->inputs = {flag};
    const auto selected = AddNode(graph,Operation::Select,ValueType::Int32);
    const auto edge = AddNode(graph,Operation::Constant,ValueType::Int32); FindNode(graph,edge)->value = IntValue(INT32_MIN);
    const auto minus = AddNode(graph,Operation::Constant,ValueType::Int32); FindNode(graph,minus)->value = IntValue(-1);
    const auto division = AddNode(graph,Operation::Divide,ValueType::Int32); FindNode(graph,division)->inputs = {edge,minus}; FindNode(graph,selected)->inputs = {negate,minus,division};
    const auto output = AddNode(graph,Operation::WriteAttribute,ValueType::Int32); FindNode(graph,output)->name = "Particles.Edge"; FindNode(graph,output)->inputs = {selected};
    auto compiled = std::make_shared<CompiledSystem>(); std::vector<Diagnostic> diagnostics; ASSERT_TRUE(CompileSystem(source,*compiled,diagnostics));
    const auto backend = device->GetAdapter()->GetBackendType(); ASSERT_TRUE(CompilePlatformPrograms(*compiled,std::span(&backend,1),root,diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    std::string error; ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(*compiled,false),*compiled,error)) << error;
    auto cpuSettings = compiled->asset.emitters[0]; cpuSettings.target = SimulationTarget::CPU; cpuSettings.simulationStages.clear(); CpuEmitter cpu;
    ASSERT_TRUE(cpu.Initialize(cpuSettings,compiled->emitters[0].spawn,compiled->emitters[0].update,error,compiled->asset.parameters));
    SystemInstance world; ASSERT_TRUE(world.Initialize(compiled,error)); auto proxy = std::make_unique<SystemRenderProxy>(compiled); ASSERT_TRUE(proxy->Prepare(device,world.Capture(),error)) << error;
    ParticleDataSet expected; std::unordered_map<uint64_t,std::array<Value,3>> stored; std::unordered_map<std::string,Value> parameters;
    for (const auto& p : compiled->asset.parameters) parameters[p.name] = p.value;
    const auto advance = [&]()
    {
        ASSERT_TRUE(cpu.Tick(1.0/60)); expected = cpu.Data();
        for (uint32_t i = 0; i < expected.Count(); ++i) if (stored.contains(expected.Id(i)))
        { const auto previous = ReadInt(stored.at(expected.Id(i))[0]); const auto step = ReadInt(parameters.at("User.Step")); expected.Write(expected.FindAttribute("Particles.Counter"),i,IntValue(std::bit_cast<int32_t>(std::bit_cast<uint32_t>(previous)+std::bit_cast<uint32_t>(step)))); }
        std::vector<SimulationStageExecution> controls; ASSERT_TRUE(ResolveSimulationStageControls(compiled->asset.emitters[0].simulationStages,parameters,controls,error)); std::vector<Value> registers;
        for (uint32_t stage = 0; stage < controls.size(); ++stage) if (controls[stage].enabled)
                for (uint32_t iteration = 0; iteration < controls[stage].iterations; ++iteration)
                    ASSERT_TRUE(RunParticleProgram(compiled->emitters[0].stages[stage].program, expected, 0, expected.Count(), {1.0f / 60, 1, 0, &parameters, stage, iteration}, registers));
        for (uint32_t i = 0; i < expected.Count(); ++i) stored[expected.Id(i)] = {expected.Read(expected.FindAttribute("Particles.Counter"),i),expected.Read(expected.FindAttribute("Particles.Flag"),i),expected.Read(expected.FindAttribute("Particles.Edge"),i)};
    };
    const auto read = [&](const auto& buffer,void* data,size_t bytes)
    { rhi::RHIBufferReadbackDesc request; request.source = buffer; request.data = data; request.size = bytes; auto result = device->BeginReadBuffer(request); EXPECT_TRUE(result.Succeeded()) << result.message; return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded()); };
    const auto compare = [&](const std::shared_ptr<const SystemRenderFrame>& frame,const ParticleDataSet& reference)
    {
        ASSERT_TRUE(frame); const auto& data = frame->emitters[0]; std::array<uint32_t,8> counters{}; ASSERT_TRUE(read(data->counters,counters.data(),sizeof(counters))); ASSERT_EQ(counters[0],reference.Count());
        std::vector<uint64_t> ids(data->capacity); std::vector<float> values(data->values->GetDesc().size/4); ASSERT_TRUE(read(data->ids,ids.data(),ids.size()*8)); ASSERT_TRUE(read(data->values,values.data(),values.size()*4));
        std::unordered_map<uint64_t,uint32_t> lookup; for (uint32_t i = 0; i < reference.Count(); ++i) lookup[reference.Id(i)] = i;
        for (uint32_t i = 0; i < counters[0]; ++i)
        {
            ASSERT_TRUE(lookup.contains(ids[i])); const auto index = lookup.at(ids[i]); size_t scalar = 0;
            for (uint32_t a = 0; a < reference.Attributes().size(); ++a)
                for (uint32_t c = 0; c < ComponentCount(reference.Attributes()[a].type); ++c)
                { const auto actual = values[scalar++*data->capacity+i]; if (IsIntegralType(reference.Attributes()[a].type)) EXPECT_EQ(std::bit_cast<uint32_t>(actual),std::bit_cast<uint32_t>(reference.Read(a,index)[c])); else EXPECT_NEAR(actual,reference.Read(a,index)[c],1e-5f); }
        }
    };
    ASSERT_TRUE(world.Tick(1.0/60)); advance(); const auto early = proxy->Prepare(device,world.Capture(),error); compare(early,expected); const auto earlyReference = expected;
    for (const auto& input : std::array<std::pair<bool,int32_t>,3>{{{false,3},{true,0},{true,3}}})
    {
        parameters["User.Enabled"] = BoolValue(input.first); parameters["User.Iterations"] = IntValue(input.second);
        ASSERT_TRUE(world.SetParameters(parameters)); ASSERT_TRUE(cpu.SetParameter("User.Enabled",parameters["User.Enabled"]));
        ASSERT_TRUE(world.Tick(1.0/60)); advance(); compare(proxy->Prepare(device,world.Capture(),error),expected);
    }
    parameters["User.Counter"] = IntValue(INT32_MAX); ASSERT_TRUE(world.SetTypedParameter("User.Counter",ValueType::Int32,parameters["User.Counter"])); ASSERT_TRUE(cpu.SetParameter("User.Counter",parameters["User.Counter"]));
    world.Reset(); cpu.Reset(); stored.clear(); ASSERT_TRUE(world.Tick(1.0/60)); advance(); compare(proxy->Prepare(device,world.Capture(),error),expected);
    EXPECT_EQ(ReadInt(expected.Read(expected.FindAttribute("Particles.Counter"),0)),INT32_MIN+5);
    proxy.reset(); compare(early,earlyReference);
    Engine::SceneSystem::Scene scene; auto& object = scene.CreateGameObject("Typed parameters"); auto* component = object.AddComponent<Engine::Components::ParticleSystemComponent>(); ASSERT_TRUE(component->SetCompiledSystem(compiled,error));
    ASSERT_TRUE(component->SetIntParameter("User.Counter",INT32_MAX)); ASSERT_TRUE(component->SetBoolParameter("User.Enabled",true)); EXPECT_FALSE(component->SetFloatParameter("User.Counter",1)); EXPECT_EQ(ReadInt(component->CaptureFrame()->parameters.at("User.Counter")),INT32_MAX);
    EXPECT_TRUE(executor.Flush(context::RhiFlushFlags::Gpu|context::RhiFlushFlags::ReleaseResources)); if (vulkan) EXPECT_EQ(Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}
}
TEST(VfxTypedValuesDevice, Dx12Int32BitsBoolAndDynamicStagesMatchCpu)
{
#if defined(_WIN32)
    ValidateTypedDevice(false);
#else
    GTEST_SKIP();
#endif
}
TEST(VfxTypedValuesDevice, VulkanInt32BitsBoolAndDynamicStagesMatchCpu) { ValidateTypedDevice(true); }

namespace
{
void ValidateTimedSystemDevice(bool vulkan)
{
    using namespace NLS; using namespace VFX; namespace rhi = Render::RHI; namespace context = Render::Context;
    auto& executor = context::RhiCommandListExecutor::Get(); if (executor.IsInitialized()) GTEST_SKIP() << "RHI already in use";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { ~Shutdown() { context::RhiCommandListExecutor::Get().Shutdown(); } } shutdown;
    struct Window { GLFWwindow* handle = nullptr; ~Window() { if (handle) { glfwDestroyWindow(handle); glfwTerminate(); } } } window;
    if (vulkan)
    {
        if (!glfwInit()) GTEST_SKIP(); glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window.handle = glfwCreateWindow(128,128,"System timing",nullptr,nullptr); if (!window.handle) GTEST_SKIP();
    }
    Render::Settings::DriverSettings settings; settings.debugMode = true; settings.platformWindow = window.handle;
    settings.graphicsBackend = vulkan ? Render::Settings::EGraphicsBackend::VULKAN : Render::Settings::EGraphicsBackend::DX12;
    auto device = Render::Backend::CreateRhiDevice(settings); if (!device || !device->IsBackendReady()) GTEST_SKIP();
    executor.SetTargets(context::RhiCommandListExecutor::CreateDeviceTargets(device));
    const auto validationBefore = Render::Backend::GetVulkanValidationErrorCountForTesting();
    const auto root = std::filesystem::temp_directory_path()/("NullusTimedGpu_"+Guid::New().ToString()); std::filesystem::create_directories(root);
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code error; std::filesystem::remove_all(root,error); } } cleanup{root};
    auto source = CreateDefaultSystem(); source.timing = {true,0.125,2,0.2};
    auto& emitter = source.emitters[0]; emitter.capacity = 137; emitter.initialCapacity = 3;
    emitter.spawnRate = 0; emitter.bursts = {{0,129}}; emitter.interpolatedSpawning = false;
    emitter.attributes[1].defaultValue = {0.25f,0.5f,0,0}; emitter.spawn.nodes.clear(); emitter.update.nodes.clear();
    auto& graph = emitter.update;
    const auto position = AddNode(graph,Operation::Attribute,ValueType::Float3); FindNode(graph,position)->name = "Particles.Position";
    const auto velocity = AddNode(graph,Operation::Attribute,ValueType::Float3); FindNode(graph,velocity)->name = "Particles.Velocity";
    const auto delta = AddNode(graph,Operation::DeltaTime); const auto offset = AddNode(graph,Operation::Multiply,ValueType::Float3); FindNode(graph,offset)->inputs = {velocity,delta};
    const auto sum = AddNode(graph,Operation::Add,ValueType::Float3); FindNode(graph,sum)->inputs = {position,offset};
    const auto output = AddNode(graph,Operation::WriteAttribute,ValueType::Float3); FindNode(graph,output)->name = "Particles.Position"; FindNode(graph,output)->inputs = {sum};
    emitter.attributes.push_back({"Particles.TickCount",ValueType::Int32,IntValue(0)});
    const auto counter = AddNode(graph,Operation::Attribute,ValueType::Int32); FindNode(graph,counter)->name = "Particles.TickCount";
    const auto one = AddNode(graph,Operation::Constant,ValueType::Int32); FindNode(graph,one)->value = IntValue(1);
    const auto increment = AddNode(graph,Operation::Add,ValueType::Int32); FindNode(graph,increment)->inputs = {counter,one};
    const auto write = AddNode(graph,Operation::WriteAttribute,ValueType::Int32); FindNode(graph,write)->name = "Particles.TickCount"; FindNode(graph,write)->inputs = {increment};
    auto gpu = emitter; gpu.id = Guid::New().ToString(); gpu.target = SimulationTarget::GPU;
    gpu.spawn.id = Guid::New().ToString(); gpu.update.id = Guid::New().ToString(); source.emitters.push_back(std::move(gpu));
    auto compiled = std::make_shared<CompiledSystem>(); std::vector<Diagnostic> diagnostics; ASSERT_TRUE(CompileSystem(source,*compiled,diagnostics));
    const auto backend = device->GetAdapter()->GetBackendType(); ASSERT_TRUE(CompilePlatformPrograms(*compiled,std::span(&backend,1),root,diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    std::string error; ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(*compiled,false),*compiled,error)) << error;
    SystemInstance world; ASSERT_TRUE(world.Initialize(compiled,error)); auto proxy = std::make_unique<SystemRenderProxy>(compiled);
    ASSERT_TRUE(proxy->Prepare(device,world.Capture(),error)) << error;
    const auto read = [&](const auto& buffer,void* data,size_t bytes)
    {
        rhi::RHIBufferReadbackDesc request; request.source = buffer; request.data = data; request.size = bytes;
        const auto result = device->BeginReadBuffer(request); EXPECT_TRUE(result.Succeeded()) << result.message;
        return result.Succeeded() && (!result.completion || result.completion->Wait(30000000000ull).Succeeded());
    };
    const auto compare = [&](const std::shared_ptr<const SystemRenderFrame>& frame,const ParticleDataSet& reference)
    {
        ASSERT_TRUE(frame) << error; const auto& data = frame->emitters[1]; std::array<uint32_t,8> counters{};
        ASSERT_TRUE(read(data->counters,counters.data(),sizeof(counters))); ASSERT_EQ(counters[0],reference.Count());
        std::vector<float> values(data->values->GetDesc().size/sizeof(float)); ASSERT_TRUE(read(data->values,values.data(),values.size()*sizeof(float)));
        std::vector<uint64_t> ids(data->capacity); ASSERT_TRUE(read(data->ids,ids.data(),ids.size()*sizeof(uint64_t)));
        for (uint32_t i = 0; i < counters[0]; ++i)
        {
            uint32_t index = 0; while (index < reference.Count() && reference.Id(index) != ids[i]) ++index; ASSERT_LT(index,reference.Count());
            uint32_t scalar = 0;
            for (uint32_t a = 0; a < reference.Attributes().size(); ++a)
                for (uint32_t c = 0; c < ComponentCount(reference.Attributes()[a].type); ++c)
                {
                    const auto actual = values[scalar++*data->capacity+i];
                    if (IsIntegralType(reference.Attributes()[a].type)) EXPECT_EQ(std::bit_cast<uint32_t>(actual),std::bit_cast<uint32_t>(reference.Read(a,index)[c]));
                    else EXPECT_NEAR(actual,reference.Read(a,index)[c],1e-5f);
                }
        }
    };
    ASSERT_TRUE(world.Tick(0)); EXPECT_DOUBLE_EQ(world.Age(),0); EXPECT_DOUBLE_EQ(world.Capture()->simulationAge,0.375);
    const auto early = proxy->Prepare(device,world.Capture(),error); ASSERT_TRUE(early) << error;
    const auto earlyCpu = *world.Capture()->cpu[0]; compare(early,earlyCpu); EXPECT_EQ(earlyCpu.Count(),129u);
    EXPECT_EQ(ReadInt(earlyCpu.Read(earlyCpu.FindAttribute("Particles.TickCount"),0)),2);
    for (const auto dt : {0.03,0.07,0.4})
    { ASSERT_TRUE(world.Tick(dt)); compare(proxy->Prepare(device,world.Capture(),error),*world.Capture()->cpu[0]); }
    world.SetPaused(true); ASSERT_TRUE(world.Tick(0.3)); compare(proxy->Prepare(device,world.Capture(),error),*world.Capture()->cpu[0]);
    world.Reset(); world.SetPaused(false); ASSERT_TRUE(world.Tick(0)); compare(proxy->Prepare(device,world.Capture(),error),earlyCpu);
    ASSERT_TRUE(world.Tick(0.125)); const auto skipped = world.Capture(); ASSERT_TRUE(world.Tick(0.125));
    EXPECT_FALSE(proxy->Prepare(device,world.Capture(),error)); EXPECT_NE(error.find("skipped"),std::string::npos);
    ASSERT_TRUE(proxy->Prepare(device,skipped,error)); compare(proxy->Prepare(device,world.Capture(),error),*world.Capture()->cpu[0]);
    world.Deactivate(true); compare(proxy->Prepare(device,world.Capture(),error),*world.Capture()->cpu[0]);
    proxy.reset(); compare(early,earlyCpu);
    EXPECT_TRUE(executor.Flush(context::RhiFlushFlags::Gpu|context::RhiFlushFlags::ReleaseResources));
    if (vulkan) EXPECT_EQ(Render::Backend::GetVulkanValidationErrorCountForTesting(),validationBefore);
}
}
TEST(VfxTimingDevice, Dx12WarmupAndFixedTicksMatchCpuAndRetainOldFrames)
{
#if defined(_WIN32)
    ValidateTimedSystemDevice(false);
#else
    GTEST_SKIP();
#endif
}
TEST(VfxTimingDevice, VulkanWarmupAndFixedTicksMatchCpuAndRetainOldFrames) { ValidateTimedSystemDevice(true); }
