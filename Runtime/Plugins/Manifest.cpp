#include "Manifest.h"
#include "SDK/NullusPlugin.h"
#include "Core/Assets/PluginAssetPath.h"
#include <Json/json.hpp>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace NLS::Plugins
{
using Json = nlohmann::json;
namespace
{
uint32_t Number(const std::string& s)
{
    uint32_t value = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || (s.size() > 1 && s.front() == '0') || result.ec != std::errc{} || result.ptr != s.data() + s.size())
        throw std::invalid_argument("invalid SemVer numeric identifier: " + s);
    return value;
}
std::vector<std::string> Split(const std::string& s, char separator)
{
    std::vector<std::string> result; std::istringstream input(s); std::string item;
    while (std::getline(input, item, separator)) result.push_back(item); return result;
}
bool Includes(const std::vector<std::string>& values, const std::string& value)
{ return values.empty() || std::find(values.begin(), values.end(), value) != values.end(); }
std::string Required(const Json& value, const char* key)
{
    if (!value.contains(key) || !value.at(key).is_string() || value.at(key).get<std::string>().empty())
        throw std::invalid_argument(std::string("missing/non-string manifest field: ") + key);
    return value.at(key).get<std::string>();
}
bool Compare(const SemVersion& version, const std::string& term)
{
    if (term == "*" || term.empty()) return true;
    size_t n = 0; while (n < term.size() && (term[n] == '<' || term[n] == '>' || term[n] == '=' || term[n] == '^' || term[n] == '~')) ++n;
    const auto op = term.substr(0, n); const auto base = SemVersion::Parse(term.substr(n));
    if (op == ">=") return version >= base;
    if (op == ">") return version > base;
    if (op == "<=") return version <= base;
    if (op == "<") return version < base;
    if (op == "=" || op.empty()) return version == base;
    auto upper = base; upper.prerelease.clear(); upper.build.clear();
    if (op == "~") { ++upper.minor; upper.patch = 0; }
    else if (op == "^")
    {
        if (upper.major) { ++upper.major; upper.minor = upper.patch = 0; }
        else if (upper.minor) { ++upper.minor; upper.patch = 0; }
        else ++upper.patch;
    }
    else throw std::invalid_argument("unsupported version comparator: " + op);
    return version >= base && version < upper;
}
}
SemVersion SemVersion::Parse(const std::string& text)
{
    static const std::regex pattern(R"(^([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)");
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) throw std::invalid_argument("invalid SemVer: " + text);
    SemVersion result{Number(match[1]), Number(match[2]), Number(match[3]), match[4], match[5]};
    for (const auto& part : Split(result.prerelease, '.'))
        if (part.size()>1 && part.front()=='0' && std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw std::invalid_argument("numeric prerelease identifier has a leading zero");
    return result;
}
std::string SemVersion::String() const
{
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) +
        (prerelease.empty() ? "" : "-" + prerelease) + (build.empty() ? "" : "+" + build);
}
std::strong_ordering SemVersion::operator<=>(const SemVersion& other) const
{
    if (auto c = major <=> other.major; c != 0) return c;
    if (auto c = minor <=> other.minor; c != 0) return c;
    if (auto c = patch <=> other.patch; c != 0) return c;
    if (prerelease.empty() != other.prerelease.empty()) return prerelease.empty() ? std::strong_ordering::greater : std::strong_ordering::less;
    auto a = Split(prerelease, '.'), b = Split(other.prerelease, '.');
    for (size_t i = 0; i < (std::min)(a.size(), b.size()); ++i)
    {
        const auto numeric = [](const std::string& s) { return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); };
        bool an = numeric(a[i]), bn = numeric(b[i]);
        if (an && bn) { if (a[i].size() != b[i].size()) return a[i].size() <=> b[i].size(); }
        else if (an != bn) return an ? std::strong_ordering::less : std::strong_ordering::greater;
        if (auto c = a[i] <=> b[i]; c != 0) return c;
    }
    return a.size() <=> b.size();
}
bool Satisfies(const SemVersion& version, const std::string& range)
{
    if (range.empty()) throw std::invalid_argument("empty SemVer range");
    size_t begin = 0; bool matched=false;
    for (;;)
    {
        auto end = range.find("||", begin);
        auto branch = range.substr(begin, end == std::string::npos ? end : end - begin);
        std::replace(branch.begin(), branch.end(), ',', ' ');
        std::istringstream input(branch); std::string term; bool accepted = true, explicitPrerelease = false, any = false;
        while (input >> term)
        {
            if (term==">=" || term=="<=" || term==">" || term=="<" || term=="=" || term=="^" || term=="~")
            { std::string operand; if (!(input >> operand)) throw std::invalid_argument("SemVer comparator has no operand"); term += operand; }
            any = true; accepted = Compare(version, term) && accepted;
            if (term.find('-') != std::string::npos)
            {
                const auto first = term.find_first_of("0123456789");
                auto base = SemVersion::Parse(term.substr(first));
                explicitPrerelease |= base.major == version.major && base.minor == version.minor && base.patch == version.patch;
            }
        }
        if (!any) throw std::invalid_argument("empty SemVer range branch");
        if (accepted && (version.prerelease.empty() || explicitPrerelease)) matched=true;
        if (end == std::string::npos) return matched;
        begin = end + 2;
    }
}
bool ValidPluginId(const std::string& id)
{
    static const std::regex pattern("^[a-z0-9]+([.-][a-z0-9]+)*$");
    return id.size() <= 160 && std::regex_match(id, pattern);
}
std::filesystem::path ResolvePluginPath(const std::filesystem::path& root, const std::string& relative)
{
    if (relative.empty() || relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos || relative.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid plugin relative path: " + relative);
    const auto path = std::filesystem::u8path(relative);
    if (path.is_absolute() || path.has_root_path()) throw std::invalid_argument("absolute plugin path: " + relative);
    for (const auto& part : path) if (part == "..") throw std::invalid_argument("plugin path escapes root: " + relative);
    const auto base = std::filesystem::weakly_canonical(std::filesystem::absolute(root.empty() ? "." : root));
    const auto resolved = std::filesystem::weakly_canonical(base / path);
    const auto local = resolved.lexically_relative(base);
    if (local.empty() || *local.begin() == "..") throw std::invalid_argument("plugin path escapes root through link: " + relative);
    return resolved;
}
PluginManifest PluginManifest::Parse(const std::string& text, const std::filesystem::path& rootPath)
{
    if (text.size()>8*1024*1024) throw std::invalid_argument("plugin manifest size limit exceeded");
    const auto j = Json::parse(text);
    if (!j.is_object() || j.at("schemaVersion").get<uint32_t>() != 1) throw std::invalid_argument("unsupported plugin manifest schema");
    PluginManifest p; p.raw = text; p.root = std::filesystem::absolute(rootPath.empty() ? "." : rootPath);
    p.id = Required(j, "id"); p.name = Required(j, "name"); p.version = SemVersion::Parse(Required(j, "version"));
    if (!ValidPluginId(p.id)) throw std::invalid_argument("invalid plugin id: " + p.id);
    p.author = j.value("author", ""); p.description = j.value("description", "");
    p.engineVersion = Required(j, "engineVersion"); Satisfies(SemVersion{1, 0, 0}, p.engineVersion);
    p.pluginApiVersion = j.at("pluginApiVersion").get<uint32_t>();
    if (p.pluginApiVersion != NLS_PLUGIN_ABI_MAJOR) throw std::invalid_argument("unsupported plugin API: " + p.id);
    p.canPackage = j.value("canPackage", true);
    p.platforms = j.value("platforms", std::vector<std::string>{}); p.architectures = j.value("architectures", std::vector<std::string>{});
    const auto warnUnknown=[&](const Json& object,const std::set<std::string>& allowed,const std::string& prefix) {
        for (auto it=object.begin();it!=object.end();++it)
            if (!allowed.count(it.key())) p.warnings.push_back("unknown manifest field retained: " + prefix + it.key());
    };
    const std::set<std::string> known{"schemaVersion","id","name","version","author","description","engineVersion","pluginApiVersion","canPackage","platforms","architectures","modules","dependencies","content","canContainContent","cmake"};
    warnUnknown(j,known,{});
    if (!j.contains("modules") || !j["modules"].is_array()) throw std::invalid_argument("modules must be an array");
    std::set<std::string> names;
    for (const auto& m : j["modules"])
    {
        PluginModule module; module.name = Required(m, "name");
        warnUnknown(m,{"name","type","loadingPhase","hosts","safeUnload","dependencies","binary","sourceTarget",
            "scriptBackend","script","entry","runtimeDependencies","sourceBuildHash"},"modules."+module.name+".");
        if (!std::regex_match(module.name, std::regex("^[A-Za-z_][A-Za-z0-9_]*$")) || !names.insert(module.name).second) throw std::invalid_argument("invalid/duplicate module name");
        module.descriptor.id = p.id + "." + module.name;
        const auto type = Required(m, "type"), phase = Required(m, "loadingPhase");
        using namespace Core::Modules;
        if (type == "Runtime") module.descriptor.type = ModuleType::Runtime;
        else if (type == "Editor") module.descriptor.type = ModuleType::Editor;
        else if (type == "Developer") module.descriptor.type = ModuleType::Developer;
        else if (type == "ThirdParty") module.descriptor.type = ModuleType::ThirdParty;
        else throw std::invalid_argument("unknown module type: " + type);
        if (phase == "PreEngine") module.descriptor.phase = LoadingPhase::PreEngine;
        else if (phase == "PostEngine") module.descriptor.phase = LoadingPhase::PostEngine;
        else if (phase == "PostEditor") module.descriptor.phase = LoadingPhase::PostEditor;
        else if (phase == "PrePlay") module.descriptor.phase = LoadingPhase::PrePlay;
        else throw std::invalid_argument("unknown loading phase: " + phase);
        module.descriptor.hosts = module.descriptor.type == ModuleType::Editor ? 1u : 3u;
        if (m.contains("hosts"))
        {
            module.descriptor.hosts = 0;
            for (const auto& host : m["hosts"].get<std::vector<std::string>>())
                if (host == "Editor") module.descriptor.hosts |= 1; else if (host == "Game") module.descriptor.hosts |= 2; else if (host == "NullusBuild") module.descriptor.hosts |= 4; else throw std::invalid_argument("unknown host: " + host);
        }
        module.descriptor.supportsUnload = m.value("safeUnload", false);
        for (const auto& dep : m.value("dependencies", std::vector<std::string>{})) module.descriptor.dependencies.push_back(dep.find('.') == std::string::npos ? p.id + "." + dep : dep);
        module.binary = m.value("binary", ""); module.sourceTarget = m.value("sourceTarget", "");
        module.scriptBackend = m.value("scriptBackend", ""); module.script = m.value("script", ""); module.entry = m.value("entry", "");
        if (static_cast<int>(!module.binary.empty()) + !module.sourceTarget.empty() + !module.scriptBackend.empty() != 1)
            throw std::invalid_argument("module must declare exactly one binary, sourceTarget or scriptBackend");
        if (!module.binary.empty()) ResolvePluginPath(p.root, module.binary);
        module.runtimeDependencies = m.value("runtimeDependencies",std::vector<std::string>{});
        module.sourceBuildHash = m.value("sourceBuildHash",std::string{});
        for (const auto& file : module.runtimeDependencies) ResolvePluginPath(p.root,file);
        if (!module.scriptBackend.empty())
        {
            if (module.scriptBackend != "CSharp" && module.scriptBackend != "Lua") throw std::invalid_argument("unknown script plugin backend");
            if (module.script.empty() || (module.scriptBackend == "CSharp" && module.entry.empty())) throw std::invalid_argument("script plugin entry is missing");
            ResolvePluginPath(p.root,module.script);
            if (module.descriptor.phase == LoadingPhase::PreEngine) throw std::invalid_argument("script plugins require initialized PostEngine services");
        }
        p.modules.push_back(std::move(module));
    }
    std::set<std::string> dependencies;
    for (const auto& d : j.value("dependencies", Json::array()))
    {
        PluginDependency dep; dep.id = Required(d, "id"); dep.version = Required(d, "version"); dep.optional = d.value("optional", false);
        warnUnknown(d,{"id","version","optional","platforms"},"dependencies."+dep.id+".");
        if (!ValidPluginId(dep.id) || !dependencies.insert(dep.id).second) throw std::invalid_argument("invalid/duplicate dependency id");
        Satisfies(SemVersion{1, 0, 0}, dep.version); dep.platforms = d.value("platforms", std::vector<std::string>{}); p.dependencies.push_back(std::move(dep));
    }
    std::set<std::string> mounts;
    for (const auto& c : j.value("content", Json::array()))
    {
        ContentMount mount; mount.name = Required(c, "name"); mount.path = Required(c, "path");
        warnUnknown(c,{"name","path","editorOnly","runtimeKeep"},"content."+mount.name+".");
        if (!ValidPluginId(mount.name) || !mounts.insert(mount.name).second) throw std::invalid_argument("invalid/duplicate mount");
        ResolvePluginPath(p.root, mount.path); mount.editorOnly = c.value("editorOnly", false);
        mount.runtimeKeep = c.value("runtimeKeep", std::vector<std::string>{});
        for (auto& keep : mount.runtimeKeep) { keep = Core::Assets::NormalizePluginAssetPath(keep); ResolvePluginPath(p.root / mount.path, keep); }
        p.content.push_back(std::move(mount));
    }
    return p;
}
PluginManifest PluginManifest::Read(const std::filesystem::path& path)
{
    if (std::filesystem::file_size(path)>8*1024*1024) throw std::invalid_argument("plugin manifest size limit exceeded");
    std::ifstream stream(path, std::ios::binary); if (!stream) throw std::runtime_error("cannot open plugin manifest: " + path.generic_string());
    std::string text((std::istreambuf_iterator<char>(stream)), {});
    if (text.size() > 8 * 1024 * 1024) throw std::runtime_error("plugin manifest exceeds size limit");
    auto manifest = Parse(text, path.parent_path()); const auto source=std::filesystem::weakly_canonical(path.parent_path()).generic_u8string();
    manifest.source.assign(reinterpret_cast<const char*>(source.data()),source.size()); return manifest;
}
std::string HostPlatform()
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}
std::string HostArchitecture() { return NLS_PLUGIN_ARCH == 2 ? "arm64" : "x64"; }
ResolveResult ResolvePlugins(const std::vector<ResolveRequest>& requests, const std::vector<PluginManifest>& candidates,
    const std::string& platform, const std::map<std::string, std::string>& locked)
{
    ResolveResult result;
    try
    {
        std::map<std::string, std::vector<const PluginManifest*>> available;
        for (const auto& c : candidates) if (Includes(c.platforms, platform)) available[c.id].push_back(&c);
        for (auto& [id, list] : available)
            std::sort(list.begin(), list.end(), [&](auto* a, auto* b) {
                auto it = locked.find(id);
                bool al = it != locked.end() && a->version.String() == it->second, bl = it != locked.end() && b->version.String() == it->second;
                if (al != bl) return al;
                if (a->version != b->version) return a->version > b->version;
                return a->source < b->source;
            });
        std::map<std::string, const PluginManifest*> selected;
        std::function<bool(std::vector<ResolveRequest>, std::map<std::string, const PluginManifest*>&)> solve;
        solve = [&](std::vector<ResolveRequest> pending, auto& chosen) {
            if (pending.empty()) return true;
            auto request = pending.front(); pending.erase(pending.begin());
            if (auto it = chosen.find(request.id); it != chosen.end())
                return Satisfies(it->second->version, request.range) && (request.source.empty() || request.source == it->second->source) && solve(pending, chosen);
            for (auto* candidate : available[request.id])
            {
                if (!Satisfies(candidate->version, request.range) || (!request.source.empty() && request.source != candidate->source)) continue;
                if (request.source.empty())
                    for (auto* other : available[request.id])
                        if (candidate != other && candidate->version == other->version && candidate->source != other->source)
                            throw std::runtime_error("ambiguous plugin sources: " + request.id + "; select a source explicitly");
                auto next = pending;
                for (const auto& dep : candidate->dependencies)
                    if (Includes(dep.platforms, platform) && (!dep.optional || available.count(dep.id))) next.push_back({dep.id, dep.version, {}});
                auto attempt = chosen; attempt.emplace(request.id, candidate);
                if (solve(std::move(next), attempt)) { chosen = std::move(attempt); return true; }
            }
            return false;
        };
        if (!solve(requests, selected)) throw std::runtime_error("plugin dependency/version/source conflict or missing dependency");
        std::set<std::string> done; std::vector<std::string> stack;
        std::function<void(const std::string&)> visit = [&](const std::string& id) {
            if (std::find(stack.begin(), stack.end(), id) != stack.end())
            { std::string path; for (auto& part : stack) path += part + " -> "; throw std::runtime_error("plugin cycle: " + path + id); }
            if (done.count(id)) return;
            stack.push_back(id); const auto* p = selected.at(id);
            auto deps = p->dependencies; std::sort(deps.begin(), deps.end(), [](auto& a, auto& b) { return a.id < b.id; });
            for (const auto& dep : deps) if (Includes(dep.platforms, platform) && selected.count(dep.id)) visit(dep.id);
            stack.pop_back(); done.insert(id); result.ordered.push_back(*p);
        };
        for (const auto& [id, p] : selected) visit(id);
        result.succeeded = true;
    }
    catch (const std::exception& e) { result.error = e.what(); result.ordered.clear(); }
    return result;
}
}
