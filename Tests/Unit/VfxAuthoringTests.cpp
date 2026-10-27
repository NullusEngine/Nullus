#include "VfxTestExecution.h"
#include "VfxStructuredTestAsset.h"
#include <gtest/gtest.h>

#include <functional>
#include <iostream>
#include <Guid.h>
#include <Json/json.hpp>
#include <VFX/Authoring/VfxAuthoring.h>
#include <VFX/Authoring/VfxLibrary.h>
#include <VFX/VfxTypeSystem.h>
#include <VFX/VfxTypedIR.h>
#include <VFX/VfxTypedBytecode.h>
#include <VFX/VfxTypedShader.h>
#include <VFX/VfxCompileCache.h>
#include <VFXCompiler/VfxStackCompiler.h>
#include "VFX/Editor/VfxSession.h"

namespace
{
namespace author = NLS::VFX::Authoring;
using namespace NLS::VFX;
author::Module& ModuleAt(author::System& system, author::Stage stage, size_t index)
{ return author::FindStack(system.emitters.front(),stage)->modules.at(index); }
}

TEST(VfxAuthoring, TypedStructAndParameterMapLayouts)
{
    namespace typed = NLS::VFX::Typed;
    std::string error;
    typed::TypeRegistry registry;
    const std::array<typed::FieldSpec,3> fields{{
        {"Position",typed::kFloat3},{"Lifetime",typed::kFloat},{"Id",typed::kInt32}}};
    const auto particleType = registry.AddStruct("FParticleHeader",fields,error);
    ASSERT_NE(particleType,typed::kInvalidType) << error;
    const auto* particle = registry.Get(particleType);
    ASSERT_NE(particle,nullptr);
    EXPECT_EQ(particle->size,20u);
    EXPECT_EQ(particle->alignment,4u);
    ASSERT_EQ(particle->fields.size(),3u);
    EXPECT_EQ(particle->fields[0].offset,0u);
    EXPECT_EQ(particle->fields[1].offset,12u);
    EXPECT_EQ(particle->fields[2].offset,16u);

    typed::Value particleValue;
    particleValue.type = particleType;
    particleValue.fields = {
        {typed::kFloat3,{1,2,3,0},},
        {typed::kFloat,{4,0,0,0},},
        {typed::kInt32,{7,0,0,0},}
    };
    EXPECT_TRUE(registry.ValidateValue(particleValue,error)) << error;

    std::vector<typed::ParameterMapEntry> entries{
        {"System.Age",typed::kFloat,0,{typed::kFloat,{1,0,0,0}}},
        {"Emitter.Position",typed::kFloat3,0,{typed::kFloat3,{0,1,0,0}}},
        {"Header",particleType,0,particleValue}
    };
    const auto map = typed::BuildParameterMap(registry,entries,error);
    ASSERT_FALSE(map.entries.empty()) << error;
    EXPECT_EQ(map.entries[0].offset,0u);
    EXPECT_EQ(map.entries[1].offset,4u);
    EXPECT_EQ(map.entries[2].offset,16u);
    EXPECT_EQ(map.alignment,4u);
    EXPECT_EQ(map.size,36u);
    ASSERT_NE(map.Find("Header"),nullptr);
    EXPECT_EQ(map.Find("Header")->type,particleType);
    EXPECT_FALSE(registry.ValidateValue({990,{1,2,3,4},{}},error));

    typed::DataInterfaceDesc grid{"Grid3D",typed::DataInterfaceUsage::Both,
        {{"Sample",{typed::kFloat3},typed::kFloat4}}};
    EXPECT_TRUE(grid.Supports(typed::DataInterfaceUsage::CPU));
    EXPECT_TRUE(grid.Supports(typed::DataInterfaceUsage::GPU));
    EXPECT_FALSE(grid.Supports(typed::DataInterfaceUsage::None));
    typed::DataInterfaceRegistry interfaces;
    interfaces.interfaces.push_back(grid);
    ASSERT_NE(interfaces.Find("Grid3D"),nullptr);
    EXPECT_EQ(interfaces.Find("Grid3D")->functions.front().result,typed::kFloat4);
}

TEST(VfxAuthoring, TypedIRTranslatesTheFlattenedSpawnGraph)
{
    using namespace NLS::VFX;
    Typed::TypeRegistry registry;
    auto system = CreateDefaultSystem();
    const auto& emitter = system.emitters.front();
    Typed::Graph graph;
    std::string error;
    ASSERT_TRUE(Typed::TranslateGraph(emitter.spawn,registry,emitter.attributes,graph,error)) << error;
    EXPECT_FALSE(graph.nodes.empty());
    EXPECT_TRUE(std::any_of(graph.nodes.begin(),graph.nodes.end(),[](const Typed::Node& node)
    { return node.operation == Typed::Operation::WriteAttribute && node.name == "Particles.Position"; }));
    EXPECT_TRUE(std::any_of(graph.nodes.begin(),graph.nodes.end(),[](const Typed::Node& node)
    { return node.operation == Typed::Operation::Random; }));
    for (const auto& node : graph.nodes)
        ASSERT_NE(registry.Get(node.type),nullptr) << "typed node has no registered type";

    auto invalid = emitter.spawn;
    for (auto& node : invalid.nodes)
        if (node.operation == Operation::WriteAttribute)
        { node.name = "Particles.NotDeclared"; break; }
    Typed::Graph rejected;
    EXPECT_FALSE(Typed::TranslateGraph(invalid,registry,emitter.attributes,rejected,error));
    EXPECT_NE(error.find("unknown attribute"),std::string::npos);
}

TEST(VfxAuthoring, TypedIROptimizerFoldsConstantsPrunesAndBuildsLayouts)
{
    using namespace NLS::VFX;
    Typed::TypeRegistry registry;
    Typed::Graph graph;
    auto constant = [](float value)
    {
        Typed::Node node; node.operation = Typed::Operation::Constant; node.type = Typed::kFloat;
        node.value.type = Typed::kFloat; node.value.scalar[0] = value; return node;
    };
    graph.nodes.push_back(constant(2));
    graph.nodes.push_back(constant(3));
    {
        Typed::Node add; add.operation = Typed::Operation::Add; add.type = Typed::kFloat;
        add.inputs = {0,1}; graph.nodes.push_back(add);
    }
    {
        Typed::Node write; write.operation = Typed::Operation::WriteAttribute; write.type = Typed::kFloat;
        write.name = "Particles.Size"; write.inputs = {2}; graph.nodes.push_back(write);
    }
    graph.nodes.push_back(constant(99)); // Unused and must be removed by DCE.
    {
        Typed::Node parameter; parameter.operation = Typed::Operation::Parameter; parameter.type = Typed::kFloat;
        parameter.name = "User.Threshold"; parameter.value.type = Typed::kFloat; parameter.value.scalar[0] = 0.75f;
        graph.nodes.push_back(parameter);
    }
    {
        Typed::Node write; write.operation = Typed::Operation::WriteAttribute; write.type = Typed::kFloat;
        write.name = "Particles.EventKind"; write.inputs = {5}; graph.nodes.push_back(write);
    }

    Typed::OptimizationResult layout;
    Typed::OptimizationStats stats;
    std::string error;
    ASSERT_TRUE(Typed::OptimizeGraph(graph,registry,layout,stats,error)) << error;
    EXPECT_GE(stats.constantsFolded,1u);
    EXPECT_EQ(stats.nodesRemoved,3u);
    ASSERT_EQ(graph.nodes.size(),4u);
    const auto folded = std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const Typed::Node& node)
    { return node.operation == Typed::Operation::Constant && node.value.scalar[0] == 5.0f; });
    ASSERT_NE(folded,graph.nodes.end());
    ASSERT_EQ(layout.parameters.entries.size(),1u);
    EXPECT_EQ(layout.parameters.entries[0].name,"User.Threshold");
    EXPECT_EQ(layout.parameters.entries[0].offset,0u);
    const auto sizeWrite = std::find_if(layout.attributes.begin(),layout.attributes.end(),
        [](const Typed::AttributeSlot& slot) { return slot.name == "Particles.Size"; });
    ASSERT_NE(sizeWrite,layout.attributes.end());
    EXPECT_TRUE(sizeWrite->write);
    EXPECT_FALSE(sizeWrite->read);
}

TEST(VfxAuthoring, TypedIRCompilesToExecutableCpuBytecode)
{
    using namespace NLS::VFX;
    Typed::TypeRegistry registry;
    Typed::Graph graph;
    const auto constant = [](float value)
    {
        Typed::Node node; node.operation = Typed::Operation::Constant; node.type = Typed::kFloat;
        node.value.type = Typed::kFloat; node.value.scalar[0] = value; return node;
    };
    graph.nodes.push_back(constant(2));
    graph.nodes.push_back(constant(3));
    {
        Typed::Node add; add.operation = Typed::Operation::Add; add.type = Typed::kFloat;
        add.inputs = {0,1}; graph.nodes.push_back(add);
    }
    {
        Typed::Node write; write.operation = Typed::Operation::WriteAttribute; write.type = Typed::kFloat;
        write.name = "Particles.Size"; write.inputs = {2}; graph.nodes.push_back(write);
    }
    {
        Typed::Node parameter; parameter.operation = Typed::Operation::Parameter; parameter.type = Typed::kFloat;
        parameter.name = "User.Threshold"; parameter.value.type = Typed::kFloat; parameter.value.scalar[0] = 0.5f;
        graph.nodes.push_back(parameter);
    }
    {
        Typed::Node write; write.operation = Typed::Operation::WriteAttribute; write.type = Typed::kFloat;
        write.name = "Particles.EventKind"; write.inputs = {4}; graph.nodes.push_back(write);
    }

    Typed::OptimizationResult layout;
    Typed::OptimizationStats stats;
    std::string error;
    ASSERT_TRUE(Typed::OptimizeGraph(graph,registry,layout,stats,error)) << error;
    Typed::BytecodeProgram program;
    ASSERT_TRUE(Typed::CompileToBytecode(graph,registry,layout,program,error)) << error;
    EXPECT_EQ(program.parameters.entries.size(),1u);
    EXPECT_EQ(program.attributes.size(),2u);

    Typed::BytecodeExecutionContext context;
    context.types = &registry;
    context.parameters["User.Threshold"] = {Typed::kFloat,{0.75f,0,0,0}};
    ASSERT_TRUE(Typed::ExecuteBytecode(program,context,error)) << error;
    ASSERT_TRUE(context.attributes.contains("Particles.Size"));
    ASSERT_TRUE(context.attributes.contains("Particles.EventKind"));
    EXPECT_FLOAT_EQ(context.attributes.at("Particles.Size").scalar[0],5.0f);
    EXPECT_FLOAT_EQ(context.attributes.at("Particles.EventKind").scalar[0],0.75f);
}

TEST(VfxAuthoring, TypedBytecodeVmImplementsVectorAndMathOps)
{
    using namespace NLS::VFX;
    Typed::TypeRegistry registry;
    Typed::Graph graph;
    {
        Typed::Node parameter; parameter.operation = Typed::Operation::Parameter; parameter.type = Typed::kFloat;
        parameter.name = "User.Value"; parameter.value.type = Typed::kFloat; parameter.value.scalar[0] = 0.5f;
        graph.nodes.push_back(parameter);
    }
    {
        Typed::Node zero; zero.operation = Typed::Operation::Constant; zero.type = Typed::kFloat;
        zero.value.type = Typed::kFloat; graph.nodes.push_back(zero);
    }
    {
        Typed::Node one; one.operation = Typed::Operation::Constant; one.type = Typed::kFloat;
        one.value.type = Typed::kFloat; one.value.scalar[0] = 1.0f; graph.nodes.push_back(one);
    }
    {
        Typed::Node clamp; clamp.operation = Typed::Operation::Clamp; clamp.type = Typed::kFloat;
        clamp.inputs = {0,1,2}; graph.nodes.push_back(clamp);
    }
    {
        Typed::Node write; write.operation = Typed::Operation::WriteAttribute; write.type = Typed::kFloat;
        write.name = "Particles.Size"; write.inputs = {3}; graph.nodes.push_back(write);
    }
    {
        Typed::Node parameter; parameter.operation = Typed::Operation::Parameter; parameter.type = Typed::kFloat3;
        parameter.name = "User.Direction"; parameter.value.type = Typed::kFloat3; parameter.value.scalar = {3,4,0,0};
        graph.nodes.push_back(parameter);
    }
    {
        Typed::Node length; length.operation = Typed::Operation::Length; length.type = Typed::kFloat;
        length.inputs = {5}; graph.nodes.push_back(length);
    }
    {
        Typed::Node write; write.operation = Typed::Operation::WriteAttribute; write.type = Typed::kFloat;
        write.name = "Particles.EventKind"; write.inputs = {6}; graph.nodes.push_back(write);
    }

    Typed::OptimizationResult layout;
    Typed::OptimizationStats stats;
    std::string error;
    ASSERT_TRUE(Typed::OptimizeGraph(graph,registry,layout,stats,error)) << error;
    Typed::BytecodeProgram program;
    ASSERT_TRUE(Typed::CompileToBytecode(graph,registry,layout,program,error)) << error;
    Typed::BytecodeExecutionContext context;
    context.types = &registry;
    context.parameters["User.Value"] = {Typed::kFloat,{2,0,0,0}};
    context.parameters["User.Direction"] = {Typed::kFloat3,{3,4,0,0}};
    ASSERT_TRUE(Typed::ExecuteBytecode(program,context,error)) << error;
    EXPECT_FLOAT_EQ(context.attributes.at("Particles.Size").scalar[0],1.0f);
    EXPECT_FLOAT_EQ(context.attributes.at("Particles.EventKind").scalar[0],5.0f);
}

TEST(VfxAuthoring, TypedIRGeneratesHlslAndShaderMap)
{
    using namespace NLS::VFX;
    Typed::TypeRegistry types;
    auto system = CreateDefaultSystem();
    const auto& emitter = system.emitters.front();
    Typed::Graph typed;
    Typed::OptimizationResult layout;
    Typed::OptimizationStats stats;
    std::string error;
    ASSERT_TRUE(Typed::TranslateGraph(emitter.spawn,types,emitter.attributes,typed,error)) << error;
    ASSERT_TRUE(Typed::OptimizeGraph(typed,types,layout,stats,error)) << error;
    auto map = Typed::BuildTypedShaderMap(typed,layout,types,emitter.attributes,ScriptStage::ParticleSpawn,error);
    ASSERT_FALSE(map.key.empty()) << error;
    ASSERT_EQ(map.stages.size(),1u);
    EXPECT_EQ(map.stages[0].entryPoint,"SpawnParticles");
    EXPECT_NE(map.stages[0].source.find("VfxEvaluate"),std::string::npos);
    EXPECT_NE(map.stages[0].source.find("SpawnParticles"),std::string::npos);
    EXPECT_NE(map.stages[0].source.find("VfxAttributes"),std::string::npos);
    EXPECT_NE(map.stages[0].source.find("VfxParameters"),std::string::npos);
    EXPECT_NE(map.stages[0].source.find("VfxDeltaTime"),std::string::npos);
}

TEST(VfxAuthoring, TypedBytecodeExecutesOverParticleData)
{
    using namespace NLS::VFX;
    Typed::TypeRegistry registry;
    Typed::Graph graph;
    {
        Typed::Node attribute; attribute.operation = Typed::Operation::Attribute; attribute.type = Typed::kFloat;
        attribute.name = "Particles.Size"; graph.nodes.push_back(attribute);
    }
    {
        Typed::Node constant; constant.operation = Typed::Operation::Constant; constant.type = Typed::kFloat;
        constant.value.type = Typed::kFloat; constant.value.scalar[0] = 1.0f; graph.nodes.push_back(constant);
    }
    {
        Typed::Node add; add.operation = Typed::Operation::Add; add.type = Typed::kFloat;
        add.inputs = {0,1}; graph.nodes.push_back(add);
    }
    {
        Typed::Node write; write.operation = Typed::Operation::WriteAttribute; write.type = Typed::kFloat;
        write.name = "Particles.Size"; write.inputs = {2}; graph.nodes.push_back(write);
    }
    Typed::OptimizationResult layout;
    Typed::OptimizationStats stats;
    std::string error;
    ASSERT_TRUE(Typed::OptimizeGraph(graph,registry,layout,stats,error)) << error;
    Typed::BytecodeProgram program;
    ASSERT_TRUE(Typed::CompileToBytecode(graph,registry,layout,program,error)) << error;

    ParticleDataSet data;
    data.Initialize({{"Particles.Size",ValueType::Float,{0.5f,0,0,0}}},2);
    ASSERT_TRUE(data.Spawn(1)); ASSERT_TRUE(data.Spawn(2));
    std::vector<int32_t> bindings;
    ASSERT_TRUE(Typed::BindBytecodeAttributes(program,data,bindings,error)) << error;
    Typed::BytecodeExecutionContext context;
    context.types = &registry;
    context.attributeBindings = bindings;
    ASSERT_TRUE(Typed::ExecuteBytecodeParticles(program,data,0,2,context,error)) << error;
    const auto size = data.FindAttribute("Particles.Size");
    ASSERT_GE(size,0);
    EXPECT_FLOAT_EQ(data.Read(size,0)[0],1.5f);
    EXPECT_FLOAT_EQ(data.Read(size,1)[0],1.5f);
}

TEST(VfxAuthoring, TypedIRGeneratesComparisonAndConvertHlsl)
{
    using namespace NLS::VFX;
    Typed::TypeRegistry types;
    Typed::Graph graph;
    {
        Typed::Node parameter; parameter.operation = Typed::Operation::Parameter; parameter.type = Typed::kFloat;
        parameter.name = "User.Value"; parameter.value.type = Typed::kFloat; parameter.value.scalar[0] = 0.5f;
        graph.nodes.push_back(parameter);
    }
    {
        Typed::Node zero; zero.operation = Typed::Operation::Constant; zero.type = Typed::kFloat;
        zero.value.type = Typed::kFloat; graph.nodes.push_back(zero);
    }
    {
        Typed::Node greater; greater.operation = Typed::Operation::GreaterThan; greater.type = Typed::kBool;
        greater.inputs = {0,1}; graph.nodes.push_back(greater);
    }
    {
        Typed::Node convert; convert.operation = Typed::Operation::Convert; convert.type = Typed::kFloat;
        convert.inputs = {2}; graph.nodes.push_back(convert);
    }
    {
        Typed::Node write; write.operation = Typed::Operation::WriteAttribute; write.type = Typed::kFloat;
        write.name = "Particles.EventKind"; write.inputs = {3}; graph.nodes.push_back(write);
    }
    Typed::OptimizationResult layout;
    Typed::OptimizationStats stats;
    std::string error;
    ASSERT_TRUE(Typed::OptimizeGraph(graph,types,layout,stats,error)) << error;
    const std::array<Attribute,1> attributes{{{"Particles.EventKind",ValueType::Float,{}}}};
    auto map = Typed::BuildTypedShaderMap(graph,layout,types,attributes,ScriptStage::ParticleSpawn,error);
    ASSERT_FALSE(map.key.empty()) << error;
    ASSERT_FALSE(map.stages.empty());
    Typed::BytecodeProgram bytecode;
    ASSERT_TRUE(Typed::CompileToBytecode(graph, types, layout, bytecode, error)) << error;
    for (const auto& [input, expected] : std::array<std::pair<float, float>, 3>{{{0.5f, 1.0f}, {0.0f, 0.0f}, {-0.5f, 0.0f}}})
    {
        Typed::BytecodeExecutionContext context;
        context.types = &types;
        context.parameters["User.Value"] = {Typed::kFloat, {input, 0, 0, 0}};
        ASSERT_TRUE(Typed::ExecuteBytecode(bytecode, context, error)) << error;
        EXPECT_FLOAT_EQ(context.attributes.at("Particles.EventKind").scalar[0], expected);
    }
}

TEST(VfxAuthoring, ScriptHashingAndCompileCacheKeys)
{
    using namespace NLS::VFX;
    auto source = CreateDefaultSystem("Cache source");
    const auto first = HashSystemAsset(source);
    EXPECT_EQ(first,HashSystemAsset(source));
    auto changed = source;
    changed.parameters.push_back({"User.Value",ValueType::Float,{1,0,0,0}});
    EXPECT_NE(first,HashSystemAsset(changed));

    const std::array<uint32_t,1> backends{{1}};
    auto key = MakeCompileCacheKey(source,backends);
    auto other = MakeCompileCacheKey(changed,backends);
    EXPECT_NE(key,other);
    auto versioned = key;
    versioned.compilerVersion = kVfxCompilerVersion+1;
    EXPECT_NE(key,versioned);
    EXPECT_NE(key.ToString(),std::string{});
    const std::array<Authoring::SourceDependency,1> dependencyA{{{"module.a","graph-a",{}}}};
    const std::array<Authoring::SourceDependency,1> dependencyB{{{"module.a","graph-b",{}}}};
    EXPECT_NE(HashDependencies(dependencyA),HashDependencies(dependencyB));
    EXPECT_NE(MakeCompileCacheKey(source,backends,HashDependencies(dependencyA)),
        MakeCompileCacheKey(source,backends,HashDependencies(dependencyB)));

    CompileCache cache;
    auto program = std::make_shared<CompiledSystem>();
    cache.Put(key,program);
    EXPECT_EQ(cache.Size(),1u);
    EXPECT_EQ(cache.Get(key),program);
    EXPECT_EQ(cache.Get(other),nullptr);
    const auto stats = cache.Stats();
    EXPECT_EQ(stats.hits,1u);
    EXPECT_EQ(stats.misses,1u);
    cache.Clear();
    EXPECT_EQ(cache.Size(),0u);
}

TEST(VfxAuthoring, CompileCacheReusesCompiledSystem)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem("Cached system");
    author::SourceLibrary library;
    CompileCache cache;
    std::vector<author::SourceDependency> dependencies;
    std::vector<author::Issue> issues;
    CompiledSystem first, second;
    ASSERT_TRUE(author::Compile(system,library,first,dependencies,issues,nullptr,&cache))
        << (issues.empty() ? std::string{"none"} : issues.front().message);
    ASSERT_TRUE(author::Compile(system,library,second,dependencies,issues,nullptr,&cache))
        << (issues.empty() ? std::string{"none"} : issues.front().message);
    const auto stats = cache.Stats();
    EXPECT_EQ(stats.hits,1u);
    EXPECT_EQ(stats.misses,1u);
    EXPECT_EQ(first.emitters.size(),second.emitters.size());
    EXPECT_EQ(first.systemSpawn.instructions.size(),second.systemSpawn.instructions.size());
}

TEST(VfxAuthoring, DiskCompileCacheRoundTripsAndHitsAcrossInstances)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem("Disk cache system");
    author::SourceLibrary library;
    CompiledSystem compiled;
    std::vector<author::SourceDependency> dependencies;
    std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues))
        << (issues.empty() ? std::string{"none"} : issues.front().message);
    const auto key = MakeCompileCacheKey(compiled.asset,{});
    const auto directory = std::filesystem::temp_directory_path()/
        ("NullusVfxCache_"+NLS::Guid::New().ToString());
    {
        CompileCache writer(directory);
        writer.Put(key,std::make_shared<CompiledSystem>(compiled));
        EXPECT_TRUE(std::filesystem::exists(directory));
    }
    {
        CompileCache reader(directory);
        const auto restored = reader.Get(key);
        ASSERT_NE(restored,nullptr);
        EXPECT_EQ(restored->asset.emitters.size(),compiled.asset.emitters.size());
        EXPECT_EQ(reader.Stats().hits,1u);
        EXPECT_EQ(reader.Stats().misses,0u);
    }
    std::error_code error;
    std::filesystem::remove_all(directory,error);
}

TEST(VfxAuthoring, TypedCompilerMatchesFrozenEngineModuleResults)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    std::ifstream input("Tests/Reference/VfxCompilerBaseline.json");
    ASSERT_TRUE(input);
    const auto reference = nlohmann::json::parse(input);
    ASSERT_EQ(reference["modules"].size(), 118u);
    std::vector<Attribute> attributes;
    for (const auto& entry : reference["attributes"])
        attributes.push_back({entry["name"].get<std::string>(), static_cast<ValueType>(entry["type"].get<uint32_t>()), {}});
    author::SourceLibrary library;
    std::string error;
    const std::array roots{std::filesystem::path("App") / "Assets" / "Engine"};
    ASSERT_TRUE(library.Refresh(roots, error)) << error;
    size_t compared = 0;
    for (const auto& expected : reference["modules"])
    {
        const auto id = expected["id"].get<std::string>();
        const auto* asset = library.Script(id);
        ASSERT_NE(asset, nullptr);
        SCOPED_TRACE(asset->name);
        EXPECT_EQ(HashScriptAsset(*asset), expected["sourceHash"].get<uint64_t>());
        const auto* version = author::SelectVersion(*asset, {});
        ASSERT_NE(version, nullptr);
        const auto compiled = CompileGraph(version->graph, attributes);
        ASSERT_TRUE(compiled.Succeeded()) << (compiled.diagnostics.empty() ? "" : compiled.diagnostics[0].message);
        std::vector<std::string> writes;
        for (const auto& slot : compiled.compiled.program.attributes)
            if (slot.write)
                writes.push_back(slot.name);
        std::sort(writes.begin(), writes.end());
        EXPECT_EQ(writes, expected["writes"].get<std::vector<std::string>>());
        // The persisted table includes reserved/dead declarations for stable GPU
        // binding. Compare the live inputs actually consumed by the bytecode.
        std::set<std::string> parameters;
        for (const auto& instruction : compiled.compiled.program.instructions)
            if (instruction.operation == Operation::Parameter)
                parameters.insert(instruction.name);
        EXPECT_EQ(parameters, expected["parameters"].get<std::set<std::string>>());
        ParticleDataSet data;
        data.Initialize(attributes, 2);
        for (uint32_t particle = 0; particle < 2; ++particle)
        {
            ASSERT_TRUE(data.Spawn(100 + particle));
            for (uint32_t a = 0; a < attributes.size(); ++a)
            {
                const auto& attribute = attributes[a];
                auto value = attribute.type == ValueType::Int32 ? IntValue(1 + particle) : attribute.type == ValueType::Bool ? BoolValue(true) :
                                                                                                                               Value{0.25f + particle, 0.5f, 0.75f, 1.0f};
                if (attribute.name == "Particles.Lifetime")
                    value = {2, 0, 0, 0};
                if (attribute.name == "Particles.Age")
                    value = {0.2f, 0, 0, 0};
                data.Write(a, particle, value);
            }
        }
        ExecutionContext context{0.1f, 73, 11, nullptr, 0, 0, 2};
        ASSERT_TRUE(Typed::ExecuteParticleProgram(compiled.compiled.program, data, 0, 2, context, error)) << error;
        for (const auto& name : writes)
        {
            const auto attribute = data.FindAttribute(name);
            ASSERT_GE(attribute, 0);
            for (uint32_t particle = 0; particle < 2; ++particle)
            {
                const auto bits = expected["values"][name][particle].get<std::array<uint32_t, 4>>();
                const auto values = std::bit_cast<Value>(bits);
                const auto actual = data.Read(attribute, particle);
                for (uint32_t c = 0; c < ComponentCount(attributes[attribute].type); ++c)
                {
                    if (IsIntegralType(attributes[attribute].type))
                        EXPECT_EQ(std::bit_cast<uint32_t>(actual[c]), bits[c]) << name;
                    else if (std::isnan(values[c]))
                        EXPECT_TRUE(std::isnan(actual[c])) << name;
                    else if (std::isinf(values[c]))
                        EXPECT_EQ(actual[c], values[c]) << name;
                    else
                        EXPECT_NEAR(actual[c], values[c], 1e-5f * (std::max)(1.0f, std::abs(values[c]))) << name << " particle=" << particle << " component=" << c;
                }
            }
        }
        ++compared;
    }
    EXPECT_EQ(compared, 118u);
    auto invalid = CreateDefaultSystem();
    invalid.emitters[0].spawn.nodes.clear();
    const auto value = AddNode(invalid.emitters[0].spawn, Operation::Constant);
    const auto write = AddNode(invalid.emitters[0].spawn, Operation::WriteAttribute);
    FindNode(invalid.emitters[0].spawn, write)->name = "Particles.NotDeclared";
    FindNode(invalid.emitters[0].spawn, write)->inputs = {value};
    EXPECT_FALSE(CompileGraph(invalid.emitters[0].spawn, invalid.emitters[0].attributes).Succeeded());
}

TEST(VfxAuthoring, GpuEventScriptsUseTypedKernels)
{
    using namespace NLS::VFX;
    auto system = CreateDefaultSystem();
    auto& asset = system.emitters[0];
    asset.target = SimulationTarget::GPU;
    asset.eventGenerationMask = kParticleEventGenerationAll;
    auto receiver = asset;
    receiver.id = NLS::Guid::New().ToString();
    receiver.spawn.nodes.clear(); receiver.spawn.nextNodeId = 1;
    receiver.update.nodes.clear(); receiver.update.nextNodeId = 1;
    receiver.eventGenerationMask = 0;
    receiver.eventHandlers.clear();
    EmitterAsset::EventHandler handler;
    handler.id = NLS::Guid::New().ToString();
    handler.receiver.eventKind = ParticleEventSelection::Spawn;
    handler.receiver.spawnCount = 1;
    handler.receiver.executionMode = EventExecutionMode::SpawnedParticles;
    receiver.eventHandlers.push_back(handler);
    system.emitters.push_back(receiver);
    CompiledSystem compiled; std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(system,compiled,diagnostics)) << (diagnostics.empty()?"":diagnostics.front().message);
    EXPECT_NE(compiled.emitters[0].spawnHlsl.find("void VfxSpawnOne"), std::string::npos);
    EXPECT_NE(compiled.emitters[1].spawnHlsl.find("void ReceiveEvents"), std::string::npos);
}

