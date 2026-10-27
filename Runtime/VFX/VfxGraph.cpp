#include "VfxGraph.h"
#include "VfxGraphSchemaSerialization.h"
#include "VfxValueSerialization.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_set>

#include <Guid.h>
#include <Json/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace NLS::VFX
{
namespace
{
using Json = nlohmann::json;
bool ValidGuid(const std::string& text)
{
    const auto parsed = Guid::TryParse(text);
    return parsed && parsed->IsValid();
}
constexpr const char* operationNames[] = {
    "Constant", "Attribute", "Parameter", "Delta Time", "Normalized Age", "Random",
    "Add", "Subtract", "Multiply", "Divide", "Minimum", "Maximum", "Clamp", "Lerp",
    "Sine", "Cosine", "Absolute", "Square Root", "Normalize", "Length", "Dot", "Cross", "Write Attribute", "Stage Index", "Stage Iteration",
    "Equal", "Not Equal", "Less Than", "Less or Equal", "Greater Than", "Greater or Equal", "And", "Or", "Not", "Select", "Convert"
    , "Spawn Index", "Make Struct", "Get Struct Field", "Data Interface Call"
};

Json EncodeGraph(const Graph& graph, bool editor)
{
    Json result = {{"id", graph.id}, {"stage", static_cast<uint32_t>(graph.stage)},
                   {"nextNodeId", graph.nextNodeId}, {"nodes", Json::array()}};
    Schema::EncodeGraphDeclarations(result,graph);
    for (const auto& node : graph.nodes)
    {
        Json entry = {{"id", node.id}, {"operation", static_cast<uint32_t>(node.operation)},
                      {"type", static_cast<uint32_t>(node.type)}, {"value", Detail::EncodeValue(node.value,node.type)},
                      {"name", node.name}, {"inputs", node.inputs}};
        if (!node.structType.empty()) entry["structType"] = node.structType;
        if (editor) entry["position"] = node.position;
        result["nodes"].push_back(std::move(entry));
    }
    return result;
}

Graph DecodeGraph(const Json& source, ScriptStage expected)
{
    Graph graph;
    Schema::DecodeGraphDeclarations(source,graph);
    graph.id = source.at("id").get<std::string>();
    if (!ValidGuid(graph.id)) throw std::runtime_error("Invalid graph GUID");
    if (source.at("stage").get<uint32_t>() != static_cast<uint32_t>(expected))
        throw std::runtime_error("Graph stage does not match its emitter slot");
    graph.stage = expected;
    graph.nextNodeId = source.value("nextNodeId", NodeId{1});
    std::unordered_set<NodeId> ids;
    if (!source.at("nodes").is_array() || source.at("nodes").size() > 65536) throw std::runtime_error("Invalid graph node list");
    for (const auto& entry : source.at("nodes"))
    {
        Node node;
        node.id = entry.at("id").get<NodeId>();
        const auto operation = entry.at("operation").get<uint32_t>();
        const auto type = entry.at("type").get<uint32_t>();
        if (!node.id || node.id > ((std::numeric_limits<NodeId>::max)() >> 3) || !ids.insert(node.id).second)
            throw std::runtime_error("Graph contains an invalid or duplicate node ID");
        if (operation > static_cast<uint32_t>(Operation::DataInterfaceCall) || type > static_cast<uint32_t>(ValueType::Bool))
            throw std::runtime_error("Graph contains an unknown operation or value type");
        node.operation = static_cast<Operation>(operation);
        node.type = static_cast<ValueType>(type);
        node.value = Detail::DecodeValue(entry.at("value"),node.type);
        node.name = entry.value("name", std::string{});
        node.inputs = entry.at("inputs").get<std::vector<NodeId>>();
        node.structType = entry.value("structType",std::string{});
        if (!ValidInputCount(node.operation,node.inputs.size()))
            throw std::runtime_error("Node input count does not match its operation");
        if (entry.contains("position")) node.position = entry.at("position").get<std::array<float, 2>>();
        if (!std::isfinite(node.position[0]) || !std::isfinite(node.position[1])) throw std::runtime_error("Node positions must be finite");
        graph.nextNodeId = (std::max)(graph.nextNodeId, node.id + 1);
        graph.nodes.push_back(std::move(node));
    }
    if (!graph.nextNodeId || graph.nextNodeId >= ((std::numeric_limits<NodeId>::max)() >> 3))
        throw std::runtime_error("Invalid next node ID");
    return graph;
}
}

uint32_t ComponentCount(ValueType type) { return IsIntegralType(type) ? 1 : IsFloatType(type) ? static_cast<uint32_t>(type)+1 : 0; }
const char* OperationName(Operation operation)
{
    const auto index = static_cast<size_t>(operation);
    return index < std::size(operationNames) ? operationNames[index] : "Unknown";
}

uint32_t InputCount(Operation operation)
{
    switch (operation)
    {
    case Operation::Add: case Operation::Subtract: case Operation::Multiply: case Operation::Divide:
    case Operation::Minimum: case Operation::Maximum: case Operation::Dot: case Operation::Cross:
    case Operation::Equal: case Operation::NotEqual: case Operation::LessThan: case Operation::LessEqual:
    case Operation::GreaterThan: case Operation::GreaterEqual: case Operation::LogicalAnd: case Operation::LogicalOr: return 2;
    case Operation::Clamp: case Operation::Lerp: case Operation::Select: return 3;
    case Operation::Sine: case Operation::Cosine: case Operation::Absolute: case Operation::SquareRoot:
    case Operation::StructGet: case Operation::DataInterfaceCall:
    case Operation::Normalize: case Operation::Length: case Operation::WriteAttribute: case Operation::LogicalNot: case Operation::Convert: return 1;
    // SpawnIndex, StageIndex, StageIteration and the literal/context opcodes
    // all take zero operands.
    default: return 0;
    }
}

Node* FindNode(Graph& graph, NodeId id)
{
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const Node& node) { return node.id == id; });
    return found == graph.nodes.end() ? nullptr : &*found;
}
const Node* FindNode(const Graph& graph, NodeId id)
{
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const Node& node) { return node.id == id; });
    return found == graph.nodes.end() ? nullptr : &*found;
}

NodeId AddNode(Graph& graph, Operation operation, ValueType type)
{
    if (graph.nextNodeId == 0 || graph.nextNodeId >= ((std::numeric_limits<NodeId>::max)() >> 3)) return 0;
    Node node;
    node.id = graph.nextNodeId++;
    node.operation = operation;
    node.type = type;
    if ((operation >= Operation::Equal && operation <= Operation::LogicalNot)) node.type = ValueType::Bool;
    node.inputs.resize(InputCount(operation));
    graph.nodes.push_back(node);
    return node.id;
}

