#include <VFX/VfxAggregateLowering.h>
#include "VfxCompiler.h"
#include "ParticleSpriteSource.h"
#include <VFX/VfxTypeRules.h>
#include <VFX/VfxParameterValidation.h>
#include <VFX/CpuEmitter.h>
#include <VFX/VfxTypedIR.h>
#include <VFX/VfxTypedBytecode.h>
#include <VFX/VfxTypedShader.h>
#include <VFX/GpuEventBuffer.h>
#include <VFX/GpuEventSchedule.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <Guid.h>

namespace NLS::VFX
{
bool BuildRibbonShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const RibbonSettings& settings, const SpriteSettings& material);
namespace
{
std::string Number(float value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::scientific << std::setprecision(9) << value;
    return stream.str();
}

std::string Vector(const Value& value, uint32_t width)
{
    if (width == 1) return Number(value[0]);
    std::string result = "float" + std::to_string(width) + "(";
    for (uint32_t component = 0; component < width; ++component)
        result += (component ? "," : "") + Number(value[component]);
    return result + ")";
}

std::string AttributeExpressionAt(const std::vector<Attribute>& attributes, uint32_t attribute,
    const std::string& indexExpression)
{
    uint32_t offset = 0;
    for (uint32_t index = 0; index < attribute; ++index) offset += ComponentCount(attributes[index].type);
    const auto width = ComponentCount(attributes[attribute].type);
    std::string result = width == 1 ? "" : "float" + std::to_string(width) + "(";
    for (uint32_t component = 0; component < width; ++component)
        result += (component ? "," : "") + std::string("asfloat(VfxAttributes[") +
            std::to_string(offset + component) + "u * VfxCapacity + " + indexExpression + "])";
    return result + (width == 1 ? "" : ")");
}

std::string AttributeExpression(const std::vector<Attribute>& attributes, uint32_t attribute,bool bits = false)
{
    uint32_t offset = 0;
    for (uint32_t index = 0; index < attribute; ++index) offset += ComponentCount(attributes[index].type);
    const auto width = ComponentCount(attributes[attribute].type);
    std::string result = width == 1 ? "" : "float" + std::to_string(width) + "(";
    for (uint32_t component = 0; component < width; ++component)
    {
        auto value = std::string("VfxAttributes[") + std::to_string(offset + component) + "u * VfxCapacity + index]";
        if (bits && IsFloatType(attributes[attribute].type)) value = "asfloat("+value+")";
        result += (component ? "," : "") + value;
    }
    return result + (width == 1 ? "" : ")");
}

bool MeshTransformSource(const std::vector<Attribute>& attributes, const MeshSettings& mesh, std::string& source, std::string& error)
{
    if (!ValidateMeshSettings(mesh,error)) return false;
    ParticleDataSet schema;
    try { schema.Initialize(attributes,1); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    const auto position = schema.FindAttribute("Particles.Position");
    const auto scale = mesh.scaleAttribute.empty() ? -1 : schema.FindAttribute(mesh.scaleAttribute);
    const auto orientation = mesh.orientationAttribute.empty() ? -1 : schema.FindAttribute(mesh.orientationAttribute);
    const auto meshIndex = mesh.meshIndexAttribute.empty() ? -1 : schema.FindAttribute(mesh.meshIndexAttribute);
    if (position < 0 || attributes[position].type != ValueType::Float3 ||
        (scale >= 0 && attributes[scale].type != ValueType::Float && attributes[scale].type != ValueType::Float3) ||
        (orientation >= 0 && attributes[orientation].type != ValueType::Float4) ||
        (meshIndex >= 0 && attributes[meshIndex].type != ValueType::Float))
    { error = "Mesh rendering requires float3 Position, scalar/float3 Scale, float4 MeshOrientation (xyzw quaternion) and a scalar MeshIndex"; return false; }
    std::ostringstream shader;
    shader << R"(float3 VfxRotate(float4 q, float3 v) { return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v); }
bool VfxMeshTransform(uint index, out float3 position, out float3 scale, out float4 orientation)
{
)";
    shader << "    position = " << AttributeExpression(attributes,position) << ";\n"
           << "    scale = " << (scale < 0 ? Vector(mesh.defaultScale,3) : attributes[scale].type == ValueType::Float
                ? "float3("+AttributeExpression(attributes,scale)+","+AttributeExpression(attributes,scale)+","+AttributeExpression(attributes,scale)+")" : AttributeExpression(attributes,scale)) << ";\n"
           << "    orientation = " << (orientation < 0 ? Vector(mesh.defaultOrientation,4) : AttributeExpression(attributes,orientation)) << ";\n";
    shader << R"(    if (!all(isfinite(position)) || !all(isfinite(scale)) || !all(isfinite(orientation))) return false;
    float largest = max(max(abs(orientation.x),abs(orientation.y)),max(abs(orientation.z),abs(orientation.w)));
    if (largest > 1e-20) { orientation /= largest; orientation *= rsqrt(dot(orientation,orientation)); }
    else orientation = float4(0,0,0,1);
    return true;
}
bool VfxMeshSphere(uint index, float4 sphere, out float3 center, out float radius)
{
    float3 position, scale; float4 orientation;
    bool valid = VfxMeshTransform(index,position,scale,orientation);
    center = position+VfxRotate(orientation,sphere.xyz*scale);
    radius = sphere.w*max(max(abs(scale.x),abs(scale.y)),abs(scale.z));
    return valid && all(isfinite(center)) && isfinite(radius);
}
)";
    shader << "uint VfxMeshSlotRaw(uint index)\n{\n";
    if (meshIndex < 0) shader << "    return 0u;\n";
    else
    {
        shader << "    float value = " << AttributeExpression(attributes,meshIndex) << ";\n"
               << "    if (!isfinite(value)) return 0xffffffffu;\n"
               << "    return (uint)clamp(floor(value+0.5),0.0," << (mesh.meshes.empty() ? 0 : mesh.meshes.size()-1) << ".0);\n";
    }
    shader << "}\n";
    source = shader.str(); return true;
}

} // namespace

bool BuildGpuCompactionShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error)
{
    try { ParticleDataSet validation; validation.Initialize(attributes,1); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    int32_t age = -1,lifetime = -1;
    uint32_t scalars = 0;
    for (const auto& attribute : attributes)
    {
        if (attribute.name == "Particles.Age" && attribute.type == ValueType::Float) age = static_cast<int32_t>(scalars);
        if (attribute.name == "Particles.Lifetime" && attribute.type == ValueType::Float) lifetime = static_cast<int32_t>(scalars);
        scalars += ComponentCount(attribute.type);
    }
    if (age < 0 || lifetime < 0) { error = "GPU compaction requires scalar Age and Lifetime attributes"; return false; }
    std::ostringstream shader;
    shader << R"(cbuffer VfxConstants : register(b0, space1)
{
    uint VfxCount; uint VfxCapacity; float VfxDeltaTime; uint VfxSeed;
    uint2 VfxTick; uint2 VfxPadding;
};
StructuredBuffer<float> VfxSourceAttributes : register(t0, space1);
StructuredBuffer<uint2> VfxSourceIds : register(t1, space1);
RWStructuredBuffer<float> VfxAttributes : register(u0, space1);
RWStructuredBuffer<uint2> VfxIds : register(u1, space1);
RWStructuredBuffer<uint> VfxCounters : register(u2, space1);
[numthreads(64,1,1)]
void Compact(uint3 dispatchId : SV_DispatchThreadID)
{
    uint index = dispatchId.x+dispatchId.y*4194240u;
    if (index >= min(VfxCount,VfxCounters[0])) return;
)";
    shader << "    float age = VfxSourceAttributes[" << age << "u * VfxCapacity + index];\n"
           << "    float lifetime = VfxSourceAttributes[" << lifetime << "u * VfxCapacity + index];\n"
           << "    if (!isfinite(age) || !isfinite(lifetime) || lifetime <= 0 || age >= lifetime) return;\n"
           << "    uint destination; InterlockedAdd(VfxCounters[1],1u,destination);\n"
           << "    if (destination >= VfxCapacity) return;\n"
           << "    for (uint component = 0; component < " << scalars << "u; ++component)\n"
           << "        VfxAttributes[component*VfxCapacity+destination] = VfxSourceAttributes[component*VfxCapacity+index];\n"
           << "    VfxIds[destination] = VfxSourceIds[index];\n}\n";
    shader << R"([numthreads(1,1,1)]
void Finish(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId)) return;
    VfxCounters[0] = VfxCounters[1];
    VfxCounters[1] = 0;
    VfxCounters[4] = VfxPadding.x;
    VfxCounters[5] = VfxCounters[0];
    VfxCounters[6] = 0;
    VfxCounters[7] = 0;
}
[numthreads(1,1,1)]
void Reset(uint3 dispatchId : SV_DispatchThreadID)
{
    if (any(dispatchId)) return;
    VfxCounters[0] = 0; VfxCounters[1] = 0;
    VfxCounters[2] = 0; VfxCounters[3] = 0;
    VfxCounters[4] = VfxPadding.x; VfxCounters[5] = 0;
    VfxCounters[6] = 0; VfxCounters[7] = 0;
}
)";
    source = shader.str(); error.clear(); return true;
}

