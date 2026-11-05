#include "ContentHash.h"
#include "ArtifactManifest.h"
#include "PluginAssetPath.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <set>

namespace NLS::Core::Assets
{
std::string HashContentFile(const std::filesystem::path& path)
{
    std::ifstream input(path,std::ios::binary); if (!input) throw std::runtime_error("cannot hash content file: " + path.generic_string());
    Sha256Hasher hasher; std::array<uint8_t,65536> buffer{};
    while (input)
    {
        input.read(reinterpret_cast<char*>(buffer.data()),buffer.size());
        if (!hasher.Update(buffer.data(),static_cast<size_t>(input.gcount()))) throw std::runtime_error("SHA-256 failed");
    }
    if (!input.eof()) throw std::runtime_error("cannot read content file");
    return ToHexString(hasher.Finalize());
}
std::string HashContentDirectory(const std::filesystem::path& root, uint32_t maxFiles, uint64_t maxBytes, bool ignoreVcsMetadata)
{
    std::vector<std::pair<std::string,std::filesystem::path>> files; std::set<std::string> aliases;
    uint64_t total = 0; uint32_t count = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(root); it != std::filesystem::recursive_directory_iterator(); ++it)
    {
        if (PluginAssetCollisionKey(it->path().filename().generic_string()) == ".git")
        {
            if (!ignoreVcsMetadata) throw std::runtime_error("package cache contains untracked Git metadata");
            if (it->is_directory()) it.disable_recursion_pending(); continue;
        }
        if (it->is_symlink()) throw std::runtime_error("content tree contains symbolic links");
        const auto utf8 = it->path().lexically_relative(root).generic_u8string();
        const auto name = NormalizePluginAssetPath(std::string(reinterpret_cast<const char*>(utf8.data()),utf8.size()));
        if (!aliases.insert(PluginAssetCollisionKey(name)).second) throw std::runtime_error("content path normalization/case conflict");
        if (++count > maxFiles) throw std::runtime_error("content file count limit exceeded");
        if (it->is_directory()) continue;
        if (!it->is_regular_file()) throw std::runtime_error("content tree contains non-regular files");
        const auto size = it->file_size(); if (size > maxBytes - total) throw std::runtime_error("content size limit exceeded");
        total += size; files.emplace_back(name,it->path());
    }
    std::sort(files.begin(),files.end(),[](auto& a,auto& b) { return a.first < b.first; });
    Sha256Hasher hash;
    for (const auto& [name,path] : files)
    {
        const auto entry = std::to_string(name.size()) + ":" + name + ":" + std::to_string(std::filesystem::file_size(path)) + ":" + HashContentFile(path) + "\n";
        if (!hash.Update(reinterpret_cast<const uint8_t*>(entry.data()),entry.size())) throw std::runtime_error("content tree hash failed");
    }
    return ToHexString(hash.Finalize());
}
}
