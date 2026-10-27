#pragma once

#include <Guid.h>
#include <VFX/Authoring/VfxLibrary.h>

namespace NLS::Tests
{
struct AuthoredStageTestAsset
{
    VFX::Authoring::System system;
    VFX::Authoring::ScriptAsset function, module;
    VFX::Authoring::EmitterSource emitter;
};
inline AuthoredStageTestAsset CreateAuthoredStageTestAsset(bool inherited = false)
{
    namespace author = VFX::Authoring;
    using namespace VFX;
    AuthoredStageTestAsset result;
    result.function = author::CreateScript(author::AssetKind::Function,"Iteration Weight");
    auto& function = result.function.versions[0]; function.stages = {author::Stage::Simulation}; function.inputs.clear(); function.graph.nodes.clear();
    const auto index = AddNode(function.graph,Operation::StageIndex), iteration = AddNode(function.graph,Operation::StageIteration);
    const auto one = AddNode(function.graph,Operation::Constant); FindNode(function.graph,one)->value[0] = 1;
    const auto sum = AddNode(function.graph,Operation::Add); FindNode(function.graph,sum)->inputs = {index,iteration};
    const auto weight = AddNode(function.graph,Operation::Add); FindNode(function.graph,weight)->inputs = {sum,one};
    const auto output = AddNode(function.graph,Operation::WriteAttribute); FindNode(function.graph,output)->name = "Output"; FindNode(function.graph,output)->inputs = {weight};
    result.module = author::CreateScript(author::AssetKind::Module,"Stage Offset");
    auto& module = result.module.versions[0]; module.stages = {author::Stage::Simulation}; module.inputs[0].type = ValueType::Float3; module.inputs[0].defaultValue = {0.01f,0,0,0}; module.graph.nodes.clear();
    const auto input = AddNode(module.graph,Operation::Parameter,ValueType::Float3); FindNode(module.graph,input)->name = "Module.Value";
    const auto call = AddNode(module.graph,Operation::Parameter); module.calls.push_back({call,{result.function.id,function.id},"Output",{}});
    const auto offset = AddNode(module.graph,Operation::Multiply,ValueType::Float3); FindNode(module.graph,offset)->inputs = {input,call};
    const auto position = AddNode(module.graph,Operation::Attribute,ValueType::Float3); FindNode(module.graph,position)->name = "Particles.Position";
    const auto moved = AddNode(module.graph,Operation::Add,ValueType::Float3); FindNode(module.graph,moved)->inputs = {position,offset};
    const auto write = AddNode(module.graph,Operation::WriteAttribute,ValueType::Float3); FindNode(module.graph,write)->name = "Particles.Position"; FindNode(module.graph,write)->inputs = {moved};
    result.system = author::CreateSystem("Authored Simulation Stages");
    result.system.parameters.push_back({Guid::New().ToString(),"User.Offset",ValueType::Float3,{0.01f,0,0,0}});
    auto& emitter = result.system.emitters[0]; emitter.settings.target = SimulationTarget::GPU; emitter.settings.capacity = 8; emitter.settings.initialCapacity = 1;
    auto* spawn = author::FindStack(emitter,author::Stage::ParticleSpawn); spawn->modules.resize(1);
    spawn->modules[0].inputs["Position"].value = {-0.5f,0.5f,0,0}; spawn->modules[0].inputs["Velocity"].value = {};
    spawn->modules[0].inputs["Color"].value = {0,1,0,1}; spawn->modules[0].inputs["Size"].value[0] = 0.6f;
    auto* schedule = author::FindStack(emitter,author::Stage::EmitterUpdate); schedule->modules[1].enabled = false;
    auto burst = author::CreateModule("SpawnBurst"); burst.inputs["Count"].value[0] = 1; schedule->modules.push_back(burst);
    auto first = author::CreateSimulationStage("Iterated Offset"); first.iterations = 2;
    auto instance = author::InstantiateModule(result.module); instance.inputs["Value"].mode = author::InputMode::Parameter; instance.inputs["Value"].parameter = "User.Offset";
    first.modules.push_back(instance); emitter.stacks.push_back(first);
    auto disabled = author::CreateSimulationStage("Disabled Stage"); disabled.enabled = false; disabled.modules.push_back(author::CreateModule("UnavailableDisabledModule")); emitter.stacks.push_back(disabled);
    auto second = author::CreateSimulationStage("Final Offset"); instance.id = Guid::New().ToString(); second.modules.push_back(instance); emitter.stacks.push_back(second);
    auto size = author::CreateSimulationStage("Scale Size"); auto scale = author::CreateModule("ScaleSize"); scale.inputs["Scale"].value[0] = 0.5f; size.modules.push_back(scale); emitter.stacks.push_back(size);
    result.emitter = author::CreateEmitterSource("Staged GPU Emitter"); result.emitter.versions[0].emitter = emitter;
    if (inherited) result.system.emitters[0] = author::InstantiateEmitter(result.emitter);
    return result;
}
}