namespace
{
// Event policy and particle programs use the same typed translator as every
// other stage. Only storage and payload parameter bindings differ.
bool BuildGpuEventShader(const EmitterAsset& emitter,std::span<const Graph> particleGraphs,
    std::span<const std::string> parameters,std::string& source,std::string& error)
{
    std::ostringstream hlsl;
    hlsl << R"(
struct VfxEventPayload { float4 position; float4 velocity; float4 normal; uint2 id; float source; };
struct VfxHandlerState { uint values[)";
    uint32_t scalarCount = 0;
    for (const auto& attribute : emitter.emitterAttributes) scalarCount += ComponentCount(attribute.type);
    hlsl << (std::max)(scalarCount,1u) << R"(]; };
)";
    const auto payloadExpressions = []()
    {
        return std::unordered_map<std::string,std::string>{
            {"Emitter.EventPosition","float4(payload.position.xyz,0)"},
            {"Emitter.EventVelocity","float4(payload.velocity.xyz,0)"},
            {"Emitter.EventNormal","float4(payload.normal.xyz,0)"},
            {"Emitter.EventKind","float4(payload.position.w,0,0,0)"},
            {"Emitter.EventParticleId","float4(float(payload.id.x),0,0,0)"},
            {"Emitter.EventSourceIndex","float4(payload.source,0,0,0)"}};
    };
    std::vector<std::unordered_set<std::string>> writes(emitter.eventHandlers.size());
    const auto generate = [&](const Graph& graph,std::span<const Attribute> attributes,
        const std::string& function,Typed::HlslGenerationOptions options)
    {
        Typed::TypeRegistry types; Typed::Graph typed; Typed::OptimizationResult layout; Typed::OptimizationStats stats;
        std::string text;
        if ((!graph.nodes.empty() && (!Typed::TranslateGraph(graph,types,attributes,typed,error) ||
            !Typed::OptimizeGraph(typed,types,layout,stats,error))) ||
            !Typed::GenerateTypedHlsl(typed,layout,types,attributes,text,error,parameters,function,options)) return false;
        hlsl << text; return true;
    };
    for (size_t index = 0; index < emitter.eventHandlers.size(); ++index)
    {
        const auto& graph = emitter.eventHandlers[index].graph;
        Graph policy = graph;
        policy.nodes.clear();
        std::unordered_set<NodeId> needed;
        std::function<void(NodeId)> visit = [&](NodeId id)
        {
            if (!needed.insert(id).second) return;
            if (const auto* node = FindNode(graph,id)) for (const auto input : node->inputs) visit(input);
        };
        for (const auto& node : graph.nodes)
            if (node.operation == Operation::WriteAttribute && node.name.starts_with("Emitter."))
            { writes[index].insert(node.name); visit(node.id); }
        for (const auto& node : graph.nodes) if (needed.contains(node.id)) policy.nodes.push_back(node);
        Typed::HlslGenerationOptions options;
        options.declarations = false; options.attributeBuffer = "state.values"; options.attributeStride = "1u";
        options.functionParameters = ", inout VfxHandlerState state, VfxEventPayload payload";
        options.parameterExpressions = payloadExpressions();
        options.particleIdExpression = "uint2(1,0)"; options.tickExpression = "payload.id";
        if (!generate(policy,emitter.emitterAttributes,"VfxEventPolicy"+std::to_string(index),options)) return false;
        options.attributeBuffer = "VfxAttributes"; options.attributeStride = "VfxCapacity"; options.particleIdExpression = "VfxIds[index]";
        options.functionParameters = ", VfxEventPayload payload";
        if (!generate(particleGraphs[index],emitter.attributes,"VfxEvaluateEvent"+std::to_string(index),options)) return false;
    }
    const auto parameterIndex = [&](std::string_view name)
    {
        const auto found = std::find(parameters.begin(),parameters.end(),name);
        return static_cast<uint32_t>(found-parameters.begin());
    };
    const auto parameter = [&](std::string_view name) { return "asfloat(VfxParameters["+std::to_string(parameterIndex(name))+"].x)"; };
    const auto vectorParameter = [&](std::string_view name) { return "asfloat(VfxParameters["+std::to_string(parameterIndex(name))+"])"; };
    const auto offset = [](std::span<const Attribute> attributes,std::string_view name)
    {
        uint32_t scalar = 0;
        for (const auto& attribute : attributes)
        { if (attribute.name == name) return scalar; scalar += ComponentCount(attribute.type); }
        return UINT32_MAX;
    };
    hlsl << R"([numthreads(1,1,1)]
void ReceiveEvents(uint3 dispatchId : SV_DispatchThreadID)
{
    uint receiver = uint()" << parameter("Emitter.EventEmitterIndex") << R"();
    uint handler = uint()" << parameter("Emitter.EventHandlerIndex") << R"();
    uint consumedSlot = 256u+receiver*64u+handler;
    uint maxEvents = uint()" << parameter("Emitter.EventMaxEvents") << R"();
    uint mode = uint()" << parameter("Emitter.EventExecutionMode") << R"();
    bool gpuSource = )" << parameter("Emitter.EventMode") << R"( != 0;
    int boundSource = int()" << parameter("Emitter.EventSourceIndex") << R"();
    uint beginSource = gpuSource && boundSource >= 0 ? uint(boundSource) : 0u;
    uint endSource = gpuSource ? (boundSource >= 0 ? beginSource+1u : uint()" << parameter("Emitter.EventEmitterCount") << R"()) : 1u;
    uint birth = 0;
    VfxHandlerState state;
)";
    uint32_t scalar = 0;
    for (const auto& attribute : emitter.emitterAttributes)
        for (uint32_t component = 0; component < ComponentCount(attribute.type); ++component)
            hlsl << "    state.values[" << scalar++ << "] = " << std::bit_cast<uint32_t>(attribute.defaultValue[component]) << "u;\n";
    hlsl << R"(    for (uint source = beginSource; source < endSource; ++source)
    {
        if (gpuSource && source == receiver) continue;
        uint eventCount = gpuSource ? min(VfxEventCounts[source],256u) : 1u;
        for (uint ordinal = 0; ordinal < eventCount; ++ordinal)
        {
            if (maxEvents != 0 && VfxEventCounts[consumedSlot] >= maxEvents) break;
            VfxEventPayload payload;
            if (gpuSource)
            {
                uint record = (source*256u+ordinal)*3u;
                payload.position = VfxEvents[record]; payload.velocity = VfxEvents[record+1u]; payload.normal = VfxEvents[record+2u];
                payload.id = uint2(asuint(payload.velocity.w),asuint(payload.normal.w)); payload.source = float(source);
                if (uint(payload.position.w) != uint()" << parameter("Emitter.EventKind") << R"()) continue;
            }
            else
            {
                payload.position = )" << vectorParameter("Emitter.EventPosition") << R"(;
                payload.position.w = )" << parameter("Emitter.EventKind") << R"(;
                payload.velocity = )" << vectorParameter("Emitter.EventVelocity") << R"(;
                payload.normal = )" << vectorParameter("Emitter.EventNormal") << R"(;
                payload.id = VfxTick; payload.source = )" << parameter("Emitter.EventSourceIndex") << R"(;
            }
            VfxEventCounts[consumedSlot]++;
            float spawnCount = )" << parameter("Emitter.EventPerEvent") << R"(;
            float randomAge = )" << parameter("Emitter.EventRandomAge") << R"(;
            float3 receivePosition = payload.position.xyz, receiveVelocity = payload.velocity.xyz;
            bool overrideVelocity = )" << parameter("Emitter.EventOverrideVelocity") << R"( != 0;
)";
    for (size_t index = 0; index < emitter.eventHandlers.size(); ++index)
    {
        hlsl << "            if (gpuSource && handler == " << index << "u)\n            {\n"
             << "                VfxEventPolicy" << index << "(0,ordinal,0,state,payload);\n";
        for (const auto& [name,variable] : std::array<std::pair<std::string_view,std::string_view>,4>{{
            {"Emitter.ReceiveSpawnCount","spawnCount"},{"Emitter.ReceiveRandomAge","randomAge"},
            {"Emitter.ReceivePosition","receivePosition"},{"Emitter.ReceiveVelocity","receiveVelocity"}}})
        {
            const auto field = offset(emitter.emitterAttributes,name);
            if (field == UINT32_MAX || !writes[index].contains(std::string(name))) continue;
            const bool vector = name == "Emitter.ReceivePosition" || name == "Emitter.ReceiveVelocity";
            if (name == "Emitter.ReceiveRandomAge")
            {
                const auto value = "asfloat(state.values["+std::to_string(field)+"])";
                hlsl << "                if (isfinite(" << value << ") && " << value << " >= 0 && " << value << " <= 3600) randomAge = " << value << ";\n";
                continue;
            }
            hlsl << "                " << variable << " = ";
            if (vector) hlsl << "float3(";
            for (uint32_t c = 0; c < (vector?3u:1u); ++c)
            { if (c) hlsl << ","; hlsl << "asfloat(state.values[" << field+c << "])"; }
            if (vector) hlsl << ")";
            hlsl << ";\n";
            if (name == "Emitter.ReceiveVelocity") hlsl << "                overrideVelocity = true;\n";
        }
        hlsl << "            }\n";
    }
    hlsl << R"(            if (!isfinite(spawnCount)) continue;
            uint count = mode == 0u ? uint(clamp(floor(spawnCount+0.5),0,100000)) : min(VfxCounters[0],VfxCapacity);
            if (mode == 0u) count = min(count,VfxCapacity-min(VfxCounters[0],VfxCapacity));
            uint base = mode == 0u ? min(VfxCounters[0],VfxCapacity) : 0u;
            if (mode == 0u) VfxCounters[0] = base+count;
            for (uint particle = 0; particle < count; ++particle)
            {
                uint index = base+particle;
                if (mode == 2u && any(VfxIds[index] != payload.id)) continue;
                if (mode == 0u) VfxSpawnOne(index,birth++);
)";
    const auto writeVector = [&](std::string_view name,std::string_view value,bool conditional)
    {
        const auto field = offset(emitter.attributes,name); if (field == UINT32_MAX) return;
        if (conditional) hlsl << "                if (mode == 0u" << (name == "Particles.Velocity" ? " && overrideVelocity" : "") << ")\n                {\n";
        for (uint32_t c = 0; c < 3; ++c)
            hlsl << "                VfxAttributes[" << field+c << "u*VfxCapacity+index] = asuint(" << value << "." << "xyz"[c] << ");\n";
        if (conditional) hlsl << "                }\n";
    };
    writeVector("Particles.Position","receivePosition",true); writeVector("Particles.Velocity","receiveVelocity",true);
    writeVector("Particles.EventPosition","payload.position",false); writeVector("Particles.EventVelocity","payload.velocity",false);
    writeVector("Particles.EventNormal","payload.normal",false);
    for (const auto& [name,value] : std::array<std::pair<std::string_view,std::string_view>,3>{{
        {"Particles.EventKind","payload.position.w"},{"Particles.EventParticleId","float(payload.id.x)"},{"Particles.EventSourceIndex","payload.source"}}})
    {
        const auto field = offset(emitter.attributes,name);
        if (field != UINT32_MAX) hlsl << "                VfxAttributes[" << field << "u*VfxCapacity+index] = asuint(" << value << ");\n";
    }
    for (size_t index = 0; index < emitter.eventHandlers.size(); ++index)
        hlsl << "                if (handler == " << index << "u) VfxEvaluateEvent" << index << "(index,particle,0,payload);\n";
    const auto age = offset(emitter.attributes,"Particles.Age");
    if (age != UINT32_MAX)
        hlsl << "                if (mode == 0u && isfinite(randomAge) && randomAge > 0) VfxAdvance(index,VfxRandom(VfxIds[index],0x9e3779b9u,uint2(0,0),0)*min(randomAge,3600.0));\n";
    hlsl << R"(                if (mode == 0u) VfxPublishEvent(index,1u);
            }
        }
    }
}
)";
    source = hlsl.str(); return true;
}

