#pragma once

#include <VFX/VfxGraph.h>

namespace NLS::Tests
{
inline VFX::SystemAsset CreateStagedParticleTestAsset()
{
    using namespace VFX;
    auto system = CreateDefaultSystem("Growing staged particles"); auto& emitter = system.emitters[0];
    emitter.target = SimulationTarget::GPU; emitter.capacity = 257; emitter.initialCapacity = 3;
    emitter.spawnRate = 600; emitter.duration = 1; emitter.looping = false; emitter.interpolatedSpawning = false;
    emitter.bursts = {{0,2},{0.04,11},{0.08,71}}; emitter.spawn.nodes.clear(); emitter.update.nodes.clear();
    emitter.attributes[0].defaultValue = {0.05f,0.1f,0.5f,0}; emitter.attributes[1].defaultValue = {};
    emitter.attributes.back().defaultValue = {10,0,0,0};
    const auto node = [](Graph& graph, Operation operation, ValueType type, Value value = {}, std::string name = {}, std::vector<NodeId> inputs = {})
    {
        const auto id = AddNode(graph,operation,type); auto* entry = FindNode(graph,id);
        entry->value = value; entry->name = std::move(name); entry->inputs = std::move(inputs); return id;
    };
    auto stage = CreateSimulationStage("Translate"); stage.iterations = 2;
    auto& translate = stage.graph;
    const auto position = node(translate,Operation::Attribute,ValueType::Float3,{},"Particles.Position");
    const auto offset = node(translate,Operation::Parameter,ValueType::Float3,{0.2f,0.4f,0,0},"User.StageOffset");
    const auto delta = node(translate,Operation::DeltaTime,ValueType::Float);
    const auto displacement = node(translate,Operation::Multiply,ValueType::Float3,{},"",{offset,delta});
    const auto add = node(translate,Operation::Add,ValueType::Float3,{},"",{position,displacement});
    node(translate,Operation::WriteAttribute,ValueType::Float3,{},"Particles.Position",{add});
    emitter.simulationStages.push_back(stage);
    stage = CreateSimulationStage("Contract"); auto& contract = stage.graph;
    const auto current = node(contract,Operation::Attribute,ValueType::Float3,{},"Particles.Position");
    const auto factor = node(contract,Operation::Constant,ValueType::Float,{0.9f,0,0,0});
    const auto product = node(contract,Operation::Multiply,ValueType::Float3,{},"",{current,factor});
    node(contract,Operation::WriteAttribute,ValueType::Float3,{},"Particles.Position",{product});
    emitter.simulationStages.push_back(stage);
    stage = CreateSimulationStage("Disabled"); stage.enabled = false;
    const auto disabledValue = node(stage.graph,Operation::Constant,ValueType::Float3,{9999,9999,9999,0});
    node(stage.graph,Operation::WriteAttribute,ValueType::Float3,{},"Particles.Position",{disabledValue});
    emitter.simulationStages.push_back(stage);
    stage = CreateSimulationStage("Iteration context"); stage.iterations = 3; auto& context = stage.graph;
    const auto stageIndex = node(context,Operation::StageIndex,ValueType::Float), iteration = node(context,Operation::StageIteration,ValueType::Float);
    const auto indexScale = node(context,Operation::Constant,ValueType::Float,{0.1f,0,0,0}), iterationScale = node(context,Operation::Constant,ValueType::Float,{0.01f,0,0,0});
    const auto scaledIndex = node(context,Operation::Multiply,ValueType::Float,{},"",{stageIndex,indexScale});
    const auto scaledIteration = node(context,Operation::Multiply,ValueType::Float,{},"",{iteration,iterationScale});
    const auto size = node(context,Operation::Add,ValueType::Float,{},"",{scaledIndex,scaledIteration});
    node(context,Operation::WriteAttribute,ValueType::Float,{},"Particles.Size",{size});
    const auto random = node(context,Operation::Random,ValueType::Float4,{0.2f,0.2f,0.2f,0});
    const auto base = node(context,Operation::Constant,ValueType::Float4,{0.5f,0.5f,0.5f,1});
    const auto color = node(context,Operation::Add,ValueType::Float4,{},"",{random,base});
    node(context,Operation::WriteAttribute,ValueType::Float4,{},"Particles.Color",{color});
    emitter.simulationStages.push_back(stage);
    stage = CreateSimulationStage("Lifetime");
    const auto lifetime = node(stage.graph,Operation::Parameter,ValueType::Float,{10,0,0,0},"User.StageLifetime");
    node(stage.graph,Operation::WriteAttribute,ValueType::Float,{},"Particles.Lifetime",{lifetime});
    emitter.simulationStages.push_back(stage);
    return system;
}
}
