#include "MaterialAsset.h"

#include <fstream>
#include <set>
#include <sstream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#include <Json/json.hpp>
#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Resources/MaterialSerialization.h"
#include "Guid.h"

namespace NLS::Render::Assets
{
namespace
{
using Json = nlohmann::json;
using Value = Resources::MaterialParameterValue;
using Type = Resources::UniformType;

Json Reference(const MaterialAssetReference& ref)
{ return {{"guid", ref.guid}, {"subAsset", ref.subAssetKey}, {"path", ref.path}}; }
MaterialAssetReference ReadReference(const Json& ref)
{ return {ref.value("guid",std::string{}),ref.value("subAsset",std::string{}),ref.value("path",std::string{})}; }
std::string TypeName(Type type)
{
    switch (type)
    {
    case Type::UNIFORM_BOOL: return "Int";
    case Type::UNIFORM_INT: return "Int";
    case Type::UNIFORM_SAMPLER_2D: return "Texture2D";
    case Type::UNIFORM_SAMPLER_CUBE: return "TextureCube";
    case Type::UNIFORM_FLOAT_VEC2:
    case Type::UNIFORM_FLOAT_VEC3:
    case Type::UNIFORM_FLOAT_VEC4: return "Vector";
    case Type::UNIFORM_FLOAT_MAT4: return "Matrix";
    default: return "Float";
    }
}
std::string ValueText(const Value& value)
{
    std::ostringstream stream;
    stream.precision(9);
    std::visit([&stream](const auto& item)
    {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T,std::array<float,4>> || std::is_same_v<T,std::array<float,16>>)
        { for (size_t index=0;index<item.size();++index) {if(index) stream << ' ';stream << item[index];} }
        else if constexpr (std::is_same_v<T,std::string>) stream << Resources::EscapeMaterialField(item);
        else if constexpr (!std::is_same_v<T,std::monostate>) stream << item;
    },value);
    return stream.str();
}
void AppendProjection(std::ostringstream& output, const ResolvedMaterialAsset& asset)
{
    output << "shaderLabMaterialVersion=1\nshader=" << Resources::EscapeMaterialField(asset.shader.path) << '\n';
    for (const auto& [name,value] : asset.renderStateOverrides) output << name << '=' << value << '\n';
    for (const auto& [name,enabled] : asset.staticParameters) output << (enabled?"keyword ":"keywordOff ") << name << '\n';
    for (const auto& [name,parameter] : asset.parameters)
        output << "property " << name << ' ' << TypeName(parameter.type) << ' ' << ValueText(parameter.value) << '\n';
    for (const auto& [name,sampler] : asset.samplerOverrides)
    {
        const auto wrap = [](RHI::TextureWrap wrap) { return wrap == RHI::TextureWrap::ClampToEdge ? "ClampToEdge" :
            wrap == RHI::TextureWrap::MirrorRepeat ? "MirrorRepeat" : "Repeat"; };
        output << "samplerOverride " << name << " wrapS=" << wrap(sampler.wrapU) << " wrapT=" << wrap(sampler.wrapV)
            << " minFilter=" << (sampler.minFilter == RHI::TextureFilter::Nearest ? "Nearest" : "Linear")
            << " magFilter=" << (sampler.magFilter == RHI::TextureFilter::Nearest ? "Nearest" : "Linear")
            << " native=" << Resources::EscapeMaterialField(Resources::SerializeMaterialSampler(sampler)) << '\n';
    }
}
}

bool MaterialAsset::Validate(std::string& error) const
{
    if (version != 2 || shader.Empty() == parent.Empty())
    { error = "Material requires exactly one shader or parent reference"; return false; }
    std::set<std::string> names;
    std::set<uint64_t> ids;
    for (const auto& parameter : parameters)
    {
        if (parameter.name.empty() || !names.insert(parameter.name).second ||
            (parameter.id && !ids.insert(parameter.id).second))
        { error = "Duplicate or empty material override: " + parameter.name; return false; }
        if(!Resources::ValidateMaterialParameterValue(parameter.type,parameter.value))
        {error="Material override has an incompatible type or nonfinite value: "+parameter.name;return false;}
    }
    if (!parent.Empty() && !identity.Empty() && parent.Key() == identity.Key())
    { error = "Material inheritance cycle: " + identity.Key(); return false; }
    for(const auto& [name,sampler]:samplerOverrides)
        if(name.empty() || !Resources::DeserializeMaterialSampler(Resources::SerializeMaterialSampler(sampler)))
        {error="Invalid material sampler: "+name;return false;}
    error.clear(); return true;
}