// True when every attribute the graph writes exists in the emitter layout.
bool AttributesAvailable(const Graph& graph,std::span<const Attribute> attributes)
{
    for (const auto& node : graph.nodes)
        if (node.operation == Operation::WriteAttribute)
        {
            const auto found = std::find_if(attributes.begin(),attributes.end(),
                [&](const Attribute& attribute) { return attribute.name == node.name; });
            if (found == attributes.end()) return false;
        }
    return true;
}
Graph ExtractEventGraph(const Graph& source,bool particles)
{
    Graph result;
    result.id = source.id;
    result.stage = ScriptStage::Event;
    result.nextNodeId = source.nextNodeId;
    result.structs = source.structs; result.dataInterfaces = source.dataInterfaces;
    std::unordered_set<NodeId> needed;
    std::function<void(NodeId)> visit = [&](NodeId id)
    {
        if (!id || !needed.insert(id).second) return;
        if (const auto* node = FindNode(source,id))
            for (const auto input : node->inputs) visit(input);
    };
    for (const auto& node : source.nodes)
        if (node.operation == Operation::WriteAttribute && node.name.starts_with(particles ? "Particles." : "Emitter."))
        {
            needed.insert(node.id);
            for (const auto input : node.inputs) visit(input);
        }
    for (const auto& node : source.nodes)
        if (needed.contains(node.id)) result.nodes.push_back(node);
    return result;
}
}
bool CompileSystem(const SystemAsset& asset, CompiledSystem& output, std::vector<Diagnostic>& diagnostics)
{
    CompiledSystem candidate;
    candidate.asset = asset;
    // Runtime artifacts ignore emitter-stage graphs. Keep their identities
    // valid and unique because asset serialization validates both even when
    // callers copy an emitter (which copies the source graph GUIDs).
    std::set<std::string> emitterGraphIds{candidate.asset.systemSpawn.id,candidate.asset.systemUpdate.id};
    for (auto& emitter : candidate.asset.emitters)
    {
        const auto ensureGraphId = [&](Graph& graph,std::string_view slot,ScriptStage stage)
        {
            const auto parsed = Guid::TryParse(graph.id);
            if (!parsed || !parsed->IsValid() || !emitterGraphIds.insert(graph.id).second)
            {
                uint32_t suffix = 0;
                do { graph.id = Guid::NewDeterministic(emitter.id+"/CompiledGraph/"+std::string(slot)+"/"+std::to_string(suffix++)).ToString(); }
                while (!emitterGraphIds.insert(graph.id).second);
            }
            if (graph.nodes.empty()) graph.stage = stage;
        };
        ensureGraphId(emitter.spawn,"ParticleSpawn",ScriptStage::ParticleSpawn);
        ensureGraphId(emitter.update,"ParticleUpdate",ScriptStage::ParticleUpdate);
        ensureGraphId(emitter.emitterSpawn,"EmitterSpawn",ScriptStage::EmitterSpawn);
        ensureGraphId(emitter.emitterUpdate,"EmitterUpdate",ScriptStage::EmitterUpdate);
        ensureGraphId(emitter.emitterCollision,"Collision",ScriptStage::ParticleUpdate);
        for (size_t h = 0; h < emitter.eventHandlers.size(); ++h)
            ensureGraphId(emitter.eventHandlers[h].graph,"Event"+std::to_string(h),ScriptStage::Event);
        for (size_t stage = 0; stage < emitter.simulationStages.size(); ++stage)
            ensureGraphId(emitter.simulationStages[stage].graph,"Simulation"+std::to_string(stage),ScriptStage::SimulationStage);
    }
    for (auto& emitter : candidate.asset.emitters)
        if (emitter.target == SimulationTarget::GPU && !emitter.eventHandlers.empty())
        {
            for (auto name : {"Particles.EventPosition","Particles.EventVelocity","Particles.EventNormal"})
                if (std::none_of(emitter.attributes.begin(),emitter.attributes.end(),[&](const auto& a) { return a.name == name; }))
                    emitter.attributes.push_back({name,ValueType::Float3,{}});
            for (auto name : {"Particles.EventKind","Particles.EventParticleId","Particles.EventSourceIndex"})
                if (std::none_of(emitter.attributes.begin(),emitter.attributes.end(),[&](const auto& a) { return a.name == name; }))
                    emitter.attributes.push_back({name,ValueType::Float,{}});
        }
    diagnostics.clear();
    std::vector<size_t> gpuOrder; std::string eventOrderError;
    if (!BuildGpuEventOrder(asset,gpuOrder,eventOrderError)) { diagnostics.push_back({eventOrderError}); return false; }
    std::string timingError;
    if (!ValidateSystemTiming(asset.timing,timingError)) { diagnostics.push_back({timingError}); return false; }
    auto systemSpawn = CompileGraph(asset.systemSpawn,asset.systemAttributes);
    auto systemUpdate = CompileGraph(asset.systemUpdate,asset.systemAttributes);
    diagnostics.insert(diagnostics.end(), systemSpawn.diagnostics.begin(), systemSpawn.diagnostics.end());
    diagnostics.insert(diagnostics.end(),systemUpdate.diagnostics.begin(),systemUpdate.diagnostics.end());
    for (size_t emitterIndex = 0; emitterIndex < asset.emitters.size(); ++emitterIndex)
    {
        const auto& emitter = candidate.asset.emitters[emitterIndex];
        EmissionScheduler emission;
        std::string settingsError;
        if (!emission.Initialize(emitter,settingsError))
            diagnostics.push_back({std::move(settingsError),emitter.spawn.id,0});
        if (!ValidateSpriteSettings(emitter.sprite,settingsError)) diagnostics.push_back({settingsError,emitter.update.id,0});
        if (emitter.renderer == RendererType::Ribbon && !ValidateRibbonSettings(emitter.ribbon,settingsError))
            diagnostics.push_back({settingsError,emitter.update.id,0});
        std::string spriteSource;
        const auto* mesh = emitter.renderer == RendererType::Mesh ? &emitter.mesh : nullptr;
        if (static_cast<uint32_t>(emitter.renderer) > 2)
            diagnostics.push_back({"Unknown particle renderer",emitter.update.id,0});
        if (mesh && (mesh->meshes.empty() || std::any_of(mesh->meshes.begin(),mesh->meshes.end(),
            [](const AssetReference& reference) { return reference.Empty(); })))
            diagnostics.push_back({"Mesh renderer requires an imported mesh subasset for every slot",emitter.update.id,0});
        if (emitter.renderer == RendererType::Ribbon)
        {
            if (!BuildRibbonShader(emitter.attributes,spriteSource,settingsError,emitter.ribbon,emitter.sprite))
                diagnostics.push_back({settingsError,emitter.update.id,0});
        }
        else if (!BuildSpriteShader(emitter.attributes,spriteSource,settingsError,emitter.sprite,mesh))
            diagnostics.push_back({settingsError,emitter.update.id,0});
        if (emitter.target == SimulationTarget::GPU && emitter.collisionEnabled)
            diagnostics.push_back({"PhysX collision requires CPU simulation; GPU collision data interfaces are not available yet",emitter.update.id,0});
        if (emitter.target == SimulationTarget::CPU && !emitter.simulationStages.empty())
            diagnostics.push_back({"Simulation Stages require GPU simulation",emitter.update.id,0});
        uint64_t scalars = 0; for (const auto& attribute : emitter.attributes) scalars += ComponentCount(attribute.type);
        if (emitter.target == SimulationTarget::GPU && uint64_t(emitter.capacity)*scalars*sizeof(float) > 512u*1024u*1024u)
            diagnostics.push_back({"GPU particle limit exceeds the 512 MiB attribute storage limit",emitter.update.id,0});
        if (emitter.eventHandlers.size() > 64) diagnostics.push_back({"An emitter supports at most 64 event handlers",emitter.spawn.id,0});
        // Per-particle payload program. The emitter event graph owns receiver
        // policy; this compact program runs on each target particle for
        // EveryParticle/SingleParticle/SpawnedParticles execution modes.
        std::vector<CompileResult> eventParticles(emitter.eventHandlers.size());
        std::vector<Graph> eventParticleGraphs(emitter.eventHandlers.size());
        for (size_t handlerIndex = 0; handlerIndex < emitter.eventHandlers.size(); ++handlerIndex)
        {
            auto particleGraph = ExtractEventGraph(emitter.eventHandlers[handlerIndex].graph,true);
            if (particleGraph.nodes.empty())
            {
                Graph graph;
                graph.id = Guid::New().ToString();
                graph.stage = ScriptStage::Event;
                const auto parameter = [&](std::string_view name,ValueType type)
                {
                    const auto id = AddNode(graph,Operation::Parameter,type);
                    FindNode(graph,id)->name = std::string(name);
                    return id;
                };
                const auto write = [&](std::string_view name,ValueType type,NodeId input)
                {
                    const auto id = AddNode(graph,Operation::WriteAttribute,type);
                    FindNode(graph,id)->name = std::string(name);
                    FindNode(graph,id)->inputs = {input};
                };
                write("Particles.EventPosition",ValueType::Float3,parameter("Emitter.EventPosition",ValueType::Float3));
                write("Particles.EventVelocity",ValueType::Float3,parameter("Emitter.EventVelocity",ValueType::Float3));
                write("Particles.EventNormal",ValueType::Float3,parameter("Emitter.EventNormal",ValueType::Float3));
                write("Particles.EventKind",ValueType::Float,parameter("Emitter.EventKind",ValueType::Float));
                write("Particles.EventParticleId",ValueType::Float,parameter("Emitter.EventParticleId",ValueType::Float));
                write("Particles.EventSourceIndex",ValueType::Float,parameter("Emitter.EventSourceIndex",ValueType::Float));
                particleGraph = std::move(graph);
            }
            // A GPU receiver runs the payload program inside its spawn kernel.
            // It is only compiled when the emitter layout carries every
            // attribute the program writes.
            if (emitter.target == SimulationTarget::GPU && !AttributesAvailable(particleGraph,emitter.attributes))
                continue;
            eventParticleGraphs[handlerIndex] = particleGraph;
            eventParticles[handlerIndex] = CompileGraph(particleGraph,emitter.attributes);
        }
        auto spawnGraph = emitter.spawn;
        auto updateGraph = emitter.update;
        if (emitter.target == SimulationTarget::GPU)
        {
            const auto addParameter = [](Graph& graph,std::string_view name,ValueType type,Value value = {})
            {
                for (const auto& node : graph.nodes) if (node.operation == Operation::Parameter && node.name == name) return;
                const auto id = AddNode(graph,Operation::Parameter,type); auto* node = FindNode(graph,id);
                node->name = name; node->value = value;
            };
            for (auto* graph : {&spawnGraph,&updateGraph})
            {
                addParameter(*graph,"Emitter.EventPublish",ValueType::Float,{static_cast<float>(emitter.eventGenerationMask),0,0,0});
                addParameter(*graph,"Emitter.EventEmitterIndex",ValueType::Float,{static_cast<float>(emitterIndex),0,0,0});
                addParameter(*graph,"Emitter.EventBufferEnabled",ValueType::Float);
            }
            if (!emitter.eventHandlers.empty())
            {
                for (auto name : {"Emitter.EventActive","Emitter.EventKind","Emitter.EventParticleId","Emitter.EventSourceIndex",
                    "Emitter.EventOverrideVelocity","Emitter.EventMode","Emitter.EventPerEvent","Emitter.EventHandlerIndex",
                    "Emitter.EventExecutionMode","Emitter.EventMaxEvents","Emitter.EventRandomAge","Emitter.EventEmitterCount"})
                    addParameter(spawnGraph,name,ValueType::Float);
                for (auto name : {"Emitter.EventPosition","Emitter.EventVelocity","Emitter.EventNormal"})
                    addParameter(spawnGraph,name,ValueType::Float3);
                for (const auto& event : eventParticles)
                    for (const auto& parameter : event.compiled.program.parameters.entries)
                        addParameter(spawnGraph, parameter.name, Typed::ToValueType(parameter.type), parameter.defaultValue.scalar);
                for (const auto& node : emitter.update.nodes)
                    if (node.operation == Operation::Parameter) addParameter(spawnGraph,node.name,node.type,node.value);
                for (const auto& handler : emitter.eventHandlers)
                    for (const auto& node : handler.graph.nodes)
                        if (node.operation == Operation::Parameter) addParameter(spawnGraph,node.name,node.type,node.value);
            }
        }
        auto spawn = CompileGraph(spawnGraph,emitter.attributes), update = CompileGraph(updateGraph,emitter.attributes);
        auto emitterSpawn = CompileGraph(emitter.emitterSpawn, emitter.emitterAttributes);
        auto emitterUpdate = CompileGraph(emitter.emitterUpdate, emitter.emitterAttributes);
        std::vector<CompileResult> emitterEvents(emitter.eventHandlers.size());
        for (size_t handlerIndex = 0; handlerIndex < emitter.eventHandlers.size(); ++handlerIndex)
            if (!emitter.eventHandlers[handlerIndex].graph.nodes.empty())
                emitterEvents[handlerIndex] = CompileGraph(ExtractEventGraph(emitter.eventHandlers[handlerIndex].graph,false),emitter.emitterAttributes);
        CompileResult emitterCollision;
        if (!emitter.emitterCollision.nodes.empty()) emitterCollision = CompileGraph(emitter.emitterCollision,emitter.emitterAttributes);
        diagnostics.insert(diagnostics.end(), spawn.diagnostics.begin(), spawn.diagnostics.end());
        diagnostics.insert(diagnostics.end(),update.diagnostics.begin(),update.diagnostics.end());
        diagnostics.insert(diagnostics.end(), emitterSpawn.diagnostics.begin(), emitterSpawn.diagnostics.end());
        diagnostics.insert(diagnostics.end(),emitterUpdate.diagnostics.begin(),emitterUpdate.diagnostics.end());
        for (const auto& event : emitterEvents)
            diagnostics.insert(diagnostics.end(),event.diagnostics.begin(),event.diagnostics.end());
        for (const auto& event : eventParticles)
            diagnostics.insert(diagnostics.end(),event.diagnostics.begin(),event.diagnostics.end());
        diagnostics.insert(diagnostics.end(),emitterCollision.diagnostics.begin(),emitterCollision.diagnostics.end());
        bool eventsSucceeded = true;
        for (const auto& event : emitterEvents) eventsSucceeded &= event.Succeeded();
        for (const auto& event : eventParticles) eventsSucceeded &= event.Succeeded();
        if (spawn.Succeeded() && update.Succeeded() && emitterUpdate.Succeeded() && eventsSucceeded &&
            emitterCollision.Succeeded() && emitter.target == SimulationTarget::CPU)
        {
            std::vector<Program> eventPrograms, particlePrograms;
            for (const auto& event : emitterEvents) eventPrograms.push_back(event.compiled.program);
            for (const auto& event : eventParticles) particlePrograms.push_back(event.compiled.program);
            CpuEmitter validator;
            std::string error;
            if (!validator.Initialize(emitter, spawn.compiled.program, update.compiled.program, error, {}, emitterUpdate.compiled.program, emitterCollision.compiled.program, eventPrograms, particlePrograms, emitterSpawn.compiled.program))
                diagnostics.push_back({std::move(error),emitter.update.id,0});
        }
        EmitterPrograms programs;
        programs.spawn = std::move(spawn.compiled.program);
        programs.update = std::move(update.compiled.program);
        programs.emitterSpawn = std::move(emitterSpawn.compiled.program);
        programs.emitterUpdate = std::move(emitterUpdate.compiled.program);
        for (auto& event : emitterEvents) programs.eventHandlers.push_back(std::move(event.compiled.program));
        if (emitter.target == SimulationTarget::CPU)
            for (auto& event : eventParticles) programs.eventParticles.push_back(std::move(event.compiled.program));
        programs.emitterCollision = std::move(emitterCollision.compiled.program);
        programs.spawnHlsl = std::move(spawn.compiled.hlsl);
        programs.updateHlsl = std::move(update.compiled.hlsl);
        if (emitter.target == SimulationTarget::GPU)
        {
            std::string typedSource, typedError;
            if (emitter.eventHandlers.empty())
                programs.spawnHlsl += "\n[numthreads(1,1,1)] void ReceiveEvents(uint3 dispatchId : SV_DispatchThreadID) {}\n";
            if (!emitter.eventHandlers.empty())
            {
                std::string events;
                if (!BuildGpuEventShader(emitter,eventParticleGraphs,spawn.compiled.parameters,events,typedError))
                    diagnostics.push_back({typedError,emitter.spawn.id,0});
                else programs.spawnHlsl += "\nvoid VfxAdvance(uint index,float deltaTime);\n"+events;
                // ReceiveEvents uses both typed evaluators; give the update
                // function a distinct name and bind its parameters by name.
                Typed::TypeRegistry types; Typed::Graph typed; Typed::OptimizationResult layout; Typed::OptimizationStats stats;
                Typed::HlslGenerationOptions options; options.declarations = false; options.tickExpression = "VfxPadding";
                if ((!emitter.update.nodes.empty() && (!Typed::TranslateGraph(emitter.update,types,emitter.attributes,typed,typedError) ||
                    !Typed::OptimizeGraph(typed,types,layout,stats,typedError))) ||
                    !Typed::GenerateTypedHlsl(typed,layout,types,emitter.attributes,typedSource,typedError,spawn.compiled.parameters,"VfxEventAgeUpdate",options))
                    diagnostics.push_back({typedError,emitter.update.id,0});
                else
                {
                    uint32_t age = 0; for (const auto& attribute : emitter.attributes) { if (attribute.name == "Particles.Age") break; age += ComponentCount(attribute.type); }
                    programs.spawnHlsl += typedSource+"\nvoid VfxAdvance(uint index,float deltaTime) { VfxAttributes["+std::to_string(age)+
                        "u*VfxCapacity+index] = asuint(asfloat(VfxAttributes["+std::to_string(age)+"u*VfxCapacity+index])+deltaTime); VfxEventAgeUpdate(index,0,deltaTime); }\n";
                }
            }
        }
        candidate.emitters.push_back(std::move(programs));
        uint32_t iterations = 0;
        if (emitter.simulationStages.size() > 64) diagnostics.push_back({"An emitter supports at most 64 Simulation Stages",emitter.update.id,0});
        for (const auto& stage : emitter.simulationStages)
        {
            if (stage.iterations > 128 || (iterations += stage.iterations) > 1024 || stage.graph.stage != ScriptStage::SimulationStage)
                diagnostics.push_back({"Invalid Simulation Stage iteration count or graph stage",stage.graph.id,0});
            if (!ValidateSimulationStageBindings(stage.bindings,settingsError)) diagnostics.push_back({settingsError,stage.graph.id,0});
            auto compiled = CompileGraph(stage.graph,emitter.attributes);
            diagnostics.insert(diagnostics.end(),compiled.diagnostics.begin(),compiled.diagnostics.end());
            candidate.emitters.back().stages.push_back({std::move(compiled.compiled.program), std::move(compiled.compiled.hlsl)});
        }
        if (emitter.target == SimulationTarget::GPU)
        {
            std::string error;
            if (!BuildGpuCompactionShader(emitter.attributes,candidate.emitters.back().compactionHlsl,error))
                diagnostics.push_back({std::move(error),emitter.update.id,0});
        }
    }
    if (!diagnostics.empty()) return false;
    candidate.systemSpawn = systemSpawn.compiled.program;
    candidate.systemUpdate = systemUpdate.compiled.program;
    Detail::ParameterTypes types; std::string parameterError;
    if (!Detail::AddParameterTypes(types,asset.parameters,parameterError)) diagnostics.push_back({parameterError,{},0});
    if (!Detail::AddParameterTypes(types,candidate.systemSpawn,parameterError) ||
        !Detail::AddParameterTypes(types,candidate.systemUpdate,parameterError))
        diagnostics.push_back({parameterError,{},{}});
    for (size_t i = 0; i < candidate.emitters.size(); ++i)
    {
        const auto& program = candidate.emitters[i];
        bool parametersOk =
            Detail::AddParameterTypes(types,program.spawn,parameterError) && Detail::AddParameterTypes(types,program.update,parameterError) &&
            Detail::AddParameterTypes(types,program.emitterSpawn,parameterError) && Detail::AddParameterTypes(types,program.emitterUpdate,parameterError) &&
            Detail::AddParameterTypes(types,program.emitterCollision,parameterError);
        for (const auto& event : program.eventHandlers) parametersOk &= Detail::AddParameterTypes(types,event,parameterError);
        for (const auto& event : program.eventParticles) parametersOk &= Detail::AddParameterTypes(types,event,parameterError);
        if (!parametersOk)
            diagnostics.push_back({parameterError,asset.emitters[i].update.id,0});
        for (const auto& stage : program.stages) if (!Detail::AddParameterTypes(types,stage.program,parameterError)) diagnostics.push_back({parameterError,{},0});
        for (const auto& stage : asset.emitters[i].simulationStages) if (!Detail::AddStageControlTypes(types,stage.bindings,parameterError)) diagnostics.push_back({parameterError,stage.graph.id,0});
    }
    if (!diagnostics.empty()) return false;
    candidate.systemSpawn = std::move(systemSpawn.compiled.program);
    candidate.systemUpdate = std::move(systemUpdate.compiled.program);
    std::vector<DataInterfaceDefinition> interfaces;std::string interfaceError;
    if(!CollectCompiledDataInterfaces(candidate,interfaces,interfaceError)) {diagnostics.push_back({interfaceError});return false;}
    output = std::move(candidate);
    return true;
}