bool CanConnect(const Graph& graph, NodeId source, NodeId destination, uint32_t input)
{
    const auto* from = FindNode(graph, source);
    const auto* to = FindNode(graph, destination);
    if (!from || !to || source == destination || from->operation == Operation::WriteAttribute || input >= to->inputs.size()) return false;
    std::unordered_set<NodeId> visited;
    std::vector<NodeId> pending{source};
    while (!pending.empty())
    {
        const auto id = pending.back(); pending.pop_back();
        if (id == destination) return false;
        if (!visited.insert(id).second) continue;
        if (const auto* node = FindNode(graph, id))
            pending.insert(pending.end(), node->inputs.begin(), node->inputs.end());
    }
    return true;
}

SystemAsset CreateDefaultSystem(std::string name)
{
    SystemAsset system;
    system.id = Guid::New().ToString();
    system.name = std::move(name);
    system.systemAttributes = {
        {"System.Age", ValueType::Float, {}},
        {"System.FrameIndex", ValueType::Float, {}},
        {"System.SpawnCount", ValueType::Float, {}}
    };
    system.systemSpawn.id = Guid::New().ToString();
    system.systemSpawn.stage = ScriptStage::SystemSpawn;
    system.systemUpdate.id = Guid::New().ToString();
    system.systemUpdate.stage = ScriptStage::SystemUpdate;
    EmitterAsset emitter;
    emitter.id = Guid::New().ToString();
    emitter.attributes = {
        {"Particles.Position", ValueType::Float3, {}},
        {"Particles.Velocity", ValueType::Float3, {0, 3, 0, 0}},
        {"Particles.Color", ValueType::Float4, {1, 0.5f, 0.1f, 1}},
        {"Particles.Size", ValueType::Float, {0.05f, 0, 0, 0}},
        {"Particles.Age", ValueType::Float, {}},
        {"Particles.Lifetime", ValueType::Float, {2, 0, 0, 0}}
    };
    emitter.spawn.id = Guid::New().ToString();
    emitter.spawn.stage = ScriptStage::ParticleSpawn;
    emitter.emitterSpawn.id = Guid::New().ToString();
    emitter.emitterSpawn.stage = ScriptStage::EmitterSpawn;
    emitter.emitterUpdate.id = Guid::New().ToString();
    emitter.emitterUpdate.stage = ScriptStage::EmitterUpdate;
    emitter.emitterCollision.id = Guid::New().ToString();
    emitter.emitterCollision.stage = ScriptStage::ParticleUpdate;
    emitter.emitterAttributes = {
        {"Emitter.SpawnRate", ValueType::Float, {}},
        {"Emitter.SpawnPerFrame", ValueType::Float, {}},
        {"Emitter.SpawnPerUnit", ValueType::Float, {}},
        {"Emitter.Duration", ValueType::Float, {5, 0, 0, 0}},
        {"Emitter.Loop", ValueType::Float, {1, 0, 0, 0}},
        {"Emitter.UnusedGracePeriod", ValueType::Float, {}},
        {"Emitter.BurstPhase", ValueType::Float, {}},
        {"Emitter.SpawnCount", ValueType::Float, {}},
        {"Emitter.GenerateSpawn", ValueType::Float, {}},
        {"Emitter.GenerateDeath", ValueType::Float, {}},
        {"Emitter.GenerateCollision", ValueType::Float, {}},
        {"Emitter.ReceiveKind", ValueType::Float, {}},
        {"Emitter.ReceiveSpawnCount", ValueType::Float, {1, 0, 0, 0}},
        {"Emitter.ReceiveRandomAge", ValueType::Float, {}},
        {"Emitter.CollisionEnabled", ValueType::Float, {}},
        {"Emitter.CollisionRadius", ValueType::Float, {0.05f, 0, 0, 0}},
        {"Emitter.CollisionRestitution", ValueType::Float, {0.5f, 0, 0, 0}},
        {"Emitter.CollisionFriction", ValueType::Float, {}}};
    auto id = AddNode(emitter.spawn, Operation::Random, ValueType::Float3);
    FindNode(emitter.spawn, id)->value = {0.5f, 0.5f, 0.5f, 0};
    FindNode(emitter.spawn, id)->position = {0, 0};
    auto write = AddNode(emitter.spawn, Operation::WriteAttribute, ValueType::Float3);
    FindNode(emitter.spawn, write)->name = "Particles.Position";
    FindNode(emitter.spawn, write)->inputs = {id};
    FindNode(emitter.spawn, write)->position = {280, 0};

    auto& graph = emitter.update;
    graph.id = Guid::New().ToString();
    graph.stage = ScriptStage::ParticleUpdate;
    const auto position = AddNode(graph, Operation::Attribute, ValueType::Float3);
    FindNode(graph, position)->name = "Particles.Position";
    FindNode(graph, position)->position = {0, 0};
    const auto velocity = AddNode(graph, Operation::Attribute, ValueType::Float3);
    FindNode(graph, velocity)->name = "Particles.Velocity";
    FindNode(graph, velocity)->position = {0, 130};
    const auto delta = AddNode(graph, Operation::DeltaTime);
    FindNode(graph, delta)->position = {0, 250};
    const auto displacement = AddNode(graph, Operation::Multiply, ValueType::Float3);
    FindNode(graph, displacement)->inputs = {velocity, delta};
    FindNode(graph, displacement)->position = {240, 130};
    const auto sum = AddNode(graph, Operation::Add, ValueType::Float3);
    FindNode(graph, sum)->inputs = {position, displacement};
    FindNode(graph, sum)->position = {480, 0};
    write = AddNode(graph, Operation::WriteAttribute, ValueType::Float3);
    FindNode(graph, write)->name = "Particles.Position";
    FindNode(graph, write)->inputs = {sum};
    FindNode(graph, write)->position = {720, 0};
    system.emitters.push_back(std::move(emitter));
    return system;
}

SimulationStage CreateSimulationStage(std::string name)
{
    SimulationStage stage; stage.id = Guid::New().ToString(); stage.name = std::move(name);
    stage.graph.id = Guid::New().ToString(); stage.graph.stage = ScriptStage::SimulationStage; return stage;
}

