#pragma once
#include "VfxGraph.h"
#include <limits>
#include <stdexcept>
#include <Json/json.hpp>

namespace NLS::VFX::Detail
{
inline nlohmann::json EncodeValue(const Value& value,ValueType type)
{
    if (type == ValueType::Int32) return ReadInt(value);
    if (type == ValueType::Bool) return ReadBool(value);
    return value;
}
inline Value DecodeValue(const nlohmann::json& source,ValueType type)
{
    Value value{};
    if (type == ValueType::Bool)
    {
        if (!source.is_boolean()) throw std::runtime_error("Bool values require JSON true or false");
        value = BoolValue(source.get<bool>());
    }
    else if (type == ValueType::Int32)
    {
        if (!source.is_number_integer() ||
            (source.is_number_unsigned() && source.get<uint64_t>() > uint64_t(INT32_MAX))) throw std::runtime_error("Expected a signed 32-bit integer");
        const auto integer = source.get<int64_t>();
        if (integer < INT32_MIN || integer > INT32_MAX) throw std::runtime_error("Integer is outside the signed 32-bit range");
        value = IntValue(static_cast<int32_t>(integer));
    }
    else value = source.get<Value>();
    if (!ValidValue(value,type)) throw std::runtime_error("Invalid VFX value or type");
    return value;
}
}
