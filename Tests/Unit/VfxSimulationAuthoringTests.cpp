#include "VfxTestExecution.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <Json/json.hpp>
#include <VFXCompiler/VfxStackCompiler.h>
#include "VFX/Editor/VfxSession.h"
#include "VfxAuthoredStageTestAsset.h"
#include "VfxTypedTestAsset.h"

namespace
{
namespace author = NLS::VFX::Authoring;
using namespace NLS::VFX;
bool AddLibrary(const NLS::Tests::AuthoredStageTestAsset& source,author::SourceLibrary& library,std::string& error)
{ return library.Add(source.function,{},error) && library.Add(source.module,{},error) && library.Add(source.emitter,{},error); }
}
TEST(VfxSimulationAuthoring, OrderedIterationsUseStageContextParameterDefaultsAndSsa)
{
    auto fixture = NLS::Tests::CreateAuthoredStageTestAsset(); author::SourceLibrary library; std::string error;
    ASSERT_TRUE(AddLibrary(fixture,library,error)) << error;
    auto repeated = fixture.system.emitters[0].stacks[4].modules[0]; repeated.id = NLS::Guid::New().ToString(); fixture.system.emitters[0].stacks[4].modules.push_back(repeated);
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(fixture.system,library,compiled,dependencies,issues)) << (issues.empty() ? "" : issues[0].message);
    ASSERT_EQ(compiled.emitters[0].stages.size(),4u); EXPECT_GE(dependencies.size(),2u);
    EXPECT_TRUE(compiled.emitters[0].stages[1].program.instructions.empty()); EXPECT_FALSE(compiled.asset.emitters[0].simulationStages[1].enabled);
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(1)); std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].spawn, data, 0, 1, {}, registers));
    const auto age = data.FindAttribute("Particles.Age"); data.Write(age,0,{0.25f,0,0,0});
    for (uint32_t index = 0; index < 4; ++index)
    {
        const auto& stage = compiled.asset.emitters[0].simulationStages[index];
        if (stage.enabled) for (uint32_t iteration = 0; iteration < stage.iterations; ++iteration)
                ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].stages[index].program, data, 0, 1, {1.0f / 60, 7, 0, nullptr, index, iteration}, registers));
    }
    EXPECT_NEAR(data.Read(data.FindAttribute("Particles.Position"),0)[0],-0.41f,1e-6f);
    EXPECT_FLOAT_EQ(data.Read(data.FindAttribute("Particles.Size"),0)[0],0.3f); EXPECT_FLOAT_EQ(data.Read(age,0)[0],0.25f);
    CompiledSystem loaded; ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled,false),loaded,error)) << error;
    EXPECT_EQ(loaded.asset.emitters[0].simulationStages[0].id,compiled.asset.emitters[0].simulationStages[0].id);
    EXPECT_TRUE(loaded.asset.emitters[0].simulationStages[0].graph.nodes.empty()); EXPECT_TRUE(loaded.emitters[0].stages[0].hlsl.empty());
}
TEST(VfxSimulationAuthoring, InheritanceOverridesStagesByStableIdentityAndRejectsRemovedTargets)
{
    auto fixture = NLS::Tests::CreateAuthoredStageTestAsset(true); author::SourceLibrary library; std::string error;
    const auto& parentStacks = fixture.emitter.versions[0].emitter.stacks;
    auto& child = fixture.system.emitters[0]; child.stackOverrides = {{parentStacks[4].id,true,3,std::string{"Inherited Offset"}},{parentStacks[6].id,false,{},{}}};
    auto local = author::CreateSimulationStage("Local"); local.modules.push_back(author::CreateModule("ScaleSize")); child.stacks.push_back(local);
    ASSERT_TRUE(AddLibrary(fixture,library,error)) << error;
    author::System loaded; ASSERT_TRUE(author::Deserialize(author::Serialize(fixture.system),loaded,error)) << error;
    EXPECT_EQ(author::Serialize(loaded),author::Serialize(fixture.system)); fixture.system.emitters.push_back(author::InstantiateEmitter(fixture.emitter));
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues; author::System resolved;
    ASSERT_TRUE(author::Compile(fixture.system,library,compiled,dependencies,issues,&resolved)) << (issues.empty() ? "" : issues[0].message);
    ASSERT_EQ(compiled.asset.emitters[0].simulationStages.size(),5u); EXPECT_EQ(compiled.asset.emitters[0].simulationStages[0].iterations,3u);
    EXPECT_EQ(compiled.asset.emitters[0].simulationStages[0].name,"Inherited Offset"); EXPECT_FALSE(compiled.asset.emitters[0].simulationStages[2].enabled);
    EXPECT_EQ(resolved.emitters[0].stacks[4].inheritedId,parentStacks[4].id);
    EXPECT_NE(compiled.asset.emitters[0].simulationStages[0].id,compiled.asset.emitters[1].simulationStages[0].id);
    const auto previous = SerializeCompiledSystem(compiled); fixture.system.emitters[0].stackOverrides[0].stack = NLS::Guid::New().ToString();
    EXPECT_FALSE(author::Compile(fixture.system,library,compiled,dependencies,issues)); EXPECT_EQ(previous,SerializeCompiledSystem(compiled));
    ASSERT_FALSE(issues.empty()); EXPECT_EQ(issues[0].stack,fixture.system.emitters[0].stackOverrides[0].stack);
}
TEST(VfxSimulationAuthoring, StageEditsUndoRedoSaveAndCompileFailureRetainTheWorkingProgram)
{
    auto fixture = NLS::Tests::CreateAuthoredStageTestAsset(); author::SourceLibrary library; std::string error; ASSERT_TRUE(AddLibrary(fixture,library,error));
    NLS::Editor::Undo::EditorUndoService undo; NLS::Editor::VFXTools::VfxSession session(&undo); session.SetLibrary(library);
    ASSERT_TRUE(session.document.Apply(fixture.system,"Staged system")); ASSERT_TRUE(session.Rebuild());
    auto changed = fixture.system; auto& emitter = changed.emitters[0]; const auto id = emitter.stacks[4].id;
    ASSERT_TRUE(author::MoveSimulationStage(emitter,id,2,error)); ASSERT_TRUE(session.document.Apply(changed,"Reorder stage")); ASSERT_TRUE(session.Rebuild());
    EXPECT_EQ(session.Program()->asset.emitters[0].simulationStages[2].id,id);
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); ASSERT_TRUE(session.Rebuild()); EXPECT_EQ(session.Program()->asset.emitters[0].simulationStages[0].id,id);
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); ASSERT_TRUE(session.Rebuild()); EXPECT_EQ(session.Program()->asset.emitters[0].simulationStages[2].id,id);
    changed = session.document.Asset(); auto& value = *author::FindStack(changed.emitters[0],author::Stage::Simulation);
    const auto before = author::Serialize(changed); ASSERT_TRUE(session.document.BeginEdit("Drag iterations"));
    for (uint32_t i = 2; i < 6; ++i) { value.iterations = i; ASSERT_TRUE(session.document.PreviewEdit(changed)); }
    ASSERT_TRUE(session.document.EndEdit()); ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(author::Serialize(session.document.Asset()),before);
    ASSERT_TRUE(undo.RequestRedo().Succeeded());
    const auto path = std::filesystem::temp_directory_path()/("NullusStage_"+NLS::Guid::New().ToString()+".vfx");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path,error); } } cleanup{path};
    ASSERT_TRUE(session.document.Save(path)); NLS::Editor::VFXTools::VfxDocument reopened; ASSERT_TRUE(reopened.Load(path)); EXPECT_EQ(author::Serialize(session.document.Asset()),author::Serialize(reopened.Asset()));
    ASSERT_TRUE(session.Rebuild()); const auto program = session.Program(); changed = session.document.Asset(); changed.emitters[0].settings.target = SimulationTarget::CPU;
    ASSERT_TRUE(session.document.Apply(changed,"Invalid CPU stage")); EXPECT_FALSE(session.Rebuild()); EXPECT_EQ(session.Program(),program);
    ASSERT_FALSE(session.Issues().empty()); EXPECT_FALSE(session.Issues()[0].stack.empty()); EXPECT_TRUE(session.Tick(1.0/60));
}
TEST(VfxSimulationAuthoring, StageDuplicationClonesDynamicGraphsAndLimitsRejectWithoutMutation)
{
    auto fixture = NLS::Tests::CreateAuthoredStageTestAsset(); auto& emitter = fixture.system.emitters[0];
    auto& scale = emitter.stacks.back().modules[0].inputs["Scale"]; scale.mode = author::InputMode::DynamicInput; scale.expression.id = NLS::Guid::New().ToString(); scale.expression.stage = ScriptStage::SimulationStage;
    const auto value = AddNode(scale.expression,Operation::Constant); FindNode(scale.expression,value)->value[0] = 0.5f;
    const auto output = AddNode(scale.expression,Operation::WriteAttribute); FindNode(scale.expression,output)->name = "Output"; FindNode(scale.expression,output)->inputs = {value};
    const auto original = emitter.stacks.back(); std::string error; const auto duplicate = author::DuplicateSimulationStage(emitter,original.id,error); ASSERT_FALSE(duplicate.empty());
    EXPECT_NE(emitter.stacks.back().modules[0].id,original.modules[0].id); EXPECT_NE(emitter.stacks.back().modules[0].inputs["Scale"].expression.id,original.modules[0].inputs.at("Scale").expression.id);
    std::vector<author::Issue> issues; EXPECT_TRUE(author::Validate(fixture.system,issues));
    const auto before = author::Serialize(fixture.system); EXPECT_FALSE(author::MoveSimulationStage(emitter,duplicate,99,error)); EXPECT_EQ(before,author::Serialize(fixture.system));
    for (auto& stack : emitter.stacks) if (stack.stage == author::Stage::Simulation) stack.iterations = 128;
    while (emitter.stacks.size() < 13) { auto stage = author::CreateSimulationStage(); stage.iterations = 128; emitter.stacks.push_back(stage); }
    EXPECT_FALSE(author::Validate(fixture.system,issues));
}
TEST(VfxSimulationAuthoring, InlineStageContextMovesWithModulesAndDiagnosticsLocateTheStack)
{
    auto system = author::CreateSystem(); auto& emitter = system.emitters[0]; emitter.settings.target = SimulationTarget::GPU;
    auto stage = author::CreateSimulationStage(); auto scale = author::CreateModule("ScaleSize"); auto& input = scale.inputs["Scale"];
    input.mode = author::InputMode::DynamicInput; input.expression.id = NLS::Guid::New().ToString(); input.expression.stage = ScriptStage::SimulationStage;
    const auto iteration = AddNode(input.expression,Operation::StageIteration), one = AddNode(input.expression,Operation::Constant); FindNode(input.expression,one)->value[0] = 1;
    const auto sum = AddNode(input.expression,Operation::Add); FindNode(input.expression,sum)->inputs = {iteration,one};
    const auto output = AddNode(input.expression,Operation::WriteAttribute); FindNode(input.expression,output)->name = "Output"; FindNode(input.expression,output)->inputs = {sum};
    stage.iterations = 3; stage.modules.push_back(scale); emitter.stacks.push_back(stage);
    CompiledSystem compiled; std::vector<author::Issue> issues; ASSERT_TRUE(author::Compile(system,compiled,issues));
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(1));
    data.Write(data.FindAttribute("Particles.Size"),0,{1,0,0,0}); std::vector<Value> registers;
    for (uint32_t i = 0; i < 3; ++i)
        ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].stages[0].program, data, 0, 1, {0, 0, 0, nullptr, 0, i}, registers));
    EXPECT_FLOAT_EQ(data.Read(data.FindAttribute("Particles.Size"),0)[0],6);
    const auto previous = SerializeCompiledSystem(compiled); const auto updateId = author::FindStack(emitter,author::Stage::ParticleUpdate)->id; std::string error;
    ASSERT_TRUE(author::MoveModule(system,scale.id,updateId,0,error));
    EXPECT_EQ(author::FindStack(emitter,author::Stage::ParticleUpdate)->modules[0].inputs["Scale"].expression.stage,ScriptStage::ParticleUpdate);
    EXPECT_FALSE(author::Compile(system,compiled,issues)); EXPECT_EQ(previous,SerializeCompiledSystem(compiled)); ASSERT_FALSE(issues.empty());
    EXPECT_EQ(issues[0].emitter,emitter.settings.id); EXPECT_EQ(issues[0].stack,updateId);
    ASSERT_TRUE(author::MoveModule(system,scale.id,stage.id,0,error)); ASSERT_TRUE(author::Compile(system,compiled,issues));
    auto& binding = emitter.stacks.back().modules[0].inputs["Scale"]; binding.mode = author::InputMode::Parameter; binding.parameter = "User.Missing";
    EXPECT_FALSE(author::Compile(system,compiled,issues)); ASSERT_FALSE(issues.empty()); EXPECT_EQ(issues[0].stack,stage.id); EXPECT_EQ(issues[0].input,"Scale");
}
TEST(VfxSimulationAuthoring, InheritedControlBindingsBakeDefaultsAndVersionTheStrippedRuntime)
{
    auto fixture = NLS::Tests::CreateAuthoredStageTestAsset(true); author::SourceLibrary library; std::string error;
    fixture.system.parameters.push_back({NLS::Guid::New().ToString(),"User.StageEnabled",ValueType::Float,{0,0,0,0}});
    fixture.system.parameters.push_back({NLS::Guid::New().ToString(),"User.Iterations",ValueType::Float,{3,0,0,0}});
    const auto stageId = fixture.emitter.versions[0].emitter.stacks[4].id;
    author::Emitter::StackOverride override; override.stack = stageId; override.bindings = SimulationStageBindings{"User.StageEnabled","User.Iterations"};
    fixture.system.emitters[0].stackOverrides.push_back(override);
    ASSERT_TRUE(AddLibrary(fixture,library,error));
    author::System source; ASSERT_TRUE(author::Deserialize(author::Serialize(fixture.system),source,error)) << error;
    EXPECT_EQ(source.version,3u);
    EXPECT_EQ(author::Serialize(source),author::Serialize(fixture.system));
    auto sourceJson = nlohmann::json::parse(author::Serialize(source)); sourceJson["version"] = 2;
    const auto retainedSource = author::Serialize(source); EXPECT_FALSE(author::Deserialize(sourceJson.dump(),source,error)); EXPECT_EQ(author::Serialize(source),retainedSource);
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(source,library,compiled,dependencies,issues)) << (issues.empty() ? "" : issues[0].message);
    const auto& stage = compiled.asset.emitters[0].simulationStages[0]; EXPECT_EQ(stage.iterations,3u); EXPECT_FALSE(stage.bindings.enabledDefault);
    auto json = nlohmann::json::parse(SerializeCompiledSystem(compiled, false));
    EXPECT_EQ(json["version"], 7);
    CompiledSystem loaded; ASSERT_TRUE(DeserializeCompiledSystem(json.dump(),loaded,error)) << error;
    std::vector<SimulationStageExecution> controls; ASSERT_TRUE(ResolveSimulationStageControls(loaded.asset.emitters[0].simulationStages,{},controls,error));
    EXPECT_FALSE(controls[0].enabled); EXPECT_EQ(controls[0].iterations,3u);
    const auto previous = SerializeCompiledSystem(loaded); json["version"] = 2;
    EXPECT_FALSE(DeserializeCompiledSystem(json.dump(),loaded,error)); EXPECT_EQ(SerializeCompiledSystem(loaded),previous);
    fixture.system.parameters.back().value[0] = 1.5f;
    EXPECT_FALSE(author::Compile(fixture.system,library,loaded,dependencies,issues)); EXPECT_EQ(SerializeCompiledSystem(loaded),previous); ASSERT_FALSE(issues.empty()); EXPECT_FALSE(issues[0].stack.empty());
    fixture.system.parameters.back().type = ValueType::Float3;
    EXPECT_FALSE(author::Compile(fixture.system,library,loaded,dependencies,issues));
    fixture.system.parameters.back().type = ValueType::Float; fixture.system.parameters.back().value[0] = -2;
    ASSERT_TRUE(author::Compile(fixture.system,library,loaded,dependencies,issues)); EXPECT_EQ(loaded.asset.emitters[0].simulationStages[0].iterations,0u);
}
TEST(VfxSimulationAuthoring, RuntimeControlChangesAreAtomicAndKeepCapturedFramesAndPrograms)
{
    auto fixture = NLS::Tests::CreateAuthoredStageTestAsset(); author::SourceLibrary library; std::string error; ASSERT_TRUE(AddLibrary(fixture,library,error));
    fixture.system.parameters.push_back({NLS::Guid::New().ToString(),"User.Enabled",ValueType::Float,{1,0,0,0}});
    fixture.system.parameters.push_back({NLS::Guid::New().ToString(),"User.Count",ValueType::Float,{2,0,0,0}});
    fixture.system.emitters[0].stacks[4].bindings = {"User.Enabled","User.Count"};
    NLS::Editor::VFXTools::VfxSession session; session.SetLibrary(library); ASSERT_TRUE(session.document.Apply(fixture.system,"Control binding")); ASSERT_TRUE(session.Rebuild());
    const auto program = session.Program(); const auto generation = session.ProgramGeneration(); const auto captured = session.Instance().Capture();
    ASSERT_TRUE(session.SetParameter("User.Count",{0,0,0,0})); ASSERT_TRUE(session.SetParameter("User.Enabled",{}));
    EXPECT_EQ(session.Program(),program); EXPECT_EQ(session.ProgramGeneration(),generation); EXPECT_EQ(captured->parameters.at("User.Count")[0],2);
    const auto previous = session.Instance().Capture(); EXPECT_FALSE(session.SetParameter("User.Count",{1.25f,0,0,0})); EXPECT_FALSE(session.SetParameter("User.Enabled",{2,0,0,0}));
    EXPECT_FALSE(session.SetParameter("User.Count",{129,0,0,0})); EXPECT_EQ(session.Instance().Capture(),previous);
    EXPECT_FLOAT_EQ(session.Parameters().at("User.Count")[0],0); ASSERT_TRUE(session.SetParameter("User.Count",{-3,0,0,0}));
    std::vector<SimulationStageExecution> execution; ASSERT_TRUE(ResolveSimulationStageControls(program->asset.emitters[0].simulationStages,session.Instance().Capture()->parameters,execution,error));
    EXPECT_FALSE(execution[0].enabled); EXPECT_EQ(execution[0].iterations,0u);
}
TEST(VfxSimulationAuthoring, ControlBudgetRejectsBatchWithoutPublishingPartialParameters)
{
    auto source = author::CreateSystem(); source.emitters[0].settings.target = SimulationTarget::GPU;
    for (uint32_t i = 0; i < 9; ++i)
    {
        const auto name = "User.Count"+std::to_string(i); source.parameters.push_back({NLS::Guid::New().ToString(),name,ValueType::Float,{1,0,0,0}});
        auto stage = author::CreateSimulationStage(); stage.bindings.iterations = name; source.emitters[0].stacks.push_back(stage);
    }
    CompiledSystem compiled; std::vector<author::Issue> issues; ASSERT_TRUE(author::Compile(source,compiled,issues));
    SystemInstance instance; std::string error; ASSERT_TRUE(instance.Initialize(std::make_shared<CompiledSystem>(compiled),error));
    std::unordered_map<std::string,Value> values; for (uint32_t i = 0; i < 9; ++i) values["User.Count"+std::to_string(i)] = {128,0,0,0};
    const auto previous = instance.Capture(); EXPECT_FALSE(instance.SetParameters(values)); EXPECT_EQ(instance.Capture(),previous); EXPECT_TRUE(instance.Capture()->parameters.empty());
    values["User.Count8"][0] = 0; ASSERT_TRUE(instance.SetParameters(values)); const auto accepted = instance.Capture();
    EXPECT_FALSE(instance.SetParameter("User.Count8",{1,0,0,0})); EXPECT_EQ(instance.Capture(),accepted);
    values["User.Count0"][0] = 0; values["User.Count8"][0] = 128; ASSERT_TRUE(instance.SetParameters(values));
    EXPECT_FLOAT_EQ(accepted->parameters.at("User.Count0")[0],128); EXPECT_FLOAT_EQ(instance.Capture()->parameters.at("User.Count0")[0],0);
    std::vector<SimulationStageExecution> execution{{true,7}}; source.emitters[0].stacks[0].bindings.iterations = "Particles.Invalid";
    EXPECT_FALSE(author::Validate(source,issues));
}
TEST(VfxSimulationAuthoring, BoundDefaultsRebuildRetainsOverridesAndClearingInheritanceBindingsPersists)
{
    auto fixture = NLS::Tests::CreateAuthoredStageTestAsset(true); author::SourceLibrary library; std::string error;
    fixture.emitter.versions[0].emitter.stacks[4].bindings = {"User.Enabled","User.Count"};
    fixture.system.parameters.push_back({NLS::Guid::New().ToString(),"User.Enabled",ValueType::Float,{1,0,0,0}});
    fixture.system.parameters.push_back({NLS::Guid::New().ToString(),"User.Count",ValueType::Float,{2,0,0,0}}); ASSERT_TRUE(AddLibrary(fixture,library,error));
    NLS::Editor::Undo::EditorUndoService undo; NLS::Editor::VFXTools::VfxSession session(&undo); session.SetLibrary(library); ASSERT_TRUE(session.document.Apply(fixture.system,"Inherited binding")); ASSERT_TRUE(session.Rebuild());
    ASSERT_TRUE(session.SetParameter("User.Count",{4,0,0,0}));
    fixture.system.parameters.back().value[0] = 3; ASSERT_TRUE(session.document.Apply(fixture.system,"Change count default")); ASSERT_TRUE(session.Rebuild());
    EXPECT_EQ(session.Program()->asset.emitters[0].simulationStages[0].iterations,3u); EXPECT_FLOAT_EQ(session.Parameters().at("User.Count")[0],4);
    author::Emitter::StackOverride override; override.stack = fixture.emitter.versions[0].emitter.stacks[4].id; override.bindings = SimulationStageBindings{};
    fixture.system.emitters[0].stackOverrides = {override}; ASSERT_TRUE(session.document.Apply(fixture.system,"Clear inherited controls")); ASSERT_TRUE(session.Rebuild());
    EXPECT_TRUE(session.Program()->asset.emitters[0].simulationStages[0].bindings.iterations.empty());
    author::System loaded; ASSERT_TRUE(author::Deserialize(author::Serialize(session.document.Asset()),loaded,error)); ASSERT_TRUE(loaded.emitters[0].stackOverrides[0].bindings.has_value());
    EXPECT_TRUE(loaded.emitters[0].stackOverrides[0].bindings->iterations.empty()); ASSERT_TRUE(undo.RequestUndo().Succeeded()); ASSERT_TRUE(session.Rebuild());
    EXPECT_EQ(session.Program()->asset.emitters[0].simulationStages[0].bindings.iterations,"User.Count");
}
