#pragma once

// Typed IR GPU backend foundation. It emits a self-contained HLSL evaluate
// function from the typed SSA graph and wraps it in a ShaderMap descriptor.
// Platform compilation and RHI binding remain separate steps.

#include "VfxTypedIR.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::VFX::Typed
{
struct TypedShaderStage
{
    std::string entryPoint;
    std::string source;
};

struct TypedShaderMap
{
    std::string key;
    ScriptStage stage = ScriptStage::ParticleUpdate;
    std::vector<TypedShaderStage> stages;
};

inline uint64_t HashShaderSource(std::string_view source)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : source) { hash ^= byte; hash *= 1099511628211ull; }
    return hash;
}

inline bool IsIntKind(TypeKind kind) { return kind == TypeKind::Bool || kind == TypeKind::Int32; }

struct HlslGenerationOptions
{
    bool declarations = true;
    std::string attributeBuffer = "VfxAttributes";
    std::string attributeStride = "VfxCapacity";
    std::string functionParameters;
    std::string particleIdExpression = "VfxIds[index]";
    std::string tickExpression;
    std::unordered_map<std::string,std::string> parameterExpressions;
};

inline bool GenerateTypedHlsl(const Graph& authored,const OptimizationResult& authoredLayout,
    const TypeRegistry& types,std::span<const Attribute> attributes,
    std::string& source,std::string& error,std::span<const std::string> parameterOrder = {},
    std::string_view functionName = "VfxEvaluate",const HlslGenerationOptions& options = {})
{
    source.clear();
    VFX::Graph declarations;declarations.dataInterfaces=authored.dataInterfaces;
    if(!ValidateGraphDeclarations(declarations,error))return false;
    Graph scalarGraph; OptimizationResult scalarLayout; OptimizationStats scalarStats;
    const bool aggregates=std::any_of(authored.nodes.begin(),authored.nodes.end(),[&](const auto& node)
    { const auto* type=types.Get(node.type);return node.operation==Operation::StructMake || node.operation==Operation::StructGet || (type && type->IsStruct()); });
    if(aggregates && (!LowerTypedAggregates(authored,types,scalarGraph,error) || !OptimizeGraph(scalarGraph,types,scalarLayout,scalarStats,error)))return false;
    const auto& graph=aggregates ? scalarGraph : authored;
    const auto& layout=aggregates ? scalarLayout : authoredLayout;
    const auto attributeOffset = [&](std::string_view name,TypeId& type) -> int32_t
    {
        uint32_t offset = 0;
        for (const auto& attribute : attributes)
        {
            if (attribute.name == name)
            {
                type = FromValueType(attribute.type);
                return static_cast<int32_t>(offset);
            }
            offset += ComponentCount(attribute.type);
        }
        return -1;
    };
    std::vector<int32_t> offsets(layout.attributes.size(),-1);
    for (size_t index = 0; index < layout.attributes.size(); ++index)
    {
        TypeId type = kInvalidType;
        offsets[index] = attributeOffset(layout.attributes[index].name,type);
        if (offsets[index] < 0 || type != layout.attributes[index].type)
        { error = "Typed shader attribute is missing or has an incompatible type: " + layout.attributes[index].name; return false; }
    }
    std::unordered_map<std::string,uint32_t> parameterIndices;
    if (!parameterOrder.empty())
        for (uint32_t index = 0; index < parameterOrder.size(); ++index)
            parameterIndices.emplace(parameterOrder[index],index);
    else
        for (uint32_t index = 0; index < layout.parameters.entries.size(); ++index)
            parameterIndices.emplace(layout.parameters.entries[index].name,index);

    std::ostringstream hlsl;
    if (options.declarations) hlsl << R"(cbuffer VfxConstants : register(b0, space1)
{
    uint VfxCount; uint VfxCapacity; float VfxDeltaTime; uint VfxSeed;
    uint2 VfxTick; uint2 VfxPadding;
    uint2 VfxFirstId; float VfxInitialAge; float VfxAgeStep;
};
StructuredBuffer<uint4> VfxParameters : register(t0, space1);
RWStructuredBuffer<uint> VfxAttributes : register(u0, space1);
RWStructuredBuffer<uint2> VfxIds : register(u1, space1);
RWStructuredBuffer<uint> VfxCounters : register(u2, space1);
RWStructuredBuffer<float4> VfxEvents : register(u3, space1);
RWStructuredBuffer<uint> VfxEventCounts : register(u4, space1);
float VfxDivide(float a, float b) { return abs(b) > 1e-20 ? a / b : 0; }
float2 VfxDivide(float2 a, float2 b) { return float2(VfxDivide(a.x,b.x),VfxDivide(a.y,b.y)); }
float3 VfxDivide(float3 a, float3 b) { return float3(VfxDivide(a.x,b.x),VfxDivide(a.y,b.y),VfxDivide(a.z,b.z)); }
float4 VfxDivide(float4 a, float4 b) { return float4(VfxDivide(a.x,b.x),VfxDivide(a.y,b.y),VfxDivide(a.z,b.z),VfxDivide(a.w,b.w)); }
int VfxIntDivide(int a, int b)
{
    if (b == 0) return 0;
    if (a == (-2147483647-1) && b == -1) return a;
    return a / b;
}
int4 VfxIntDivide(int4 a, int4 b)
{
    return int4(VfxIntDivide(a.x,b.x),VfxIntDivide(a.y,b.y),VfxIntDivide(a.z,b.z),VfxIntDivide(a.w,b.w));
}
uint VfxHash(uint x)
{
    x ^= x >> 16u; x *= 0x7feb352du; x ^= x >> 15u; x *= 0x846ca68bu; x ^= x >> 16u;
    return x;
}
float VfxRandom(uint2 id, uint stream, uint2 tick, uint component)
{
    uint h = VfxHash(id.x ^ VfxHash(id.y) ^ VfxHash(VfxSeed) ^ VfxHash(stream) ^
        VfxHash(tick.x) ^ VfxHash(tick.y) ^ VfxHash(component + 1u));
    return float(h >> 8u)*(1.0/16777216.0);
}
int VfxFloatToInt(float value)
{
    if (!isfinite(value)) return 0;
    if (value >= 2147483648.0) return 2147483647;
    if (value <= -2147483648.0) return (-2147483647-1);
    return int(value);
}
)";

    for (size_t bindingIndex=0;bindingIndex<graph.dataInterfaces.size();++bindingIndex)
    {
        const auto& binding=graph.dataInterfaces[bindingIndex];
        const auto prefix="VfxDI_"+std::string(functionName)+"_"+std::to_string(bindingIndex);
        hlsl << "uint4 " << prefix << "_Load(int i)\n{\n";
        if(binding.data.empty())hlsl << "    return uint4(0,0,0,0);\n";
        else hlsl << "    i=clamp(i,0," << binding.data.size()-1 << ");\n";
        std::vector<uint32_t> slots;
        for(size_t element=0;element<binding.data.size();++element)
        {
            const auto found=parameterIndices.find(DataInterfaceParameter(binding.name,element));
            if(found==parameterIndices.end()){error="Data interface uniform is missing: "+binding.name;return false;}
            slots.push_back(found->second);
        }
        bool contiguous=!slots.empty();
        for(size_t i=1;i<slots.size();++i)contiguous&=slots[i]==slots[0]+i;
        if(contiguous)hlsl << "    return VfxParameters[" << slots[0] << "u+uint(i)];\n";
        else
        {
            hlsl << "    switch(i) {\n";
            for(size_t i=0;i<slots.size();++i)hlsl << "    case " << i << ": return VfxParameters[" << slots[i] << "];\n";
            hlsl << "    default:return uint4(0,0,0,0); }\n";
        }
        hlsl << "}\n";
        if(!IsFloatType(binding.elementType))continue;
        hlsl << "float4 " << prefix << "_Sample(float t)\n{\n    if(!isfinite(t)) return 0;\n";
        if(binding.data.empty())hlsl << "    return 0;\n";
        else if(binding.kind==DataInterfaceKind::Array)
        {
            hlsl << "    float p=saturate(t)*" << binding.data.size()-1 << ".0; int lo=int(p); int hi=min(lo+1," << binding.data.size()-1 << ");\n"
                 << "    return lerp(asfloat(" << prefix << "_Load(lo)),asfloat(" << prefix << "_Load(hi)),p-float(lo));\n";
        }
        else
        {
            hlsl << "    float4 a=asfloat(" << prefix << "_Load(0));\n"
                 << "    if(t<=a.x) return float4(a.y,0,0,0);\n"
                 << "    for(int i=1;i<" << binding.data.size() << ";++i) { float4 b=asfloat(" << prefix << "_Load(i));\n"
                 << "        if(t<b.x) return float4(lerp(a.y,b.y,(t-a.x)/(b.x-a.x)),0,0,0); a=b; }\n"
                 << "    return float4(a.y,0,0,0);\n";
        }
        hlsl << "}\n";
    }
    hlsl << "void " << functionName << "(uint index, uint spawnIndex, float deltaTime" << options.functionParameters << ")\n{\n";
    const auto nodeType = [&](ValueId id) -> const TypeDesc*
    {
        return id < graph.nodes.size() ? types.Get(graph.nodes[id].type) : nullptr;
    };
    const auto inputExpr = [&](const Node& node,uint32_t slot,uint32_t component) -> std::string
    {
        const auto input = node.inputs[slot];
        const auto* type = nodeType(input);
        if (!type) return "0";
        const auto width = TypeWidth(*type);
        std::string value = "r"+std::to_string(input);
        if (width == 1) return value+".x";
        return value+"."+std::string(1,"xyzw"[component < width ? component : 0]);
    };
    const auto scalarExpr = [&](const Node& node,uint32_t slot) { return inputExpr(node,slot,0); };
    // A typed register only carries a scalar in its x component, so a scalar
    // operand must be broadcast across the operation width. Without this a
    // vector multiplied by a scalar constant would only scale its x channel.
    const auto operandExpr = [&](const Node& node,uint32_t slot,uint32_t outputWidth,bool integerOutput) -> std::string
    {
        const auto input = node.inputs[slot];
        const auto* type = nodeType(input);
        const auto width = type ? TypeWidth(*type) : 1u;
        const std::string value = "r"+std::to_string(input);
        if (width != 1 || outputWidth <= 1) return value;
        const std::string component = value+".x";
        return (integerOutput ? "int4(" : "float4(")+component+","+component+","+component+","+component+")";
    };
    for (uint32_t index = 0; index < graph.nodes.size(); ++index)
    {
        const auto& node = graph.nodes[index];
        const auto* type = types.Get(node.type);
        if (!type || !IsScalarOrVector(type->kind))
        { error = "Typed shader supports scalar/vector nodes only"; return false; }
        const auto width = TypeWidth(*type);
        const auto integer = IsIntKind(type->kind);
        const auto resultType = integer ? "int4" : "float4";
        if (node.operation == Operation::WriteAttribute)
        {
            const auto attribute = std::find_if(layout.attributes.begin(),layout.attributes.end(),
                [&](const AttributeSlot& slot) { return slot.name == node.name; });
            if (attribute == layout.attributes.end()) { error = "Typed shader write has no attribute layout"; return false; }
            const auto slot = static_cast<size_t>(attribute-layout.attributes.begin());
            if (node.inputs.size() != 1) { error = "Typed shader write has an invalid input count"; return false; }
            const auto& slotDesc = layout.attributes[slot];
            const auto* slotType = types.Get(slotDesc.type);
            const auto count = slotType ? TypeWidth(*slotType) : width;
            for (uint32_t component = 0; component < count; ++component)
                hlsl << "    " << options.attributeBuffer << "[" << (offsets[slot]+component)
                    << "u*" << options.attributeStride << "+index] = asuint(" << inputExpr(node,0,component) << ");\n";
            continue;
        }
        hlsl << "    " << resultType << " r" << index << " = ";
        switch (node.operation)
        {
        case Operation::Constant:
        {
            hlsl << resultType << "(";
            for (uint32_t component = 0; component < 4; ++component)
            {
                if (component) hlsl << ",";
                if (integer) hlsl << static_cast<int32_t>(std::bit_cast<int32_t>(node.value.scalar[component]));
                else hlsl << node.value.scalar[component];
            }
            hlsl << ")";
            break;
        }
        case Operation::Attribute:
        {
            const auto attribute = std::find_if(layout.attributes.begin(),layout.attributes.end(),
                [&](const AttributeSlot& slot) { return slot.name == node.name; });
            if (attribute == layout.attributes.end()) { error = "Typed shader attribute has no layout"; return false; }
            const auto slot = static_cast<size_t>(attribute-layout.attributes.begin());
            hlsl << resultType << "(";
            for (uint32_t component = 0; component < 4; ++component)
            {
                if (component) hlsl << ",";
                if (component >= width) { hlsl << (integer ? "0" : "0.0"); continue; }
                const auto source = options.attributeBuffer+"["+std::to_string(offsets[slot]+component)+"u*"+options.attributeStride+"+index]";
                hlsl << (integer ? "asint("+source+")" : "asfloat("+source+")");
            }
            hlsl << ")";
            break;
        }
        case Operation::Parameter:
        {
            const auto found = parameterIndices.find(node.name);
            if (found == parameterIndices.end()) { error = "Typed shader parameter has no layout"; return false; }
            if (const auto expression = options.parameterExpressions.find(node.name); expression != options.parameterExpressions.end())
                hlsl << expression->second;
            else hlsl << (integer ? "asint(" : "asfloat(") << "VfxParameters[" << found->second << "])";
            break;
        }
        case Operation::Add: case Operation::Subtract: case Operation::Multiply: case Operation::Divide:
        case Operation::Minimum: case Operation::Maximum:
        {
            const auto a = operandExpr(node,0,width,integer), b = operandExpr(node,1,width,integer);
            const auto op = node.operation == Operation::Add ? "+" : node.operation == Operation::Subtract ? "-" :
                node.operation == Operation::Multiply ? "*" : node.operation == Operation::Divide ? "/" :
                node.operation == Operation::Minimum ? "min" : "max";
            if (node.operation == Operation::Minimum || node.operation == Operation::Maximum)
                hlsl << op << "(" << a << "," << b << ")";
            else if (node.operation == Operation::Divide)
                hlsl << (integer ? "VfxIntDivide(" : "VfxDivide(") << a << "," << b << ")";
            else
                hlsl << "(" << a << op << b << ")";
            break;
        }
        case Operation::Clamp:
            hlsl << "clamp(" << operandExpr(node,0,width,integer) << "," << operandExpr(node,1,width,integer)
                << "," << operandExpr(node,2,width,integer) << ")";
            break;
        case Operation::Lerp:
            hlsl << "lerp(" << operandExpr(node,0,width,integer) << "," << operandExpr(node,1,width,integer)
                << "," << operandExpr(node,2,width,integer) << ")";
            break;
        case Operation::Sine: hlsl << "sin(" << operandExpr(node,0,width,integer) << ")"; break;
        case Operation::Cosine: hlsl << "cos(" << operandExpr(node,0,width,integer) << ")"; break;
        case Operation::Absolute: hlsl << "abs(" << operandExpr(node,0,width,integer) << ")"; break;
        case Operation::SquareRoot: hlsl << "sqrt(max(" << operandExpr(node,0,width,integer) << ",0))"; break;
        case Operation::Length:
        case Operation::Dot:
        case Operation::Cross:
        case Operation::Normalize:
        {
            const auto* inputType = nodeType(node.inputs[0]);
            const auto count = inputType ? TypeWidth(*inputType) : 1u;
            const auto swizzle = count == 1 ? ".x" : count == 2 ? ".xy" :
                                                 count == 3     ? ".xyz" :
                                                                  "";
            const auto a = "r" + std::to_string(node.inputs[0]) + swizzle;
            if (node.operation == Operation::Length)
                hlsl << "float4(length(" << a << "),0,0,0)";
            else if (node.operation == Operation::Dot)
                hlsl << "float4(dot(" << a << ",r" << node.inputs[1] << swizzle << "),0,0,0)";
            else if (node.operation == Operation::Cross)
                hlsl << "float4(cross(" << a << ",r" << node.inputs[1] << ".xyz),0)";
            else
            {
                if (count < 4)
                    hlsl << "float4(";
                hlsl << "(length(" << a << ") > 1e-20 ? " << a << "/length(" << a << ") : ";
                if (count == 1)
                    hlsl << "0.0)";
                else
                {
                    hlsl << "float" << count << "(";
                    for (uint32_t c = 0; c < count; ++c)
                        hlsl << (c ? ",0.0" : "0.0");
                    hlsl << "))";
                }
                if (count < 4)
                    hlsl << (count == 1 ? ",0,0,0)" : count == 2 ? ",0,0)" :
                                                                   ",0)");
            }
            break;
        }
        case Operation::Select:
            // A selected SSA value retains its declared width and bit pattern.
            hlsl << "(" << scalarExpr(node,0) << " != 0 ? r"+std::to_string(node.inputs[1])+" : r"
                << std::to_string(node.inputs[2]) << ")";
            break;
        case Operation::Convert:
        {
            const auto* sourceType = graph.nodes[node.inputs[0]].type == kInvalidType ? nullptr :
                types.Get(graph.nodes[node.inputs[0]].type);
            const auto sourceInteger = sourceType &&
                (sourceType->kind == TypeKind::Bool || sourceType->kind == TypeKind::Int32);
            hlsl << (integer ? "int4(" : "float4(");
            for (uint32_t component = 0; component < 4; ++component)
            {
                if (component) hlsl << ",";
                const auto input = inputExpr(node,0,component);
                if (type->kind == TypeKind::Bool) hlsl << "((" << input << " != 0) ? -1 : 0)";
                else if (type->kind == TypeKind::Int32)
                    hlsl << (sourceType && sourceType->kind == TypeKind::Bool ? "(" + input + " != 0 ? 1 : 0)" : sourceInteger ? "int(" + input + ")" :
                                                                                                                                 "VfxFloatToInt(" + input + ")");
                else
                    hlsl << (sourceType && sourceType->kind == TypeKind::Bool ? "(" + input + " != 0 ? 1.0 : 0.0)" : "float(" + input + ")");
            }
            hlsl << ")";
            break;
        }
        case Operation::DeltaTime:
            hlsl << "float4(deltaTime,deltaTime,deltaTime,deltaTime)";
            break;
        case Operation::StageIndex:
            hlsl << "int4(VfxPadding.x,VfxPadding.x,VfxPadding.x,VfxPadding.x)";
            break;
        case Operation::StageIteration:
            hlsl << "int4(VfxPadding.y,VfxPadding.y,VfxPadding.y,VfxPadding.y)";
            break;
        case Operation::SpawnIndex:
            hlsl << (integer ? "int4(spawnIndex,spawnIndex,spawnIndex,spawnIndex)" :
                "float4(spawnIndex,spawnIndex,spawnIndex,spawnIndex)");
            break;
        case Operation::Random:
        {
            const auto tick = options.tickExpression.empty() ? (graph.stage == ScriptStage::ParticleSpawn ? "uint2(0,0)" : "VfxTick") : options.tickExpression;
            hlsl << "float4(";
            for (uint32_t component = 0; component < 4; ++component)
            {
                if (component) hlsl << ",";
                hlsl << "((VfxRandom(" << options.particleIdExpression << ",(" << node.randomStream << "u" << (graph.stage == ScriptStage::SimulationStage ? " ^ VfxHash(VfxPadding.x) ^ VfxHash(VfxPadding.y+1u)" : "") << "),"
                     << tick << "," << component << "u)*2-1)*" << node.value.scalar[component] << ")";
            }
            hlsl << ")";
            break;
        }
        case Operation::NormalizedAge:
        {
            TypeId ageType = kInvalidType, lifetimeType = kInvalidType;
            const auto ageOffset = attributeOffset("Particles.Age",ageType);
            const auto lifetimeOffset = attributeOffset("Particles.Lifetime",lifetimeType);
            if (ageOffset < 0 || lifetimeOffset < 0)
            { error = "NormalizedAge requires Particles.Age and Particles.Lifetime"; return false; }
            const auto age = "asfloat(VfxAttributes["+std::to_string(ageOffset)+"u*VfxCapacity+index])";
            const auto lifetime = "asfloat(VfxAttributes["+std::to_string(lifetimeOffset)+"u*VfxCapacity+index])";
            hlsl << "float4(" << age << "/max(" << lifetime << ",1e-20)," << age << "/max(" << lifetime
                << ",1e-20)," << age << "/max(" << lifetime << ",1e-20),0)";
            break;
        }
        case Operation::Equal: case Operation::NotEqual: case Operation::LessThan: case Operation::LessEqual:
        case Operation::GreaterThan: case Operation::GreaterEqual:
        {
            const auto op = node.operation == Operation::Equal ? "==" : node.operation == Operation::NotEqual ? "!=" :
                node.operation == Operation::LessThan ? "<" : node.operation == Operation::LessEqual ? "<=" :
                node.operation == Operation::GreaterThan ? ">" : ">=";
            hlsl << "int4(";
            for (uint32_t component = 0; component < 4; ++component)
            {
                if (component) hlsl << ",";
                hlsl << "(" << inputExpr(node,0,component) << op << inputExpr(node,1,component) << " ? -1 : 0)";
            }
            hlsl << ")";
            break;
        }
        case Operation::LogicalAnd: case Operation::LogicalOr: case Operation::LogicalNot:
        {
            const auto a = scalarExpr(node,0);
            if (node.operation == Operation::LogicalNot)
                hlsl << "int4((" << a << " == 0) ? -1 : 0,0,0,0)";
            else
                hlsl << "int4(((" << a << " != 0) " << (node.operation == Operation::LogicalAnd ? "&&" : "||")
                    << " (" << scalarExpr(node,1) << " != 0)) ? -1 : 0,0,0,0)";
            break;
        }
        case Operation::DataInterfaceCall:
        {
            std::vector<VFX::ValueType> inputs;
            for (const auto input : node.inputs)
                inputs.push_back(ToValueType(graph.nodes[input].type));
            DataInterfaceCallBinding call;
            if (!ResolveDataInterfaceCall(graph.dataInterfaces, node.name, inputs, ToValueType(node.type), call, error))
                return false;
            const auto prefix="VfxDI_"+std::string(functionName)+"_"+std::to_string(call.index);
            if(call.method==DataInterfaceMethod::Length)hlsl << "int4(" << graph.dataInterfaces[call.index].data.size() << ",0,0,0)";
            else if(call.method==DataInterfaceMethod::IsValidIndex)
                hlsl << "int4((" << scalarExpr(node,0) << ">=0 && " << scalarExpr(node,0) << "<" << graph.dataInterfaces[call.index].data.size() << ")?-1:0,0,0,0)";
            else if(call.method==DataInterfaceMethod::Get)hlsl << (integer?"asint(":"asfloat(") << prefix << "_Load(int(" << scalarExpr(node,0) << ")))";
            else hlsl << prefix << "_Sample(" << scalarExpr(node,0) << ")";
            break;
        }
        case Operation::WriteAttribute: case Operation::StructMake: case Operation::StructGet:
            error = "Unlowered aggregate in scalar shader emission"; return false;
        }
        hlsl << ";\n";
    }
    hlsl << "}\n";
    source = hlsl.str();
    error.clear();
    return true;
}