bool ValidateSimulationStageBindings(const SimulationStageBindings& bindings,std::string& error)
{
    const auto valid = [](const std::string& name)
    { return name.empty() || (name.starts_with("User.") && name.size() > 5 && name.size() <= 1024 && name.find('\0') == std::string::npos); };
    if (!valid(bindings.enabled) || !valid(bindings.iterations))
    { error = "Simulation Stage controls require User parameters"; return false; }
    error.clear(); return true;
}
bool ResolveSimulationStageControl(bool enabled,uint32_t iterations,const SimulationStageBindings& bindings,
    const std::unordered_map<std::string,Value>& parameters,SimulationStageExecution& execution,std::string& error)
{
    if (iterations > 128) { error = "Simulation Stage supports at most 128 iterations"; return false; }
    if (!ValidateSimulationStageBindings(bindings,error)) return false;
    SimulationStageExecution candidate{enabled,iterations};
    if (!bindings.enabled.empty())
    {
            float value = bindings.enabledDefault ? 1.0f : 0.0f;
        if (const auto parameter = parameters.find(bindings.enabled); parameter != parameters.end())
        {
            if (bindings.typed)
            { if (!ValidValue(parameter->second,ValueType::Bool)) { error = "Enabled Binding requires Bool"; return false; } value = ReadBool(parameter->second) ? 1.0f : 0.0f; }
            else value = parameter->second[0];
        }
        if (!std::isfinite(value) || (value != 0 && value != 1))
        { error = "Simulation Stage Enabled Binding requires 0 or 1: "+bindings.enabled; return false; }
        candidate.enabled = enabled && value != 0;
    }
    if (!bindings.iterations.empty())
        if (const auto parameter = parameters.find(bindings.iterations); parameter != parameters.end())
        {
            const auto value = bindings.typed ? static_cast<double>(ReadInt(parameter->second)) : static_cast<double>(parameter->second[0]);
            if (!std::isfinite(value) || std::floor(value) != value)
            { error = "Simulation Stage Num Iterations requires an integer: "+bindings.iterations; return false; }
            if (value > 128) { error = "Simulation Stage Num Iterations exceeds 128: "+bindings.iterations; return false; }
            candidate.iterations = value <= 0 ? 0u : static_cast<uint32_t>(value);
        }
    execution = candidate; error.clear(); return true;
}
bool ResolveSimulationStageControls(std::span<const SimulationStage> stages,const std::unordered_map<std::string,Value>& parameters,
    std::vector<SimulationStageExecution>& execution,std::string& error)
{
    if (stages.size() > 64) { error = "An emitter supports at most 64 Simulation Stages"; return false; }
    std::vector<SimulationStageExecution> candidate; candidate.reserve(stages.size()); uint32_t total = 0;
    for (const auto& stage : stages)
    {
        SimulationStageExecution value;
        if (!ResolveSimulationStageControl(stage.enabled,stage.iterations,stage.bindings,parameters,value,error)) return false;
        if (value.enabled && (total += value.iterations) > 1024) { error = "Simulation Stages exceed 1024 total iterations"; return false; }
        candidate.push_back(value);
    }
    execution = std::move(candidate); error.clear(); return true;
}
bool ValidateRibbonSettings(const RibbonSettings& settings, std::string& error)
{
    const auto validReference = [](const AssetReference& reference) { return reference.Empty() || (ValidGuid(reference.guid) && reference.localId != 0); };
    if (!validReference(settings.material) || !validReference(settings.texture) ||
        static_cast<uint32_t>(settings.blend) > static_cast<uint32_t>(SpriteBlendMode::Premultiplied) ||
        !std::all_of(settings.tint.begin(),settings.tint.end(),[](float value) { return std::isfinite(value); }) ||
        !std::isfinite(settings.emissive) || settings.emissive < 0 ||
        !std::isfinite(settings.uvTilingDistance) || settings.uvTilingDistance <= 0 || settings.uvTilingDistance > 1e6 ||
        settings.widthAttribute.size() > 256)
    { error = "Invalid Ribbon material, blend, tint, emissive or UV tiling settings"; return false; }
    if (settings.useMaterialShader && settings.material.Empty())
    { error = "Ribbon material shader requires a material reference"; return false; }
    return true;
}

SpriteSettings ResolveRibbonDrawSettings(const RibbonSettings& settings)
{
    SpriteSettings resolved;
    resolved.material = settings.material;
    resolved.texture = settings.texture;
    resolved.blend = settings.blend;
    resolved.tint = settings.tint;
    resolved.emissive = settings.emissive;
    // A ribbon strip is always camera facing and has no SubUV or depth fade.
    resolved.alignment = SpriteAlignment::Camera;
    resolved.alphaCutoff = 0.333f;
    resolved.softDistance = 0;
    resolved.subImagesX = 1; resolved.subImagesY = 1;
    resolved.circularMask = false;
    resolved.useMaterialShader = settings.useMaterialShader;
    return resolved;
}

bool ValidateSpriteSettings(const SpriteSettings& settings, std::string& error)
{
    const auto validReference = [](const AssetReference& reference) { return reference.Empty() || (ValidGuid(reference.guid) && reference.localId != 0); };
    if (!validReference(settings.material) || !validReference(settings.texture) ||
        static_cast<uint32_t>(settings.blend) > static_cast<uint32_t>(SpriteBlendMode::Premultiplied) ||
        static_cast<uint32_t>(settings.alignment) > static_cast<uint32_t>(SpriteAlignment::Velocity) ||
        !std::all_of(settings.tint.begin(),settings.tint.end(),[](float value) { return std::isfinite(value); }) ||
        !std::isfinite(settings.emissive) || settings.emissive < 0 || !std::isfinite(settings.alphaCutoff) || settings.alphaCutoff < 0 || settings.alphaCutoff > 1 ||
        !std::isfinite(settings.softDistance) || settings.softDistance < 0 || !settings.subImagesX || !settings.subImagesY ||
        settings.subImagesX > 256 || settings.subImagesY > 256)
    { error = "Invalid Sprite material, blend mode, alignment, SubUV or depth fade settings"; return false; }
    if ((settings.blend == SpriteBlendMode::Opaque || settings.blend == SpriteBlendMode::Masked) && settings.softDistance != 0)
    { error = "Soft particles require a translucent, additive or premultiplied material"; return false; }
    if ((settings.useMaterialShader && settings.material.Empty()) || settings.materialParameters.size() > 64 ||
        (!settings.useMaterialShader && !settings.materialParameters.empty()))
    { error = "Material parameter bindings require a ParticleSprite material; maximum 64 bindings"; return false; }
    const auto finite = [](const Value& value) { return std::all_of(value.begin(),value.end(),[](float v) { return std::isfinite(v); }); };
    for (const auto& binding : settings.dynamicParameters)
        if (binding.attribute.size() > 256 || binding.fallbackParameter.size() > 256 || !finite(binding.defaultValue))
        { error = "Invalid dynamic particle material binding"; return false; }
    std::unordered_set<std::string> names;
    for (const auto& binding : settings.materialParameters)
        if (binding.materialParameter.empty() || binding.materialParameter.size() > 256 || binding.systemParameter.empty() || binding.systemParameter.size() > 256 ||
            static_cast<uint32_t>(binding.type) > 3 || !finite(binding.defaultValue) || !names.insert(binding.materialParameter).second)
        { error = "Invalid or duplicate material parameter binding"; return false; }
    error.clear(); return true;
}

