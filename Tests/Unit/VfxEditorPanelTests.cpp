#include <gtest/gtest.h>
#include <fstream>
#include <chrono>
#include <thread>
#include <functional>
#include <algorithm>
#include <array>
#include <cmath>
#include <Guid.h>
#include <imgui.h>
#include <imgui_node_editor.h>
#include <VFX/Authoring/VfxAuthoring.h>
#include <VFX/CpuEmitter.h>
#include <VFXCompiler/VfxStackCompiler.h>
#include <Rendering/Context/RhiCommandListExecutor.h>
#include <Rendering/Context/RenderCommandPipe.h>
#include <Rendering/RHI/Backends/RHIDeviceFactory.h>
#include <Rendering/Settings/DriverSettings.h>
#include "Panels/VfxEditor.h"
#include "Panels/VfxLibraryEditor.h"
#include "VFX/Editor/VfxLibraryDocument.h"
#include "VFX/Editor/VfxSession.h"
#include "VFX/Editor/VfxStackEditor.h"
#include "VFX/Editor/VfxInspector.h"
#include "VFX/Editor/VfxSystemPreview.h"
#include "VFX/Editor/VfxPreviewCamera.h"
#include "VFX/Editor/VfxTimelineView.h"
#include "VFX/Editor/VfxClipboard.h"
#include "Assets/AssetDatabaseFacade.h"
#include <Engine/Assets/ParticleSystemAsset.h>
#include "Core/PanelsManager.h"
#include <UI/Modules/Canvas.h>

namespace
{
struct NextTestDirectory
{
    std::filesystem::path root = std::filesystem::temp_directory_path()/("NullusVfxEditor_"+NLS::Guid::New().ToString());
    NextTestDirectory() { std::filesystem::create_directories(root); }
    ~NextTestDirectory() { std::error_code error; std::filesystem::remove_all(root,error); }
};
}
TEST(VfxEditor, DefaultPanelOpensStackAssetsAndRejectsLegacyWithoutChangingFilesOrDocuments)
{
    using namespace NLS;
    NextTestDirectory directory; std::string error;
    const auto path = directory.root/"Created.vfx";
    ASSERT_TRUE(VFX::Authoring::Save(path,VFX::Authoring::CreateSystem("Created from Asset Browser"),error));
    const auto legacy = directory.root/"Legacy.vfx";
    ASSERT_TRUE(VFX::SaveSystem(legacy,VFX::CreateDefaultSystem("Existing source"),error));
    const auto read = [](const auto& file) { std::ifstream input(file,std::ios::binary); return std::string{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()}; };
    const auto original = read(legacy);
    auto* previous = ImGui::GetCurrentContext(); auto* context = ImGui::CreateContext();
    struct Restore { ImGuiContext *current,*previous; ~Restore() { ImGui::DestroyContext(current); ImGui::SetCurrentContext(previous); } } restore{context,previous};
    UI::Canvas canvas; Editor::Core::PanelsManager panels(canvas); UI::PanelWindowSettings settings;
    panels.CreatePanel<Editor::Panels::VfxEditor>("Particle Editor",false,settings,directory.root/"Cache");
    auto& panel = panels.GetPanelAs<Editor::Panels::VfxEditor>("Particle Editor");
    ASSERT_TRUE(panel.OpenDocument(path)); EXPECT_TRUE(panel.IsOpened()); ASSERT_EQ(panel.DocumentCount(),1u);
    ASSERT_TRUE(panel.OpenDocument(path)); EXPECT_EQ(panel.DocumentCount(),1u);
    panel.SetOpened(false); EXPECT_FALSE(panel.OpenDocument(legacy)); EXPECT_TRUE(panel.IsOpened()); EXPECT_EQ(panel.DocumentCount(),1u);
    EXPECT_EQ(read(legacy),original); EXPECT_TRUE(panels.HasPanel("Particle Editor"));
}
TEST(VfxEditor, IndependentDocumentsDockingDpiAndHideRestore)
{
    NextTestDirectory directory;
    auto first = NLS::VFX::Authoring::CreateSystem("First"), second = NLS::VFX::Authoring::CreateSystem("Second"); std::string error;
    first.timing = {true,0.125,2,0.25};
    first.emitters[0].settings.target = NLS::VFX::SimulationTarget::GPU;
    auto stage = NLS::VFX::Authoring::CreateSimulationStage("Editor Iteration"); stage.iterations = 3; stage.modules.push_back(NLS::VFX::Authoring::CreateModule("ScaleSize")); first.emitters[0].stacks.push_back(stage);
    first.parameters.push_back({NLS::Guid::New().ToString(),"User.StageEnabled",NLS::VFX::ValueType::Bool,NLS::VFX::BoolValue(true)});
    first.parameters.push_back({NLS::Guid::New().ToString(),"User.StageIterations",NLS::VFX::ValueType::Int32,NLS::VFX::IntValue(3)});
    first.emitters[0].stacks.back().bindings = {"User.StageEnabled","User.StageIterations"};
    ASSERT_TRUE(NLS::VFX::Authoring::Save(directory.root/"First.vfx",first,error));
    ASSERT_TRUE(NLS::VFX::Authoring::Save(directory.root/"Second.vfx",second,error));
    auto* previous = ImGui::GetCurrentContext(); auto* context = ImGui::CreateContext();
    struct Restore { ImGuiContext *context,*previous; ~Restore() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); } } restore{context,previous};
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = {1600,1100}; io.DeltaTime = 1.0f/60;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    NLS::UI::PanelWindowSettings settings;
    NLS::Editor::Panels::VfxEditor panel("Particle Editor Test",true,settings,directory.root/"Cache");
    ASSERT_TRUE(panel.OpenDocument(directory.root/"First.vfx")); ASSERT_TRUE(panel.OpenDocument(directory.root/"Second.vfx"));
    ASSERT_TRUE(panel.OpenDocument(directory.root/"First.vfx")); EXPECT_EQ(panel.DocumentCount(),2u);
    const auto saved = NLS::VFX::Authoring::Serialize(first);
    for (const float scale : {1.0f,1.5f,2.0f})
    {
        io.FontGlobalScale = scale;
        for (int frame = 0; frame < 5; ++frame)
        {
            panel.SetOpened(frame != 2); ImGui::NewFrame(); ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1500,1000});
            panel.Draw(); ImGui::Render();
            if (frame == 4) EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount,1000);
        }
    }
    NLS::VFX::Authoring::System unchanged;
    ASSERT_TRUE(NLS::VFX::Authoring::Load(directory.root/"First.vfx",unchanged,error)); EXPECT_EQ(saved,NLS::VFX::Authoring::Serialize(unchanged));
}