inline bool GenerateTypedComputeHlsl(const Graph& graph,const OptimizationResult& layout,
    const TypeRegistry& types,std::span<const Attribute> attributes,ScriptStage stage,
    std::string& source,std::string& error,std::span<const std::string> parameterOrder = {})
{
    std::string evaluate;
    if (!GenerateTypedHlsl(graph,layout,types,attributes,evaluate,error,parameterOrder)) return false;
    const auto attributeOffset = [&](std::string_view name,TypeId& type) -> int32_t
    {
        uint32_t offset = 0;
        for (const auto& attribute : attributes)
        {
            if (attribute.name == name)
            {
                type = FromValueType(attribute.type);
                return static_cast<int32_t>(offset);
            }
            offset += ComponentCount(attribute.type);
        }
        return -1;
    };
    std::ostringstream hlsl;
    hlsl << evaluate;
    const auto parameterIndex = [&](std::string_view name) -> int32_t
    {
        for (uint32_t index = 0; index < parameterOrder.size(); ++index)
            if (parameterOrder[index] == name) return static_cast<int32_t>(index);
        return -1;
    };
    // These parameters are declared in all GPU particle programs. A standalone
    // emitter binds a placeholder and keeps EventBufferEnabled at zero.
    const auto publish = parameterIndex("Emitter.EventPublish");
    const auto emitter = parameterIndex("Emitter.EventEmitterIndex");
    const auto enabled = parameterIndex("Emitter.EventBufferEnabled");
    hlsl << "void VfxPublishEvent(uint index,uint kind)\n{\n";
    if (publish >= 0 && emitter >= 0 && enabled >= 0)
    {
        hlsl << "    if (asfloat(VfxParameters[" << enabled << "].x) == 0 || (uint(asfloat(VfxParameters["
             << publish << "].x)) & (1u << (kind-1u))) == 0) return;\n"
             << "    uint source = uint(asfloat(VfxParameters[" << emitter << "].x));\n"
             << "    uint ordinal; InterlockedAdd(VfxEventCounts[source],1u,ordinal);\n"
             << "    if (ordinal >= 256u) return;\n"
             << "    uint record = (source*256u+ordinal)*3u;\n"
             << "    float3 position = 0, velocity = 0;\n";
        for (const auto& [name,expression] : std::array<std::pair<std::string_view,std::string_view>,2>{{
            {"Particles.Position","position"},{"Particles.Velocity","velocity"}}})
        {
            TypeId type = kInvalidType;
            const auto offset = attributeOffset(name,type);
            if (offset >= 0 && type == kFloat3)
                for (uint32_t c = 0; c < 3; ++c)
                    hlsl << "    " << expression << "." << "xyz"[c] << " = asfloat(VfxAttributes["
                         << offset+c << "u*VfxCapacity+index]);\n";
        }
        hlsl << "    VfxEvents[record] = float4(position,float(kind));\n"
             << "    VfxEvents[record+1u] = float4(velocity,asfloat(VfxIds[index].x));\n"
             << "    VfxEvents[record+2u] = float4(0,0,0,asfloat(VfxIds[index].y));\n";
    }
    hlsl << "}\n";
    if (stage == ScriptStage::ParticleSpawn)
    {
        hlsl << "void VfxSpawnOne(uint index,uint birth)\n{\n"
             << "    uint low = VfxFirstId.x+birth;\n"
             << "    VfxIds[index] = uint2(low,VfxFirstId.y+uint(low < VfxFirstId.x));\n";
        uint32_t scalar = 0;
        for (const auto& attribute : attributes)
            for (uint32_t component = 0; component < ComponentCount(attribute.type); ++component)
                hlsl << "    VfxAttributes[" << scalar++ << "u*VfxCapacity+index] = "
                    << (IsIntegralType(attribute.type) ? std::to_string(std::bit_cast<uint32_t>(attribute.defaultValue[component]))+"u" :
                        "asuint("+std::to_string(attribute.defaultValue[component])+")") << ";\n";
        hlsl << "    VfxEvaluate(index,birth,0);\n}\n";
        hlsl << R"([numthreads(1,1,1)]
void BeginSpawn(uint3 dispatchId : SV_DispatchThreadID)
{
    uint base = min(VfxCounters[0],VfxCapacity);
    uint accepted = min(VfxCount,VfxCapacity-base);
    VfxCounters[2] = base; VfxCounters[3] = accepted; VfxCounters[0] = base+accepted;
}
[numthreads(64,1,1)]
void SpawnParticles(uint3 dispatchId : SV_DispatchThreadID)
{
    uint birth = dispatchId.x+dispatchId.y*4194240u;
    if (birth >= min(VfxCount,VfxCounters[3])) return;
    uint index = VfxCounters[2]+birth;
    if (index >= VfxCapacity) return;
    VfxSpawnOne(index,birth);
    VfxPublishEvent(index,1u);
}
[numthreads(1,1,1)]
void ResetEvents(uint3 dispatchId : SV_DispatchThreadID)
{
)";
        if (emitter >= 0 && enabled >= 0)
            hlsl << "    if (asfloat(VfxParameters[" << enabled << "].x) != 0)\n    {\n"
                 << "        uint source = uint(asfloat(VfxParameters[" << emitter << "].x));\n"
                 << "        VfxEventCounts[source] = 0u;\n"
                 << "        if (VfxPadding.y != 0) for (uint h=0; h<64u; ++h) VfxEventCounts[256u+source*64u+h] = 0u;\n    }\n";
        hlsl << "}\n";
        hlsl << R"([numthreads(64,1,1)]
void Main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint index = dispatchId.x+dispatchId.y*4194240u;
    if (index >= min(VfxCount,VfxCounters[0])) return;
    VfxEvaluate(index,0,VfxDeltaTime);
}
)";
    }
    else if (stage == ScriptStage::ParticleUpdate)
    {
        int32_t ageOffset = -1;
        uint32_t scalar = 0;
        for (const auto& attribute : attributes)
        {
            if (attribute.name == "Particles.Age" && attribute.type == ValueType::Float) ageOffset = static_cast<int32_t>(scalar);
            scalar += ComponentCount(attribute.type);
        }
        hlsl << "void VfxAdvance(uint index, float deltaTime)\n{\n    if (deltaTime <= 0) return;\n";
        if (ageOffset >= 0)
            hlsl << "    VfxAttributes[" << ageOffset << "u*VfxCapacity+index] = asuint(asfloat(VfxAttributes[" << ageOffset << "u*VfxCapacity+index])+deltaTime);\n";
        hlsl << "    VfxEvaluate(index,0,deltaTime);\n";
        TypeId lifetimeType = kInvalidType;
        const auto lifetime = attributeOffset("Particles.Lifetime",lifetimeType);
        if (lifetime >= 0 && ageOffset >= 0)
            hlsl << "    if (asfloat(VfxAttributes[" << ageOffset << "u*VfxCapacity+index]) >= asfloat(VfxAttributes["
                 << lifetime << "u*VfxCapacity+index])) VfxPublishEvent(index,2u);\n";
        hlsl << "}\n"
            << R"([numthreads(64,1,1)]
void UpdateParticles(uint3 dispatchId : SV_DispatchThreadID)
{
    uint index = dispatchId.x+dispatchId.y*4194240u;
    if (index >= min(VfxCount,VfxCounters[0])) return;
    VfxAdvance(index,VfxDeltaTime);
}
[numthreads(64,1,1)]
void AdvanceSpawn(uint3 dispatchId : SV_DispatchThreadID)
{
    uint birth = dispatchId.x+dispatchId.y*4194240u;
    if (birth >= min(VfxCount,VfxCounters[3])) return;
    uint index = VfxCounters[2]+birth;
    if (index >= min(VfxCounters[0],VfxCapacity)) return;
    VfxAdvance(index,max(0,VfxInitialAge-float(birth)*VfxAgeStep));
}
)";
        hlsl << R"([numthreads(64,1,1)]
