#include "MaterialParameterSchema.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <Json/json.hpp>

namespace NLS::Render::Resources
{
std::string SerializeMaterialShaderInterface(const MaterialShaderInterface& shaderInterface)
{
    return nlohmann::json{{"domain",shaderInterface.domain},{"unlit",shaderInterface.unlit},{"forwardOpaque",shaderInterface.forwardOpaque},
        {"wpo",shaderInterface.usesWorldPositionOffset},{"maximumWorldDisplacement",shaderInterface.maximumWorldDisplacement},
        {"blend",static_cast<int>(shaderInterface.blendMode)}}.dump();
}
std::optional<MaterialShaderInterface> DeserializeMaterialShaderInterface(std::string_view text)
{
    try
    {
        const auto root=nlohmann::json::parse(text);MaterialShaderInterface shaderInterface;
        shaderInterface.domain=root.value("domain",std::string("Surface"));shaderInterface.unlit=root.value("unlit",false);
        shaderInterface.forwardOpaque=root.value("forwardOpaque",false);shaderInterface.usesWorldPositionOffset=root.value("wpo",false);
        shaderInterface.maximumWorldDisplacement=root.value("maximumWorldDisplacement",0.f);
        const auto blend=root.value("blend",0);
        if(blend<0 || blend>4 || !std::isfinite(shaderInterface.maximumWorldDisplacement) || shaderInterface.maximumWorldDisplacement<0)return std::nullopt;
        shaderInterface.blendMode=static_cast<MaterialBlendMode>(blend);return shaderInterface;
    }
    catch(const std::exception&){return std::nullopt;}
}
const MaterialParameterDesc* MaterialParameterSchema::Find(std::string_view name) const
{
    const auto it = std::find_if(parameters.begin(), parameters.end(),
        [name](const auto& parameter) { return parameter.name == name; });
    return it == parameters.end() ? nullptr : &*it;
}

const MaterialParameterDesc* MaterialParameterSchema::Find(MaterialParameterId id) const
{
    const auto it = std::find_if(parameters.begin(), parameters.end(),
        [id](const auto& parameter) { return parameter.id == id; });
    return it == parameters.end() ? nullptr : &*it;
}

bool ValidateMaterialParameterValue(UniformType type,const MaterialParameterValue& value)
{
    bool finite=true;
    std::visit([&](const auto& item){using T=std::decay_t<decltype(item)>;
        if constexpr(std::is_same_v<T,float>)finite=std::isfinite(item);
        else if constexpr(std::is_same_v<T,std::array<float,4>> || std::is_same_v<T,std::array<float,16>>)
            finite=std::all_of(item.begin(),item.end(),[](float v){return std::isfinite(v);});},value);
    if(!finite)return false;
    switch(type)
    {
    case UniformType::UNIFORM_BOOL:return std::holds_alternative<bool>(value);
    case UniformType::UNIFORM_INT:return std::holds_alternative<int32_t>(value);
    case UniformType::UNIFORM_FLOAT:return std::holds_alternative<float>(value);
    case UniformType::UNIFORM_FLOAT_VEC2:case UniformType::UNIFORM_FLOAT_VEC3:case UniformType::UNIFORM_FLOAT_VEC4:
        return std::holds_alternative<std::array<float,4>>(value);
    case UniformType::UNIFORM_FLOAT_MAT4:return std::holds_alternative<std::array<float,16>>(value);
    case UniformType::UNIFORM_SAMPLER_2D:case UniformType::UNIFORM_SAMPLER_CUBE:return std::holds_alternative<std::string>(value);
    default:return false;
    }
}

bool MaterialParameterSchema::Validate(std::string& error) const
{
    if (version != 1) { error = "Unsupported material parameter schema version"; return false; }
    std::set<MaterialParameterId> ids;
    std::set<std::string> names;
    for (const auto& parameter : parameters)
    {
        if (!parameter.id || parameter.name.empty() || !ids.insert(parameter.id).second ||
            !names.insert(parameter.name).second)
        { error = "Duplicate or empty material parameter identity: " + parameter.name; return false; }
        if (parameter.range && (!std::isfinite((*parameter.range)[0]) || !std::isfinite((*parameter.range)[1]) ||
            (*parameter.range)[0] > (*parameter.range)[1]))
        { error = "Invalid material parameter range: " + parameter.name; return false; }
        if(!std::holds_alternative<std::monostate>(parameter.defaultValue) && !ValidateMaterialParameterValue(parameter.type,parameter.defaultValue))
        {error="Material parameter default has an incompatible type or nonfinite value: "+parameter.name;return false;}
        if(parameter.isStatic && parameter.type!=UniformType::UNIFORM_BOOL)
        {error="Static switches require a boolean parameter: "+parameter.name;return false;}
    }
    error.clear();
    return true;
}

MaterialParameterId MakeMaterialParameterId(std::string_view owner, std::string_view name)
{
    uint64_t hash = 14695981039346656037ull;
    auto append = [&hash](std::string_view text) { for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ull; } };
    append(owner); append(":" ); append(name);
    return hash ? hash : 1;
}

