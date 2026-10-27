#pragma once
#include "VfxProgram.h"

namespace NLS::VFX::Detail
{
using ParameterTypes = std::unordered_map<std::string,ValueType>;
inline bool AddParameterType(ParameterTypes& types,const std::string& name,ValueType type,std::string& error)
{
    if (name.empty() || !IsValueType(type)) { error = "Invalid particle parameter declaration"; return false; }
    const auto [found,added] = types.emplace(name,type);
    if (!added && found->second != type) { error = "Parameter has inconsistent types: "+name; return false; }
    return true;
}
inline bool AddParameterTypes(ParameterTypes& types,const Program& program,std::string& error)
{
    for (const auto& parameter : program.parameters.entries)
        if (!AddParameterType(types, parameter.name, Typed::ToValueType(parameter.type), error))
            return false;
    return true;
}
inline bool AddParameterTypes(ParameterTypes& types,std::span<const ParameterDefinition> parameters,std::string& error)
{
    for (const auto& parameter : parameters)
        if (!ValidValue(parameter.value,parameter.type) || !AddParameterType(types,parameter.name,parameter.type,error)) return false;
    return true;
}
inline bool AddStageControlTypes(ParameterTypes& types,const SimulationStageBindings& bindings,std::string& error)
{
    return (bindings.enabled.empty() || AddParameterType(types,bindings.enabled,bindings.typed ? ValueType::Bool : ValueType::Float,error)) &&
        (bindings.iterations.empty() || AddParameterType(types,bindings.iterations,bindings.typed ? ValueType::Int32 : ValueType::Float,error));
}
inline bool ValidParameter(const ParameterTypes& types,const std::string& name,const Value& value)
{
    const auto found = types.find(name);
    return !name.empty() && ValidValue(value,found == types.end() ? ValueType::Float4 : found->second);
}
}
