#pragma once
#include "PluginsDef.h"
#include "Core/Modules/ModuleSystem.h"
#include <compare>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace NLS::Plugins
{
struct NLS_PLUGINS_API SemVersion
{
    uint32_t major = 0, minor = 0, patch = 0;
    std::string prerelease, build;
    static SemVersion Parse(const std::string& text);
    std::string String() const;
    std::strong_ordering operator<=>(const SemVersion& other) const;
    bool operator==(const SemVersion& other) const { return (*this <=> other) == 0; }
};
NLS_PLUGINS_API bool Satisfies(const SemVersion& version, const std::string& range);

struct PluginDependency
{
    std::string id, version;
    bool optional = false;
    std::vector<std::string> platforms;
};
struct PluginModule
{
    Core::Modules::ModuleDescriptor descriptor;
    std::string name, binary, sourceTarget, scriptBackend, script, entry;
    std::vector<std::string> runtimeDependencies;
    std::string sourceBuildHash;
};
struct ContentMount
{
    std::string name, path;
    bool editorOnly = false;
    std::vector<std::string> runtimeKeep;
};
struct NLS_PLUGINS_API PluginManifest
{
    std::string id, name, author, description, source, raw;
    // Distribution metadata is supplied by a package catalog, never interpreted
    // as a loader entry point or as permission to execute build scripts.
    std::string distributionUrl, distributionHash;
    SemVersion version;
    std::string engineVersion;
    uint32_t pluginApiVersion = 1;
    bool canPackage = true;
    std::vector<std::string> platforms, architectures, warnings;
    std::vector<PluginModule> modules;
    std::vector<PluginDependency> dependencies;
    std::vector<ContentMount> content;
    std::filesystem::path root;
    static PluginManifest Parse(const std::string& text, const std::filesystem::path& root = {});
    static PluginManifest Read(const std::filesystem::path& manifest);
};
NLS_PLUGINS_API bool ValidPluginId(const std::string& id);
NLS_PLUGINS_API std::filesystem::path ResolvePluginPath(const std::filesystem::path& root, const std::string& relative);
NLS_PLUGINS_API std::string HostPlatform();
NLS_PLUGINS_API std::string HostArchitecture();

struct ResolveRequest { std::string id, range, source; };
struct ResolveResult
{
    bool succeeded = false;
    std::vector<PluginManifest> ordered;
    std::string error;
};
// Candidates are metadata only. Resolution never loads code or runs CMake.
NLS_PLUGINS_API ResolveResult ResolvePlugins(const std::vector<ResolveRequest>& requests,
    const std::vector<PluginManifest>& candidates, const std::string& platform,
    const std::map<std::string, std::string>& lockedVersions = {});
}