std::optional<ResolvedMaterialAsset> MaterialAssetResolver::Resolve(const MaterialAsset& asset,
    const Provider& provider, std::string& error, const Resources::MaterialParameterSchema* schema)
{
    ResolvedMaterialAsset result;
    std::vector<MaterialAsset> chain;
    std::set<std::string> visited;
    MaterialAsset current = asset;
    for (;;)
    {
        if (!current.Validate(error)) return std::nullopt;
        const auto key = current.identity.Empty() ? "<working>" : current.identity.Key();
        if (!visited.insert(key).second || chain.size() >= 128)
        {
            error = "Material inheritance cycle or depth limit:";
            for (const auto& entry : chain) error += " " + entry.identity.Key() + " ->";
            error += " " + key;
            return std::nullopt;
        }
        chain.push_back(current);
        if (current.parent.Empty()) break;
        const auto parent = provider ? provider(current.parent) : std::nullopt;
        if (!parent) { error = "Missing parent material: " + current.parent.Key(); return std::nullopt; }
        current = *parent;
        if (current.identity.Empty()) current.identity = chain.back().parent;
    }
    result.shader = chain.back().shader;
    if (schema)
        for (const auto& parameter : schema->parameters)
            if(parameter.isStatic && std::holds_alternative<bool>(parameter.defaultValue))
                result.staticParameters[parameter.name]=std::get<bool>(parameter.defaultValue);
            else if (!parameter.isStatic) result.parameters[parameter.name] =
                {parameter.id,parameter.name,parameter.type,parameter.defaultValue};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        result.ancestry.push_back(it->identity);
        for (const auto& parameter : it->parameters)
        {
            auto resolved = parameter;
            if (schema)
            {
                const auto* definition = parameter.id ? schema->Find(parameter.id) : schema->Find(parameter.name);
                if (!definition || definition->type != parameter.type)
                { result.orphanParameters.push_back(parameter); continue; }
                resolved.name = definition->name; resolved.id = definition->id;
            }
            result.parameters[resolved.name] = std::move(resolved);
        }
        for (const auto& [name,value] : it->staticParameters) result.staticParameters[name] = value;
        for (const auto& [name,value] : it->renderStateOverrides) result.renderStateOverrides[name] = value;
        for (const auto& [name,value] : it->samplerOverrides) result.samplerOverrides[name] = value;
    }
    error.clear(); return result;
}

std::string BuildResolvedMaterialPayload(const ResolvedMaterialAsset& asset)
{
    std::ostringstream output; AppendProjection(output,asset); return output.str();
}

std::string SerializeMaterialAsset(const MaterialAsset& asset)
{
    Json root = {{"version",2},{"identity",Reference(asset.identity)}, {"shader",Reference(asset.shader)},
        {"parent",Reference(asset.parent)}, {"static",asset.staticParameters}, {"states",asset.renderStateOverrides}};
    Resources::MaterialParameterSchema overrides;
    for (const auto& parameter : asset.parameters)
    {
        Resources::MaterialParameterDesc desc;
        desc.id = parameter.id ? parameter.id : Resources::MakeMaterialParameterId("legacy",parameter.name);
        desc.name = parameter.name; desc.type = parameter.type; desc.defaultValue = parameter.value;
        overrides.parameters.push_back(std::move(desc));
    }
    root["parameters"] = Json::parse(Resources::SerializeMaterialParameterSchema(overrides));
    root["legacyParameters"] = Json::array();
    for (const auto& parameter : asset.parameters)
        if (!parameter.id) root["legacyParameters"].push_back(parameter.name);
    root["samplers"] = Json::object();
    for (const auto& [name,sampler] : asset.samplerOverrides)
        root["samplers"][name] = Json::parse(Resources::SerializeMaterialSampler(sampler));
    ResolvedMaterialAsset local;
    local.shader = asset.shader; local.staticParameters = asset.staticParameters; local.renderStateOverrides = asset.renderStateOverrides;
    local.samplerOverrides = asset.samplerOverrides;
    for (const auto& parameter : asset.parameters) local.parameters[parameter.name] = parameter;
    std::ostringstream output; AppendProjection(output,local);
    output << "materialAssetVersion=2\nmaterialAsset=" << root.dump() << '\n';
    return output.str();
}

