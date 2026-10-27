#pragma once
#include "VfxGraph.h"
#include <algorithm>

namespace NLS::VFX
{
inline bool ResolveOperationType(Operation operation,ValueType declared,std::span<const ValueType> inputs,ValueType& output,std::string& error)
{
    const auto fail = [&](const char* text) { error = text; return false; };
    if (!IsValueType(declared) || operation > Operation::DataInterfaceCall || !ValidInputCount(operation,inputs.size())) return fail("Invalid operation, type or input count");
    for (auto type : inputs) if (!IsValueType(type)) return fail("Invalid input type");
    output = declared;
    if (operation == Operation::DataInterfaceCall) return true;
    if (operation == Operation::StructMake || operation == Operation::StructGet) return fail("Aggregate operations must be lowered before scalar type resolution");
    if (operation == Operation::Convert)
    {
        if (ComponentCount(inputs[0]) != 1 || ComponentCount(declared) != 1) return fail("Convert requires scalar types");
        return true;
    }
    if (operation == Operation::Select)
    {
        if (inputs[0] != ValueType::Bool || inputs[1] != inputs[2]) return fail("Select requires Bool condition and matching branch types");
        output = inputs[1]; return true;
    }
    if (operation >= Operation::Equal && operation <= Operation::GreaterEqual)
    {
        if (inputs[0] != inputs[1] || ComponentCount(inputs[0]) != 1 ||
            (inputs[0] == ValueType::Bool && operation != Operation::Equal && operation != Operation::NotEqual)) return fail("Comparison requires matching scalar numeric types");
        output = ValueType::Bool; return true;
    }
    if (operation == Operation::LogicalAnd || operation == Operation::LogicalOr || operation == Operation::LogicalNot)
    {
        for (auto type : inputs) if (type != ValueType::Bool) return fail("Logical operations require Bool inputs");
        output = ValueType::Bool; return true;
    }
    if (operation == Operation::WriteAttribute)
    {
        if (IsIntegralType(declared) || IsIntegralType(inputs[0]))
            return inputs[0] == declared || fail("Attribute output requires an explicit type conversion");
        return ComponentCount(inputs[0]) == 1 || inputs[0] == declared || fail("Attribute output dimensions differ");
    }
    if (operation == Operation::StageIndex || operation == Operation::StageIteration)
        return declared == ValueType::Float || declared == ValueType::Int32 || fail("Stage context requires Float or Int32");
    if (operation == Operation::SpawnIndex)
        return declared == ValueType::Float || declared == ValueType::Int32 || fail("Spawn index requires Float or Int32");
    if (operation == Operation::DeltaTime || operation == Operation::NormalizedAge) { output = ValueType::Float; return true; }
    if (operation == Operation::Random) return IsFloatType(declared) || fail("Random currently requires a floating point type");
    if (inputs.empty()) return true;
    bool integral = false; for (auto type : inputs) integral |= IsIntegralType(type);
    if (integral)
    {
        for (auto type : inputs) if (type != ValueType::Int32) return fail("Integer arithmetic requires Int32 inputs; use Convert for other types");
        if (operation != Operation::Add && operation != Operation::Subtract && operation != Operation::Multiply && operation != Operation::Divide &&
            operation != Operation::Minimum && operation != Operation::Maximum && operation != Operation::Clamp && operation != Operation::Absolute) return fail("Operation does not support Int32");
        output = ValueType::Int32; return true;
    }
    uint32_t width = 1; for (auto type : inputs) width = (std::max)(width,ComponentCount(type));
    for (auto type : inputs) if (ComponentCount(type) != 1 && ComponentCount(type) != width) return fail("Input vector dimensions differ");
    output = static_cast<ValueType>(width-1);
    if (operation == Operation::Length || operation == Operation::Dot) output = ValueType::Float;
    if (operation == Operation::Dot && inputs[0] != inputs[1]) return fail("Dot inputs must have equal dimensions");
    if (operation == Operation::Cross && (inputs[0] != ValueType::Float3 || inputs[1] != ValueType::Float3)) return fail("Cross requires two float3 inputs");
    return true;
}
}