TEST(VfxAuthoring, TypedShaderBroadcastsScalarOperandsInVectorArithmetic)
{
    using namespace NLS::VFX;
    // A vector multiplied by a scalar must scale every component. The typed
    // registers only carry a scalar in their x channel, so the shader has to
    // broadcast the operand explicitly.
    Graph graph; graph.id = NLS::Guid::New().ToString(); graph.stage = ScriptStage::ParticleUpdate;
    const auto position = AddNode(graph,Operation::Attribute,ValueType::Float3);
    FindNode(graph,position)->name = "Particles.Position";
    const auto scale = AddNode(graph,Operation::Constant,ValueType::Float);
    FindNode(graph,scale)->value = {0.5f,0,0,0};
    const auto scaled = AddNode(graph,Operation::Multiply,ValueType::Float3);
    FindNode(graph,scaled)->inputs = {position,scale};
    const auto write = AddNode(graph,Operation::WriteAttribute,ValueType::Float3);
    FindNode(graph,write)->name = "Particles.Velocity"; FindNode(graph,write)->inputs = {scaled};
    const std::vector<Attribute> attributes{
        {"Particles.Position",ValueType::Float3,{}},
        {"Particles.Velocity",ValueType::Float3,{}},
        {"Particles.Age",ValueType::Float,{}},
        {"Particles.Lifetime",ValueType::Float,{2,0,0,0}}};
    Typed::TypeRegistry types;
    Typed::Graph typed; Typed::OptimizationResult layout; Typed::OptimizationStats stats; std::string error;
    ASSERT_TRUE(Typed::TranslateGraph(graph,types,attributes,typed,error)) << error;
    ASSERT_TRUE(Typed::OptimizeGraph(typed,types,layout,stats,error)) << error;
    std::string source;
    ASSERT_TRUE(Typed::GenerateTypedComputeHlsl(typed,layout,types,attributes,ScriptStage::ParticleUpdate,source,error)) << error;
    // The scalar operand must be broadcast across the vector width.
    EXPECT_NE(source.find(".x,r"),std::string::npos)
        << "the typed shader must broadcast scalar operands across the vector width:\n" << source;
    // The multiply must not use the raw scalar register.
    EXPECT_EQ(source.find("*r"),std::string::npos)
        << "the typed shader must not multiply a vector by an un-broadcast scalar:\n" << source;
}

TEST(VfxAuthoring, SystemSpawnAndUpdateGraphsExecute)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    auto system = author::CreateSystem("System stages");
    auto& spawn = system.systemSpawn;
    spawn.stage = ScriptStage::SystemSpawn;
    spawn.nodes.clear(); spawn.nextNodeId = 1;
    const auto initial = AddNode(spawn,Operation::Constant);
    FindNode(spawn,initial)->value = {2,0,0,0};
    const auto spawnWrite = AddNode(spawn,Operation::WriteAttribute);
    FindNode(spawn,spawnWrite)->name = "System.SpawnCount";
    FindNode(spawn,spawnWrite)->inputs = {initial};
    auto& update = system.systemUpdate;
    update.stage = ScriptStage::SystemUpdate;
    update.nodes.clear(); update.nextNodeId = 1;
    const auto current = AddNode(update,Operation::Attribute);
    FindNode(update,current)->name = "System.FrameIndex";
    const auto one = AddNode(update,Operation::Constant);
    FindNode(update,one)->value = {1,0,0,0};
    const auto increment = AddNode(update,Operation::Add);
    FindNode(update,increment)->inputs = {current,one};
    const auto updateWrite = AddNode(update,Operation::WriteAttribute);
    FindNode(update,updateWrite)->name = "System.FrameIndex";
    FindNode(update,updateWrite)->inputs = {increment};

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    EXPECT_EQ(compiled.systemSpawn.stage,ScriptStage::SystemSpawn);
    EXPECT_EQ(compiled.systemUpdate.stage,ScriptStage::SystemUpdate);

    std::string error;
    auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    auto frame = instance->Capture();
    ASSERT_TRUE(frame);
    EXPECT_FLOAT_EQ(frame->systemAttributes.at("System.SpawnCount")[0],2.0f);
    EXPECT_FLOAT_EQ(frame->systemAttributes.at("System.FrameIndex")[0],0.0f);
    ASSERT_TRUE(instance->Tick(1.0/60.0));
    frame = instance->Capture();
    ASSERT_TRUE(frame);
    EXPECT_FLOAT_EQ(frame->systemAttributes.at("System.FrameIndex")[0],1.0f);
}

TEST(VfxAuthoring, VersionBoundaryAndRoundTripPreserveOrderedStackAndBindings)
{
    auto source = author::CreateSystem();
    source.parameters.push_back({NLS::Guid::New().ToString(),"User.Tint",ValueType::Float4,{0,1,0,1}});
    auto& color = ModuleAt(source,author::Stage::ParticleSpawn,0).inputs.at("Color");
    color.mode = author::InputMode::Parameter; color.parameter = "User.Tint";
    source.emitters.front().position = {150,240};
    author::System loaded; std::string error;
    ASSERT_TRUE(author::Deserialize(author::Serialize(source),loaded,error)) << error;
    EXPECT_EQ(author::Serialize(source),author::Serialize(loaded));
    const auto before = author::Serialize(loaded);
    EXPECT_FALSE(author::Deserialize(SerializeSystem(CreateDefaultSystem()),loaded,error));
    EXPECT_NE(error.find("v2"),std::string::npos);
    EXPECT_EQ(before,author::Serialize(loaded));
    auto json = nlohmann::json::parse(before);
    json["emitters"][0]["stacks"][2]["modules"][1]["id"] = json["emitters"][0]["stacks"][2]["modules"][0]["id"];
    EXPECT_FALSE(author::Deserialize(json.dump(),loaded,error));
    EXPECT_EQ(before,author::Serialize(loaded));
    EXPECT_TRUE(author::IsSystemSource(before)); EXPECT_FALSE(author::IsSystemSource(SerializeSystem(CreateDefaultSystem())));
}

TEST(VfxAuthoring, ParameterMapReadsPriorModuleWritesAndCompilesBothTargets)
{
    auto system = author::CreateSystem();
    auto& emitter = system.emitters.front();
    author::FindStack(emitter,author::Stage::ParticleSpawn)->modules.resize(1);
    ModuleAt(system,author::Stage::ParticleSpawn,0).inputs["Position"].value = {0,0,0,0};
    ModuleAt(system,author::Stage::ParticleSpawn,0).inputs["Velocity"].value = {1,0,0,0};
    auto add = author::CreateModule("AddVelocity"); add.inputs["Velocity"].value = {2,0,0,0};
    author::FindStack(emitter,author::Stage::ParticleSpawn)->modules.push_back(add);
    auto* schedule = author::FindStack(emitter,author::Stage::EmitterUpdate);
    schedule->modules[1].enabled = false;
    auto burst = author::CreateModule("SpawnBurst"); burst.inputs["Count"].value = {1,0,0,0}; schedule->modules.push_back(burst);
    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? "" : issues.front().message);
    CpuEmitter cpu; std::string error;
    ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error,{},
        compiled.emitters[0].emitterUpdate)) << error;
    ASSERT_TRUE(cpu.Tick(0.1)); ASSERT_EQ(cpu.Data().Count(),1u);
    EXPECT_NEAR(cpu.Data().Read(cpu.Data().FindAttribute("Particles.Velocity"),0)[0],3,1e-5);
    EXPECT_NEAR(cpu.Data().Read(cpu.Data().FindAttribute("Particles.Position"),0)[0],0.3f,1e-5);
    system.emitters[0].settings.target = SimulationTarget::GPU;
    ASSERT_TRUE(author::Compile(system,compiled,issues));
    EXPECT_FALSE(compiled.emitters[0].spawnHlsl.empty());
    EXPECT_FALSE(compiled.emitters[0].compactionHlsl.empty());
}

TEST(VfxAuthoring, ReorderDisableAndInvalidStageHaveRealExecutionConsequences)
{
    auto system = author::CreateSystem();
    auto& stack = *author::FindStack(system.emitters[0],author::Stage::ParticleSpawn);
    auto add = author::CreateModule("AddVelocity"); stack.modules.push_back(add);
    std::string error;
    ASSERT_TRUE(author::MoveModule(system,add.id,stack.id,0,error));
    CompiledSystem compiled; std::vector<author::Issue> issues;
    EXPECT_FALSE(author::Compile(system,compiled,issues));
    EXPECT_EQ(issues.front().module,add.id);
    stack.modules.front().enabled = false;
    ASSERT_TRUE(author::Compile(system,compiled,issues));
    const auto before = author::Serialize(system);
    auto* update = author::FindStack(system.emitters[0],author::Stage::ParticleUpdate);
    EXPECT_FALSE(author::MoveModule(system,add.id,update->id,0,error));
    EXPECT_EQ(before,author::Serialize(system));
    const auto original = stack.modules.back().id;
    const auto copy = author::DuplicateModule(system,original,error);
    EXPECT_FALSE(copy.empty()); EXPECT_NE(copy,original);
}

TEST(VfxAuthoring, LayoutDoesNotRecompileAndFailureKeepsPreviousProgram)
{
    NLS::Editor::VFXTools::VfxSession session;
    ASSERT_TRUE(session.Rebuild()); const auto program = session.Program(); const auto generation = session.ProgramGeneration();
    auto candidate = session.document.Asset(); candidate.emitters[0].position = {100,200};
    ASSERT_TRUE(session.document.Apply(candidate,"Move emitter")); ASSERT_TRUE(session.Rebuild());
    EXPECT_EQ(session.Program(),program); EXPECT_EQ(session.ProgramGeneration(),generation);
    candidate.emitters[0].stacks[0].modules.push_back(author::CreateModule("NotImplemented"));
    ASSERT_TRUE(session.document.Apply(candidate,"Invalid script")); EXPECT_FALSE(session.Rebuild());
    EXPECT_EQ(session.Program(),program); EXPECT_FALSE(session.Issues().empty());
    EXPECT_TRUE(session.Tick(0.1)); EXPECT_GT(session.Instance().Age(),0);
}

TEST(VfxAuthoring, ContinuousEditUndoRedoCancelAndSaveAreDocumentScoped)
{
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Editor::VFXTools::VfxDocument document(&undo);
    const auto before = author::Serialize(document.Asset());
    ASSERT_TRUE(document.BeginEdit("Drag input"));
    for (int i = 1; i <= 4; ++i)
    {
        auto candidate = document.Asset(); ModuleAt(candidate,author::Stage::EmitterUpdate,1).inputs["Rate"].value[0] = float(i);
        ASSERT_TRUE(document.PreviewEdit(candidate));
    }
    ASSERT_TRUE(document.EndEdit()); const auto after = author::Serialize(document.Asset());
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(before,author::Serialize(document.Asset()));
    EXPECT_FALSE(undo.CanUndo()); ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(after,author::Serialize(document.Asset()));
    ASSERT_TRUE(document.BeginEdit("Cancel drag")); auto candidate = document.Asset(); candidate.name = "Cancelled";
    ASSERT_TRUE(document.PreviewEdit(candidate)); ASSERT_TRUE(document.EndEdit(false)); EXPECT_EQ(after,author::Serialize(document.Asset()));
    const auto path = std::filesystem::temp_directory_path()/("NullusVfx_"+NLS::Guid::New().ToString()+".vfx");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path,error); } } cleanup{path};
    ASSERT_TRUE(document.Save(path)) << document.Error(); EXPECT_FALSE(document.IsDirty());
    NLS::Editor::VFXTools::VfxDocument reopened; ASSERT_TRUE(reopened.Load(path)); EXPECT_EQ(after,author::Serialize(reopened.Asset()));
}

TEST(VfxAuthoring, CubicCurvesAndGradientsLowerToCpuAndGpuExpressions)
{
    auto system = author::CreateSystem();
    auto module = author::CreateModule("ScaleColor"); auto& input = module.inputs["Scale"];
    input.mode = author::InputMode::Curve;
    input.curve = {{0,{0,1,0,1},{},{2,-1,0,0},author::CurveInterpolation::Cubic},
        {0.4f,{1,0,0.25f,0.75f},{1,0,0,0},{},author::CurveInterpolation::Linear},
        {1,{0,0.5f,1,0}}};
    author::FindStack(system.emitters[0],author::Stage::ParticleUpdate)->modules.push_back(module);
    CompiledSystem compiled; std::vector<author::Issue> issues; std::string error;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? "" : issues.front().message);
    author::System loaded; ASSERT_TRUE(author::Deserialize(author::Serialize(system),loaded,error)) << error; EXPECT_EQ(author::Serialize(system),author::Serialize(loaded));
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(1));
    const auto age = data.FindAttribute("Particles.Age"), lifetime = data.FindAttribute("Particles.Lifetime"), color = data.FindAttribute("Particles.Color");
    std::vector<Value> registers;
    for (const float t : {0.0f,0.1f,0.399f,0.4f,0.7f,1.0f})
    {
        data.Write(age,0,{t,0,0,0}); data.Write(lifetime,0,{1,0,0,0}); data.Write(color,0,{1,1,1,1});
        ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {}, registers));
        const auto expected = author::EvaluateCurve(input.curve,t), actual = data.Read(color,0);
        for (size_t i = 0; i < 4; ++i) EXPECT_NEAR(actual[i],expected[i],1e-5) << "time=" << t << " channel=" << i;
    }
    system.emitters[0].settings.target = SimulationTarget::GPU; ASSERT_TRUE(author::Compile(system,compiled,issues)); EXPECT_FALSE(compiled.emitters[0].updateHlsl.empty());
}

TEST(VfxAuthoring, DynamicInputsReadTheOrderedParameterMapAndReportCycles)
{
    auto system = author::CreateSystem(); auto& spawn = *author::FindStack(system.emitters[0],author::Stage::ParticleSpawn);
    spawn.modules.resize(1); auto module = author::CreateModule("AddVelocity"); auto& input = module.inputs["Velocity"];
    input.mode = author::InputMode::DynamicInput; input.expression.id = NLS::Guid::New().ToString(); input.expression.stage = ScriptStage::ParticleSpawn;
    const auto read = AddNode(input.expression,Operation::Attribute,ValueType::Float3); FindNode(input.expression,read)->name = "Particles.Velocity";
    const auto write = AddNode(input.expression,Operation::WriteAttribute,ValueType::Float3); FindNode(input.expression,write)->name = "Output"; FindNode(input.expression,write)->inputs = {read};
    spawn.modules.push_back(module); CompiledSystem compiled; std::vector<author::Issue> issues; ASSERT_TRUE(author::Compile(system,compiled,issues));
    CpuEmitter cpu; std::string error; ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error));
    ASSERT_TRUE(cpu.Tick(0.1)); ASSERT_GT(cpu.Data().Count(),0u); EXPECT_NEAR(cpu.Data().Read(cpu.Data().FindAttribute("Particles.Velocity"),0)[1],6,1e-5);
    auto& graph = spawn.modules.back().inputs["Velocity"].expression; const auto sum = AddNode(graph,Operation::Add,ValueType::Float3);
    FindNode(graph,sum)->inputs = {sum,read}; FindNode(graph,write)->inputs = {sum};
    const auto previous = SerializeCompiledSystem(compiled); EXPECT_FALSE(author::Compile(system,compiled,issues)); EXPECT_EQ(previous,SerializeCompiledSystem(compiled));
}

TEST(VfxAuthoring, SeekIsIncrementalAndParameterDefaultsDoNotBecomeOverrides)
{
    NLS::Editor::VFXTools::VfxSession session; auto system = session.document.Asset();
    system.parameters.push_back({NLS::Guid::New().ToString(),"User.Tint",ValueType::Float4,{1,0,0,1}});
    auto& input = ModuleAt(system,author::Stage::ParticleSpawn,0).inputs["Color"];
    input.mode = author::InputMode::Parameter; input.parameter = "User.Tint";
    ASSERT_TRUE(session.document.Apply(system,"Bind parameter")); ASSERT_TRUE(session.Rebuild()); EXPECT_TRUE(session.Parameters().empty());
    ASSERT_TRUE(session.RequestSeek(5)); ASSERT_TRUE(session.Tick(0)); EXPECT_TRUE(session.IsSeeking()); EXPECT_LT(session.Instance().Age(),0.2);
    EXPECT_LE(session.TakeFrames().size(),9u);
    system.parameters[0].value = {0,1,0,1}; ASSERT_TRUE(session.document.Apply(system,"Change default")); ASSERT_TRUE(session.Rebuild());
    EXPECT_TRUE(session.Parameters().empty());
    ASSERT_TRUE(session.SetParameter("User.Tint",{0,0,1,1}));
    system.parameters[0].value = {1,1,0,1}; ASSERT_TRUE(session.document.Apply(system,"Change default again")); ASSERT_TRUE(session.Rebuild());
    EXPECT_EQ(session.Parameters().at("User.Tint"),(Value{0,0,1,1}));
    session.Reset(); EXPECT_FALSE(session.IsSeeking()); EXPECT_EQ(session.TakeFrames().size(),1u);
}

TEST(VfxAuthoring, SavingCandidateNeverRewritesLegacyAssetsAndDocumentsHaveSeparateHistories)
{
    const auto path = std::filesystem::temp_directory_path()/("NullusVfxLegacy_"+NLS::Guid::New().ToString()+".vfx");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path,error); } } cleanup{path};
    std::string error; auto legacy = CreateDefaultSystem(); ASSERT_TRUE(SaveSystem(path,legacy,error));
    EXPECT_FALSE(author::Save(path,author::CreateSystem(),error));
    SystemAsset loaded; ASSERT_TRUE(LoadSystem(path,loaded,error)); EXPECT_EQ(SerializeSystem(legacy),SerializeSystem(loaded));
    NLS::Editor::Undo::EditorUndoService firstUndo, secondUndo;
    NLS::Editor::VFXTools::VfxDocument first(&firstUndo), second(&secondUndo);
    auto a = first.Asset(), b = second.Asset(); a.name = "First change"; b.name = "Second change";
    ASSERT_TRUE(first.Apply(a,"First")); ASSERT_TRUE(second.Apply(b,"Second")); ASSERT_TRUE(firstUndo.RequestUndo().Succeeded());
    EXPECT_NE(first.Asset().name,"First change"); EXPECT_EQ(second.Asset().name,"Second change"); EXPECT_TRUE(secondUndo.CanUndo());
}
TEST(VfxAuthoring, LibraryEditsRecompileWithoutTouchingSystemAndInvalidDependenciesRetainProgram)
{
    namespace author = NLS::VFX::Authoring;
    NLS::Editor::VFXTools::VfxSession session;
    auto module = author::CreateScript(author::AssetKind::Module,"External Size");
    auto system = session.document.Asset(); author::FindStack(system.emitters[0],author::Stage::ParticleSpawn)->modules.push_back(author::InstantiateModule(module));
    author::SourceLibrary library; std::string error; ASSERT_TRUE(library.Add(module,{},error)); session.SetLibrary(library);
    ASSERT_TRUE(session.document.Apply(system,"Reference module")); ASSERT_TRUE(session.Rebuild()); const auto generation = session.ProgramGeneration(); const auto documentRevision = session.document.Revision();
    auto& version = module.versions[0]; const auto node = AddNode(version.graph,Operation::Constant); FindNode(version.graph,node)->value[0] = 0.4f; FindNode(version.graph,2)->inputs = {node};
    author::SourceLibrary updated; ASSERT_TRUE(updated.Add(module,{},error)); session.SetLibrary(updated); ASSERT_TRUE(session.Rebuild());
    EXPECT_GT(session.ProgramGeneration(),generation); EXPECT_EQ(session.document.Revision(),documentRevision);
    const auto program = session.Program(); session.SetLibraryError("Module file is invalid"); EXPECT_FALSE(session.Rebuild()); EXPECT_EQ(session.Program(),program);
    session.SetLibrary(updated); ASSERT_TRUE(session.Rebuild()); EXPECT_TRUE(session.Issues().empty());
    session.SetLibrary({}); EXPECT_FALSE(session.Rebuild()); EXPECT_TRUE(session.Tick(0.1));
}

TEST(VfxAuthoringTiming, WarmupFixedSeekUndoAndInvalidTimingPreserveTheDocumentAndProgram)
{
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Editor::VFXTools::VfxSession session(&undo); auto source = session.document.Asset();
    source.timing = {true,0.125,2,0.25};
    ASSERT_TRUE(session.document.Apply(source,"Configure timing")); ASSERT_TRUE(session.Rebuild());
    EXPECT_DOUBLE_EQ(session.Instance().Age(),0); EXPECT_DOUBLE_EQ(session.Instance().Capture()->simulationAge,0.5);
    EXPECT_GT(session.Instance().Capture()->cpu[0]->Count(),0u);
    ASSERT_TRUE(session.Seek(0.31)); EXPECT_FALSE(session.IsSeeking()); EXPECT_DOUBLE_EQ(session.Instance().Age(),0.25);
    const auto frozen = session.Instance().Capture(); const auto program = session.Program();
    source.timing.fixedTickDelta = 0; EXPECT_FALSE(session.document.Apply(source,"Invalid timing")); EXPECT_EQ(session.Program(),program);
    source.timing.fixedTickDelta = 0.25; ASSERT_TRUE(session.document.Apply(source,"Change fixed delta")); ASSERT_TRUE(session.Rebuild());
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); ASSERT_TRUE(session.Rebuild());
    session.Reset(); EXPECT_DOUBLE_EQ(session.Instance().Age(),0); EXPECT_DOUBLE_EQ(session.Instance().Capture()->simulationAge,0.5);
    EXPECT_DOUBLE_EQ(frozen->age,0.25); EXPECT_DOUBLE_EQ(frozen->simulationAge,0.75);
    const auto frames = session.TakeFrames(); ASSERT_EQ(frames.size(),1u); EXPECT_FALSE(frames[0]->simulationSteps.empty());
    ASSERT_TRUE(session.Tick(0.01)); EXPECT_TRUE(session.TakeFrames().empty()); EXPECT_DOUBLE_EQ(session.Instance().Age(),0);
}

TEST(VfxAuthoring, MeshRendererSettingsSurviveUndoRedoSaveAndReopenInTheOnlyDocumentFormat)
{
    NLS::Editor::Undo::EditorUndoService undo;
    NLS::Editor::VFXTools::VfxDocument document(&undo); auto candidate = document.Asset();
    auto& emitter = candidate.emitters[0].settings; emitter.renderer = RendererType::Mesh;
    emitter.mesh.meshes = {{NLS::Guid::New().ToString(),128},{NLS::Guid::New().ToString(),256}};
    emitter.mesh.materials = {{},{NLS::Guid::New().ToString(),512}};
    emitter.mesh.defaultScale = {-2,0.5f,3,0}; emitter.mesh.meshIndexAttribute = "Particles.Shape";
    emitter.mesh.orientationAttribute = "Particles.Orientation"; emitter.mesh.lodIndex = 2;
    ASSERT_TRUE(document.Apply(candidate,"Set renderer settings")); const auto source = author::Serialize(document.Asset());
    ASSERT_TRUE(undo.RequestUndo().Succeeded()); EXPECT_EQ(document.Asset().emitters[0].settings.renderer,RendererType::Sprite);
    ASSERT_TRUE(undo.RequestRedo().Succeeded()); EXPECT_EQ(author::Serialize(document.Asset()),source);
    const auto path = std::filesystem::temp_directory_path()/("NullusStackMesh_"+NLS::Guid::New().ToString()+".vfx");
    struct Cleanup {std::filesystem::path path; ~Cleanup(){std::error_code error; std::filesystem::remove(path,error);}} cleanup{path};
    ASSERT_TRUE(document.Save(path)); NLS::Editor::VFXTools::VfxDocument reopened;
    ASSERT_TRUE(reopened.Load(path)); EXPECT_EQ(author::Serialize(reopened.Asset()),source);
}

TEST(VfxAuthoring, SecondBatchModulesLowerWithCorrectSemantics)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem();
    auto& emitter = system.emitters.front();
    auto& spawn = *author::FindStack(emitter,author::Stage::ParticleSpawn);
    // Keep the default InitializeParticle first: the location modules declare it
    // as a prerequisite.
    spawn.modules.resize(1);
    spawn.modules[0].inputs["Lifetime"].value = {10,0,0,0};
    spawn.modules[0].inputs["Position"].value = {0,0,0,0};
    spawn.modules[0].inputs["Velocity"].value = {0,0,0,0};

    // Torus sampling must land on the tube surface: the distance to the major
    // circle equals the minor radius.
    auto torus = author::CreateModule("TorusLocation");
    torus.inputs["Center"].value = {0,0,0,0};
    torus.inputs["Major Radius"].value = {1,0,0,0};
    torus.inputs["Minor Radius"].value = {0.25f,0,0,0};
    torus.inputs["Axis"].value = {0,1,0,0};
    spawn.modules.push_back(torus);

    auto& update = *author::FindStack(emitter,author::Stage::ParticleUpdate);
    auto align = author::CreateModule("AlignVelocityToRandomAxis");
    align.inputs["Randomness"].value = {0,0,0,0}; // keep direction, check speed is preserved
    update.modules.push_back(align);
    auto kill = author::CreateModule("KillParticles");
    kill.inputs["Kill"].value = {0,0,0,0};
    update.modules.push_back(kill);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << "issue: " << (issues.empty() ? std::string{"none"} : issues.front().message);

    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(3));
    const auto position = data.FindAttribute("Particles.Position");
    const auto velocity = data.FindAttribute("Particles.Velocity");
    const auto age = data.FindAttribute("Particles.Age");
    const auto lifetime = data.FindAttribute("Particles.Lifetime");
    std::vector<Value> registers;

    // Torus: distance from the Y axis major circle equals the minor radius.
    data.Write(position,0,{}); data.Write(velocity,0,{});
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].spawn, data, 0, 1, {0, 1, 0, {}}, registers));
    const auto sampled = data.Read(position,0);
    const auto radial = std::sqrt(sampled[0]*sampled[0]+sampled[2]*sampled[2]);
    const auto tubeDistance = std::sqrt((radial-1.0f)*(radial-1.0f)+sampled[1]*sampled[1]);
    EXPECT_NEAR(tubeDistance,0.25f,1e-4f) << "torus sampling must sit on the tube surface";

    // Alignment with zero randomness preserves the speed exactly.
    data.Write(velocity,0,{3,-4,12,0});
    data.Write(age,0,{}); data.Write(lifetime,0,{10,0,0,0});
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    const auto aligned = data.Read(velocity,0);
    EXPECT_NEAR(std::sqrt(aligned[0]*aligned[0]+aligned[1]*aligned[1]+aligned[2]*aligned[2]),13.0f,1e-4f)
        << "aligning must keep the speed";
    EXPECT_NEAR(data.Read(lifetime,0)[0],10.0f,1e-6f) << "a false kill input must not change lifetime";

    // A true kill input drives lifetime below age so compaction removes it.
    auto killedSystem = system;
    author::FindStack(killedSystem.emitters[0],author::Stage::ParticleUpdate)->modules.back().inputs["Kill"].value = {1,0,0,0};
    CompiledSystem killed; std::vector<author::Issue> killedIssues;
    ASSERT_TRUE(author::Compile(killedSystem,killed,killedIssues)) << (killedIssues.empty() ? "" : killedIssues.front().message);
    ParticleDataSet victim; victim.Initialize(killed.asset.emitters[0].attributes,1); ASSERT_TRUE(victim.Spawn(5));
    victim.Write(victim.FindAttribute("Particles.Age"),0,{0.5f,0,0,0});
    victim.Write(victim.FindAttribute("Particles.Lifetime"),0,{10,0,0,0});
    ASSERT_TRUE(RunParticleProgram(killed.emitters[0].update, victim, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_LE(victim.Read(victim.FindAttribute("Particles.Lifetime"),0)[0],
        victim.Read(victim.FindAttribute("Particles.Age"),0)[0])
        << "a true kill input must make the particle expire";
}

TEST(VfxAuthoring, LimitForceRequiresAPrecedingForceAndClampsMagnitude)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem();
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();
    update.modules.push_back(author::CreateModule("LimitForce"));

    // Without an accumulated force there is nothing to clamp.
    CompiledSystem compiled; std::vector<author::Issue> issues;
    EXPECT_FALSE(author::Compile(system,compiled,issues));
    ASSERT_FALSE(issues.empty());
    EXPECT_NE(issues.front().message.find("Limit Force"),std::string::npos);

    // A force larger than the limit is scaled down to exactly the limit.
    auto gravity = author::CreateModule("AccelerationForce");
    gravity.inputs["Acceleration"].value = {0,-100,0,0};
    update.modules.insert(update.modules.begin(),gravity);
    auto limit = author::CreateModule("LimitForce");
    limit.inputs["Force Limit"].value = {10,0,0,0};
    update.modules[1] = limit;
    update.modules.push_back(author::CreateModule("SolveForcesAndVelocity"));
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? "" : issues.front().message);

    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(9));
    data.Write(data.FindAttribute("Particles.Position"),0,{});
    data.Write(data.FindAttribute("Particles.Velocity"),0,{});
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {1.0f, 1, 0, {}}, registers));
    // Force was clamped to 10 before the solver integrated one second.
    EXPECT_NEAR(data.Read(data.FindAttribute("Particles.Velocity"),0)[1],-10.0f,1e-4f);
}
TEST(VfxAuthoring, ThirdBatchWritesMeshRendererAttributesAndPureMathState)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem();
    auto& emitter = system.emitters.front();
    auto& spawn = *author::FindStack(emitter,author::Stage::ParticleSpawn);
    spawn.modules.resize(1);
    spawn.modules[0].inputs["Position"].value = {0,0,0,0};
    spawn.modules[0].inputs["Velocity"].value = {3,0,0,0};
    auto orientation = author::CreateModule("InitialMeshOrientation");
    orientation.inputs["Axis"].value = {0,0,1,0};
    orientation.inputs["Angle"].value = {90,0,0,0};
    spawn.modules.push_back(orientation);

    auto& update = *author::FindStack(emitter,author::Stage::ParticleUpdate);
    update.modules.clear();
    auto counter = author::CreateModule("FrameCounter");
    update.modules.push_back(counter);
    auto distance = author::CreateModule("TrackDistanceTraveled");
    update.modules.push_back(distance);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);

    // The mesh attributes must exist on the emitter without hand editing; the
    // mesh shader already consumed them but nothing could author them before.
    const auto& attributes = compiled.asset.emitters[0].attributes;
    const auto typeOf = [&](std::string_view name) -> int32_t
    {
        const auto found = std::find_if(attributes.begin(),attributes.end(),
            [&](const Attribute& a) { return a.name == name; });
        return found == attributes.end() ? -1 : static_cast<int32_t>(found->type);
    };
    EXPECT_EQ(typeOf("Particles.MeshOrientation"),static_cast<int32_t>(ValueType::Float4));
    EXPECT_EQ(typeOf("Particles.FrameCounter"),static_cast<int32_t>(ValueType::Float));
    EXPECT_EQ(typeOf("Particles.DistanceTraveled"),static_cast<int32_t>(ValueType::Float));

    ParticleDataSet data; data.Initialize(attributes,1); ASSERT_TRUE(data.Spawn(17));
    const auto meshOrientation = data.FindAttribute("Particles.MeshOrientation");
    const auto counterAttr = data.FindAttribute("Particles.FrameCounter");
    const auto distanceAttr = data.FindAttribute("Particles.DistanceTraveled");
    std::vector<Value> registers;

    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].spawn, data, 0, 1, {0, 1, 0, {}}, registers));
    // 90 degrees about Z is (0,0,sin45,cos45).
    const auto quaternion = data.Read(meshOrientation,0);
    EXPECT_NEAR(quaternion[2],0.70710678f,1e-5f) << "90 deg about Z must give sin(45) on Z";
    EXPECT_NEAR(quaternion[3],0.70710678f,1e-5f) << "90 deg about Z must give cos(45) on W";
    EXPECT_NEAR(quaternion[0],0.0f,1e-5f);
    EXPECT_NEAR(quaternion[1],0.0f,1e-5f);

    // Two update ticks at 0.5s each: the counter reaches 2 and, at 3 u/s, the
    // travelled distance reaches 3.
    for (int tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.5f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(counterAttr,0)[0],2.0f,1e-5f);
    EXPECT_NEAR(data.Read(distanceAttr,0)[0],3.0f,1e-5f);

    // A disabled increment must hold the counter instead of advancing it.
    auto held = system;
    author::FindStack(held.emitters[0],author::Stage::ParticleUpdate)->modules.front().inputs["Increment Counter"].value = {0,0,0,0};
    CompiledSystem heldCompiled; std::vector<author::Issue> heldIssues;
    ASSERT_TRUE(author::Compile(held,heldCompiled,heldIssues)) << (heldIssues.empty() ? std::string{"none"} : heldIssues.front().message);
    ParticleDataSet heldData; heldData.Initialize(heldCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(heldData.Spawn(19));
    const auto heldCounter = heldData.FindAttribute("Particles.FrameCounter");
    heldData.Write(heldCounter,0,{5,0,0,0});
    ASSERT_TRUE(RunParticleProgram(heldCompiled.emitters[0].update, heldData, 0, 1, {0.5f, 1, 0, {}}, registers));
    EXPECT_NEAR(heldData.Read(heldCounter,0)[0],5.0f,1e-5f) << "a false increment must not change the counter";
}

