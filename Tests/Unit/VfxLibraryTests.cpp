#include "VfxTestExecution.h"
#include <gtest/gtest.h>
#include <array>
#include <Guid.h>
#include <VFX/Authoring/VfxLibrary.h>
#include <VFXCompiler/VfxStackCompiler.h>
#include <VFX/CpuEmitter.h>

namespace
{
namespace author = NLS::VFX::Authoring;
using namespace NLS::VFX;
struct LibraryDirectory
{
    std::filesystem::path root = std::filesystem::temp_directory_path()/("NullusVfxLibrary_"+NLS::Guid::New().ToString());
    LibraryDirectory() { std::filesystem::create_directories(root); }
    ~LibraryDirectory() { std::error_code error; std::filesystem::remove_all(root,error); }
};
}
TEST(VfxLibrary, ScriptAndEmitterVersionRoundTripsRejectCorruptReferencesWithoutMutation)
{
    auto script = author::CreateScript(author::AssetKind::Module,"Size"); auto version = script.versions.front(); version.id = NLS::Guid::New().ToString(); version.minor = 1;
    script.versions.push_back(version); script.exposedVersion = version.id;
    author::ScriptAsset loaded; std::string error;
    ASSERT_TRUE(author::DeserializeScript(author::SerializeScript(script),loaded,error)) << error;
    EXPECT_EQ(author::SerializeScript(script),author::SerializeScript(loaded)); EXPECT_EQ(author::SelectVersion(loaded,{})->minor,1u);
    const auto before = author::SerializeScript(loaded); script.versions.back().id = script.versions.front().id;
    EXPECT_FALSE(author::DeserializeScript(author::SerializeScript(script),loaded,error)); EXPECT_EQ(before,author::SerializeScript(loaded));
    auto emitter = author::CreateEmitterSource("Inherited"); author::EmitterSource other;
    ASSERT_TRUE(author::DeserializeEmitterSource(author::SerializeEmitterSource(emitter),other,error)) << error;
    EXPECT_EQ(author::SerializeEmitterSource(emitter),author::SerializeEmitterSource(other));
}
TEST(VfxLibrary, InputMetadataRoundTripsAndIsValidated)
{
    using namespace NLS::VFX;
    auto script = author::CreateScript(author::AssetKind::Module,"Metadata");
    auto& version = script.versions.front();
    auto& first = version.inputs.front();
    first.metadata.displayName = "Value Display";
    first.metadata.tooltip = "Value tooltip";
    first.metadata.unit = "cm";
    first.metadata.hasRange = true;
    first.metadata.minimum = 0;
    first.metadata.maximum = 10;
    first.metadata.step = 0.5f;
    first.metadata.visibleIf = "Enabled";
    author::ScriptInput toggle;
    toggle.id = NLS::Guid::New().ToString();
    toggle.name = "Enabled";
    toggle.type = ValueType::Bool;
    toggle.defaultValue = {1,0,0,0};
    version.inputs.push_back(toggle);
    author::ScriptAsset loaded; std::string error;
    ASSERT_TRUE(author::DeserializeScript(author::SerializeScript(script),loaded,error)) << error;
    const auto* restored = author::SelectVersion(loaded,{});
    ASSERT_NE(restored,nullptr);
    ASSERT_EQ(restored->inputs.size(),2u);
    EXPECT_EQ(restored->inputs[0].metadata.displayName,"Value Display");
    EXPECT_EQ(restored->inputs[0].metadata.tooltip,"Value tooltip");
    EXPECT_EQ(restored->inputs[0].metadata.unit,"cm");
    EXPECT_EQ(restored->inputs[0].metadata.visibleIf,"Enabled");
    EXPECT_TRUE(restored->inputs[0].metadata.hasRange);
    EXPECT_FLOAT_EQ(restored->inputs[0].metadata.minimum,0.0f);
    EXPECT_FLOAT_EQ(restored->inputs[0].metadata.maximum,10.0f);
    EXPECT_FLOAT_EQ(restored->inputs[0].metadata.step,0.5f);
    const auto before = author::SerializeScript(loaded);
    auto invalid = script;
    invalid.versions.front().inputs[0].metadata.visibleIf = "Missing";
    EXPECT_FALSE(author::DeserializeScript(author::SerializeScript(invalid),loaded,error));
    EXPECT_EQ(before,author::SerializeScript(loaded));
    // Engine assets inherit the curated metadata from the module definition.
    const auto* definition = author::FindModule("GravityForce");
    ASSERT_NE(definition,nullptr);
    const auto asset = author::CreateEngineModuleAsset(*definition);
    ASSERT_FALSE(asset.versions.empty());
    const auto acceleration = std::find_if(asset.versions.front().inputs.begin(),asset.versions.front().inputs.end(),
        [](const auto& input) { return input.name == "Acceleration"; });
    ASSERT_NE(acceleration,asset.versions.front().inputs.end());
    EXPECT_EQ(acceleration->metadata.unit,"cm/s2");
    EXPECT_FALSE(acceleration->metadata.tooltip.empty());
    EXPECT_EQ(asset.versions.front().capabilities,definition->capabilities);
    const auto serialized = author::SerializeScript(asset);
    EXPECT_EQ(serialized.find("nativeModule"),std::string::npos);
    author::ScriptAsset restoredAsset;
    ASSERT_TRUE(author::DeserializeScript(serialized,restoredAsset,error)) << error;
    EXPECT_EQ(restoredAsset.versions.front().capabilities,definition->capabilities);
    const auto* limit = author::FindModule("LimitForce");
    ASSERT_NE(limit,nullptr);
    EXPECT_NE(limit->capabilities & author::ModuleCapability_ForceLimit,0u);
    EXPECT_EQ(author::InstantiateModule(asset).script,asset.name);
}
TEST(VfxLibrary, StandaloneModuleInliningVersionPinningAndNestedFunctionsAffectCpuAndGpuPrograms)
{
    author::SourceLibrary library; std::string error;
    auto function = author::CreateScript(author::AssetKind::Function,"Double");
    auto& f = function.versions.front(); const auto input = f.graph.nodes.front().id;
    const auto two = AddNode(f.graph,Operation::Constant); FindNode(f.graph,two)->value = {2,0,0,0};
    const auto multiply = AddNode(f.graph,Operation::Multiply); FindNode(f.graph,multiply)->inputs = {input,two};
    FindNode(f.graph,2)->inputs = {multiply}; ASSERT_TRUE(library.Add(function,{},error));
    auto module = author::CreateScript(author::AssetKind::Module,"Set Size"); auto& m = module.versions.front();
    const auto call = AddNode(m.graph,Operation::Parameter); m.calls.push_back({call,{function.id,f.id},"Output",{{"Value",1}}}); FindNode(m.graph,2)->inputs = {call};
    ASSERT_TRUE(library.Add(module,{},error));
    auto system = author::CreateSystem(); auto& stack = *author::FindStack(system.emitters.front(),author::Stage::ParticleSpawn);
    auto instance = author::InstantiateModule(module); instance.inputs["Value"].value = {0.25f,0,0,0}; stack.modules.push_back(instance);
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)) << (issues.empty() ? "" : issues.front().message);
    EXPECT_GE(dependencies.size(),2u);
    CpuEmitter cpu; ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error)); ASSERT_TRUE(cpu.Tick(0.1));
    ASSERT_GT(cpu.Data().Count(),0u); EXPECT_FLOAT_EQ(cpu.Data().Read(cpu.Data().FindAttribute("Particles.Size"),0)[0],0.5f);
    system.emitters.front().settings.target = SimulationTarget::GPU; ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues));
    EXPECT_FALSE(compiled.emitters[0].spawnHlsl.empty());
    stack.modules.back().scriptAsset.version = NLS::Guid::New().ToString(); const auto before = SerializeCompiledSystem(compiled);
    EXPECT_FALSE(author::Compile(system,library,compiled,dependencies,issues)); EXPECT_EQ(before,SerializeCompiledSystem(compiled));
}
TEST(VfxLibrary, StaticSwitchExcludesInactiveBranchesAndRejectsRuntimeBindings)
{
    auto script = author::CreateScript(author::AssetKind::Module,"Switch"); auto& version = script.versions.front(); version.inputs.front().isStatic = true;
    const auto positive = AddNode(version.graph,Operation::Constant); FindNode(version.graph,positive)->value = {0.75f,0,0,0};
    const auto invalid = AddNode(version.graph,Operation::Attribute); FindNode(version.graph,invalid)->name = "Particles.Missing";
    const auto branch = AddNode(version.graph,Operation::Parameter); version.switches.push_back({branch,positive,invalid,"Value"}); FindNode(version.graph,2)->inputs = {branch};
    author::SourceLibrary library; std::string error; ASSERT_TRUE(library.Add(script,{},error));
    auto system = author::CreateSystem(); auto module = author::InstantiateModule(script); author::FindStack(system.emitters[0],author::Stage::ParticleSpawn)->modules.push_back(module);
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues));
    auto& value = author::FindStack(system.emitters[0],author::Stage::ParticleSpawn)->modules.back().inputs["Value"];
    value.value[0] = 0; EXPECT_FALSE(author::Compile(system,library,compiled,dependencies,issues));
    value.mode = author::InputMode::Parameter; value.parameter = "User.Value"; system.parameters.push_back({NLS::Guid::New().ToString(),"User.Value",ValueType::Float,{1,0,0,0}});
    EXPECT_FALSE(author::Compile(system,library,compiled,dependencies,issues));
}
TEST(VfxLibrary, EmitterInheritanceMergesStableOverridesAndInstancesHaveIndependentGraphIds)
{
    auto parent = author::CreateEmitterSource("Parent"); auto& base = parent.versions.front().emitter;
    const auto module = author::FindStack(base,author::Stage::ParticleSpawn)->modules.front().id;
    author::SourceLibrary library; std::string error; ASSERT_TRUE(library.Add(parent,{},error));
    auto system = author::CreateSystem(); system.emitters = {author::InstantiateEmitter(parent),author::InstantiateEmitter(parent)};
    system.emitters[0].overrides.push_back({module,{},{{"Color",author::Input{author::InputMode::Literal,{0,1,0,1}}}}});
    system.emitters[1].settings.target = SimulationTarget::GPU; system.emitters[1].settingsOverrides = {"target"};
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)) << (issues.empty() ? "" : issues.front().message);
    ASSERT_EQ(compiled.emitters.size(),2u); EXPECT_NE(compiled.asset.emitters[0].spawn.id,compiled.asset.emitters[1].spawn.id);
    EXPECT_EQ(compiled.asset.emitters[1].target,SimulationTarget::GPU);
    CompiledSystem roundTrip; ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled),roundTrip,error)) << error;
    CpuEmitter cpu; ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error)); ASSERT_TRUE(cpu.Tick(0.1));
    EXPECT_EQ(cpu.Data().Read(cpu.Data().FindAttribute("Particles.Color"),0),(Value{0,1,0,1}));
    system.emitters[0].overrides[0].module = NLS::Guid::New().ToString(); EXPECT_FALSE(author::Compile(system,library,compiled,dependencies,issues));
}
TEST(VfxLibrary, RecursiveInheritanceAndFunctionCallsAreDiagnosedAndFileMovesPreserveReferences)
{
    LibraryDirectory directory; std::string error; auto source = author::CreateEmitterSource("Cycle"); source.versions.front().emitter.parent = {source.id,source.exposedVersion};
    author::SourceLibrary library; ASSERT_TRUE(library.Add(source,{},error)); auto system = author::CreateSystem(); system.emitters = {author::InstantiateEmitter(source)};
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    EXPECT_FALSE(author::Compile(system,library,compiled,dependencies,issues)); ASSERT_FALSE(issues.empty()); EXPECT_NE(issues.front().message.find("cycle"),std::string::npos);
    auto script = author::CreateScript(author::AssetKind::Module,"Movable"); ASSERT_TRUE(author::SaveScript(directory.root/"Old.vfxscript",script,error));
    const std::array roots{directory.root}; ASSERT_TRUE(library.Refresh(roots,error));
    std::filesystem::rename(directory.root/"Old.vfxscript",directory.root/"New.vfxscript"); ASSERT_TRUE(library.Refresh(roots,error));
    EXPECT_TRUE(library.Script(script.id)); EXPECT_EQ(library.Dependency(script.id)->path.filename(),"New.vfxscript");
    ASSERT_TRUE(author::SaveScript(directory.root/"Duplicate.vfxscript",script,error)); const auto before = library.SemanticSource();
    EXPECT_FALSE(library.Refresh(roots,error)); EXPECT_EQ(before,library.SemanticSource());
}
TEST(VfxLibrary, PinnedVersionsKeepBehaviorWhileInheritedDefaultsAndLocalOverridesTrackSourceUpdates)
{
    std::string error; auto source = author::CreateEmitterSource("Evolving Parent"); const auto originalVersion = source.exposedVersion;
    auto second = source.versions.front(); second.id = NLS::Guid::New().ToString(); second.minor = 1;
    auto* init = author::FindStack(second.emitter,author::Stage::ParticleSpawn); init->modules.front().inputs["Size"].value[0] = 0.75f;
    source.versions.push_back(second); source.exposedVersion = second.id;
    auto system = author::CreateSystem(); system.emitters = {author::InstantiateEmitter(source,originalVersion)};
    author::SourceLibrary library; ASSERT_TRUE(library.Add(source,{},error)); CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    const auto sample = [&]() { CpuEmitter cpu; EXPECT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error)); EXPECT_TRUE(cpu.Tick(0.1)); return cpu.Data().Read(cpu.Data().FindAttribute("Particles.Size"),0)[0]; };
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)); EXPECT_FLOAT_EQ(sample(),0.05f);
    system.emitters[0].parent.version = second.id; ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)); EXPECT_FLOAT_EQ(sample(),0.75f);
    const auto moduleId = second.emitter.stacks[2].modules.front().id;
    system.emitters[0].overrides.push_back({moduleId,{},{{"Size",author::Input{author::InputMode::Literal,{0.4f,0,0,0}}}}});
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)); EXPECT_FLOAT_EQ(sample(),0.4f);
    system.emitters[0].overrides.clear(); ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)); EXPECT_FLOAT_EQ(sample(),0.75f);
}
TEST(VfxLibrary, RecursiveFunctionCallsFailWithoutReplacingProgram)
{
    auto function = author::CreateScript(author::AssetKind::Function,"Recursive"); auto& version = function.versions.front();
    const auto call = AddNode(version.graph,Operation::Parameter); version.calls.push_back({call,{function.id,version.id},"Output",{}}); FindNode(version.graph,2)->inputs = {call};
    auto module = author::CreateScript(author::AssetKind::Module,"Caller"); const auto invoke = AddNode(module.versions[0].graph,Operation::Parameter);
    module.versions[0].calls.push_back({invoke,{function.id,version.id},"Output",{}}); FindNode(module.versions[0].graph,2)->inputs = {invoke};
    author::SourceLibrary library; std::string error; ASSERT_TRUE(library.Add(function,{},error)); ASSERT_TRUE(library.Add(module,{},error));
    auto system = author::CreateSystem(); CompiledSystem compiled; std::vector<author::Issue> issues; ASSERT_TRUE(author::Compile(system,compiled,issues)); const auto before = SerializeCompiledSystem(compiled);
    author::FindStack(system.emitters[0],author::Stage::ParticleSpawn)->modules.push_back(author::InstantiateModule(module)); std::vector<author::SourceDependency> dependencies;
    EXPECT_FALSE(author::Compile(system,library,compiled,dependencies,issues)); EXPECT_EQ(before,SerializeCompiledSystem(compiled));
    ASSERT_FALSE(issues.empty()); EXPECT_NE(issues.front().message.find("cycle"),std::string::npos);
}
TEST(VfxLibrary, DynamicInputAssetBindingsRoundTripAndCompileIntoBothTargets)
{
    auto inputScript = author::CreateScript(author::AssetKind::DynamicInput,"Size Input");
    author::SourceLibrary library; std::string error; ASSERT_TRUE(library.Add(inputScript,{},error));
    auto system = author::CreateSystem(); auto& value = author::FindStack(system.emitters[0],author::Stage::ParticleSpawn)->modules[0].inputs["Size"];
    value.mode = author::InputMode::Script; value.scriptAsset = {inputScript.id,inputScript.exposedVersion}; value.scriptArguments["Value"] = {0.3f,0,0,0};
    author::System loaded; ASSERT_TRUE(author::Deserialize(author::Serialize(system),loaded,error)) << error; EXPECT_EQ(author::Serialize(system),author::Serialize(loaded));
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)); EXPECT_GE(dependencies.size(),1u);
    CpuEmitter cpu; ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error)); ASSERT_TRUE(cpu.Tick(0.1));
    EXPECT_FLOAT_EQ(cpu.Data().Read(cpu.Data().FindAttribute("Particles.Size"),0)[0],0.3f);
    system.emitters[0].settings.target = SimulationTarget::GPU; ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)); EXPECT_FALSE(compiled.emitters[0].spawnHlsl.empty());
}
TEST(VfxLibrary, NormalizedAgeReadsLifetimeWrittenByAnEarlierModule)
{
    auto script = author::CreateScript(author::AssetKind::Module,"Change Lifetime"); script.versions[0].inputs[0].defaultValue[0] = 4;
    FindNode(script.versions[0].graph,2)->name = "Particles.Lifetime";
    author::SourceLibrary library; std::string error; ASSERT_TRUE(library.Add(script,{},error));
    auto system = author::CreateSystem(); auto* update = author::FindStack(system.emitters[0],author::Stage::ParticleUpdate);
    update->modules.clear(); update->modules.push_back(author::InstantiateModule(script));
    auto color = author::CreateModule("ColorOverLife"); color.inputs["Start"].value = {1,0,0,1}; color.inputs["End"].value = {0,0,0,1}; update->modules.push_back(color);
    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues));
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(1));
    data.Write(data.FindAttribute("Particles.Age"),0,{1,0,0,0}); data.Write(data.FindAttribute("Particles.Lifetime"),0,{2,0,0,0});
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {}, registers));
    EXPECT_FLOAT_EQ(data.Read(data.FindAttribute("Particles.Color"),0)[0],0.75f); EXPECT_FLOAT_EQ(data.Read(data.FindAttribute("Particles.Lifetime"),0)[0],4);
}

TEST(VfxLibrary, RefreshPreservesAliasPathsAndDeduplicatesPhysicalSources)
{
    LibraryDirectory directory;
    const auto real = directory.root / "Real";
    const auto alias = directory.root / "Alias";
    std::filesystem::create_directories(real);
    std::error_code linkError;
    std::filesystem::create_directory_symlink(real.filename(), alias, linkError);
    if (linkError)
        GTEST_SKIP() << "Directory symlinks are unavailable: " << linkError.message();
    const auto script = author::CreateScript(author::AssetKind::Module, "Alias Size");
    std::string error;
    ASSERT_TRUE(author::SaveScript(real / "Size.vfxscript", script, error)) << error;
    author::SourceLibrary library;
    const std::array roots{alias, real};
    ASSERT_TRUE(library.Refresh(roots, error)) << error;
    const auto dependency = library.Dependency(script.id);
    ASSERT_TRUE(dependency.has_value());
    EXPECT_EQ(dependency->path, alias / "Size.vfxscript");
}