TEST(VfxEditor, Dx12SystemPreviewRendersAllCpuAndGpuEmittersAndSeekFrames)
{
#if !defined(_WIN32)
    GTEST_SKIP() << "DX12 device validation requires Windows";
#else
    using namespace NLS;
    namespace author = VFX::Authoring;
    namespace context = Render::Context;
    auto& executor = context::RhiCommandListExecutor::Get();
    if (executor.IsInitialized()) GTEST_SKIP() << "Another fixture owns the RHI executor";
    executor.Initialize(context::RhiThreadMode::None,{});
    struct Shutdown { context::RhiCommandListExecutor& executor; ~Shutdown() { executor.Shutdown(); } } shutdown{executor};
    Render::Settings::DriverSettings settings; settings.graphicsBackend = Render::Settings::EGraphicsBackend::DX12; settings.debugMode = true;
    auto device = Render::Backend::CreateRhiDevice(settings);
    if (!device || !device->IsBackendReady()) GTEST_SKIP() << "DX12 device unavailable";
    executor.Initialize(context::RhiThreadMode::None,context::RhiCommandListExecutor::CreateDeviceTargets(device));
    NextTestDirectory directory;
    Editor::VFXTools::VfxSession session;
    auto system = author::CreateSystem(); system.emitters.push_back(author::CreateEmitter("GPU")); system.emitters[1].settings.target = VFX::SimulationTarget::GPU;
    system.timing = {true,0.03125,2,0.0625};
    for (size_t i = 0; i < 2; ++i)
    {
        auto& emitter = system.emitters[i]; emitter.settings.capacity = 8;
        auto* spawn = author::FindStack(emitter,author::Stage::ParticleSpawn); spawn->modules.resize(1);
        spawn->modules[0].inputs["Position"].value = {i ? 0.6f : -0.6f,0.5f,0,0};
        spawn->modules[0].inputs["Velocity"].value = {}; spawn->modules[0].inputs["Color"].value = i ? VFX::Value{0,1,0,1} : VFX::Value{1,0,0,1};
        spawn->modules[0].inputs["Size"].value = {0.6f,0,0,0};
        auto* schedule = author::FindStack(emitter,author::Stage::EmitterUpdate); schedule->modules[1].enabled = false;
        auto burst = author::CreateModule("SpawnBurst"); burst.inputs["Count"].value[0] = 1; schedule->modules.push_back(std::move(burst));
    }
    // Exercise a pinned module -> function call and emitter inheritance through
    // the actual CPU/GPU preview, not only HLSL generation.
    auto function = author::CreateScript(author::AssetKind::Function,"Preview Size");
    auto& functionVersion = function.versions[0]; functionVersion.inputs[0].defaultValue[0] = 0.6f;
    functionVersion.inputs.push_back({Guid::New().ToString(),"UseValue",VFX::ValueType::Float,{1,0,0,0},true});
    const auto unavailable = VFX::AddNode(functionVersion.graph,VFX::Operation::Attribute); VFX::FindNode(functionVersion.graph,unavailable)->name = "Particles.Unavailable";
    const auto branch = VFX::AddNode(functionVersion.graph,VFX::Operation::Parameter);
    functionVersion.switches.push_back({branch,1,unavailable,"UseValue"}); VFX::FindNode(functionVersion.graph,2)->inputs = {branch};
    auto module = author::CreateScript(author::AssetKind::Module,"Preview Size Module");
    module.versions[0].calls.push_back({3,{function.id,function.exposedVersion},"Output",{}});
    const auto callNode = VFX::AddNode(module.versions[0].graph,VFX::Operation::Parameter);
    ASSERT_EQ(callNode,3u); VFX::FindNode(module.versions[0].graph,2)->inputs = {callNode};
    author::SourceLibrary library; std::string libraryError;
    ASSERT_TRUE(library.Add(function,{},libraryError)); ASSERT_TRUE(library.Add(module,{},libraryError));
    for (auto& emitter : system.emitters) author::FindStack(emitter,author::Stage::ParticleSpawn)->modules.push_back(author::InstantiateModule(module));
    auto inherited = author::CreateEmitterSource("Preview GPU Parent"); inherited.versions[0].emitter = system.emitters[1]; ASSERT_TRUE(library.Add(inherited,{},libraryError));
    system.emitters[1] = author::InstantiateEmitter(inherited);
    session.SetLibrary(std::move(library));
    ASSERT_TRUE(session.document.Apply(system,"Mixed preview")); ASSERT_TRUE(session.Rebuild()); ASSERT_TRUE(session.Seek(0.15));
    EXPECT_DOUBLE_EQ(session.Instance().Age(),0.125); EXPECT_DOUBLE_EQ(session.Instance().Capture()->simulationAge,0.25);
    Editor::VFXTools::VfxSystemPreview preview; Editor::VFXTools::VfxSystemPreviewInput input; input.width = 128; input.height = 128;
    struct Drain
    {
        Editor::VFXTools::VfxSystemPreview& preview;
        ~Drain() { preview.Clear(); context::RenderCommandDispatcher::FlushToDrain(); (void)context::RhiCommandListExecutor::Get().Flush(context::RhiFlushFlags::Gpu|context::RhiFlushFlags::ReleaseResources); }
    } drain{preview};
    input.camera.viewProjection[10] = 0.01f; input.camera.viewProjection[11] = 0.5f;
    preview.Update(device,session.Program(),session.TakeFrames(),directory.root,input);
    std::shared_ptr<Render::RHI::RHITextureView> view;
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(45);
    while (std::chrono::steady_clock::now() < deadline)
    {
        preview.Update(device,session.Program(),{},directory.root,input); context::RenderCommandDispatcher::Dispatch(); view = preview.View();
        if (view) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(view) << preview.Status();
    std::vector<uint8_t> pixels(128*128*4);
    auto read = device->BeginReadPixels(view->GetTexture(),0,0,128,128,Render::Settings::EPixelDataFormat::RGBA,Render::Settings::EPixelDataType::UNSIGNED_BYTE,pixels.data());
    ASSERT_TRUE(read.Succeeded()) << read.message;
    if (read.completion) ASSERT_TRUE(read.completion->Wait(30000000000ull).Succeeded());
    uint32_t red = 0,green = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) { red += pixels[i] > 180 && pixels[i+1] < 50; green += pixels[i+1] > 180 && pixels[i] < 50; }
    EXPECT_GT(red,20u); EXPECT_GT(green,20u);
    // Resize and paused view changes keep the previous published image alive.
    input.width = 96; input.height = 64; input.background = {0,0,1,1};
    const auto resizeDeadline = std::chrono::steady_clock::now()+std::chrono::seconds(30);
    std::shared_ptr<Render::RHI::RHITextureView> resized;
    while (std::chrono::steady_clock::now() < resizeDeadline)
    {
        preview.Update(device,session.Program(),{},directory.root,input); context::RenderCommandDispatcher::Dispatch(); resized = preview.View();
        if (resized && resized != view && resized->GetTexture()->GetDesc().extent.width == 96) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(resized) << preview.Status(); ASSERT_NE(resized,view); EXPECT_EQ(resized->GetTexture()->GetDesc().extent.height,64u);
    std::array<uint8_t,4> background{};
    auto backgroundRead = device->BeginReadPixels(resized->GetTexture(),0,0,1,1,Render::Settings::EPixelDataFormat::RGBA,Render::Settings::EPixelDataType::UNSIGNED_BYTE,background.data());
    ASSERT_TRUE(backgroundRead.Succeeded()); if (backgroundRead.completion) ASSERT_TRUE(backgroundRead.completion->Wait(30000000000ull).Succeeded());
    EXPECT_GT(background[2],240); EXPECT_LT(background[0],10);
    std::vector<uint8_t> retained(pixels.size());
    auto retainedRead = device->BeginReadPixels(view->GetTexture(),0,0,128,128,Render::Settings::EPixelDataFormat::RGBA,Render::Settings::EPixelDataType::UNSIGNED_BYTE,retained.data());
    ASSERT_TRUE(retainedRead.Succeeded()); if (retainedRead.completion) ASSERT_TRUE(retainedRead.completion->Wait(30000000000ull).Succeeded());
    EXPECT_EQ(retained,pixels);
    preview.Clear(); context::RenderCommandDispatcher::FlushToDrain();
    (void)executor.Flush(context::RhiFlushFlags::Gpu|context::RhiFlushFlags::ReleaseResources);
#endif
}

TEST(VfxEditor, VersionTwoImportsIntoRuntimeArtifactAndFailureRetainsLastProgram)
{
    using namespace NLS;
    NextTestDirectory directory; std::filesystem::create_directories(directory.root/"Assets");
    auto system = VFX::Authoring::CreateSystem("Imported Stack"); std::string error;
    const auto path = directory.root/"Assets/Stack.vfx";
    ASSERT_TRUE(VFX::Authoring::Save(path,system,error));
    Editor::Assets::AssetDatabaseFacade database(Editor::Assets::MakeProjectEditorAssetRoots(directory.root));
    ASSERT_TRUE(database.Refresh());
    const auto imported = database.ImportAsset("Assets/Stack.vfx");
    if (!imported) for (const auto& diagnostic : database.GetDiagnostics()) ADD_FAILURE() << diagnostic.message;
    ASSERT_TRUE(imported);
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/Stack.vfx"); ASSERT_TRUE(manifest);
    const auto artifact = database.ResolveArtifactPathAtPath("Assets/Stack.vfx",manifest->primarySubAssetKey);
    Engine::Assets::ParticleSystemAsset runtime;
    std::filesystem::rename(path,directory.root/"Source.hidden");
    ASSERT_TRUE(runtime.Reload(artifact,manifest->FindPrimaryArtifact()->contentHash,error)) << error;
    ASSERT_TRUE(runtime.Program()); EXPECT_EQ(runtime.Program()->asset.name,"Imported Stack"); EXPECT_EQ(runtime.Program()->platforms.size(),3u);
    const auto previous = manifest->FindPrimaryArtifact()->contentHash;
    std::filesystem::rename(directory.root/"Source.hidden",path);
    system.emitters.front().stacks.front().modules.push_back(VFX::Authoring::CreateModule("MissingScript"));
    ASSERT_TRUE(VFX::Authoring::Save(path,system,error)); EXPECT_FALSE(database.ImportAsset("Assets/Stack.vfx"));
    const auto retained = database.GetArtifactManifestForAssetPath("Assets/Stack.vfx"); ASSERT_TRUE(retained);
    EXPECT_EQ(previous,retained->FindPrimaryArtifact()->contentHash); EXPECT_TRUE(std::filesystem::exists(artifact));
}
TEST(VfxLibraryEditor, SourceDocumentsPreserveVersionsAndUndoContinuousGraphChanges)
{
    using namespace NLS; NextTestDirectory directory; Editor::Undo::EditorUndoService undo;
    Editor::VFXTools::VfxLibraryDocument document(&undo); auto asset = document.Asset(); const auto before = Editor::VFXTools::SerializeLibraryAsset(asset);
    ASSERT_TRUE(document.BeginEdit("Graph value drag"));
    for (int i = 1; i <= 4; ++i) { asset.script.versions[0].graph.nodes[0].value[0] = float(i); ASSERT_TRUE(document.PreviewEdit(asset)); }
    ASSERT_TRUE(document.EndEdit()); const auto after = Editor::VFXTools::SerializeLibraryAsset(document.Asset());
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(before,Editor::VFXTools::SerializeLibraryAsset(document.Asset())); EXPECT_FALSE(undo.CanUndo());
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(after,Editor::VFXTools::SerializeLibraryAsset(document.Asset()));
    ASSERT_TRUE(document.Save(directory.root/"Module.vfxscript")); EXPECT_FALSE(document.IsDirty());
    Editor::VFXTools::VfxLibraryDocument reopened; ASSERT_TRUE(reopened.Load(directory.root/"Module.vfxscript")); EXPECT_EQ(after,Editor::VFXTools::SerializeLibraryAsset(reopened.Asset()));
    auto* previous = ImGui::GetCurrentContext(); auto* context = ImGui::CreateContext();
    struct Restore { ImGuiContext *context,*previous; ~Restore() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); } } restore{context,previous};
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1400,950}; io.DeltaTime = 1.0f/60;
    unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    UI::PanelWindowSettings settings; Editor::Panels::VfxLibraryEditor panel("Library Test",true,settings);
    ASSERT_TRUE(panel.OpenDocument(directory.root/"Module.vfxscript"));
    for (int i = 0; i < 5; ++i) { ImGui::NewFrame(); ImGui::SetNextWindowSize({1300,850}); panel.Draw(); ImGui::Render(); }
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount,1000);
    auto emitter = VFX::Authoring::CreateEmitterSource("Controlled Emitter"); emitter.versions[0].emitter.settings.target = VFX::SimulationTarget::GPU;
    auto stage = VFX::Authoring::CreateSimulationStage(); stage.bindings = {"User.Enabled","User.Iterations"}; stage.modules.push_back(VFX::Authoring::CreateModule("ScaleSize")); emitter.versions[0].emitter.stacks.push_back(stage);
    std::string error; const auto emitterPath = directory.root/"Emitter.vfxemitter"; ASSERT_TRUE(VFX::Authoring::SaveEmitterSource(emitterPath,emitter,error));
    Editor::Panels::VfxLibraryEditor emitterPanel("Emitter Library Test",true,settings); ASSERT_TRUE(emitterPanel.OpenDocument(emitterPath));
    for (int i = 0; i < 5; ++i) { ImGui::NewFrame(); ImGui::SetNextWindowSize({1300,850}); emitterPanel.Draw(); ImGui::Render(); }
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount,1000);
}
TEST(VfxLibraryEditor, ImportedLibrariesInvalidateDependentsAndRuntimeLoadsWithoutAnySource)
{
    using namespace NLS; namespace author = VFX::Authoring; NextTestDirectory directory; std::filesystem::create_directories(directory.root/"Assets"); std::string error;
    auto script = author::CreateScript(author::AssetKind::Module,"Library Size");
    const auto scriptPath = directory.root/"Assets/Size.vfxscript"; ASSERT_TRUE(author::SaveScript(scriptPath,script,error));
    auto emitter = author::CreateEmitterSource("Library Emitter"); author::FindStack(emitter.versions[0].emitter,author::Stage::ParticleSpawn)->modules.push_back(author::InstantiateModule(script));
    ASSERT_TRUE(author::SaveEmitterSource(directory.root/"Assets/Emitter.vfxemitter",emitter,error));
    auto system = author::CreateSystem(); system.emitters = {author::InstantiateEmitter(emitter)}; ASSERT_TRUE(author::Save(directory.root/"Assets/System.vfx",system,error));
    Editor::Assets::AssetDatabaseFacade database(Editor::Assets::MakeProjectEditorAssetRoots(directory.root)); ASSERT_TRUE(database.Refresh());
    const auto import = [&](const char* path) { const bool result = database.ImportAsset(path); if (!result) for (const auto& issue : database.GetDiagnostics()) ADD_FAILURE() << issue.message; return result; };
    ASSERT_TRUE(import("Assets/Size.vfxscript")); ASSERT_TRUE(import("Assets/Emitter.vfxemitter")); ASSERT_TRUE(import("Assets/System.vfx"));
    const auto libraryManifest = database.GetArtifactManifestForAssetPath("Assets/Size.vfxscript"); ASSERT_TRUE(libraryManifest); EXPECT_EQ(libraryManifest->FindPrimaryArtifact()->artifactType,Core::Assets::ArtifactType::VfxScript);
    const auto manifest = database.GetArtifactManifestForAssetPath("Assets/System.vfx"); ASSERT_TRUE(manifest); EXPECT_TRUE(database.IsArtifactManifestCurrentForAssetPath("Assets/System.vfx"));
    EXPECT_GE(manifest->dependencies.size(),2u);
    const auto artifact = database.ResolveArtifactPathAtPath("Assets/System.vfx",manifest->primarySubAssetKey); const auto before = manifest->FindPrimaryArtifact()->contentHash;
    script.versions[0].inputs[0].defaultValue[0] = 2; VFX::FindNode(script.versions[0].graph,1)->value[0] = 2; ASSERT_TRUE(author::SaveScript(scriptPath,script,error));
    EXPECT_FALSE(database.IsArtifactManifestCurrentForAssetPath("Assets/System.vfx"));
    // Selected module arguments remain instance overrides, but source changes
    // must still invalidate and revalidate the dependent system.
    ASSERT_TRUE(import("Assets/System.vfx"));
    std::filesystem::rename(directory.root/"Assets",directory.root/"Assets.hidden"); Engine::Assets::ParticleSystemAsset runtime;
    ASSERT_TRUE(runtime.Reload(artifact,before,error)) << error; ASSERT_TRUE(runtime.Program()); EXPECT_EQ(runtime.Program()->asset.emitters.size(),1u);
}

