#include "MaterialGraph.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <set>
#include <Json/json.hpp>
#include "Guid.h"
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

namespace NLS::MaterialCompiler
{
namespace
{
using Json=nlohmann::json;
Json Link(const MaterialGraphLink& link){return {link.node,link.output};}
MaterialGraphLink ReadLink(const Json& value){return {value.at(0).get<uint64_t>(),value.at(1).get<uint32_t>()};}
Json Port(const MaterialGraphPort& port){return {{"id",port.id},{"name",port.name},{"type",static_cast<int>(port.type)}};}
MaterialGraphPort ReadPort(const Json& value){return {value.at("id").get<uint64_t>(),value.at("name").get<std::string>(),
    static_cast<MaterialGraphType>(value.at("type").get<int>())};}
}
std::string MaterialGraphBindingName(uint64_t parameter,bool isStatic)
{return std::string(isStatic?"_K_":"_P_")+std::to_string(parameter);}
std::string MaterialGraphTypeName(MaterialGraphType type)
{
    switch(type)
    {
    case MaterialGraphType::Float2:return "float2";case MaterialGraphType::Float3:return "float3";
    case MaterialGraphType::Float4:return "float4";case MaterialGraphType::Int:return "int";
    case MaterialGraphType::Bool:return "bool";case MaterialGraphType::Texture2D:return "Texture2D";
    case MaterialGraphType::TextureCube:return "TextureCube";case MaterialGraphType::Sampler:return "SamplerState";
    default:return "float";
    }
}
MaterialGraphNode* FindMaterialGraphNode(MaterialGraphAsset& asset,uint64_t id)
{const auto it=std::find_if(asset.nodes.begin(),asset.nodes.end(),[id](const auto& node){return node.id==id;});return it==asset.nodes.end()?nullptr:&*it;}
const MaterialGraphNode* FindMaterialGraphNode(const MaterialGraphAsset& asset,uint64_t id)
{const auto it=std::find_if(asset.nodes.begin(),asset.nodes.end(),[id](const auto& node){return node.id==id;});return it==asset.nodes.end()?nullptr:&*it;}
uint64_t AddMaterialGraphNode(MaterialGraphAsset& asset,std::string operation,MaterialGraphType type)
{
    uint64_t id=1;for(const auto& node:asset.nodes)id=std::max(id,node.id+1);
    MaterialGraphNode node;node.id=id;node.operation=std::move(operation);node.type=type;
    asset.nodes.push_back(std::move(node));return id;
}
MaterialGraphAsset CreateDefaultMaterialGraph(std::string name)
{
    MaterialGraphAsset graph;graph.id=NLS::Guid::New().ToString();graph.name=std::move(name);
    MaterialGraphParameter color;
    color.id=Render::Resources::MakeMaterialParameterId(graph.id,"Base Color");color.name="Base Color";
    color.type=MaterialGraphType::Float4;color.color=true;color.value=std::array<float,4>{0.8f,0.8f,0.8f,1};
    graph.parameters.push_back(color);
    const auto node=AddMaterialGraphNode(graph,"Parameter",MaterialGraphType::Float4);
    graph.nodes.back().parameter=color.id;graph.outputs["BaseColor"]={node,0};
    return graph;
}

bool ValidateMaterialGraph(const MaterialGraphAsset& graph,std::string& error)
{
    if(graph.version!=1 || graph.id.empty()){error="Unsupported or unidentified material graph";return false;}
    if(graph.nodes.size()>16384 || graph.parameters.size()>1024){error="Material graph exceeds its size limit";return false;}
    if(!std::isfinite(graph.maximumWorldDisplacement) || graph.maximumWorldDisplacement<0)
    {error="Maximum world displacement must be finite and nonnegative";return false;}
    if(static_cast<int>(graph.domain)>1 || static_cast<int>(graph.shading)>1 || static_cast<int>(graph.blend)>4)
    {error="Invalid material graph domain or settings";return false;}
    if(graph.domain==MaterialGraphDomain::ParticleSprite && graph.shading!=MaterialGraphShading::Unlit)
    {error="ParticleSprite currently uses the engine Unlit material ABI";return false;}
    std::set<uint64_t> ids;
    for(const auto& node:graph.nodes)
    {
        if(!node.id || !ids.insert(node.id).second || static_cast<int>(node.type)>8)
        {error="Duplicate node identity or invalid value type";return false;}
        for(float value:node.value)if(!std::isfinite(value)){error="Node constants must be finite";return false;}
    }
    const auto validatePorts=[&](const auto& ports)
    {std::set<uint64_t> portIds;std::set<std::string> portNames;
        for(const auto& port:ports)
            if(!port.id || port.name.empty() || !portIds.insert(port.id).second || !portNames.insert(port.name).second ||
                (!std::isalpha(static_cast<unsigned char>(port.name.front())) && port.name.front()!='_') ||
                port.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos ||
                static_cast<int>(port.type)>8)return false;
        return true;};
    if(!validatePorts(graph.functionInputs) || !validatePorts(graph.functionOutputs))
    {error="Function ports need unique identities and valid HLSL names";return false;}
    for(const auto& node:graph.nodes)
        if(!validatePorts(node.customInputs) || !validatePorts(node.customOutputs))
        {error="Custom node ports need unique identities and valid HLSL names";return false;}
    ids.clear();std::set<std::string> names;
    for(const auto& parameter:graph.parameters)
    {
        if(!parameter.id || !ids.insert(parameter.id).second || parameter.name.empty() || !names.insert(parameter.name).second)
        {error="Duplicate or empty graph parameter identity";return false;}
        if(static_cast<int>(parameter.type)>8 || parameter.type==MaterialGraphType::Sampler)
        {error="Invalid exposed graph parameter type";return false;}
        if(parameter.isStatic && (parameter.type!=MaterialGraphType::Bool || !std::holds_alternative<bool>(parameter.value)))
        {error="Static graph switches need a boolean type and default";return false;}
        if(parameter.type==MaterialGraphType::Bool && !parameter.isStatic)
        {error="Boolean graph parameters must be declared static switches";return false;}
    }
    for(const auto& node:graph.nodes)for(const auto& [pin,link]:node.inputs)
        if(link.node && !FindMaterialGraphNode(graph,link.node)){error="Missing linked node on pin "+pin;return false;}
    for(const auto& [pin,link]:graph.outputs)
        if(link.node && !FindMaterialGraphNode(graph,link.node)){error="Missing material output node: "+pin;return false;}
    error.clear();return true;
}

std::string SerializeMaterialGraph(const MaterialGraphAsset& graph)
{
    Json root={{"version",graph.version},{"id",graph.id},{"name",graph.name},{"function",graph.function},
        {"domain",static_cast<int>(graph.domain)},{"shading",static_cast<int>(graph.shading)},{"blend",static_cast<int>(graph.blend)},
        {"doubleSided",graph.doubleSided},{"maximumWorldDisplacement",graph.maximumWorldDisplacement},
        {"nodes",Json::array()},{"parameters",Json::array()},{"outputs",Json::object()},
        {"functionInputs",Json::array()},{"functionOutputs",Json::array()},{"comments",Json::object()}};
    for(const auto& parameter:graph.parameters)
    {
        Render::Resources::MaterialParameterDesc value;value.id=parameter.id;value.name=parameter.name;value.defaultValue=parameter.value;
        using U=Render::Resources::UniformType;
        value.type=parameter.type==MaterialGraphType::Float?U::UNIFORM_FLOAT:parameter.type==MaterialGraphType::Float2?U::UNIFORM_FLOAT_VEC2:
            parameter.type==MaterialGraphType::Float3?U::UNIFORM_FLOAT_VEC3:parameter.type==MaterialGraphType::Float4?U::UNIFORM_FLOAT_VEC4:
            parameter.type==MaterialGraphType::Int?U::UNIFORM_INT:parameter.type==MaterialGraphType::Bool?U::UNIFORM_BOOL:
            parameter.type==MaterialGraphType::Texture2D?U::UNIFORM_SAMPLER_2D:U::UNIFORM_SAMPLER_CUBE;
        const auto encoded=Json::parse(Render::Resources::SerializeMaterialParameterSchema({1,{value}}));
        root["parameters"].push_back({{"value",encoded},{"group",parameter.group},{"type",static_cast<int>(parameter.type)},
            {"color",parameter.color},{"static",parameter.isStatic},{"range",parameter.range?Json(*parameter.range):Json{}}});
    }
    for(const auto& node:graph.nodes)
    {
        Json value={{"id",node.id},{"op",node.operation},{"type",static_cast<int>(node.type)},{"name",node.name},
            {"value",node.value},{"parameter",node.parameter},{"position",node.position},{"inputs",Json::object()},
            {"customInputs",Json::array()},{"customOutputs",Json::array()},{"code",node.code},{"includes",node.includes},
            {"function",{{"guid",node.function.guid},{"subAsset",node.function.subAssetKey},{"path",node.function.path}}}};
        for(const auto& [pin,link]:node.inputs)value["inputs"][pin]=Link(link);
        for(const auto& port:node.customInputs)value["customInputs"].push_back(Port(port));
        for(const auto& port:node.customOutputs)value["customOutputs"].push_back(Port(port));
        root["nodes"].push_back(std::move(value));
    }
    for(const auto& port:graph.functionInputs)root["functionInputs"].push_back(Port(port));
    for(const auto& port:graph.functionOutputs)root["functionOutputs"].push_back(Port(port));
    for(const auto& [pin,link]:graph.outputs)root["outputs"][pin]=Link(link);
    for(const auto& [id,comment]:graph.comments)root["comments"][std::to_string(id)]=comment;
    return root.dump(2);
}

bool DeserializeMaterialGraph(std::string_view source,MaterialGraphAsset& asset,std::string& error)
{
    try
    {
        const auto root=Json::parse(source);MaterialGraphAsset graph;
        graph.version=root.at("version").get<uint32_t>();graph.id=root.at("id").get<std::string>();graph.name=root.at("name").get<std::string>();
        graph.function=root.value("function",false);graph.domain=static_cast<MaterialGraphDomain>(root.value("domain",0));
        graph.shading=static_cast<MaterialGraphShading>(root.value("shading",0));graph.blend=static_cast<MaterialGraphBlend>(root.value("blend",0));
        graph.doubleSided=root.value("doubleSided",false);graph.maximumWorldDisplacement=root.value("maximumWorldDisplacement",0.f);
        for(const auto& value:root.at("parameters"))
        {
            const auto schema=Render::Resources::DeserializeMaterialParameterSchema(value.at("value").dump());
            if(!schema || schema->parameters.size()!=1){error="Invalid graph parameter";return false;}
            const auto& definition=schema->parameters[0];MaterialGraphParameter parameter;
            parameter.id=definition.id;parameter.name=definition.name;parameter.value=definition.defaultValue;
            parameter.group=value.value("group",std::string{});parameter.type=static_cast<MaterialGraphType>(value.at("type").get<int>());
            parameter.color=value.value("color",false);parameter.isStatic=value.value("static",false);
            if(value.contains("range") && !value["range"].is_null())parameter.range=value["range"].get<std::array<float,2>>();
            graph.parameters.push_back(std::move(parameter));
        }
        for(const auto& value:root.at("nodes"))
        {
            MaterialGraphNode node;node.id=value.at("id").get<uint64_t>();node.operation=value.at("op").get<std::string>();
            node.type=static_cast<MaterialGraphType>(value.at("type").get<int>());node.name=value.value("name",std::string{});
            node.value=value.at("value").get<std::array<float,4>>();node.parameter=value.value("parameter",uint64_t{});
            node.position=value.value("position",std::array<float,2>{});node.code=value.value("code",std::string{});
            node.includes=value.value("includes",std::vector<std::string>{});
            const auto& function=value.at("function");node.function={function.value("guid",std::string{}),
                function.value("subAsset",std::string{}),function.value("path",std::string{})};
            for(const auto& [pin,link]:value.at("inputs").items())node.inputs[pin]=ReadLink(link);
            for(const auto& port:value.at("customInputs"))node.customInputs.push_back(ReadPort(port));
            for(const auto& port:value.at("customOutputs"))node.customOutputs.push_back(ReadPort(port));
            graph.nodes.push_back(std::move(node));
        }
        for(const auto& port:root.at("functionInputs"))graph.functionInputs.push_back(ReadPort(port));
        for(const auto& port:root.at("functionOutputs"))graph.functionOutputs.push_back(ReadPort(port));
        for(const auto& [pin,link]:root.at("outputs").items())graph.outputs[pin]=ReadLink(link);
        if(root.contains("comments"))for(const auto& [id,text]:root["comments"].items())graph.comments[std::stoull(id)]=text.get<std::string>();
        if(!ValidateMaterialGraph(graph,error))return false;
        asset=std::move(graph);error.clear();return true;
    }
    catch(const std::exception& exception){error=exception.what();return false;}
}

bool SaveMaterialGraph(const MaterialGraphAsset& asset,const std::filesystem::path& path,std::string& error)
{
    if(!ValidateMaterialGraph(asset,error))return false;
    return Render::ShaderCompiler::WriteShaderArtifactTextAtomic(path.generic_string(),SerializeMaterialGraph(asset),&error);
}
}