TEST(VfxAuthoring, MeshOrientationCompositionRotatesAboutTheGivenAxis)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem();
    auto& emitterSettings = system.emitters.front().settings;
    emitterSettings.renderer = RendererType::Mesh;
    // The mesh renderer validates every slot, so bind one imported mesh.
    emitterSettings.mesh.meshes = {{NLS::Guid::New().ToString(),64}};
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();
    auto spin = author::CreateModule("MeshRotationRate");
    spin.inputs["Axis"].value = {0,0,1,0};
    spin.inputs["Rate"].value = {90,0,0,0};
    update.modules.push_back(spin);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << "issue: " << (issues.empty() ? std::string{"none"} : issues.front().message);
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(23));
    const auto orientation = data.FindAttribute("Particles.MeshOrientation");
    data.Write(orientation,0,{0,0,0,1}); // identity
    std::vector<Value> registers;

    // 90 deg/s over one second must give a quarter turn about Z, and the result
    // must stay a unit quaternion.
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {1.0f, 1, 0, {}}, registers));
    auto value = data.Read(orientation,0);
    const auto length = std::sqrt(value[0]*value[0]+value[1]*value[1]+value[2]*value[2]+value[3]*value[3]);
    EXPECT_NEAR(length,1.0f,1e-5f) << "composed orientation must remain normalised";
    EXPECT_NEAR(value[2],0.70710678f,1e-5f);
    EXPECT_NEAR(value[3],0.70710678f,1e-5f);

    // A second equal step must compose to a half turn about Z.
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {1.0f, 1, 0, {}}, registers));
    value = data.Read(orientation,0);
    EXPECT_NEAR(std::abs(value[3]),0.0f,1e-5f) << "180 degrees about Z lands on W=0";
    EXPECT_NEAR(std::abs(value[2]),1.0f,1e-5f);
}
TEST(VfxAuthoring, FourthBatchWritesDynamicMaterialParametersAndMathState)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem();
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();

    // Only slot 0 carries a literal binding change; the other three keep their
    // defaults and must not be forced into the attribute set.
    auto dynamicParameters = author::CreateModule("DynamicMaterialParameters");
    dynamicParameters.inputs["Index 0"].mode = author::InputMode::Parameter;
    dynamicParameters.inputs["Index 0"].parameter = "User.DynamicZero";
    system.parameters.push_back({NLS::Guid::New().ToString(),"User.DynamicZero",ValueType::Float4,{0.25f,0.5f,0.75f,1}});
    update.modules.push_back(dynamicParameters);

    auto increment = author::CreateModule("IncrementOverTime");
    increment.inputs["Increment"].value = {3,0,0,0};
    update.modules.push_back(increment);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << "issue: " << (issues.empty() ? std::string{"none"} : issues.front().message);

    const auto& attributes = compiled.asset.emitters[0].attributes;
    const auto has = [&](std::string_view name)
    { return std::any_of(attributes.begin(),attributes.end(),[&](const Attribute& a) { return a.name == name; }); };
    // Declaring the module registers all four slots, which matches how the
    // sprite shader binds them; the per-slot write below still only affects the
    // slot that carries a non-default binding.
    EXPECT_TRUE(has("Particles.DynamicMaterialParameter"));
    EXPECT_TRUE(has("Particles.DynamicMaterialParameter3"));
    EXPECT_TRUE(has("Particles.IncrementCounter"));

    ParticleDataSet data; data.Initialize(attributes,1); ASSERT_TRUE(data.Spawn(31));
    const auto dynamicAttr = data.FindAttribute("Particles.DynamicMaterialParameter");
    const auto counterAttr = data.FindAttribute("Particles.IncrementCounter");
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.5f, 1, 0, {}}, registers));
    const auto dynamicValue = data.Read(dynamicAttr,0);
    EXPECT_NEAR(dynamicValue[0],0.25f,1e-5f);
    EXPECT_NEAR(dynamicValue[1],0.5f,1e-5f);
    EXPECT_NEAR(dynamicValue[2],0.75f,1e-5f);
    EXPECT_NEAR(dynamicValue[3],1.0f,1e-5f);
    // Three units per second over a half second step.
    EXPECT_NEAR(data.Read(counterAttr,0)[0],1.5f,1e-5f);
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.5f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(counterAttr,0)[0],3.0f,1e-5f) << "the counter must accumulate across frames";
}

TEST(VfxAuthoring, ConeConstraintAndLineSegmentFractionClampCorrectly)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem();
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();
    auto cone = author::CreateModule("ConstrainVectorToCone");
    cone.inputs["Cone Direction"].value = {0,1,0,0};
    cone.inputs["Cone Angle"].value = {45,0,0,0};
    update.modules.push_back(cone);
    auto segment = author::CreateModule("FindClosestPointOnLineSegment");
    segment.inputs["Segment Start"].value = {0,0,0,0};
    segment.inputs["Segment End"].value = {0,0,1,0};
    update.modules.push_back(segment);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << "issue: " << (issues.empty() ? std::string{"none"} : issues.front().message);
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(37));
    const auto velocity = data.FindAttribute("Particles.Velocity");
    const auto position = data.FindAttribute("Particles.Position");
    const auto fractionAttr = data.FindAttribute("Particles.LineSegmentFraction");
    std::vector<Value> registers;

    // Already inside the cone: the direction and speed must be untouched.
    data.Write(velocity,0,{0,5,0,0});
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    auto value = data.Read(velocity,0);
    EXPECT_NEAR(value[1],5.0f,1e-5f) << "an in-cone vector must not be modified";
    EXPECT_NEAR(std::abs(value[0])+std::abs(value[2]),0.0f,1e-5f);

    // Far outside the cone: the result must sit exactly on the 45 degree shell
    // while preserving the original speed.
    data.Write(velocity,0,{8,1,0,0});
    const auto originalSpeed = std::sqrt(8.0f*8.0f+1.0f);
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    value = data.Read(velocity,0);
    const auto clampedSpeed = std::sqrt(value[0]*value[0]+value[1]*value[1]+value[2]*value[2]);
    EXPECT_NEAR(clampedSpeed,originalSpeed,1e-4f) << "constraining must preserve speed";
    const auto angleCosine = value[1]/clampedSpeed;
    EXPECT_NEAR(angleCosine,std::cos(45.0f*3.14159265f/180.0f),1e-4f)
        << "the constrained vector must land on the cone shell";

    // Fraction along the bound segment clamps at both ends.
    for (const auto& probe : {std::pair{Value{0,0,0.25f,0},0.25f}, std::pair{Value{0,0,-3,0},0.0f}, std::pair{Value{0,0,9,0},1.0f}})
    {
        data.Write(position,0,probe.first);
        ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
        EXPECT_NEAR(data.Read(fractionAttr,0)[0],probe.second,1e-5f);
    }
}
TEST(VfxAuthoring, FifthBatchForcePrimitivesAccumulateAndSolve)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    // Every force primitive needs the solver after it; verify each contributes
    // the direction the reference describes.
    const auto solveStep = [](const char* script, const std::function<void(author::Module&)>& configure,
        const Value& startPosition, const Value& startVelocity, float delta) -> Value
    {
        auto system = author::CreateSystem();
        auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
        update.modules.clear();
        auto force = author::CreateModule(script);
        configure(force);
        update.modules.push_back(force);
        update.modules.push_back(author::CreateModule("SolveForcesAndVelocity"));

        CompiledSystem compiled; std::vector<author::Issue> issues;
        EXPECT_TRUE(author::Compile(system,compiled,issues)) << script << ": " << (issues.empty() ? std::string{"none"} : issues.front().message);
        ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1);
        EXPECT_TRUE(data.Spawn(41));
        data.Write(data.FindAttribute("Particles.Position"),0,startPosition);
        data.Write(data.FindAttribute("Particles.Velocity"),0,startVelocity);
        std::vector<Value> registers;
        const bool ran = RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {delta, 1, 0, {}}, registers);
        EXPECT_TRUE(ran);
        return data.Read(data.FindAttribute("Particles.Velocity"),0);
    };

    // A line pull towards +Z from the origin must add velocity on Z only.
    const auto line = solveStep("LineAttractionForce",[](author::Module& module)
    {
        module.inputs["Line Start"].value = {0,0,0,0};
        module.inputs["Line End"].value = {0,0,1,0};
        module.inputs["Attraction Strength"].value = {4,0,0,0};
    },Value{0,0,-2,0},Value{},1.0f);
    // The particle starts at z=-2 against a unit segment on +Z. The closest
    // point is the segment start, two units away, so the pull is the offset
    // (0,0,2) scaled by the strength 4 -> (0,0,8).
    EXPECT_NEAR(line[2],8.0f,1e-4f) << "line attraction must pull towards the segment";
    EXPECT_NEAR(std::abs(line[0])+std::abs(line[1]),0.0f,1e-4f);

    // A spring at the rest position with zero velocity produces no force.
    const auto restSpring = solveStep("SpringForce",[](author::Module& module)
    {
        module.inputs["Rest Position"].value = {1,2,3,0};
        module.inputs["Spring"].value = {10,0,0,0};
    },Value{1,2,3,0},Value{},1.0f);
    EXPECT_NEAR(restSpring[0]+restSpring[1]+restSpring[2],0.0f,1e-4f)
        << "a spring already at rest must not accelerate the particle";

    // Displacing the particle away from rest must pull it back.
    const auto displaced = solveStep("SpringForce",[](author::Module& module)
    {
        module.inputs["Rest Position"].value = {0,0,0,0};
        module.inputs["Spring"].value = {10,0,0,0};
        module.inputs["Damping"].value = {0,0,0,0};
    },Value{1,0,0,0},Value{},0.1f);
    EXPECT_LT(displaced[0],0.0f) << "a displaced spring must pull the particle back towards rest";

    // Repulsion with no falloff pushes straight away from the repulsor.
    const auto repulsion = solveStep("PointRepulsionForce",[](author::Module& module)
    {
        module.inputs["Repulsor Position"].value = {0,0,0,0};
        module.inputs["Repulsion Strength"].value = {6,0,0,0};
        module.inputs["Falloff Radius"].value = {10,0,0,0};
    },Value{2,0,0,0},Value{},1.0f);
    // At distance 2 with a radius of 10 the falloff is (1 - 2/10) = 0.8, so the
    // impulse is 6*0.8 = 4.8 along +X for a one second step.
    EXPECT_NEAR(repulsion[0],4.8f,1e-3f) << "repulsion must push away with distance falloff";

    // Wind blends the velocity towards the wind vector.
    const auto wind = solveStep("WindForce",[](author::Module& module)
    {
        module.inputs["Wind Velocity"].value = {10,0,0,0};
        module.inputs["Wind Scale"].value = {2,0,0,0};
    },Value{},Value{},0.25f);
    EXPECT_GT(wind[0],0.0f) << "wind must accelerate the particle towards the wind direction";
    EXPECT_LT(wind[0],20.0f) << "a partial step must not snap straight to the wind velocity";
}
TEST(VfxAuthoring, SixthBatchKillVolumeDoOnceMassAndRegionMask)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    const auto build = [](const char* script, const std::function<void(author::Module&)>& configure,
        CompiledSystem& compiled, std::vector<author::Issue>& issues)
    {
        auto system = author::CreateSystem();
        auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
        update.modules.clear();
        auto module = author::CreateModule(script);
        configure(module);
        update.modules.push_back(module);
        const bool ok = author::Compile(system,compiled,issues);
        EXPECT_TRUE(ok) << script << ": " << (issues.empty() ? std::string{"none"} : issues.front().message);
        return ok;
    };

    // Killing inside a sphere drives Lifetime below Age only for particles that
    // are actually inside it, and the invert flag flips the test.
    CompiledSystem kill; std::vector<author::Issue> killIssues;
    const bool killBuilt = build("KillParticlesInVolume",[](author::Module& module)
    {
        module.inputs["Volume Center"].value = {0,0,0,0};
        module.inputs["Volume Radius"].value = {1,0,0,0};
        module.inputs["Invert"].value = {0,0,0,0};
    },kill,killIssues);
    ASSERT_TRUE(killBuilt);
    ParticleDataSet data; data.Initialize(kill.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(43));
    const auto position = data.FindAttribute("Particles.Position");
    const auto age = data.FindAttribute("Particles.Age");
    const auto lifetime = data.FindAttribute("Particles.Lifetime");
    std::vector<Value> registers;
    const auto lifetimeAfter = [&](Value probe)
    {
        data.Write(position,0,probe); data.Write(age,0,{0.5f,0,0,0}); data.Write(lifetime,0,{10,0,0,0});
        EXPECT_TRUE(RunParticleProgram(kill.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
        return data.Read(lifetime,0)[0];
    };
    EXPECT_LE(lifetimeAfter(Value{0,0,0,0}),0.5f) << "a particle inside the kill sphere must expire";
    EXPECT_NEAR(lifetimeAfter(Value{5,0,0,0}),10.0f,1e-5f) << "a particle outside the sphere must survive";

    // DoOnce latches: the first true trigger reports one, later frames zero.
    CompiledSystem once; std::vector<author::Issue> onceIssues;
    ASSERT_TRUE(build("DoOnce",[](author::Module& module) { module.inputs["Trigger"].value = {1,0,0,0}; },once,onceIssues));
    ParticleDataSet onceData; onceData.Initialize(once.asset.emitters[0].attributes,1); ASSERT_TRUE(onceData.Spawn(47));
    const auto latchAttr = onceData.FindAttribute("Particles.DoOnce");
    EXPECT_NEAR(onceData.Read(latchAttr,0)[0],0.0f,1e-5f) << "the latch starts clear";
    ASSERT_TRUE(RunParticleProgram(once.emitters[0].update, onceData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(onceData.Read(latchAttr,0)[0],1.0f,1e-5f) << "the first true trigger must latch";
    for (int frame = 0; frame < 3; ++frame)
        ASSERT_TRUE(RunParticleProgram(once.emitters[0].update, onceData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(onceData.Read(latchAttr,0)[0],1.0f,1e-5f) << "a latched DoOnce must stay latched";
    // A cleared trigger on a fresh particle must leave the latch alone.
    ParticleDataSet quiet; quiet.Initialize(once.asset.emitters[0].attributes,1); ASSERT_TRUE(quiet.Spawn(71));
    ASSERT_TRUE(RunParticleProgram(once.emitters[0].update, quiet, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(quiet.Read(quiet.FindAttribute("Particles.DoOnce"),0)[0],1.0f,1e-5f);

    // Mass from density and volume, then kinetic energy from mass and speed.
    CompiledSystem mass; std::vector<author::Issue> massIssues;
    ASSERT_TRUE(build("CalculateMassAndRotationalInertiaByVolume",[](author::Module& module)
    { module.inputs["Density"].value = {2,0,0,0}; },mass,massIssues));
    ParticleDataSet massData; massData.Initialize(mass.asset.emitters[0].attributes,1); ASSERT_TRUE(massData.Spawn(53));
    massData.Write(massData.FindAttribute("Particles.Size"),0,{3,0,0,0});
    ASSERT_TRUE(RunParticleProgram(mass.emitters[0].update, massData, 0, 1, {0.1f, 1, 0, {}}, registers));
    // Volume 27, density 2 -> mass 54; inertia 54*9/6 = 81.
    EXPECT_NEAR(massData.Read(massData.FindAttribute("Particles.Mass"),0)[0],54.0f,1e-4f);
    EXPECT_NEAR(massData.Read(massData.FindAttribute("Particles.RotationalInertia"),0)[0],81.0f,1e-4f);

    CompiledSystem energy; std::vector<author::Issue> energyIssues;
    ASSERT_TRUE(build("FindKineticAndPotentialEnergy",[](author::Module& module)
    {
        module.inputs["Mass"].value = {2,0,0,0};
        module.inputs["Potential Energy"].value = {5,0,0,0};
    },energy,energyIssues));
    ParticleDataSet energyData; energyData.Initialize(energy.asset.emitters[0].attributes,1); ASSERT_TRUE(energyData.Spawn(59));
    energyData.Write(energyData.FindAttribute("Particles.Velocity"),0,{3,4,0,0});
    ASSERT_TRUE(RunParticleProgram(energy.emitters[0].update, energyData, 0, 1, {0.1f, 1, 0, {}}, registers));
    // 0.5 * 2 * 25 = 25 kinetic, plus 5 potential -> 30 total.
    EXPECT_NEAR(energyData.Read(energyData.FindAttribute("Particles.KineticEnergy"),0)[0],25.0f,1e-4f);
    EXPECT_NEAR(energyData.Read(energyData.FindAttribute("Particles.TotalEnergy"),0)[0],30.0f,1e-4f);

    // The region mask is one inside the box and zero outside it.
    CompiledSystem region; std::vector<author::Issue> regionIssues;
    ASSERT_TRUE(build("RegionMask",[](author::Module& module)
    {
        module.inputs["Region Minimum"].value = {-1,-1,-1,0};
        module.inputs["Region Maximum"].value = {1,1,1,0};
    },region,regionIssues));
    ParticleDataSet regionData; regionData.Initialize(region.asset.emitters[0].attributes,1); ASSERT_TRUE(regionData.Spawn(61));
    const auto maskAttr = regionData.FindAttribute("Particles.RegionMask");
    for (const auto& probe : {std::pair{Value{0,0,0,0},1.0f}, std::pair{Value{2,0,0,0},0.0f}, std::pair{Value{0,-3,0,0},0.0f}})
    {
        regionData.Write(regionData.FindAttribute("Particles.Position"),0,probe.first);
        ASSERT_TRUE(RunParticleProgram(region.emitters[0].update, regionData, 0, 1, {0.1f, 1, 0, {}}, registers));
        EXPECT_NEAR(regionData.Read(maskAttr,0)[0],probe.second,1e-5f);
    }
}
TEST(VfxAuthoring, SeventhBatchDerivedVelocityOwnerScaleNoiseAndSpring)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    const auto buildOne = [](const char* script, const std::function<void(author::Module&)>& configure,
        CompiledSystem& compiled, std::vector<author::Issue>& issues)
    {
        auto system = author::CreateSystem();
        auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
        update.modules.clear();
        auto module = author::CreateModule(script);
        configure(module);
        update.modules.push_back(module);
        EXPECT_TRUE(author::Compile(system,compiled,issues)) << script << ": " << (issues.empty() ? std::string{"none"} : issues.front().message);
    };

    // Derived velocity: a 3 unit move over a 0.5s step is 6 units per second.
    CompiledSystem derived; std::vector<author::Issue> derivedIssues;
    buildOne("CalculateAccurateVelocity",[](author::Module& module) { module.inputs["Velocity Scale"].value = {1,0,0,0}; },
        derived,derivedIssues);
    ParticleDataSet data; data.Initialize(derived.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(67));
    const auto position = data.FindAttribute("Particles.Position");
    const auto previous = data.FindAttribute("Particles.PreviousPosition");
    const auto velocity = data.FindAttribute("Particles.Velocity");
    data.Write(previous,0,{0,0,0,0}); data.Write(position,0,{3,0,0,0});
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(derived.emitters[0].update, data, 0, 1, {0.5f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(velocity,0)[0],6.0f,1e-4f) << "velocity must be the position delta over delta time";
    EXPECT_NEAR(data.Read(previous,0)[0],3.0f,1e-4f) << "the previous position must advance to the sampled one";

    // Owner scale multiplies position per axis and size by the largest axis.
    CompiledSystem scaled; std::vector<author::Issue> scaledIssues;
    buildOne("ApplyOwnerScaleToAttributes",[](author::Module& module)
    { module.inputs["Owner Scale"].value = {2,3,4,0}; },scaled,scaledIssues);
    ParticleDataSet scaledData; scaledData.Initialize(scaled.asset.emitters[0].attributes,1); ASSERT_TRUE(scaledData.Spawn(73));
    scaledData.Write(scaledData.FindAttribute("Particles.Position"),0,{1,1,1,0});
    scaledData.Write(scaledData.FindAttribute("Particles.Size"),0,{1,0,0,0});
    ASSERT_TRUE(RunParticleProgram(scaled.emitters[0].update, scaledData, 0, 1, {0.1f, 1, 0, {}}, registers));
    const auto scaledPosition = scaledData.Read(scaledData.FindAttribute("Particles.Position"),0);
    EXPECT_NEAR(scaledPosition[0],2.0f,1e-5f);
    EXPECT_NEAR(scaledPosition[1],3.0f,1e-5f);
    EXPECT_NEAR(scaledPosition[2],4.0f,1e-5f);
    EXPECT_NEAR(scaledData.Read(scaledData.FindAttribute("Particles.Size"),0)[0],4.0f,1e-5f)
        << "size follows the largest owner axis";

    // The spring relaxes towards its rest value and converges across steps.
    CompiledSystem spring; std::vector<author::Issue> springIssues;
    buildOne("SolveFloatSpringConstraint",[](author::Module& module)
    {
        module.inputs["Rest Value"].value = {0,0,0,0};
        module.inputs["Spring"].value = {10,0,0,0};
        module.inputs["Damping"].value = {2,0,0,0};
    },spring,springIssues);
    ParticleDataSet springData; springData.Initialize(spring.asset.emitters[0].attributes,1); ASSERT_TRUE(springData.Spawn(79));
    const auto springValue = springData.FindAttribute("Particles.SpringValue");
    const auto springVelocity = springData.FindAttribute("Particles.SpringVelocity");
    springData.Write(springValue,0,{1,0,0,0}); springData.Write(springVelocity,0,{});
    // With k=10, c=2 and dt=0.01 the discrete system converges in a few
    // seconds of simulated time; 1200 steps is 12 seconds.
    const auto initialValue = springData.Read(springValue,0)[0];
    for (int step = 0; step < 200; ++step)
        ASSERT_TRUE(RunParticleProgram(spring.emitters[0].update, springData, 0, 1, {0.01f, 1, 0, {}}, registers));
    const auto midValue = springData.Read(springValue,0)[0];
    EXPECT_LT(std::abs(midValue),std::abs(initialValue)) << "the spring must relax towards rest";
    for (int step = 0; step < 1200; ++step)
        ASSERT_TRUE(RunParticleProgram(spring.emitters[0].update, springData, 0, 1, {0.01f, 1, 0, {}}, registers));
    EXPECT_NEAR(springData.Read(springValue,0)[0],0.0f,1e-2f) << "the spring must converge to its rest value";
    EXPECT_NEAR(springData.Read(springVelocity,0)[0],0.0f,1e-2f);
}
TEST(VfxAuthoring, EventReceiverSpawnsFromAnotherEmittersPublishedEvents)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    // A source emitter that dies immediately, and a receiver that spawns one
    // particle per death event from it. The receiver has no schedule of its own.
    auto system = author::CreateSystem("Event system");
    auto& source = system.emitters.front();
    source.settings.capacity = 16;
    source.settings.spawnRate = 0;
    source.settings.duration = 1.0e6;
    // This source explicitly publishes only death events; its burst locations
    // must not create a recursive stream of location events.
    auto& generateStack = source.stacks.emplace_back();
    generateStack.id = NLS::Guid::New().ToString();
    generateStack.name = "Generate";
    generateStack.stage = author::Stage::Event;
    generateStack.modules.push_back(author::CreateModule("GenerateDeathEvent"));
    // The Emitter Update stack rebuilds the schedule, so the burst and the
    // continuous rate must both be expressed as modules.
    if (auto* update = author::FindStack(source,author::Stage::EmitterUpdate))
        update->modules.erase(std::remove_if(update->modules.begin(),update->modules.end(),
            [](const author::Module& module) { return module.script == "SpawnRate"; }),update->modules.end());
    if (auto* update = author::FindStack(source,author::Stage::EmitterUpdate))
    {
        auto burst = author::CreateModule("SpawnBurst");
        burst.inputs["Time"].value = {0,0,0,0};
        burst.inputs["Count"].value = {4,0,0,0};
        update->modules.push_back(burst);
    }
    // The spawn stack's Initialize Particle module writes Lifetime, so the
    // lifetime must be set through that module rather than the attribute default.
    for (auto& module : author::FindStack(source,author::Stage::ParticleSpawn)->modules)
        if (module.script == "InitializeParticle")
        module.inputs["Lifetime"].value = {0.5f,0,0,0};

    auto receiver = author::CreateEmitter("Receiver");
    receiver.settings.capacity = 64;
    receiver.settings.spawnRate = 0;
    receiver.settings.bursts.clear();
    // CreateEmitter seeds Emitter Update with Emitter State and Spawn Rate
    // modules that would re-add a schedule during lowering; clear them so the
    // receiver can only spawn from received events.
    if (auto* update = author::FindStack(receiver,author::Stage::EmitterUpdate))
        update->modules.clear();
    // Build the handler stack directly: CreateSimulationStage is for the
    // Simulation stage and leaves iterations at zero, which a non-simulation
    // stack rejects.
    auto& eventStack = receiver.stacks.emplace_back();
    eventStack.id = NLS::Guid::New().ToString();
    eventStack.name = "Handlers";
    eventStack.stage = author::Stage::Event;
    auto receive = author::CreateModule("ReceiveDeathEvent");
    receive.inputs["Spawn Count"].value = {1,0,0,0};
    receive.inputs["Random Age"].value = {0,0,0,0};
    eventStack.modules.push_back(receive);
    system.emitters.push_back(std::move(receiver));

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    ASSERT_EQ(compiled.asset.emitters[1].eventHandlers.size(),1u);
    EXPECT_EQ(static_cast<uint32_t>(compiled.asset.emitters[1].eventHandlers[0].receiver.eventKind),
        static_cast<uint32_t>(ParticleEventSelection::Death));

    std::string error;
    auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;

    // The System splits a tick into 60 Hz sub-steps and the source bursts every
    // loop, so drive a single explicit sub-step to observe the event contract
    // without loop multiplication: one burst, no deaths yet, empty receiver.
    ASSERT_TRUE(instance->Tick(1.0/60.0));
    const auto burstCount = instance->CpuEmitters()[0]->Data().Count();
    EXPECT_GT(burstCount,0u) << "the burst must spawn on the first step";
    EXPECT_EQ(instance->CpuEmitters()[0]->Statistics().died,0u);
    EXPECT_EQ(instance->CpuEmitters()[1]->Data().Count(),0u)
        << "the receiver must not spawn before any event is published";

    // Advance past the 0.5s lifetime. The source dies out and
    // the receiver mirrors the published death events one for one.
    // The System dispatches events per sub-step, so the receiver count tracks
    // the death count with at most one sub-step of lag. Drive enough steps for
    // the source's 0.5s lifetime to elapse and observe the correspondence.
    for (int step = 0; step < 60; ++step)
    {
        ASSERT_TRUE(instance->Tick(1.0/60.0));
    }
    const auto died = instance->CpuEmitters()[0]->Statistics().died;
    EXPECT_GE(died,burstCount) << "the burst must have expired";
    EXPECT_GT(died,0u) << "at least one death event must be published for the receiver";
    // Compare like with like: track the receiver count across a single
    // sub-step transition where the source publishes a known number of deaths.
    // Each sustained tick dies one source particle and the receiver mirrors it.
    const auto beforeDeaths = instance->CpuEmitters()[0]->Statistics().died;
    const auto beforeReceived = instance->CpuEmitters()[1]->Statistics().spawned;
    ASSERT_TRUE(instance->Tick(1.0/60.0));
    const auto deltaDeaths = instance->CpuEmitters()[0]->Statistics().died-beforeDeaths;
    const auto deltaReceived = instance->CpuEmitters()[1]->Statistics().spawned-beforeReceived;
    EXPECT_EQ(deltaReceived,deltaDeaths)
        << "within one sub-step the receiver must spawn one particle per published death event";
    EXPECT_GT(instance->CpuEmitters()[1]->Statistics().spawned,0u)
        << "the receiver must have spawned from events over the run";
    EXPECT_EQ(instance->CpuEmitters()[1]->Data().Count(),instance->CpuEmitters()[1]->Statistics().spawned)
        << "received particles use the source lifetime and must not have expired yet";
    // Event payload positions must reach the receiver's particle position column.
    const auto& received = instance->CpuEmitters()[1]->Data();
    const auto position = received.FindAttribute("Particles.Position");
    ASSERT_GE(position,0);
    for (uint32_t particle = 0; particle < received.Count(); ++particle)
    {
        const auto value = received.Read(position,particle);
        EXPECT_TRUE(std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]));
    }
}

TEST(VfxAuthoring, EventHandlerRunsPerEventWithPayload)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    // The receiver's Event Handler reads the published particle id to choose a
    // per-event spawn count, and writes Emitter.ReceivePosition to override the
    // birth position. This proves that the handler runs once per event with
    // Emitter.Event* payload values and that its receive writes reach spawn.
    auto handlerAsset = author::CreateScript(author::AssetKind::Module,"Event Spawn By Position");
    auto& handler = handlerAsset.versions.front();
    handler.stages = {author::Stage::Event};
    handler.inputs.clear();
    handler.graph.stage = ScriptStage::Event;
    handler.graph.nodes.clear(); handler.graph.nextNodeId = 1;
    auto& graph = handler.graph;
    const auto eventId = AddNode(graph,Operation::Parameter);
    FindNode(graph,eventId)->name = "Emitter.EventParticleId";
    const auto zero = AddNode(graph,Operation::Constant);
    FindNode(graph,zero)->value = {0,0,0,0};
    const auto greater = AddNode(graph,Operation::GreaterThan);
    FindNode(graph,greater)->inputs = {eventId,zero};
    const auto two = AddNode(graph,Operation::Constant);
    FindNode(graph,two)->value = {2,0,0,0};
    const auto one = AddNode(graph,Operation::Constant);
    FindNode(graph,one)->value = {1,0,0,0};
    const auto select = AddNode(graph,Operation::Select);
    FindNode(graph,select)->inputs = {greater,two,one};
    const auto spawnCount = AddNode(graph,Operation::WriteAttribute);
    FindNode(graph,spawnCount)->name = "Emitter.ReceiveSpawnCount";
    FindNode(graph,spawnCount)->inputs = {select};
    const auto deathKind = AddNode(graph,Operation::Constant);
    FindNode(graph,deathKind)->value = {2,0,0,0};
    const auto receiveKind = AddNode(graph,Operation::WriteAttribute);
    FindNode(graph,receiveKind)->name = "Emitter.ReceiveKind";
    FindNode(graph,receiveKind)->inputs = {deathKind};
    const auto spawnPosition = AddNode(graph,Operation::Constant,ValueType::Float3);
    FindNode(graph,spawnPosition)->value = {42,0,0,0};
    const auto receivePosition = AddNode(graph,Operation::WriteAttribute,ValueType::Float3);
    FindNode(graph,receivePosition)->name = "Emitter.ReceivePosition";
    FindNode(graph,receivePosition)->inputs = {spawnPosition};

    author::SourceLibrary library; std::string error;
    ASSERT_TRUE(library.Add(handlerAsset,{},error)) << error;

    auto system = author::CreateSystem("Payload event system");
    auto& source = system.emitters.front();
    source.settings.capacity = 16;
    source.settings.spawnRate = 0;
    source.settings.duration = 1.0e6;
    auto& sourceUpdate = *author::FindStack(source,author::Stage::EmitterUpdate);
    sourceUpdate.modules.erase(std::remove_if(sourceUpdate.modules.begin(),sourceUpdate.modules.end(),
        [](const author::Module& module) { return module.script == "SpawnRate"; }),sourceUpdate.modules.end());
    auto burst = author::CreateModule("SpawnBurst");
    burst.inputs["Time"].value = {0,0,0,0};
    burst.inputs["Count"].value = {4,0,0,0};
    sourceUpdate.modules.push_back(burst);
    for (auto& module : author::FindStack(source,author::Stage::ParticleSpawn)->modules)
    {
        if (module.script == "InitializeParticle") module.inputs["Lifetime"].value = {0.5f,0,0,0};
        if (module.script == "BoxLocation")
        {
            module.inputs["Center"].value = {10,0,0,0};
            module.inputs["Extent"].value = {1,1,1,0};
        }
    }
    auto& generateStack = source.stacks.emplace_back();
    generateStack.id = NLS::Guid::New().ToString();
    generateStack.name = "Generate";
    generateStack.stage = author::Stage::Event;
    generateStack.modules.push_back(author::CreateModule("GenerateDeathEvent"));

    auto receiver = author::CreateEmitter("Payload Receiver");
    receiver.settings.capacity = 64;
    receiver.settings.spawnRate = 0;
    receiver.settings.bursts.clear();
    if (auto* update = author::FindStack(receiver,author::Stage::EmitterUpdate)) update->modules.clear();
    auto& eventStack = receiver.stacks.emplace_back();
    eventStack.id = NLS::Guid::New().ToString();
    eventStack.name = "Handlers";
    eventStack.stage = author::Stage::Event;
    eventStack.modules.push_back(author::InstantiateModule(handlerAsset));
    system.emitters.push_back(std::move(receiver));

    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);

    auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    ASSERT_TRUE(instance->Tick(1.0/60.0));
    EXPECT_EQ(instance->CpuEmitters()[1]->Data().Count(),0u);
    for (int step = 0; step < 60; ++step) ASSERT_TRUE(instance->Tick(1.0/60.0));

    const auto died = instance->CpuEmitters()[0]->Statistics().died;
    ASSERT_GT(died,0u);
    EXPECT_EQ(instance->CpuEmitters()[1]->Statistics().spawned,died*2u)
        << "each death event must run the handler and use its payload-driven count";
    const auto& received = instance->CpuEmitters()[1]->Data();
    const auto position = received.FindAttribute("Particles.Position");
    ASSERT_GE(position,0);
    ASSERT_GT(received.Count(),0u);
    const auto first = received.Read(position,0);
    EXPECT_FLOAT_EQ(first[0],42.0f);
    EXPECT_TRUE(std::isfinite(first[1]) && std::isfinite(first[2]));
}

TEST(VfxAuthoring, MultipleEventHandlersExecuteIndependently)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    auto system = author::CreateSystem("Multiple event handlers");
    auto& source = system.emitters.front();
    source.settings.capacity = 16;
    source.settings.spawnRate = 0;
    source.settings.duration = 1.0e6;
    auto& update = *author::FindStack(source,author::Stage::EmitterUpdate);
    update.modules.erase(std::remove_if(update.modules.begin(),update.modules.end(),
        [](const author::Module& module) { return module.script == "SpawnRate"; }),update.modules.end());
    auto burst = author::CreateModule("SpawnBurst");
    burst.inputs["Time"].value = {0,0,0,0};
    burst.inputs["Count"].value = {2,0,0,0};
    update.modules.push_back(burst);
    for (auto& module : author::FindStack(source,author::Stage::ParticleSpawn)->modules)
        if (module.script == "InitializeParticle") module.inputs["Lifetime"].value = {0.5f,0,0,0};
    auto& generate = source.stacks.emplace_back();
    generate.id = NLS::Guid::New().ToString();
    generate.name = "Generate";
    generate.stage = author::Stage::Event;
    generate.modules.push_back(author::CreateModule("GenerateDeathEvent"));

    auto receiver = author::CreateEmitter("Multi Handler Receiver");
    receiver.settings.capacity = 64;
    receiver.settings.spawnRate = 0;
    receiver.settings.bursts.clear();
    if (auto* emitterUpdate = author::FindStack(receiver,author::Stage::EmitterUpdate)) emitterUpdate->modules.clear();
    for (const auto count : {1.0f,2.0f})
    {
        auto& stack = receiver.stacks.emplace_back();
        stack.id = NLS::Guid::New().ToString();
        stack.name = count > 1 ? "Handler B" : "Handler A";
        stack.stage = author::Stage::Event;
        auto module = author::CreateModule("ReceiveDeathEvent");
        module.inputs["Spawn Count"].value = {count,0,0,0};
        stack.modules.push_back(module);
    }
    system.emitters.push_back(std::move(receiver));

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    ASSERT_EQ(compiled.asset.emitters[1].eventHandlers.size(),2u);
    ASSERT_EQ(compiled.emitters[1].eventHandlers.size(),2u);
    EXPECT_EQ(compiled.asset.emitters[1].eventHandlers[0].name,"Handler A");
    EXPECT_EQ(compiled.asset.emitters[1].eventHandlers[1].name,"Handler B");
    EXPECT_EQ(static_cast<uint32_t>(compiled.asset.emitters[1].eventHandlers[0].receiver.eventKind),
        static_cast<uint32_t>(ParticleEventSelection::Death));
    EXPECT_EQ(static_cast<uint32_t>(compiled.asset.emitters[1].eventHandlers[1].receiver.eventKind),
        static_cast<uint32_t>(ParticleEventSelection::Death));

    std::string error;
    CompiledSystem restored;
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled,false),restored,error)) << error;
    ASSERT_EQ(restored.emitters[1].eventHandlers.size(),2u);
    ASSERT_EQ(restored.emitters[1].eventParticles.size(),2u);
    ASSERT_EQ(restored.asset.emitters[1].eventHandlers[1].name,"Handler B");
    auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    ASSERT_TRUE(instance->Tick(1.0/60.0));
    for (int step = 0; step < 60; ++step) ASSERT_TRUE(instance->Tick(1.0/60.0));
    const auto died = instance->CpuEmitters()[0]->Statistics().died;
    ASSERT_GT(died,0u);
    EXPECT_EQ(instance->CpuEmitters()[1]->Statistics().spawned,died*3u)
        << "both handlers must run for every event with their own spawn count";
}