std::string SerializeMaterialParameterSchema(const MaterialParameterSchema& schema)
{
    using Json = nlohmann::json;
    Json root = { {"version", schema.version}, {"parameters", Json::array()} };
    for (const auto& parameter : schema.parameters)
    {
        Json value;
        std::visit([&value](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (!std::is_same_v<T, std::monostate>) value = item;
        }, parameter.defaultValue);
        Json item = { {"id", parameter.id}, {"name", parameter.name}, {"displayName", parameter.displayName},
            {"group", parameter.group}, {"type", static_cast<int>(parameter.type)}, {"valueType", parameter.defaultValue.index()},
            {"default", value}, {"color", parameter.color}, {"static", parameter.isStatic} };
        if (parameter.range) item["range"] = *parameter.range;
        root["parameters"].push_back(std::move(item));
    }
    return root.dump();
}

std::optional<MaterialParameterSchema> DeserializeMaterialParameterSchema(std::string_view text)
{
    try
    {
        const auto root = nlohmann::json::parse(text);
        MaterialParameterSchema schema;
        schema.version = root.at("version").get<uint32_t>();
        for (const auto& item : root.at("parameters"))
        {
            MaterialParameterDesc parameter;
            parameter.id = item.at("id").get<uint64_t>();
            parameter.name = item.at("name").get<std::string>();
            parameter.displayName = item.value("displayName", parameter.name);
            parameter.group = item.value("group", std::string{});
            const auto type = item.at("type").get<int>();
            if (type < static_cast<int>(UniformType::UNIFORM_BOOL) || type > static_cast<int>(UniformType::UNIFORM_SAMPLER_CUBE))
                return std::nullopt;
            parameter.type = static_cast<UniformType>(type);
            parameter.color = item.value("color", false);
            parameter.isStatic = item.value("static", false);
            if (item.contains("range")) parameter.range = item.at("range").get<std::array<float, 2>>();
            const auto& value = item.at("default");
            switch (item.at("valueType").get<size_t>())
            {
            case 0: break;
            case 1: parameter.defaultValue = value.get<bool>(); break;
            case 2: parameter.defaultValue = value.get<int32_t>(); break;
            case 3: parameter.defaultValue = value.get<float>(); break;
            case 4: parameter.defaultValue = value.get<std::array<float, 4>>(); break;
            case 5: parameter.defaultValue = value.get<std::string>(); break;
            case 6: parameter.defaultValue = value.get<std::array<float,16>>(); break;
            default: return std::nullopt;
            }
            schema.parameters.push_back(std::move(parameter));
        }
        std::string error;
        return schema.Validate(error) ? std::optional{std::move(schema)} : std::nullopt;
    }
    catch (const std::exception&) { return std::nullopt; }
}
}
