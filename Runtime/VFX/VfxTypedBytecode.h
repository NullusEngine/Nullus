#pragma once

// CPU bytecode backend for the typed SSA IR. This is deliberately a small,
// deterministic VM contract: the compiler emits register/attribute/parameter
// indices. This bytecode is the executable runtime program on every CPU
// script stage; there is no compatibility interpreter.

#include "VfxTypedIR.h"
#include "ParticleDataSet.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NLS::VFX::Typed
{
struct BytecodeInstruction
{
    Operation operation = Operation::Constant;
    TypeId type = kInvalidType;
    std::vector<ValueId> inputs;
    std::vector<TypeId> inputTypes;
    std::vector<uint32_t> fieldIndices;
    uint32_t inputCount = 0;
    uint32_t destination = 0;
    uint32_t attribute = (std::numeric_limits<uint32_t>::max)();
    uint32_t parameter = (std::numeric_limits<uint32_t>::max)();
    uint32_t randomStream = 0;
    Value literal{};
    std::string name;
};

struct BytecodeProgram
{
    std::vector<BytecodeInstruction> instructions;
    uint32_t registerCount = 0;
    ScriptStage stage = ScriptStage::ParticleUpdate;
    ParameterMapLayout parameters;
    std::vector<AttributeSlot> attributes;
    std::vector<VFX::DataInterfaceDefinition> dataInterfaces;
};

inline bool ValidateBytecodeProgram(const BytecodeProgram& program, const TypeRegistry& types, std::string& error)
{
    const auto fail = [&](std::string message)
    {error=std::move(message);return false; };
    if (program.instructions.size() > 65536 || program.registerCount != program.instructions.size() || program.attributes.size() > 1024 || program.parameters.entries.size() > 16384 || static_cast<uint32_t>(program.stage) > static_cast<uint32_t>(ScriptStage::SystemUpdate))
        return fail("Invalid typed bytecode size, register count or stage");
    const auto parameters = BuildParameterMap(types, program.parameters.entries, error);
    if (!error.empty())
        return false;
    if (parameters.size != program.parameters.size || parameters.alignment != program.parameters.alignment)
        return fail("Invalid typed parameter layout");
    for (size_t i = 0; i < parameters.entries.size(); ++i)
    {
        const auto& value = program.parameters.entries[i];
        if (value.offset != parameters.entries[i].offset)
            return fail("Invalid typed parameter offset");
        const auto scalar = ToValueType(value.type);
        if (VFX::IsValueType(scalar) && !VFX::ValidValue(value.defaultValue.scalar, scalar))
            return fail("Invalid typed parameter default: " + value.name);
    }
    std::unordered_set<std::string> attributes, writes, reads;
    for (const auto& attribute : program.attributes)
        if (attribute.name.empty() || !types.Get(attribute.type) || !attributes.insert(attribute.name).second)
            return fail("Invalid or duplicate bytecode attribute");
    VFX::Graph declarations;
    declarations.dataInterfaces = program.dataInterfaces;
    if (!ValidateGraphDeclarations(declarations, error))
        return false;
    for (const auto& binding : program.dataInterfaces)
        for (size_t index = 0; index < binding.data.size(); ++index)
        {
            const auto name = DataInterfaceParameter(binding.name, index);
            const auto* entry = program.parameters.Find(name);
            if (!entry || entry->type != FromValueType(DataInterfaceStorageType(binding)) || !SameValue(entry->defaultValue.scalar, binding.data[index]))
                return fail("Data interface uniform declaration is missing or inconsistent: " + name);
        }
    for (uint32_t index = 0; index < program.instructions.size(); ++index)
    {
        const auto& instruction = program.instructions[index];
        const auto* output = types.Get(instruction.type);
        if (instruction.operation > Operation::DataInterfaceCall || !output || instruction.destination != index || instruction.inputCount > 64 || instruction.inputs.size() != instruction.inputCount || instruction.inputTypes.size() != instruction.inputCount || !ValidInputCount(instruction.operation, instruction.inputCount))
            return fail("Invalid typed bytecode instruction");
        for (size_t i = 0; i < instruction.inputs.size(); ++i)
            if (instruction.inputs[i] >= index || instruction.inputTypes[i] != program.instructions[instruction.inputs[i]].type)
                return fail("Typed bytecode reads an invalid register or mismatched input type");
        if ((instruction.operation == Operation::StageIndex || instruction.operation == Operation::StageIteration) && program.stage != ScriptStage::SimulationStage)
            return fail("Stage context nodes require a Simulation Stage graph");
        if (instruction.operation == Operation::SpawnIndex && program.stage != ScriptStage::ParticleSpawn)
            return fail("Spawn index requires a Particle Spawn graph");
        if (instruction.operation == Operation::StructMake)
        {
            if (!output->IsStruct() || instruction.inputs.size() != output->fields.size())
                return fail("StructMake field count/type mismatch");
            for (size_t i = 0; i < instruction.inputs.size(); ++i)
                if (instruction.inputTypes[i] != output->fields[i].type)
                    return fail("StructMake field type mismatch");
        }
        else if (instruction.operation == Operation::StructGet)
        {
            if (instruction.fieldIndices.empty())
                return fail("StructGet requires a field path");
            const auto* field = types.Get(instruction.inputTypes[0]);
            for (const auto f : instruction.fieldIndices)
            {
                if (!field || !field->IsStruct() || f >= field->fields.size())
                    return fail("StructGet field path is invalid");
                field = types.Get(field->fields[f].type);
            }
            if (!field || field->id != instruction.type)
                return fail("StructGet result type mismatch");
        }
        else if (instruction.operation == Operation::Select && output->IsStruct())
        {
            if (instruction.inputTypes[0] != kBool || instruction.inputTypes[1] != instruction.type || instruction.inputTypes[2] != instruction.type)
                return fail("Struct Select type mismatch");
        }
        else if (!(output->IsStruct() && (instruction.operation == Operation::Constant || instruction.operation == Operation::Parameter || instruction.operation == Operation::Attribute || instruction.operation == Operation::WriteAttribute)))
        {
            std::vector<VFX::ValueType> inputs;
            for (auto type : instruction.inputTypes)
                inputs.push_back(ToValueType(type));
            VFX::ValueType resolved;
            if (!ResolveOperationType(instruction.operation, ToValueType(instruction.type), inputs, resolved, error))
                return false;
            if (FromValueType(resolved) != instruction.type)
                return fail("Bytecode result type disagrees with its operation");
        }
        if (instruction.operation == Operation::Constant || instruction.operation == Operation::Random)
        {
            if (instruction.literal.type != instruction.type || !types.ValidateValue(instruction.literal, error))
                return fail("Invalid typed literal");
            if (IsScalarOrVector(output->kind) && !VFX::ValidValue(instruction.literal.scalar, ToValueType(instruction.type)))
                return fail("Invalid typed literal value");
        }
        if (instruction.operation == Operation::Parameter)
        {
            if (instruction.parameter >= program.parameters.entries.size())
                return fail("Invalid bytecode parameter slot");
            const auto& entry = program.parameters.entries[instruction.parameter];
            if (entry.type != instruction.type || entry.name != instruction.name || entry.defaultValue.type != instruction.literal.type || !SameValue(entry.defaultValue.scalar, instruction.literal.scalar))
                return fail("Inconsistent bytecode parameter binding");
        }
        if (instruction.operation == Operation::Attribute || instruction.operation == Operation::WriteAttribute)
        {
            if (instruction.attribute >= program.attributes.size() || program.attributes[instruction.attribute].type != instruction.type || program.attributes[instruction.attribute].name != instruction.name)
                return fail("Invalid bytecode attribute binding");
            if (instruction.operation == Operation::Attribute)
                reads.insert(instruction.name);
            if (instruction.operation == Operation::WriteAttribute && !writes.insert(instruction.name).second)
                return fail("Attribute has more than one writer: " + instruction.name);
        }
        if (instruction.operation == Operation::DataInterfaceCall)
        {
            std::vector<VFX::ValueType> inputs;
            for (auto type : instruction.inputTypes)
                inputs.push_back(ToValueType(type));
            DataInterfaceCallBinding call;
            if (!ResolveDataInterfaceCall(program.dataInterfaces, instruction.name, inputs, ToValueType(instruction.type), call, error))
                return false;
            if (call.index != instruction.attribute || static_cast<uint32_t>(call.method) != instruction.randomStream)
                return fail("Data interface instruction binding mismatch");
        }
    }
    for (const auto& attribute : program.attributes)
        if (attribute.read != reads.contains(attribute.name) || attribute.write != writes.contains(attribute.name))
            return fail("Bytecode attribute access flags disagree with its instructions");

    error.clear();
    return true;
}

inline bool CompileToBytecode(const Graph& graph, const TypeRegistry& types,
                              const OptimizationResult& layout, BytecodeProgram& program, std::string& error)
{
    program = {};
    VFX::Graph declarations;
    declarations.dataInterfaces = graph.dataInterfaces;
    if (!ValidateGraphDeclarations(declarations, error))
        return false;
    program.stage = graph.stage;
    program.dataInterfaces = graph.dataInterfaces;
    program.parameters = layout.parameters;
    program.attributes = layout.attributes;
    program.registerCount = static_cast<uint32_t>(graph.nodes.size());
    const auto findParameter = [&](std::string_view name) -> int32_t
    {
        const auto found = std::find_if(layout.parameters.entries.begin(), layout.parameters.entries.end(), [&](const ParameterMapEntry& entry)
                                        { return entry.name == name; });
        return found == layout.parameters.entries.end() ? -1 : static_cast<int32_t>(found - layout.parameters.entries.begin());
    };
    const auto findAttribute = [&](std::string_view name) -> int32_t
    {
        const auto found = std::find_if(layout.attributes.begin(), layout.attributes.end(), [&](const AttributeSlot& slot)
                                        { return slot.name == name; });
        return found == layout.attributes.end() ? -1 : static_cast<int32_t>(found - layout.attributes.begin());
    };
    for (uint32_t index = 0; index < graph.nodes.size(); ++index)
    {
        const auto& source = graph.nodes[index];
        if (source.inputs.size() > 64)
        {
            error = "Bytecode node has too many inputs";
            return false;
        }
        BytecodeInstruction instruction;
        instruction.operation = source.operation;
        instruction.type = source.type;
        instruction.destination = index;
        instruction.name = source.name;
        instruction.literal = source.value;
        instruction.randomStream = source.randomStream;
        instruction.inputCount = static_cast<uint32_t>(source.inputs.size());
        instruction.inputs.resize(instruction.inputCount);
        instruction.inputTypes.resize(instruction.inputCount);
        instruction.fieldIndices = source.fieldIndices;
        instruction.literal.type = source.type;
        for (uint32_t inputIndex = 0; inputIndex < instruction.inputCount; ++inputIndex)
        {
            const auto input = source.inputs[inputIndex];
            if (input >= index)
            {
                error = "Bytecode node references a forward or invalid input";
                return false;
            }
            instruction.inputs[inputIndex] = input;
            instruction.inputTypes[inputIndex] = graph.nodes[input].type;
        }
        const auto* outputType = types.Get(source.type);
        if (!outputType)
        {
            error = "Bytecode node has an unknown type";
            return false;
        }
        if (source.operation == Operation::StructMake)
        {
            if (!outputType->IsStruct() || outputType->fields.size() != source.inputs.size())
            {
                error = "StructMake field count/type mismatch";
                return false;
            }
            for (size_t f = 0; f < source.inputs.size(); ++f)
                if (instruction.inputTypes[f] != outputType->fields[f].type)
                {
                    error = "StructMake field type mismatch";
                    return false;
                }
        }
        else if (source.operation == Operation::StructGet)
        {
            if (source.inputs.size() != 1 || source.fieldIndices.empty())
            {
                error = "StructGet requires one struct and a field path";
                return false;
            }
            const auto* field = types.Get(instruction.inputTypes[0]);
            for (const auto selected : source.fieldIndices)
            {
                if (!field || !field->IsStruct() || selected >= field->fields.size())
                {
                    error = "StructGet field path is invalid";
                    return false;
                }
                field = types.Get(field->fields[selected].type);
            }
            if (!field || field->id != source.type)
            {
                error = "StructGet result type mismatch";
                return false;
            }
        }
        else if (source.operation == Operation::DataInterfaceCall)
        {
            std::vector<VFX::ValueType> inputs;
            for (auto type : instruction.inputTypes)
                inputs.push_back(ToValueType(type));
            DataInterfaceCallBinding call;
            if (!ResolveDataInterfaceCall(graph.dataInterfaces, source.name, inputs, ToValueType(source.type), call, error))
                return false;
            instruction.attribute=call.index;instruction.randomStream=static_cast<uint32_t>(call.method);
        }
        if (source.operation == Operation::Parameter)
        {
            const auto parameter = findParameter(source.name);
            if (parameter < 0) { error = "Bytecode parameter is missing from its layout: " + source.name; return false; }
            instruction.parameter = static_cast<uint32_t>(parameter);
            instruction.literal = layout.parameters.entries[static_cast<size_t>(parameter)].defaultValue;
        }
        else if (source.operation == Operation::Attribute || source.operation == Operation::WriteAttribute)
        {
            const auto attribute = findAttribute(source.name);
            if (attribute < 0) { error = "Bytecode attribute is missing from its layout: " + source.name; return false; }
            instruction.attribute = static_cast<uint32_t>(attribute);
        }
        if (!types.Get(source.type)) { error = "Bytecode node has an unknown type"; return false; }
        program.instructions.push_back(std::move(instruction));
    }
    return ValidateBytecodeProgram(program, types, error);
}


struct BytecodeExecutionContext
{
    const TypeRegistry* types = nullptr;
    const std::unordered_map<std::string, VFX::Value>* boundParameters = nullptr;
    std::unordered_map<std::string,Value> parameters;
    std::unordered_map<std::string,Value> attributes;
    float deltaTime = 0;
    uint32_t seed = 0;
    uint64_t tick = 0;
    uint32_t stageIndex = 0;
    uint32_t stageIteration = 0;
    uint32_t spawnIndex = 0;
    uint64_t particleId = 0;
    // Optional particle-backed execution. When set, attribute instructions
    // read/write the bound ParticleDataSet instead of the string map.
    ParticleDataSet* particleData = nullptr;
    uint32_t particleIndex = 0;
    std::vector<int32_t> attributeBindings;
};

inline bool BindBytecodeAttributes(const BytecodeProgram& program,const ParticleDataSet& data,
    std::vector<int32_t>& bindings,std::string& error)
{
    bindings.assign(program.attributes.size(),-1);
    for (size_t index = 0; index < program.attributes.size(); ++index)
    {
        bindings[index] = data.FindAttribute(program.attributes[index].name);
        if (bindings[index] < 0 || FromValueType(data.Attributes()[bindings[index]].type) != program.attributes[index].type)
        { error = "Particle data is missing typed bytecode attribute: " + program.attributes[index].name; return false; }
    }
    error.clear();
    return true;
}

inline float TypedRandom(uint64_t particle,uint32_t seed,uint64_t tick,uint32_t stream)
{
    uint64_t value = particle ^ (uint64_t(seed) << 32) ^ tick ^ (uint64_t(stream)*0x9e3779b97f4a7c15ull);
    value ^= value >> 30; value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27; value *= 0x94d049bb133111ebull;
    value ^= value >> 31;
    return static_cast<float>(value >> 40)*(1.0f/16777216.0f);
}

inline Value ZeroValue(TypeId type)
{
    Value value; value.type = type; return value;
}

inline bool ExecuteBytecodeInstructions(const BytecodeProgram& program, BytecodeExecutionContext& context, std::string& error)
{
    if (!context.types) { error = "Bytecode execution has no type registry"; return false; }
    std::vector<Value> registers(program.registerCount);
    for (auto& registerValue : registers) registerValue.type = kInvalidType;
    const auto readRegister = [&](uint32_t index,Value& output) -> bool
    {
        if (index >= registers.size()) { error = "Bytecode reads an invalid register"; return false; }
        output = registers[index]; return true;
    };
    const auto inputWidth = [&](const BytecodeInstruction& instruction,uint32_t slot) -> uint32_t
    {
        const auto* type = context.types->Get(instruction.inputTypes[slot]);
        return type ? TypeWidth(type->kind) : 1u;
    };
    const auto inputValue = [&](const BytecodeInstruction& instruction,uint32_t slot,uint32_t component) -> float
    {
        const auto& value = registers[instruction.inputs[slot]];
        return value.scalar[component < inputWidth(instruction,slot) ? component : 0];
    };
    for (const auto& instruction : program.instructions)
    {
        if(instruction.destination>=registers.size() || instruction.inputCount!=instruction.inputs.size() || instruction.inputCount!=instruction.inputTypes.size())
        {error="Invalid bytecode register/input layout";return false;}
        for(const auto input:instruction.inputs) if(input>=instruction.destination || registers[input].type==kInvalidType)
        {error="Bytecode reads an uninitialized register";return false;}
        const auto* outputType = context.types->Get(instruction.type);
        if (!outputType) { error = "Bytecode instruction has an unknown result type"; return false; }
        Value result; result.type = instruction.type;
        switch (instruction.operation)
        {
        case Operation::Constant:
            result = instruction.literal;
            break;
        case Operation::Parameter:
            if (instruction.parameter >= program.parameters.entries.size())
            { error = "Bytecode parameter index is invalid"; return false; }
            {
                const auto& entry = program.parameters.entries[instruction.parameter];
                const auto found = context.parameters.find(entry.name);
                result = found == context.parameters.end() ? entry.defaultValue : found->second;
                if (found == context.parameters.end() && context.boundParameters)
                    if (const auto bound = context.boundParameters->find(entry.name); bound != context.boundParameters->end())
                        result = {entry.type, bound->second};
            }
            break;
        case Operation::Attribute:
            if (instruction.attribute >= program.attributes.size())
            { error = "Bytecode attribute index is invalid"; return false; }
            {
                const auto& attribute = program.attributes[instruction.attribute];
                if (context.particleData && instruction.attribute < context.attributeBindings.size() &&
                    context.attributeBindings[instruction.attribute] >= 0)
                {
                    const auto legacy = context.particleData->Read(
                        static_cast<uint32_t>(context.attributeBindings[instruction.attribute]),context.particleIndex);
                    result.type = attribute.type;
                    result.scalar = legacy;
                }
                else
                {
                    const auto found = context.attributes.find(attribute.name);
                    result = found == context.attributes.end() ? ZeroValue(attribute.type) : found->second;
                }
            }
            break;
        case Operation::WriteAttribute:
            if (instruction.attribute >= program.attributes.size())
            { error = "Bytecode write attribute index is invalid"; return false; }
            {
                Value value;
                if (!readRegister(instruction.inputs[0],value)) return false;
                const auto* inputType = context.types->Get(value.type);
                const auto* attributeType = context.types->Get(instruction.type);
                if (inputType && attributeType && TypeWidth(*inputType) == 1 && TypeWidth(*attributeType) > 1)
                    value.scalar.fill(value.scalar[0]);
                value.type = instruction.type;
                if (context.particleData && instruction.attribute < context.attributeBindings.size() &&
                    context.attributeBindings[instruction.attribute] >= 0)
                    context.particleData->Write(
                        static_cast<uint32_t>(context.attributeBindings[instruction.attribute]),context.particleIndex,value.scalar);
                else
                    context.attributes[program.attributes[instruction.attribute].name] = value;
                result = std::move(value);
            }
            break;
        case Operation::Add: case Operation::Subtract: case Operation::Multiply: case Operation::Divide:
        case Operation::Minimum: case Operation::Maximum:
        {
            const auto width = TypeWidth(*outputType);
            for (uint32_t component = 0; component < width; ++component)
            {
                const auto a = inputValue(instruction,0,component);
                const auto b = inputValue(instruction,1,component);
                if (outputType->kind == TypeKind::Int32)
                {
                    const auto ai = TypedInt(a), bi = TypedInt(b);
                    const auto ua = std::bit_cast<uint32_t>(ai), ub = std::bit_cast<uint32_t>(bi);
                    const auto value = instruction.operation == Operation::Add      ? std::bit_cast<int32_t>(ua + ub) :
                                       instruction.operation == Operation::Subtract ? std::bit_cast<int32_t>(ua - ub) :
                                       instruction.operation == Operation::Multiply ? std::bit_cast<int32_t>(ua * ub) :
                                       instruction.operation == Operation::Divide   ? (bi == 0 ? 0 : ai == INT32_MIN && bi == -1 ? ai :
                                                                                                                                   ai / bi) :
                                       instruction.operation == Operation::Minimum  ? (std::min)(ai, bi) :
                                                                                      (std::max)(ai, bi);
                    result.scalar[component] = TypedFloat(value);
                }
                else
                {
                    result.scalar[component] = instruction.operation == Operation::Add ? a+b :
                        instruction.operation == Operation::Subtract ? a-b :
                        instruction.operation == Operation::Multiply ? a*b :
                        instruction.operation == Operation::Divide ? (std::abs(b) < 1e-20f ? 0.0f : a/b) :
                        instruction.operation == Operation::Minimum ? (std::min)(a,b) : (std::max)(a,b);
                }
            }
            break;
        }
        case Operation::Select:
        {
            Value condition;
            if (!readRegister(instruction.inputs[0],condition)) return false;
            const auto selected = instruction.inputs[condition.scalar[0] != 0 ? 1 : 2];
            if (selected >= registers.size()) { error = "Bytecode Select input is invalid"; return false; }
            result = registers[selected];
            result.type = instruction.type;
            break;
        }
        case Operation::Convert:
        {
            const auto* inputType = context.types->Get(instruction.inputTypes[0]);
            const auto inputIsIntegral = inputType &&
                (inputType->kind == TypeKind::Bool || inputType->kind == TypeKind::Int32);
            const auto width = TypeWidth(*outputType);
            for (uint32_t component = 0; component < width; ++component)
            {
                const auto raw = inputValue(instruction,0,component);
                const auto value = inputType && inputType->kind == TypeKind::Bool ? (TypedInt(raw) != 0 ? 1.0 : 0.0) : inputIsIntegral ? static_cast<double>(TypedInt(raw)) :
                                                                                                                                         static_cast<double>(raw);
                if (outputType->kind == TypeKind::Bool)
                    result.scalar[component] = TypedBool(value != 0);
                else if (outputType->kind == TypeKind::Int32)
                {
                    if (inputIsIntegral)
                        result.scalar[component] = TypedFloat(static_cast<int32_t>(value));
                    else result.scalar[component] = TypedFloat(!std::isfinite(value) ? 0 :
                        value >= 2147483647.0 ? INT32_MAX : value <= -2147483648.0 ? INT32_MIN : static_cast<int32_t>(value));
                }
                else
                    result.scalar[component] = static_cast<float>(value);
            }
            break;
        }
        case Operation::DeltaTime:
            result.scalar[0] = context.deltaTime;
            break;
        case Operation::StageIndex:
            result.scalar[0] = outputType->kind == TypeKind::Int32 ? TypedFloat(static_cast<int32_t>(context.stageIndex)) : static_cast<float>(context.stageIndex);
            break;
        case Operation::StageIteration:
            result.scalar[0] = outputType->kind == TypeKind::Int32 ? TypedFloat(static_cast<int32_t>(context.stageIteration)) : static_cast<float>(context.stageIteration);
            break;
        case Operation::SpawnIndex:
            result.scalar[0] = outputType->kind == TypeKind::Int32 ? TypedFloat(static_cast<int32_t>(context.spawnIndex)) : static_cast<float>(context.spawnIndex);
            break;
        case Operation::NormalizedAge:
        {
            if (context.particleData)
            {
                const auto age = context.particleData->FindAttribute("Particles.Age"), lifetime = context.particleData->FindAttribute("Particles.Lifetime");
                result.scalar[0] = age >= 0 && lifetime >= 0 ? std::clamp(context.particleData->Read(age, context.particleIndex)[0] / (std::max)(context.particleData->Read(lifetime, context.particleIndex)[0], 1e-20f), 0.0f, 1.0f) : 0;
                break;
            }
            const auto age = context.attributes.find("Particles.Age");
            const auto lifetime = context.attributes.find("Particles.Lifetime");
            result.scalar[0] = age != context.attributes.end() && lifetime != context.attributes.end()
                ? std::clamp(age->second.scalar[0]/(std::max)(lifetime->second.scalar[0],1e-20f),0.0f,1.0f) : 0;
            break;
        }
        case Operation::Random:
        {
            const auto stream = instruction.randomStream ^
                (program.stage == ScriptStage::SimulationStage ?
                    RandomHash(context.stageIndex) ^ RandomHash(context.stageIteration+1) : 0u);
            const auto tick = program.stage == ScriptStage::ParticleSpawn ? 0 : context.tick;
            const auto width = TypeWidth(*outputType);
            for (uint32_t component = 0; component < width; ++component)
                result.scalar[component] = (RandomFloat(context.particleId,context.seed,stream,tick,component)*2-1)*
                    instruction.literal.scalar[component];
            break;
        }
        case Operation::Sine: case Operation::Cosine: case Operation::Absolute: case Operation::SquareRoot:
        {
            const auto width = TypeWidth(*outputType);
            for (uint32_t component = 0; component < width; ++component)
            {
                const auto value = inputValue(instruction,0,component);
                if (outputType->kind == TypeKind::Int32 && instruction.operation == Operation::Absolute)
                {
                    const auto raw = TypedInt(value);
                    result.scalar[component] = TypedFloat(raw == INT32_MIN ? raw : raw < 0 ? -raw :
                                                                                             raw);
                    continue;
                }
                result.scalar[component] = instruction.operation == Operation::Sine ? std::sin(value) :
                    instruction.operation == Operation::Cosine ? std::cos(value) :
                    instruction.operation == Operation::Absolute ? std::abs(value) : std::sqrt((std::max)(0.0f,value));
            }
            break;
        }
        case Operation::Clamp:
        case Operation::Lerp:
        {
            const auto width = TypeWidth(*outputType);
            const auto integral = outputType->kind == TypeKind::Bool || outputType->kind == TypeKind::Int32;
            for (uint32_t component = 0; component < width; ++component)
            {
                const auto a = inputValue(instruction,0,component);
                const auto b = inputValue(instruction,1,component);
                const auto c = inputValue(instruction,2,component);
                if (integral && instruction.operation == Operation::Clamp)
                    result.scalar[component] = TypedFloat((std::min)((std::max)(TypedInt(a),TypedInt(b)),TypedInt(c)));
                else
                    result.scalar[component] = instruction.operation == Operation::Clamp ?
                        (std::min)((std::max)(a,b),c) : a+(b-a)*c;
            }
            break;
        }
        case Operation::Normalize:
        case Operation::Length:
        case Operation::Dot:
        case Operation::Cross:
        {
            const auto aw = inputWidth(instruction,0);
            if (instruction.operation == Operation::Cross)
            {
                if (aw < 3 || inputWidth(instruction,1) < 3) { error = "Bytecode Cross requires Float3 inputs"; return false; }
                const auto ax = inputValue(instruction,0,0), ay = inputValue(instruction,0,1), az = inputValue(instruction,0,2);
                const auto bx = inputValue(instruction,1,0), by = inputValue(instruction,1,1), bz = inputValue(instruction,1,2);
                result.scalar[0] = ay*bz-az*by; result.scalar[1] = az*bx-ax*bz; result.scalar[2] = ax*by-ay*bx;
                break;
            }
            float dot = 0;
            if (instruction.operation == Operation::Dot)
            {
                const auto bw = inputWidth(instruction,1);
                for (uint32_t component = 0; component < (std::max)(aw,bw); ++component)
                    dot += inputValue(instruction,0,component)*inputValue(instruction,1,component);
            }
            else for (uint32_t component = 0; component < aw; ++component)
                dot += inputValue(instruction,0,component)*inputValue(instruction,0,component);
            if (instruction.operation == Operation::Dot || instruction.operation == Operation::Length)
            {
                result.scalar[0] = instruction.operation == Operation::Length ? std::sqrt((std::max)(0.0f,dot)) : dot;
                break;
            }
            const auto length = std::sqrt((std::max)(0.0f,dot));
            const auto width = TypeWidth(*outputType);
            if (length > 1e-20f)
                for (uint32_t component = 0; component < width; ++component)
                    result.scalar[component] = inputValue(instruction,0,component)/length;
            break;
        }
        case Operation::Equal: case Operation::NotEqual:
        case Operation::LessThan: case Operation::LessEqual:
        case Operation::GreaterThan: case Operation::GreaterEqual:
        {
            const auto* leftType = context.types->Get(instruction.inputTypes[0]);
            const auto* rightType = context.types->Get(instruction.inputTypes[1]);
            const auto integral = leftType && rightType &&
                (leftType->kind == TypeKind::Bool || leftType->kind == TypeKind::Int32) &&
                (rightType->kind == TypeKind::Bool || rightType->kind == TypeKind::Int32);
            const auto a = integral ? static_cast<double>(TypedInt(inputValue(instruction,0,0))) :
                static_cast<double>(inputValue(instruction,0,0));
            const auto b = integral ? static_cast<double>(TypedInt(inputValue(instruction,1,0))) :
                static_cast<double>(inputValue(instruction,1,0));
            const auto value = instruction.operation == Operation::Equal ? a == b :
                instruction.operation == Operation::NotEqual ? a != b :
                instruction.operation == Operation::LessThan ? a < b :
                instruction.operation == Operation::LessEqual ? a <= b :
                instruction.operation == Operation::GreaterThan ? a > b : a >= b;
            result.scalar[0] = TypedBool(value);
            break;
        }
        case Operation::LogicalAnd:
        case Operation::LogicalOr:
        {
            const auto a = inputValue(instruction,0,0) != 0;
            const auto b = inputValue(instruction,1,0) != 0;
            result.scalar[0] = TypedBool(instruction.operation == Operation::LogicalAnd ? a && b : a || b);
            break;
        }
        case Operation::LogicalNot:
            result.scalar[0] = TypedBool(inputValue(instruction,0,0) == 0);
            break;
        case Operation::StructMake:
            for (const auto input : instruction.inputs) result.fields.push_back(registers[input]);
            break;
        case Operation::StructGet:
            result=registers[instruction.inputs[0]];
            for (const auto field : instruction.fieldIndices)
            {
                if (field>=result.fields.size()) {error="StructGet runtime field is missing";return false;}
                auto selected=result.fields[field];result=std::move(selected);
            }
            break;
        case Operation::DataInterfaceCall:
        {
            if (instruction.attribute>=program.dataInterfaces.size()) {error="Bytecode data interface binding is invalid";return false;}
            const auto& binding=program.dataInterfaces[instruction.attribute];
            std::array<VFX::Value,1> arguments{};
            if(instruction.inputCount)arguments[0]=registers[instruction.inputs[0]].scalar;
            result.scalar = EvaluateDataInterface(binding, static_cast<DataInterfaceMethod>(instruction.randomStream), std::span(arguments.data(), instruction.inputCount), [&](uint32_t element)
                                                  {
                    const auto name=DataInterfaceParameter(binding.name,element);
                    if(const auto value=context.parameters.find(name);value!=context.parameters.end())return value->second.scalar;
                    if(context.boundParameters)if(const auto value=context.boundParameters->find(name);value!=context.boundParameters->end())return value->second;
                    return binding.data[element]; });
            break;
        }
        }
        if (result.type != instruction.type)
        {
            error = "Bytecode result has an incompatible type";
            return false;
        }
        registers[instruction.destination] = std::move(result);
    }
    error.clear();
    return true;
}

inline bool ValidateBytecodeContext(const BytecodeProgram& program, const BytecodeExecutionContext& context, std::string& error)
{
    if (!context.types || !std::isfinite(context.deltaTime))
    {
        error = "Bytecode context has no types or invalid delta time";
        return false;
    }
    if (!ValidateBytecodeProgram(program, *context.types, error))
        return false;
    for (const auto& entry : program.parameters.entries)
    {
        if (const auto found = context.parameters.find(entry.name); found != context.parameters.end())
        {
            if (found->second.type != entry.type || !context.types->ValidateValue(found->second, error))
            {
                error = "Typed parameter type mismatch: " + entry.name;
                return false;
            }
            const auto type = ToValueType(entry.type);
            if (VFX::IsValueType(type) && !VFX::ValidValue(found->second.scalar, type))
            {
                error = "Invalid typed parameter value: " + entry.name;
                return false;
            }
        }
        else if (context.boundParameters)
            if (const auto found = context.boundParameters->find(entry.name); found != context.boundParameters->end())
                if (!VFX::ValidValue(found->second, ToValueType(entry.type)))
                {
                    error = "Invalid bound parameter: " + entry.name;
                    return false;
                }
    }
    return true;
}
inline bool ExecuteBytecode(const BytecodeProgram& program, BytecodeExecutionContext& context, std::string& error)
{
    return ValidateBytecodeContext(program, context, error) && ExecuteBytecodeInstructions(program, context, error);
}

inline bool ExecuteBytecodeParticles(const BytecodeProgram& program,ParticleDataSet& data,
    uint32_t first,uint32_t count,BytecodeExecutionContext& context,std::string& error)
{
    if (!ValidateBytecodeContext(program, context, error))
        return false;
    if (context.attributeBindings.size() != program.attributes.size())
    { error = "Typed bytecode particle attribute bindings are incomplete"; return false; }
    if (first > data.Count() || count > data.Count()-first)
    { error = "Typed bytecode particle range is invalid"; return false; }
    for (size_t index = 0; index < context.attributeBindings.size(); ++index)
        if (context.attributeBindings[index] < 0 || static_cast<size_t>(context.attributeBindings[index]) >= data.Attributes().size() || FromValueType(data.Attributes()[context.attributeBindings[index]].type) != program.attributes[index].type)
        {
            error = "Typed bytecode attribute type mismatch";
            return false;
        }
    const auto firstSpawnIndex = context.spawnIndex;
    context.particleData = &data;
    for (uint32_t index = 0; index < count; ++index)
    {
        context.spawnIndex = firstSpawnIndex + index;
        context.particleIndex = first+index;
        context.particleId = data.Id(context.particleIndex);
        if (!ExecuteBytecodeInstructions(program, context, error))
        { context.particleData = nullptr; return false; }
    }
    context.particleData = nullptr;
    context.spawnIndex = firstSpawnIndex;
    error.clear();
    return true;
}
// Shared CPU runtime entry. Programs are already bytecode; binding resolves
// stable attribute names against the current SoA without translating programs.
inline bool ExecuteParticleProgram(const BytecodeProgram& program, ParticleDataSet& data, uint32_t first, uint32_t count,
                                   const VFX::ExecutionContext& execution, std::string& error, std::span<const int32_t> bindings = {})
{
    TypeRegistry types;
    BytecodeExecutionContext context;
    context.types = &types;
    context.boundParameters = execution.parameters;
    context.deltaTime = execution.deltaTime;
    context.seed = execution.seed;
    context.tick = execution.tick;
    context.stageIndex = execution.stageIndex;
    context.stageIteration = execution.stageIteration;
    context.spawnIndex = execution.spawnIndex;
    if (bindings.empty())
    {
        if (!BindBytecodeAttributes(program, data, context.attributeBindings, error))
            return false;
    }
    else
        context.attributeBindings.assign(bindings.begin(), bindings.end());
    return ExecuteBytecodeParticles(program, data, first, count, context, error);
}
} // namespace NLS::VFX::Typed
