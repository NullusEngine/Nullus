#pragma once

// Typed SSA shared by the bytecode and HLSL backends. Source graphs are
// validated and translated once; both targets consume the same typed values.

#include "VfxGraph.h"
#include "VfxAggregateLowering.h"
#include "VfxTypeSystem.h"
#include "ParticleDataSet.h"
#include "VfxTypeRules.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NLS::VFX::Typed
{
using ValueId = uint32_t;
using NodeId = uint64_t;

using Operation = VFX::Operation;

struct Node
{
    Operation operation = Operation::Constant;
    TypeId type = kInvalidType;
    std::vector<ValueId> inputs;
    Value value {};
    std::string name;
    uint32_t randomStream = 0;
    // Struct/data-interface metadata. Empty for ordinary scalar/vector nodes.
    std::vector<TypeId> fieldTypes;
    std::vector<uint32_t> fieldIndices;
};

struct Graph
{
    std::vector<Node> nodes;
    ScriptStage stage = ScriptStage::ParticleUpdate;
    std::vector<VFX::DataInterfaceDefinition> dataInterfaces;
    std::vector<ParameterMapEntry> declaredParameters;
};

inline bool IsScalarOrVector(TypeKind kind)
{
    return kind == TypeKind::Bool || kind == TypeKind::Int32 || kind == TypeKind::Float ||
        kind == TypeKind::Float2 || kind == TypeKind::Float3 || kind == TypeKind::Float4;
}

inline uint32_t TypeWidth(TypeKind kind)
{
    switch (kind)
    {
    case TypeKind::Float2: return 2;
    case TypeKind::Float3: return 3;
    case TypeKind::Float4: return 4;
    default: return 1;
    }
}

inline TypeId FromValueType(ValueType type)
{
    switch (type)
    {
    case ValueType::Float: return kFloat;
    case ValueType::Float2: return kFloat2;
    case ValueType::Float3: return kFloat3;
    case ValueType::Float4: return kFloat4;
    case ValueType::Int32: return kInt32;
    case ValueType::Bool: return kBool;
    }
    return kInvalidType;
}

inline VFX::ValueType ToValueType(TypeId type)
{
    switch(type)
    {
    case kFloat:return VFX::ValueType::Float; case kFloat2:return VFX::ValueType::Float2;
    case kFloat3:return VFX::ValueType::Float3; case kFloat4:return VFX::ValueType::Float4;
    case kInt32:return VFX::ValueType::Int32; case kBool:return VFX::ValueType::Bool;
    default:return static_cast<VFX::ValueType>(255);
    }
}


inline const Attribute* FindAttribute(std::span<const Attribute> attributes, std::string_view name)
{
    const auto found = std::find_if(attributes.begin(), attributes.end(),
        [&](const Attribute& attribute) { return attribute.name == name; });
    return found == attributes.end() ? nullptr : &*found;
}

// Translates a flattened authored graph into typed SSA order. The source graph
// must already have function calls/static switches expanded.
inline uint32_t TypeWidth(const TypeDesc& type) { return TypeWidth(type.kind); }

inline int32_t TypedInt(float value) { return std::bit_cast<int32_t>(value); }
inline float TypedFloat(int32_t value) { return std::bit_cast<float>(value); }
inline float TypedBool(bool value) { return TypedFloat(value ? -1 : 0); }

inline bool IsConstantScalar(const Node& node,const TypeRegistry& types)
{
    const auto* type = types.Get(node.type);
    return type && IsScalarOrVector(type->kind);
}

inline bool EvaluateConstant(const Graph& graph,ValueId value,const TypeRegistry& types,
    std::unordered_map<ValueId,std::optional<Value>>& cache,std::string& error)
{
    if (const auto found = cache.find(value); found != cache.end()) return found->second.has_value();
    if (value >= graph.nodes.size()) { error = "Constant evaluator received an invalid value ID"; return false; }
    const auto& node = graph.nodes[value];
    if (node.operation == Operation::Constant)
    {
        cache[value] = node.value;
        return true;
    }
    const auto evaluateInput = [&](uint32_t index,Value& result) -> bool
    {
        if (index >= node.inputs.size()) { error = "Typed constant node has a missing input"; return false; }
        if (!EvaluateConstant(graph,node.inputs[index],types,cache,error)) return false;
        result = *cache.at(node.inputs[index]);
        return true;
    };
    const auto* outputType = types.Get(node.type);
    if (!outputType || !IsScalarOrVector(outputType->kind))
    { error = "Typed constant node has a non-scalar type"; return false; }
    const auto width = TypeWidth(*outputType);
    const auto component = [](const Value& input,uint32_t index,uint32_t fallbackWidth) -> float
    {
        return input.scalar[index < fallbackWidth ? index : 0];
    };
    const auto inputWidth = [&](uint32_t index) -> uint32_t
    {
        const auto* type = types.Get(graph.nodes[node.inputs[index]].type);
        return type ? TypeWidth(*type) : 1u;
    };
    Value result;
    result.type = node.type;
    switch (node.operation)
    {
    case Operation::Add:
    case Operation::Subtract:
    case Operation::Multiply:
    case Operation::Divide:
    case Operation::Minimum:
    case Operation::Maximum:
    {
        Value a, b;
        if (!evaluateInput(0,a) || !evaluateInput(1,b)) return false;
        const auto aw = inputWidth(0), bw = inputWidth(1);
        for (uint32_t i = 0; i < width; ++i)
        {
            const auto av = component(a,i,aw), bv = component(b,i,bw);
            if (outputType->kind == TypeKind::Int32)
            {
                const auto ai = TypedInt(av), bi = TypedInt(bv);
                const auto ua = std::bit_cast<uint32_t>(ai), ub = std::bit_cast<uint32_t>(bi);
                const auto value = node.operation == Operation::Add      ? std::bit_cast<int32_t>(ua + ub) :
                                   node.operation == Operation::Subtract ? std::bit_cast<int32_t>(ua - ub) :
                                   node.operation == Operation::Multiply ? std::bit_cast<int32_t>(ua * ub) :
                                   node.operation == Operation::Divide   ? (bi == 0 ? 0 : ai == INT32_MIN && bi == -1 ? ai :
                                                                                                                        ai / bi) :
                                   node.operation == Operation::Minimum  ? (std::min)(ai, bi) :
                                                                           (std::max)(ai, bi);
                result.scalar[i] = TypedFloat(value);
            }
            else
            {
                const auto value = node.operation == Operation::Add ? av+bv :
                    node.operation == Operation::Subtract ? av-bv :
                    node.operation == Operation::Multiply ? av*bv :
                    node.operation == Operation::Divide ? (std::abs(bv) < 1e-20f ? 0.0f : av/bv) :
                    node.operation == Operation::Minimum ? (std::min)(av,bv) : (std::max)(av,bv);
                result.scalar[i] = value;
            }
        }
        break;
    }
    case Operation::Clamp:
    case Operation::Lerp:
    {
        Value a, b, c;
        if (!evaluateInput(0,a) || !evaluateInput(1,b) || !evaluateInput(2,c)) return false;
        const auto aw = inputWidth(0), bw = inputWidth(1), cw = inputWidth(2);
        for (uint32_t i = 0; i < width; ++i)
        {
            const auto av = component(a,i,aw), bv = component(b,i,bw), cv = component(c,i,cw);
            if (outputType->kind == TypeKind::Int32 && node.operation == Operation::Clamp)
            {
                result.scalar[i] = TypedFloat((std::min)((std::max)(TypedInt(av), TypedInt(bv)), TypedInt(cv)));
                continue;
            }
            result.scalar[i] = node.operation == Operation::Clamp ?
                (std::min)((std::max)(av,bv),cv) : av+(bv-av)*cv;
        }
        break;
    }
    case Operation::Select:
    {
        Value condition, yes, no;
        if (!evaluateInput(0,condition) || !evaluateInput(1,yes) || !evaluateInput(2,no)) return false;
        const auto selected = condition.scalar[0] != 0 ? yes : no;
        result.scalar = selected.scalar;
        break;
    }
    case Operation::Equal: case Operation::NotEqual:
    case Operation::LessThan: case Operation::LessEqual:
    case Operation::GreaterThan: case Operation::GreaterEqual:
    {
        Value a,b;
        if (!evaluateInput(0,a) || !evaluateInput(1,b)) return false;
        const auto* inputType = types.Get(graph.nodes[node.inputs[0]].type);
        const bool integral = inputType && (inputType->kind == TypeKind::Bool || inputType->kind == TypeKind::Int32);
        const double av = integral ? static_cast<double>(TypedInt(a.scalar[0])) : a.scalar[0];
        const double bv = integral ? static_cast<double>(TypedInt(b.scalar[0])) : b.scalar[0];
        const auto value = node.operation == Operation::Equal ? av == bv :
            node.operation == Operation::NotEqual ? av != bv :
            node.operation == Operation::LessThan ? av < bv :
            node.operation == Operation::LessEqual ? av <= bv :
            node.operation == Operation::GreaterThan ? av > bv : av >= bv;
        result.type = kBool;
        result.scalar[0] = TypedBool(value);
        break;
    }
    case Operation::LogicalAnd: case Operation::LogicalOr:
    {
        Value a,b;
        if (!evaluateInput(0,a) || !evaluateInput(1,b)) return false;
        result.type = kBool;
        result.scalar[0] = TypedBool(node.operation == Operation::LogicalAnd ?
            (a.scalar[0] != 0 && b.scalar[0] != 0) : (a.scalar[0] != 0 || b.scalar[0] != 0));
        break;
    }
    case Operation::LogicalNot:
    {
        Value a;
        if (!evaluateInput(0,a)) return false;
        result.type = kBool;
        result.scalar[0] = TypedBool(a.scalar[0] == 0);
        break;
    }
    case Operation::Sine: case Operation::Cosine: case Operation::Absolute: case Operation::SquareRoot:
    {
        Value a;
        if (!evaluateInput(0,a)) return false;
        for (uint32_t i = 0; i < width; ++i)
        {
            const auto value = component(a,i,inputWidth(0));
            if (outputType->kind == TypeKind::Int32 && node.operation == Operation::Absolute)
            {
                const auto integer = TypedInt(value);
                result.scalar[i] = TypedFloat(integer == INT32_MIN ? integer : integer < 0 ? -integer :
                                                                                             integer);
                continue;
            }
            result.scalar[i] = node.operation == Operation::Sine ? std::sin(value) :
                node.operation == Operation::Cosine ? std::cos(value) :
                node.operation == Operation::Absolute ? std::abs(value) : std::sqrt((std::max)(0.0f,value));
        }
        break;
    }
    case Operation::Length:
    case Operation::Dot:
    {
        Value a,b;
        if (!evaluateInput(0,a) || (node.operation == Operation::Dot && !evaluateInput(1,b))) return false;
        const auto aw = inputWidth(0);
        float value = 0;
        if (node.operation == Operation::Dot)
        {
            const auto bw = inputWidth(1);
            for (uint32_t i = 0; i < (std::max)(aw,bw); ++i) value += component(a,i,aw)*component(b,i,bw);
        }
        else for (uint32_t i = 0; i < aw; ++i) value += a.scalar[i]*a.scalar[i];
        result.type = kFloat;
        result.scalar[0] = node.operation == Operation::Length ? std::sqrt((std::max)(0.0f,value)) : value;
        break;
    }
    case Operation::Convert:
    {
        Value a;
        if (!evaluateInput(0,a)) return false;
        const auto aw = inputWidth(0);
        for (uint32_t i = 0; i < width; ++i)
        {
            const auto raw = component(a, i, aw);
            const auto* inputType = types.Get(graph.nodes[node.inputs[0]].type);
            const bool integral = inputType && (inputType->kind == TypeKind::Bool || inputType->kind == TypeKind::Int32);
            const double value = inputType && inputType->kind == TypeKind::Bool ? (TypedInt(raw) != 0 ? 1.0 : 0.0) : integral ? static_cast<double>(TypedInt(raw)) :
                                                                                                                                raw;
            if (outputType->kind == TypeKind::Bool)
                result.scalar[i] = TypedBool(value != 0);
            else if (outputType->kind == TypeKind::Int32)
                result.scalar[i] = TypedFloat(!std::isfinite(value) ? 0 : value >= 2147483647.0 ? INT32_MAX :
                                                                      value <= -2147483648.0    ? INT32_MIN :
                                                                                                  static_cast<int32_t>(value));
            else
                result.scalar[i] = static_cast<float>(value);
        }
        break;
    }
    default:
        return false;
    }
    cache[value] = std::move(result);
    return true;
}

struct AttributeSlot
{
    std::string name;
    TypeId type = kInvalidType;
    bool read = false;
    bool write = false;
};

struct OptimizationResult
{
    ParameterMapLayout parameters;
    std::vector<AttributeSlot> attributes;
};

struct OptimizationStats
{
    uint32_t constantsFolded = 0;
    uint32_t nodesRemoved = 0;
};

inline bool OptimizeGraph(Graph& graph,const TypeRegistry& types,OptimizationResult& layout,
    OptimizationStats& stats,std::string& error)
{
    stats = {};
    layout = {};
    std::unordered_map<ValueId,std::optional<Value>> constants;
    for (uint32_t index = 0; index < graph.nodes.size(); ++index)
    {
        auto& node = graph.nodes[index];
        if (node.operation == Operation::Constant || node.operation == Operation::WriteAttribute) continue;
        if (!EvaluateConstant(graph,index,types,constants,error)) continue;
        const auto found = constants.find(index);
        if (found == constants.end() || !found->second.has_value()) continue;
        const auto scalar = ToValueType(found->second->type);
        if (VFX::IsValueType(scalar) && !VFX::ValidValue(found->second->scalar, scalar))
            continue;
        node.value = *found->second;
        node.type = node.value.type;
        node.operation = Operation::Constant;
        node.inputs.clear();
        node.name.clear();
        ++stats.constantsFolded;
    }
    std::vector<uint8_t> keep(graph.nodes.size(),0);
    std::function<void(ValueId)> mark = [&](ValueId id)
    {
        if (id >= graph.nodes.size() || keep[id]) return;
        keep[id] = 1;
        for (const auto input : graph.nodes[id].inputs) mark(input);
    };
    for (uint32_t index = 0; index < graph.nodes.size(); ++index)
        if (graph.nodes[index].operation == Operation::WriteAttribute) mark(index);
    std::vector<uint32_t> remap(graph.nodes.size(),UINT32_MAX);
    std::vector<Node> optimized;
    optimized.reserve(graph.nodes.size());
    for (uint32_t index = 0; index < graph.nodes.size(); ++index)
        if (keep[index])
        {
            remap[index] = static_cast<uint32_t>(optimized.size());
            optimized.push_back(graph.nodes[index]);
            ++stats.nodesRemoved;
        }
    stats.nodesRemoved = static_cast<uint32_t>(graph.nodes.size())-static_cast<uint32_t>(optimized.size());
    for (auto& node : optimized)
        for (auto& input : node.inputs)
        {
            if (input >= remap.size() || remap[input] == UINT32_MAX)
            { error = "Typed graph DCE removed a live dependency"; return false; }
            input = remap[input];
        }
    graph.nodes = std::move(optimized);

    std::vector<ParameterMapEntry> parameters = graph.declaredParameters;
    std::vector<AttributeSlot> attributes;
    const auto parameter = [&](std::string_view name,TypeId type,const Value& value)
    {
        const auto found = std::find_if(parameters.begin(),parameters.end(),
            [&](const ParameterMapEntry& entry) { return entry.name == name; });
        if (found != parameters.end())
        {
            if (found->type != type) { error = "Typed parameter is used with inconsistent types: " + std::string(name); return false; }
            return true;
        }
        parameters.push_back({std::string(name),type,0,value});
        return true;
    };
    const auto attribute = [&](std::string_view name,TypeId type,bool read,bool write)
    {
        const auto found = std::find_if(attributes.begin(),attributes.end(),
            [&](const AttributeSlot& slot) { return slot.name == name; });
        if (found != attributes.end())
        {
            if (found->type != type) { error = "Typed attribute is used with inconsistent types: " + std::string(name); return false; }
            found->read |= read; found->write |= write; return true;
        }
        attributes.push_back({std::string(name),type,read,write});
        return true;
    };
    for (const auto& node : graph.nodes)
    {
        if (node.operation == Operation::Parameter && !parameter(node.name,node.type,node.value)) return false;
        if (node.operation == Operation::Attribute && !attribute(node.name,node.type,true,false)) return false;
        if (node.operation == Operation::WriteAttribute && !attribute(node.name,node.type,false,true)) return false;
    }
    for (const auto& binding : graph.dataInterfaces)
        for (size_t index=0; index<binding.data.size(); ++index)
        {
            Value value;
            value.type = FromValueType(DataInterfaceStorageType(binding));
            value.scalar = binding.data[index];
            if (!parameter(DataInterfaceParameter(binding.name,index),value.type,value)) return false;
        }
    layout.attributes = std::move(attributes);
    std::string mapError;
    layout.parameters = BuildParameterMap(types,parameters,mapError);
    if (!mapError.empty()) { error = mapError; return false; }
    error.clear();
    return true;
}


// GPU aggregate lowering uses the same field bundle semantics as the source
// frontend. This also accepts directly constructed Typed IR, including nested
// structs, struct parameters/constants, selects and aggregate attribute writes.
inline bool LowerTypedAggregates(const Graph& source,const TypeRegistry& types,Graph& output,std::string& error)
{
    Graph result;result.stage=source.stage;result.dataInterfaces=source.dataInterfaces;
    struct Bundle {TypeId type=kInvalidType;ValueId value=UINT32_MAX;std::vector<Bundle> fields;};
    std::vector<Bundle> bundles;
    const auto append=[&](Node node)->ValueId {const auto id=static_cast<ValueId>(result.nodes.size());result.nodes.push_back(std::move(node));return id;};
    std::function<bool(const Node&,TypeId,const std::string&,const Value&,Bundle&)> expand;
    expand=[&](const Node& node,TypeId id,const std::string& name,const Value& literal,Bundle& out)
    {
        const auto* type=types.Get(id);if(!type){error="Unknown typed aggregate field type";return false;}
        out.type=id;
        if(type->IsStruct())
        {
            if(!literal.fields.empty() && literal.fields.size()!=type->fields.size()){error="Struct literal field count mismatch";return false;}
            for(size_t f=0;f<type->fields.size();++f)
            {
                Bundle child;Value value;value.type=type->fields[f].type;
                if(!literal.fields.empty())value=literal.fields[f];
                if(!expand(node,type->fields[f].type,name+"."+type->fields[f].name,value,child))return false;
                out.fields.push_back(std::move(child));
            }
        }
        else
        {
            if(!IsScalarOrVector(type->kind)){error="Unsupported aggregate field type";return false;}
            Node field=node;field.type=id;field.name=name;field.value=literal;field.value.type=id;field.inputs.clear();out.value=append(std::move(field));
        }
        return true;
    };
    for(const auto& node:source.nodes)
    {
        const auto* type=types.Get(node.type);if(!type){error="Unknown typed aggregate node type";return false;}
        std::vector<Bundle> inputs;
        for(const auto input:node.inputs){if(input>=bundles.size()){error="Typed aggregate dependency is not SSA ordered";return false;}inputs.push_back(bundles[input]);}
        Bundle value;value.type=node.type;
        if(node.operation==Operation::StructMake)
        {
            if(!type->IsStruct() || inputs.size()!=type->fields.size()){error="StructMake field count/type mismatch";return false;}
            for(size_t f=0;f<inputs.size();++f)if(inputs[f].type!=type->fields[f].type){error="StructMake field type mismatch";return false;}
            value.fields=std::move(inputs);
        }
        else if(node.operation==Operation::StructGet)
        {
            if(inputs.size()!=1 || node.fieldIndices.empty()){error="StructGet requires a field path";return false;}
            value=inputs[0];
            for(const auto f:node.fieldIndices)
            {if(f>=value.fields.size()){error="StructGet field index out of range";return false;}auto selected=value.fields[f];value=std::move(selected);}
            if(value.type!=node.type){error="StructGet result type mismatch";return false;}
        }
        else if(type->IsStruct() && (node.operation==Operation::Parameter || node.operation==Operation::Attribute || node.operation==Operation::Constant))
        {if(!expand(node,node.type,node.name,node.value,value))return false;}
        else if(type->IsStruct() && node.operation==Operation::Select)
        {
            if(inputs.size()!=3 || inputs[0].type!=kBool || inputs[1].type!=node.type || inputs[2].type!=node.type){error="Struct Select type mismatch";return false;}
            std::function<Bundle(const Bundle&,const Bundle&)> choose=[&](const Bundle& a,const Bundle& b)
            {
                Bundle value=a;
                if(!a.fields.empty()){for(size_t f=0;f<a.fields.size();++f)value.fields[f]=choose(a.fields[f],b.fields[f]);}
                else {Node selected;selected.operation=Operation::Select;selected.type=a.type;selected.inputs={inputs[0].value,a.value,b.value};value.value=append(std::move(selected));}
                return value;
            };value=choose(inputs[1],inputs[2]);
        }
        else if(type->IsStruct() && node.operation==Operation::WriteAttribute)
        {
            if(inputs.size()!=1 || inputs[0].type!=node.type){error="Struct write type mismatch";return false;}
            std::function<void(const Bundle&,const std::string&)> write=[&](const Bundle& field,const std::string& name)
            {
                if(!field.fields.empty()){const auto* desc=types.Get(field.type);for(size_t f=0;f<field.fields.size();++f)write(field.fields[f],name+"."+desc->fields[f].name);}
                else {Node leaf=node;leaf.type=field.type;leaf.name=name;leaf.inputs={field.value};append(std::move(leaf));}
            };write(inputs[0],node.name);
        }
        else
        {
            if(type->IsStruct()){error="Unsupported operation on a struct value";return false;}
            Node leaf=node;leaf.inputs.clear();
            for(const auto& input:inputs){if(!input.fields.empty()){error="Extract a struct field before scalar arithmetic";return false;}leaf.inputs.push_back(input.value);}
            value.value=append(std::move(leaf));
        }
        bundles.push_back(std::move(value));
        if(result.nodes.size()>65536){error="Aggregate field expansion exceeds 65536 nodes";return false;}
    }
    output=std::move(result);error.clear();return true;
}

inline bool TranslateGraph(const VFX::Graph& authored, const TypeRegistry& types,
                           std::span<const Attribute> attributes, Graph& result, std::string& error)
{
    result = {};
    VFX::Graph source;
    if (!LowerAggregateGraph(authored, source, error))
        return false;
    if (source.nodes.size() > 65536 || attributes.size() > 1024 || static_cast<uint32_t>(source.stage) > static_cast<uint32_t>(ScriptStage::SystemUpdate))
    {
        error = "Graph stage, node or attribute limit exceeded";
        return false;
    }
    result.stage = source.stage;
    result.dataInterfaces = source.dataInterfaces;
    std::unordered_map<std::string, VFX::ValueType> attributeTypes;
    for (const auto& attribute : attributes)
        if (attribute.name.empty() || !VFX::ValidValue(attribute.defaultValue, attribute.type) || !attributeTypes.emplace(attribute.name, attribute.type).second)
        {
            error = "Invalid or duplicate attribute: " + attribute.name;
            return false;
        }
    std::unordered_map<VFX::NodeId, size_t> indices;
    for (size_t i = 0; i < source.nodes.size(); ++i)
        if (!source.nodes[i].id || !indices.emplace(source.nodes[i].id, i).second)
        {
            error = "Invalid or duplicate node ID";
            return false;
        }
    std::vector<uint8_t> visiting(source.nodes.size());
    std::unordered_map<VFX::NodeId, ValueId> emitted;
    std::unordered_set<std::string> writes;
    std::function<bool(VFX::NodeId, ValueId&, uint32_t)> emit = [&](VFX::NodeId id, ValueId& output, uint32_t depth)
    {
        if (const auto found = emitted.find(id); found != emitted.end())
        {
            output = found->second;
            return true;
        }
        const auto index = indices.find(id);
        if (index == indices.end())
        {
            error = "Input is not connected to an existing node";
            return false;
        }
        if (depth > 512 || visiting[index->second])
        {
            error = "Graph contains a dependency cycle or exceeds depth 512";
            return false;
        }
        const auto& node = source.nodes[index->second];
        visiting[index->second] = 1;
        if (node.operation > Operation::DataInterfaceCall || !VFX::ValidValue(node.value, node.type) || !ValidInputCount(node.operation, node.inputs.size()))
        {
            error = "Invalid operation, type, value or input count";
            return false;
        }
        if ((node.operation == Operation::StageIndex || node.operation == Operation::StageIteration) && source.stage != ScriptStage::SimulationStage)
        {
            error = "Stage context nodes require a Simulation Stage graph";
            return false;
        }
        if (node.operation == Operation::SpawnIndex && source.stage != ScriptStage::ParticleSpawn)
        {
            error = "Spawn index requires a Particle Spawn graph";
            return false;
        }
        std::vector<ValueId> inputs;
        std::vector<VFX::ValueType> inputTypes;
        for (const auto input : node.inputs)
        {
            ValueId value;
            if (!emit(input, value, depth + 1))
                return false;
            if (result.nodes[value].operation == Operation::WriteAttribute)
            {
                error = "Attribute outputs are terminal nodes";
                return false;
            }
            inputs.push_back(value);
            inputTypes.push_back(ToValueType(result.nodes[value].type));
        }
        auto declared = node.type;
        if (node.operation == Operation::Attribute || node.operation == Operation::WriteAttribute)
        {
            const auto attribute = attributeTypes.find(node.name);
            if (attribute == attributeTypes.end())
            {
                error = "Typed graph references unknown attribute: " + node.name;
                return false;
            }
            declared = attribute->second;
            if (node.operation == Operation::WriteAttribute && !writes.insert(node.name).second)
            {
                error = "Attribute has more than one writer: " + node.name;
                return false;
            }
        }
        if (node.operation == Operation::NormalizedAge)
            for (const auto* name : {"Particles.Age", "Particles.Lifetime"})
                if (!attributeTypes.contains(name) || attributeTypes.at(name) != VFX::ValueType::Float)
                {
                    error = "Normalized age requires scalar Age and Lifetime attributes";
                    return false;
                }
        VFX::ValueType resolved;
        if (!ResolveOperationType(node.operation, declared, inputTypes, resolved, error))
            return false;
        if (node.operation == Operation::DataInterfaceCall)
        {
            DataInterfaceCallBinding call;
            if (!ResolveDataInterfaceCall(source.dataInterfaces, node.name, inputTypes, resolved, call, error))
                return false;
        }
        if (node.operation == Operation::Parameter)
        {
            const auto found = std::find_if(result.declaredParameters.begin(), result.declaredParameters.end(), [&](const auto& p)
                                            { return p.name == node.name; });
            const auto type = FromValueType(resolved);
            if (node.name.empty() || (found != result.declaredParameters.end() && (found->type != type || !SameValue(found->defaultValue.scalar, node.value))))
            {
                error = "Parameter has inconsistent types/defaults or an empty name: " + node.name;
                return false;
            }
            if (found == result.declaredParameters.end())
                result.declaredParameters.push_back({node.name, type, 0, Value{type, node.value}});
        }
        Node value;
        value.operation = node.operation;
        value.type = FromValueType(resolved);
        value.inputs = std::move(inputs);
        value.value = {value.type, node.value};
        value.name = node.name;
        value.randomStream = RandomHash(static_cast<uint32_t>(node.id)) ^ RandomHash(static_cast<uint32_t>(node.id >> 32));
        output = static_cast<ValueId>(result.nodes.size());
        result.nodes.push_back(std::move(value));
        emitted[id] = output;
        visiting[index->second] = 0;
        return true;
    };
    // Validate disconnected nodes too. All expressions read the incoming stage
    // state; commit every output only after evaluating all non-write values.
    for (const auto& node : source.nodes)
    {
        ValueId ignored;
        if (!emit(node.id, ignored, 0))
            return false;
    }
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < result.nodes.size(); ++i)
        if (result.nodes[i].operation != Operation::WriteAttribute)
            order.push_back(i);
    for (uint32_t i = 0; i < result.nodes.size(); ++i)
        if (result.nodes[i].operation == Operation::WriteAttribute)
            order.push_back(i);
    std::vector<uint32_t> remap(result.nodes.size());
    for (uint32_t i = 0; i < order.size(); ++i)
        remap[order[i]] = i;
    std::vector<Node> ordered;
    ordered.reserve(order.size());
    for (const auto i : order)
    {
        auto node = std::move(result.nodes[i]);
        for (auto& input : node.inputs)
            input = remap[input];
        ordered.push_back(std::move(node));
    }
    result.nodes = std::move(ordered);
    error.clear();
    return true;
}
} // namespace NLS::VFX::Typed
