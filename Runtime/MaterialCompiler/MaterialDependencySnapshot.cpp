#include "MaterialDependencySnapshot.h"
#include <Json/json.hpp>
#include "Assets/NativeArtifactContainer.h"
#include <fstream>
#include <regex>
#include "Rendering/ShaderCompiler/ShaderCompiler.h"

namespace NLS::MaterialCompiler
{
namespace
{
std::string Hash(std::string_view text)
{
    return Core::Assets::ComputeNativeArtifactPayloadHash(reinterpret_cast<const uint8_t*>(text.data()),text.size());
}
void Append(std::string& output,std::string_view text)
{ output+=std::to_string(text.size())+":"; output.append(text); }
std::string SourceId(const MaterialDependencySource& source)
{
    std::string text;
    Append(text,source.identity); Append(text,source.path.generic_string());
    Append(text,Hash(source.content)); Append(text,source.metadataHash);
    Append(text,std::to_string(source.appliedGeneration));
    for (const auto& dependency : source.dependencies) Append(text,dependency);
    for (const auto& [name,path] : source.includes) {Append(text,name);Append(text,path);}
    return Hash(text);
}
}

std::string MaterialDependencySnapshot::Id() const
{
    std::string text;
    Append(text,context);
    for (const auto& [key,source] : sources) { Append(text,key); Append(text,SourceId(source)); }
    return Hash(text);
}

bool MaterialDependencySnapshot::Save(const std::filesystem::path& path,std::string& error) const
{
    using Json=nlohmann::json;
    Json root={{"version",1},{"id",Id()},{"context",std::vector<uint8_t>(context.begin(),context.end())},{"sources",Json::object()}};
    for(const auto& [key,source]:sources)
        root["sources"][key]={{"identity",source.identity},{"path",source.path.generic_string()},
            {"content",std::vector<uint8_t>(source.content.begin(),source.content.end())},{"metadataHash",source.metadataHash},{"appliedGeneration",source.appliedGeneration},
            {"dependencies",source.dependencies},{"includes",source.includes}};
    try
    {
        std::filesystem::create_directories(path.parent_path());
        return Render::ShaderCompiler::WriteShaderArtifactTextAtomic(path.generic_string(),root.dump(),&error);
    }
    catch(const std::exception& ex){error=ex.what();return false;}
}

std::optional<MaterialDependencySnapshot> MaterialDependencySnapshot::Load(const std::filesystem::path& path,std::string& error)
{
    try
    {
        std::ifstream file(path,std::ios::binary);if(!file){error="Missing material dependency archive";return std::nullopt;}
        const auto root=nlohmann::json::parse(file);
        if(root.at("version")!=1){error="Unsupported material dependency archive";return std::nullopt;}
        const auto context=root.at("context").get<std::vector<uint8_t>>();
        MaterialDependencySnapshot snapshot;snapshot.context.assign(context.begin(),context.end());
        for(const auto& [key,value]:root.at("sources").items())
        {
            MaterialDependencySource source;source.identity=value.at("identity").get<std::string>();
            source.path=value.at("path").get<std::string>();
            const auto content=value.at("content").get<std::vector<uint8_t>>();source.content.assign(content.begin(),content.end());
            source.metadataHash=value.at("metadataHash").get<std::string>();source.appliedGeneration=value.at("appliedGeneration").get<uint64_t>();
            source.dependencies=value.at("dependencies").get<std::vector<std::string>>();
            source.includes=value.at("includes").get<std::map<std::string,std::string>>();snapshot.sources[key]=std::move(source);
        }
        if(snapshot.Id()!=root.at("id").get<std::string>()){error="Material dependency archive hash mismatch";return std::nullopt;}
        error.clear();return snapshot;
    }
    catch(const std::exception& ex){error=ex.what();return std::nullopt;}
}

bool MaterialDependencySnapshot::IsCurrent(const Provider& provider) const
{
    if (!provider) return false;
    for (const auto& [key,source] : sources)
    {
        const auto current=provider(key);
        if (!current) return false;
        auto normalized=*current;
        if (normalized.identity.empty()) normalized.identity=key;
        if (SourceId(normalized)!=SourceId(source)) return false;
    }
    return true;
}

std::optional<MaterialDependencySnapshot> MaterialDependencySnapshot::Capture(
    const std::vector<std::string>& roots,const Provider& provider,std::string context,std::string& error)
{
    if (!provider || roots.empty()) { error="A material snapshot needs roots and a source provider"; return std::nullopt; }
    MaterialDependencySnapshot snapshot;
    snapshot.context=std::move(context);
    std::vector<std::string> pending=roots;
    while (!pending.empty())
    {
        auto key=std::move(pending.back()); pending.pop_back();
        if (snapshot.sources.contains(key)) continue;
        auto source=provider(key);
        if (!source) { error="Missing material dependency: "+key; return std::nullopt; }
        if (source->identity.empty()) source->identity=key;
        pending.insert(pending.end(),source->dependencies.begin(),source->dependencies.end());
        snapshot.sources.emplace(std::move(key),std::move(*source));
        if (snapshot.sources.size()>16384) { error="Material dependency snapshot exceeds its resource limit"; return std::nullopt; }
    }
    if (!snapshot.IsCurrent(provider)) { error="Material dependencies changed while the snapshot was captured"; return std::nullopt; }
    error.clear(); return snapshot;
}

std::filesystem::path MaterialDependencySnapshot::StagePath(const std::filesystem::path& root,
    const std::filesystem::path& original) const
{
    const auto portable=std::filesystem::absolute(original).lexically_normal().generic_string();
    auto snapshot=Id();auto file=Hash(portable);
    snapshot=snapshot.substr(snapshot.find(':')+1);file=file.substr(file.find(':')+1);
    return root/snapshot/(file+original.extension().string());
}

bool MaterialDependencySnapshot::Stage(const std::filesystem::path& root,std::string& error) const
{
    try
    {
        const std::regex includes(R"nls((#\s*include\s*["<])([^">]+)([">]))nls");
        for (const auto& [key,source] : sources)
        {
            if (source.path.empty()) continue;
            auto content=source.content;
            std::string rewritten;
            std::smatch match;
            while (std::regex_search(content,match,includes))
            {
                rewritten+=match.prefix().str()+match[1].str();
                const std::filesystem::path include=match[2].str();
                const auto resolved=source.includes.find(include.generic_string());
                rewritten+=resolved!=source.includes.end()?StagePath(root,resolved->second).generic_string():
                    include.is_absolute()?StagePath(root,include).generic_string():include.generic_string();
                rewritten+=match[3].str();
                content=match.suffix().str();
            }
            rewritten+=content;
            const auto destination=StagePath(root,source.path);
            std::filesystem::create_directories(destination.parent_path());
            std::ifstream existing(destination,std::ios::binary);
            if (existing)
            {
                const std::string bytes{std::istreambuf_iterator<char>(existing),std::istreambuf_iterator<char>()};
                if (bytes!=rewritten) { error="Frozen material source cache content mismatch: "+destination.string(); return false; }
                continue;
            }
            if (!Render::ShaderCompiler::WriteShaderArtifactTextAtomic(destination.generic_string(),rewritten,&error)) return false;
        }
        error.clear();return true;
    }
    catch (const std::exception& exception) {error=exception.what();return false;}
}
}