TEST(VfxAuthoring, EventHandlerMaxEventsPerFrameLimitsConsumption)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    auto system = author::CreateSystem("Event consumption limit");
    auto& source = system.emitters.front();
    source.settings.capacity = 16;
    source.settings.spawnRate = 0;
    source.settings.duration = 1.0e6;
    auto& update = *author::FindStack(source,author::Stage::EmitterUpdate);
    update.modules.erase(std::remove_if(update.modules.begin(),update.modules.end(),
        [](const author::Module& module) { return module.script == "SpawnRate"; }),update.modules.end());
    auto burst = author::CreateModule("SpawnBurst");
    burst.inputs["Time"].value = {0,0,0,0};
    burst.inputs["Count"].value = {4,0,0,0};
    update.modules.push_back(burst);
    for (auto& module : author::FindStack(source,author::Stage::ParticleSpawn)->modules)
        if (module.script == "InitializeParticle") module.inputs["Lifetime"].value = {0.5f,0,0,0};
    auto& generate = source.stacks.emplace_back();
    generate.id = NLS::Guid::New().ToString();
    generate.name = "Generate";
    generate.stage = author::Stage::Event;
    generate.modules.push_back(author::CreateModule("GenerateDeathEvent"));

    auto receiver = author::CreateEmitter("Limited Receiver");
    receiver.settings.capacity = 64;
    receiver.settings.spawnRate = 0;
    receiver.settings.bursts.clear();
    if (auto* emitterUpdate = author::FindStack(receiver,author::Stage::EmitterUpdate)) emitterUpdate->modules.clear();
    auto& stack = receiver.stacks.emplace_back();
    stack.id = NLS::Guid::New().ToString();
    stack.name = "Limited Handler";
    stack.stage = author::Stage::Event;
    auto properties = author::CreateModule("EventHandlerProperties");
    properties.inputs["Source Emitter Index"].value = {0,0,0,0};
    properties.inputs["Max Events Per Frame"].value = {1,0,0,0};
    stack.modules.push_back(properties);
    auto receive = author::CreateModule("ReceiveDeathEvent");
    receive.inputs["Spawn Count"].value = {1,0,0,0};
    stack.modules.push_back(receive);
    system.emitters.push_back(std::move(receiver));

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    ASSERT_EQ(compiled.asset.emitters[1].eventHandlers.size(),1u);
    EXPECT_EQ(compiled.asset.emitters[1].eventHandlers[0].receiver.maxEventsPerFrame,1u);
    EXPECT_EQ(compiled.asset.emitters[1].eventHandlers[0].receiver.sourceEmitter,system.emitters[0].settings.id);

    std::string error;
    auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    uint64_t previousDied = 0, previousSpawned = 0, largestDeathDelta = 0;
    for (int step = 0; step < 60; ++step)
    {
        ASSERT_TRUE(instance->Tick(1.0/60.0));
        const auto died = instance->CpuEmitters()[0]->Statistics().died;
        const auto spawned = instance->CpuEmitters()[1]->Statistics().spawned;
        largestDeathDelta = (std::max)(largestDeathDelta,died-previousDied);
        if (died > previousDied)
            EXPECT_LE(spawned-previousSpawned,1u)
                << "a handler may consume at most one event per System tick";
        previousDied = died; previousSpawned = spawned;
    }
    EXPECT_GT(largestDeathDelta,1u) << "the source must publish multiple deaths in one tick";
}

TEST(VfxAuthoring, EventHandlerEveryParticleAppliesPayloadToExistingParticles)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    auto system = author::CreateSystem("Every particle handler");
    auto& source = system.emitters.front();
    source.settings.capacity = 8;
    source.settings.spawnRate = 0;
    source.settings.duration = 1.0e6;
    auto& sourceUpdate = *author::FindStack(source,author::Stage::EmitterUpdate);
    sourceUpdate.modules.erase(std::remove_if(sourceUpdate.modules.begin(),sourceUpdate.modules.end(),
        [](const author::Module& module) { return module.script == "SpawnRate"; }),sourceUpdate.modules.end());
    auto sourceBurst = author::CreateModule("SpawnBurst");
    sourceBurst.inputs["Time"].value = {0,0,0,0};
    sourceBurst.inputs["Count"].value = {1,0,0,0};
    sourceUpdate.modules.push_back(sourceBurst);
    for (auto& module : author::FindStack(source,author::Stage::ParticleSpawn)->modules)
        if (module.script == "InitializeParticle") module.inputs["Lifetime"].value = {0.2f,0,0,0};
    auto& generate = source.stacks.emplace_back();
    generate.id = NLS::Guid::New().ToString();
    generate.name = "Generate";
    generate.stage = author::Stage::Event;
    generate.modules.push_back(author::CreateModule("GenerateDeathEvent"));

    auto receiver = author::CreateEmitter("Every Particle Receiver");
    receiver.settings.capacity = 16;
    receiver.settings.spawnRate = 0;
    receiver.settings.bursts.clear();
    if (auto* update = author::FindStack(receiver,author::Stage::EmitterUpdate))
    {
        update->modules.clear();
        auto burst = author::CreateModule("SpawnBurst");
        burst.inputs["Time"].value = {0,0,0,0};
        burst.inputs["Count"].value = {4,0,0,0};
        update->modules.push_back(burst);
    }
    for (auto& module : author::FindStack(receiver,author::Stage::ParticleSpawn)->modules)
        if (module.script == "InitializeParticle") module.inputs["Lifetime"].value = {2,0,0,0};
    auto& eventStack = receiver.stacks.emplace_back();
    eventStack.id = NLS::Guid::New().ToString();
    eventStack.name = "Every Particle Handler";
    eventStack.stage = author::Stage::Event;
    auto properties = author::CreateModule("EventHandlerProperties");
    properties.inputs["Source Emitter Index"].value = {0,0,0,0};
    properties.inputs["Execution Mode"].value = {1,0,0,0};
    eventStack.modules.push_back(properties);
    auto receive = author::CreateModule("ReceiveDeathEvent");
    receive.inputs["Spawn Count"].value = {99,0,0,0};
    eventStack.modules.push_back(receive);
    auto sizeAsset = author::CreateScript(author::AssetKind::Module,"Event Set Size");
    auto& sizeVersion = sizeAsset.versions.front();
    sizeVersion.stages = {author::Stage::Event};
    sizeVersion.inputs.clear();
    sizeVersion.graph.stage = ScriptStage::Event;
    sizeVersion.graph.nodes.clear(); sizeVersion.graph.nextNodeId = 1;
    const auto half = AddNode(sizeVersion.graph,Operation::Constant);
    FindNode(sizeVersion.graph,half)->value = {0.5f,0,0,0};
    const auto sizeWrite = AddNode(sizeVersion.graph,Operation::WriteAttribute);
    FindNode(sizeVersion.graph,sizeWrite)->name = "Particles.Size";
    FindNode(sizeVersion.graph,sizeWrite)->inputs = {half};
    author::SourceLibrary library; std::string error;
    ASSERT_TRUE(library.Add(sizeAsset,{},error)) << error;
    eventStack.modules.push_back(author::InstantiateModule(sizeAsset));
    system.emitters.push_back(std::move(receiver));

    CompiledSystem compiled; std::vector<author::SourceDependency> dependencies; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,library,compiled,dependencies,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    ASSERT_EQ(compiled.asset.emitters[1].eventHandlers.size(),1u);
    ASSERT_EQ(compiled.emitters[1].eventParticles.size(),1u);
    EXPECT_FALSE(compiled.emitters[1].eventParticles[0].instructions.empty());
    EXPECT_EQ(compiled.asset.emitters[1].eventHandlers[0].receiver.executionMode,EventExecutionMode::EveryParticle);
    const auto eventKind = std::find_if(compiled.asset.emitters[1].attributes.begin(),compiled.asset.emitters[1].attributes.end(),
        [](const auto& attribute) { return attribute.name == "Particles.EventKind"; });
    ASSERT_NE(eventKind,compiled.asset.emitters[1].attributes.end());

    auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    ASSERT_TRUE(instance->Tick(1.0/60.0));
    ASSERT_EQ(instance->CpuEmitters()[1]->Data().Count(),4u);
    for (int step = 0; step < 30; ++step) ASSERT_TRUE(instance->Tick(1.0/60.0));

    EXPECT_EQ(instance->CpuEmitters()[1]->Statistics().spawned,4u)
        << "Every Particle mode must not spawn new particles";
    const auto& data = instance->CpuEmitters()[1]->Data();
    const auto kindAttribute = data.FindAttribute("Particles.EventKind");
    ASSERT_GE(kindAttribute,0);
    ASSERT_GT(data.Count(),0u);
    const auto sizeAttribute = data.FindAttribute("Particles.Size");
    ASSERT_GE(sizeAttribute,0);
    for (uint32_t particle = 0; particle < data.Count(); ++particle)
    {
        EXPECT_FLOAT_EQ(data.Read(kindAttribute,particle)[0],2.0f)
            << "the death event payload must reach every existing receiver particle";
        EXPECT_FLOAT_EQ(data.Read(sizeAttribute,particle)[0],0.5f)
            << "custom Event graph particle outputs must execute per target particle";
    }
}

TEST(VfxAuthoring, GpuEventReceiverQueuesCpuEventPayloadForTheRenderProxy)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    auto system = author::CreateSystem("GPU event receiver");
    auto& source = system.emitters.front();
    source.settings.capacity = 8;
    source.settings.spawnRate = 0;
    source.settings.duration = 1.0e6;
    auto& sourceUpdate = *author::FindStack(source,author::Stage::EmitterUpdate);
    sourceUpdate.modules.erase(std::remove_if(sourceUpdate.modules.begin(),sourceUpdate.modules.end(),
        [](const author::Module& module) { return module.script == "SpawnRate"; }),sourceUpdate.modules.end());
    auto sourceBurst = author::CreateModule("SpawnBurst");
    sourceBurst.inputs["Time"].value = {0,0,0,0};
    sourceBurst.inputs["Count"].value = {1,0,0,0};
    sourceUpdate.modules.push_back(sourceBurst);
    for (auto& module : author::FindStack(source,author::Stage::ParticleSpawn)->modules)
        if (module.script == "InitializeParticle") module.inputs["Lifetime"].value = {0.2f,0,0,0};
    auto& generate = source.stacks.emplace_back();
    generate.id = NLS::Guid::New().ToString();
    generate.name = "Generate";
    generate.stage = author::Stage::Event;
    generate.modules.push_back(author::CreateModule("GenerateDeathEvent"));

    auto receiver = author::CreateEmitter("GPU Receiver");
    receiver.settings.target = SimulationTarget::GPU;
    receiver.settings.capacity = 16;
    receiver.settings.spawnRate = 0;
    receiver.settings.bursts.clear();
    if (auto* update = author::FindStack(receiver,author::Stage::EmitterUpdate)) update->modules.clear();
    auto& eventStack = receiver.stacks.emplace_back();
    eventStack.id = NLS::Guid::New().ToString();
    eventStack.name = "GPU Handler";
    eventStack.stage = author::Stage::Event;
    auto properties = author::CreateModule("EventHandlerProperties");
    properties.inputs["Source Emitter Index"].value = {0,0,0,0};
    eventStack.modules.push_back(properties);
    eventStack.modules.push_back(author::CreateModule("ReceiveDeathEvent"));
    system.emitters.push_back(std::move(receiver));

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    ASSERT_EQ(compiled.asset.emitters[1].eventHandlers.size(),1u);
    EXPECT_TRUE(compiled.emitters[1].eventParticles.empty())
        << "GPU event particles are consumed by the render-side emitter";
    EXPECT_TRUE(compiled.emitters[1].spawn.parameters.Find("Emitter.EventActive") != nullptr)
        << "GPU spawn must expose the event override parameters";

    std::string error;
    auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    bool received = false;
    for (int step = 0; step < 60 && !received; ++step)
    {
        ASSERT_TRUE(instance->Tick(1.0/60.0));
        const auto frame = instance->Capture();
        if (frame && frame->gpuEvents.size() > 1 && !frame->gpuEvents[1].empty())
        {
            received = true;
            EXPECT_EQ(frame->gpuEvents[1].front().count,1u);
            EXPECT_TRUE(std::isfinite(frame->gpuEvents[1].front().position[0]));
        }
    }
    EXPECT_TRUE(received) << "CPU death events must reach a GPU receiver without a device";
}

TEST(VfxAuthoring, GpuEventReceiverRunsTheHandlerScriptAndPayloadProgram)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    auto system = author::CreateSystem("GPU event handler script");
    auto& source = system.emitters.front();
    source.settings.capacity = 8;
    source.settings.spawnRate = 0;
    source.settings.duration = 1.0e6;
    auto& sourceUpdate = *author::FindStack(source,author::Stage::EmitterUpdate);
    sourceUpdate.modules.erase(std::remove_if(sourceUpdate.modules.begin(),sourceUpdate.modules.end(),
        [](const author::Module& module) { return module.script == "SpawnRate"; }),sourceUpdate.modules.end());
    auto sourceBurst = author::CreateModule("SpawnBurst");
    sourceBurst.inputs["Time"].value = {0,0,0,0};
    sourceBurst.inputs["Count"].value = {1,0,0,0};
    sourceUpdate.modules.push_back(sourceBurst);
    for (auto& module : author::FindStack(source,author::Stage::ParticleSpawn)->modules)
        if (module.script == "InitializeParticle") module.inputs["Lifetime"].value = {0.2f,0,0,0};
    auto& generate = source.stacks.emplace_back();
    generate.id = NLS::Guid::New().ToString();
    generate.name = "Generate";
    generate.stage = author::Stage::Event;
    generate.modules.push_back(author::CreateModule("GenerateDeathEvent"));

    auto receiver = author::CreateEmitter("GPU Receiver");
    receiver.settings.target = SimulationTarget::GPU;
    receiver.settings.capacity = 16;
    receiver.settings.spawnRate = 0;
    receiver.settings.bursts.clear();
    if (auto* update = author::FindStack(receiver,author::Stage::EmitterUpdate)) update->modules.clear();
    auto& eventStack = receiver.stacks.emplace_back();
    eventStack.id = NLS::Guid::New().ToString();
    eventStack.name = "GPU Handler";
    eventStack.stage = author::Stage::Event;
    auto properties = author::CreateModule("EventHandlerProperties");
    properties.inputs["Source Emitter Index"].value = {0,0,0,0};
    eventStack.modules.push_back(properties);
    auto receive = author::CreateModule("ReceiveDeathEvent");
    receive.inputs["Spawn Count"].value = {3,0,0,0};
    receive.inputs["Random Age"].value = {0.5f,0,0,0};
    eventStack.modules.push_back(receive);
    system.emitters.push_back(std::move(receiver));

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    // The handler payload program is compiled into the GPU spawn kernel.
    EXPECT_NE(compiled.emitters[1].spawnHlsl.find("VfxEvaluateEvent"),std::string::npos)
        << "a GPU event receiver must run the handler payload program in its spawn kernel";
    for (const char* parameter : {"Emitter.EventActive","Emitter.EventPosition","Emitter.EventVelocity","Emitter.EventNormal",
        "Emitter.EventKind","Emitter.EventParticleId","Emitter.EventSourceIndex","Emitter.EventOverrideVelocity"})
        EXPECT_TRUE(compiled.emitters[1].spawn.parameters.Find(parameter) != nullptr)
            << "the GPU spawn program must declare the event payload parameter " << parameter;

    std::string error;
    auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    bool received = false;
    for (int step = 0; step < 60 && !received; ++step)
    {
        ASSERT_TRUE(instance->Tick(1.0/60.0));
        const auto frame = instance->Capture();
        if (frame && frame->gpuEvents.size() > 1 && !frame->gpuEvents[1].empty())
        {
            received = true;
            // The handler script resolved the spawn count and the random age.
            EXPECT_EQ(frame->gpuEvents[1].front().count,3u);
            EXPECT_FLOAT_EQ(frame->gpuEvents[1].front().randomAge,0.5f);
            // The Receive module forwards the event velocity, so the receiver
            // overrides the birth velocity like a CPU receiver does.
            EXPECT_TRUE(frame->gpuEvents[1].front().overrideVelocity);
            EXPECT_TRUE(std::isfinite(frame->gpuEvents[1].front().position[0]));
            EXPECT_TRUE(std::isfinite(frame->gpuEvents[1].front().velocity[1]));
        }
    }
    EXPECT_TRUE(received) << "CPU death events must reach a GPU receiver without a device";
}

