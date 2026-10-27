#include "VfxTestExecution.h"
#include <gtest/gtest.h>
#include <Guid.h>
#include <Json/json.hpp>
#include <VFXCompiler/VfxStackCompiler.h>
#include <VFX/CpuEmitter.h>
#include <VFX/SystemInstance.h>
#include "VfxTypedTestAsset.h"

namespace author = NLS::VFX::Authoring;
using namespace NLS::VFX;

TEST(VfxTypedValues, SourceAndCookedValuesPreserveAllIntBitsAndBoolMasks)
{
    for (const int32_t integer : {0,1,-1,16777217,INT32_MIN,INT32_MAX})
    {
        auto asset = NLS::Tests::CreateTypedParticleTestAsset(SimulationTarget::CPU); asset.parameters[0].value = IntValue(integer);
        asset.emitters[0].attributes.back().defaultValue = IntValue(integer);
        SystemAsset loaded; std::string error; ASSERT_TRUE(DeserializeSystem(SerializeSystem(asset),loaded,error)) << error;
        EXPECT_EQ(ReadInt(loaded.parameters[0].value),integer); EXPECT_EQ(ReadInt(loaded.emitters[0].attributes.back().defaultValue),integer);
        EXPECT_EQ(ReadInt(loaded.parameters[2].value),-1);
        CompiledSystem compiled; std::vector<Diagnostic> diagnostics; ASSERT_TRUE(CompileSystem(loaded,compiled,diagnostics));
        const auto text = SerializeCompiledSystem(compiled, false);
        auto json = nlohmann::json::parse(text);
        EXPECT_EQ(json["version"], 7);
        CompiledSystem restored; ASSERT_TRUE(DeserializeCompiledSystem(text,restored,error)) << error; EXPECT_EQ(ReadInt(restored.asset.parameters[0].value),integer);
        json["version"] = 3; const auto before = SerializeCompiledSystem(restored); EXPECT_FALSE(DeserializeCompiledSystem(json.dump(),restored,error)); EXPECT_EQ(SerializeCompiledSystem(restored),before);
    }
}
TEST(VfxTypedValues, IntegerArithmeticComparisonsSelectionAndExplicitConversionExecuteExactly)
{
    auto asset = NLS::Tests::CreateTypedParticleTestAsset(SimulationTarget::CPU); auto& emitter = asset.emitters[0]; auto& graph = emitter.update; graph.nodes.clear();
    const auto constant = [&](ValueType type,Value value) { const auto id = AddNode(graph,Operation::Constant,type); FindNode(graph,id)->value = value; return id; };
    const auto maximum = constant(ValueType::Int32,IntValue(INT32_MAX)), one = constant(ValueType::Int32,IntValue(1));
    const auto overflow = AddNode(graph,Operation::Add,ValueType::Int32); FindNode(graph,overflow)->inputs = {maximum,one};
    const auto minimum = constant(ValueType::Int32,IntValue(INT32_MIN)); const auto same = AddNode(graph,Operation::Equal); FindNode(graph,same)->inputs = {overflow,minimum};
    const auto yes = constant(ValueType::Int32,IntValue(16777217)), no = constant(ValueType::Int32,IntValue(-1));
    const auto selected = AddNode(graph,Operation::Select,ValueType::Int32); FindNode(graph,selected)->inputs = {same,yes,no};
    const auto write = AddNode(graph,Operation::WriteAttribute,ValueType::Int32); FindNode(graph,write)->name = "Particles.Counter"; FindNode(graph,write)->inputs = {selected};
    const auto flag = AddNode(graph,Operation::WriteAttribute,ValueType::Bool); FindNode(graph,flag)->name = "Particles.Flag"; FindNode(graph,flag)->inputs = {same};
    const auto divided = AddNode(graph,Operation::Divide,ValueType::Int32); FindNode(graph,divided)->inputs = {minimum,no};
    const auto edge = AddNode(graph,Operation::WriteAttribute,ValueType::Int32); FindNode(graph,edge)->name = "Particles.Edge"; FindNode(graph,edge)->inputs = {divided};
    const auto compiled = CompileGraph(graph,emitter.attributes); ASSERT_TRUE(compiled.Succeeded()); ParticleDataSet data; data.Initialize(emitter.attributes,1); ASSERT_TRUE(data.Spawn(1)); std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.compiled.program, data, 0, 1, {}, registers));
    EXPECT_EQ(ReadInt(data.Read(data.FindAttribute("Particles.Counter"), 0)), 16777217);
    EXPECT_EQ(ReadInt(data.Read(data.FindAttribute("Particles.Edge"), 0)), INT32_MIN);
    EXPECT_EQ(ReadInt(data.Read(data.FindAttribute("Particles.Flag"), 0)), -1);
    const auto floating = constant(ValueType::Float,{1,0,0,0}); FindNode(graph,overflow)->inputs = {yes,floating}; EXPECT_FALSE(CompileGraph(graph,emitter.attributes).Succeeded());
    const auto converted = AddNode(graph,Operation::Convert,ValueType::Int32); FindNode(graph,converted)->inputs = {floating}; FindNode(graph,overflow)->inputs = {yes,converted}; EXPECT_TRUE(CompileGraph(graph,emitter.attributes).Succeeded());
    FindNode(graph,same)->operation = Operation::LogicalAnd; EXPECT_FALSE(CompileGraph(graph,emitter.attributes).Succeeded());
}
TEST(VfxTypedValues, TypedParametersAndStageControlsRejectWrongApiTypesAtomically)
{
    auto asset = NLS::Tests::CreateTypedParticleTestAsset(); CompiledSystem compiled; std::vector<Diagnostic> diagnostics; ASSERT_TRUE(CompileSystem(asset,compiled,diagnostics));
    SystemInstance instance; std::string error; ASSERT_TRUE(instance.Initialize(std::make_shared<CompiledSystem>(compiled),error));
    ASSERT_TRUE(instance.SetTypedParameter("User.Counter",ValueType::Int32,IntValue(INT32_MAX)));
    ASSERT_TRUE(instance.SetTypedParameter("User.Enabled",ValueType::Bool,BoolValue(false)));
    ASSERT_TRUE(instance.SetTypedParameter("User.Iterations",ValueType::Int32,IntValue(3)));
    const auto before = instance.Capture(); EXPECT_FALSE(instance.SetTypedParameter("User.Counter",ValueType::Float,{1,0,0,0})); EXPECT_FALSE(instance.SetTypedParameter("User.Enabled",ValueType::Int32,IntValue(-1)));
    EXPECT_FALSE(instance.SetParameter("User.Enabled",IntValue(1))); EXPECT_EQ(instance.Capture(),before);
    std::vector<SimulationStageExecution> controls; ASSERT_TRUE(ResolveSimulationStageControls(compiled.asset.emitters[0].simulationStages,before->parameters,controls,error)); EXPECT_FALSE(controls[0].enabled); EXPECT_EQ(controls[0].iterations,3u);
    ASSERT_TRUE(instance.SetTypedParameter("User.Enabled",ValueType::Bool,BoolValue(true))); ASSERT_TRUE(instance.SetTypedParameter("User.Iterations",ValueType::Int32,IntValue(-1)));
    ASSERT_TRUE(ResolveSimulationStageControls(compiled.asset.emitters[0].simulationStages,instance.Capture()->parameters,controls,error)); EXPECT_EQ(controls[0].iterations,0u);
}
TEST(VfxTypedValues, LibraryBoolSwitchAndIntegerInputsPersistThroughVersionedSource)
{
    auto script = author::CreateScript(author::AssetKind::Module,"Typed module"); auto& version = script.versions[0];
    version.inputs = {{NLS::Guid::New().ToString(),"Value",ValueType::Int32,IntValue(16777217),false},{NLS::Guid::New().ToString(),"Enabled",ValueType::Bool,BoolValue(true),true}};
    version.graph.nodes.clear(); const auto value = AddNode(version.graph,Operation::Parameter,ValueType::Int32); FindNode(version.graph,value)->name = "Module.Value";
    const auto no = AddNode(version.graph,Operation::Constant,ValueType::Int32); FindNode(version.graph,no)->value = IntValue(-1);
    const auto branch = AddNode(version.graph,Operation::Parameter,ValueType::Int32); version.switches.push_back({branch,value,no,"Enabled"});
    const auto output = AddNode(version.graph,Operation::WriteAttribute,ValueType::Int32); FindNode(version.graph,output)->name = "Particles.Counter"; FindNode(version.graph,output)->inputs = {branch};
    author::ScriptAsset restored; std::string error; ASSERT_TRUE(author::DeserializeScript(author::SerializeScript(script),restored,error)) << error; EXPECT_EQ(restored.version,4u);
    author::SourceLibrary library; ASSERT_TRUE(library.Add(restored,{},error)); auto system = author::CreateSystem(); system.emitters[0].settings.attributes.push_back({"Particles.Counter",ValueType::Int32,IntValue(0)});
    author::FindStack(system.emitters[0],author::Stage::ParticleSpawn)->modules.push_back(author::InstantiateModule(restored));
    author::System loaded; ASSERT_TRUE(author::Deserialize(author::Serialize(system),loaded,error)) << error; EXPECT_EQ(loaded.version,4u);
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(loaded,library,compiled,dependencies,issues)) << (issues.empty() ? "" : issues[0].message);
    CpuEmitter cpu; ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error)); ASSERT_TRUE(cpu.Tick(1.0/60));
    ASSERT_GT(cpu.Data().Count(),0u); EXPECT_EQ(ReadInt(cpu.Data().Read(cpu.Data().FindAttribute("Particles.Counter"),0)),16777217);
}