namespace
{
struct EmitterStackGui
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    EmitterStackGui()
    {
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1200,1000}; io.DeltaTime = 1.0f/60;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels; int width,height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    }
    ~EmitterStackGui() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
    void Frame(const std::function<void()>& draw)
    {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({800,800});
        ImGui::Begin("Emitter stack interaction",nullptr,ImGuiWindowFlags_NoDecoration);
        draw(); ImGui::End(); ImGui::Render();
    }
    void Click(ImVec2 point,const std::function<void()>& draw)
    {
        auto& io = ImGui::GetIO(); io.AddMousePosEvent(point.x,point.y); Frame(draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left,true); Frame(draw);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left,false); Frame(draw);
    }
};
}

TEST(VfxEmitterPanel, ModuleMenuFiltersStageSearchAndLibraryVisibility)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    author::SourceLibrary library; std::string error;
    auto module = author::CreateScript(author::AssetKind::Module,"Custom Brightness");
    module.category = "Color"; module.description = "Scales the visible brightness"; module.versions[0].stages = {author::Stage::ParticleUpdate};
    ASSERT_TRUE(library.Add(module,{},error));
    auto hidden = module; hidden.id = Guid::New().ToString(); hidden.name = "Hidden Brightness"; hidden.visible = false; ASSERT_TRUE(library.Add(hidden,{},error));
    auto deprecated = module; deprecated.id = Guid::New().ToString(); deprecated.name = "Old Brightness"; deprecated.deprecated = true; ASSERT_TRUE(library.Add(deprecated,{},error));
    auto function = author::CreateScript(author::AssetKind::Function,"Brightness Function"); ASSERT_TRUE(library.Add(function,{},error));
    const auto normal = FindVfxModules(library,author::Stage::ParticleUpdate,"BRIGHTNESS color");
    ASSERT_EQ(normal.size(),1u); EXPECT_EQ(normal[0].id,module.id); EXPECT_TRUE(normal[0].asset);
    EXPECT_EQ(FindVfxModules(library,author::Stage::ParticleUpdate,"brightness",true).size(),2u);
    EXPECT_TRUE(FindVfxModules(library,author::Stage::ParticleSpawn,"brightness").empty());
    for (const auto& choice : FindVfxModules(library,author::Stage::ParticleSpawn)) if (!choice.asset)
        ASSERT_TRUE(author::SupportsStage(*author::FindModule(choice.id),author::Stage::ParticleSpawn));
}

TEST(VfxEmitterPanel, AddEnableAndUndoUseTheDocumentAndRetainTheLastGoodProgram)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    Editor::Undo::EditorUndoService undo; VfxSession session(&undo); ASSERT_TRUE(session.Rebuild());
    const auto emitter = session.document.Asset().emitters[0].settings.id;
    const auto stack = author::FindStack(const_cast<author::Emitter&>(session.document.Asset().emitters[0]),author::Stage::ParticleUpdate)->id;
    const auto before = author::Serialize(session.document.Asset());
    EXPECT_FALSE(session.AddModule(emitter,stack,"InitializeParticle")); EXPECT_EQ(before,author::Serialize(session.document.Asset()));
    ASSERT_TRUE(session.AddModule(emitter,stack,"ScaleSize")) << session.ActionError();
    const auto module = session.selectedModule; EXPECT_EQ(session.selection,VfxSelection::Module); ASSERT_FALSE(module.empty());
    ASSERT_TRUE(session.Rebuild()); const auto good = session.Program();
    ASSERT_TRUE(session.SetModuleEnabled(emitter,module,false)); EXPECT_EQ(session.selectedModule,module);
    const auto& disabled = session.EditingSystem().emitters[0];
    bool foundDisabled = false; for (const auto& item : disabled.stacks) for (const auto& entry : item.modules) if (entry.id == module) foundDisabled = !entry.enabled;
    EXPECT_TRUE(foundDisabled);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); ASSERT_TRUE(session.Rebuild());
    bool foundEnabled = false; for (const auto& item : session.EditingSystem().emitters[0].stacks) for (const auto& entry : item.modules) if (entry.id == module) foundEnabled = entry.enabled;
    EXPECT_TRUE(foundEnabled); EXPECT_EQ(session.selectedModule,module);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); session.ReconcileSelection();
    EXPECT_EQ(session.selection,VfxSelection::Stack); EXPECT_TRUE(session.selectedModule.empty()); EXPECT_EQ(session.selectedStack,stack);
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); session.SelectModule(emitter,stack,module); ASSERT_TRUE(session.Rebuild());
    const auto generation = session.ProgramGeneration(); const auto running = session.Program();
    auto broken = session.document.Asset(); author::FindStack(broken.emitters[0],author::Stage::ParticleUpdate)->modules.push_back(author::CreateModule("MissingModule"));
    ASSERT_TRUE(session.document.Apply(broken,"Broken module")); EXPECT_FALSE(session.Rebuild()); EXPECT_EQ(session.Program(),running);
    ASSERT_TRUE(session.SetModuleEnabled(emitter,module,false)); EXPECT_FALSE(session.Rebuild()); EXPECT_EQ(session.ProgramGeneration(),generation);
    bool currentDisabled = false; for (const auto& item : session.EditingSystem().emitters[0].stacks) for (const auto& entry : item.modules) if (entry.id == module) currentDisabled = !entry.enabled;
    EXPECT_TRUE(currentDisabled); EXPECT_EQ(session.selectedModule,module);
}

TEST(VfxEmitterPanel, InheritedAddAndEnableOverrideTheInstanceAndKeepStableSelection)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    Editor::Undo::EditorUndoService undo; VfxSession session(&undo); author::SourceLibrary library; std::string error;
    auto parent = author::CreateEmitterSource("Parent"); const auto parentBefore = author::SerializeEmitterSource(parent);
    ASSERT_TRUE(library.Add(parent,{},error)); session.SetLibrary(library);
    auto system = author::CreateSystem(); system.emitters = {author::InstantiateEmitter(parent)}; ASSERT_TRUE(session.document.Apply(system,"Child"));
    const auto emitter = system.emitters[0].settings.id;
    const auto& resolved = session.EditingSystem().emitters[0];
    const auto spawn = std::find_if(resolved.stacks.begin(),resolved.stacks.end(),[](const auto& item) { return item.stage == author::Stage::ParticleSpawn; }); ASSERT_NE(spawn,resolved.stacks.end());
    const auto scopedModule = spawn->modules.back().id, module = std::string(VfxSourceId(spawn->modules.back())), scopedStack = spawn->id;
    session.SelectModule(emitter,scopedStack,scopedModule); EXPECT_EQ(session.selectedModule,module); EXPECT_EQ(session.selectedStack,VfxSourceId(*spawn));
    ASSERT_TRUE(session.SetModuleEnabled(emitter,scopedModule,false)) << session.ActionError();
    ASSERT_EQ(session.document.Asset().emitters[0].overrides.size(),1u); EXPECT_EQ(session.document.Asset().emitters[0].overrides[0].module,module);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_TRUE(session.document.Asset().emitters[0].overrides.empty());
    const auto& updateView = session.EditingSystem().emitters[0]; const auto update = std::find_if(updateView.stacks.begin(),updateView.stacks.end(),[](const auto& item) { return item.stage == author::Stage::ParticleUpdate; }); ASSERT_NE(update,updateView.stacks.end());
    const auto updateId = std::string(VfxSourceId(*update));
    ASSERT_TRUE(session.AddModule(emitter,update->id,"ScaleSize")); const auto added = session.selectedModule;
    session.ReconcileSelection(); EXPECT_EQ(session.selectedModule,added); EXPECT_EQ(session.selectedStack,updateId);
    ASSERT_TRUE(session.Rebuild()); EXPECT_EQ(session.selectedModule,added);
    ASSERT_TRUE(session.SetModuleEnabled(emitter,added,false)); EXPECT_EQ(session.document.Asset().emitters[0].overrides.size(),0u);
    EXPECT_EQ(author::SerializeEmitterSource(*session.Library().EmitterAsset(parent.id)),parentBefore);
    NextTestDirectory directory; ASSERT_TRUE(session.document.Save(directory.root/"Child.vfx"));
    VfxSession reopened; reopened.SetLibrary(library); ASSERT_TRUE(reopened.document.Load(directory.root/"Child.vfx")); ASSERT_TRUE(reopened.Rebuild());
    bool disabled = false; for (const auto& item : reopened.EditingSystem().emitters[0].stacks) for (const auto& entry : item.modules) if (VfxSourceId(entry) == added) disabled = !entry.enabled;
    EXPECT_TRUE(disabled);
}

TEST(VfxEmitterPanel, WorkingSelectionDoesNotRecompileAndDocumentReplacementInvalidatesTheView)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    VfxSession session; ASSERT_TRUE(session.Rebuild()); const auto revision = session.document.Revision(), generation = session.ProgramGeneration();
    const auto& emitter = session.EditingSystem().emitters[0];
    const auto stack = std::find_if(emitter.stacks.begin(),emitter.stacks.end(),[](const auto& item) { return !item.modules.empty(); }); ASSERT_NE(stack,emitter.stacks.end());
    session.SelectModule(emitter.settings.id,stack->id,stack->modules[0].id); ASSERT_TRUE(session.Rebuild());
    EXPECT_EQ(session.document.Revision(),revision); EXPECT_EQ(session.ProgramGeneration(),generation);
    session.SelectSystem(); EXPECT_EQ(session.selection,VfxSelection::System); ASSERT_TRUE(session.Rebuild()); EXPECT_TRUE(session.selectedEmitter.empty());
    const auto old = session.EditingSystem().id; session.document.New("Replacement"); EXPECT_NE(session.EditingSystem().id,old); EXPECT_EQ(session.EditingSystem().name,"Replacement");
}

TEST(VfxEmitterPanel, DisabledModuleRowsRemainSelectableAndEnableFromTheirOwnCheckboxAtEveryScale)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    EmitterStackGui gui; VfxSession session;
    const auto emitterId = session.document.Asset().emitters[0].settings.id;
    auto source = session.document.Asset(); const auto* spawn = author::FindStack(source.emitters[0],author::Stage::ParticleSpawn);
    const auto stackId = spawn->id, moduleId = spawn->modules.back().id;
    for (const float scale : {1.0f,1.5f,2.0f})
    {
        ImGui::GetIO().FontGlobalScale = scale;
        ASSERT_TRUE(session.SetModuleEnabled(emitterId,moduleId,false)); session.SelectEmitter(emitterId);
        ImVec2 origin; float rowHeight = 0;
        const auto draw = [&]
        {
            origin = ImGui::GetCursorScreenPos(); rowHeight = ImGui::GetFontSize()+7*ImGui::GetFontSize()/13;
            ImGui::PushID(moduleId.c_str()); VfxStackRow row; row.label = "Box Location"; row.width = 300*scale; row.enable = true;
            for (const auto& stack : session.EditingSystem().emitters[0].stacks) for (const auto& module : stack.modules) if (module.id == moduleId) row.enabled = module.enabled;
            row.selected = session.selectedModule == moduleId; const auto result = DrawVfxStackRow(row);
            if (result.selected) session.SelectModule(emitterId,stackId,moduleId);
            if (result.enabled) ASSERT_TRUE(session.SetModuleEnabled(emitterId,moduleId,!row.enabled));
            ImGui::PopID();
        };
        gui.Frame(draw); gui.Frame(draw); const auto revision = session.document.Revision();
        gui.Click({origin.x+90*scale,origin.y+rowHeight/2},draw);
        EXPECT_EQ(session.selectedModule,moduleId); EXPECT_EQ(session.document.Revision(),revision);
        gui.Click({origin.x+300*scale-rowHeight/2,origin.y+rowHeight/2},draw);
        bool enabled = false; for (const auto& stack : session.EditingSystem().emitters[0].stacks) for (const auto& module : stack.modules) if (module.id == moduleId) enabled = module.enabled;
        EXPECT_TRUE(enabled); EXPECT_EQ(session.selectedModule,moduleId);
    }
}