TEST(VfxAuthoring, GenerateModulesSelectPublishChannelsAndBeamStateFeedsTheRibbon)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    const auto compilePublisher = [](const char* script, bool enabled)
    {
        auto system = author::CreateSystem("Event policy");
        auto& emitter = system.emitters.front();
        emitter.settings.spawnRate = 0;
        auto& stack = emitter.stacks.emplace_back();
        stack.id = NLS::Guid::New().ToString();
        stack.name = "Generate";
        stack.stage = author::Stage::Event;
        auto module = author::CreateModule(script);
        module.inputs["Event Generation Enabled"].value = {enabled ? 1.0f : 0.0f,0,0,0};
        stack.modules.push_back(module);
        CompiledSystem compiled; std::vector<author::Issue> issues;
        struct Result { bool ok = false; CompiledSystem compiled; std::vector<author::Issue> issues; };
        Result result;
        result.ok = author::Compile(system,result.compiled,result.issues);
        return result;
    };

    // An emitter that declares exactly one Generate module publishes exactly
    // that channel. Disabling the module clears it instead.
    {
        auto result = compilePublisher("GenerateDeathEvent",true);
        ASSERT_TRUE(result.ok) << (result.issues.empty() ? std::string{"none"} : result.issues.front().message);
    EXPECT_EQ(result.compiled.asset.emitters[0].eventGenerationMask,EventBit(ParticleEventSelection::Death));
    }
    {
        auto result = compilePublisher("GenerateDeathEvent",false);
        ASSERT_TRUE(result.ok) << (result.issues.empty() ? std::string{"none"} : result.issues.front().message);
        EXPECT_EQ(result.compiled.asset.emitters[0].eventGenerationMask,0u);
    }
    {
        auto result = compilePublisher("GenerateCollisionEvent",false);
        ASSERT_TRUE(result.ok) << (result.issues.empty() ? std::string{"none"} : result.issues.front().message);
        EXPECT_EQ(result.compiled.asset.emitters[0].eventGenerationMask,0u);
    }
    // The mask travels through runtime serialization so a cooked package
    // cannot silently start publishing a suppressed channel.
    {
        auto result = compilePublisher("GenerateLocationEvent",false);
        ASSERT_TRUE(result.ok) << (result.issues.empty() ? std::string{"none"} : result.issues.front().message);
        SystemAsset restored; std::string error;
        ASSERT_TRUE(DeserializeSystem(SerializeSystem(result.compiled.asset),restored,error)) << error;
        EXPECT_EQ(restored.emitters[0].eventGenerationMask,result.compiled.asset.emitters[0].eventGenerationMask);
    }

    // BeamEmitterSetup writes the endpoints, tangents, link order and width
    // consumed by the existing ribbon path, and the tangent blend keeps the
    // endpoints themselves exact.
    auto beamSystem = author::CreateSystem("Beam setup");
    auto& beamEmitter = beamSystem.emitters.front();
    beamEmitter.settings.renderer = RendererType::Ribbon;
    auto& beamSpawn = *author::FindStack(beamEmitter,author::Stage::ParticleSpawn);
    beamSpawn.modules.resize(1);
    beamSpawn.modules[0].inputs["Position"].value = {0,0,0,0};
    auto beam = author::CreateModule("BeamEmitterSetup");
    beam.inputs["Beam Start"].value = {0,0,0,0};
    beam.inputs["Beam End"].value = {10,0,0,0};
    beam.inputs["Beam Start Tangent"].value = {0,1,0,0};
    beam.inputs["Beam End Tangent"].value = {0,1,0,0};
    beam.inputs["Use Beam Tangents"].value = {1,0,0,0};
    beam.inputs["Width"].value = {0.4f,0,0,0};
    beamSpawn.modules.push_back(beam);
    CompiledSystem beamCompiled; std::vector<author::Issue> beamIssues;
    ASSERT_TRUE(author::Compile(beamSystem,beamCompiled,beamIssues)) << (beamIssues.empty() ? std::string{"none"} : beamIssues.front().message);
    const auto& beamAttributes = beamCompiled.asset.emitters[0].attributes;
    const auto findAttribute = [&](std::string_view name)
    {
        return std::find_if(beamAttributes.begin(),beamAttributes.end(),
            [&](const Attribute& attribute) { return attribute.name == name; });
    };
    for (const char* required : {"Particles.BeamStart","Particles.BeamEnd","Particles.BeamStartTangent",
        "Particles.BeamEndTangent","Particles.BeamTangentWeight","Particles.RibbonLinkOrder"})
        EXPECT_NE(findAttribute(required),beamAttributes.end()) << required;
    ParticleDataSet beamData; beamData.Initialize(beamAttributes,3);
    ASSERT_TRUE(beamData.Spawn(241)); ASSERT_TRUE(beamData.Spawn(242)); ASSERT_TRUE(beamData.Spawn(243));
    const auto beamPosition = beamData.FindAttribute("Particles.Position");
    const auto beamOrder = beamData.FindAttribute("Particles.RibbonLinkOrder");
    const auto beamWidth = beamData.FindAttribute("Particles.RibbonWidth");
    std::vector<Value> registers;
    for (uint32_t particle = 0; particle < 3; ++particle)
    {
        beamData.Write(beamOrder,particle,{particle / 2.0f,0,0,0});
        ASSERT_TRUE(RunParticleProgram(beamCompiled.emitters[0].spawn, beamData, particle, 1, {0, 1, 0, {}}, registers));
    }
    EXPECT_NEAR(beamData.Read(beamPosition,0)[0],0.0f,1e-5f);
    EXPECT_NEAR(beamData.Read(beamPosition,1)[0],5.0f,1e-5f);
    EXPECT_NEAR(beamData.Read(beamPosition,2)[0],10.0f,1e-5f);
    EXPECT_NEAR(beamData.Read(beamWidth,1)[0],0.4f,1e-5f);
    EXPECT_NEAR(beamData.Read(beamOrder,2)[0],1.0f,1e-5f);
}

TEST(VfxAuthoring, ParticleStateAndSpriteBasedLinePublishSharedColumns)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    auto stateSystem = author::CreateSystem("Particle state");
    auto& stateUpdate = *author::FindStack(stateSystem.emitters.front(),author::Stage::ParticleUpdate);
    stateUpdate.modules.clear();
    auto state = author::CreateModule("ParticleState");
    state.inputs["Loop Particles Lifetime"].value = {1,0,0,0};
    state.inputs["Kill Particles When Lifetime Has Elapsed"].value = {0,0,0,0};
    stateUpdate.modules.push_back(state);
    CompiledSystem stateCompiled; std::vector<author::Issue> stateIssues;
    ASSERT_TRUE(author::Compile(stateSystem,stateCompiled,stateIssues)) << (stateIssues.empty() ? std::string{"none"} : stateIssues.front().message);
    ParticleDataSet stateData; stateData.Initialize(stateCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(stateData.Spawn(251));
    const auto age = stateData.FindAttribute("Particles.Age");
    const auto lifetime = stateData.FindAttribute("Particles.Lifetime");
    const auto normalized = stateData.FindAttribute("Particles.NormalizedAge");
    std::vector<Value> registers;
    stateData.Write(age,0,{1.25f,0,0,0}); stateData.Write(lifetime,0,{1,0,0,0});
    ASSERT_TRUE(RunParticleProgram(stateCompiled.emitters[0].update, stateData, 0, 1, {0.01f, 1, 0, {}}, registers));
    EXPECT_NEAR(stateData.Read(age,0)[0],0.25f,1e-5f) << "looping wraps age back inside the lifetime";
    EXPECT_NEAR(stateData.Read(normalized,0)[0],1.0f,1e-5f) << "the pre-wrap normalized value saturates";

    // Without looping, the kill policy lowers the lifetime below age so the
    // existing compactor removes the particle on the next death pass.
    auto killSystem = author::CreateSystem("Particle state kill");
    auto& killUpdate = *author::FindStack(killSystem.emitters.front(),author::Stage::ParticleUpdate);
    killUpdate.modules.clear();
    auto kill = author::CreateModule("ParticleState");
    kill.inputs["Loop Particles Lifetime"].value = {0,0,0,0};
    kill.inputs["Kill Particles When Lifetime Has Elapsed"].value = {1,0,0,0};
    killUpdate.modules.push_back(kill);
    CompiledSystem killCompiled; std::vector<author::Issue> killIssues;
    ASSERT_TRUE(author::Compile(killSystem,killCompiled,killIssues)) << (killIssues.empty() ? std::string{"none"} : killIssues.front().message);
    ParticleDataSet killData; killData.Initialize(killCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(killData.Spawn(257));
    const auto killAge = killData.FindAttribute("Particles.Age");
    const auto killLifetime = killData.FindAttribute("Particles.Lifetime");
    killData.Write(killAge,0,{1.5f,0,0,0}); killData.Write(killLifetime,0,{1,0,0,0});
    ASSERT_TRUE(RunParticleProgram(killCompiled.emitters[0].update, killData, 0, 1, {0.01f, 1, 0, {}}, registers));
    EXPECT_LT(killData.Read(killLifetime,0)[0],killData.Read(killAge,0)[0]) << "the compactor must see an expired lifetime";

    // Sprite Based Line places the sprite at the segment center, puts the
    // segment length on the X size and keeps the width on Y.
    auto lineSystem = author::CreateSystem("Sprite line");
    auto& lineUpdate = *author::FindStack(lineSystem.emitters.front(),author::Stage::ParticleUpdate);
    lineUpdate.modules.clear();
    auto line = author::CreateModule("SpriteBasedLine");
    line.inputs["Point B"].value = {3,0,0,0};
    line.inputs["Sprite Width"].value = {0.5f,0,0,0};
    lineUpdate.modules.push_back(line);
    CompiledSystem lineCompiled; std::vector<author::Issue> lineIssues;
    ASSERT_TRUE(author::Compile(lineSystem,lineCompiled,lineIssues)) << (lineIssues.empty() ? std::string{"none"} : lineIssues.front().message);
    ParticleDataSet lineData; lineData.Initialize(lineCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(lineData.Spawn(263));
    const auto linePosition = lineData.FindAttribute("Particles.Position");
    const auto lineSize = lineData.FindAttribute("Particles.Size");
    lineData.Write(linePosition,0,{1,0,0,0});
    ASSERT_TRUE(RunParticleProgram(lineCompiled.emitters[0].update, lineData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(lineData.Read(linePosition,0)[0],2.0f,1e-5f) << "center of (1,0,0) and (3,0,0)";
    EXPECT_NEAR(lineData.Read(lineSize,0)[0],2.0f,1e-5f) << "the segment length is the sprite stretch size";
}

TEST(VfxAuthoring, CollisionModuleConfiguresTheEmitterAndLerpBlendsAttributes)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    // Collision is an emitter policy: it must reach the lowered asset fields
    // the existing PhysX bounce reads, and reject out-of-range settings.
    const auto compileCollision = [](float radius, float restitution, float friction, bool enabled)
    {
        auto system = author::CreateSystem("Collision policy");
        auto& emitter = system.emitters.front();
        auto& update = *author::FindStack(emitter,author::Stage::ParticleUpdate);
        update.modules.clear();
        auto collision = author::CreateModule("Collision");
        collision.inputs["Collision Enabled"].value = {enabled ? 1.0f : 0.0f,0,0,0};
        collision.inputs["Particle Radius"].value = {radius,0,0,0};
        collision.inputs["Restitution"].value = {restitution,0,0,0};
        collision.inputs["Friction"].value = {friction,0,0,0};
        collision.inputs["Collision Layers"].value = {-1,0,0,0};
        update.modules.push_back(collision);
        CompiledSystem compiled; std::vector<author::Issue> issues;
        struct Result { bool ok = false; CompiledSystem compiled; std::vector<author::Issue> issues; };
        Result result; result.ok = author::Compile(system,result.compiled,result.issues);
        return result;
    };
    {
        auto result = compileCollision(0.25f,0.25f,0.75f,true);
        ASSERT_TRUE(result.ok) << (result.issues.empty() ? std::string{"none"} : result.issues.front().message);
        const auto& emitter = result.compiled.asset.emitters[0];
        EXPECT_TRUE(emitter.collisionEnabled);
        EXPECT_NEAR(emitter.collisionRadius,0.25f,1e-6f);
        EXPECT_NEAR(emitter.restitution,0.25f,1e-6f);
        EXPECT_NEAR(emitter.friction,0.75f,1e-6f);
        // Round trip preserves the policy for cooked packages.
        SystemAsset restored; std::string error;
        ASSERT_TRUE(DeserializeSystem(SerializeSystem(result.compiled.asset),restored,error)) << error;
        EXPECT_TRUE(restored.emitters[0].collisionEnabled);
        EXPECT_NEAR(restored.emitters[0].collisionRadius,0.25f,1e-6f);
    }
    {
        auto result = compileCollision(-1.0f,0.5f,0.0f,true);
        EXPECT_FALSE(result.ok);
        ASSERT_FALSE(result.issues.empty());
        EXPECT_NE(result.issues.front().message.find("radius"),std::string::npos);
    }
    {
        auto result = compileCollision(0.05f,1.5f,0.0f,true);
        EXPECT_FALSE(result.ok);
        ASSERT_FALSE(result.issues.empty());
        EXPECT_NE(result.issues.front().message.find("restitution"),std::string::npos);
    }
    {
        auto result = compileCollision(0.05f,0.5f,0.0f,false);
        ASSERT_TRUE(result.ok) << (result.issues.empty() ? std::string{"none"} : result.issues.front().message);
        EXPECT_FALSE(result.compiled.asset.emitters[0].collisionEnabled);
    }

    // Lerp Particle Attributes blends position, velocity and color with the
    // clamped alpha and leaves the intrinsic columns as the single output.
    auto lerpSystem = author::CreateSystem("Lerp attributes");
    auto& lerpUpdate = *author::FindStack(lerpSystem.emitters.front(),author::Stage::ParticleUpdate);
    lerpUpdate.modules.clear();
    auto lerp = author::CreateModule("LerpParticleAttributes");
    lerp.inputs["Lerp Alpha"].value = {0.25f,0,0,0};
    lerp.inputs["Position A"].value = {0,0,0,0};
    lerp.inputs["Position B"].value = {8,0,0,0};
    lerp.inputs["Velocity A"].value = {0,0,0,0};
    lerp.inputs["Velocity B"].value = {4,0,0,0};
    lerp.inputs["Color A"].value = {0,0,0,1};
    lerp.inputs["Color B"].value = {1,1,1,1};
    lerpUpdate.modules.push_back(lerp);
    CompiledSystem lerpCompiled; std::vector<author::Issue> lerpIssues;
    ASSERT_TRUE(author::Compile(lerpSystem,lerpCompiled,lerpIssues)) << (lerpIssues.empty() ? std::string{"none"} : lerpIssues.front().message);
    ParticleDataSet lerpData; lerpData.Initialize(lerpCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(lerpData.Spawn(271));
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(lerpCompiled.emitters[0].update, lerpData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(lerpData.Read(lerpData.FindAttribute("Particles.Position"),0)[0],2.0f,1e-5f);
    EXPECT_NEAR(lerpData.Read(lerpData.FindAttribute("Particles.Velocity"),0)[0],1.0f,1e-5f);
    EXPECT_NEAR(lerpData.Read(lerpData.FindAttribute("Particles.Color"),0)[3],1.0f,1e-5f);
    EXPECT_NEAR(lerpData.Read(lerpData.FindAttribute("Particles.Color"),0)[0],0.25f,1e-5f);
}

TEST(VfxAuthoring, MeshRotationForceConvertsIntoRotationalVelocityThroughApplyInitialForces)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    // Rotation x lever accumulates a torque; Apply Initial Forces turns the
    // accumulated torque into angular velocity and consumes it.
    auto system = author::CreateSystem("Rotational forces");
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();
    auto rotation = author::CreateModule("MeshRotationForce");
    rotation.inputs["Rotation"].value = {0,0,2,0};
    rotation.inputs["Lever Radius"].value = {1.5f,0,0,0};
    update.modules.push_back(rotation);
    auto apply = author::CreateModule("ApplyInitialForces");
    apply.inputs["Apply Force to Velocity"].value = {0,0,0,0};
    apply.inputs["Apply Force to Position"].value = {0,0,0,0};
    apply.inputs["Apply Rotational Force to Rotational Velocity"].value = {1,0,0,0};
    apply.inputs["Positional Force Warmup Time"].value = {1,0,0,0};
    update.modules.push_back(apply);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    const auto& attributes = compiled.asset.emitters[0].attributes;
    EXPECT_TRUE(std::any_of(attributes.begin(),attributes.end(),
        [](const Attribute& attribute) { return attribute.name == "Particles.RotationalVelocity"; }));
    ParticleDataSet data; data.Initialize(attributes,1); ASSERT_TRUE(data.Spawn(281));
    const auto mass = data.FindAttribute("Particles.Mass");
    const auto radial = data.FindAttribute("Particles.RotationalVelocity");
    ASSERT_GE(mass,0); ASSERT_GE(radial,0);
    data.Write(mass,0,{3,0,0,0}); data.Write(radial,0,{});
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    // torque = rotation * lever = 2*1.5 = 3, angular velocity = 3/3 = 1.
    EXPECT_NEAR(data.Read(radial,0)[2],1.0f,1e-5f);

    // The torque module re-accumulates on every tick, so a second tick adds
    // the same angular velocity again; "consumed" only means the linear solver
    // in the same tick cannot apply it a second time.
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(radial,0)[2],2.0f,1e-5f) << "each tick converts the newly accumulated torque";

    // Positional warmup converts linear force into a pre-offset; the module
    // must not invent a velocity write when that path is disabled.
    auto linearSystem = author::CreateSystem("Linear forces");
    auto& linearUpdate = *author::FindStack(linearSystem.emitters.front(),author::Stage::ParticleUpdate);
    linearUpdate.modules.clear();
    auto acceleration = author::CreateModule("AccelerationForce");
    acceleration.inputs["Acceleration"].value = {4,0,0,0};
    linearUpdate.modules.push_back(acceleration);
    auto linearApply = author::CreateModule("ApplyInitialForces");
    linearApply.inputs["Apply Force to Velocity"].value = {0,0,0,0};
    linearApply.inputs["Apply Force to Position"].value = {1,0,0,0};
    linearApply.inputs["Apply Rotational Force to Rotational Velocity"].value = {0,0,0,0};
    linearApply.inputs["Positional Force Warmup Time"].value = {0.25f,0,0,0};
    linearUpdate.modules.push_back(linearApply);
    CompiledSystem linearCompiled; std::vector<author::Issue> linearIssues;
    ASSERT_TRUE(author::Compile(linearSystem,linearCompiled,linearIssues)) << (linearIssues.empty() ? std::string{"none"} : linearIssues.front().message);
    ParticleDataSet linearData; linearData.Initialize(linearCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(linearData.Spawn(283));
    const auto linearPosition = linearData.FindAttribute("Particles.Position");
    linearData.Write(linearPosition,0,{});
    ASSERT_TRUE(RunParticleProgram(linearCompiled.emitters[0].update, linearData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(linearData.Read(linearPosition,0)[0],4.0f*0.25f*0.5f,1e-5f)
        << "warmup uses half the positional force time";
}

TEST(VfxAuthoring, SpawnIndexDrivesExactGridCellsOnCpuAndGpu)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    // A 3x2x1 lattice with unit cell size and no jitter must place particle
    // n at (n%3, n/3, 0) exactly, in birth order.
    auto system = author::CreateSystem("Grid location");
    auto& spawn = *author::FindStack(system.emitters.front(),author::Stage::ParticleSpawn);
    spawn.modules.resize(1);
    spawn.modules[0].inputs["Lifetime"].value = {10,0,0,0};
    spawn.modules[0].inputs["Position"].value = {0,0,0,0};
    auto grid = author::CreateModule("GridLocation");
    grid.inputs["Grid Origin"].value = {0,0,0,0};
    grid.inputs["Grid X Count"].value = {3,0,0,0};
    grid.inputs["Grid Y Count"].value = {2,0,0,0};
    grid.inputs["Grid Z Count"].value = {1,0,0,0};
    grid.inputs["XYZ Dimensions"].value = {1,1,1,0};
    grid.inputs["Jitter Inside Grid Cells"].value = {0,0,0,0};
    spawn.modules.push_back(grid);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    const auto position = [&]
    {
        ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,6);
        for (uint32_t i = 0; i < 6; ++i) EXPECT_TRUE(data.Spawn(300+i));
        std::vector<Value> registers;
        for (uint32_t particle = 0; particle < 6; ++particle)
            EXPECT_TRUE(RunParticleProgram(compiled.emitters[0].spawn, data, particle, 1, {0, 1, 0, nullptr, 0, 0, particle}, registers));
        const auto attribute = data.FindAttribute("Particles.Position");
        std::vector<Value> result;
        for (uint32_t particle = 0; particle < 6; ++particle) result.push_back(data.Read(attribute,particle));
        return result;
    }();
    for (uint32_t particle = 0; particle < 6; ++particle)
    {
        EXPECT_NEAR(position[particle][0],static_cast<float>(particle % 3u),1e-5f) << "particle " << particle;
        EXPECT_NEAR(position[particle][1],static_cast<float>(particle / 3u),1e-5f) << "particle " << particle;
        EXPECT_NEAR(position[particle][2],0.0f,1e-5f);
    }

    // The same program must compile for the GPU path, where the spawn shader
    // derives the ordinal from the dispatch's birth index.
    system.emitters[0].settings.target = SimulationTarget::GPU;
    CompiledSystem gpuCompiled; std::vector<author::Issue> gpuIssues;
    ASSERT_TRUE(author::Compile(system,gpuCompiled,gpuIssues)) << (gpuIssues.empty() ? std::string{"none"} : gpuIssues.front().message);
    EXPECT_FALSE(gpuCompiled.emitters[0].spawnHlsl.empty());
    EXPECT_NE(gpuCompiled.emitters[0].spawnHlsl.find("SpawnParticles"),std::string::npos);

    // Grid cell bookkeeping reports the ordinal and the configured cell age.
    auto cellSystem = author::CreateSystem("Grid cells");
    auto& cellSpawn = *author::FindStack(cellSystem.emitters.front(),author::Stage::ParticleSpawn);
    cellSpawn.modules.resize(1);
    cellSpawn.modules[0].inputs["Lifetime"].value = {10,0,0,0};
    auto inGrid = author::CreateModule("SpawnParticlesInGrid");
    inGrid.inputs["Age"].value = {0.25f,0,0,0};
    inGrid.inputs["X Count"].value = {4,0,0,0};
    cellSpawn.modules.push_back(inGrid);
    CompiledSystem cellCompiled; std::vector<author::Issue> cellIssues;
    ASSERT_TRUE(author::Compile(cellSystem,cellCompiled,cellIssues)) << (cellIssues.empty() ? std::string{"none"} : cellIssues.front().message);
    ParticleDataSet cellData; cellData.Initialize(cellCompiled.asset.emitters[0].attributes,3);
    ASSERT_TRUE(cellData.Spawn(320)); ASSERT_TRUE(cellData.Spawn(321)); ASSERT_TRUE(cellData.Spawn(322));
    std::vector<Value> registers;
    for (uint32_t particle = 0; particle < 3; ++particle)
        ASSERT_TRUE(RunParticleProgram(cellCompiled.emitters[0].spawn, cellData, particle, 1, {0, 1, 0, {nullptr}, 0, 0, particle}, registers));
    const auto cellAttribute = cellData.FindAttribute("Particles.GridCellIndex");
    const auto ageAttribute = cellData.FindAttribute("Particles.GridCellAge");
    ASSERT_GE(cellAttribute,0); ASSERT_GE(ageAttribute,0);
    for (uint32_t particle = 0; particle < 3; ++particle)
    {
        EXPECT_NEAR(cellData.Read(cellAttribute,particle)[0],static_cast<float>(particle),1e-5f);
        EXPECT_NEAR(cellData.Read(ageAttribute,particle)[0],0.25f,1e-5f);
    }
}

TEST(VfxAuthoring, PartitionParticlesAssignsStableEvenlySpacedPartitions)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    // Four particles per partition, an unbounded partition count, and eight
    // particles: indices 0-3 land in partition 0 and 4-7 in partition 1, with
    // the normalized value running 0, 0.25, 0.5, 0.75 inside each.
    auto system = author::CreateSystem("Partitions");
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();
    auto partition = author::CreateModule("PartitionParticles");
    partition.inputs["Number Of Partitions"].value = {8,0,0,0};
    partition.inputs["Particles Per Partition"].value = {4,0,0,0};
    update.modules.push_back(partition);
    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);

    const auto& attributes = compiled.asset.emitters[0].attributes;
    // The compiler must synthesize the persisted birth ordinal in the spawn
    // graph so an update-stage partition module has a stable input.
    EXPECT_TRUE(std::any_of(attributes.begin(),attributes.end(),
        [](const Attribute& attribute) { return attribute.name == "Particles.SpawnIndex"; }));
    EXPECT_TRUE(std::any_of(compiled.emitters[0].spawn.instructions.begin(),compiled.emitters[0].spawn.instructions.end(),
        [](const Instruction& instruction) { return instruction.operation == Operation::SpawnIndex; }))
        << "the spawn graph must populate the persisted ordinal";

    ParticleDataSet data; data.Initialize(attributes,8);
    for (uint32_t i = 0; i < 8; ++i) ASSERT_TRUE(data.Spawn(400+i));
    const auto spawnIndex = data.FindAttribute("Particles.SpawnIndex");
    ASSERT_GE(spawnIndex,0);
    std::vector<Value> registers;
    // SpawnPass writes the ordinal; the update program then partitions it.
    for (uint32_t particle = 0; particle < 8; ++particle)
    {
        data.Write(spawnIndex,particle,{static_cast<float>(particle),0,0,0});
        ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, particle, 1, {0.1f, 1, 0, {}}, registers));
    }
    const auto partitionAttribute = data.FindAttribute("Particles.Partition");
    const auto normalizedAttribute = data.FindAttribute("Particles.PartitionNormalized");
    ASSERT_GE(partitionAttribute,0); ASSERT_GE(normalizedAttribute,0);
    const std::array<float,8> expectedPartitions{0,0,0,0,1,1,1,1};
    const std::array<float,8> expectedNormalized{0,0.25f,0.5f,0.75f,0,0.25f,0.5f,0.75f};
    for (uint32_t particle = 0; particle < 8; ++particle)
    {
        EXPECT_NEAR(data.Read(partitionAttribute,particle)[0],expectedPartitions[particle],1e-5f)
            << "particle " << particle;
        EXPECT_NEAR(data.Read(normalizedAttribute,particle)[0],expectedNormalized[particle],1e-5f)
            << "particle " << particle;
    }

    // The assignment is derived from the persisted ordinal, so a second tick
    // must not reshuffle partitions.
    for (uint32_t particle = 0; particle < 8; ++particle)
        ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, particle, 1, {0.1f, 1, 0, {}}, registers));
    for (uint32_t particle = 0; particle < 8; ++particle)
        EXPECT_NEAR(data.Read(partitionAttribute,particle)[0],expectedPartitions[particle],1e-5f)
            << "partitions must stay stable across ticks";
}