std::optional<MaterialAsset> DeserializeMaterialAsset(std::string_view payload, std::string& error)
{
    MaterialAsset asset;
    try
    {
        std::istringstream stream{std::string(payload)};
        std::string line;
        while (std::getline(stream,line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("materialAsset="))
            {
                const auto root = Json::parse(line.substr(14));
                asset.version = root.at("version").get<uint32_t>();
                asset.identity = ReadReference(root.at("identity"));
                asset.shader = ReadReference(root.at("shader")); asset.parent = ReadReference(root.at("parent"));
                asset.staticParameters = root.at("static").get<std::map<std::string,bool>>();
                asset.renderStateOverrides = root.at("states").get<std::map<std::string,std::string>>();
                const auto schema = Resources::DeserializeMaterialParameterSchema(root.at("parameters").dump());
                if (!schema) { error = "Invalid material overrides"; return std::nullopt; }
                asset.parameters.clear();
                for (const auto& parameter : schema->parameters)
                    asset.parameters.push_back({parameter.id,parameter.name,parameter.type,parameter.defaultValue});
                for (const auto& name : root.value("legacyParameters",Json::array()))
                    for (auto& parameter : asset.parameters) if (parameter.name == name.get<std::string>()) parameter.id = 0;
                const auto samplers = root.value("samplers",Json::object());
                for (const auto& [name,value] : samplers.items())
                {
                    const auto sampler=Resources::DeserializeMaterialSampler(value.dump());
                    if(!sampler){error="Invalid material sampler: "+name;return std::nullopt;}
                    asset.samplerOverrides[name] = *sampler;
                }
                return asset.Validate(error) ? std::optional{std::move(asset)} : std::nullopt;
            }
            if (line.starts_with("shader=")) asset.shader.path = Resources::UnescapeMaterialField(line.substr(7));
            else if (line.starts_with("keyword ")) asset.staticParameters[line.substr(8)] = true;
            else if (line.starts_with("keywordOff ")) asset.staticParameters[line.substr(11)] = false;
            else if (line.starts_with("property "))
            {
                std::istringstream property(line); std::string marker,type,value;
                MaterialParameterOverride parameter;
                property >> marker >> parameter.name >> type; std::getline(property >> std::ws,value);
                if (type == "Texture2D" || type == "TextureCube")
                { parameter.type = type == "TextureCube" ? Type::UNIFORM_SAMPLER_CUBE : Type::UNIFORM_SAMPLER_2D;
                    parameter.value = Resources::UnescapeMaterialField(value); }
                else if (type == "Vector" || type == "Color")
                {
                    for (auto& c : value) if (c == ',' || c == '(' || c == ')') c = ' ';
                    std::istringstream numbers(value); std::array<float,4> vector{};
                    if (!(numbers >> vector[0] >> vector[1] >> vector[2] >> vector[3]))
                    { error = "Invalid material vector"; return std::nullopt; }
                    parameter.type = Type::UNIFORM_FLOAT_VEC4; parameter.value = vector;
                }
                else if (type == "Matrix")
                {
                    std::array<float,16> matrix{};std::istringstream numbers(value);
                    for (auto& component : matrix) if (!(numbers>>component)) {error="Invalid material matrix";return std::nullopt;}
                    parameter.type=Type::UNIFORM_FLOAT_MAT4;parameter.value=matrix;
                }
                else if (type == "Int") { parameter.type = Type::UNIFORM_INT; parameter.value = static_cast<int32_t>(std::stol(value)); }
                else { parameter.type = Type::UNIFORM_FLOAT; parameter.value = std::stof(value); }
                asset.parameters.push_back(std::move(parameter));
            }
            else
            {
                const auto equals = line.find('=');
                if (equals != std::string::npos)
                {
                    const auto key = line.substr(0,equals);
                    if (key == "surfaceMode" || key == "alphaMode" || key == "doubleSided" || key == "depthWrite")
                        asset.renderStateOverrides[key] = line.substr(equals+1);
                }
            }
        }
        return asset.Validate(error) ? std::optional{std::move(asset)} : std::nullopt;
    }
    catch (const std::exception& exception) { error = exception.what(); return std::nullopt; }
}

bool SaveMaterialAsset(const MaterialAsset& asset, const std::filesystem::path& path, std::string& error)
{
    if (!asset.Validate(error)) return false;
    const auto text = SerializeMaterialAsset(asset);
    Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = Core::Assets::ArtifactType::Material; metadata.schemaName = "material";
    if (const auto id = NLS::Guid::TryParse(asset.identity.guid)) metadata.sourceAssetId = Core::Assets::AssetId(*id);
    const auto bytes = Core::Assets::WriteNativeArtifactContainer(metadata,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),text.size()));
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(),ec);
    if (ec) { error = ec.message(); return false; }
    const auto temporary = std::filesystem::path(path.string()+"."+NLS::Guid::New().ToString()+".material-tmp");
    { std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); file.flush();
        if (!file) { error = "Could not write material source: " + temporary.string(); return false; } }
    // Existing native files are replaced only after the complete write succeeds.
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        ec = std::error_code(static_cast<int>(GetLastError()),std::system_category());
#else
    std::filesystem::rename(temporary,path,ec);
#endif
    if (ec) { error = ec.message(); std::filesystem::remove(temporary,ec); return false; }
    error.clear(); return true;
}
}
