#pragma once
#include <VFX/VfxGraph.h>

namespace NLS::Tests
{
inline VFX::SystemAsset CreateTypedParticleTestAsset(VFX::SimulationTarget target = VFX::SimulationTarget::GPU)
{
    using namespace VFX;
    auto system = CreateDefaultSystem("Typed Particle Values");
    system.parameters = {{"User.Counter",ValueType::Int32,IntValue(16777217)},{"User.Step",ValueType::Int32,IntValue(3)},
        {"User.Enabled",ValueType::Bool,BoolValue(true)},{"User.Iterations",ValueType::Int32,IntValue(2)}};
    auto& emitter = system.emitters[0]; emitter.target = target; emitter.capacity = 137; emitter.initialCapacity = 3;
    emitter.spawnRate = 0; emitter.bursts = {{0,129}}; emitter.interpolatedSpawning = false;
    emitter.attributes[0].defaultValue = {0,0.5f,0,0}; emitter.attributes[1].defaultValue = {}; emitter.attributes[3].defaultValue[0] = 0.3f;
    emitter.attributes.push_back({"Particles.Counter",ValueType::Int32,IntValue(0)});
    emitter.attributes.push_back({"Particles.Flag",ValueType::Bool,BoolValue(false)});
    emitter.attributes.push_back({"Particles.Edge",ValueType::Int32,IntValue(INT32_MIN)});
    emitter.spawn.nodes.clear(); emitter.update.nodes.clear();
    const auto parameter = [&](Graph& g,const char* name,ValueType type,Value value)
    { const auto id = AddNode(g,Operation::Parameter,type); auto* node = FindNode(g,id); node->name = name; node->value = value; return id; };
    const auto write = [&](Graph& g,const char* name,ValueType type,NodeId value)
    { const auto id = AddNode(g,Operation::WriteAttribute,type); FindNode(g,id)->name = name; FindNode(g,id)->inputs = {value}; };
    write(emitter.spawn,"Particles.Counter",ValueType::Int32,parameter(emitter.spawn,"User.Counter",ValueType::Int32,IntValue(16777217)));
    write(emitter.spawn,"Particles.Flag",ValueType::Bool,parameter(emitter.spawn,"User.Enabled",ValueType::Bool,BoolValue(true)));
    const auto read = AddNode(emitter.update,Operation::Attribute,ValueType::Int32); FindNode(emitter.update,read)->name = "Particles.Counter";
    const auto step = parameter(emitter.update,"User.Step",ValueType::Int32,IntValue(3));
    const auto sum = AddNode(emitter.update,Operation::Add,ValueType::Int32); FindNode(emitter.update,sum)->inputs = {read,step}; write(emitter.update,"Particles.Counter",ValueType::Int32,sum);
    const auto enabled = parameter(emitter.update,"User.Enabled",ValueType::Bool,BoolValue(true));
    write(emitter.update,"Particles.Flag",ValueType::Bool,enabled);
    if (target == SimulationTarget::GPU)
    {
        auto stage = CreateSimulationStage("Typed iteration"); stage.iterations = 2; stage.bindings = {"User.Enabled","User.Iterations",true,true};
        const auto counter = AddNode(stage.graph,Operation::Attribute,ValueType::Int32); FindNode(stage.graph,counter)->name = "Particles.Counter";
        const auto iteration = AddNode(stage.graph,Operation::StageIteration,ValueType::Int32);
        const auto one = AddNode(stage.graph,Operation::Constant,ValueType::Int32); FindNode(stage.graph,one)->value = IntValue(1);
        const auto increment = AddNode(stage.graph,Operation::Add,ValueType::Int32); FindNode(stage.graph,increment)->inputs = {iteration,one};
        const auto next = AddNode(stage.graph,Operation::Add,ValueType::Int32); FindNode(stage.graph,next)->inputs = {counter,increment}; write(stage.graph,"Particles.Counter",ValueType::Int32,next);
        emitter.simulationStages.push_back(stage);
    }
    return system;
}
}
