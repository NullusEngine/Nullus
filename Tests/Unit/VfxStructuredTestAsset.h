#pragma once
#include <VFX/Authoring/VfxLibrary.h>
#include <VFXCompiler/VfxStackCompiler.h>
#include <Guid.h>
#include <limits>

namespace NLS::VFX::Tests
{
inline Authoring::ScriptAsset StructuredModule()
{
    namespace author=Authoring;
    auto module=author::CreateScript(author::AssetKind::Module,"Structured lookup");
    auto& version=module.versions.front();version.inputs.clear();version.stages={author::Stage::ParticleSpawn,author::Stage::ParticleUpdate,author::Stage::Simulation};
    auto& graph=version.graph;graph.nodes.clear();graph.nextNodeId=1;
    graph.structs={
        {"LookupInput",{{"index",ValueType::Int32,{},IntValue(1)},{"weight",ValueType::Float,{}, {0.25f,0,0,0}}}},
        {"LookupContext",{{"state",ValueType::Float,"LookupInput",{}},{"position",ValueType::Float3,{}, {}}}},
        {"LookupResult",{{"point",ValueType::Float3,{},{}},{"value",ValueType::Float,{},{}},{"enabled",ValueType::Bool,{},BoolValue(false)},{"code",ValueType::Int32,{},IntValue(0)}}}
    };
    graph.dataInterfaces={
        {"User.Points",DataInterfaceKind::Array,ValueType::Float3,{{1,2,3,0},{4,5,6,0},{7,8,9,0}}},
        {"User.WeightCurve",DataInterfaceKind::Curve,ValueType::Float,{{0,2,0,0},{1,6,0,0}}},
        {"User.Codes",DataInterfaceKind::Array,ValueType::Int32,{IntValue(INT32_MIN),IntValue(INT32_MAX),IntValue(-7)}},
        {"User.Enabled",DataInterfaceKind::Array,ValueType::Bool,{BoolValue(false),BoolValue(true),BoolValue(false)}},
        {"User.Empty",DataInterfaceKind::Array,ValueType::Float3,{}}
    };
    const auto node=[&](Operation operation,ValueType type,std::string name,std::vector<NodeId> inputs={})
    {const auto id=AddNode(graph,operation,type);auto* n=FindNode(graph,id);n->name=std::move(name);n->inputs=std::move(inputs);return id;};
    const auto context=node(Operation::Parameter,ValueType::Float,"User.Lookup");FindNode(graph,context)->structType="LookupContext";
    const auto state=node(Operation::StructGet,ValueType::Float,"state",{context});
    const auto index=node(Operation::StructGet,ValueType::Int32,"index",{state});
    const auto weight=node(Operation::StructGet,ValueType::Float,"weight",{state});
    const auto position=node(Operation::StructGet,ValueType::Float3,"position",{context});
    const auto point=node(Operation::DataInterfaceCall,ValueType::Float3,"User.Points.Get",{index});
    const auto sample=node(Operation::DataInterfaceCall,ValueType::Float,"User.WeightCurve.SampleCurve",{weight});
    const auto enabled=node(Operation::DataInterfaceCall,ValueType::Bool,"User.Enabled.Get",{index});
    const auto code=node(Operation::DataInterfaceCall,ValueType::Int32,"User.Codes.Get",{index});
    const auto enabledNumber=node(Operation::Convert,ValueType::Float,{}, {enabled});
    node(Operation::WriteAttribute,ValueType::Float,"Particles.EnabledAsFloat",{enabledNumber});
    const auto bundle=node(Operation::StructMake,ValueType::Float,"LookupResult",{point,sample,enabled,code});
    const auto selected=node(Operation::StructGet,ValueType::Float3,"point",{bundle});
    const auto factor=node(Operation::StructGet,ValueType::Float,"value",{bundle});
    const auto scaled=node(Operation::Multiply,ValueType::Float3,{}, {selected,factor});
    const auto output=node(Operation::Add,ValueType::Float3,{}, {scaled,position});
    node(Operation::WriteAttribute,ValueType::Float3,"Particles.Position",{output});
    node(Operation::WriteAttribute,ValueType::Float,"Particles.Size",{factor});
    const auto velocity=node(Operation::DataInterfaceCall,ValueType::Float3,"User.Points.Sample",{weight});
    node(Operation::WriteAttribute,ValueType::Float3,"Particles.Velocity",{velocity});
    const auto aggregateWrite=node(Operation::WriteAttribute,ValueType::Float,"Particles.LookupResult",{bundle});
    FindNode(graph,aggregateWrite)->structType="LookupResult";
    const auto count=node(Operation::DataInterfaceCall,ValueType::Int32,"User.Points.Length");
    node(Operation::WriteAttribute,ValueType::Int32,"Particles.LookupCount",{count});
    const auto valid=node(Operation::DataInterfaceCall,ValueType::Bool,"User.Points.IsValidIndex",{index});
    node(Operation::WriteAttribute,ValueType::Bool,"Particles.LookupValid",{valid});
    const auto empty=node(Operation::DataInterfaceCall,ValueType::Float3,"User.Empty.Get",{index});
    node(Operation::WriteAttribute,ValueType::Float3,"Particles.EmptyLookup",{empty});
    return module;
}
inline Authoring::System StructuredSystem(const Authoring::ScriptAsset& module,SimulationTarget target)
{
    namespace author=Authoring;auto system=author::CreateSystem("Structured runtime");auto& emitter=system.emitters.front();
    emitter.settings.target=target;emitter.settings.capacity=32;emitter.settings.initialCapacity=4;emitter.settings.spawnRate=0;
    emitter.settings.duration=1000;emitter.settings.looping=false;emitter.settings.bursts={{0,4}};
    for(auto& stack:emitter.stacks)
    {
        stack.modules.clear();
        if(stack.stage==author::Stage::ParticleSpawn || stack.stage==author::Stage::ParticleUpdate)stack.modules.push_back(author::InstantiateModule(module));
        if(stack.stage==author::Stage::EmitterUpdate)
        {
            auto burst=author::CreateModule("SpawnBurst");burst.inputs["Time"].value={0,0,0,0};burst.inputs["Count"].value={4,0,0,0};
            stack.modules.push_back(std::move(burst));
        }
    }
    return system;
}
}