TEST(VfxAuthoring, EventReceiverValidationRejectsUnsupportedInputs)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    const auto compileWith = [](const char* script, float count, float randomAge)
    {
        auto system = author::CreateSystem("Event validation");
        system.emitters.front().settings.spawnRate = 0;
        system.emitters.front().settings.bursts.clear();
        auto receiver = author::CreateEmitter("Receiver");
        auto& stack = receiver.stacks.emplace_back();
        stack.id = NLS::Guid::New().ToString();
        stack.name = "Handlers";
        stack.stage = author::Stage::Event;
        auto module = author::CreateModule(script);
        module.inputs["Spawn Count"].value = {count,0,0,0};
        module.inputs["Random Age"].value = {randomAge,0,0,0};
        stack.modules.push_back(module);
        system.emitters.push_back(std::move(receiver));
        CompiledSystem compiled; std::vector<author::Issue> issues;
        return std::pair{author::Compile(system,compiled,issues),issues};
    };

    // A non-integer or out-of-range spawn count must be rejected with a diagnostic.
    {
        auto [ok,issues] = compileWith("ReceiveDeathEvent",0.5f,0.0f);
        EXPECT_FALSE(ok);
        ASSERT_FALSE(issues.empty());
        EXPECT_NE(issues.front().message.find("positive integer"),std::string::npos);
    }
    {
        auto [ok,issues] = compileWith("ReceiveDeathEvent",100001.0f,0.0f);
        EXPECT_FALSE(ok);
        ASSERT_FALSE(issues.empty());
    }
    {
        auto [ok,issues] = compileWith("ReceiveDeathEvent",1.0f,4000.0f);
        EXPECT_FALSE(ok);
        ASSERT_FALSE(issues.empty());
        EXPECT_NE(issues.front().message.find("random age"),std::string::npos);
    }
    // A valid configuration must compile and bind the matching event kind.
    {
        auto [ok,issues] = compileWith("ReceiveCollisionEvent",2.0f,0.1f);
        EXPECT_TRUE(ok) << (issues.empty() ? std::string{"none"} : issues.front().message);
    }
}
TEST(VfxAuthoring, SpawnPerFrameEmitsAnExactCountEverySubStep)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem("Spawn per frame");
    auto& emitter = system.emitters.front();
    emitter.settings.capacity = 1000;
    emitter.settings.duration = 1.0e6;
    // Replace the default schedule with Spawn Per Frame only.
    auto& emitterUpdate = *author::FindStack(emitter,author::Stage::EmitterUpdate);
    emitterUpdate.modules.clear();
    auto spawn = author::CreateModule("SpawnPerFrame");
    spawn.inputs["Count"].value = {3,0,0,0};
    emitterUpdate.modules.push_back(spawn);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    EXPECT_EQ(compiled.asset.emitters[0].spawnPerFrame,3u);

    std::string error; CpuEmitter cpu;
    ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error)) << error;

    // Two sub-steps must each add exactly three particles regardless of the
    // sub-step duration, which is what distinguishes this from a spawn rate.
    ASSERT_TRUE(cpu.TickSteps(std::array<double,2>{0.01,0.05}));
    EXPECT_EQ(cpu.Data().Count(),6u) << "each sub-step must add its exact count";
    ASSERT_TRUE(cpu.TickSteps(std::array<double,1>{0.2}));
    EXPECT_EQ(cpu.Data().Count(),9u) << "a long sub-step must not scale the count";

    // Deactivating stops frame-based spawning like the scheduled path.
    cpu.Deactivate();
    ASSERT_TRUE(cpu.TickSteps(std::array<double,1>{0.01}));
    EXPECT_EQ(cpu.Data().Count(),9u) << "a stopped emitter must not spawn";
}
TEST(VfxAuthoring, CameraRelativeModulesUseThePublishedSnapshot)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    const auto buildWith = [](const char* script, const std::function<void(author::Module&)>& configure,
        CompiledSystem& compiled, std::vector<author::Issue>& issues)
    {
        auto system = author::CreateSystem("Camera modules");
        auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
        update.modules.clear();
        auto module = author::CreateModule(script);
        configure(module);
        // Point the module input at the reserved camera parameter so the lowerer
        // declares Camera.* on the system.
        update.modules.push_back(module);
        EXPECT_TRUE(author::Compile(system,compiled,issues)) << script << ": " << (issues.empty() ? std::string{"none"} : issues.front().message);
    };

    // A camera two units in front of the particle on +Z. Camera Offset with a
    // scale of 0.5 must move the particle halfway towards the camera.
    CameraContext camera;
    camera.valid = true;
    camera.position = {0,0,2,0};
    camera.forward = {0,0,-1,0};   // Camera looks along -Z, particle is at origin.
    camera.right = {1,0,0,0};
    camera.up = {0,1,0,0};
    camera.fovDegrees = 90;
    camera.viewportWidth = 1000; camera.viewportHeight = 1000;

    CompiledSystem offset; std::vector<author::Issue> offsetIssues;
    buildWith("CameraOffset",[](author::Module& module) { module.inputs["Offset Scale"].value = {0.5f,0,0,0}; },offset,offsetIssues);
    ParticleDataSet data; data.Initialize(offset.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(83));
    const auto position = data.FindAttribute("Particles.Position");
    data.Write(position,0,{});
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(offset.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    // The program reads Camera.Position from the parameter map, so publish the
    // snapshot through the same map the emitter uses.
    std::unordered_map<std::string,Value> cameraParameters{{"Camera.Position",camera.position},
        {"Camera.Forward",camera.forward},{"Camera.Right",camera.right},{"Camera.Up",camera.up},
        {"Camera.FovDegrees",{camera.fovDegrees,0,0,0}},
        {"Camera.ViewportWidth",{camera.viewportWidth,0,0,0}},{"Camera.ViewportHeight",{camera.viewportHeight,0,0,0}}};
    data.Write(position,0,{});
    ASSERT_TRUE(RunParticleProgram(offset.emitters[0].update, data, 0, 1, ExecutionContext{0.1f, 1, 0, &cameraParameters}, registers));
    const auto moved = data.Read(position,0);
    EXPECT_NEAR(moved[0],0.0f,1e-5f);
    EXPECT_NEAR(moved[2],-1.0f,1e-5f) << "half of the 2-unit vector towards the camera on -Z";

    // View Mask: a particle on the camera axis is inside the view (mask 1).
    CompiledSystem mask; std::vector<author::Issue> maskIssues;
    buildWith("ViewMask",[](author::Module& module) { module.inputs["Edge Width"].value = {0.5f,0,0,0}; },mask,maskIssues);
    ParticleDataSet maskData; maskData.Initialize(mask.asset.emitters[0].attributes,1); ASSERT_TRUE(maskData.Spawn(89));
    const auto maskPosition = maskData.FindAttribute("Particles.Position");
    const auto maskAttr = maskData.FindAttribute("Particles.ViewMask");
    const auto insideAttr = maskData.FindAttribute("Particles.InsideView");
    ASSERT_TRUE(RunParticleProgram(mask.emitters[0].update, maskData, 0, 1, ExecutionContext{0.1f, 1, 0, &cameraParameters}, registers));
    EXPECT_NEAR(maskData.Read(maskAttr,0)[0],1.0f,1e-4f) << "an on-axis particle is fully inside the view";
    EXPECT_NEAR(maskData.Read(insideAttr,0)[0],1.0f,1e-4f);

    // A particle far to the side falls outside the 90 degree half-angle at one
    // unit of depth, so Inside View must be zero and the mask must fade out.
    maskData.Write(maskPosition,0,{100,0,0,0});
    ASSERT_TRUE(RunParticleProgram(mask.emitters[0].update, maskData, 0, 1, ExecutionContext{0.1f, 1, 0, &cameraParameters}, registers));
    EXPECT_NEAR(maskData.Read(insideAttr,0)[0],0.0f,1e-4f) << "the particle is outside the viewport";
    EXPECT_NEAR(maskData.Read(maskAttr,0)[0],0.0f,1e-4f) << "the mask must fade to zero at the edge";

    // Maintain in Camera Particle Scale: at the reference distance with the
    // reference FOV the scale is exactly one.
    CompiledSystem scale; std::vector<author::Issue> scaleIssues;
    buildWith("MaintainInCameraParticleScale",[](author::Module& module)
    {
        module.inputs["Reference Distance"].value = {2,0,0,0};
        module.inputs["Reference Fov"].value = {90,0,0,0};
    },scale,scaleIssues);
    ParticleDataSet scaleData; scaleData.Initialize(scale.asset.emitters[0].attributes,1); ASSERT_TRUE(scaleData.Spawn(97));
    const auto sizeAttr = scaleData.FindAttribute("Particles.Size");
    scaleData.Write(scaleData.FindAttribute("Particles.Position"),0,{});
    scaleData.Write(sizeAttr,0,{1,0,0,0});
    ASSERT_TRUE(RunParticleProgram(scale.emitters[0].update, scaleData, 0, 1, ExecutionContext{0.1f, 1, 0, &cameraParameters}, registers));
    EXPECT_NEAR(scaleData.Read(sizeAttr,0)[0],1.0f,1e-4f)
        << "at the reference distance and FOV the apparent size is unchanged";

    // Halving the distance with a fixed FOV must double the scale.
    camera.position = {0,0,1,0};
    cameraParameters["Camera.Position"] = camera.position;
    scaleData.Write(scaleData.FindAttribute("Particles.Position"),0,{});
    scaleData.Write(sizeAttr,0,{1,0,0,0});
    ASSERT_TRUE(RunParticleProgram(scale.emitters[0].update, scaleData, 0, 1, ExecutionContext{0.1f, 1, 0, &cameraParameters}, registers));
    EXPECT_NEAR(scaleData.Read(sizeAttr,0)[0],0.5f,1e-4f)
        << "a closer camera must shrink the scale to preserve apparent size";
}
TEST(VfxAuthoring, CameraSnapshotTravelsWithTheWorldFrameForGpuEmitters)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    // A GPU emitter that reads the camera, so the system declares the reserved
    // parameter set and the world frame must carry the published snapshot.
    auto system = author::CreateSystem("Camera gpu system");
    system.emitters.front().settings.target = SimulationTarget::GPU;
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    auto offset = author::CreateModule("CameraOffset");
    offset.inputs["Offset Scale"].value = {1,0,0,0};
    update.modules.push_back(offset);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    // The camera parameters must be declared exactly when a camera module runs.
    const auto declares = [&](std::string_view name)
    { return std::any_of(compiled.asset.parameters.begin(),compiled.asset.parameters.end(),
        [&](const ParameterDefinition& p) { return p.name == name; }); };
    EXPECT_TRUE(declares("Camera.Position"));
    EXPECT_TRUE(declares("Camera.Forward"));
    EXPECT_TRUE(declares("Camera.FovDegrees"));
    EXPECT_TRUE(declares("Camera.ViewportWidth"));

    std::string error; auto instance = std::make_shared<SystemInstance>();
    ASSERT_TRUE(instance->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;

    CameraContext camera;
    camera.valid = true;
    camera.position = {4,5,6,0};
    camera.forward = {0,0,-1,0};
    camera.right = {1,0,0,0};
    camera.up = {0,1,0,0};
    camera.fovDegrees = 75;
    camera.viewportWidth = 1280; camera.viewportHeight = 720;
    instance->SetCameraContext(camera);
    ASSERT_TRUE(instance->Tick(0.01));

    // The published world frame carries the same values the render pass uses, so
    // a GPU emitter binding these names reads an identical camera.
    const auto frame = instance->Capture();
    ASSERT_TRUE(frame);
    const auto value = [&](std::string_view name)
    {
        const auto found = frame->parameters.find(std::string{name});
        return found == frame->parameters.end() ? Value{} : found->second;
    };
    EXPECT_NEAR(value("Camera.Position")[0],4.0f,1e-5f);
    EXPECT_NEAR(value("Camera.Position")[2],6.0f,1e-5f);
    EXPECT_NEAR(value("Camera.FovDegrees")[0],75.0f,1e-5f);
    EXPECT_NEAR(value("Camera.ViewportHeight")[0],720.0f,1e-5f);

    // Without a published camera the frame must not invent one: a system that
    // never receives a snapshot keeps the declared neutral defaults.
    auto neutral = std::make_shared<SystemInstance>();
    ASSERT_TRUE(neutral->Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    ASSERT_TRUE(neutral->Tick(0.01));
    const auto neutralFrame = neutral->Capture();
    ASSERT_TRUE(neutralFrame);
    const auto neutralPosition = neutralFrame->parameters.find("Camera.Position");
    ASSERT_NE(neutralPosition,neutralFrame->parameters.end());
    EXPECT_NEAR(neutralPosition->second[0],0.0f,1e-5f) << "no snapshot means the declared default";
}
TEST(VfxAuthoring, ViewRecyclerMovesOffscreenParticlesBackIntoView)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem("View recycler");
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();
    auto recycler = author::CreateModule("ViewRecycler");
    recycler.inputs["Recycle Margin"].value = {0.5f,0,0,0};
    update.modules.push_back(recycler);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);

    CameraContext camera;
    camera.valid = true;
    camera.position = {0,0,0,0};
    camera.forward = {0,0,1,0};
    camera.right = {1,0,0,0}; camera.up = {0,1,0,0};
    const std::unordered_map<std::string,Value> published{{"Camera.Position",camera.position},
        {"Camera.Forward",{0,0,1,0}},{"Camera.Right",{1,0,0,0}},{"Camera.Up",{0,1,0,0}},
        {"Camera.FovDegrees",{60,0,0,0}},{"Camera.ViewportWidth",{1920,0,0,0}},{"Camera.ViewportHeight",{1080,0,0,0}}};
    const ExecutionContext context{0.1f,1,0,&published};

    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(101));
    const auto position = data.FindAttribute("Particles.Position");
    std::vector<Value> registers;

    // A particle in front of the camera must be left alone.
    data.Write(position,0,{0,0,10,0});
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, context, registers));
    EXPECT_NEAR(data.Read(position,0)[2],10.0f,1e-4f) << "an in-view particle must not move";

    // A particle behind the camera must be recycled to in front of it.
    data.Write(position,0,{0,0,-5,0});
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, context, registers));
    EXPECT_GT(data.Read(position,0)[2],0.0f) << "a recycled particle must end up in front of the camera";
    // The mirror is about the margin plane: depth -5 reflects to +6 across 0.5.
    EXPECT_NEAR(data.Read(position,0)[2],6.0f,1e-4f) << "it reflects across the recycle margin plane";
}
TEST(VfxAuthoring, EighthBatchTriangleDistanceMassInertiaRotationAndMomentum)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    const auto buildOne = [](const char* script, const std::function<void(author::Module&)>& configure,
        CompiledSystem& compiled, std::vector<author::Issue>& issues)
    {
        auto system = author::CreateSystem("Eighth batch");
        auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
        update.modules.clear();
        auto module = author::CreateModule(script);
        configure(module);
        update.modules.push_back(module);
        EXPECT_TRUE(author::Compile(system,compiled,issues)) << script << ": " << (issues.empty() ? std::string{"none"} : issues.front().message);
    };

    // Triangle on the XY plane at z=0; a particle at z=3 is 3 units away.
    CompiledSystem triangle; std::vector<author::Issue> triangleIssues;
    buildOne("FindClosestPointOnTriangle",[](author::Module& module)
    {
        module.inputs["Vertex A"].value = {0,0,0,0};
        module.inputs["Vertex B"].value = {1,0,0,0};
        module.inputs["Vertex C"].value = {0,1,0,0};
    },triangle,triangleIssues);
    ParticleDataSet data; data.Initialize(triangle.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(103));
    const auto position = data.FindAttribute("Particles.Position");
    const auto distanceAttr = data.FindAttribute("Particles.TriangleDistance");
    std::vector<Value> registers;
    data.Write(position,0,{0.25f,0.25f,3,0});
    ASSERT_TRUE(RunParticleProgram(triangle.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(distanceAttr,0)[0],3.0f,1e-4f) << "the plane distance is the triangle distance";
    data.Write(position,0,{0.25f,0.25f,-1.5f,0});
    ASSERT_TRUE(RunParticleProgram(triangle.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(distanceAttr,0)[0],1.5f,1e-4f) << "the distance is unsigned";

    // Mass 1 with density 1000 gives size cbrt(0.001) = 0.1.
    CompiledSystem mass; std::vector<author::Issue> massIssues;
    buildOne("CalculateSizeAndRotationalInertiaByMass",[](author::Module& module)
    {
        module.inputs["Mass"].value = {1,0,0,0};
        module.inputs["Density"].value = {1000,0,0,0};
    },mass,massIssues);
    ParticleDataSet massData; massData.Initialize(mass.asset.emitters[0].attributes,1); ASSERT_TRUE(massData.Spawn(107));
    const auto sizeAttr = massData.FindAttribute("Particles.Size");
    ASSERT_TRUE(RunParticleProgram(mass.emitters[0].update, massData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(massData.Read(sizeAttr,0)[0],0.1f,1e-4f) << "size is the cube root of mass over density";
    // Inertia = m*s^2/6 = 1*0.01/6.
    EXPECT_NEAR(massData.Read(massData.FindAttribute("Particles.RotationalInertia"),0)[0],0.01f/6.0f,1e-6f);

    // Rotation adds in radians: 90 deg/s over the step is not integrated here,
    // the module accumulates the converted angular velocity directly.
    CompiledSystem rotation; std::vector<author::Issue> rotationIssues;
    buildOne("AddRotationalVelocity",[](author::Module& module)
    { module.inputs["Rotational Velocity"].value = {0,90,0,0}; },rotation,rotationIssues);
    ParticleDataSet rotationData; rotationData.Initialize(rotation.asset.emitters[0].attributes,1); ASSERT_TRUE(rotationData.Spawn(109));
    ASSERT_TRUE(RunParticleProgram(rotation.emitters[0].update, rotationData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(rotationData.Read(rotationData.FindAttribute("Particles.RotationalVelocity"),0)[1],1.57079633f,1e-5f)
        << "90 degrees per second becomes pi/2 radians per second";

    // Momentum preservation: velocity scales by the inverse mass ratio.
    CompiledSystem momentum; std::vector<author::Issue> momentumIssues;
    buildOne("UpdateVelocityOnMassChange",[](author::Module& module)
    { module.inputs["Previous Mass"].value = {1,0,0,0}; },momentum,momentumIssues);
    ParticleDataSet momentumData; momentumData.Initialize(momentum.asset.emitters[0].attributes,1); ASSERT_TRUE(momentumData.Spawn(113));
    const auto velocity = momentumData.FindAttribute("Particles.Velocity");
    const auto massNow = momentumData.FindAttribute("Particles.Mass");
    momentumData.Write(velocity,0,{4,0,0,0}); momentumData.Write(massNow,0,{2,0,0,0});
    ASSERT_TRUE(RunParticleProgram(momentum.emitters[0].update, momentumData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(momentumData.Read(velocity,0)[0],2.0f,1e-4f)
        << "doubling the mass must halve the velocity to preserve momentum";
}
TEST(VfxAuthoring, NinthBatchEngineContextFirstFrameCounterTimelineAndStateMachine)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    const auto buildOne = [](const char* script, const std::function<void(author::Module&)>& configure,
        CompiledSystem& compiled, std::vector<author::Issue>& issues)
    {
        auto system = author::CreateSystem("Ninth batch");
        auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
        update.modules.clear();
        auto module = author::CreateModule(script);
        configure(module);
        update.modules.push_back(module);
        EXPECT_TRUE(author::Compile(system,compiled,issues)) << script << ": " << (issues.empty() ? std::string{"none"} : issues.front().message);
    };

    // First Frame is one on the first simulated tick and zero once age advances.
    CompiledSystem firstFrame; std::vector<author::Issue> firstIssues;
    buildOne("FirstFrame",[](author::Module&){},firstFrame,firstIssues);
    // The reserved engine parameters must be declared for the module to compile.
    const auto declares = [&](const CompiledSystem& compiled, std::string_view name)
    { return std::any_of(compiled.asset.parameters.begin(),compiled.asset.parameters.end(),
        [&](const ParameterDefinition& p) { return p.name == name; }); };
    EXPECT_TRUE(declares(firstFrame,"Engine.FirstFrame"));
    EXPECT_TRUE(declares(firstFrame,"Engine.SystemAge"));

    ParticleDataSet data; data.Initialize(firstFrame.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(127));
    const auto firstAttr = data.FindAttribute("Particles.FirstFrame");
    std::vector<Value> registers;
    const std::unordered_map<std::string,Value> ageZero{{"Engine.FirstFrame",{1,0,0,0}},{"Engine.SystemAge",{}},{"Engine.FrameIndex",{}}};
    ASSERT_TRUE(RunParticleProgram(firstFrame.emitters[0].update, data, 0, 1, ExecutionContext{0.1f, 1, 0, &ageZero}, registers));
    EXPECT_NEAR(data.Read(firstAttr,0)[0],1.0f,1e-5f) << "the first tick reports First Frame";
    const std::unordered_map<std::string,Value> ageLater{{"Engine.FirstFrame",{}},{"Engine.SystemAge",{0.5f,0,0,0}},{"Engine.FrameIndex",{3,0,0,0}}};
    ASSERT_TRUE(RunParticleProgram(firstFrame.emitters[0].update, data, 0, 1, ExecutionContext{0.1f, 1, 0, &ageLater}, registers));
    EXPECT_NEAR(data.Read(firstAttr,0)[0],0.0f,1e-5f) << "later ticks clear First Frame";

    // The engine-frame counter increments once per execution while enabled.
    CompiledSystem counter; std::vector<author::Issue> counterIssues;
    buildOne("EmitterFrameCounter",[](author::Module& module)
    { module.inputs["Increment Counter"].value = {1,0,0,0}; },counter,counterIssues);
    ParticleDataSet counterData; counterData.Initialize(counter.asset.emitters[0].attributes,1); ASSERT_TRUE(counterData.Spawn(131));
    const auto counterAttr = counterData.FindAttribute("Particles.EmitterFrameCounter");
    for (int step = 1; step <= 3; ++step)
    {
        ASSERT_TRUE(RunParticleProgram(counter.emitters[0].update, counterData, 0, 1, {0.1f, 1, 0, {}}, registers));
        EXPECT_NEAR(counterData.Read(counterAttr,0)[0],static_cast<float>(step),1e-5f);
    }

    // Timeline: at age 0.5 with a 1-second length the normalised value is 0.5.
    CompiledSystem timeline; std::vector<author::Issue> timelineIssues;
    buildOne("Timeline",[](author::Module& module)
    {
        module.inputs["Timeline Length"].value = {1,0,0,0};
        module.inputs["Play Rate"].value = {1,0,0,0};
    },timeline,timelineIssues);
    ParticleDataSet timelineData; timelineData.Initialize(timeline.asset.emitters[0].attributes,1); ASSERT_TRUE(timelineData.Spawn(137));
    ASSERT_TRUE(RunParticleProgram(timeline.emitters[0].update, timelineData, 0, 1, ExecutionContext{0.1f, 1, 0, &ageLater}, registers));
    EXPECT_NEAR(timelineData.Read(timelineData.FindAttribute("Particles.TimelineNormalized"),0)[0],0.5f,1e-4f)
        << "half a second into a one second timeline is 0.5";
    EXPECT_NEAR(timelineData.Read(timelineData.FindAttribute("Particles.TimelinePlaying"),0)[0],1.0f,1e-5f)
        << "a positive play rate means the timeline is playing";
    // A negative play rate must report rewinding.
    auto rewindSystem = author::CreateSystem("Rewind");
    auto& rewindUpdate = *author::FindStack(rewindSystem.emitters.front(),author::Stage::ParticleUpdate);
    rewindUpdate.modules.clear();
    auto rewindModule = author::CreateModule("Timeline");
    rewindModule.inputs["Timeline Length"].value = {2,0,0,0};
    rewindModule.inputs["Play Rate"].value = {-1,0,0,0};
    rewindUpdate.modules.push_back(rewindModule);
    CompiledSystem rewind; std::vector<author::Issue> rewindIssues;
    ASSERT_TRUE(author::Compile(rewindSystem,rewind,rewindIssues)) << (rewindIssues.empty() ? std::string{"none"} : rewindIssues.front().message);
    ParticleDataSet rewindData; rewindData.Initialize(rewind.asset.emitters[0].attributes,1); ASSERT_TRUE(rewindData.Spawn(139));
    ASSERT_TRUE(RunParticleProgram(rewind.emitters[0].update, rewindData, 0, 1, ExecutionContext{0.1f, 1, 0, &ageLater}, registers));
    EXPECT_NEAR(rewindData.Read(rewindData.FindAttribute("Particles.TimelinePlaying"),0)[0],0.0f,1e-5f)
        << "a negative play rate reports not playing";
    // The raw position is negative when rewinding from a positive age.
    EXPECT_NEAR(rewindData.Read(rewindData.FindAttribute("Particles.TimelinePosition"),0)[0],-0.5f,1e-4f);

    // State machine: within the on window the output is zero, then one.
    CompiledSystem machine; std::vector<author::Issue> machineIssues;
    buildOne("TimeBasedStateMachine",[](author::Module& module)
    {
        module.inputs["On Duration"].value = {1,0,0,0};
        module.inputs["Off Duration"].value = {1,0,0,0};
        module.inputs["Rate"].value = {1,0,0,0};
    },machine,machineIssues);
    ParticleDataSet machineData; machineData.Initialize(machine.asset.emitters[0].attributes,1); ASSERT_TRUE(machineData.Spawn(149));
    const auto stateAttr = machineData.FindAttribute("Particles.OnOffPercentage");
    for (const auto& probe : {std::pair{0.25f,1.0f}, std::pair{1.5f,0.0f}, std::pair{2.5f,1.0f}})
    {
        const std::unordered_map<std::string,Value> probeAge{{"Engine.SystemAge",{probe.first,0,0,0}}};
        ASSERT_TRUE(RunParticleProgram(machine.emitters[0].update, machineData, 0, 1, ExecutionContext{0.1f, 1, 0, &probeAge}, registers));
        EXPECT_NEAR(machineData.Read(stateAttr,0)[0],probe.second,1e-4f) << "system age " << probe.first;
    }
}
TEST(VfxAuthoring, TenthBatchSystemTransformLocationVelocityAndState)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    // A spawn stack with the system location offset and inherited velocity.
    auto system = author::CreateSystem("System transform");
    auto& spawn = *author::FindStack(system.emitters.front(),author::Stage::ParticleSpawn);
    spawn.modules.resize(1);
    spawn.modules[0].inputs["Position"].value = {0,0,0,0};
    spawn.modules[0].inputs["Velocity"].value = {0,0,0,0};
    auto location = author::CreateModule("SystemLocation");
    location.inputs["Offset"].value = {1,0,0,0};
    spawn.modules.push_back(location);
    auto inherit = author::CreateModule("InheritSystemVelocity");
    inherit.inputs["Inherit Scale"].value = {0.5f,0,0,0};
    spawn.modules.push_back(inherit);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    // The system transform must be declared exactly when a module needs it.
    const auto declares = [&](std::string_view name)
    { return std::any_of(compiled.asset.parameters.begin(),compiled.asset.parameters.end(),
        [&](const ParameterDefinition& p) { return p.name == name; }); };
    EXPECT_TRUE(declares("System.Position"));
    EXPECT_TRUE(declares("System.Velocity"));

    const std::unordered_map<std::string,Value> published{{"System.Position",{10,20,30,0}},{"System.Velocity",{4,0,0,0}}};
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(151));
    const auto position = data.FindAttribute("Particles.Position");
    const auto velocity = data.FindAttribute("Particles.Velocity");
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].spawn, data, 0, 1, ExecutionContext{0, 1, 0, &published}, registers));
    const auto moved = data.Read(position,0);
    EXPECT_NEAR(moved[0],11.0f,1e-4f) << "system position 10 plus the module offset 1";
    EXPECT_NEAR(moved[1],20.0f,1e-4f);
    EXPECT_NEAR(moved[2],30.0f,1e-4f);
    EXPECT_NEAR(data.Read(velocity,0)[0],2.0f,1e-4f) << "half of the system velocity 4";

    // System State reports the age and its normalised position over the duration.
    auto stateSystem = author::CreateSystem("System state");
    auto& update = *author::FindStack(stateSystem.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();
    auto state = author::CreateModule("SystemState");
    state.inputs["System Duration"].value = {4,0,0,0};
    update.modules.push_back(state);

    CompiledSystem stateCompiled; std::vector<author::Issue> stateIssues;
    ASSERT_TRUE(author::Compile(stateSystem,stateCompiled,stateIssues)) << (stateIssues.empty() ? std::string{"none"} : stateIssues.front().message);
    const std::unordered_map<std::string,Value> agePublished{{"Engine.SystemAge",{1,0,0,0}}};
    ParticleDataSet stateData; stateData.Initialize(stateCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(stateData.Spawn(157));
    ASSERT_TRUE(RunParticleProgram(stateCompiled.emitters[0].update, stateData, 0, 1, ExecutionContext{0.1f, 1, 0, &agePublished}, registers));
    EXPECT_NEAR(stateData.Read(stateData.FindAttribute("Particles.SystemAge"),0)[0],1.0f,1e-4f);
    EXPECT_NEAR(stateData.Read(stateData.FindAttribute("Particles.SystemNormalizedAge"),0)[0],0.25f,1e-4f)
        << "one second of a four second system duration";

    // Beyond the duration the normalised value saturates rather than exceeding one.
    const std::unordered_map<std::string,Value> latePublished{{"Engine.SystemAge",{9,0,0,0}}};
    ASSERT_TRUE(RunParticleProgram(stateCompiled.emitters[0].update, stateData, 0, 1, ExecutionContext{0.1f, 1, 0, &latePublished}, registers));
    EXPECT_NEAR(stateData.Read(stateData.FindAttribute("Particles.SystemNormalizedAge"),0)[0],1.0f,1e-4f);
}
TEST(VfxAuthoring, CompleteIfUnusedCompletesTheSystemAfterTheGracePeriod)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    // A one-shot burst that dies quickly, with completion armed at 0.2 seconds.
    auto system = author::CreateSystem("Complete if unused");
    auto& emitter = system.emitters.front();
    emitter.settings.capacity = 32;
    emitter.settings.spawnRate = 0;
    emitter.settings.duration = 1.0e6;
    // Bursts come from the Spawn Burst module: the authoring settings' burst
    // list is rebuilt from the Emitter Update stack during lowering.
    for (auto& module : author::FindStack(emitter,author::Stage::ParticleSpawn)->modules)
        if (module.script == "InitializeParticle") module.inputs["Lifetime"].value = {0.5f,0,0,0};
    // Keep Emitter State (it owns the duration the burst fires within) but clear
    // the continuous Spawn Rate module so only the burst produces particles.
    auto& emitterUpdate = *author::FindStack(emitter,author::Stage::EmitterUpdate);
    emitterUpdate.modules.erase(std::remove_if(emitterUpdate.modules.begin(),emitterUpdate.modules.end(),
        [](const author::Module& module) { return module.script == "SpawnRate"; }),emitterUpdate.modules.end());
    auto burst = author::CreateModule("SpawnBurst");
    burst.inputs["Time"].value = {0,0,0,0};
    burst.inputs["Count"].value = {3,0,0,0};
    emitterUpdate.modules.push_back(burst);
    auto complete = author::CreateModule("CompleteIfUnused");
    complete.inputs["Unused Grace Period"].value = {0.2f,0,0,0};
    emitterUpdate.modules.push_back(complete);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    EXPECT_TRUE(compiled.asset.completion.enabled);
    EXPECT_NEAR(compiled.asset.completion.unusedGracePeriod,0.2,1e-6);

    std::string error; SystemInstance instance;
    ASSERT_TRUE(instance.Initialize(std::make_shared<CompiledSystem>(compiled),error)) << error;
    EXPECT_FALSE(instance.IsComplete());

    // While particles are alive the system must not report completion.
    ASSERT_TRUE(instance.Tick(0.01));
    EXPECT_GT(instance.CpuEmitters()[0]->Data().Count(),0u) << "the burst must have spawned";
    EXPECT_FALSE(instance.IsComplete()) << "a populated system is not complete";

    // Once the burst dies and the grace period elapses, completion latches.
    ASSERT_TRUE(instance.Tick(0.6));
    EXPECT_EQ(instance.CpuEmitters()[0]->Data().Count(),0u) << "the burst must have expired";
    ASSERT_TRUE(instance.Tick(0.3));
    EXPECT_TRUE(instance.IsComplete()) << "the unused grace period must complete the system";

    // Reset clears the completion latch.
    instance.Reset();
    EXPECT_FALSE(instance.IsComplete());
}
TEST(VfxAuthoring, SpawnPerUnitEmitsInProportionToTravelledDistance)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    // A system that travels 10 units per second and emits 2 particles per unit.
    auto system = author::CreateSystem("Spawn per unit");
    auto& emitter = system.emitters.front();
    emitter.settings.capacity = 1000;
    emitter.settings.duration = 1.0e6;
    auto& emitterUpdate = *author::FindStack(emitter,author::Stage::EmitterUpdate);
    emitterUpdate.modules.erase(std::remove_if(emitterUpdate.modules.begin(),emitterUpdate.modules.end(),
        [](const author::Module& module) { return module.script == "SpawnRate"; }),emitterUpdate.modules.end());
    auto perUnit = author::CreateModule("SpawnPerUnit");
    perUnit.inputs["Spawn Per Unit"].value = {2,0,0,0};
    emitterUpdate.modules.push_back(perUnit);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    EXPECT_NEAR(compiled.asset.emitters[0].spawnPerUnit,2.0,1e-6);
    EXPECT_TRUE(std::any_of(compiled.asset.parameters.begin(),compiled.asset.parameters.end(),
        [](const ParameterDefinition& p) { return p.name == "System.Velocity"; }));

    std::string error; CpuEmitter cpu;
    ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error)) << error;
    ASSERT_TRUE(cpu.SetParameter("System.Velocity",{10,0,0,0}));

    // A 0.1 second step at 10 units per second covers 1 unit, so 2 particles.
    ASSERT_TRUE(cpu.TickSteps(std::array<double,1>{0.1}));
    EXPECT_EQ(cpu.Data().Count(),2u) << "one unit travelled at 2 per unit";

    // A smaller step accumulates a fractional particle instead of dropping it:
    // 0.025 seconds covers 0.25 units, which is 0.5 of a particle.
    ASSERT_TRUE(cpu.TickSteps(std::array<double,1>{0.025}));
    ASSERT_TRUE(cpu.TickSteps(std::array<double,1>{0.025}));
    EXPECT_EQ(cpu.Data().Count(),3u) << "two quarter-unit steps sum to one particle";

    // No system velocity means no distance, so no further spawning.
    ASSERT_TRUE(cpu.SetParameter("System.Velocity",{}));
    ASSERT_TRUE(cpu.TickSteps(std::array<double,1>{0.5}));
    EXPECT_EQ(cpu.Data().Count(),3u) << "a stationary system must not spawn";
}
TEST(VfxAuthoring, RibbonRendererCompilesAndValidatesItsSettings)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;
    auto system = author::CreateSystem("Ribbon");
    auto& emitter = system.emitters.front();
    emitter.settings.renderer = RendererType::Ribbon;
    emitter.settings.ribbon.blend = SpriteBlendMode::Additive;
    emitter.settings.ribbon.tint = {1,0.5f,0.25f,1};

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    EXPECT_EQ(static_cast<uint32_t>(compiled.asset.emitters[0].renderer),static_cast<uint32_t>(RendererType::Ribbon));
    // The ribbon shader builds from the ribbon settings and exposes its own
    // vertex entry point, so a package validated as Sprite or Mesh cannot
    // masquerade as a ribbon.
    std::string ribbonSource, shaderError;
    ASSERT_TRUE(BuildRibbonShader(compiled.asset.emitters[0].attributes,ribbonSource,shaderError,
        compiled.asset.emitters[0].ribbon,compiled.asset.emitters[0].sprite)) << shaderError;
    EXPECT_NE(ribbonSource.find("RibbonVS"),std::string::npos);
    EXPECT_NE(ribbonSource.find("RibbonPS"),std::string::npos);
    // The width column is resolved to an attribute index, so the generated
    // source indexes VfxAttributes rather than naming the attribute.
    EXPECT_NE(ribbonSource.find("VfxAttributes["),std::string::npos);

    // A width module must make the column exist and drive the strip width.
    auto widthSystem = system;
    auto& widthUpdate = *author::FindStack(widthSystem.emitters.front(),author::Stage::ParticleUpdate);
    auto width = author::CreateModule("RibbonWidth");
    width.inputs["Width"].value = {0.25f,0,0,0};
    widthUpdate.modules.push_back(width);
    CompiledSystem widthCompiled; std::vector<author::Issue> widthIssues;
    ASSERT_TRUE(author::Compile(widthSystem,widthCompiled,widthIssues)) << (widthIssues.empty() ? std::string{"none"} : widthIssues.front().message);
    EXPECT_GE(widthCompiled.asset.emitters[0].attributes.end()-std::find_if(
        widthCompiled.asset.emitters[0].attributes.begin(),widthCompiled.asset.emitters[0].attributes.end(),
        [](const Attribute& attribute) { return attribute.name == "Particles.RibbonWidth"; }),1);
    std::string widthSource, widthError;
    ASSERT_TRUE(BuildRibbonShader(widthCompiled.asset.emitters[0].attributes,widthSource,widthError,
        widthCompiled.asset.emitters[0].ribbon,widthCompiled.asset.emitters[0].sprite)) << widthError;
    EXPECT_NE(widthSource.find("RibbonVS"),std::string::npos);

    // Round trip keeps the ribbon renderer and its settings.
    author::System restored; std::string error;
    ASSERT_TRUE(author::Deserialize(author::Serialize(system),restored,error)) << error;
    EXPECT_EQ(static_cast<uint32_t>(restored.emitters[0].settings.renderer),static_cast<uint32_t>(RendererType::Ribbon));
    EXPECT_NEAR(restored.emitters[0].settings.ribbon.tint[1],0.5f,1e-6f);
    EXPECT_EQ(static_cast<uint32_t>(restored.emitters[0].settings.ribbon.blend),static_cast<uint32_t>(SpriteBlendMode::Additive));

    // The shared draw settings resolve the ribbon material and blend mode.
    const auto draw = ResolveRibbonDrawSettings(system.emitters[0].settings.ribbon);
    EXPECT_EQ(static_cast<uint32_t>(draw.blend),static_cast<uint32_t>(SpriteBlendMode::Additive));
    EXPECT_NEAR(draw.tint[2],0.25f,1e-6f);

    // Invalid settings are rejected instead of silently compiling.
    RibbonSettings invalid = system.emitters[0].settings.ribbon;
    invalid.blend = static_cast<SpriteBlendMode>(99);
    std::string validationError;
    EXPECT_FALSE(ValidateRibbonSettings(invalid,validationError));
    EXPECT_FALSE(validationError.empty());
    invalid = system.emitters[0].settings.ribbon;
    invalid.uvTilingDistance = 0;
    EXPECT_FALSE(ValidateRibbonSettings(invalid,validationError));
}
TEST(VfxAuthoring, ModuleLibraryExposesDeclaredStagesInputsAndRendererAttributes)
{
    const auto modules = author::ModuleLibrary();
    EXPECT_GT(modules.size(),24u) << "The candidate library must grow beyond the original twelve modules";
    for (const auto& definition : modules)
    {
        SCOPED_TRACE(definition.id);
        EXPECT_FALSE(definition.name.empty());
        EXPECT_FALSE(definition.category.empty());
        EXPECT_FALSE(definition.description.empty());
        for (const auto& input : definition.inputs) EXPECT_FALSE(input.name.empty());
        EXPECT_TRUE(author::SupportsStage(definition,definition.stage));
    }
    // Renderer-consumed attributes must be declared by the modules that write
    // them, otherwise the sprite shader can never see a rotation or sub image.
    const auto declares = [&](std::string_view module,std::string_view attribute)
    {
        const auto* definition = author::FindModule(module);
        if (definition == nullptr) return false;
        return std::any_of(definition->attributes.begin(),definition->attributes.end(),
            [&](const auto& entry) { return entry.name == attribute; });
    };
    EXPECT_TRUE(declares("SpriteRotationRate","Particles.Rotation"));
    EXPECT_TRUE(declares("SubUVAnimation","Particles.SubImageIndex"));
}

