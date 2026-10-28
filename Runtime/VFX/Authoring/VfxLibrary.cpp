#include "VfxLibrary.h"
#include <VFX/VfxValueSerialization.h>
#include "VfxAuthoringSerialization.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <Guid.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace NLS::VFX::Authoring
{
namespace
{
using Json = nlohmann::json;
bool ValidId(const std::string& id) { const auto parsed = Guid::TryParse(id); return parsed && parsed->IsValid(); }
bool Finite(const Value& value) { return std::all_of(value.begin(),value.end(),[](float v) { return std::isfinite(v); }); }
const char* KindName(AssetKind kind)
{
    switch (kind) { case AssetKind::Module: return "Module"; case AssetKind::Function: return "Function"; case AssetKind::DynamicInput: return "DynamicInput"; default: return "Unknown"; }
}
std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if (!file) throw std::runtime_error("Cannot read VFX library asset: "+path.string());
    const auto length = file.tellg();
    if (length < 0 || length > 64u*1024u*1024u) throw std::runtime_error("VFX library asset exceeds 64 MiB");
    std::string text(static_cast<size_t>(length),'\0'); file.seekg(0); file.read(text.data(),static_cast<std::streamsize>(text.size()));
    if (!file) throw std::runtime_error("Cannot read complete VFX library asset");
    return text;
}
bool Write(const std::filesystem::path& path,const std::string& text,std::string& error)
{
    std::filesystem::path temporary;
    try
    {
        if (std::filesystem::exists(path))
        {
            const auto old = Json::parse(Read(path)), next = Json::parse(text);
            if ((old.value("version",0u) != kSchemaVersion && old.value("version",0u) != 4) || old.value("kind",std::string{}) != next.at("kind").get<std::string>())
                throw std::runtime_error("Refusing to replace a different VFX asset kind or legacy file");
        }
        temporary = path; temporary += "."+Guid::New().ToString()+".tmp";
        { std::ofstream file(temporary,std::ios::binary|std::ios::trunc); file.write(text.data(),static_cast<std::streamsize>(text.size())); file.close();
          if (!file) throw std::runtime_error("Cannot write VFX library asset"); }
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace VFX library asset");
#else
        std::filesystem::rename(temporary,path);
#endif
        error.clear(); return true;
    }
    catch (const std::exception& exception) { if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary,ignored); } error = exception.what(); return false; }
}
std::string ScopedId(const std::string& scope,const std::string& id)
{
    const auto hash = [](std::string_view text,uint64_t seed) { for (unsigned char c : text) seed = (seed^c)*1099511628211ull; return seed; };
    const auto key = scope+":"+id; std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash(key,1469598103934665603ull) << std::setw(16) << hash(key,7809847782465536322ull);
    auto value = stream.str(); return value.substr(0,8)+"-"+value.substr(8,4)+"-"+value.substr(12,4)+"-"+value.substr(16,4)+"-"+value.substr(20);
}
template<class Asset> auto FindVersion(const Asset& asset,const std::string& id)
{
    const auto key = id.empty() ? asset.exposedVersion : id;
    const auto found = std::find_if(asset.versions.begin(),asset.versions.end(),[&](const auto& version) { return version.id == key; });
    return found == asset.versions.end() ? nullptr : &*found;
}
template<class Asset> void ValidateVersions(const Asset& asset)
{
    if ((asset.version != kSchemaVersion && asset.version != 4) || !ValidId(asset.id) || asset.name.empty() || asset.versions.empty() || asset.versions.size() > 256 || !FindVersion(asset,asset.exposedVersion))
        throw std::runtime_error("Invalid versioned VFX asset header or exposed version");
    std::set<std::string> ids; std::set<std::pair<uint32_t,uint32_t>> numbers;
    for (const auto& version : asset.versions)
        if (!ValidId(version.id) || !version.major || !ids.insert(version.id).second || !numbers.emplace(version.major,version.minor).second)
            throw std::runtime_error("Invalid or duplicate VFX version ID/number");
}
}
ScriptAsset CreateScript(AssetKind kind,std::string name)
{
    ScriptAsset asset; asset.kind = kind; asset.id = Guid::New().ToString(); asset.name = std::move(name);
    ScriptVersion version; version.id = Guid::New().ToString(); version.graph.id = Guid::New().ToString();
    version.inputs.push_back({Guid::New().ToString(),"Value",ValueType::Float,{1,0,0,0}});
    const auto input = AddNode(version.graph,Operation::Parameter); FindNode(version.graph,input)->name = "Module.Value";
    const auto output = AddNode(version.graph,Operation::WriteAttribute); FindNode(version.graph,output)->name = kind == AssetKind::Module ? "Particles.Size" : "Output";
    FindNode(version.graph,output)->inputs = {input}; FindNode(version.graph,output)->position = {300,0};
    asset.exposedVersion = version.id; asset.versions.push_back(std::move(version)); return asset;
}
ScriptAsset CreateEngineModuleAsset(const ModuleDefinition& definition,const Graph& graph)
{
    ScriptAsset asset; asset.kind = AssetKind::Module;
    asset.id = ScopedId("vfx.engine.module",definition.id);
    asset.name = definition.name; asset.category = definition.category; asset.description = definition.description;
    asset.visible = true;
    ScriptVersion version;
    version.id = ScopedId("vfx.engine.module.version",definition.id+"/1.0");
    version.major = 1; version.minor = 0; version.description = definition.description;
    version.capabilities = definition.capabilities;
    version.graph = graph;
    version.attributes.clear();
    for (const auto& attribute : definition.attributes)
        version.attributes.push_back({attribute.name,attribute.type,attribute.defaultValue});
    if (!definition.requiresModule.empty())
    {
        // Dependencies are asset names, not code-defined ids.
        const auto* required = FindModule(definition.requiresModule);
        version.requiresBefore.push_back(required ? required->name : definition.requiresModule);
    }
    if (version.graph.id.empty()) version.graph.id = ScopedId("vfx.engine.module.graph",definition.id);
    version.stages = {definition.stage};
    for (const auto stage : definition.additionalStages)
        if (std::find(version.stages.begin(),version.stages.end(),stage) == version.stages.end()) version.stages.push_back(stage);
    for (const auto& input : definition.inputs)
        version.inputs.push_back({ScopedId("vfx.engine.module.input",definition.id+"/"+input.name),input.name,
            input.type,input.defaultValue,false,input.metadata});
    asset.exposedVersion = version.id; asset.versions.push_back(std::move(version)); return asset;
}
std::string EngineModuleAssetId(std::string_view moduleId)
{ return ScopedId("vfx.engine.module",std::string(moduleId)); }
std::string EngineModuleVersionId(std::string_view moduleId)
{ return ScopedId("vfx.engine.module.version",std::string(moduleId)+"/1.0"); }
ScriptAsset DuplicateScript(const ScriptAsset& source)
{
    ScriptAsset copy = source;
    copy.id = Guid::New().ToString();
    for (auto& version : copy.versions)
    {
        const auto wasExposed = version.id == source.exposedVersion;
        version.id = Guid::New().ToString();
        version.graph.id = Guid::New().ToString();
        for (auto& staged : version.stageGraphs) staged.graph.id = Guid::New().ToString();
        for (auto& input : version.inputs) input.id = Guid::New().ToString();
        if (wasExposed) copy.exposedVersion = version.id;
    }
    return copy;
}
bool IsPathWithin(const std::filesystem::path& path,const std::filesystem::path& root)
{
    const auto normalizedPath = std::filesystem::absolute(path).lexically_normal();
    const auto normalizedRoot = std::filesystem::absolute(root).lexically_normal();
    const auto relative = normalizedPath.lexically_relative(normalizedRoot);
    if (relative.empty() || relative.is_absolute()) return false;
    return *relative.begin() != "..";
}
EmitterSource CreateEmitterSource(std::string name)
{
    EmitterSource asset; asset.id = Guid::New().ToString(); asset.name = std::move(name);
    EmitterVersion version; version.id = Guid::New().ToString(); version.emitter = CreateEmitter(asset.name);
    asset.exposedVersion = version.id; asset.versions.push_back(std::move(version)); return asset;
}
const ScriptVersion* SelectVersion(const ScriptAsset& asset,const std::string& version) { return FindVersion(asset,version); }
const EmitterVersion* SelectVersion(const EmitterSource& asset,const std::string& version) { return FindVersion(asset,version); }
bool ValidateScript(const ScriptAsset& asset,std::string& error)
{
    try
    {
        ValidateVersions(asset);
        if (asset.kind != AssetKind::Module && asset.kind != AssetKind::Function && asset.kind != AssetKind::DynamicInput) throw std::runtime_error("Unsupported VFX script asset kind");
        for (const auto& version : asset.versions)
        {
            if (version.inputs.size() > 256 || version.stages.empty() || version.calls.size() > 1024 || version.switches.size() > 1024) throw std::runtime_error("Script input, stage or call limit exceeded");
            if (version.capabilities & ~static_cast<uint32_t>(ModuleCapability_All)) throw std::runtime_error("Unknown module capability");
            (void)Detail::DecodeGraph(Detail::EncodeGraph(version.graph,true));
            std::set<std::string> names,ids; std::set<uint32_t> stages; std::set<NodeId> special;
            for (const auto stage : version.stages) if (static_cast<uint32_t>(stage) > 7 || !stages.insert(static_cast<uint32_t>(stage)).second) throw std::runtime_error("Invalid script usage stages");
            for (const auto& input : version.inputs)
            {
                const auto& metadata = input.metadata;
                if (!ValidId(input.id) || input.name.empty() || !names.insert(input.name).second || !ids.insert(input.id).second || !ValidValue(input.defaultValue,input.type) || (input.isStatic && input.type != ValueType::Float && input.type != ValueType::Bool))
                    throw std::runtime_error("Invalid or duplicate script input");
                if (metadata.displayName.size() > 256 || metadata.tooltip.size() > 2048 || metadata.unit.size() > 32 ||
                    metadata.visibleIf.size() > 128 || !std::isfinite(metadata.minimum) || !std::isfinite(metadata.maximum) ||
                    !std::isfinite(metadata.step) || metadata.step < 0 || (metadata.hasRange && metadata.minimum > metadata.maximum))
                    throw std::runtime_error("Invalid script input metadata");
                if (!metadata.visibleIf.empty())
                    if (std::none_of(version.inputs.begin(),version.inputs.end(),
                        [&](const auto& other) { return other.name == metadata.visibleIf; }))
                        throw std::runtime_error("Input visibility references an unknown input: "+metadata.visibleIf);
            }
            std::set<std::string> attributeNames;
            for (const auto& attribute : version.attributes)
                if (attribute.name.empty() || !attributeNames.insert(attribute.name).second ||
                    !IsValueType(attribute.type) || !ValidValue(attribute.defaultValue,attribute.type))
                    throw std::runtime_error("Invalid or duplicate script attribute");
            // Metadata-only engine assets may be created before a graph is
            // captured. Runtime compilation still rejects them because they
            // cannot lower; validation only enforces outputs when a graph exists.
            const auto validateGraph = [&](const Graph& graph)
            {
                if (graph.nodes.empty()) return;
                size_t outputs = 0; std::set<std::string> outputNames;
                for (const auto& node : graph.nodes) if (node.operation == Operation::WriteAttribute)
                {
                    ++outputs;
                    if (!outputNames.insert(node.name).second) throw std::runtime_error("Script has multiple writers for one output");
                    if (asset.kind != AssetKind::Module && node.name != "Output") throw std::runtime_error("Function and dynamic input scripts write only Output");
                    const bool validOutput = node.name.starts_with("Particles.") || node.name.starts_with("Emitter.");
                    if (asset.kind == AssetKind::Module && !validOutput) throw std::runtime_error("Module outputs must name particle or emitter attributes");
                }
                if (!outputs || (asset.kind != AssetKind::Module && outputs != 1))
                    throw std::runtime_error("Invalid script output count");
            };
            validateGraph(version.graph);
            std::set<uint32_t> stageGraphs;
            for (const auto& staged : version.stageGraphs)
            {
                if (static_cast<uint32_t>(staged.stage) > 7 || !stages.contains(static_cast<uint32_t>(staged.stage)) ||
                    !stageGraphs.insert(static_cast<uint32_t>(staged.stage)).second)
                    throw std::runtime_error("Invalid stage-specific graph");
                (void)Detail::DecodeGraph(Detail::EncodeGraph(staged.graph,true));
                validateGraph(staged.graph);
            }
            for (const auto& call : version.calls)
            {
                const auto* node = FindNode(version.graph,call.node);
                if (!node || node->operation != Operation::Parameter || !special.insert(call.node).second || call.script.Empty() || call.arguments.size() > 256 || call.output != "Output") throw std::runtime_error("Invalid function call node");
                (void)Detail::DecodeReference(Detail::EncodeReference(call.script));
                for (const auto& [name,id] : call.arguments)
                    if (const auto* argument = FindNode(version.graph,id); name.empty() || (id && (!argument || argument->operation == Operation::WriteAttribute))) throw std::runtime_error("Function argument references a missing or terminal node");
            }
            for (const auto& branch : version.switches)
            {
                const auto* node = FindNode(version.graph,branch.node);
                const auto input = std::find_if(version.inputs.begin(),version.inputs.end(),[&](const auto& value) { return value.name == branch.input && value.isStatic; });
                if (!node || node->operation != Operation::Parameter || !special.insert(branch.node).second || input == version.inputs.end() || (branch.whenTrue && !FindNode(version.graph,branch.whenTrue)) || (branch.whenFalse && !FindNode(version.graph,branch.whenFalse))) throw std::runtime_error("Invalid static switch node");
            }
        }
        error.clear(); return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool ValidateEmitterSource(const EmitterSource& asset,std::string& error)
{
    try
    {
        ValidateVersions(asset);
        for (const auto& version : asset.versions)
        {
            System wrapper; wrapper.id = asset.id; wrapper.name = asset.name; wrapper.emitters = {version.emitter};
            System validated;
            if (!Deserialize(Serialize(wrapper),validated,error)) return false;
        }
        error.clear(); return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
std::string SerializeScript(const ScriptAsset& asset,bool editor)
{
    Json result{{"version",asset.version},{"kind",KindName(asset.kind)},{"id",asset.id},{"name",asset.name},{"category",asset.category},
        {"description",asset.description},{"exposedVersion",asset.exposedVersion},{"visible",asset.visible},{"suggested",asset.suggested},{"deprecated",asset.deprecated},{"versions",Json::array()}};
    for (const auto& version : asset.versions)
    {
        Json row{{"id",version.id},{"major",version.major},{"minor",version.minor},{"description",version.description},{"stages",Json::array()},
            {"capabilities",version.capabilities},{"inputs",Json::array()},{"attributes",Json::array()},{"graph",Detail::EncodeGraph(version.graph,editor)},
            {"calls",Json::array()},{"switches",Json::array()},{"provides",version.provides},{"requiresBefore",version.requiresBefore}};
        if (!version.stageGraphs.empty())
        {
            row["stageGraphs"] = Json::array();
            for (const auto& staged : version.stageGraphs)
                row["stageGraphs"].push_back({{"stage",static_cast<uint32_t>(staged.stage)},
                    {"graph",Detail::EncodeGraph(staged.graph,editor)}});
        }
        for (auto stage : version.stages) row["stages"].push_back(static_cast<uint32_t>(stage));
        for (const auto& input : version.inputs)
        {
            Json inputRow{{"id",input.id},{"name",input.name},{"type",static_cast<uint32_t>(input.type)},
                {"default",NLS::VFX::Detail::EncodeValue(input.defaultValue,input.type)},{"static",input.isStatic}};
            const auto& metadata = input.metadata;
            if (!metadata.displayName.empty() || !metadata.tooltip.empty() || !metadata.unit.empty() ||
                !metadata.visibleIf.empty() || metadata.hasRange || metadata.step > 0)
            {
                Json meta{{"displayName",metadata.displayName},{"tooltip",metadata.tooltip},{"unit",metadata.unit},
                    {"visibleIf",metadata.visibleIf},{"hasRange",metadata.hasRange},{"minimum",metadata.minimum},
                    {"maximum",metadata.maximum},{"step",metadata.step}};
                inputRow["metadata"] = std::move(meta);
            }
            row["inputs"].push_back(std::move(inputRow));
        }
        for (const auto& attribute : version.attributes)
            row["attributes"].push_back({{"name",attribute.name},{"type",static_cast<uint32_t>(attribute.type)},
                {"default",NLS::VFX::Detail::EncodeValue(attribute.defaultValue,attribute.type)}});
        for (const auto& call : version.calls) row["calls"].push_back({{"node",call.node},{"script",Detail::EncodeReference(call.script)},{"output",call.output},{"arguments",call.arguments}});
        for (const auto& branch : version.switches) row["switches"].push_back({{"node",branch.node},{"input",branch.input},{"true",branch.whenTrue},{"false",branch.whenFalse}});
        result["versions"].push_back(std::move(row));
    }
    if (Detail::HasTypedData(result)) result["version"] = 4;
    return result.dump(2);
}
bool DeserializeScript(std::string_view text,ScriptAsset& asset,std::string& error)
{
    try
    {
        if (text.size() > 64u*1024u*1024u) throw std::runtime_error("Script asset exceeds 64 MiB");
        const auto source = Json::parse(text); ScriptAsset candidate;
        if (source.at("version").get<uint32_t>() < 4 && Detail::HasTypedData(source)) throw std::runtime_error("Typed script data requires format v4");
        candidate.version = source.at("version").get<uint32_t>(); const auto kind = source.at("kind").get<std::string>();
        if (kind == "Module") candidate.kind = AssetKind::Module; else if (kind == "Function") candidate.kind = AssetKind::Function; else if (kind == "DynamicInput") candidate.kind = AssetKind::DynamicInput; else throw std::runtime_error("Expected a VFX script asset");
        candidate.id = source.at("id").get<std::string>(); candidate.name = source.at("name").get<std::string>(); candidate.category = source.at("category").get<std::string>();
        candidate.description = source.at("description").get<std::string>(); candidate.exposedVersion = source.at("exposedVersion").get<std::string>();
        candidate.visible = source.value("visible",true); candidate.suggested = source.value("suggested",false); candidate.deprecated = source.value("deprecated",false);
        const auto& versions = source.at("versions");
        if (!versions.is_array() || versions.size() > 256) throw std::runtime_error("Invalid script version list");
        for (const auto& row : versions)
        {
            ScriptVersion version; version.id = row.at("id").get<std::string>(); version.major = row.at("major").get<uint32_t>(); version.minor = row.at("minor").get<uint32_t>(); version.description = row.at("description").get<std::string>();
            version.capabilities = row.value("capabilities",0u);
            if (version.capabilities & ~static_cast<uint32_t>(ModuleCapability_All)) throw std::runtime_error("Unknown module capability");
            version.stages.clear(); if (!row.at("stages").is_array() || row.at("stages").size() > 8) throw std::runtime_error("Invalid script usage list");
            for (const auto& stage : row.at("stages")) { const auto value = stage.get<uint32_t>(); if (value > 7) throw std::runtime_error("Unknown script stage"); version.stages.push_back(static_cast<Stage>(value)); }
            if (!row.at("inputs").is_array() || row.at("inputs").size() > 256) throw std::runtime_error("Invalid script inputs");
            for (const auto& input : row.at("inputs"))
            {
                const auto type = input.at("type").get<uint32_t>(); if (type > static_cast<uint32_t>(ValueType::Bool)) throw std::runtime_error("Unknown script input type");
                ScriptInput parsed{input.at("id").get<std::string>(),input.at("name").get<std::string>(),
                    static_cast<ValueType>(type),NLS::VFX::Detail::DecodeValue(input.at("default"),static_cast<ValueType>(type)),
                    input.value("static",false),{}};
                if (input.contains("metadata"))
                {
                    const auto& metadata = input.at("metadata");
                    if (!metadata.is_object()) throw std::runtime_error("Invalid script input metadata");
                    parsed.metadata.displayName = metadata.value("displayName",std::string{});
                    parsed.metadata.tooltip = metadata.value("tooltip",std::string{});
                    parsed.metadata.unit = metadata.value("unit",std::string{});
                    parsed.metadata.visibleIf = metadata.value("visibleIf",std::string{});
                    parsed.metadata.hasRange = metadata.value("hasRange",false);
                    parsed.metadata.minimum = metadata.value("minimum",0.0f);
                    parsed.metadata.maximum = metadata.value("maximum",0.0f);
                    parsed.metadata.step = metadata.value("step",0.0f);
                    if (parsed.metadata.displayName.size() > 256 || parsed.metadata.tooltip.size() > 2048 ||
                        parsed.metadata.unit.size() > 32 || parsed.metadata.visibleIf.size() > 128 ||
                        !std::isfinite(parsed.metadata.minimum) || !std::isfinite(parsed.metadata.maximum) ||
                        !std::isfinite(parsed.metadata.step) || parsed.metadata.step < 0 ||
                        (parsed.metadata.hasRange && parsed.metadata.minimum > parsed.metadata.maximum))
                        throw std::runtime_error("Invalid script input metadata");
                }
                version.inputs.push_back(std::move(parsed));
            }
            if (row.contains("attributes"))
            {
                const auto& attributes = row.at("attributes");
                if (!attributes.is_array() || attributes.size() > 1024) throw std::runtime_error("Invalid script attribute list");
                std::unordered_set<std::string> names;
                for (const auto& data : attributes)
                {
                    Attribute attribute;
                    attribute.name = data.at("name").get<std::string>();
                    const auto type = data.at("type").get<uint32_t>();
                    if (type > static_cast<uint32_t>(ValueType::Bool) || attribute.name.empty() || !names.insert(attribute.name).second)
                        throw std::runtime_error("Invalid script attribute");
                    attribute.type = static_cast<ValueType>(type);
                    attribute.defaultValue = NLS::VFX::Detail::DecodeValue(data.at("default"),attribute.type);
                    version.attributes.push_back(std::move(attribute));
                }
            }
            version.graph = Detail::DecodeGraph(row.at("graph")); version.provides = row.value("provides",std::vector<std::string>{}); version.requiresBefore = row.value("requiresBefore",std::vector<std::string>{});
            if (row.contains("stageGraphs"))
            {
                const auto& stagedGraphs = row.at("stageGraphs");
                if (!stagedGraphs.is_array() || stagedGraphs.size() > 8) throw std::runtime_error("Invalid stage-specific graph list");
                for (const auto& staged : stagedGraphs)
                {
                    const auto stage = staged.at("stage").get<uint32_t>();
                    if (stage > 7) throw std::runtime_error("Unknown stage-specific graph stage");
                    version.stageGraphs.push_back({static_cast<Stage>(stage),Detail::DecodeGraph(staged.at("graph"))});
                }
            }
            if (!row.at("calls").is_array() || row.at("calls").size() > 1024 || !row.at("switches").is_array() || row.at("switches").size() > 1024) throw std::runtime_error("Invalid script call/switch list");
            for (const auto& call : row.at("calls")) version.calls.push_back({call.at("node").get<NodeId>(),Detail::DecodeReference(call.at("script")),call.at("output").get<std::string>(),call.at("arguments").get<std::map<std::string,NodeId>>()});
            for (const auto& branch : row.at("switches")) version.switches.push_back({branch.at("node").get<NodeId>(),branch.at("true").get<NodeId>(),branch.at("false").get<NodeId>(),branch.at("input").get<std::string>()});
            candidate.versions.push_back(std::move(version));
        }
        if (!ValidateScript(candidate,error)) return false;
        asset = std::move(candidate); error.clear(); return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
std::string SerializeEmitterSource(const EmitterSource& asset,bool editor)
{
    Json result{{"version",asset.version},{"kind","Emitter"},{"id",asset.id},{"name",asset.name},{"exposedVersion",asset.exposedVersion},{"versions",Json::array()}};
    for (const auto& version : asset.versions)
    {
        System wrapper; wrapper.id = asset.id; wrapper.name = asset.name; wrapper.emitters = {version.emitter};
        result["versions"].push_back({{"id",version.id},{"major",version.major},{"minor",version.minor},{"description",version.description},{"body",Json::parse(Serialize(wrapper,editor))}});
    }
    if (Detail::HasTypedData(result)) result["version"] = 4;
    return result.dump(2);
}
bool DeserializeEmitterSource(std::string_view text,EmitterSource& asset,std::string& error)
{
    try
    {
        if (text.size() > 64u*1024u*1024u) throw std::runtime_error("Emitter asset exceeds 64 MiB");
        const auto source = Json::parse(text); if (source.at("kind") != "Emitter") throw std::runtime_error("Expected an emitter asset");
        EmitterSource candidate; candidate.version = source.at("version").get<uint32_t>(); candidate.id = source.at("id").get<std::string>();
        candidate.name = source.at("name").get<std::string>(); candidate.exposedVersion = source.at("exposedVersion").get<std::string>();
        if (!source.at("versions").is_array() || source.at("versions").size() > 256) throw std::runtime_error("Invalid emitter version list");
        for (const auto& row : source.at("versions"))
        {
            System body; if (!Deserialize(row.at("body").dump(),body,error)) return false;
            if (body.id != candidate.id || body.name != candidate.name || body.emitters.size() != 1) throw std::runtime_error("Emitter body identity mismatch");
            candidate.versions.push_back({row.at("id").get<std::string>(),row.at("major").get<uint32_t>(),row.at("minor").get<uint32_t>(),row.at("description").get<std::string>(),std::move(body.emitters.front())});
        }
        if (!ValidateEmitterSource(candidate,error)) return false;
        asset = std::move(candidate); error.clear(); return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool SaveScript(const std::filesystem::path& path,const ScriptAsset& asset,std::string& error)
{ if (!ValidateScript(asset,error)) return false; return Write(path,SerializeScript(asset),error); }
bool SaveEmitterSource(const std::filesystem::path& path,const EmitterSource& asset,std::string& error)
{ if (!ValidateEmitterSource(asset,error)) return false; return Write(path,SerializeEmitterSource(asset),error); }
bool LoadScript(const std::filesystem::path& path,ScriptAsset& asset,std::string& error)
{ try { return DeserializeScript(Read(path),asset,error); } catch (const std::exception& exception) { error = exception.what(); return false; } }
bool LoadEmitterSource(const std::filesystem::path& path,EmitterSource& asset,std::string& error)
{ try { return DeserializeEmitterSource(Read(path),asset,error); } catch (const std::exception& exception) { error = exception.what(); return false; } }
Module InstantiateModule(const ScriptAsset& asset,const std::string& version)
{
    Module module; const auto* selected = SelectVersion(asset,version);
    if (!selected || asset.kind != AssetKind::Module) return module;
    module.id = Guid::New().ToString();
    // The asset name is the stable display/diagnostic identity; execution is
    // resolved through scriptAsset and the asset graph.
    module.script = asset.name;
    module.scriptAsset = {asset.id,selected->id};
    for (const auto& input : selected->inputs) { module.inputs[input.name].value = input.defaultValue; module.inputs[input.name].type = input.type; }
    return module;
}
Emitter InstantiateEmitter(const EmitterSource& asset,const std::string& version)
{
    auto emitter = CreateEmitter(asset.name); emitter.stacks.clear();
    if (const auto* selected = SelectVersion(asset,version)) emitter.parent = {asset.id,selected->id};
    return emitter;
}
bool SourceLibrary::Add(ScriptAsset asset,std::filesystem::path path,std::string& error)
{
    if (!ValidateScript(asset,error)) return false;
    if (m_paths.contains(asset.id)) { error = "Duplicate VFX source identity: "+asset.id; return false; }
    m_paths[asset.id] = std::move(path); m_scripts.emplace(asset.id,std::move(asset)); return true;
}
bool SourceLibrary::Add(EmitterSource asset,std::filesystem::path path,std::string& error)
{
    if (!ValidateEmitterSource(asset,error)) return false;
    if (m_paths.contains(asset.id)) { error = "Duplicate VFX source identity: "+asset.id; return false; }
    m_paths[asset.id] = std::move(path); m_emitters.emplace(asset.id,std::move(asset)); return true;
}
const ScriptAsset* SourceLibrary::Script(std::string_view id) const { const auto found = m_scripts.find(std::string(id)); return found == m_scripts.end() ? nullptr : &found->second; }
const EmitterSource* SourceLibrary::EmitterAsset(std::string_view id) const { const auto found = m_emitters.find(std::string(id)); return found == m_emitters.end() ? nullptr : &found->second; }
std::optional<SourceDependency> SourceLibrary::Dependency(std::string_view id) const
{
    const auto path = m_paths.find(std::string(id)); if (path == m_paths.end()) return {};
    if (const auto* asset = Script(id)) return SourceDependency{std::string(id),SerializeScript(*asset,false),path->second};
    if (const auto* asset = EmitterAsset(id)) return SourceDependency{std::string(id),SerializeEmitterSource(*asset,false),path->second};
    return {};
}
std::string SourceLibrary::SemanticSource() const
{
    std::string result; for (const auto& [id,path] : m_paths) { result += id; result += Dependency(id)->semanticSource; } return result;
}
bool SourceLibrary::Refresh(std::span<const std::filesystem::path> roots,std::string& error)
{
    SourceLibrary candidate; std::set<std::filesystem::path> visited;
    try
    {
        for (const auto& root : roots)
        {
            if (root.empty() || !std::filesystem::exists(root)) continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root,std::filesystem::directory_options::skip_permission_denied))
            {
                if (!entry.is_regular_file()) continue;
                // Keep the source root spelling for project-relative dependency tracking.
                // Canonical paths are only used to deduplicate overlapping roots and aliases.
                const auto path = std::filesystem::absolute(entry.path()).lexically_normal();
                if (!visited.insert(std::filesystem::weakly_canonical(path)).second) continue;
                auto extension = path.extension().string(); std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (extension == ".vfxscript") { ScriptAsset asset; if (!LoadScript(path,asset,error) || !candidate.Add(std::move(asset),path,error)) throw std::runtime_error(path.string()+": "+error); }
                if (extension == ".vfxemitter") { EmitterSource asset; if (!LoadEmitterSource(path,asset,error) || !candidate.Add(std::move(asset),path,error)) throw std::runtime_error(path.string()+": "+error); }
            }
        }
        *this = std::move(candidate); error.clear(); return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool SourceLibrary::Resolve(const System& source,System& result,std::vector<SourceDependency>& dependencies,std::vector<Issue>& issues) const
{
    issues.clear(); std::map<std::string,SourceDependency> captured; std::set<std::string> visiting;
    std::function<bool(const Emitter&,Emitter&,uint32_t)> resolve = [&](const Emitter& input,Emitter& output,uint32_t depth)
    {
        if (depth > 32) { issues.push_back({"Emitter inheritance depth exceeds 32",input.settings.id}); return false; }
        if (input.parent.Empty()) { output = input; return true; }
        const auto* asset = EmitterAsset(input.parent.asset);
        const auto* version = asset ? SelectVersion(*asset,input.parent.version) : nullptr;
        const auto key = input.parent.asset+":"+(version ? version->id : input.parent.version);
        if (!version || !visiting.insert(key).second) { issues.push_back({version ? "Emitter inheritance cycle" : "Emitter asset or pinned version is missing",input.settings.id}); return false; }
        captured[asset->id] = *Dependency(asset->id); Emitter parent;
        if (!resolve(version->emitter,parent,depth+1)) return false;
        visiting.erase(key); output = parent;
        System p; p.id = source.id; p.name = source.name; p.emitters = {parent}; System c = p; c.emitters = {input};
        auto parentJson = Json::parse(Serialize(p))["runtime"]["emitters"][0]; const auto childJson = Json::parse(Serialize(c))["runtime"]["emitters"][0];
        for (const auto& name : input.settingsOverrides) parentJson[name] = childJson.at(name);
        parentJson["id"] = input.settings.id; parentJson["name"] = input.settings.name;
        auto runtime = Json::parse(SerializeSystem(SystemAsset{1,source.id,source.name,{parent.settings}})); runtime["emitters"][0] = std::move(parentJson);
        SystemAsset settings; std::string error;
        if (!DeserializeSystem(runtime.dump(),settings,error)) { issues.push_back({error,input.settings.id}); return false; }
        output.settings = std::move(settings.emitters.front()); output.enabled = input.enabled; output.position = input.position;
        for (const auto& stack : input.stacks)
        {
            Stack* target = nullptr;
            if (stack.stage == Stage::Simulation || stack.stage == Stage::Event)
            { for (auto& candidate : output.stacks) if (candidate.id == stack.id) target = &candidate; }
            else target = FindStack(output,stack.stage);
            if (target) target->modules.insert(target->modules.end(),stack.modules.begin(),stack.modules.end()); else output.stacks.push_back(stack);
        }
        for (const auto& item : input.overrides)
        {
            Module* module = nullptr; for (auto& stack : output.stacks) for (auto& candidate : stack.modules) if (candidate.id == item.module) module = &candidate;
            if (!module) { issues.push_back({"Inherited override addresses a removed module",input.settings.id,{},item.module}); return false; }
            if (item.enabled) module->enabled = *item.enabled;
            for (const auto& [name,value] : item.inputs)
            {
                if (!module->inputs.contains(name)) { issues.push_back({"Inherited override addresses a removed input",input.settings.id,{},item.module,name}); return false; }
                module->inputs[name] = value;
            }
        }
        for (const auto& item : input.stackOverrides)
        {
            const auto stack = std::find_if(output.stacks.begin(),output.stacks.end(),[&](const auto& value) { return value.id == item.stack && value.stage == Stage::Simulation; });
            if (stack == output.stacks.end()) { issues.push_back({"Inherited override addresses a removed Simulation Stage",input.settings.id,item.stack}); return false; }
            if (item.enabled) stack->enabled = *item.enabled;
            if (item.iterations) stack->iterations = *item.iterations;
            if (item.name) stack->name = *item.name;
            if (item.bindings) stack->bindings = *item.bindings;
        }
        output.parent = {}; output.overrides.clear(); output.settingsOverrides.clear(); output.stackOverrides.clear(); return true;
    };
    System candidate = source;
    for (size_t i = 0; i < source.emitters.size(); ++i)
    {
        if (!resolve(source.emitters[i],candidate.emitters[i],0)) return false;
        if (!source.emitters[i].parent.Empty())
        {
            auto& emitter = candidate.emitters[i]; const auto scope = source.emitters[i].settings.id;
            emitter.settings.spawn.id = ScopedId(scope,emitter.settings.spawn.id); emitter.settings.update.id = ScopedId(scope,emitter.settings.update.id);
            for (auto& stack : emitter.stacks)
            {
                stack.inheritedId = stack.id;
                stack.id = ScopedId(scope,stack.id);
                for (auto& module : stack.modules)
                {
                    module.inheritedId = module.id;
                    module.id = ScopedId(scope,module.id);
                    for (auto& [name,input] : module.inputs) if (input.mode == InputMode::DynamicInput) input.expression.id = ScopedId(scope,input.expression.id);
                }
            }
        }
    }
    if (!Validate(candidate,issues)) return false;
    result = std::move(candidate); dependencies.clear(); for (auto& [id,value] : captured) dependencies.push_back(std::move(value)); return true;
}
}
