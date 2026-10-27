#pragma once
#include "VfxDataInterface.h"
#include "VfxValueSerialization.h"
#include <Json/json.hpp>
#include <stdexcept>

namespace NLS::VFX::Schema
{
using Json=nlohmann::json;
inline Json EncodeDataInterfaces(std::span<const DataInterfaceDefinition> bindings)
{
    Json list=Json::array();
    for(const auto& binding:bindings)
    {
        Json data=Json::array();for(const auto& value:binding.data)data.push_back(Detail::EncodeValue(value,DataInterfaceStorageType(binding)));
        list.push_back({{"name",binding.name},{"kind",static_cast<uint32_t>(binding.kind)},
            {"elementType",static_cast<uint32_t>(binding.elementType)},{"data",std::move(data)}});
    }
    return list;
}
inline std::vector<DataInterfaceDefinition> DecodeDataInterfaces(const Json& list)
{
    if(!list.is_array() || list.size()>64)throw std::runtime_error("Invalid data interface list");
    Graph graph;
    for(const auto& row:list)
    {
        const auto kind=row.at("kind").get<uint32_t>(),type=row.at("elementType").get<uint32_t>();
        if(kind>1 || type>static_cast<uint32_t>(ValueType::Bool) || !row.at("data").is_array() || row.at("data").size()>4096)
            throw std::runtime_error("Invalid data interface kind, type or record count");
        DataInterfaceDefinition binding;binding.name=row.at("name").get<std::string>();binding.kind=static_cast<DataInterfaceKind>(kind);binding.elementType=static_cast<ValueType>(type);
        for(const auto& value:row.at("data"))binding.data.push_back(Detail::DecodeValue(value,DataInterfaceStorageType(binding)));
        graph.dataInterfaces.push_back(std::move(binding));
    }
    std::string error;if(!ValidateGraphDeclarations(graph,error))throw std::runtime_error(error);
    return graph.dataInterfaces;
}
inline void EncodeGraphDeclarations(Json& result,const Graph& graph)
{
    if(graph.structs.empty() && graph.dataInterfaces.empty())return;
    result["typedSchema"]=1;
    if(!graph.structs.empty())
    {
        result["structs"]=Json::array();
        for(const auto& type:graph.structs)
        {
            Json fields=Json::array();
            for(const auto& field:type.fields)fields.push_back({{"name",field.name},{"type",static_cast<uint32_t>(field.type)},
                {"structType",field.structType},{"default",Detail::EncodeValue(field.defaultValue,field.type)}});
            result["structs"].push_back({{"name",type.name},{"fields",std::move(fields)}});
        }
    }
    if(!graph.dataInterfaces.empty())result["dataInterfaces"]=EncodeDataInterfaces(graph.dataInterfaces);
}
inline void DecodeGraphDeclarations(const Json& source,Graph& graph)
{
    if(source.value("typedSchema",1u)!=1)throw std::runtime_error("Unsupported graph type schema");
    const auto structs=source.value("structs",Json::array());
    if(!structs.is_array() || structs.size()>64)throw std::runtime_error("Invalid struct declaration list");
    for(const auto& row:structs)
    {
        StructDefinition type;type.name=row.at("name").get<std::string>();const auto& fields=row.at("fields");
        if(!fields.is_array() || fields.empty() || fields.size()>64)throw std::runtime_error("Invalid struct field list");
        for(const auto& field:fields)
        {
            const auto scalarType=field.at("type").get<uint32_t>();
            if(scalarType>static_cast<uint32_t>(ValueType::Bool))throw std::runtime_error("Unknown struct field type");
            StructFieldDefinition value;value.name=field.at("name").get<std::string>();value.type=static_cast<ValueType>(scalarType);
            value.structType=field.value("structType",std::string{});value.defaultValue=Detail::DecodeValue(field.at("default"),value.type);type.fields.push_back(std::move(value));
        }
        graph.structs.push_back(std::move(type));
    }
    graph.dataInterfaces=DecodeDataInterfaces(source.value("dataInterfaces",Json::array()));
    std::string error;if(!ValidateGraphDeclarations(graph,error))throw std::runtime_error(error);
}
}