bool BuildGpuBoundsShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const MeshSettings* mesh)
{
    if (!BuildSpriteShader(attributes,source,error,{},mesh)) return false;
    ParticleDataSet schema; schema.Initialize(attributes,1);
    const auto position = schema.FindAttribute("Particles.Position"), size = schema.FindAttribute("Particles.Size");
    const auto sizeExpression = size < 0 ? "float2(0.05,0.05)" : attributes[size].type == ValueType::Float
        ? "float2("+AttributeExpression(attributes,size)+","+AttributeExpression(attributes,size)+")" : AttributeExpression(attributes,size);
    std::ostringstream shader;
    shader << R"(cbuffer VfxBoundsConstants : register(b0,space1) { uint VfxCapacity; uint VfxBoundsGroups; uint2 VfxBoundsPadding; float4 VfxMeshBounds[8]; uint VfxMeshCount; uint3 VfxBoundsMeshPadding; };
struct Bounds { float4 minimum; float4 maximum; float radius; uint count; uint invalid; uint padding; };
StructuredBuffer<float> VfxAttributes : register(t0,space1);
StructuredBuffer<uint> VfxCounters : register(t1,space1);
StructuredBuffer<Bounds> VfxPartialBounds : register(t2,space1);
RWStructuredBuffer<Bounds> VfxBoundsOutput : register(u0,space1);
)";
    if (mesh) { std::string transform; if (!MeshTransformSource(attributes,*mesh,transform,error)) return false; shader << transform; }
    shader << R"(