TEST(VfxEmitterPanel, StagePlusOpensTheSearchPickerAndEnterAddsAndSelectsTheModule)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    EmitterStackGui gui; VfxSession session; VfxModulePicker picker;
    auto source = session.document.Asset(); const auto emitterId = source.emitters[0].settings.id;
    const auto stackId = author::FindStack(source.emitters[0],author::Stage::ParticleUpdate)->id;
    const auto before = author::FindStack(source.emitters[0],author::Stage::ParticleUpdate)->modules.size();
    ImVec2 origin; float height = 0;
    const auto draw = [&]
    {
        origin = ImGui::GetCursorScreenPos(); height = ImGui::GetFontSize()+10*ImGui::GetFontSize()/13;
        VfxStackRow row; row.label = "Particle Update"; row.width = 300; row.header = true; row.add = true; row.expandable = true;
        if (DrawVfxStackRow(row).add) picker.Open(emitterId,stackId);
        picker.Draw(session);
    };
    gui.Frame(draw); gui.Frame(draw); gui.Click({origin.x+300-height/2,origin.y+height/2},draw);
    gui.Frame(draw); gui.Frame(draw);
    auto& io = ImGui::GetIO(); io.AddInputCharactersUTF8("kill particles"); gui.Frame(draw); gui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_Enter,true); gui.Frame(draw); io.AddKeyEvent(ImGuiKey_Enter,false); gui.Frame(draw);
    auto result = session.document.Asset(); const auto* stack = author::FindStack(result.emitters[0],author::Stage::ParticleUpdate);
    ASSERT_EQ(stack->modules.size(),before+1); EXPECT_EQ(stack->modules.back().script,"KillParticles");
    EXPECT_EQ(session.selectedModule,stack->modules.back().id); EXPECT_EQ(session.selection,VfxSelection::Module); ASSERT_TRUE(session.Rebuild());
}
TEST(VfxEmitterPanel, SharedInspectorEditsInheritedInputsWithoutChangingTheirBindingModeOrParent)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    Editor::Undo::EditorUndoService undo; VfxSession session(&undo); author::SourceLibrary library; std::string error;
    auto parent = author::CreateEmitterSource("Input Parent"); ASSERT_TRUE(library.Add(parent,{},error)); session.SetLibrary(library);
    auto system = author::CreateSystem(); system.emitters = {author::InstantiateEmitter(parent)};
    system.parameters.push_back({Guid::New().ToString(),"User.Size",VFX::ValueType::Float,{0.25f,0,0,0}});
    ASSERT_TRUE(session.document.Apply(system,"Child")); ASSERT_TRUE(session.Rebuild());
    const auto& emitterView = session.EditingSystem().emitters[0];
    const auto stack = std::find_if(emitterView.stacks.begin(),emitterView.stacks.end(),[](const auto& item) { return item.stage == author::Stage::ParticleSpawn; }); ASSERT_NE(stack,emitterView.stacks.end());
    const auto module = std::find_if(stack->modules.begin(),stack->modules.end(),[](const auto& item) { return item.script == "InitializeParticle"; }); ASSERT_NE(module,stack->modules.end());
    const auto target = *module; const auto source = author::SerializeEmitterSource(*session.Library().EmitterAsset(parent.id));
    auto candidate = session.document.Asset(); auto input = target.inputs.at("Size");
    input.mode = author::InputMode::Parameter; input.parameter = "User.Size";
    ASSERT_TRUE(StoreVfxModuleInput(candidate.emitters[0],target,"Size",input));
    Editor::Inspector::ReflectedPropertyEdit edit; edit.changed = true;
    ASSERT_TRUE(CommitInspectorEdit(session.document,candidate,edit,"Bind inherited input"));
    edit.changed = false; edit.completed = true;
    ASSERT_TRUE(CommitInspectorEdit(session.document,candidate,edit,"Bind inherited input"));
    ASSERT_TRUE(session.Rebuild());
    const auto& override = session.document.Asset().emitters[0].overrides[0].inputs.at("Size");
    EXPECT_EQ(override.mode,author::InputMode::Parameter); EXPECT_EQ(override.parameter,"User.Size");
    EXPECT_EQ(author::SerializeEmitterSource(*session.Library().EmitterAsset(parent.id)),source);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_TRUE(session.document.Asset().emitters[0].overrides.empty());
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(session.document.Asset().emitters[0].overrides[0].inputs.at("Size").parameter,"User.Size");
    NextTestDirectory directory; ASSERT_TRUE(session.document.Save(directory.root/"BoundChild.vfx"));
    VfxSession reopened; reopened.SetLibrary(library); ASSERT_TRUE(reopened.document.Load(directory.root/"BoundChild.vfx")); ASSERT_TRUE(reopened.Rebuild());
    EXPECT_EQ(reopened.document.Asset().emitters[0].overrides[0].inputs.at("Size").mode,author::InputMode::Parameter);
}
TEST(VfxEmitterPanel, ResetVfxModuleInputClearsInheritedOverride)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    Editor::Undo::EditorUndoService undo; VfxSession session(&undo); author::SourceLibrary library; std::string error;
    auto parent = author::CreateEmitterSource("Reset Parent"); ASSERT_TRUE(library.Add(parent,{},error)); session.SetLibrary(library);
    auto system = author::CreateSystem(); system.emitters = {author::InstantiateEmitter(parent)};
    ASSERT_TRUE(session.document.Apply(system,"Child")); ASSERT_TRUE(session.Rebuild());
    const auto& emitterView = session.EditingSystem().emitters[0];
    const auto stack = std::find_if(emitterView.stacks.begin(),emitterView.stacks.end(),
        [](const auto& item) { return item.stage == author::Stage::ParticleSpawn; });
    ASSERT_NE(stack,emitterView.stacks.end());
    const auto module = std::find_if(stack->modules.begin(),stack->modules.end(),
        [](const auto& item) { return item.script == "InitializeParticle"; });
    ASSERT_NE(module,stack->modules.end());
    const auto target = *module;
    auto candidate = session.document.Asset();
    author::Input bound{author::InputMode::Parameter};
    bound.parameter = "User.Size";
    ASSERT_TRUE(StoreVfxModuleInput(candidate.emitters[0],target,"Size",bound));
    ASSERT_EQ(candidate.emitters[0].overrides.size(),1u);
    EXPECT_TRUE(candidate.emitters[0].overrides[0].inputs.contains("Size"));
    author::Input fallback{author::InputMode::Literal,target.inputs.at("Size").value};
    EXPECT_TRUE(ResetVfxModuleInput(candidate.emitters[0],target,"Size",fallback));
    EXPECT_TRUE(candidate.emitters[0].overrides.empty());
    // Local modules reset by writing the literal default; no override is created.
    auto local = author::CreateSystem();
    auto* localStack = author::FindStack(local.emitters[0],author::Stage::ParticleSpawn);
    ASSERT_NE(localStack,nullptr);
    localStack->modules.push_back(author::CreateModule("InitializeParticle"));
    const auto& localModule = localStack->modules.back();
    author::Input changed{author::InputMode::Literal,{5,0,0,0}};
    ASSERT_TRUE(StoreVfxModuleInput(local.emitters[0],localModule,"Size",changed));
    EXPECT_TRUE(ResetVfxModuleInput(local.emitters[0],localModule,"Size",fallback));
    EXPECT_EQ(localStack->modules.back().inputs.at("Size").value,fallback.value);
}