TEST(VfxAuthoring, NewShapeModulesRegisterAttributesAndExecuteOnCpuAndGpu)
{
    auto system = author::CreateSystem();
    auto& emitter = system.emitters.front();
    auto& spawn = *author::FindStack(emitter,author::Stage::ParticleSpawn);
    spawn.modules.resize(1);
    ModuleAt(system,author::Stage::ParticleSpawn,0).inputs["Position"].value = {0,0,0,0};
    ModuleAt(system,author::Stage::ParticleSpawn,0).inputs["Velocity"].value = {0,0,0,0};

    auto sphere = author::CreateModule("SphereLocation");
    sphere.inputs["Radius"].value = {2,0,0,0};
    sphere.inputs["Surface Only"].value = {1,0,0,0};
    spawn.modules.push_back(sphere);
    auto cone = author::CreateModule("AddVelocityInCone");
    cone.inputs["Speed"].value = {5,0,0,0}; cone.inputs["Angle"].value = {0,0,0,0};
    spawn.modules.push_back(cone);

    auto& update = *author::FindStack(emitter,author::Stage::ParticleUpdate);
    auto rotation = author::CreateModule("SpriteRotationRate");
    rotation.inputs["Rotation Rate"].value = {180,0,0,0};
    update.modules.push_back(rotation);
    auto frame = author::CreateModule("SubUVAnimation");
    frame.inputs["First Frame"].value = {0,0,0,0}; frame.inputs["Frame Count"].value = {4,0,0,0};
    frame.inputs["Random Start"].value = {0,0,0,0};
    update.modules.push_back(frame);

    CompiledSystem compiled; std::vector<author::Issue> issues; std::string error;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? "" : issues.front().message);

    // The modules introduced these attributes; nothing hand-edited the emitter.
    const auto& attributes = compiled.asset.emitters[0].attributes;
    const auto has = [&](std::string_view name)
    { return std::any_of(attributes.begin(),attributes.end(),[&](const Attribute& a) { return a.name == name; }); };
    EXPECT_TRUE(has("Particles.Rotation"));
    EXPECT_TRUE(has("Particles.SubImageIndex"));

    CpuEmitter cpu;
    ASSERT_TRUE(cpu.Initialize(compiled.asset.emitters[0],compiled.emitters[0].spawn,compiled.emitters[0].update,error)) << error;
    ASSERT_TRUE(cpu.Tick(0.5)); ASSERT_GT(cpu.Data().Count(),0u);
    const auto position = cpu.Data().FindAttribute("Particles.Position");
    const auto velocity = cpu.Data().FindAttribute("Particles.Velocity");
    for (uint32_t particle = 0; particle < cpu.Data().Count(); ++particle)
    {
        const auto center = cpu.Data().Read(position,particle);
        const auto speed = cpu.Data().Read(velocity,particle);
        EXPECT_NEAR(speed[1],5.0f,1e-4f) << "a zero cone angle must follow the direction exactly";
        EXPECT_NEAR(std::abs(speed[0])+std::abs(speed[2]),0.0f,1e-4f);
        // The solver integrates position from the spawn velocity, so the sphere
        // radius only holds at spawn; verify the shell there instead.
        const auto travel = speed[1]*static_cast<float>(cpu.Data().Read(cpu.Data().FindAttribute("Particles.Age"),particle)[0]);
        const auto spawnCenter = Value{center[0],center[1]-travel,center[2],0};
        const auto radius = std::sqrt(spawnCenter[0]*spawnCenter[0]+spawnCenter[1]*spawnCenter[1]+spawnCenter[2]*spawnCenter[2]);
        EXPECT_NEAR(radius,2.0f,1e-4f) << "surface sampling must land on the sphere";
    }

    // Update-stage modules are verified directly so the per-particle age is
    // exact instead of depending on when the emitter spawned each particle.
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(7));
    const auto age = data.FindAttribute("Particles.Age"), lifetime = data.FindAttribute("Particles.Lifetime");
    const auto rotationAttr = data.FindAttribute("Particles.Rotation");
    const auto frameAttr = data.FindAttribute("Particles.SubImageIndex");
    std::vector<Value> registers;
    data.Write(lifetime,0,{1,0,0,0});
    // 180 deg/s over a half-second lifetime must land on pi/2 radians.
    for (const float normalized : {0.0f,0.25f,0.5f,1.0f})
    {
        data.Write(age,0,{normalized,0,0,0}); data.Write(rotationAttr,0,{});
        ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.5f, 1, 0, {}}, registers));
        EXPECT_NEAR(data.Read(rotationAttr,0)[0],1.57079633f,1e-4f) << "normalized age " << normalized;
        const auto frameValue = data.Read(frameAttr,0)[0];
        EXPECT_GE(frameValue,0.0f); EXPECT_LE(frameValue,3.0f);
        EXPECT_NEAR(frameValue,std::clamp(normalized,0.0f,1.0f)*3.0f,1e-4f) << "four frames over normalized age";
    }

    emitter.settings.target = SimulationTarget::GPU;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? "" : issues.front().message);
    EXPECT_FALSE(compiled.emitters[0].spawnHlsl.empty());
    EXPECT_FALSE(compiled.emitters[0].updateHlsl.empty());
}

TEST(VfxAuthoring, ForceModulesAccumulateIntoTheSolverAndRejectMissingSolver)
{
    auto system = author::CreateSystem();
    auto& update = *author::FindStack(system.emitters.front(),author::Stage::ParticleUpdate);
    update.modules.clear();
    auto attraction = author::CreateModule("PointAttractionForce");
    attraction.inputs["Attractor Position"].value = {0,10,0,0};
    attraction.inputs["Attraction Strength"].value = {0,0,0,0};
    update.modules.push_back(attraction);

    // A force without the solver must be rejected instead of silently ignored.
    CompiledSystem compiled; std::vector<author::Issue> issues;
    EXPECT_FALSE(author::Compile(system,compiled,issues));
    ASSERT_FALSE(issues.empty());
    EXPECT_NE(issues.front().message.find("Solve Forces"),std::string::npos);

    // The force accumulates first; the solver then consumes it in module order.
    update.modules.push_back(author::CreateModule("SolveForcesAndVelocity"));
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? "" : issues.front().message);

    // Execute the update once for a particle with a known state. With zero
    // strength the attractor is inert, so the solver integrates the unchanged
    // spawn velocity over exactly one delta time.
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(11));
    const auto position = data.FindAttribute("Particles.Position");
    const auto velocity = data.FindAttribute("Particles.Velocity");
    data.Write(position,0,{}); data.Write(velocity,0,{0,3,0,0});
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(position,0)[1],0.3f,1e-4f);
    EXPECT_NEAR(data.Read(velocity,0)[1],3.0f,1e-4f);
}