groupshared float4 VfxMin[256], VfxMax[256];
groupshared uint2 VfxCounts[256];
void ReduceTile(uint lane)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint distance = 128; distance != 0; distance >>= 1)
    {
        if (lane < distance)
        {
            VfxMin[lane] = min(VfxMin[lane],VfxMin[lane+distance]);
            VfxMax[lane] = max(VfxMax[lane],VfxMax[lane+distance]);
            VfxCounts[lane] += VfxCounts[lane+distance];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
Bounds TileBounds()
{
    Bounds result;
    bool valid = VfxCounts[0].x != VfxCounts[0].y;
    result.minimum = valid ? float4(VfxMin[0].xyz,0) : 0;
    result.maximum = valid ? float4(VfxMax[0].xyz,0) : 0;
    result.radius = valid ? VfxMax[0].w : 0;
    result.count = VfxCounts[0].x; result.invalid = VfxCounts[0].y; result.padding = 0;
    return result;
}
[numthreads(256,1,1)]
void BoundsReduce(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint groupIndex = group.y*65535+group.x;
    uint index = groupIndex*256+lane;
    VfxMin[lane] = 3.402823466e+38f; VfxMax[lane] = float4(-3.402823466e+38f,-3.402823466e+38f,-3.402823466e+38f,0);
    VfxCounts[lane] = 0;
    if (index < min(VfxCounters[0],VfxCapacity))
    {
)";
    if (mesh)
        shader << R"(        float3 center; float radius;
        VfxCounts[lane].x = 1;
        uint slot = VfxMeshSlotRaw(index);
        if (slot < VfxMeshCount && VfxMeshSphere(index,VfxMeshBounds[min(slot,7u)],center,radius))
        { VfxMin[lane] = float4(center-radius,0); VfxMax[lane] = float4(center+radius,0); }
        else VfxCounts[lane].y = 1;
)";
    else {
    shader << "        float3 position = " << AttributeExpression(attributes,position) << ";\n"
           << "        float2 size = abs(" << sizeExpression << ")*0.5;\n";
    shader << R"(        VfxCounts[lane].x = 1;
        if (all(isfinite(position)) && all(isfinite(size)))
        {
            float scale = max(size.x,size.y);
            float radius = scale > 0 ? scale*length(size/scale) : 0;
            VfxMin[lane] = float4(position,0); VfxMax[lane] = float4(position,radius);
        }
        else VfxCounts[lane].y = 1;
)";
    }
    shader << R"(    }
    ReduceTile(lane);
    if (lane == 0) VfxBoundsOutput[groupIndex] = TileBounds();
}
[numthreads(256,1,1)]
void BoundsFinish(uint lane : SV_GroupIndex)
{
    float4 lower = 3.402823466e+38f, upper = float4(-3.402823466e+38f,-3.402823466e+38f,-3.402823466e+38f,0);
    uint2 counts = 0;
    for (uint index = lane; index < VfxBoundsGroups; index += 256)
    {
        Bounds partial = VfxPartialBounds[index];
        counts += uint2(partial.count,partial.invalid);
        if (partial.count != partial.invalid)
        { lower = min(lower,partial.minimum); upper = max(upper,float4(partial.maximum.xyz,partial.radius)); }
    }
    VfxMin[lane] = lower; VfxMax[lane] = upper; VfxCounts[lane] = counts;
    ReduceTile(lane);
    if (lane == 0) VfxBoundsOutput[0] = TileBounds();
}
)";
    source = shader.str(); error.clear(); return true;
}

bool BuildGpuVisibilityShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const MeshSettings* mesh)
{
    if (!BuildSpriteShader(attributes,source,error,{},mesh)) return false;
    ParticleDataSet schema; schema.Initialize(attributes,1);
    const auto position = schema.FindAttribute("Particles.Position"), size = schema.FindAttribute("Particles.Size");
    const auto sizeExpression = size < 0 ? "float2(0.05,0.05)" : attributes[size].type == ValueType::Float
        ? "float2("+AttributeExpression(attributes,size)+","+AttributeExpression(attributes,size)+")" : AttributeExpression(attributes,size);
    std::ostringstream shader;
    shader << R"(cbuffer VfxVisibilityView : register(b0,space1)
{
    row_major float4x4 VfxViewProjection; row_major float4x4 VfxLocalToWorld;
    float4 VfxCameraRight, VfxCameraUp, VfxCameraForward;
    uint VfxCapacity; uint VfxVisibilityCount; uint VfxVisibilityGroups; uint VfxCull;
    float4 VfxMeshBounds[8];
    uint4 VfxIndexCounts[2];
    uint VfxIndexCount; uint VfxMeshCount; uint2 VfxDrawPadding;
};
struct Bounds { float4 minimum; float4 maximum; float radius; uint count; uint invalid; uint padding; };
StructuredBuffer<float> VfxAttributes : register(t0,space1);
StructuredBuffer<uint2> VfxIds : register(t1,space1);
StructuredBuffer<uint> VfxCounters : register(t2,space1);
StructuredBuffer<Bounds> VfxBounds : register(t3,space1);
RWStructuredBuffer<uint4> VfxVisibleScratch : register(u0,space1);
RWStructuredBuffer<uint4> VfxVisibilityGroupsData : register(u1,space1);
RWStructuredBuffer<uint4> VfxVisibleKeys : register(u2,space1);
RWStructuredBuffer<uint> VfxViewCounters : register(u3,space1);
)";
    if (mesh) { std::string transform; if (!MeshTransformSource(attributes,*mesh,transform,error)) return false; shader << transform; }
    shader << R"(
