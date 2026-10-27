#pragma once
#include <filesystem>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NLS::MaterialCompiler
{
struct MaterialDependencySource
{
    std::string identity;
    std::filesystem::path path;
    std::string content;
    std::string metadataHash;
    uint64_t appliedGeneration = 0;
    std::vector<std::string> dependencies;
    std::map<std::string,std::string> includes;
};

// Retains actual source bytes, not just revision counters. Providers choose
// Working/Saved/Applied content before capture; workers never re-query them.
struct MaterialDependencySnapshot
{
    using Provider = std::function<std::optional<MaterialDependencySource>(const std::string&)>;
    std::string context;
    std::map<std::string,MaterialDependencySource> sources;
    std::string Id() const;
    std::filesystem::path StagePath(const std::filesystem::path& root,const std::filesystem::path& original) const;
    bool Stage(const std::filesystem::path& root,std::string& error) const;
    bool IsCurrent(const Provider& provider) const;
    bool Save(const std::filesystem::path& path,std::string& error) const;
    static std::optional<MaterialDependencySnapshot> Load(const std::filesystem::path& path,std::string& error);
    static std::optional<MaterialDependencySnapshot> Capture(const std::vector<std::string>& roots,
        const Provider& provider, std::string context, std::string& error);
};
}