TEST(VfxAuthoring, EleventhBatchStateConstraintsAndPointForceUseSharedIr)
{
    namespace author = NLS::VFX::Authoring;
    using namespace NLS::VFX;

    // Spawn both scalar state machines from explicit initial values.
    auto system = author::CreateSystem("Eleventh batch");
    auto& emitter = system.emitters.front();
    auto& spawn = *author::FindStack(emitter,author::Stage::ParticleSpawn);
    spawn.modules.resize(1);
    spawn.modules[0].inputs["Lifetime"].value = {10,0,0,0};
    spawn.modules[0].inputs["Position"].value = {0,0,0,0};
    spawn.modules[0].inputs["Velocity"].value = {0,0,0,0};
    auto fadeSpawn = author::CreateModule("FadeOverTime");
    fadeSpawn.inputs["Initial Value"].value = {1,0,0,0};
    fadeSpawn.inputs["Minimum Value"].value = {0.2f,0,0,0};
    spawn.modules.push_back(fadeSpawn);
    auto interpolateSpawn = author::CreateModule("InterpolateOverTime");
    interpolateSpawn.inputs["Initial Value"].value = {2,0,0,0};
    spawn.modules.push_back(interpolateSpawn);

    auto& update = *author::FindStack(emitter,author::Stage::ParticleUpdate);
    update.modules.clear();
    auto fade = author::CreateModule("FadeOverTime");
    fade.inputs["Target Value"].value = {-1,0,0,0};
    fade.inputs["Fade Rate"].value = {0.5f,0,0,0};
    fade.inputs["Minimum Value"].value = {0.2f,0,0,0};
    update.modules.push_back(fade);
    auto interpolate = author::CreateModule("InterpolateOverTime");
    interpolate.inputs["Target Value"].value = {10,0,0,0};
    interpolate.inputs["Rate Of Change"].value = {4,0,0,0};
    update.modules.push_back(interpolate);
    auto distance = author::CreateModule("MaintainA_SetDistanceBetweenPoints");
    distance.inputs["Target Position"].value = {0,0,0,0};
    distance.inputs["Ideal Distance"].value = {2,0,0,0};
    distance.inputs["Influence"].value = {1,0,0,0};
    update.modules.push_back(distance);

    CompiledSystem compiled; std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(system,compiled,issues)) << (issues.empty() ? std::string{"none"} : issues.front().message);
    ParticleDataSet data; data.Initialize(compiled.asset.emitters[0].attributes,1); ASSERT_TRUE(data.Spawn(211));
    const auto fadeAttribute = data.FindAttribute("Particles.FadeValue");
    const auto interpolatedAttribute = data.FindAttribute("Particles.InterpolatedValue");
    const auto positionAttribute = data.FindAttribute("Particles.Position");
    std::vector<Value> registers;
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].spawn, data, 0, 1, {0, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(fadeAttribute,0)[0],1.0f,1e-5f);
    EXPECT_NEAR(data.Read(interpolatedAttribute,0)[0],2.0f,1e-5f);
    // A 0.5*0.1 fade removes five percent of the remaining distance, so the
    // value becomes 0.96. Interpolation adds 0.4 without overshooting.
    data.Write(positionAttribute,0,{3,4,0,0});
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(fadeAttribute,0)[0],0.96f,1e-5f);
    EXPECT_NEAR(data.Read(interpolatedAttribute,0)[0],2.4f,1e-5f);
    EXPECT_NEAR(data.Read(positionAttribute,0)[0],1.2f,1e-5f);
    EXPECT_NEAR(data.Read(positionAttribute,0)[1],1.6f,1e-5f);
    // At the floor it stays there even if the target is below the floor; the
    // clamp applies before each step.
    data.Write(fadeAttribute,0,{0.2f,0,0,0});
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(fadeAttribute,0)[0],0.2f,1e-5f) << "the floor must reject targets below it";

    // A rate beyond the remaining distance snaps exactly to the target.
    data.Write(interpolatedAttribute,0,{9.9f,0,0,0});
    ASSERT_TRUE(RunParticleProgram(compiled.emitters[0].update, data, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(data.Read(interpolatedAttribute,0)[0],10.0f,1e-5f);

    // Pendulum setup writes stable columns and the constraint projects the
    // particle onto the configured sphere without changing its direction.
    auto pendulumSystem = author::CreateSystem("Pendulum");
    auto& pendulumEmitter = pendulumSystem.emitters.front();
    auto& pendulumSpawn = *author::FindStack(pendulumEmitter,author::Stage::ParticleSpawn);
    pendulumSpawn.modules.resize(1);
    pendulumSpawn.modules[0].inputs["Position"].value = {0,0,0,0};
    pendulumSpawn.modules[0].inputs["Velocity"].value = {0,0,0,0};
    auto setup = author::CreateModule("PendulumSetup");
    setup.inputs["Pendulum Pivot"].value = {0,1,0,0};
    setup.inputs["Pendulum Rest Axis"].value = {0,-1,0,0};
    setup.inputs["Pendulum Length"].value = {2,0,0,0};
    pendulumSpawn.modules.push_back(setup);
    auto& pendulumUpdate = *author::FindStack(pendulumEmitter,author::Stage::ParticleUpdate);
    pendulumUpdate.modules.clear();
    pendulumUpdate.modules.push_back(author::CreateModule("PendulumConstraint"));
    CompiledSystem pendulumCompiled; std::vector<author::Issue> pendulumIssues;
    ASSERT_TRUE(author::Compile(pendulumSystem,pendulumCompiled,pendulumIssues)) << (pendulumIssues.empty() ? std::string{"none"} : pendulumIssues.front().message);
    ParticleDataSet pendulumData; pendulumData.Initialize(pendulumCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(pendulumData.Spawn(223));
    ASSERT_TRUE(RunParticleProgram(pendulumCompiled.emitters[0].spawn, pendulumData, 0, 1, {0, 1, 0, {}}, registers));
    const auto pendulumPosition = pendulumData.FindAttribute("Particles.Position");
    pendulumData.Write(pendulumPosition,0,{4,1,0,1});
    ASSERT_TRUE(RunParticleProgram(pendulumCompiled.emitters[0].update, pendulumData, 0, 1, {0.1f, 1, 0, {}}, registers));
    const auto constrained = pendulumData.Read(pendulumPosition,0);
    const auto relative = std::sqrt(constrained[0]*constrained[0] + (constrained[1]-1.0f)*(constrained[1]-1.0f) +
        constrained[2]*constrained[2]);
    EXPECT_NEAR(relative,2.0f,1e-4f) << "length is measured from the pivot, so the direction remains unchanged";
    EXPECT_NEAR(pendulumData.Read(pendulumData.FindAttribute("Particles.PendulumRestAxis"),0)[1],-1.0f,1e-5f);

    // Point force accumulates in the existing solver. With no falloff a
    // positive strength pushes radially away from the origin.
    auto forceSystem = author::CreateSystem("Point force");
    auto& forceUpdate = *author::FindStack(forceSystem.emitters.front(),author::Stage::ParticleUpdate);
    forceUpdate.modules.clear();
    auto point = author::CreateModule("PointForce");
    point.inputs["Force Origin"].value = {0,0,0,0};
    point.inputs["Force Strength"].value = {2,0,0,0};
    point.inputs["Falloff Distance"].value = {1,0,0,0};
    point.inputs["Use Falloff"].value = {0,0,0,0};
    point.inputs["Invert Falloff"].value = {0,0,0,0};
    forceUpdate.modules.push_back(point);
    forceUpdate.modules.push_back(author::CreateModule("SolveForcesAndVelocity"));
    CompiledSystem forceCompiled; std::vector<author::Issue> forceIssues;
    ASSERT_TRUE(author::Compile(forceSystem,forceCompiled,forceIssues)) << (forceIssues.empty() ? std::string{"none"} : forceIssues.front().message);
    ParticleDataSet forceData; forceData.Initialize(forceCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(forceData.Spawn(227));
    const auto forcePosition = forceData.FindAttribute("Particles.Position");
    const auto forceVelocity = forceData.FindAttribute("Particles.Velocity");
    forceData.Write(forcePosition,0,{1,0,0,0}); forceData.Write(forceVelocity,0,{0,0,0,0});
    ASSERT_TRUE(RunParticleProgram(forceCompiled.emitters[0].update, forceData, 0, 1, {0.5f, 1, 0, {}}, registers));
    EXPECT_NEAR(forceData.Read(forceVelocity,0)[0],1.0f,1e-4f) << "force 2 over 0.5 seconds";
    EXPECT_NEAR(forceData.Read(forcePosition,0)[0],1.5f,1e-4f);

    // Source movement adds to velocity with its scale; it does not depend on
    // a second force accumulator or a new parameter namespace.
    auto sourceSystem = author::CreateSystem("Source movement");
    auto& sourceSpawn = *author::FindStack(sourceSystem.emitters.front(),author::Stage::ParticleSpawn);
    sourceSpawn.modules.resize(1);
    sourceSpawn.modules[0].inputs["Velocity"].value = {0,0,0,0};
    auto source = author::CreateModule("InheritSourceMovement");
    source.inputs["Source Velocity"].value = {6,0,0,0};
    source.inputs["Applied Force Scale"].value = {0.5f,0,0,0};
    sourceSpawn.modules.push_back(source);
    CompiledSystem sourceCompiled; std::vector<author::Issue> sourceIssues;
    ASSERT_TRUE(author::Compile(sourceSystem,sourceCompiled,sourceIssues)) << (sourceIssues.empty() ? std::string{"none"} : sourceIssues.front().message);
    ParticleDataSet sourceData; sourceData.Initialize(sourceCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(sourceData.Spawn(228));
    const auto sourceVelocity = sourceData.FindAttribute("Particles.Velocity");
    sourceData.Write(sourceVelocity,0,{1,0,0,0});
    ASSERT_TRUE(RunParticleProgram(sourceCompiled.emitters[0].spawn, sourceData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(sourceData.Read(sourceVelocity,0)[0],3.0f,1e-5f);

    // Angle limiting uses the configured rest axis. A direction outside the
    // 45 degree cone is projected to the cone boundary while the radius stays
    // unchanged.
    auto conePendulumSystem = pendulumSystem;
    auto& coneSpawn = *author::FindStack(conePendulumSystem.emitters.front(),author::Stage::ParticleSpawn);
    coneSpawn.modules.back().inputs["Pendulum Pivot"].value = {0,1,0,0};
    coneSpawn.modules.back().inputs["Pendulum Rest Axis"].value = {0,-1,0,0};
    coneSpawn.modules.back().inputs["Pendulum Length"].value = {2,0,0,0};
    auto& coneUpdate = *author::FindStack(conePendulumSystem.emitters.front(),author::Stage::ParticleUpdate);
    auto coneConstraint = author::CreateModule("PendulumConstraint");
    coneConstraint.inputs["Use Angle Constraint"].value = {1,0,0,0};
    coneConstraint.inputs["Angle Constraint"].value = {45,0,0,0};
    coneUpdate.modules = {coneConstraint};
    CompiledSystem coneCompiled; std::vector<author::Issue> coneIssues;
    ASSERT_TRUE(author::Compile(conePendulumSystem,coneCompiled,coneIssues)) << (coneIssues.empty() ? std::string{"none"} : coneIssues.front().message);
    ParticleDataSet coneData; coneData.Initialize(coneCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(coneData.Spawn(231));
    ASSERT_TRUE(RunParticleProgram(coneCompiled.emitters[0].spawn, coneData, 0, 1, {0, 1, 0, {}}, registers));
    const auto conePosition = coneData.FindAttribute("Particles.Position");
    coneData.Write(conePosition,0,{2,1,0,1});
    ASSERT_TRUE(RunParticleProgram(coneCompiled.emitters[0].update, coneData, 0, 1, {0.1f, 1, 0, {}}, registers));
    const auto coneOffset = coneData.Read(conePosition,0);
    const auto coneDot = (coneOffset[1]-1.0f)/std::sqrt(coneOffset[0]*coneOffset[0] + (coneOffset[1]-1.0f)*(coneOffset[1]-1.0f));
    EXPECT_NEAR(coneDot,-std::sqrt(0.5f),1e-4f) << "outside direction is clamped to 45 degrees from the rest axis";

    // The spring driver writes a velocity correction through the same
    // pendulum columns, so it remains compatible with the force solver.
    auto springPendulumSystem = conePendulumSystem;
    auto& springSpawn = *author::FindStack(springPendulumSystem.emitters.front(),author::Stage::ParticleSpawn);
    springSpawn.modules.back().inputs["Enable Spring Driver"].value = {1,0,0,0};
    springSpawn.modules.back().inputs["Spring Tightness"].value = {2,0,0,0};
    auto& springUpdate = *author::FindStack(springPendulumSystem.emitters.front(),author::Stage::ParticleUpdate);
    springUpdate.modules.clear();
    auto springConstraint = author::CreateModule("PendulumConstraint");
    springConstraint.inputs["Use Angle Constraint"].value = {0,0,0,0};
    springUpdate.modules.push_back(springConstraint);
    CompiledSystem springCompiled; std::vector<author::Issue> springIssues;
    ASSERT_TRUE(author::Compile(springPendulumSystem,springCompiled,springIssues)) << (springIssues.empty() ? std::string{"none"} : springIssues.front().message);
    EXPECT_NE(std::find_if(springCompiled.asset.emitters[0].attributes.begin(),springCompiled.asset.emitters[0].attributes.end(),
        [](const Attribute& attribute) { return attribute.name == "Particles.PendulumSpring"; }),
        springCompiled.asset.emitters[0].attributes.end());
    ParticleDataSet springData; springData.Initialize(springCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(springData.Spawn(233));
    ASSERT_TRUE(RunParticleProgram(springCompiled.emitters[0].spawn, springData, 0, 1, {0, 1, 0, {}}, registers));
    const auto springPosition = springData.FindAttribute("Particles.Position");
    const auto springVelocity = springData.FindAttribute("Particles.Velocity");
    EXPECT_NEAR(springData.Read(springData.FindAttribute("Particles.PendulumSpring"),0)[0],1.0f,1e-5f);
    EXPECT_NEAR(springData.Read(springData.FindAttribute("Particles.PendulumTightness"),0)[0],2.0f,1e-5f);
    springData.Write(springPosition,0,{3,1,0,1}); springData.Write(springVelocity,0,{0,0,0,0});
    ASSERT_TRUE(RunParticleProgram(springCompiled.emitters[0].update, springData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(springData.Read(springPosition,0)[0],2.0f,1e-5f) << "spring mode preserves the pendulum length";
    EXPECT_NEAR(springData.Read(springVelocity,0)[0],-0.2f,1e-5f) << "the spring impulse points back toward the pivot";

    // Ribbon width scaling consumes the column established by the width
    // module instead of overwriting it with a fresh constant.
    auto ribbonSystem = author::CreateSystem("Ribbon scale");
    auto& ribbonUpdate = *author::FindStack(ribbonSystem.emitters.front(),author::Stage::ParticleUpdate);
    ribbonUpdate.modules.clear();
    auto baseWidth = author::CreateModule("RibbonWidth");
    baseWidth.inputs["Width"].value = {0.25f,0,0,0};
    baseWidth.inputs["Width Scale"].value = {1,0,0,0};
    ribbonUpdate.modules.push_back(baseWidth);
    auto scaledWidth = author::CreateModule("ScaleRibbonWidth");
    scaledWidth.inputs["Scale"].value = {0.5f,0,0,0};
    ribbonUpdate.modules.push_back(scaledWidth);
    CompiledSystem ribbonCompiled; std::vector<author::Issue> ribbonIssues;
    ASSERT_TRUE(author::Compile(ribbonSystem,ribbonCompiled,ribbonIssues)) << (ribbonIssues.empty() ? std::string{"none"} : ribbonIssues.front().message);
    ParticleDataSet ribbonData; ribbonData.Initialize(ribbonCompiled.asset.emitters[0].attributes,1); ASSERT_TRUE(ribbonData.Spawn(229));
    const auto ribbonWidth = ribbonData.FindAttribute("Particles.RibbonWidth");
    ASSERT_TRUE(RunParticleProgram(ribbonCompiled.emitters[0].update, ribbonData, 0, 1, {0.1f, 1, 0, {}}, registers));
    EXPECT_NEAR(ribbonData.Read(ribbonWidth,0)[0],0.125f,1e-5f);
}


TEST(VfxAuthoring, GpuEventDependenciesRejectCyclesSelfBindingsAndMissingSources)
{
    using namespace NLS::VFX;
    auto system = CreateDefaultSystem("GPU event dependencies");
    system.emitters.front().target = SimulationTarget::GPU;
    auto other = system.emitters.front(); other.id = NLS::Guid::New().ToString();
    system.emitters.push_back(other);
    for (size_t index = 0; index < system.emitters.size(); ++index)
    {
        EmitterAsset::EventHandler handler; handler.id = NLS::Guid::New().ToString();
        handler.receiver.eventKind = ParticleEventSelection::Spawn;
        handler.receiver.sourceEmitter = system.emitters[1-index].id;
        system.emitters[index].eventHandlers = {handler};
    }
    CompiledSystem compiled; std::vector<Diagnostic> diagnostics;
    EXPECT_FALSE(CompileSystem(system,compiled,diagnostics));
    ASSERT_FALSE(diagnostics.empty()); EXPECT_NE(diagnostics[0].message.find("cycle"),std::string::npos);
    system.emitters[0].eventHandlers.clear();
    system.emitters[1].eventHandlers[0].receiver.sourceEmitter = system.emitters[1].id;
    EXPECT_FALSE(CompileSystem(system,compiled,diagnostics));
    ASSERT_FALSE(diagnostics.empty()); EXPECT_NE(diagnostics[0].message.find("own emitter"),std::string::npos);
    system.emitters[1].eventHandlers[0].receiver.sourceEmitter = NLS::Guid::New().ToString();
    EXPECT_FALSE(CompileSystem(system,compiled,diagnostics));
    ASSERT_FALSE(diagnostics.empty()); EXPECT_NE(diagnostics[0].message.find("missing"),std::string::npos);
}


TEST(VfxAuthoring, SingleParticleEventSelectsStableIdAndIgnoresMissingId)
{
    using namespace NLS::VFX;
    auto system = CreateDefaultSystem("Stable event ID"); auto& asset = system.emitters.front();
    asset.spawnRate = 0; asset.bursts = {{0,3}}; asset.eventGenerationMask = 0;
    EmitterAsset::EventHandler handler; handler.id = NLS::Guid::New().ToString();
    handler.receiver.eventKind = ParticleEventSelection::Spawn;
    handler.receiver.executionMode = EventExecutionMode::SingleParticle;
    handler.graph.id = NLS::Guid::New().ToString(); handler.graph.stage = ScriptStage::Event;
    const auto value = AddNode(handler.graph,Operation::Constant); FindNode(handler.graph,value)->value = {7,0,0,0};
    const auto write = AddNode(handler.graph,Operation::WriteAttribute); FindNode(handler.graph,write)->name = "Particles.Size";
    FindNode(handler.graph,write)->inputs = {value}; asset.eventHandlers.push_back(handler);
    CompiledSystem compiled; std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(system,compiled,diagnostics)) << (diagnostics.empty()?"":diagnostics[0].message);
    std::string error; CpuEmitter receiver;
    const auto& program = compiled.emitters.front();
    ASSERT_TRUE(receiver.Initialize(compiled.asset.emitters.front(),program.spawn,program.update,error,{},
        program.emitterUpdate,program.emitterCollision,program.eventHandlers,program.eventParticles)) << error;
    ASSERT_TRUE(receiver.TickStep(1.0/60.0));
    ASSERT_EQ(receiver.Data().Count(),3u);
    ParticleEvent event{ParticleEventKind::Spawn,2,{1,2,3,0}};
    uint64_t cursor = 100;
    ASSERT_TRUE(receiver.ReceiveEvents(0,std::span(&event,1),1,0,cursor));
    event.particleId = 999;
    ASSERT_TRUE(receiver.ReceiveEvents(0,std::span(&event,1),1,0,cursor));
    const auto size = receiver.Data().FindAttribute("Particles.Size");
    for (uint32_t particle = 0; particle < receiver.Data().Count(); ++particle)
        EXPECT_FLOAT_EQ(receiver.Data().Read(size,particle)[0],receiver.Data().Id(particle)==2 ? 7.0f : 0.05f);
    EXPECT_EQ(cursor,100u);
}


TEST(VfxAuthoring, StructuredModuleRunsBindingsAndAtomicUpdatesAfterCook)
{
    using namespace NLS::VFX; namespace author=NLS::VFX::Authoring;
    auto asset=Tests::StructuredModule();std::string error;author::ScriptAsset restored;
    ASSERT_TRUE(author::DeserializeScript(author::SerializeScript(asset),restored,error)) << error;
    EXPECT_EQ(restored.versions[0].graph.structs,asset.versions[0].graph.structs);
    EXPECT_EQ(restored.versions[0].graph.dataInterfaces,asset.versions[0].graph.dataInterfaces);
    author::SourceLibrary library;ASSERT_TRUE(library.Add(restored,{},error)) << error;
    auto source=Tests::StructuredSystem(restored,SimulationTarget::CPU);
    CompiledSystem compiled;std::vector<author::SourceDependency> dependencies;std::vector<author::Issue> issues;
    ASSERT_TRUE(author::Compile(source,library,compiled,dependencies,issues)) << (issues.empty()?"":issues[0].message);
    ASSERT_FALSE(compiled.emitters[0].update.dataInterfaces.empty());
    CompiledSystem cooked;
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled,false),cooked,error)) << error;
    EXPECT_TRUE(cooked.asset.emitters[0].update.nodes.empty());
    auto frozen=std::make_shared<CompiledSystem>(cooked);SystemInstance world;
    ASSERT_TRUE(world.Initialize(frozen,error)) << error;ASSERT_TRUE(world.Tick(1.0/60.0));
    ASSERT_EQ(world.CpuEmitters()[0]->Data().Count(),4u);
    const auto check=[&](Value position,float size,int32_t code)
    {
        const auto& data=world.CpuEmitters()[0]->Data();ASSERT_EQ(data.Count(),4u);
        for(uint32_t particle=0;particle<data.Count();++particle)
        {
            const auto p=data.Read(data.FindAttribute("Particles.Position"),particle);
            for(uint32_t c=0;c<3;++c)EXPECT_FLOAT_EQ(p[c],position[c]);
            EXPECT_FLOAT_EQ(data.Read(data.FindAttribute("Particles.Size"),particle)[0],size);
            EXPECT_EQ(ReadInt(data.Read(data.FindAttribute("Particles.LookupResult.code"),particle)),code);
            EXPECT_EQ(ReadInt(data.Read(data.FindAttribute("Particles.LookupCount"),particle)),3);
        }
    };
    check({12,15,18,0},3,INT32_MAX);
    const auto old=world.Capture();
    const std::array<Value,3> points{{{10,20,30,0},{40,50,60,0},{70,80,90,0}}};
    ASSERT_TRUE(world.SetDataInterface("User.Points",ValueType::Float3,points));
    ASSERT_TRUE(world.Tick(1.0/60.0));check({120,150,180,0},3,INT32_MAX);
    EXPECT_FLOAT_EQ(old->cpu[0]->Read(old->cpu[0]->FindAttribute("Particles.Position"),0)[0],12);
    const std::array<Value,2> curve{{{0,4,0,0},{1,12,0,0}}};
    ASSERT_TRUE(world.SetDataInterface("User.WeightCurve",ValueType::Float4,curve));
    ASSERT_TRUE(world.Tick(1.0/60.0));check({240,300,360,0},6,INT32_MAX);
    const auto unchanged=world.Capture();
    EXPECT_FALSE(world.SetDataInterface("User.Points",ValueType::Float,points));
    EXPECT_FALSE(world.SetDataInterface("User.Points",ValueType::Float3,std::span(points.data(),2)));
    const std::array<Value,2> badCurve{{{1,4,0,0},{0,12,0,0}}};
    EXPECT_FALSE(world.SetDataInterface("User.WeightCurve",ValueType::Float4,badCurve));
    EXPECT_EQ(world.Capture(),unchanged);
    ASSERT_TRUE(world.SetParameters({{"User.Lookup.state.index",IntValue(2)},{"User.Lookup.state.weight",{0.5f,0,0,0}},
        {"User.Lookup.position",{1,2,3,0}}}));
    ASSERT_TRUE(world.Tick(1.0/60.0));check({561,642,723,0},8,-7);
}

TEST(VfxAuthoring, StructuredTypesAndDataInterfacesRejectMalformedGraphsAndCookedBindings)
{
    using namespace NLS::VFX;namespace author=NLS::VFX::Authoring;
    const auto original=Tests::StructuredModule();
    const auto rejects=[&](author::ScriptAsset asset,std::string_view expected)
    {
        author::SourceLibrary library;std::string error;
        if(!library.Add(asset,{},error)) {EXPECT_NE(error.find(expected),std::string::npos) << error;return;}
        CompiledSystem output;std::vector<author::Issue> issues;std::vector<author::SourceDependency> dependencies;
        EXPECT_FALSE(author::Compile(Tests::StructuredSystem(asset,SimulationTarget::CPU),library,output,dependencies,issues));
        ASSERT_FALSE(issues.empty());EXPECT_NE(issues[0].message.find(expected),std::string::npos) << issues[0].message;
    };
    auto bad=original;bad.versions[0].graph.structs[0].fields[0].structType="LookupContext";rejects(bad,"Recursive");
    bad=original;for(auto& node:bad.versions[0].graph.nodes)if(node.operation==Operation::StructGet && node.name=="point")node.name="missing";
    rejects(bad,"Unknown struct field");
    bad=original;for(auto& node:bad.versions[0].graph.nodes)if(node.operation==Operation::DataInterfaceCall && node.name=="User.Points.Get")node.name="Missing.Get";
    rejects(bad,"Unbound data interface");
    bad=original;bad.versions[0].graph.dataInterfaces[0].elementType=ValueType::Float;
    rejects(bad,"result type mismatch");
    bad=original;bad.versions[0].graph.dataInterfaces[1].data[1][0]=0;rejects(bad,"strictly increasing");
}

TEST(VfxAuthoring, DataInterfaceCookedBindingsRejectCorruptionWithoutReplacingTheProgram)
{
    using namespace NLS::VFX;namespace author=NLS::VFX::Authoring;
    auto module=Tests::StructuredModule();author::SourceLibrary library;std::string error;
    ASSERT_TRUE(library.Add(module,{},error));
    CompiledSystem compiled;std::vector<author::Issue> issues;std::vector<author::SourceDependency> dependencies;
    ASSERT_TRUE(author::Compile(Tests::StructuredSystem(module,SimulationTarget::CPU),library,compiled,dependencies,issues));
    const auto text=SerializeCompiledSystem(compiled,false);const auto original=nlohmann::json::parse(text);
    EXPECT_EQ(original.at("version"), 7);
    auto preserved=compiled;
    const auto reject=[&](auto mutation)
    {
        auto malformed=original;mutation(malformed);
        EXPECT_FALSE(DeserializeCompiledSystem(malformed.dump(),preserved,error));EXPECT_FALSE(error.empty());
        EXPECT_EQ(SerializeCompiledSystem(preserved,false),text);
    };
    reject([](auto& json){json["version"]=5;});
    reject([](auto& json){json["emitters"][0]["spawn"]["dataInterfaces"][0]["kind"]=99;});
    reject([](auto& json){json["emitters"][0]["spawn"]["dataInterfaces"][1]["data"][1][0]=0;});
    reject([](auto& json)
           {for(auto& instruction:json["emitters"][0]["spawn"]["instructions"])if(instruction["operation"]==static_cast<uint32_t>(Operation::DataInterfaceCall)){instruction["inputs"]=nlohmann::json::array({99,99});break;} });
    reject([](auto& json)
    {for(auto& instruction:json["emitters"][0]["spawn"]["instructions"])if(instruction["operation"]==static_cast<uint32_t>(Operation::DataInterfaceCall)){instruction["attribute"]=999;break;}});
    reject([](auto& json)
           {for(auto& instruction:json["emitters"][0]["spawn"]["instructions"])if(instruction["operation"]==static_cast<uint32_t>(Operation::DataInterfaceCall)){instruction["name"]="Unknown.Get";break;} });
    reject([](auto& json)
           {for(auto& parameter:json["emitters"][0]["spawn"]["parameters"])if(parameter["name"]==DataInterfaceParameter("User.Points",0)){parameter["name"]="Missing.Uniform";break;} });
}


TEST(VfxAuthoring, TypedStructBytecodeAndShaderLowerNestedFieldsWithoutLosingIntegerBits)
{
    namespace typed=NLS::VFX::Typed;
    typed::TypeRegistry types;std::string error;
    const std::array innerFields{typed::FieldSpec{"code",typed::kInt32},typed::FieldSpec{"weight",typed::kFloat}};
    const auto inner=types.AddStruct("Inner",innerFields,error);ASSERT_NE(inner,typed::kInvalidType);
    const std::array outerFields{typed::FieldSpec{"inner",inner},typed::FieldSpec{"enabled",typed::kBool}};
    const auto outer=types.AddStruct("Outer",outerFields,error);
    typed::Graph graph;graph.stage=NLS::VFX::ScriptStage::SimulationStage;
    const auto constant=[&](typed::TypeId type,float value)
    {typed::Node node;node.operation=typed::Operation::Constant;node.type=type;node.value.type=type;node.value.scalar[0]=value;graph.nodes.push_back(node);return static_cast<uint32_t>(graph.nodes.size()-1);};
    const auto code=constant(typed::kInt32,std::bit_cast<float>(INT32_MIN));
    const auto weight=constant(typed::kFloat,0.25f);const auto enabled=constant(typed::kBool,std::bit_cast<float>(int32_t{-1}));
    typed::Node make;make.operation=typed::Operation::StructMake;make.type=inner;make.inputs={code,weight};graph.nodes.push_back(make);
    make.type=outer;make.inputs={3,enabled};graph.nodes.push_back(make);
    typed::Node get;get.operation=typed::Operation::StructGet;get.type=typed::kInt32;get.inputs={4};get.fieldIndices={0,0};graph.nodes.push_back(get);
    typed::Node write;write.operation=typed::Operation::WriteAttribute;write.type=typed::kInt32;write.name="Particles.Code";write.inputs={5};graph.nodes.push_back(write);
    typed::OptimizationResult layout;typed::OptimizationStats stats;
    ASSERT_TRUE(typed::OptimizeGraph(graph,types,layout,stats,error)) << error;
    typed::BytecodeProgram program;ASSERT_TRUE(typed::CompileToBytecode(graph,types,layout,program,error)) << error;
    typed::BytecodeExecutionContext context;context.types=&types;
    ASSERT_TRUE(typed::ExecuteBytecode(program,context,error)) << error;
    EXPECT_EQ(std::bit_cast<int32_t>(context.attributes.at("Particles.Code").scalar[0]),INT32_MIN);
    const std::vector<NLS::VFX::Attribute> attributes{{"Particles.Code",NLS::VFX::ValueType::Int32,{}}};
    std::string hlsl;ASSERT_TRUE(typed::GenerateTypedComputeHlsl(graph,layout,types,attributes,graph.stage,hlsl,error)) << error;
    EXPECT_FALSE(hlsl.empty());
    for(auto& node:graph.nodes)if(node.operation==typed::Operation::StructGet)node.fieldIndices={9};
    EXPECT_FALSE(typed::CompileToBytecode(graph,types,layout,program,error));
    EXPECT_FALSE(typed::GenerateTypedComputeHlsl(graph,layout,types,attributes,graph.stage,hlsl,error));
}


TEST(VfxAuthoring, ArrayNodesMatchUe574SourceContractsAndSignatureDiagnostics)
{
    using namespace NLS::VFX;
    std::ifstream input("Tests/Reference/Niagara574ArrayContracts.json");ASSERT_TRUE(input);
    const auto reference=nlohmann::json::parse(input);EXPECT_EQ(reference.at("baseline"),"UE 5.7.4 Niagara");
    Graph graph;graph.id=NLS::Guid::New().ToString();graph.stage=ScriptStage::SimulationStage;
    DataInterfaceDefinition binding{"Reference.Array",DataInterfaceKind::Array,ValueType::Int32,{}};
    for(const auto& value:reference["array"]["values"])binding.data.push_back(IntValue(value.get<int32_t>()));
    graph.dataInterfaces={binding};
    const auto index=AddNode(graph,Operation::Parameter,ValueType::Int32);FindNode(graph,index)->name="User.Index";
    const std::vector<Attribute> attributes{{"Particles.Value",ValueType::Int32,{}},{"Particles.Valid",ValueType::Bool,{}},{"Particles.Count",ValueType::Int32,{}}};
    for(size_t function=0;function<3;++function)
    {
        const auto name=function==0?"Get":function==1?"IsValidIndex":"Length";
        const auto call=AddNode(graph,Operation::DataInterfaceCall,attributes[function].type);
        FindNode(graph,call)->name="Reference.Array."+std::string(name);
        FindNode(graph,call)->inputs=function==2?std::vector<NodeId>{}:std::vector<NodeId>{index};
        const auto write=AddNode(graph,Operation::WriteAttribute,attributes[function].type);
        FindNode(graph,write)->name=attributes[function].name;FindNode(graph,write)->inputs={call};
    }
    const auto check=[&](const Graph& source,const nlohmann::json& cases,int32_t length)
    {
        auto result=CompileGraph(source,attributes);ASSERT_TRUE(result.Succeeded()) << (result.diagnostics.empty()?"":result.diagnostics[0].message);
        ParticleDataSet data;data.Initialize(attributes,1);ASSERT_TRUE(data.Spawn(1));
        Typed::TypeRegistry types;
        const auto& typed = result.compiled.program;
        std::string error;
        for(const auto& value:cases)
        {
            std::unordered_map<std::string,Value> parameters{{"User.Index",IntValue(value["index"].get<int32_t>())}};
            ExecutionContext context;context.parameters=&parameters;std::vector<Value> registers;
            ASSERT_TRUE(RunParticleProgram(result.compiled.program, data, 0, 1, context, registers));
            EXPECT_EQ(ReadInt(data.Read(0,0)),value["get"].get<int32_t>());EXPECT_EQ(ReadBool(data.Read(1,0)),value["valid"].get<bool>());EXPECT_EQ(ReadInt(data.Read(2,0)),length);
        }
    };
    check(graph,reference["array"]["cases"],3);
    auto empty=graph;empty.dataInterfaces[0].data.clear();
    const auto cases=nlohmann::json::array({{{"index",-1},{"get",0},{"valid",false}},{{"index",0},{"get",0},{"valid",false}},{{"index",INT32_MAX},{"get",0},{"valid",false}}});
    check(empty,cases,0);
    auto invalid=graph;FindNode(invalid,index)->type=ValueType::Float;
    auto rejected=CompileGraph(invalid,attributes);EXPECT_FALSE(rejected.Succeeded());ASSERT_FALSE(rejected.diagnostics.empty());
    EXPECT_NE(rejected.diagnostics[0].message.find("Int32"),std::string::npos);
}


TEST(VfxAuthoring, SystemAndEmitterStateExecuteCookedTypedBytecodeOnSpawnUpdateAndReset)
{
    using namespace NLS::VFX;
    auto source = CreateDefaultSystem("Native typed stages");
    auto& emitter = source.emitters[0];
    emitter.spawnRate = 0;
    emitter.duration = 100;
    emitter.bursts.clear();
    source.systemAttributes.push_back({"System.Counter", ValueType::Int32, IntValue(0)});
    emitter.emitterAttributes.push_back({"Emitter.Counter", ValueType::Int32, IntValue(0)});
    source.parameters = {{"User.Initial", ValueType::Int32, IntValue(16777217)}, {"User.Rate", ValueType::Float, {120, 0, 0, 0}}};
    const auto output = [&](Graph& graph, const std::string& name, ValueType type, NodeId value)
    {const auto write=AddNode(graph,Operation::WriteAttribute,type);FindNode(graph,write)->name=name;FindNode(graph,write)->inputs={value}; };
    const auto spawn = [&](Graph& graph, const std::string& name)
    {const auto input=AddNode(graph,Operation::Parameter,ValueType::Int32);FindNode(graph,input)->name="User.Initial";FindNode(graph,input)->value=IntValue(16777217);output(graph,name,ValueType::Int32,input); };
    const auto increment = [&](Graph& graph, const std::string& name)
    {
        const auto input = AddNode(graph, Operation::Attribute, ValueType::Int32);
        FindNode(graph, input)->name = name;
        const auto one = AddNode(graph, Operation::Constant, ValueType::Int32);
        FindNode(graph, one)->value = IntValue(1);
        const auto sum = AddNode(graph, Operation::Add, ValueType::Int32);
        FindNode(graph, sum)->inputs = {input, one};
        output(graph, name, ValueType::Int32, sum);
    };
    spawn(source.systemSpawn, "System.Counter");
    increment(source.systemUpdate, "System.Counter");
    spawn(emitter.emitterSpawn, "Emitter.Counter");
    increment(emitter.emitterUpdate, "Emitter.Counter");
    const auto rate = AddNode(emitter.emitterUpdate, Operation::Parameter);
    FindNode(emitter.emitterUpdate, rate)->name = "User.Rate";
    FindNode(emitter.emitterUpdate, rate)->value = {120, 0, 0, 0};
    output(emitter.emitterUpdate, "Emitter.SpawnRate", ValueType::Float, rate);
    CompiledSystem compiled;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(source, compiled, diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    std::string error;
    CompiledSystem cooked;
    ASSERT_TRUE(DeserializeCompiledSystem(SerializeCompiledSystem(compiled, false), cooked, error)) << error;
    EXPECT_TRUE(cooked.asset.systemSpawn.nodes.empty());
    EXPECT_TRUE(cooked.asset.emitters[0].emitterSpawn.nodes.empty());
    SystemInstance world;
    ASSERT_TRUE(world.Initialize(std::make_shared<CompiledSystem>(cooked), error)) << error;
    EXPECT_EQ(ReadInt(world.Capture()->systemAttributes.at("System.Counter")), 16777217);
    ASSERT_TRUE(world.Tick(1.0 / 60.0));
    EXPECT_EQ(ReadInt(world.Capture()->systemAttributes.at("System.Counter")), 16777218);
    EXPECT_EQ(world.CpuEmitters()[0]->Data().Count(), 2u);
    world.Reset();
    EXPECT_EQ(ReadInt(world.Capture()->systemAttributes.at("System.Counter")), 16777217);
    EmitterScript script;
    const auto& program = cooked.emitters[0];
    ASSERT_TRUE(script.Initialize(cooked.asset.emitters[0].emitterAttributes, program.emitterUpdate, program.emitterSpawn));
    EXPECT_EQ(ReadInt(script.ReadValue("Emitter.Counter")), 16777217);
    ASSERT_TRUE(script.Tick({}));
    EXPECT_EQ(ReadInt(script.ReadValue("Emitter.Counter")), 16777218);
    ASSERT_TRUE(script.Tick({}));
    EXPECT_EQ(ReadInt(script.ReadValue("Emitter.Counter")), 16777219);
    std::unordered_map<std::string, Value> parameters{{"User.Initial", IntValue(INT32_MAX)}};
    ExecutionContext reset;
    reset.parameters = &parameters;
    ASSERT_TRUE(script.Reset(reset));
    EXPECT_EQ(ReadInt(script.ReadValue("Emitter.Counter")), INT32_MAX);
    ASSERT_TRUE(script.Tick({}));
    EXPECT_EQ(ReadInt(script.ReadValue("Emitter.Counter")), INT32_MIN);
}

TEST(VfxAuthoring, InvalidTypedProgramsAreRejectedBeforeAnyRuntimeWrites)
{
    using namespace NLS::VFX;
    auto source = CreateDefaultSystem("Reject bad bytecode");
    CompiledSystem compiled;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(source, compiled, diagnostics));
    auto invalid = compiled.emitters[0].spawn;
    invalid.registerCount++;
    CpuEmitter emitter;
    std::string error;
    EXPECT_FALSE(emitter.Initialize(source.emitters[0], invalid, compiled.emitters[0].update, error));
    EXPECT_FALSE(error.empty());
    invalid = compiled.emitters[0].spawn;
    ASSERT_FALSE(invalid.instructions.empty());
    invalid.instructions[0].operation = static_cast<Operation>(255);
    EXPECT_FALSE(emitter.Initialize(source.emitters[0], invalid, compiled.emitters[0].update, error));
    EXPECT_FALSE(error.empty());
    ParticleDataSet data;
    data.Initialize(source.emitters[0].attributes, 1);
    ASSERT_TRUE(data.Spawn(1));
    data.Write(data.FindAttribute("Particles.Position"), 0, {9, 8, 7, 0});
    EXPECT_FALSE(Typed::ExecuteParticleProgram(invalid, data, 0, 1, {}, error));
    EXPECT_EQ(data.Read(data.FindAttribute("Particles.Position"), 0), (Value{9, 8, 7, 0}));
    auto corrupt = std::make_shared<CompiledSystem>(compiled);
    corrupt->systemUpdate.registerCount = 1;
    SystemInstance world;
    EXPECT_FALSE(world.Initialize(corrupt, error));
    EXPECT_FALSE(error.empty());
    EmitterScript script;
    EXPECT_FALSE(script.Initialize(source.emitters[0].attributes, invalid));
    auto malformed = nlohmann::json::parse(SerializeCompiledSystem(compiled, false));
    auto preserved = compiled;
    const auto before = SerializeCompiledSystem(preserved, false);
    malformed["emitters"][0]["spawn"]["registerCount"] = 999;
    EXPECT_FALSE(DeserializeCompiledSystem(malformed.dump(), preserved, error));
    EXPECT_EQ(SerializeCompiledSystem(preserved, false), before);
    malformed = nlohmann::json::parse(before);
    for (auto& attribute : malformed["emitters"][0]["spawn"]["attributes"])
        if (attribute["write"].get<bool>()) { attribute["write"] = false; break; }
    EXPECT_FALSE(DeserializeCompiledSystem(malformed.dump(), preserved, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(SerializeCompiledSystem(preserved, false), before);
    malformed = nlohmann::json::parse(before);
    malformed["version"] = 6;
    EXPECT_FALSE(DeserializeCompiledSystem(malformed.dump(), preserved, error));
    EXPECT_EQ(SerializeCompiledSystem(preserved, false), before);
}

TEST(VfxAuthoring, TypedFrontendRejectsDisconnectedInvalidNodesAndUsesIncomingStageValues)
{
    using namespace NLS::VFX;
    auto source = CreateDefaultSystem();
    auto& graph = source.emitters[0].update;
    const auto invalid = AddNode(graph, Operation::Constant);
    FindNode(graph, invalid)->operation = static_cast<Operation>(255);
    EXPECT_FALSE(CompileGraph(graph, source.emitters[0].attributes).Succeeded());
    FindNode(graph, invalid)->operation = Operation::StageIndex;
    EXPECT_FALSE(CompileGraph(graph, source.emitters[0].attributes).Succeeded());
    Graph incoming;
    incoming.id = NLS::Guid::New().ToString();
    incoming.stage = ScriptStage::ParticleUpdate;
    const auto value = AddNode(incoming, Operation::Constant);
    FindNode(incoming, value)->value = {5, 0, 0, 0};
    const auto first = AddNode(incoming, Operation::WriteAttribute);
    FindNode(incoming, first)->name = "Particles.Size";
    FindNode(incoming, first)->inputs = {value};
    const auto read = AddNode(incoming, Operation::Attribute);
    FindNode(incoming, read)->name = "Particles.Size";
    const auto second = AddNode(incoming, Operation::WriteAttribute);
    FindNode(incoming, second)->name = "Particles.Age";
    FindNode(incoming, second)->inputs = {read};
    auto result = CompileGraph(incoming, source.emitters[0].attributes);
    ASSERT_TRUE(result.Succeeded());
    ParticleDataSet data;
    data.Initialize(source.emitters[0].attributes, 1);
    ASSERT_TRUE(data.Spawn(1));
    std::string error;
    ASSERT_TRUE(Typed::ExecuteParticleProgram(result.compiled.program, data, 0, 1, {}, error)) << error;
    EXPECT_FLOAT_EQ(data.Read(data.FindAttribute("Particles.Size"), 0)[0], 5);
    EXPECT_FLOAT_EQ(data.Read(data.FindAttribute("Particles.Age"), 0)[0], 0.05f);
}