groupshared uint2 VfxPrefix[256];
groupshared uint2 VfxCarry;
groupshared uint VfxEmitterVisible;
bool BoxVisible(float3 center, float3 extent)
{
    for (uint plane = 0; plane < 6; ++plane)
    {
        float4 equation = plane == 4 ? VfxViewProjection[2] : VfxViewProjection[3] +
            (plane%2 ? -1.0 : 1.0)*VfxViewProjection[plane/2];
        float distance = dot(equation.xyz,center)+equation.w;
        float radius = dot(abs(equation.xyz),extent);
        if (distance+radius < -1e-5*(abs(distance)+radius+1)) return false;
    }
    return true;
}
bool EmitterVisible()
{
    Bounds bounds = VfxBounds[0];
    if (bounds.count == bounds.invalid) return false;
    float3 center = bounds.minimum.xyz*0.5+bounds.maximum.xyz*0.5;
    float3 extent = bounds.maximum.xyz*0.5-bounds.minimum.xyz*0.5;
    float3 world = mul(VfxLocalToWorld,float4(center,1)).xyz;
    float3 radius = mul(abs((float3x3)VfxLocalToWorld),extent) +
        bounds.radius*(abs(VfxCameraRight.xyz)+abs(VfxCameraUp.xyz));
    return !all(isfinite(world)) || !all(isfinite(radius)) || BoxVisible(world,radius);
}
void PrefixTile(uint lane)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint distance = 1; distance < 256; distance <<= 1)
    {
        uint2 add = lane >= distance ? VfxPrefix[lane-distance] : 0;
        GroupMemoryBarrierWithGroupSync();
        VfxPrefix[lane] += add;
        GroupMemoryBarrierWithGroupSync();
    }
}
[numthreads(256,1,1)]
void CullSprites(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint groupIndex = group.y*65535+group.x, index = groupIndex*256+lane;
    if (index >= VfxVisibilityCount) return;
    if (lane == 0) VfxEmitterVisible = VfxCull == 0 || EmitterVisible();
    GroupMemoryBarrierWithGroupSync();
    uint visible = 0, invalid = 0;
    uint4 key = 0xffffffff;
    if (index < min(VfxCounters[0],VfxCapacity))
    {
)";
    if (mesh)
        shader << R"(        float3 position; float radius;
        uint slot = VfxMeshSlotRaw(index);
        bool valid = slot < VfxMeshCount && VfxMeshSphere(index,VfxMeshBounds[min(slot,7u)],position,radius);
        float3 world = mul(VfxLocalToWorld,float4(position,1)).xyz;
        float3 extent = mul(abs((float3x3)VfxLocalToWorld),float3(radius,radius,radius));
        if (valid && all(isfinite(world)) && all(isfinite(extent)))
)";
    else {
    shader << "        float3 position = " << AttributeExpression(attributes,position) << ";\n"
           << "        float2 size = abs(" << sizeExpression << ")*0.5;\n";
    shader << R"(        float3 world = mul(VfxLocalToWorld,float4(position,1)).xyz;
        float3 extent = (abs(VfxCameraRight.xyz)+abs(VfxCameraUp.xyz))*length(size);
        if (all(isfinite(position)) && all(isfinite(size)) && all(isfinite(world)) && all(isfinite(extent)))
)";
    }
    shader << R"(        {
            visible = VfxEmitterVisible != 0 && (VfxCull == 0 || BoxVisible(world,extent));
            if (visible != 0)
            {
)";
    // Sorting is defined at particle position, independent of an off-center mesh.
    shader << "                float3 sortPosition = mul(VfxLocalToWorld,float4(" << AttributeExpression(attributes,position) << ",1)).xyz;\n";
    shader << R"(                float depth = dot(sortPosition,VfxCameraForward.xyz);
                if (!isfinite(depth)) depth = -3.402823466e+38f;
                uint bits = depth == 0 ? 0 : asuint(depth);
                uint ordered = bits ^ ((bits & 0x80000000) != 0 ? 0xffffffff : 0x80000000);
                uint2 id = VfxIds[index]; key = uint4(~ordered,id.y,id.x,index);
            }
        }
        else invalid = 1;
    }
    VfxVisibleKeys[index] = 0xffffffff;
    VfxPrefix[lane] = uint2(visible,invalid);
    PrefixTile(lane);
    VfxVisibleScratch[index] = uint4(key.xyz,visible != 0 ? VfxPrefix[lane].x-1 : 0xffffffff);
    if (lane == 255) VfxVisibilityGroupsData[groupIndex] = uint4(VfxPrefix[lane],0,0);
}
[numthreads(256,1,1)]
void ScanVisibility(uint lane : SV_GroupIndex)
{
    if (lane == 0) VfxCarry = 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint first = 0; first < VfxVisibilityGroups; first += 256)
    {
        uint index = first+lane;
        uint2 count = index < VfxVisibilityGroups ? VfxVisibilityGroupsData[index].xy : 0;
        VfxPrefix[lane] = count;
        PrefixTile(lane);
        if (index < VfxVisibilityGroups) VfxVisibilityGroupsData[index] = uint4(VfxCarry+VfxPrefix[lane]-count,count);
        GroupMemoryBarrierWithGroupSync();
        if (lane == 0) VfxCarry += VfxPrefix[255];
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0)
    {
        uint alive = min(VfxCounters[0],VfxCapacity);
        VfxViewCounters[0] = alive; VfxViewCounters[1] = VfxCarry.x;
        VfxViewCounters[2] = alive-VfxCarry.x-VfxCarry.y; VfxViewCounters[3] = VfxCarry.y;
        VfxViewCounters[4] = VfxIndexCount != 0 ? VfxIndexCount : 6;
        VfxViewCounters[5] = VfxCarry.x; VfxViewCounters[6] = 0; VfxViewCounters[7] = 0; VfxViewCounters[8] = 0;
        if (VfxMeshCount > 0) VfxViewCounters[4] = VfxIndexCounts[0].x;
        for (uint slot = 0; slot < VfxMeshCount; ++slot)
        {
            uint base = 12u+5u*slot;
            VfxViewCounters[base+0] = VfxIndexCounts[slot/4u][slot%4u];
            VfxViewCounters[base+1] = VfxCarry.x;
            VfxViewCounters[base+2] = 0u; VfxViewCounters[base+3] = 0u; VfxViewCounters[base+4] = 0u;
        }
    }
}
[numthreads(256,1,1)]
void ScatterVisibility(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint groupIndex = group.y*65535+group.x, index = groupIndex*256+lane;
    if (index >= VfxVisibilityCount) return;
    uint4 key = VfxVisibleScratch[index];
    if (key.w != 0xffffffff)
    {
        uint destination = VfxVisibilityGroupsData[groupIndex].x+key.w;
        VfxVisibleKeys[destination] = uint4(key.xyz,index);
    }
}
)";
    source = shader.str(); error.clear(); return true;
}

bool BuildGpuSortShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error)
{
    ParticleDataSet schema;
    try { schema.Initialize(attributes,1); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    const auto position = schema.FindAttribute("Particles.Position");
    if (position < 0 || attributes[position].type != ValueType::Float3)
    { error = "View depth sorting requires float3 Position"; return false; }
    std::ostringstream shader;
    shader << R"(cbuffer VfxSortView : register(b0, space1)
{
    row_major float4x4 VfxLocalToWorld;
    float4 VfxCameraForward;
    uint VfxCapacity; uint VfxSortCount; uint VfxSortSequence; uint VfxSortDistance;
};
StructuredBuffer<float> VfxAttributes : register(t0, space1);
StructuredBuffer<uint2> VfxIds : register(t1, space1);
StructuredBuffer<uint> VfxCounters : register(t2, space1);
RWStructuredBuffer<uint4> VfxSortKeys : register(u0, space1);
groupshared uint4 VfxTile[256];
bool Less(uint4 a, uint4 b)
{
    return a.x < b.x || (a.x == b.x && (a.y < b.y || (a.y == b.y &&
        (a.z < b.z || (a.z == b.z && a.w < b.w)))));
}
void SortTile(uint lane, uint index, uint sequence, uint distance)
{
    for (uint step = distance; step != 0; step >>= 1)
    {
        uint4 a = VfxTile[lane], b = VfxTile[lane ^ step];
        bool ascending = (index & sequence) == 0;
        bool lower = (lane & step) == 0;
        uint4 result = (Less(b,a) == (ascending == lower)) ? b : a;
        GroupMemoryBarrierWithGroupSync();
        VfxTile[lane] = result;
        GroupMemoryBarrierWithGroupSync();
    }
}
[numthreads(256,1,1)]
void SortInitialize(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint index = (group.y*65535+group.x)*256+lane;
    if (index >= VfxSortCount) return;
    uint4 key = uint4(0xffffffff,0xffffffff,0xffffffff,0xffffffff);
    if (VfxSortSequence != 0) key = VfxSortKeys[index];
    else if (index < min(VfxCounters[0],VfxCapacity))
    {
)";
    shader << "        float3 world = mul(VfxLocalToWorld,float4(" << AttributeExpression(attributes,position) << ",1)).xyz;\n";
    shader << R"(        float depth = dot(world,VfxCameraForward.xyz);
        if (!isfinite(depth)) depth = -3.402823466e+38f;
        uint bits = depth == 0 ? 0 : asuint(depth);
        uint ordered = bits ^ ((bits & 0x80000000) != 0 ? 0xffffffff : 0x80000000);
        uint2 id = VfxIds[index];
        key = uint4(~ordered,id.y,id.x,index);
    }
    VfxTile[lane] = key;
    GroupMemoryBarrierWithGroupSync();
    for (uint sequence = 2; sequence <= 256; sequence <<= 1)
        SortTile(lane,index,sequence,sequence >> 1);
    VfxSortKeys[index] = VfxTile[lane];
}
[numthreads(256,1,1)]
void SortStep(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint index = (group.y*65535+group.x)*256+lane;
    if (index >= VfxSortCount) return;
    if (VfxSortDistance < 256)
    {
        VfxTile[lane] = VfxSortKeys[index];
        GroupMemoryBarrierWithGroupSync();
        SortTile(lane,index,VfxSortSequence,VfxSortDistance);
        VfxSortKeys[index] = VfxTile[lane];
    }
    else
    {
        uint partner = index ^ VfxSortDistance;
        if (partner > index)
        {
            uint4 a = VfxSortKeys[index], b = VfxSortKeys[partner];
            if (Less(b,a) == ((index & VfxSortSequence) == 0))
            { VfxSortKeys[index] = b; VfxSortKeys[partner] = a; }
        }
    }
}
)";
    source = shader.str(); error.clear(); return true;
}

