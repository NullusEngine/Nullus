
#include "CommonSerialize.h"
#include "Serializer.h"
#include "Debug/Assertion.h"
namespace NLS
{
void CommonValueHandler::SerializeImpl(const ObjectView& obj, json& j) const
{
    auto typeName = obj.GetType().GetName();

    if (type_name_is_arithmetic(typeName))
    {
        // 基本类型直接序列化
        if (typeName == Type_of<int>.GetName())
            j = obj.As<int>();
        else if (typeName == Type_of<double>.GetName())
            j = obj.As<double>();
        else if (typeName == Type_of<float>.GetName())
            j = obj.As<float>();
        // 其他基本类型的处理...
    }
    else
    {
        j["TYPE"] = typeName;
        auto* info = Mngr.GetTypeInfo(obj.GetType());
        auto iter = info->attrs.find(Type_of<ContainerType>);
        if (iter != info->attrs.end() && *iter == ContainerType::Vector)
        {
            // 处理 std::vector 类型
            j["Array"] = json::array();
            for (size_t i = 0; i < obj.size(); i++)
            {
                json elem;
                Serializer::Instance()->SerializeObject(obj[i].RemoveReference() ,elem);
                j["Array"].push_back(elem);
            }
        }
        else
        {
            // 普通对象序列化
            auto vars = obj.GetVars();
            for (auto& [name, var] : vars)
            {
                Serializer::Instance()->SerializeObject(var, j[name.GetView()]);
            }
        }
    }
}

void CommonValueHandler::DeserializeImpl(const ObjectView& obj, const json& input) const
{
    auto typeName = obj.GetType().GetName();

    if (type_name_is_arithmetic(typeName))
    {
        // 基本类型反序列化
        if (typeName == Type_of<int>.GetName())
            obj.As<int>() = input.get<int>();
        else if (typeName == Type_of<double>.GetName())
            obj.As<double>() = input.get<double>();
        else if (typeName == Type_of<float>.GetName())
            obj.As<float>() = input.get<float>();
        // 其他基本类型的处理...
    }
    else
    {
        auto* info = Mngr.GetTypeInfo(obj.GetType());
        auto iter = info->attrs.find(Type_of<ContainerType>);
        if (iter != info->attrs.end() && *iter == ContainerType::Vector)
        {
            // 处理 std::vector 类型
            auto elemType = obj.GetType().GetElementType();
            for (auto& elemJson : input.at("Array"))
            {
                ObjectView elem = Mngr.New(elemType);
                Serializer::Instance()->DeserializeObject(elem, elemJson);
                obj.push_back(elem);
            }
        }
        else
        {
            // 普通对象反序列化
            for (auto& [name, varJson] : input.items())
            {
                if (name == "TYPE")
                    continue; // 忽略类型信息
                auto var = obj.Var(std::string_view(name));
                if (var.Valid())
                {
                    Serializer::Instance()->DeserializeObject(var, varJson);
                }
                else
                {
                    NLS_ASSERT(false, "Unknown variable name during deserialization: " + name);
                }
            }
        }
    }
}


uint32_t CommonValueHandler::CalcMatchLevel(const Type& type, bool isPointer) const
{
    if (!type.Valid())
        return IJsonHandler::NoMatch;

    return DefaultMatch;
}
} // namespace NLS