bool ValidateMeshSettings(const MeshSettings& settings, std::string& error)
{
    const auto finite = [](const Value& value) { return std::all_of(value.begin(),value.end(),[](float v) { return std::isfinite(v); }); };
    const auto validReference = [](const AssetReference& reference) { return reference.Empty() || (ValidGuid(reference.guid) && reference.localId != 0); };
    if (settings.meshes.size() > kMaxMeshSlots)
    { error = "A mesh renderer supports at most 8 mesh slots"; return false; }
    if (settings.materials.size() > settings.meshes.size())
    { error = "Mesh material overrides cannot exceed the mesh slot count"; return false; }
    for (const auto& reference : settings.meshes)
        if (!validReference(reference)) { error = "Invalid mesh slot asset reference"; return false; }
    for (const auto& reference : settings.materials)
        if (!validReference(reference)) { error = "Invalid mesh material override reference"; return false; }
    if (settings.scaleAttribute.size() > 256 || settings.orientationAttribute.size() > 256 || settings.meshIndexAttribute.size() > 256 ||
        settings.lodIndex < -1 || settings.lodIndex > 255 || !finite(settings.defaultScale) || !finite(settings.defaultOrientation))
    { error = "Invalid Mesh slot, LOD, scale or quaternion binding"; return false; }
    error.clear(); return true;
}

SpriteSettings ResolveMeshSlotMaterialSettings(const SpriteSettings& sprite, const MeshSettings& mesh, const std::size_t slot)
{
    if (slot >= mesh.materials.size() || mesh.materials[slot].Empty()) return sprite;
    auto result = sprite; result.material = mesh.materials[slot]; return result;
}

std::string SerializeSystem(const SystemAsset& asset, bool editor)
{
    bool typed = !asset.parameters.empty();
    for (const auto& a : asset.systemAttributes) typed |= IsIntegralType(a.type);
    for (const auto* graph : {&asset.systemSpawn,&asset.systemUpdate})
        for (const auto& n : graph->nodes) typed |= IsIntegralType(n.type) || n.operation > Operation::StageIteration;
    for (const auto& emitter : asset.emitters)
    {
        for (const auto& a : emitter.attributes) typed |= IsIntegralType(a.type);
        for (const auto* graph : {&emitter.spawn,&emitter.update}) for (const auto& n : graph->nodes) typed |= IsIntegralType(n.type) || n.operation > Operation::StageIteration;
        for (const auto& stage : emitter.simulationStages) { typed |= stage.bindings.typed; for (const auto& n : stage.graph.nodes) typed |= IsIntegralType(n.type) || n.operation > Operation::StageIteration; }
    }
    const bool timing = asset.timing != SystemTimingSettings{};
    Json json = {{"version",timing ? 3u : (std::max)(asset.version,typed ? 2u : 1u)}, {"id", asset.id}, {"name", asset.name}, {"emitters", Json::array()}};
    if (timing) json["timing"] = {{"fixedTick",asset.timing.fixedTick},{"fixedTickDelta",asset.timing.fixedTickDelta},
        {"warmupTickCount",asset.timing.warmupTickCount},{"warmupTickDelta",asset.timing.warmupTickDelta}};
    if (asset.completion.enabled) json["completion"] = {{"enabled",asset.completion.enabled},{"unusedGracePeriod",asset.completion.unusedGracePeriod}};
    if (!asset.parameters.empty())
    {
        json["parameters"] = Json::array();
        for (const auto& p : asset.parameters) json["parameters"].push_back({{"name",p.name},{"type",static_cast<uint32_t>(p.type)},{"value",Detail::EncodeValue(p.value,p.type)}});
    }
    json["systemAttributes"] = Json::array();
    for (const auto& attribute : asset.systemAttributes)
        json["systemAttributes"].push_back({{"name",attribute.name},{"type",static_cast<uint32_t>(attribute.type)},
            {"default",Detail::EncodeValue(attribute.defaultValue,attribute.type)}});
    if (ValidGuid(asset.systemSpawn.id)) json["systemSpawn"] = EncodeGraph(asset.systemSpawn,editor);
    if (ValidGuid(asset.systemUpdate.id)) json["systemUpdate"] = EncodeGraph(asset.systemUpdate,editor);
    for (const auto& emitter : asset.emitters)
    {
        Json attributes = Json::array(), bursts = Json::array(), stages = Json::array(), emitterAttributes = Json::array();
        for (const auto& stage : emitter.simulationStages)
        {
            Json value{{"id",stage.id},{"name",stage.name},{"enabled",stage.enabled},{"iterations",stage.iterations},{"graph",EncodeGraph(stage.graph,editor)}};
            if (!stage.bindings.enabled.empty() || !stage.bindings.iterations.empty())
                value["bindings"] = {{"enabled",stage.bindings.enabled},{"iterations",stage.bindings.iterations},{"enabledDefault",stage.bindings.enabledDefault},{"typed",stage.bindings.typed}};
            stages.push_back(std::move(value));
        }
        for (const auto& attribute : emitter.attributes)
            attributes.push_back({{"name", attribute.name}, {"type", static_cast<uint32_t>(attribute.type)}, {"default", Detail::EncodeValue(attribute.defaultValue,attribute.type)}});
        for (const auto& attribute : emitter.emitterAttributes)
            emitterAttributes.push_back({{"name", attribute.name}, {"type", static_cast<uint32_t>(attribute.type)}, {"default", Detail::EncodeValue(attribute.defaultValue,attribute.type)}});
        for (const auto& burst : emitter.bursts) bursts.push_back({{"time", burst.time}, {"count", burst.count}});
        Json dynamic = Json::array(), materialParameters = Json::array();
        for (const auto& binding : emitter.sprite.dynamicParameters)
            dynamic.push_back({{"attribute",binding.attribute},{"fallbackParameter",binding.fallbackParameter},{"default",binding.defaultValue}});
        for (const auto& binding : emitter.sprite.materialParameters)
            materialParameters.push_back({{"materialParameter",binding.materialParameter},{"systemParameter",binding.systemParameter},
                {"type",static_cast<uint32_t>(binding.type)},{"default",binding.defaultValue}});
        Json meshSlots = Json::array(), meshMaterials = Json::array(), eventHandlers = Json::array();
        for (const auto& reference : emitter.mesh.meshes)
            meshSlots.push_back({{"guid",reference.guid},{"localId",reference.localId}});
        for (const auto& reference : emitter.mesh.materials)
            meshMaterials.push_back({{"guid",reference.guid},{"localId",reference.localId}});
        for (const auto& handler : emitter.eventHandlers)
            eventHandlers.push_back({{"id",handler.id},{"name",handler.name},
                {"receiver",{{"sourceEmitter",handler.receiver.sourceEmitter},
                    {"eventKind",static_cast<uint32_t>(handler.receiver.eventKind)},
                    {"spawnCount",handler.receiver.spawnCount},
                    {"randomAge",handler.receiver.randomAge},
                    {"maxEventsPerFrame",handler.receiver.maxEventsPerFrame},
                    {"executionMode",static_cast<uint32_t>(handler.receiver.executionMode)}}},
                {"graph",EncodeGraph(handler.graph,editor)}});
        json["emitters"].push_back({{"id", emitter.id}, {"name", emitter.name},
            {"target", static_cast<uint32_t>(emitter.target)}, {"capacity", emitter.capacity}, {"initialCapacity",emitter.initialCapacity}, {"seed", emitter.seed},
            {"spawnRate", emitter.spawnRate}, {"duration", emitter.duration}, {"looping", emitter.looping},
            {"interpolatedSpawning", emitter.interpolatedSpawning},
            {"sortByViewDepth", emitter.sortByViewDepth},
            {"frustumCulling", emitter.frustumCulling},
            {"collision", {{"enabled",emitter.collisionEnabled},{"radius",emitter.collisionRadius},
                {"restitution",emitter.restitution},{"friction",emitter.friction},{"layers",emitter.collisionLayers}}},
            {"bursts", bursts}, {"attributes", attributes}, {"emitterAttributes", emitterAttributes}, {"spawnPerFrame", emitter.spawnPerFrame}, {"spawnPerUnit", emitter.spawnPerUnit},
            {"eventHandlers",std::move(eventHandlers)},
            {"eventGenerationMask",emitter.eventGenerationMask},
            {"hasEventGenerationPolicy",emitter.hasEventGenerationPolicy},
            {"simulationStages",stages},
            {"renderer",static_cast<uint32_t>(emitter.renderer)},
            {"ribbon",{{"widthAttribute",emitter.ribbon.widthAttribute},
                {"material",{{"guid",emitter.ribbon.material.guid},{"localId",emitter.ribbon.material.localId}}},
                {"texture",{{"guid",emitter.ribbon.texture.guid},{"localId",emitter.ribbon.texture.localId}}},
                {"blend",static_cast<uint32_t>(emitter.ribbon.blend)},{"tint",emitter.ribbon.tint},
                {"emissive",emitter.ribbon.emissive},{"uvTilingDistance",emitter.ribbon.uvTilingDistance},
                {"tileUvOverDistance",emitter.ribbon.tileUvOverDistance},
                {"useMaterialShader",emitter.ribbon.useMaterialShader}}},
            {"mesh",{{"meshes",std::move(meshSlots)},{"materials",std::move(meshMaterials)},
                {"meshIndexAttribute",emitter.mesh.meshIndexAttribute},{"lodIndex",emitter.mesh.lodIndex},
                {"scaleAttribute",emitter.mesh.scaleAttribute},{"orientationAttribute",emitter.mesh.orientationAttribute},
                {"defaultScale",emitter.mesh.defaultScale},{"defaultOrientation",emitter.mesh.defaultOrientation}}},
            {"sprite",{{"material",{{"guid",emitter.sprite.material.guid},{"localId",emitter.sprite.material.localId}}},
                {"texture",{{"guid",emitter.sprite.texture.guid},{"localId",emitter.sprite.texture.localId}}},
                {"blend",static_cast<uint32_t>(emitter.sprite.blend)},{"alignment",static_cast<uint32_t>(emitter.sprite.alignment)},
                {"tint",emitter.sprite.tint},{"emissive",emitter.sprite.emissive},{"alphaCutoff",emitter.sprite.alphaCutoff},{"softDistance",emitter.sprite.softDistance},
                {"subImagesX",emitter.sprite.subImagesX},{"subImagesY",emitter.sprite.subImagesY},{"blendSubImages",emitter.sprite.blendSubImages},{"circularMask",emitter.sprite.circularMask},
                {"useMaterialShader",emitter.sprite.useMaterialShader},{"dynamicParameters",std::move(dynamic)},{"materialParameters",std::move(materialParameters)}}},
            {"spawn", EncodeGraph(emitter.spawn, editor)}, {"update", EncodeGraph(emitter.update, editor)},
            {"emitterSpawn", EncodeGraph(emitter.emitterSpawn, editor)}, {"emitterUpdate", EncodeGraph(emitter.emitterUpdate, editor)},
            {"emitterCollision", EncodeGraph(emitter.emitterCollision, editor)}});
    }
    return json.dump(2);
}

