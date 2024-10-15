#pragma once
#include "CoreDef.h"
#include <Json/json.hpp>
#include "UDRefl/UDRefl.hpp"
#include <limits>
namespace NLS
{
using json = nlohmann::json;
using namespace UDRefl;
class NLS_CORE_API IJsonHandler
{
public:
    constexpr static uint32_t NoMatch = (std::numeric_limits<uint32_t>::max)();
    constexpr static uint32_t DefaultMatch = NoMatch / 2;

    IJsonHandler() = default;
    virtual ~IJsonHandler() = default;


    virtual void SerializeImpl(const ObjectView& data, json& output) const = 0;

    // 反序列化有两种情况：
    // 一是在input中包含了对象类型，例如该对象是Json中的顶层对象。
    // 二是该对象是行内对象，即是某对象的成员变量，此时对象的类型由外部调用者提供
    virtual void DeserializeImpl(const ObjectView& data, const json& input) const = 0;

    // 返回值越小匹配程度越高
    virtual uint32_t CalcMatchLevel(const Type& type, bool isPointer) const = 0;
};
} // namespace NLS