void Main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint index = dispatchId.x+dispatchId.y*4194240u;
    if (index >= VfxCount) return;
    if (index >= VfxCounters[0]) return;
    VfxEvaluate(index,0,VfxDeltaTime);
}
)";
    }
    else if (stage == ScriptStage::SimulationStage)
    {
        hlsl << R"([numthreads(64,1,1)]
void Main(uint3 dispatchId : SV_DispatchThreadID)
{
    uint index = dispatchId.x+dispatchId.y*4194240u;
    if (index >= VfxCount) return;
    if (index >= VfxCounters[0]) return;
    VfxEvaluate(index,0,VfxDeltaTime);
}
)";
    }
    else
    {
        error = "Typed compute HLSL currently supports particle stages only";
        return false;
    }
    source = hlsl.str();
    error.clear();
    return true;
}

inline TypedShaderMap BuildTypedShaderMap(const Graph& graph,const OptimizationResult& layout,
    const TypeRegistry& types,std::span<const Attribute> attributes,ScriptStage stage,std::string& error,
    std::span<const std::string> parameterOrder = {})
{
    TypedShaderMap map;
    map.stage = stage;
    std::string source;
    if (!GenerateTypedComputeHlsl(graph,layout,types,attributes,stage,source,error,parameterOrder))
        return {};
    map.key = "fnv1a:"+std::to_string(HashShaderSource(source));
    const auto entry = stage == ScriptStage::ParticleSpawn ? "SpawnParticles" :
        stage == ScriptStage::SimulationStage ? "Main" : "UpdateParticles";
    map.stages.push_back({entry,std::move(source)});
    return map;
}
} // namespace NLS::VFX::Typed