bool DeserializeSystem(std::string_view text, SystemAsset& asset, std::string& error)
{
    if (text.size() > 64u*1024u*1024u) { error = "VFX source exceeds 64 MiB"; return false; }
    try
    {
        const auto json = Json::parse(text);
        SystemAsset candidate;
        candidate.version = json.at("version").get<uint32_t>();
        if (candidate.version < 1 || candidate.version > 3) throw std::runtime_error("Unsupported VFX asset version");
        if (json.contains("timing"))
        {
            if (candidate.version < 3) throw std::runtime_error("System timing requires runtime settings version 3");
            const auto& timing = json.at("timing");
            if (!timing.at("warmupTickCount").is_number_integer() || timing.at("warmupTickCount").get<int64_t>() < 0 ||
                timing.at("warmupTickCount").get<uint64_t>() > kMaxSystemWarmupTicks)
                throw std::runtime_error("Invalid system warmup tick count");
            candidate.timing = {timing.at("fixedTick").get<bool>(),timing.at("fixedTickDelta").get<double>(),
                timing.at("warmupTickCount").get<uint32_t>(),timing.at("warmupTickDelta").get<double>()};
            if (json.contains("completion"))
            {
                const auto& completion = json.at("completion");
                const auto grace = completion.at("unusedGracePeriod").get<double>();
                if (!std::isfinite(grace) || grace < 0 || grace > 3600) throw std::runtime_error("Invalid unused grace period");
                candidate.completion = {completion.at("enabled").get<bool>(),grace};
            }
            std::string timingError;
            if (!ValidateSystemTiming(candidate.timing,timingError)) throw std::runtime_error(timingError);
        }
        if (json.contains("parameters"))
        {
            if (candidate.version < 2 || !json["parameters"].is_array() || json["parameters"].size() > 4096) throw std::runtime_error("Invalid typed parameter schema");
            std::unordered_set<std::string> names;
            for (const auto& p : json["parameters"])
            {
                const auto type = p.at("type").get<uint32_t>(); const auto name = p.at("name").get<std::string>();
                if (name.empty() || !names.insert(name).second || type > static_cast<uint32_t>(ValueType::Bool)) throw std::runtime_error("Invalid parameter declaration");
                candidate.parameters.push_back({name,static_cast<ValueType>(type),Detail::DecodeValue(p.at("value"),static_cast<ValueType>(type))});
            }
        }
        candidate.id = json.at("id").get<std::string>();
        candidate.name = json.at("name").get<std::string>();
        if (!ValidGuid(candidate.id) || !json.at("emitters").is_array() || json.at("emitters").size() > 1024)
            throw std::runtime_error("Invalid VFX system");
        std::unordered_set<std::string> emitterIds;
        std::unordered_set<std::string> graphIds;
        if (json.contains("systemAttributes"))
        {
            const auto& attributes = json.at("systemAttributes");
            if (!attributes.is_array() || attributes.size() > 1024) throw std::runtime_error("Invalid system attribute list");
            std::unordered_set<std::string> names;
            for (const auto& data : attributes)
            {
                const auto type = data.at("type").get<uint32_t>();
                const auto name = data.at("name").get<std::string>();
                if (name.empty() || !names.insert(name).second || type > static_cast<uint32_t>(ValueType::Bool))
                    throw std::runtime_error("Invalid system attribute");
                candidate.systemAttributes.push_back({name,static_cast<ValueType>(type),
                    Detail::DecodeValue(data.at("default"),static_cast<ValueType>(type))});
            }
        }
        if (json.contains("systemSpawn")) candidate.systemSpawn = DecodeGraph(json.at("systemSpawn"),ScriptStage::SystemSpawn);
        else { candidate.systemSpawn.id = Guid::New().ToString(); candidate.systemSpawn.stage = ScriptStage::SystemSpawn; }
        if (json.contains("systemUpdate")) candidate.systemUpdate = DecodeGraph(json.at("systemUpdate"),ScriptStage::SystemUpdate);
        else { candidate.systemUpdate.id = Guid::New().ToString(); candidate.systemUpdate.stage = ScriptStage::SystemUpdate; }
        if (!graphIds.insert(candidate.systemSpawn.id).second || !graphIds.insert(candidate.systemUpdate.id).second)
            throw std::runtime_error("Duplicate system graph GUID");
        for (const auto& entry : json.at("emitters"))
        {
            EmitterAsset emitter;
            emitter.id = entry.at("id").get<std::string>();
            emitter.name = entry.at("name").get<std::string>();
            if (!ValidGuid(emitter.id) || !emitterIds.insert(emitter.id).second) throw std::runtime_error("Invalid or duplicate emitter ID");
            const auto target = entry.at("target").get<uint32_t>();
            if (target > 1) throw std::runtime_error("Unknown simulation target");
            emitter.target = static_cast<SimulationTarget>(target);
            emitter.capacity = entry.at("capacity").get<uint32_t>();
            emitter.initialCapacity = entry.value("initialCapacity",0u);
            emitter.seed = entry.at("seed").get<uint32_t>();
            emitter.spawnRate = entry.at("spawnRate").get<double>();
            emitter.duration = entry.at("duration").get<double>();
            emitter.looping = entry.at("looping").get<bool>();
            emitter.interpolatedSpawning = entry.value("interpolatedSpawning", true);
            emitter.spawnPerFrame = entry.value("spawnPerFrame",0u);
            if (emitter.spawnPerFrame > 100000) throw std::runtime_error("Invalid Spawn Per Frame count");
            emitter.spawnPerUnit = entry.value("spawnPerUnit",0.0);
            if (!std::isfinite(emitter.spawnPerUnit) || emitter.spawnPerUnit < 0 || emitter.spawnPerUnit > 100000)
                throw std::runtime_error("Invalid Spawn Per Unit density");
            emitter.sortByViewDepth = entry.value("sortByViewDepth", false);
            emitter.frustumCulling = entry.value("frustumCulling", true);
            const auto renderer = entry.value("renderer",0u);
            if (renderer > 2) throw std::runtime_error("Unknown particle renderer");
            emitter.renderer = static_cast<RendererType>(renderer);
            if (entry.contains("mesh"))
            {
                const auto& mesh = entry.at("mesh");
                const auto reference = [](const Json& source) { return AssetReference{source.value("guid",std::string{}),source.value("localId",int64_t{0})}; };
                if (mesh.contains("meshes"))
                {
                    if (!mesh.at("meshes").is_array() || mesh.at("meshes").size() > kMaxMeshSlots)
                        throw std::runtime_error("Invalid particle mesh slot list");
                    for (const auto& slot : mesh.at("meshes")) emitter.mesh.meshes.push_back(reference(slot));
                }
                else if (mesh.contains("asset")) emitter.mesh.meshes.push_back(reference(mesh.at("asset")));
                if (mesh.contains("materials"))
                {
                    if (!mesh.at("materials").is_array() || mesh.at("materials").size() > kMaxMeshSlots)
                        throw std::runtime_error("Invalid particle mesh material override list");
                    for (const auto& slot : mesh.at("materials")) emitter.mesh.materials.push_back(reference(slot));
                }
                emitter.mesh.meshIndexAttribute = mesh.value("meshIndexAttribute",emitter.mesh.meshIndexAttribute);
                emitter.mesh.lodIndex = mesh.value("lodIndex",emitter.mesh.lodIndex);
                emitter.mesh.scaleAttribute = mesh.value("scaleAttribute",emitter.mesh.scaleAttribute);
                emitter.mesh.orientationAttribute = mesh.value("orientationAttribute",emitter.mesh.orientationAttribute);
                emitter.mesh.defaultScale = mesh.value("defaultScale",emitter.mesh.defaultScale);
                emitter.mesh.defaultOrientation = mesh.value("defaultOrientation",emitter.mesh.defaultOrientation);
                std::string meshError;
                if (!ValidateMeshSettings(emitter.mesh,meshError)) throw std::runtime_error(meshError);
            }
            if (entry.contains("sprite"))
            {
                const auto& sprite = entry.at("sprite");
                const auto reference = [](const Json& source) { return AssetReference{source.value("guid",std::string{}),source.value("localId",int64_t{0})}; };
                if (sprite.contains("material")) emitter.sprite.material = reference(sprite.at("material"));
                if (sprite.contains("texture")) emitter.sprite.texture = reference(sprite.at("texture"));
                const auto blend = sprite.value("blend",0u), alignment = sprite.value("alignment",0u);
                if (blend > 4 || alignment > 1) throw std::runtime_error("Invalid Sprite blend or alignment");
                emitter.sprite.blend = static_cast<SpriteBlendMode>(blend); emitter.sprite.alignment = static_cast<SpriteAlignment>(alignment);
                emitter.sprite.tint = sprite.value("tint",Value{1,1,1,1}); emitter.sprite.emissive = sprite.value("emissive",1.0f);
                emitter.sprite.alphaCutoff = sprite.value("alphaCutoff",0.333f); emitter.sprite.softDistance = sprite.value("softDistance",0.0f);
                emitter.sprite.subImagesX = sprite.value("subImagesX",1u); emitter.sprite.subImagesY = sprite.value("subImagesY",1u);
                emitter.sprite.blendSubImages = sprite.value("blendSubImages",false); emitter.sprite.circularMask = sprite.value("circularMask",true);
                emitter.sprite.useMaterialShader = sprite.value("useMaterialShader",false);
                if (sprite.contains("dynamicParameters"))
                {
                    const auto& bindings = sprite.at("dynamicParameters");
                    if (!bindings.is_array() || bindings.size() != 4) throw std::runtime_error("Four dynamic material bindings are required");
                    for (size_t i = 0; i < 4; ++i)
                        emitter.sprite.dynamicParameters[i] = {bindings[i].at("attribute").get<std::string>(),bindings[i].value("fallbackParameter",std::string{}),bindings[i].at("default").get<Value>()};
                }
                if (sprite.contains("materialParameters"))
                {
                    const auto& bindings = sprite.at("materialParameters");
                    if (!bindings.is_array() || bindings.size() > 64) throw std::runtime_error("Invalid material parameter binding count");
                    for (const auto& binding : bindings)
                    {
                        const auto type = binding.at("type").get<uint32_t>();
                        if (type > 3) throw std::runtime_error("Invalid material parameter type");
                        emitter.sprite.materialParameters.push_back({binding.at("materialParameter").get<std::string>(),binding.at("systemParameter").get<std::string>(),
                            static_cast<ValueType>(type),binding.at("default").get<Value>()});
                    }
                }
                std::string spriteError;
                if (!ValidateSpriteSettings(emitter.sprite,spriteError)) throw std::runtime_error(spriteError);
            }
            if (entry.contains("collision"))
            {
                const auto& collision = entry.at("collision");
                emitter.collisionEnabled = collision.value("enabled",false);
                emitter.collisionRadius = collision.value("radius",0.0f);
                emitter.restitution = collision.value("restitution",0.5f);
                emitter.friction = collision.value("friction",0.0f);
                emitter.collisionLayers = collision.value("layers",~0u);
                if (!std::isfinite(emitter.collisionRadius) || emitter.collisionRadius < 0 ||
                    !std::isfinite(emitter.restitution) || emitter.restitution < 0 || emitter.restitution > 1 ||
                    !std::isfinite(emitter.friction) || emitter.friction < 0 || emitter.friction > 1)
                    throw std::runtime_error("Invalid collision radius, restitution or friction");
            }
            if (!emitter.capacity || emitter.capacity > 10000000 || emitter.initialCapacity > emitter.capacity || !std::isfinite(emitter.spawnRate) ||
                emitter.spawnRate < 0 || emitter.spawnRate > 10000000 || !std::isfinite(emitter.duration) || emitter.duration < 0.0001)
                throw std::runtime_error("Invalid emitter capacity, duration or spawn rate");
            if (!entry.at("attributes").is_array() || entry.at("attributes").size() > 1024) throw std::runtime_error("Invalid particle attribute list");
            std::unordered_set<std::string> attributeNames;
            for (const auto& data : entry.at("attributes"))
            {
                Attribute attribute;
                attribute.name = data.at("name").get<std::string>();
                const auto type = data.at("type").get<uint32_t>();
                if (type > static_cast<uint32_t>(ValueType::Bool) || (candidate.version < 2 && type > 3) || attribute.name.empty() || !attributeNames.insert(attribute.name).second)
                    throw std::runtime_error("Invalid particle attribute");
                attribute.type = static_cast<ValueType>(type);
                attribute.defaultValue = Detail::DecodeValue(data.at("default"),attribute.type);
                emitter.attributes.push_back(std::move(attribute));
            }
            if (entry.contains("emitterAttributes"))
            {
                const auto& list = entry.at("emitterAttributes");
                if (!list.is_array() || list.size() > 256) throw std::runtime_error("Invalid emitter attribute list");
                std::unordered_set<std::string> names;
                for (const auto& data : list)
                {
                    Attribute attribute;
                    attribute.name = data.at("name").get<std::string>();
                    const auto type = data.at("type").get<uint32_t>();
                    if (type > static_cast<uint32_t>(ValueType::Bool) || attribute.name.rfind("Emitter.",0) != 0 ||
                        !names.insert(attribute.name).second) throw std::runtime_error("Invalid emitter attribute");
                    attribute.type = static_cast<ValueType>(type);
                    attribute.defaultValue = Detail::DecodeValue(data.at("default"),attribute.type);
                    emitter.emitterAttributes.push_back(std::move(attribute));
                }
            }
            if (entry.contains("ribbon"))
            {
                const auto& ribbon = entry.at("ribbon");
                const auto reference = [](const Json& source) { return AssetReference{source.value("guid",std::string{}),source.value("localId",int64_t{0})}; };
                emitter.ribbon.widthAttribute = ribbon.value("widthAttribute",emitter.ribbon.widthAttribute);
                emitter.ribbon.material = reference(ribbon.at("material"));
                emitter.ribbon.texture = reference(ribbon.at("texture"));
                const auto blend = ribbon.value("blend",static_cast<uint32_t>(SpriteBlendMode::Additive));
                if (blend > static_cast<uint32_t>(SpriteBlendMode::Premultiplied)) throw std::runtime_error("Invalid ribbon blend mode");
                emitter.ribbon.blend = static_cast<SpriteBlendMode>(blend);
                emitter.ribbon.tint = ribbon.value("tint",emitter.ribbon.tint);
                emitter.ribbon.emissive = ribbon.value("emissive",emitter.ribbon.emissive);
                emitter.ribbon.uvTilingDistance = ribbon.value("uvTilingDistance",emitter.ribbon.uvTilingDistance);
                emitter.ribbon.tileUvOverDistance = ribbon.value("tileUvOverDistance",emitter.ribbon.tileUvOverDistance);
                emitter.ribbon.useMaterialShader = ribbon.value("useMaterialShader",emitter.ribbon.useMaterialShader);
            }
            if (entry.contains("eventHandlers"))
            {
                const auto& handlers = entry.at("eventHandlers");
                if (!handlers.is_array() || handlers.size() > 64) throw std::runtime_error("Invalid particle event handler list");
                std::unordered_set<std::string> handlerIds;
                for (const auto& data : handlers)
                {
                    EmitterAsset::EventHandler handler;
                    handler.graph = DecodeGraph(data.at("graph"),ScriptStage::Event);
                    handler.id = data.value("id",handler.graph.id);
                    handler.name = data.value("name",std::string{});
                    if (!ValidGuid(handler.id) || !handlerIds.insert(handler.id).second)
                        throw std::runtime_error("Invalid or duplicate particle event handler ID");
                    if (data.contains("receiver"))
                    {
                        const auto& receiver = data.at("receiver");
                        const auto kind = receiver.value("eventKind",0u);
                        const auto count = receiver.value("spawnCount",1u);
                        const auto randomAge = receiver.value("randomAge",0.0f);
                        const auto maxEvents = receiver.value("maxEventsPerFrame",0u);
                        const auto executionMode = receiver.value("executionMode",0u);
                        if (kind > static_cast<uint32_t>(ParticleEventSelection::Collision) || count == 0u || count > 100000 ||
                            !std::isfinite(randomAge) || randomAge < 0 || randomAge > 3600 || maxEvents > 1000000 ||
                            executionMode > static_cast<uint32_t>(EventExecutionMode::SingleParticle))
                            throw std::runtime_error("Invalid particle event receiver");
                        handler.receiver.sourceEmitter = receiver.value("sourceEmitter",std::string{});
                        handler.receiver.eventKind = static_cast<ParticleEventSelection>(kind);
                        handler.receiver.spawnCount = count;
                        handler.receiver.randomAge = randomAge;
                        handler.receiver.maxEventsPerFrame = maxEvents;
                        handler.receiver.executionMode = static_cast<EventExecutionMode>(executionMode);
                    }
                    emitter.eventHandlers.push_back(std::move(handler));
                }
            }
            emitter.eventGenerationMask = entry.value("eventGenerationMask",uint64_t{kParticleEventGenerationAll});
            emitter.hasEventGenerationPolicy = entry.value("hasEventGenerationPolicy",false);
            if ((emitter.eventGenerationMask & ~kParticleEventGenerationAll) != 0)
                throw std::runtime_error("Invalid particle event generation mask");
            if (!entry.at("bursts").is_array() || entry.at("bursts").size() > 4096) throw std::runtime_error("Invalid burst list");
            for (const auto& data : entry.at("bursts"))
            {
                Burst burst{data.at("time").get<double>(), data.at("count").get<uint32_t>()};
                if (!std::isfinite(burst.time) || burst.time < 0 || burst.time >= emitter.duration || burst.count > 10000000)
                    throw std::runtime_error("Burst time must be inside the emitter duration");
                emitter.bursts.push_back(burst);
            }
            emitter.spawn = DecodeGraph(entry.at("spawn"), ScriptStage::ParticleSpawn);
            emitter.update = DecodeGraph(entry.at("update"), ScriptStage::ParticleUpdate);
            // Files authored before the emitter graph slots existed need valid
            // unique GUIDs so a subsequent save can round-trip through the
            // normal graph decoder.
            if (entry.contains("emitterSpawn")) emitter.emitterSpawn = DecodeGraph(entry.at("emitterSpawn"), ScriptStage::EmitterSpawn);
            else { emitter.emitterSpawn.id = Guid::New().ToString(); emitter.emitterSpawn.stage = ScriptStage::EmitterSpawn; }
            if (entry.contains("emitterUpdate")) emitter.emitterUpdate = DecodeGraph(entry.at("emitterUpdate"), ScriptStage::EmitterUpdate);
            else { emitter.emitterUpdate.id = Guid::New().ToString(); emitter.emitterUpdate.stage = ScriptStage::EmitterUpdate; }
            if (entry.contains("emitterCollision")) emitter.emitterCollision = DecodeGraph(entry.at("emitterCollision"), ScriptStage::ParticleUpdate);
            else { emitter.emitterCollision.id = Guid::New().ToString(); emitter.emitterCollision.stage = ScriptStage::ParticleUpdate; }
            if (candidate.version < 2)
            {
                for (const auto* graph : {&emitter.spawn,&emitter.update,&emitter.emitterSpawn,&emitter.emitterUpdate,&emitter.emitterCollision})
                    for (const auto& node : graph->nodes)
                        if (IsIntegralType(node.type) || node.operation > Operation::StageIteration) throw std::runtime_error("Typed graph requires asset version 2");
                for (const auto& handler : emitter.eventHandlers)
                    for (const auto& node : handler.graph.nodes)
                        if (IsIntegralType(node.type) || node.operation > Operation::StageIteration) throw std::runtime_error("Typed graph requires asset version 2");
            }
            if (!graphIds.insert(emitter.spawn.id).second || !graphIds.insert(emitter.update.id).second ||
                !graphIds.insert(emitter.emitterSpawn.id).second || !graphIds.insert(emitter.emitterUpdate.id).second ||
                !graphIds.insert(emitter.emitterCollision.id).second)
                throw std::runtime_error("Duplicate graph GUID");
            for (const auto& handler : emitter.eventHandlers)
                if (!graphIds.insert(handler.graph.id).second) throw std::runtime_error("Duplicate graph GUID");
            if (entry.contains("simulationStages"))
            {
                const auto& stages = entry.at("simulationStages");
                if (!stages.is_array() || stages.size() > 64) throw std::runtime_error("Invalid simulation stage list");
                std::unordered_set<std::string> stageIds; uint32_t iterations = 0;
                for (const auto& sourceStage : stages)
                {
                    SimulationStage stage;
                    stage.id = sourceStage.at("id").get<std::string>(); stage.name = sourceStage.at("name").get<std::string>();
                    stage.enabled = sourceStage.at("enabled").get<bool>(); stage.iterations = sourceStage.at("iterations").get<uint32_t>();
                    if (!ValidGuid(stage.id) || !stageIds.insert(stage.id).second || stage.iterations > 128 || (iterations += stage.iterations) > 1024)
                        throw std::runtime_error("Invalid simulation stage ID or iteration count");
                    if (sourceStage.contains("bindings"))
                    {
                        const auto& bindings = sourceStage.at("bindings");
                        stage.bindings = {bindings.value("enabled",std::string{}),bindings.value("iterations",std::string{}),bindings.value("enabledDefault",true)};
                        stage.bindings.typed = bindings.value("typed",false);
                        if (stage.bindings.typed && candidate.version < 2) throw std::runtime_error("Typed stage bindings require asset version 2");
                    }
                    std::string bindingError;
                    if (!ValidateSimulationStageBindings(stage.bindings,bindingError)) throw std::runtime_error(bindingError);
                    stage.graph = DecodeGraph(sourceStage.at("graph"),ScriptStage::SimulationStage);
                    if (candidate.version < 2) for (const auto& node : stage.graph.nodes)
                        if (IsIntegralType(node.type) || node.operation > Operation::StageIteration) throw std::runtime_error("Typed stage graph requires asset version 2");
                    if (!graphIds.insert(stage.graph.id).second) throw std::runtime_error("Duplicate simulation stage graph GUID");
                    emitter.simulationStages.push_back(std::move(stage));
                }
            }
            candidate.emitters.push_back(std::move(emitter));
        }
        asset = std::move(candidate);
        error.clear();
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool LoadSystem(const std::filesystem::path& path, SystemAsset& asset, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "Cannot open VFX asset: " + path.string(); return false; }
    input.seekg(0,std::ios::end);
    const auto length = input.tellg();
    if (length < 0 || length > 64u*1024u*1024u) { error = "Invalid VFX file size"; return false; }
    std::string text(static_cast<size_t>(length),'\0');
    input.seekg(0);
    input.read(text.data(),static_cast<std::streamsize>(text.size()));
    if (!input) { error = "Cannot read complete VFX asset"; return false; }
    return DeserializeSystem(text, asset, error);
}

bool SaveSystem(const std::filesystem::path& path, const SystemAsset& asset, std::string& error)
{
    try
    {
        const auto temporary = std::filesystem::path(path.string() + "." + Guid::New().ToString() + ".tmp");
        const auto text = SerializeSystem(asset);
        SystemAsset validated;
        if (!DeserializeSystem(text,validated,error)) return false;
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.close();
            if (!output) { std::filesystem::remove(temporary); error = "Failed to write VFX asset"; return false; }
        }
        std::error_code result;
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            result = std::error_code(static_cast<int>(GetLastError()),std::system_category());
#else
        std::filesystem::rename(temporary, path, result);
#endif
        if (result)
        {
            std::filesystem::remove(temporary);
            error = "Cannot replace VFX asset: " + result.message();
            return false;
        }
        error.clear();
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
}