bool BuildMeshShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const MeshSettings& mesh, const SpriteSettings& material)
{ return BuildSpriteShader(attributes,source,error,material,&mesh); }

/**
 * Ribbon strip shader.
 *
 * A ribbon connects consecutive particles in draw order into a camera-facing
 * strip. Each particle owns six triangle-list vertices (two triangles forming
 * one quad) so the indirect draw arguments keep the same shape as the sprite
 * path: vertexCount = particles * 6, instanceCount = 1. The segment for
 * particle i spans particles i and i+1; the final particle repeats itself and
 * therefore produces a degenerate quad that rasterises nothing.
 *
 * Width comes from a scalar attribute so a module can animate it over life.
 */
bool BuildRibbonShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error,
    const RibbonSettings& settings, const SpriteSettings& material)
{
    ParticleDataSet schema;
    try { schema.Initialize(attributes,1); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    if (!ValidateRibbonSettings(settings,error)) return false;
    const auto position = schema.FindAttribute("Particles.Position");
    const auto color = schema.FindAttribute("Particles.Color");
    const auto width = settings.widthAttribute.empty() ? -1 : schema.FindAttribute(settings.widthAttribute);
    const auto frame = schema.FindAttribute("Particles.SubImageIndex");
    const auto age = schema.FindAttribute("Particles.Age"), lifetime = schema.FindAttribute("Particles.Lifetime");
    if (position < 0 || attributes[position].type != ValueType::Float3 ||
        (color >= 0 && attributes[color].type != ValueType::Float4) ||
        (width >= 0 && attributes[width].type != ValueType::Float) ||
        (frame >= 0 && attributes[frame].type != ValueType::Float) ||
        (age >= 0 && attributes[age].type != ValueType::Float) ||
        (lifetime >= 0 && attributes[lifetime].type != ValueType::Float))
    { error = "Ribbon rendering requires float3 Position, optional float4 Color and scalar Width/SubImageIndex/Age/Lifetime"; return false; }
    std::array<int32_t,4> dynamicAttributes;
    for (size_t i = 0; i < dynamicAttributes.size(); ++i)
    {
        dynamicAttributes[i] = material.dynamicParameters[i].attribute.empty() ? -1
            : schema.FindAttribute(material.dynamicParameters[i].attribute);
        if (dynamicAttributes[i] >= 0 && attributes[dynamicAttributes[i]].type != ValueType::Float4)
        { error = "Ribbon dynamic material parameters require float4 attributes"; return false; }
    }

    const auto widthSource = [&](const char* index)
    { return width < 0 ? std::string{"0.05"} : AttributeExpressionAt(attributes,static_cast<uint32_t>(width),index); };
    const auto componentSource = [&](int32_t attribute, const char* index, const char* fallback)
    { return attribute < 0 ? std::string{fallback} : AttributeExpressionAt(attributes,static_cast<uint32_t>(attribute),index); };

    std::ostringstream shader;
    shader << "#define NLS_PARTICLE_DEFAULT_MATERIAL\n" << kParticleSpriteInterface;
    shader << R"(StructuredBuffer<float> VfxAttributes : register(t0, space1);
StructuredBuffer<uint4> VfxSortKeys : register(t1, space1);
SpriteVertex RibbonVS(uint vertex : SV_VertexID, uint index : SV_InstanceID)
{
    SpriteVertex output;
    if (VfxSorted != 0) index = VfxSortKeys[index].w;
    uint base = index*6u;
    uint corner = vertex-base;
    uint first = index, second = index+1u;
)";
    shader << "    if (VfxSorted != 0) { first = VfxSortKeys[first].w; second = VfxSortKeys[min(second,VfxCapacity-1u)].w; }\n"
           << "    second = min(second,VfxCapacity-1u);\n";
    shader << "    float3 startPosition = mul(VfxLocalToWorld,float4(" << AttributeExpressionAt(attributes,static_cast<uint32_t>(position),"first") << ",1)).xyz;\n"
           << "    float3 endPosition = mul(VfxLocalToWorld,float4(" << AttributeExpressionAt(attributes,static_cast<uint32_t>(position),"second") << ",1)).xyz;\n"
           << "    float startWidth = " << widthSource("first") << ";\n"
           << "    float endWidth = " << widthSource("second") << ";\n";
    shader << R"(    float3 axis = endPosition-startPosition;
    float axisLengthSquared = dot(axis,axis);
    bool degenerate = !all(isfinite(startPosition)) || !all(isfinite(endPosition)) ||
        !isfinite(startWidth) || !isfinite(endWidth) || axisLengthSquared <= 1e-12;
    if (degenerate)
    {
        output.position = float4(0,0,-1,1);
        output.worldPosition = 0; output.uv = 0; output.color = 0; output.frame = 0;
        output.dynamicParameter0 = 0; output.dynamicParameter1 = 0; output.dynamicParameter2 = 0; output.dynamicParameter3 = 0;
        output.ageLifetime = 0; output.velocity = 0;
        return output;
    }
    axis *= rsqrt(axisLengthSquared);
    // The strip faces the camera: the side vector is perpendicular to both the
    // segment direction and the view ray through the segment mid-point.
    // The view direction comes from the camera basis the interface already
    // provides, so the strip faces the viewer without needing a camera position.
    float3 view = -cross(VfxCameraRight.xyz,VfxCameraUp.xyz);
    float3 side = cross(axis,view);
    float sideLengthSquared = dot(side,side);
    if (!all(isfinite(side)) || sideLengthSquared <= 1e-12) side = VfxCameraRight.xyz;
    else side *= rsqrt(sideLengthSquared);
    // Six vertices: two triangles spanning the segment quad.
    const float2 corners[6] = {float2(0,-1),float2(0,1),float2(1,1),float2(0,-1),float2(1,1),float2(1,-1)};
    float2 corner2 = corners[corner%6];
    float3 world = lerp(startPosition+side*0.5*startWidth,endPosition+side*0.5*endWidth,corner2.x)
        + side*(corner2.y*0.5*lerp(startWidth,endWidth,corner2.x));
)";
    shader << "    output.worldPosition = world;\n"
           << "    output.position = mul(VfxViewProjection,float4(world,1));\n"
           << "    float u = corner2.x;\n"
           << "    output.uv = float2(u,corner2.y*0.5+0.5);\n"
           << "    output.color = " << componentSource(color,"first","float4(1,1,1,1)") << ";\n"
           << "    output.frame = " << componentSource(frame,"first","0") << ";\n";
    for (size_t i = 0; i < dynamicAttributes.size(); ++i)
        shader << "    output.dynamicParameter" << i << " = "
               << (dynamicAttributes[i] < 0 ? "VfxDynamicDefaults["+std::to_string(i)+"]"
                   : AttributeExpressionAt(attributes,static_cast<uint32_t>(dynamicAttributes[i]),"first")) << ";\n";
    shader << "    output.ageLifetime = float2(" << componentSource(age,"first","0") << ","
           << componentSource(lifetime,"first","1") << ");\n"
           << "    output.velocity = 0;\n"
           << "    return output;\n}\n"
           << "float4 RibbonPS(SpriteVertex input) : SV_Target0\n{\n    return ParticleMaterialPS(input);\n}\n";
    source = shader.str(); error.clear(); return true;
}

