#pragma once
#include "Serialize/IJsonHandler.h"

namespace NLS
{
class GameObjectSerializeHandler final : public IJsonHandler
{
public:
    virtual void SerializeImpl(const ObjectView& data, json& output) const;

    // 反序列化有两种情况：
    // 一是在input中包含了对象类型，例如该对象是Json中的顶层对象。
    // 二是该对象是行内对象，即是某对象的成员变量，此时对象的类型由外部调用者提供
    virtual void DeserializeImpl(const ObjectView& data, const json& input) const;

    // 返回值越小匹配程度越高
    virtual uint32_t CalcMatchLevel(const Type& type, bool isPointer) const;
};
} // namespace LUM