TEST(VfxEmitterPanel, OverviewCanvasKeepsDisabledRowsClickableWithoutDraggingTheEmitter)
{
    using namespace NLS; namespace author = VFX::Authoring; namespace ed = ax::NodeEditor; using namespace Editor::VFXTools;
    EmitterStackGui gui; VfxSession session;
    ed::Config config; config.SettingsFile = nullptr;
    auto* canvas = ed::CreateEditor(&config);
    struct DestroyCanvas { ed::EditorContext* context; ~DestroyCanvas() { ed::SetCurrentEditor(nullptr); ed::DestroyEditor(context); } } cleanup{canvas};
    auto source = session.document.Asset(); const auto emitterId = source.emitters[0].settings.id;
    const auto* stack = author::FindStack(source.emitters[0],author::Stage::ParticleSpawn); const auto stackId = stack->id, moduleId = stack->modules.back().id;
    ASSERT_TRUE(session.SetModuleEnabled(emitterId,moduleId,false));
    ImVec2 selectPoint,enablePoint;
    const auto draw = [&]
    {
        ed::SetCurrentEditor(canvas); ed::Begin("Overview canvas",{700,650}); ed::BeginNode(ed::NodeId(1));
        const auto origin = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetFontSize()+7*ImGui::GetFontSize()/13;
        selectPoint = ed::CanvasToScreen({origin.x+100,origin.y+height/2}); enablePoint = ed::CanvasToScreen({origin.x+300-height/2,origin.y+height/2});
        VfxStackRow row; row.label = "Box Location"; row.width = 300; row.enable = true; row.selected = session.selectedModule == moduleId;
        for (const auto& item : session.EditingSystem().emitters[0].stacks) for (const auto& module : item.modules) if (module.id == moduleId) row.enabled = module.enabled;
        const auto result = DrawVfxStackRow(row);
        if (result.selected) session.SelectModule(emitterId,stackId,moduleId);
        if (result.enabled) ASSERT_TRUE(session.SetModuleEnabled(emitterId,moduleId,!row.enabled));
        ed::EndNode(); ed::End(); ed::SetCurrentEditor(nullptr);
    };
    gui.Frame(draw); gui.Frame(draw); gui.Frame(draw);
    gui.Click(selectPoint,draw); EXPECT_EQ(session.selectedModule,moduleId);
    ed::SetCurrentEditor(canvas); const auto before = ed::GetNodePosition(ed::NodeId(1)); ed::SetCurrentEditor(nullptr);
    gui.Click(enablePoint,draw);
    bool enabled = false; for (const auto& item : session.EditingSystem().emitters[0].stacks) for (const auto& module : item.modules) if (module.id == moduleId) enabled = module.enabled;
    EXPECT_TRUE(enabled); EXPECT_EQ(session.selectedModule,moduleId);
    ed::SetCurrentEditor(canvas); const auto after = ed::GetNodePosition(ed::NodeId(1)); ed::SetCurrentEditor(nullptr);
    EXPECT_FLOAT_EQ(before.x,after.x); EXPECT_FLOAT_EQ(before.y,after.y);
}
namespace
{
// Drives the real Particle Editor panel inside a headless ImGui frame so the
// toolbar, command layer and shortcuts are exercised through their public API.
struct VfxEditorGui
{
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    VfxEditorGui()
    {
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1600,1100}; io.DeltaTime = 1.0f/60;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        unsigned char* pixels; int width, height; io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    }
    ~VfxEditorGui() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
    void Frame(NLS::Editor::Panels::VfxEditor& panel)
    {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1500,1000});
        panel.Draw(); ImGui::Render();
    }
    void Key(NLS::Editor::Panels::VfxEditor& panel,ImGuiKey key,bool control = false)
    {
        auto& io = ImGui::GetIO();
        if (control) io.AddKeyEvent(ImGuiMod_Ctrl,true);
        io.AddKeyEvent(key,true); Frame(panel);
        io.AddKeyEvent(key,false); Frame(panel);
        if (control) io.AddKeyEvent(ImGuiMod_Ctrl,false);
    }
};
}
TEST(VfxEditorCommands, ToolbarCommandsMatchNiagaraBindingsAndTrackDocumentState)
{
    using namespace NLS;
    NextTestDirectory directory; VfxEditorGui gui;
    UI::PanelWindowSettings settings;
    Editor::Panels::VfxEditor panel("Particle Editor Test",true,settings,directory.root/"Cache");
    gui.Frame(panel); gui.Frame(panel);
    ASSERT_EQ(panel.DocumentCount(),1u);
    EXPECT_EQ(panel.ActiveEmitterCount(),1u);
    // File.New opens a second, active document.
    EXPECT_TRUE(panel.ExecuteCommand("File.New"));
    EXPECT_EQ(panel.DocumentCount(),2u);
    gui.Frame(panel); gui.Frame(panel);
    // Preview toggles reflect and flip their checked state.
    EXPECT_TRUE(panel.IsCommandChecked("Preview.Grid"));
    EXPECT_TRUE(panel.ExecuteCommand("Preview.Grid"));
    EXPECT_FALSE(panel.IsCommandChecked("Preview.Grid"));
    EXPECT_TRUE(panel.ExecuteCommand("Preview.Grid"));
    EXPECT_TRUE(panel.IsCommandChecked("Preview.OriginAxis"));
    EXPECT_TRUE(panel.ExecuteCommand("Preview.OriginAxis"));
    EXPECT_FALSE(panel.IsCommandChecked("Preview.OriginAxis"));
    // Playback toggles the shared transport and stays enabled with a program.
    EXPECT_TRUE(panel.IsCommandEnabled("Simulation.Reset"));
    const bool playing = panel.IsCommandChecked("Simulation.PlayPause");
    EXPECT_TRUE(panel.ExecuteCommand("Simulation.PlayPause"));
    EXPECT_EQ(panel.IsCommandChecked("Simulation.PlayPause"),!playing);
    EXPECT_TRUE(panel.ExecuteCommand("Simulation.PlayPause"));
    EXPECT_EQ(panel.IsCommandChecked("Simulation.PlayPause"),playing);
    // Emitter.Add participates in the document undo history.
    const auto before = panel.ActiveEmitterCount();
    EXPECT_TRUE(panel.ExecuteCommand("Emitter.Add"));
    EXPECT_EQ(panel.ActiveEmitterCount(),before+1);
    EXPECT_TRUE(panel.IsCommandEnabled("Edit.Undo"));
    EXPECT_TRUE(panel.ExecuteCommand("Edit.Undo"));
    EXPECT_EQ(panel.ActiveEmitterCount(),before);
    EXPECT_TRUE(panel.ExecuteCommand("Edit.Redo"));
    EXPECT_EQ(panel.ActiveEmitterCount(),before+1);
    EXPECT_EQ(panel.ExecuteCommand("NoSuchCommand"),false);
}
TEST(VfxEditorCommands, ShortcutKeysDriveTheSameCommandsAsTheToolbar)
{
    using namespace NLS;
    NextTestDirectory directory; VfxEditorGui gui;
    UI::PanelWindowSettings settings;
    Editor::Panels::VfxEditor panel("Particle Editor Test",true,settings,directory.root/"Cache");
    // Explicit focus through the frame-safe API so shortcuts see keyboard focus.
    ImGui::NewFrame(); ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1500,1000});
    ImGui::SetNextWindowFocus(); panel.Draw(); ImGui::Render();
    gui.Frame(panel); gui.Frame(panel);
    ASSERT_TRUE(panel.OwnsShortcuts());
    const auto before = panel.ActiveEmitterCount();
    gui.Key(panel,ImGuiKey_E);
    EXPECT_EQ(panel.ActiveEmitterCount(),before+1);
    EXPECT_FALSE(panel.IsCommandChecked("Select.Isolate"));
    gui.Key(panel,ImGuiKey_S);
    EXPECT_TRUE(panel.IsCommandChecked("Select.Isolate"));
    gui.Key(panel,ImGuiKey_S);
    EXPECT_FALSE(panel.IsCommandChecked("Select.Isolate"));
}
TEST(VfxEditorCommands, WindowMenuTogglesWorkspacePanelsAndResetLayoutRestoresThem)
{
    using namespace NLS;
    NextTestDirectory directory; VfxEditorGui gui;
    UI::PanelWindowSettings settings;
    Editor::Panels::VfxEditor panel("Particle Editor Test",true,settings,directory.root/"Cache");
    gui.Frame(panel); gui.Frame(panel);
    for (const char* label : {"Preview","System Overview","Details","Parameters","User Parameters","Local Modules","Timeline","Curves","Niagara Log"})
        EXPECT_TRUE(panel.IsWorkspacePanelVisible(label)) << label;
    EXPECT_FALSE(panel.IsWorkspacePanelVisible("Script Editor"));
    EXPECT_TRUE(panel.SetWorkspacePanel("Curves",false));
    EXPECT_FALSE(panel.IsWorkspacePanelVisible("Curves"));
    EXPECT_TRUE(panel.SetWorkspacePanel("Script Editor",true));
    EXPECT_TRUE(panel.IsWorkspacePanelVisible("Script Editor"));
    gui.Frame(panel); gui.Frame(panel);
    EXPECT_TRUE(panel.IsWorkspacePanelVisible("Script Editor"));
    EXPECT_TRUE(panel.ExecuteCommand("View.ResetLayout"));
    EXPECT_TRUE(panel.IsWorkspacePanelVisible("Curves"));
    EXPECT_FALSE(panel.IsWorkspacePanelVisible("Script Editor"));
    for (int frame = 0; frame < 3; ++frame) gui.Frame(panel);
    EXPECT_EQ(panel.SetWorkspacePanel("Missing Panel",true),false);
}
TEST(VfxPreviewCamera, NativeSceneProjectionAndParticleDepthRangeStayConsistent)
{
    using namespace NLS; using namespace Editor::VFXTools;
    VfxPreviewCamera camera;
    camera.Focus({1,2,3},4);
    const auto matrix = camera.ViewProjection(800,600);
    const auto point = camera.FocusState().focusPoint;
    const float x = matrix[0]*point.x+matrix[1]*point.y+matrix[2]*point.z+matrix[3];
    const float y = matrix[4]*point.x+matrix[5]*point.y+matrix[6]*point.z+matrix[7];
    const float z = matrix[8]*point.x+matrix[9]*point.y+matrix[10]*point.z+matrix[11];
    const float w = matrix[12]*point.x+matrix[13]*point.y+matrix[14]*point.z+matrix[15];
    ASSERT_GT(w,0); EXPECT_NEAR(x/w,0,1e-5f); EXPECT_NEAR(y/w,0,1e-5f);
    EXPECT_GT(z/w,0); EXPECT_LT(z/w,1);
    auto remap = Maths::Matrix4::Identity; remap(2,2) = 0.5f; remap(2,3) = 0.5f;
    const auto reference = remap*camera.Camera().GetProjectionMatrix()*camera.Camera().GetViewMatrix();
    for (size_t index = 0; index < matrix.size(); ++index) EXPECT_FLOAT_EQ(matrix[index],reference.data[index]);
    camera.Camera().SetProjectionMode(Render::Settings::EProjectionMode::ORTHOGRAPHIC);
    const auto ortho = camera.ViewProjection(800,600);
    EXPECT_FLOAT_EQ(ortho[15],1); EXPECT_FLOAT_EQ(ortho[14],0);
    const auto view = camera.MakeView(800,600); const auto context = camera.MakeContext(800,600);
    EXPECT_EQ(view.viewProjection,ortho); EXPECT_EQ(context.forward,view.cameraForward);
    EXPECT_EQ(context.right,view.cameraRight); EXPECT_EQ(context.up,view.cameraUp);
    EXPECT_FLOAT_EQ(context.position[0],camera.Camera().GetPosition().x);
    EXPECT_FLOAT_EQ(context.position[1],camera.Camera().GetPosition().y);
    EXPECT_FLOAT_EQ(context.position[2],camera.Camera().GetPosition().z);
}
TEST(VfxPreviewCamera, EachDocumentOwnsAnIndependentSceneCameraAndFocus)
{
    using namespace NLS; using namespace Editor::VFXTools;
    VfxPreviewCamera first,second;
    const auto unchanged = second.MakeView(640,480);
    first.Focus({1,2,3},4);
    first.Camera().SetFov(60);
    first.Camera().SetRotation(Maths::Quaternion({15,45,0}));
    EXPECT_NE(first.MakeView(640,480).viewProjection,unchanged.viewProjection);
    EXPECT_EQ(second.MakeView(640,480).viewProjection,unchanged.viewProjection);
    EXPECT_EQ(first.FocusState().focusPoint,Maths::Vector3(1,2,3));
    EXPECT_NEAR(Maths::Vector3::Distance(first.Camera().GetPosition(),first.FocusState().focusPoint),first.FocusState().focusDistance,1e-4f);
    first.SuspendInputs();
    EXPECT_FALSE(first.IsNavigating()); EXPECT_FALSE(second.IsNavigating());
}
TEST(VfxModuleStackSummary, HeaderSummaryHighlightsBindingsExpressionsAndChangedLiterals)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    const auto* definition = author::FindModule("InitializeParticle");
    ASSERT_NE(definition,nullptr);
    auto module = author::CreateModule("InitializeParticle");
    // A module that still uses the declared defaults has no summary text.
    EXPECT_TRUE(VfxInputSummary(module,definition).empty());
    // A parameter binding shows the parameter name.
    module.inputs["Size"].mode = author::InputMode::Parameter; module.inputs["Size"].parameter = "User.Size";
    EXPECT_NE(VfxInputSummary(module,definition).find("User.Size"),std::string::npos);
    module.inputs.erase("Size");
    // A literal that differs from the default shows "Input Value".
    module.inputs["Size"].value = {0.75f,0,0,0};
    const auto literal = VfxInputSummary(module,definition);
    EXPECT_NE(literal.find("Size"),std::string::npos); EXPECT_NE(literal.find("0.75"),std::string::npos);
    // Curves and dynamic inputs show their markers.
    module.inputs["Color"].mode = author::InputMode::Curve;
    EXPECT_NE(VfxInputSummary(module,definition).find("Curve"),std::string::npos);
    module.inputs["Color"].mode = author::InputMode::DynamicInput;
    EXPECT_NE(VfxInputSummary(module,definition).find("Script"),std::string::npos);
}
TEST(VfxModuleStackSummary, SearchMatchesLabelsInputsCategoriesAndParameterBindingsCaseInsensitively)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    const auto* box = author::FindModule("BoxLocation");
    ASSERT_NE(box,nullptr);
    auto module = author::CreateModule("BoxLocation");
    EXPECT_TRUE(MatchesVfxModule(module,"Box Location",box,""));
    EXPECT_TRUE(MatchesVfxModule(module,"Box Location",box,"box loc"));
    EXPECT_TRUE(MatchesVfxModule(module,"Box Location",box,"EXTENT"));
    EXPECT_FALSE(MatchesVfxModule(module,"Box Location",box,"sphere"));
    // Parameter bindings join the search text, so a bound input is findable.
    module.inputs["Center"].mode = author::InputMode::Parameter; module.inputs["Center"].parameter = "User.Origin";
    EXPECT_TRUE(MatchesVfxModule(module,"Box Location",box,"user.origin"));
    EXPECT_TRUE(MatchesVfxModule(module,"Box Location",nullptr,"Box Location"));
    EXPECT_FALSE(MatchesVfxModule(module,"Box Location",box,"user.missing"));
}
TEST(VfxTimelineViewTests, RulerMapsTimeSnapsAndZoomsAroundTheCursor)
{
    using namespace NLS; using namespace Editor::VFXTools;
    VfxTimelineView view; view.start = 0; view.end = 5;
    const float width = 500;
    EXPECT_NEAR(view.TimeAt(view.XAt(2.5,width),width),2.5,1e-5);
    EXPECT_NEAR(view.XAt(0.0,width),0.0f,1e-5f);
    view.Pan(50.0f,width);
    EXPECT_NEAR(view.start,-0.5,1e-6);
    EXPECT_NEAR(view.TimeAt(view.XAt(3.0,width),width),3.0,1e-5);
    EXPECT_NEAR(VfxTimelineView::SnapTime(0.37,0.05),0.35,1e-6);
    EXPECT_NEAR(VfxTimelineView::SnapTime(0.38,0.05),0.40,1e-6);
    EXPECT_NEAR(view.ClampTime(-1.0),-0.5,1e-6);
    EXPECT_NEAR(view.ClampTime(99.0),4.5,1e-6);
    // Zooming keeps the time under the cursor anchored.
    const auto anchor = view.TimeAt(40.0f,width); const auto span = view.end-view.start;
    view.ZoomAt(anchor,0.5f);
    EXPECT_NEAR(view.TimeAt(40.0f,width),anchor,1e-4);
    EXPECT_NEAR(view.end-view.start,span*0.5,1e-6);
    view.Fit(10.0,width);
    EXPECT_NEAR(view.start,0.0,1e-6); EXPECT_NEAR(view.end,10.0,1e-6);
}
TEST(VfxTimelineViewTests, CurveCanvasMapsBothAxesHitTestsKeysAndKeepsOrdering)
{
    using namespace NLS; using namespace Editor::VFXTools; namespace author = VFX::Authoring;
    VfxCurveView view; view.timeMin = 0; view.timeMax = 2; view.valueMin = -1; view.valueMax = 3;
    const float width = 400, height = 200;
    EXPECT_NEAR(view.TimeAt(view.XAt(1.0,width),width),1.0,1e-5);
    EXPECT_NEAR(view.ValueAt(view.YAt(2.0f,height),height),2.0f,1e-4f);
    std::vector<author::CurveKey> keys(3);
    keys[0].time = 0; keys[0].value = {0,0,0,0};
    keys[1].time = 1; keys[1].value = {1,0,0,0};
    keys[2].time = 2; keys[2].value = {2,0,0,0};
    // A click on the middle key finds it; a click in the corner does not.
    EXPECT_EQ(view.FindKey(keys,view.XAt(1.0,width),view.YAt(1.0f,height),width,height,6.0f),1);
    EXPECT_EQ(view.FindKey(keys,2.0f,2.0f,width,height,6.0f),-1);
    // Dragging keeps the key between its neighbours and inside the domain.
    EXPECT_NEAR(VfxCurveView::ClampDragTime(keys,1,-1.0),0.001,1e-6);
    EXPECT_NEAR(VfxCurveView::ClampDragTime(keys,1,5.0),1.999,1e-6);
    EXPECT_NEAR(VfxCurveView::ClampDragTime(keys,0,-5.0),0.0,1e-6);
    // Fit frames the key extents with a value margin.
    view.Fit(keys);
    EXPECT_NEAR(view.timeMin,0.0,1e-6); EXPECT_NEAR(view.timeMax,2.0,1e-6);
    EXPECT_LT(view.valueMin,0.0f); EXPECT_GT(view.valueMax,2.0f);
    // Value zoom keeps the anchor value under the cursor.
    const auto anchorValue = view.ValueAt(50.0f,height);
    view.ZoomValue(anchorValue,0.5f);
    EXPECT_NEAR(view.ValueAt(50.0f,height),anchorValue,1e-3f);
}
TEST(VfxPreviewTransport, StepAdvancesByOneFrameWithoutPlaybackSpeedAndWhilePaused)
{
    using namespace NLS; using namespace Editor::VFXTools;
    VfxSession session;
    ASSERT_TRUE(session.Rebuild());
    session.playbackSpeed = 4.0;
    session.playing = false;
    const auto start = session.Instance().Age();
    ASSERT_TRUE(session.Advance(1.0/30.0));
    EXPECT_NEAR(session.Instance().Age(),start+1.0/30.0,1e-6);
    // Normal ticking still honours the playback speed when playing.
    session.playing = true;
    ASSERT_TRUE(session.Tick(0.05));
    EXPECT_NEAR(session.Instance().Age(),start+1.0/30.0+0.2,1e-5);
}
TEST(VfxSelectionClipboard, MultiSelectionTracksAdditiveClicksAndBatchOperations)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    VfxSession session;
    auto system = session.document.Asset();
    auto* stack = author::FindStack(system.emitters[0],author::Stage::ParticleUpdate);
    ASSERT_NE(stack,nullptr);
    auto gravity = author::CreateModule("GravityForce"), drag = author::CreateModule("Drag");
    const auto first = gravity.id, second = drag.id;
    stack->modules.insert(stack->modules.begin(),{gravity,drag});
    const auto emitterId = system.emitters[0].settings.id, stackId = stack->id;
    ASSERT_TRUE(session.document.Apply(system,"seed modules"));
    ASSERT_TRUE(session.Rebuild(true)) << (session.Issues().empty() ? std::string{} : session.Issues().front().message);
    session.SelectModule(emitterId,stackId,first);
    ASSERT_EQ(session.SelectedModules().size(),1u);
    session.SelectModule(emitterId,stackId,second,true);
    ASSERT_EQ(session.SelectedModules().size(),2u);
    EXPECT_TRUE(session.IsModuleSelected(first)); EXPECT_TRUE(session.IsModuleSelected(second));
    // Additive click toggles an already selected module off.
    session.SelectModule(emitterId,stackId,first,true);
    EXPECT_EQ(session.SelectedModules().size(),1u); EXPECT_FALSE(session.IsModuleSelected(first));
    session.SelectModule(emitterId,stackId,first,true);
    ASSERT_EQ(session.SelectedModules().size(),2u);
    // Batch enable/disable is one document step and reaches both modules.
    ASSERT_TRUE(session.SetModulesEnabled({first,second},false)); ASSERT_TRUE(session.Rebuild());
    for (const auto& emitter : session.document.Asset().emitters) for (const auto& item : emitter.stacks) for (const auto& module : item.modules)
        if (module.id == first || module.id == second) EXPECT_FALSE(module.enabled);
    ASSERT_TRUE(session.SetModulesEnabled({first,second},true));
    // Batch duplicate creates fresh ids; batch delete removes the originals and prunes selection.
    const auto copies = session.DuplicateModules({first,second});
    ASSERT_EQ(copies.size(),2u); EXPECT_NE(copies[0],first); EXPECT_NE(copies[1],second);
    ASSERT_TRUE(session.DeleteModules({first,second}));
    for (const auto& emitter : session.document.Asset().emitters) for (const auto& item : emitter.stacks) for (const auto& module : item.modules)
        EXPECT_NE(module.id,first);
    session.ReconcileSelection();
    EXPECT_FALSE(session.IsModuleSelected(first)); EXPECT_FALSE(session.IsModuleSelected(second));
}
TEST(VfxSelectionClipboard, ClipboardCopiesModulesAndEmittersAcrossSystemsWithFreshIds)
{
    using namespace NLS; namespace author = VFX::Authoring; using namespace Editor::VFXTools;
    auto& clipboard = VfxClipboard::Get(); clipboard.Clear();
    // Source system: one extra module with a changed literal input.
    VfxSession source;
    auto system = source.document.Asset();
    auto* stack = author::FindStack(system.emitters[0],author::Stage::ParticleUpdate);
    ASSERT_NE(stack,nullptr);
    auto scale = author::CreateModule("ScaleSize");
    scale.inputs["Scale"].value = {1.5f,0,0,0};
    const auto moduleId = scale.id;
    stack->modules.insert(stack->modules.begin(),scale);
    const auto emitterId = system.emitters[0].settings.id, stackId = stack->id;
    ASSERT_TRUE(source.document.Apply(system,"seed module"));
    ASSERT_TRUE(source.Rebuild(true)) << (source.Issues().empty() ? std::string{} : source.Issues().front().message);
    clipboard.CopyModules(source.document.Asset(),{moduleId});
    ASSERT_TRUE(clipboard.HasModules());
    // Paste into a different system and stack.
    VfxSession target; ASSERT_TRUE(target.Rebuild(true));
    auto targetSystem = target.document.Asset();
    const auto targetEmitter = targetSystem.emitters[0].settings.id;
    auto* targetStack = author::FindStack(targetSystem.emitters[0],author::Stage::ParticleUpdate);
    ASSERT_NE(targetStack,nullptr);
    const auto before = targetStack->modules.size();
    std::vector<std::string> pasted; std::string error;
    ASSERT_TRUE(clipboard.PasteModules(targetSystem,targetEmitter,targetStack->id,pasted,error)) << error;
    ASSERT_EQ(pasted.size(),1u); EXPECT_NE(pasted[0],moduleId);
    ASSERT_TRUE(target.document.Apply(std::move(targetSystem),"paste module")); ASSERT_TRUE(target.Rebuild(true));
    bool found = false;
    for (const auto& emitter : target.document.Asset().emitters) for (const auto& item : emitter.stacks) for (const auto& module : item.modules)
        if (module.id == pasted[0]) { found = true; EXPECT_EQ(module.script,"ScaleSize"); EXPECT_FLOAT_EQ(module.inputs.at("Scale").value[0],1.5f); }
    EXPECT_TRUE(found);
    // Emitters copy with regenerated emitter, stack, module and graph ids.
    clipboard.CopyEmitters(source.document.Asset(),{emitterId});
    ASSERT_TRUE(clipboard.HasEmitters());
    auto emitterTarget = target.document.Asset();
    const auto emittersBefore = emitterTarget.emitters.size();
    std::vector<std::string> pastedEmitters;
    ASSERT_TRUE(clipboard.PasteEmitters(emitterTarget,pastedEmitters,error)) << error;
    ASSERT_EQ(pastedEmitters.size(),1u); EXPECT_EQ(emitterTarget.emitters.size(),emittersBefore+1);
    EXPECT_NE(pastedEmitters[0],emitterId);
    const auto& copy = emitterTarget.emitters.back();
    EXPECT_EQ(copy.settings.id,pastedEmitters[0]);
    EXPECT_NE(copy.settings.spawn.id,source.document.Asset().emitters[0].settings.spawn.id);
    for (const auto& item : copy.stacks) EXPECT_FALSE(item.id.empty());
    // A cleared clipboard rejects pastes instead of silently doing nothing.
    clipboard.Clear();
    EXPECT_FALSE(clipboard.PasteModules(emitterTarget,targetEmitter,targetStack->id,pasted,error));
    EXPECT_FALSE(error.empty());
}
TEST(VfxSelectionClipboard, CommentsAndGroupsRoundTripInEditorDataButNotInSemanticSource)
{
    using namespace NLS; namespace author = VFX::Authoring;
    auto system = author::CreateSystem();
    author::Comment comment;
    comment.id = Guid::New().ToString(); comment.text = "Fire core";
    comment.position = {-200,120}; comment.size = {260,140}; comment.color = 0xFF203040;
    system.comments.push_back(comment);
    author::Group group;
    group.id = Guid::New().ToString(); group.title = "Core"; group.emitters = {system.emitters[0].settings.id};
    system.groups.push_back(group);
    std::string error; author::System restored;
    ASSERT_TRUE(author::Deserialize(author::Serialize(system,true),restored,error)) << error;
    ASSERT_EQ(restored.comments.size(),1u);
    EXPECT_EQ(restored.comments[0].text,"Fire core");
    EXPECT_FLOAT_EQ(restored.comments[0].position[0],-200.0f);
    EXPECT_EQ(restored.comments[0].color,0xFF203040u);
    ASSERT_EQ(restored.groups.size(),1u);
    EXPECT_EQ(restored.groups[0].title,"Core");
    ASSERT_EQ(restored.groups[0].emitters.size(),1u);
    EXPECT_EQ(restored.groups[0].emitters[0],system.emitters[0].settings.id);
    // Runtime/semantic serialization strips editor-only annotations.
    author::System semantic;
    ASSERT_TRUE(author::Deserialize(author::Serialize(system,false),semantic,error)) << error;
    EXPECT_TRUE(semantic.comments.empty());
    EXPECT_TRUE(semantic.groups.empty());
    EXPECT_EQ(author::SemanticSource(system),author::SemanticSource(semantic));
}
TEST(VfxEngineModules, EngineAssetsWrapBuiltinModulesAndCompileIdentically)
{
    using namespace NLS; namespace author = VFX::Authoring;
    author::SourceLibrary library; std::string error;
    const std::array roots{std::filesystem::path("App")/"Assets"/"Engine"};
    ASSERT_TRUE(library.Refresh(roots,error)) << error;
    const auto* gravity = library.Script(author::EngineModuleAssetId("GravityForce"));
    ASSERT_NE(gravity,nullptr) << "the generated engine module assets are missing; run NullusVfxCompile generate-modules";
    const auto module = author::InstantiateModule(*gravity);
    ASSERT_FALSE(module.scriptAsset.asset.empty());
    EXPECT_EQ(module.scriptAsset.version,gravity->exposedVersion);
    EXPECT_FLOAT_EQ(module.inputs.at("Acceleration").value[1],-9.81f);
    // An asset-backed engine module lowers exactly like its code-defined twin.
    const auto build = [](const author::Module& item)
    {
        auto system = author::CreateSystem();
        auto* stack = author::FindStack(system.emitters[0],author::Stage::ParticleUpdate);
        stack->modules.insert(stack->modules.begin(),item);
        return system;
    };
    VFX::CompiledSystem builtinCompiled, assetCompiled;
    std::vector<author::SourceDependency> dependencies;
    std::vector<author::Issue> issues;
    author::System resolved;
    ASSERT_TRUE(VFX::Authoring::Compile(build(author::CreateModule("GravityForce")),library,builtinCompiled,dependencies,issues,&resolved))
        << (issues.empty() ? std::string{} : issues.front().message);
    ASSERT_TRUE(VFX::Authoring::Compile(build(module),library,assetCompiled,dependencies,issues,&resolved))
        << (issues.empty() ? std::string{} : issues.front().message);
    ASSERT_EQ(builtinCompiled.emitters.size(),assetCompiled.emitters.size());
    const auto& builtin = builtinCompiled.emitters[0].update.instructions;
    const auto& asset = assetCompiled.emitters[0].update.instructions;
    ASSERT_EQ(builtin.size(),asset.size());
    for (size_t index = 0; index < builtin.size(); ++index)
    {
        EXPECT_EQ(builtin[index].operation,asset[index].operation);
        EXPECT_EQ(builtin[index].type,asset[index].type);
    }
    // Regenerating engine content produces stable identities.
    const auto first = author::CreateEngineModuleAsset(*author::FindModule("GravityForce"));
    const auto second = author::CreateEngineModuleAsset(*author::FindModule("GravityForce"));
    EXPECT_EQ(first.id,second.id);
    EXPECT_EQ(first.versions[0].id,second.versions[0].id);
    EXPECT_EQ(first.versions[0].graph.id,second.versions[0].graph.id);
    EXPECT_EQ(first.versions[0].inputs[0].id,second.versions[0].inputs[0].id);
}
TEST(VfxEngineModules, EveryGraphEngineModuleCompilesLikeItsBuiltin)
{
    using namespace NLS; namespace author = VFX::Authoring;
    author::SourceLibrary library; std::string error;
    const std::array roots{std::filesystem::path("App")/"Assets"/"Engine"};
    ASSERT_TRUE(library.Refresh(roots,error)) << error;
    size_t graphModules = 0, skippedBuiltin = 0;
    for (const auto& [id,asset] : library.Scripts())
    {
        if (asset.kind != author::AssetKind::Module) continue;
        const auto* version = author::SelectVersion(asset,{});
        if (!version || (version->graph.nodes.empty() && version->stageGraphs.empty())) continue;
        ++graphModules;
        const author::ModuleDefinition* definition = nullptr;
        for (const auto& candidate : author::ModuleLibrary())
            if (candidate.name == asset.name) { definition = &candidate; break; }
        ASSERT_NE(definition,nullptr) << asset.name;
        std::vector<author::Stage> stages{definition->stage};
        for (const auto stage : definition->additionalStages)
            if (std::find(stages.begin(),stages.end(),stage) == stages.end()) stages.push_back(stage);
        bool builtinFailed = false;
        for (const auto stage : stages)
        {
            if (stage != author::Stage::ParticleSpawn && stage != author::Stage::ParticleUpdate) continue;
            const auto supports = [&](const std::string& moduleId)
            {
                const auto* required = author::FindModule(moduleId);
                return required && (required->stage == stage ||
                    std::find(required->additionalStages.begin(),required->additionalStages.end(),stage) != required->additionalStages.end());
            };
            if (!definition->requiresModule.empty() && !supports(definition->requiresModule)) continue;
            const auto build = [&](const bool assetBacked)
            {
                auto system = author::CreateSystem();
                auto* stack = author::FindStack(system.emitters[0],stage);
                EXPECT_NE(stack,nullptr) << author::StageName(stage);
                std::vector<author::Module> modules;
                if (!definition->requiresModule.empty())
                    modules.push_back(author::CreateModule(definition->requiresModule));
                // Isolated harness providers: Limit Force needs an earlier force
                // accumulator; Align Sprite reads the mesh orientation column.
                if (definition->id == "LimitForce") modules.push_back(author::CreateModule("GravityForce"));
                if (definition->id == "AlignSpriteToMeshOrientation") modules.push_back(author::CreateModule("UpdateMeshOrientation"));
                modules.push_back(assetBacked ? author::InstantiateModule(asset,version->id) : author::CreateModule(definition->id));
                if (stack) stack->modules.insert(stack->modules.begin(),modules.begin(),modules.end());
                return system;
            };
            VFX::CompiledSystem builtin, captured;
            std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues; author::System resolved;
            if (!VFX::Authoring::Compile(build(false),library,builtin,dependencies,issues,&resolved)) { builtinFailed = true; continue; }
            if (!VFX::Authoring::Compile(build(true),library,captured,dependencies,issues,&resolved))
            { ADD_FAILURE() << definition->id << " " << author::StageName(stage) << " asset: " << (issues.empty() ? std::string{} : issues.front().message); continue; }
            ASSERT_EQ(builtin.emitters.size(),captured.emitters.size());
            const auto signature = [](const VFX::Instruction& instruction)
            {
                std::string text = std::to_string(static_cast<int>(instruction.operation)) + "|" + std::to_string(static_cast<int>(instruction.type)) + "|" + instruction.name + "|";
                for (const auto value : instruction.literal.scalar)
                    text += std::to_string(value) + ",";
                return text;
            };
            const auto compare = [&](const std::vector<VFX::Instruction>& left,const std::vector<VFX::Instruction>& right,
                const std::vector<VFX::Attribute>& leftAttributes,const std::vector<VFX::Attribute>& rightAttributes,const char* program)
            {
                std::vector<std::string> builtin, captured;
                for (const auto& instruction : left) builtin.push_back(signature(instruction));
                for (const auto& instruction : right) captured.push_back(signature(instruction));
                std::sort(builtin.begin(),builtin.end()); std::sort(captured.begin(),captured.end());
                // The graph path prunes expressions that do not feed a write, so
                // every captured instruction must appear in the native lowering.
                EXPECT_TRUE(std::includes(builtin.begin(),builtin.end(),captured.begin(),captured.end()))
                    << definition->id << " " << author::StageName(stage) << " " << program << ": graph output is not a subset of the native lowering";
                const auto writes = [&](const std::vector<VFX::Instruction>& instructions,const std::vector<VFX::Attribute>& attributes)
                {
                    std::vector<std::string> result;
                    for (const auto& instruction : instructions) if (instruction.operation == VFX::Operation::WriteAttribute)
                    {
                        // Attribute indices depend on registration order; compare
                        // the target name and type instead.
                        const auto& name = instruction.name;
                        result.push_back(name+"|"+std::to_string(static_cast<int>(instruction.type)));
                    }
                    std::sort(result.begin(),result.end());
                    return result;
                };
                EXPECT_EQ(writes(left,leftAttributes),writes(right,rightAttributes)) << definition->id << " " << author::StageName(stage) << " " << program << ": attribute writes differ";
            };
            for (size_t index = 0; index < builtin.emitters.size(); ++index)
            {
                compare(builtin.emitters[index].spawn.instructions,captured.emitters[index].spawn.instructions,
                    builtin.asset.emitters[index].attributes,captured.asset.emitters[index].attributes,"spawn");
                compare(builtin.emitters[index].update.instructions,captured.emitters[index].update.instructions,
                    builtin.asset.emitters[index].attributes,captured.asset.emitters[index].attributes,"update");
            }
        }
        if (builtinFailed) ++skippedBuiltin;
    }
    EXPECT_GE(graphModules,99u); EXPECT_EQ(skippedBuiltin,0u);
}
TEST(VfxEngineModules, EmitterUpdateGraphsDriveEmitterSettings)
{
    using namespace NLS; namespace author = VFX::Authoring;
    author::SourceLibrary library; std::string error;
    const std::array roots{std::filesystem::path("App")/"Assets"/"Engine"};
    ASSERT_TRUE(library.Refresh(roots,error)) << error;
    const auto find = [&](std::string_view name)
    {
        for (const auto& [id,script] : library.Scripts())
            if (script.kind == author::AssetKind::Module && script.name == name)
                if (const auto* version = author::SelectVersion(script,{}); version && !version->graph.nodes.empty())
                    return &script;
        return static_cast<const author::ScriptAsset*>(nullptr);
    };
    const auto* spawnRate = find("Spawn Rate");
    const auto* emitterState = find("Emitter State");
    ASSERT_NE(spawnRate,nullptr);
    ASSERT_NE(emitterState,nullptr);
    const auto build = [&](const bool assetBacked,const float rate,const float duration)
    {
        auto system = author::CreateSystem();
        auto* stack = author::FindStack(system.emitters[0],author::Stage::EmitterUpdate);
        EXPECT_NE(stack,nullptr);
        stack->modules.clear();
        auto spawn = assetBacked ? author::InstantiateModule(*spawnRate) : author::CreateModule("SpawnRate");
        spawn.inputs.at("Rate").value = {rate,0,0,0};
        auto state = assetBacked ? author::InstantiateModule(*emitterState) : author::CreateModule("EmitterState");
        state.inputs.at("Duration").value = {duration,0,0,0};
        state.inputs.at("Loop").value = {0,0,0,0};
        stack->modules.push_back(spawn);
        stack->modules.push_back(state);
        return system;
    };
    std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues; author::System resolved;
    const auto compile = [&](const bool assetBacked,const float rate,const float duration)
    {
        VFX::CompiledSystem compiled;
        EXPECT_TRUE(VFX::Authoring::Compile(build(assetBacked,rate,duration),library,compiled,dependencies,issues,&resolved))
            << (issues.empty() ? std::string{} : issues.front().message);
        return compiled;
    };
    const auto builtin = compile(false,42.0f,2.5f);
    const auto captured = compile(true,42.0f,2.5f);
    ASSERT_EQ(captured.emitters.size(),1u);
    EXPECT_DOUBLE_EQ(captured.asset.emitters[0].spawnRate,builtin.asset.emitters[0].spawnRate);
    EXPECT_DOUBLE_EQ(captured.asset.emitters[0].duration,builtin.asset.emitters[0].duration);
    EXPECT_EQ(captured.asset.emitters[0].looping,builtin.asset.emitters[0].looping);
    // The captured graph is the source: changing its literal input changes the
    // compiled schedule without touching the built-in lowering table.
    const auto changed = compile(true,7.0f,1.25f);
    EXPECT_LT(changed.asset.emitters[0].spawnRate,captured.asset.emitters[0].spawnRate);
    EXPECT_LT(changed.asset.emitters[0].duration,captured.asset.emitters[0].duration);
    EXPECT_FALSE(captured.asset.emitters[0].emitterUpdate.nodes.empty());
    EXPECT_TRUE(std::any_of(captured.asset.emitters[0].emitterUpdate.nodes.begin(),captured.asset.emitters[0].emitterUpdate.nodes.end(),
        [](const auto& node) { return node.operation == VFX::Operation::WriteAttribute && node.name == "Emitter.SpawnRate"; }));
}
TEST(VfxEngineModules, ParameterDrivenEmitterScriptRunsAtRuntime)
{
    using namespace NLS; namespace author = VFX::Authoring;
    author::SourceLibrary library; std::string error;
    const std::array roots{std::filesystem::path("App")/"Assets"/"Engine"};
    ASSERT_TRUE(library.Refresh(roots,error)) << error;
    const auto* spawnRate = library.Script(author::EngineModuleAssetId("SpawnRate"));
    ASSERT_NE(spawnRate,nullptr);
    ASSERT_FALSE(author::SelectVersion(*spawnRate,{})->graph.nodes.empty());
    auto system = author::CreateSystem();
    system.emitters[0].settings.spawnRate = 0;
    system.parameters.push_back({Guid::New().ToString(),"User.Rate",VFX::ValueType::Float,{25,0,0,0}});
    auto* stack = author::FindStack(system.emitters[0],author::Stage::EmitterUpdate);
    ASSERT_NE(stack,nullptr);
    stack->modules.clear();
    auto module = author::InstantiateModule(*spawnRate);
    module.inputs.at("Rate").mode = author::InputMode::Parameter;
    module.inputs.at("Rate").parameter = "User.Rate";
    stack->modules.push_back(module);
    VFX::CompiledSystem compiled; std::vector<author::SourceDependency> dependencies;
    std::vector<author::Issue> issues; author::System resolved;
    ASSERT_TRUE(VFX::Authoring::Compile(system,library,compiled,dependencies,issues,&resolved))
        << (issues.empty() ? std::string{} : issues.front().message);
    ASSERT_EQ(compiled.emitters.size(),1u);
    EXPECT_FALSE(compiled.emitters[0].emitterUpdate.instructions.empty());
    // Dynamic emitter inputs stay out of the baked settings and are evaluated
    // by the CPU-owned emitter script at tick time.
    EXPECT_DOUBLE_EQ(compiled.asset.emitters[0].spawnRate,0.0);
    VFX::CpuEmitter cpu;
    ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error,
        compiled.asset.parameters,compiled.emitters[0].emitterUpdate)) << error;
    ASSERT_TRUE(cpu.SetParameter("User.Rate",{25,0,0,0}));
    ASSERT_TRUE(cpu.Tick(1.0));
    EXPECT_EQ(cpu.Statistics().spawned,25u);
    ASSERT_TRUE(cpu.SetParameter("User.Rate",{10,0,0,0}));
    ASSERT_TRUE(cpu.Tick(1.0));
    EXPECT_EQ(cpu.Statistics().spawned,35u);
}
TEST(VfxEngineModules, ParameterDrivenEmitterStateRunsAtRuntime)
{
    using namespace NLS; namespace author = VFX::Authoring;
    author::SourceLibrary library; std::string error;
    const std::array roots{std::filesystem::path("App")/"Assets"/"Engine"};
    ASSERT_TRUE(library.Refresh(roots,error)) << error;
    const auto* emitterState = library.Script(author::EngineModuleAssetId("EmitterState"));
    ASSERT_NE(emitterState,nullptr);
    ASSERT_FALSE(author::SelectVersion(*emitterState,{})->graph.nodes.empty());
    auto system = author::CreateSystem();
    system.emitters[0].settings.spawnRate = 100;
    system.parameters.push_back({Guid::New().ToString(),"User.Duration",VFX::ValueType::Float,{0.5f,0,0,0}});
    auto* stack = author::FindStack(system.emitters[0],author::Stage::EmitterUpdate);
    ASSERT_NE(stack,nullptr);
    stack->modules.clear();
    auto rate = author::CreateModule("SpawnRate");
    rate.inputs.at("Rate").value = {100,0,0,0};
    stack->modules.push_back(rate);
    auto module = author::InstantiateModule(*emitterState);
    module.inputs.at("Duration").mode = author::InputMode::Parameter;
    module.inputs.at("Duration").parameter = "User.Duration";
    module.inputs.at("Loop").value = {0,0,0,0};
    stack->modules.push_back(module);
    VFX::CompiledSystem compiled; std::vector<author::SourceDependency> dependencies;
    std::vector<author::Issue> issues; author::System resolved;
    ASSERT_TRUE(VFX::Authoring::Compile(system,library,compiled,dependencies,issues,&resolved))
        << (issues.empty() ? std::string{} : issues.front().message);
    ASSERT_EQ(compiled.emitters.size(),1u);
    EXPECT_FALSE(compiled.emitters[0].emitterUpdate.instructions.empty());
    EXPECT_DOUBLE_EQ(compiled.asset.emitters[0].duration,5.0);
    VFX::CpuEmitter cpu;
    ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error,
        compiled.asset.parameters,compiled.emitters[0].emitterUpdate)) << error;
    ASSERT_TRUE(cpu.Tick(1.0));
    EXPECT_EQ(cpu.Statistics().spawned,50u);
    ASSERT_TRUE(cpu.Tick(1.0));
    EXPECT_EQ(cpu.Statistics().spawned,50u);
}
TEST(VfxEngineModules, ParameterDrivenSpawnBurstRunsAtRuntime)
{
    using namespace NLS; namespace author = VFX::Authoring;
    author::SourceLibrary library; std::string error;
    const std::array roots{std::filesystem::path("App")/"Assets"/"Engine"};
    ASSERT_TRUE(library.Refresh(roots,error)) << error;
    const auto* spawnBurst = library.Script(author::EngineModuleAssetId("SpawnBurst"));
    ASSERT_NE(spawnBurst,nullptr);
    ASSERT_FALSE(author::SelectVersion(*spawnBurst,{})->graph.nodes.empty());
    auto system = author::CreateSystem();
    system.parameters.push_back({Guid::New().ToString(),"User.Count",VFX::ValueType::Float,{5,0,0,0}});
    auto* stack = author::FindStack(system.emitters[0],author::Stage::EmitterUpdate);
    ASSERT_NE(stack,nullptr);
    stack->modules.clear();
    auto module = author::InstantiateModule(*spawnBurst);
    module.inputs.at("Time").value = {0,0,0,0};
    module.inputs.at("Count").mode = author::InputMode::Parameter;
    module.inputs.at("Count").parameter = "User.Count";
    stack->modules.push_back(module);
    VFX::CompiledSystem compiled; std::vector<author::SourceDependency> dependencies;
    std::vector<author::Issue> issues; author::System resolved;
    ASSERT_TRUE(VFX::Authoring::Compile(system,library,compiled,dependencies,issues,&resolved))
        << (issues.empty() ? std::string{} : issues.front().message);
    ASSERT_EQ(compiled.emitters.size(),1u);
    EXPECT_TRUE(compiled.asset.emitters[0].bursts.empty());
    EXPECT_FALSE(compiled.emitters[0].emitterUpdate.instructions.empty());
    VFX::CpuEmitter cpu;
    ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error,
        compiled.asset.parameters,compiled.emitters[0].emitterUpdate)) << error;
    ASSERT_TRUE(cpu.Tick(0.1));
    EXPECT_EQ(cpu.Statistics().spawned,5u);
    ASSERT_TRUE(cpu.Tick(0.5));
    EXPECT_EQ(cpu.Statistics().spawned,5u);
}
TEST(VfxLibraryScripts, DuplicateScriptRegeneratesIdentityAndPreservesCapturedGraph)
{
    using namespace NLS; namespace author = VFX::Authoring;
    author::SourceLibrary library; std::string error;
    const std::array roots{std::filesystem::path("App")/"Assets"/"Engine"};
    ASSERT_TRUE(library.Refresh(roots,error)) << error;
    const auto* source = library.Script(author::EngineModuleAssetId("ScaleSize"));
    ASSERT_NE(source,nullptr) << "the generated engine module assets are missing; run NullusVfxCompile generate-modules";
    ASSERT_FALSE(author::SelectVersion(*source,{})->graph.nodes.empty());
    const auto copy = author::DuplicateScript(*source);
    EXPECT_NE(copy.id,source->id);
    ASSERT_EQ(copy.versions.size(),source->versions.size());
    EXPECT_NE(copy.versions[0].id,source->versions[0].id);
    EXPECT_EQ(copy.exposedVersion,copy.versions[0].id);
    EXPECT_FALSE(copy.versions[0].graph.nodes.empty());
    EXPECT_EQ(copy.versions[0].capabilities,source->versions[0].capabilities);
    EXPECT_NE(copy.versions[0].graph.id,source->versions[0].graph.id);
    for (const auto& input : copy.versions[0].inputs)
        EXPECT_TRUE(std::none_of(source->versions[0].inputs.begin(),source->versions[0].inputs.end(),
            [&](const auto& original) { return original.id == input.id; }));
    EXPECT_TRUE(author::ValidateScript(copy,error)) << error;
}
TEST(VfxLibraryScripts, EnginePathsAreDetectedAndReadOnlyDocumentsRejectEdits)
{
    using namespace NLS; namespace author = VFX::Authoring;
    NextTestDirectory directory;
    const auto path = directory.root/"ReadOnly.vfxscript";
    std::string error;
    ASSERT_TRUE(author::SaveScript(path,author::CreateScript(author::AssetKind::Module,"Read Only"),error)) << error;
    Editor::VFXTools::VfxLibraryDocument document;
    ASSERT_TRUE(document.Load(path,true));
    EXPECT_TRUE(document.IsReadOnly());
    EXPECT_FALSE(document.Apply(document.Asset(),"Edit"));
    EXPECT_FALSE(document.Error().empty());
    EXPECT_FALSE(document.BeginEdit("Edit"));
    EXPECT_FALSE(document.Save());
    EXPECT_TRUE(author::IsPathWithin(path,directory.root));
    EXPECT_FALSE(author::IsPathWithin(path,directory.root/"Other"));
}