bool BuildSpriteShader(const std::vector<Attribute>& attributes, std::string& source, std::string& error, const SpriteSettings& settings, const MeshSettings* mesh)
{
    ParticleDataSet schema;
    try { schema.Initialize(attributes,1); }
    catch (const std::invalid_argument& exception) { error = exception.what(); return false; }
    const auto position = schema.FindAttribute("Particles.Position"), color = schema.FindAttribute("Particles.Color"), size = schema.FindAttribute("Particles.Size");
    const auto rotation = schema.FindAttribute("Particles.Rotation"), frame = schema.FindAttribute("Particles.SubImageIndex"), velocity = schema.FindAttribute("Particles.Velocity");
    // Optional per-particle orientation channels. They let modules choose the
    // facing vector per particle (mesh orientation, collision plane, custom
    // alignment) instead of being limited to the renderer-wide enum.
    const auto spriteFacing = schema.FindAttribute("Particles.SpriteFacing");
    const auto spriteAlignment = schema.FindAttribute("Particles.SpriteAlignment");
    if (position < 0 || attributes[position].type != ValueType::Float3 ||
        (color >= 0 && attributes[color].type != ValueType::Float4) ||
        (!mesh && size >= 0 && attributes[size].type != ValueType::Float && attributes[size].type != ValueType::Float2) ||
        (!mesh && rotation >= 0 && attributes[rotation].type != ValueType::Float) || (frame >= 0 && attributes[frame].type != ValueType::Float) ||
        (velocity >= 0 && attributes[velocity].type != ValueType::Float3) ||
        (spriteFacing >= 0 && attributes[spriteFacing].type != ValueType::Float3) ||
        (spriteAlignment >= 0 && attributes[spriteAlignment].type != ValueType::Float3))
    { error = "Sprite rendering requires float3 Position/Velocity, float4 Color, scalar/float2 Size, scalar Rotation/SubImageIndex and float3 Facing/Alignment"; return false; }
    std::array<int32_t,4> dynamicAttributes;
    for (size_t i = 0; i < dynamicAttributes.size(); ++i)
    {
        dynamicAttributes[i] = settings.dynamicParameters[i].attribute.empty() ? -1 : schema.FindAttribute(settings.dynamicParameters[i].attribute);
        if (dynamicAttributes[i] >= 0 && attributes[dynamicAttributes[i]].type != ValueType::Float4)
        { error = "Dynamic material parameters require float4 attributes"; return false; }
    }
    const auto age = schema.FindAttribute("Particles.Age"), lifetime = schema.FindAttribute("Particles.Lifetime");
    if ((age >= 0 && attributes[age].type != ValueType::Float) || (lifetime >= 0 && attributes[lifetime].type != ValueType::Float))
    { error = "Particle material Age and Lifetime must be scalar attributes"; return false; }
    std::ostringstream shader;
    shader << "#define NLS_PARTICLE_DEFAULT_MATERIAL\n" << kParticleSpriteInterface;
    shader << R"(StructuredBuffer<float> VfxAttributes : register(t0, space1);
StructuredBuffer<uint4> VfxSortKeys : register(t1, space1);
)";
    if (mesh)
    {
        std::string transform; if (!MeshTransformSource(attributes,*mesh,transform,error)) return false;
        shader << "#define NLS_VFX_MESH_SLOTS " << mesh->meshes.size() << "\n";
        shader << transform << R"(SpriteVertex MeshVS(float3 vertexPosition : POSITION, float2 vertexUv : TEXCOORD0, uint index : SV_InstanceID)
{
    SpriteVertex output;
    if (VfxSorted != 0) index = VfxSortKeys[index].w;
    if (NLS_VFX_MESH_SLOTS > 1 && VfxMeshSlotRaw(index) != (uint)VfxShading.w)
    {
        output.position = float4(0,0,-1,1);
        output.worldPosition = 0; output.uv = 0; output.color = 0; output.frame = 0;
        output.dynamicParameter0 = 0; output.dynamicParameter1 = 0; output.dynamicParameter2 = 0; output.dynamicParameter3 = 0;
        output.ageLifetime = 0; output.velocity = 0;
        return output;
    }
    float3 particlePosition, particleScale; float4 particleOrientation;
    VfxMeshTransform(index,particlePosition,particleScale,particleOrientation);
    float3 world = mul(VfxLocalToWorld,float4(particlePosition+VfxRotate(particleOrientation,vertexPosition*particleScale),1)).xyz;
    output.position = mul(VfxViewProjection,float4(world,1));
    output.worldPosition = world; output.uv = vertexUv;
)";
    }
    else {
    shader << R"(SpriteVertex SpriteVS(uint vertex : SV_VertexID, uint index : SV_InstanceID)
{
    const float2 corners[6] = {float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};
    SpriteVertex output;
    float2 corner = corners[vertex%6];
    if (VfxSorted != 0) index = VfxSortKeys[index].w;
)";
    shader << "    float3 center = mul(VfxLocalToWorld,float4(" << AttributeExpression(attributes,position) << ",1)).xyz;\n";
    shader << "    float2 size = " << (size < 0 ? "float2(0.05,0.05)" : attributes[size].type == ValueType::Float
        ? "float2("+AttributeExpression(attributes,size)+","+AttributeExpression(attributes,size)+")" : AttributeExpression(attributes,size)) << ";\n";
    shader << "    float rotation = " << (rotation < 0 ? "0" : AttributeExpression(attributes,rotation)) << ";\n";
    }
    shader << "    output.color = " << (color < 0 ? "float4(1,1,1,1)" : AttributeExpression(attributes,color)) << ";\n";
    shader << "    float3 velocity = " << (velocity < 0 ? "float3(0,0,0)" : AttributeExpression(attributes,velocity)) << ";\n";
    shader << "    float frame = " << (frame < 0 ? "0" : AttributeExpression(attributes,frame)) << ";\n";
    for (size_t i = 0; i < dynamicAttributes.size(); ++i)
        shader << "    output.dynamicParameter" << i << " = " << (dynamicAttributes[i] < 0 ? "VfxDynamicDefaults["+std::to_string(i)+"]" : AttributeExpression(attributes,dynamicAttributes[i])) << ";\n";
    shader << "    output.ageLifetime = float2(" << (age < 0 ? "0" : AttributeExpression(attributes,age)) << "," << (lifetime < 0 ? "1" : AttributeExpression(attributes,lifetime)) << ");\n";
    shader << R"(    if (!isfinite(frame)) frame = 0;
    output.frame = clamp(frame,0,(float)(VfxSubImagesX*VfxSubImagesY-1));
)";
    if (!mesh) {
    shader << R"(    if (!isfinite(rotation)) rotation = 0;
    float3 right = VfxCameraRight.xyz, up = VfxCameraUp.xyz;
    // A module may drive the facing vector and its alignment reference per
    // particle. Both are optional columns, so a system without them keeps the
    // previous renderer-wide VfxAlignment behaviour.
    float facingMode = VfxAlignment;
    float3 facingAxis = velocity;
    float3 alignmentAxis = float3(0,0,0);
)";
    // A declared but undriven facing column must not hijack orientation: a zero
    // vector keeps the renderer-wide VfxAlignment in charge, so a system that
    // only declares these optional columns still renders exactly as before.
    if (spriteFacing >= 0) shader << "    float3 facingValue = " << AttributeExpression(attributes,spriteFacing) << ";\n    if (dot(facingValue,facingValue) > 1e-12) { facingAxis = facingValue; facingMode = 1.0; }\n";
    if (spriteAlignment >= 0) shader << "    alignmentAxis = " << AttributeExpression(attributes,spriteAlignment) << ";\n";
    shader << R"(    if (facingMode == 1)
    {
        float3 worldFacing = mul((float3x3)VfxLocalToWorld,facingAxis);
        float3 projected = worldFacing;
        if (dot(alignmentAxis,alignmentAxis) > 1e-12)
        {
            float3 worldAlignment = normalize(mul((float3x3)VfxLocalToWorld,alignmentAxis));
            projected -= worldAlignment*dot(projected,worldAlignment);
        }
        float2 screen = float2(dot(projected,right),dot(projected,up));
        float lengthSquared = dot(screen,screen);
        if (all(isfinite(screen)) && lengthSquared > 1e-12)
        {
            screen *= rsqrt(lengthSquared);
            right = VfxCameraRight.xyz*screen.y-VfxCameraUp.xyz*screen.x;
            up = VfxCameraRight.xyz*screen.x+VfxCameraUp.xyz*screen.y;
        }
    }
    float sine,cosine; sincos(rotation,sine,cosine);
    float2 offset = corner*size;
    offset = float2(cosine*offset.x-sine*offset.y,sine*offset.x+cosine*offset.y);
    float3 world = center+0.5*(right*offset.x+up*offset.y);
    output.position = mul(VfxViewProjection,float4(world,1));
    output.worldPosition = world;
    output.uv = float2(corner.x*0.5+0.5,0.5-corner.y*0.5);
)";
    }
    shader << R"(    output.velocity = mul((float3x3)VfxLocalToWorld,velocity);
    return output;
}
float4 SpritePS(SpriteVertex input) : SV_Target0
{
    return ParticleMaterialPS(input);
}
)";
    source = shader.str(); error.clear(); return true;
}

CompileResult CompileGraph(const Graph& source, const std::vector<Attribute>& attributes)
{
    CompileResult result;
    Typed::TypeRegistry types;
    Typed::Graph graph;
    Typed::OptimizationResult layout;
    Typed::OptimizationStats stats;
    std::string error;
    if (!Typed::TranslateGraph(source, types, attributes, graph, error) || !Typed::OptimizeGraph(graph, types, layout, stats, error) || !Typed::CompileToBytecode(graph, types, layout, result.compiled.program, error))
    {
        result.diagnostics.push_back({error.empty() ? "Typed compiler rejected a graph" : error, source.id, 0});
        return result;
    }
    for (const auto& parameter : layout.parameters.entries)
    {
        result.compiled.parameters.push_back(parameter.name);
        result.compiled.parameterDefaults.push_back(parameter.defaultValue.scalar);
    }
    const bool particle = source.stage == ScriptStage::ParticleSpawn || source.stage == ScriptStage::ParticleUpdate || source.stage == ScriptStage::SimulationStage;
    const auto generated = particle ? Typed::GenerateTypedComputeHlsl(graph, layout, types, attributes, source.stage, result.compiled.hlsl, error, result.compiled.parameters) : Typed::GenerateTypedHlsl(graph, layout, types, attributes, result.compiled.hlsl, error, result.compiled.parameters);
    if (!generated)
    {
        result.compiled = {};
        result.diagnostics.push_back({error.empty() ? "Typed shader backend rejected a graph" : error, source.id, 0});
    }
    return result;
}
}
