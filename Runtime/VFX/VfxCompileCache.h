#pragma once

// Deterministic script hashing and optional persistent derived-data cache.

#include "CompiledSystem.h"
#include "Authoring/VfxLibrary.h"
#include <Rendering/ShaderCompiler/ShaderCompiler.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NLS::VFX
{
inline constexpr uint64_t kVfxCompilerVersion = 5;
inline constexpr uint64_t kVfxCompileCacheSchema = 3;

inline uint64_t HashBytes(std::string_view bytes)
{
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : bytes)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

inline uint64_t HashScriptAsset(const Authoring::ScriptAsset& asset)
{
    return HashBytes(Authoring::SerializeScript(asset,false));
}

inline uint64_t HashSystemAsset(const SystemAsset& asset)
{
    return HashBytes(SerializeSystem(asset,false));
}

inline uint64_t HashDependencies(std::span<const Authoring::SourceDependency> dependencies)
{
    std::vector<const Authoring::SourceDependency*> sorted;
    sorted.reserve(dependencies.size());
    for (const auto& dependency : dependencies) sorted.push_back(&dependency);
    std::sort(sorted.begin(),sorted.end(),[](const auto* a,const auto* b) { return a->asset < b->asset; });
    uint64_t hash = 1469598103934665603ull;
    for (const auto* dependency : sorted)
    {
        for (const unsigned char byte : dependency->asset) { hash ^= byte; hash *= 1099511628211ull; }
        hash ^= 0xffu; hash *= 1099511628211ull;
        for (const unsigned char byte : dependency->semanticSource) { hash ^= byte; hash *= 1099511628211ull; }
        hash ^= 0xfeu; hash *= 1099511628211ull;
    }
    return hash;
}

struct CompileCacheKey
{
    uint64_t sourceHash = 0;
    uint64_t compilerVersion = kVfxCompilerVersion;
    uint64_t shaderLayoutVersion = kVfxShaderLayoutVersion;
    uint64_t cacheSchema = kVfxCompileCacheSchema;
    uint64_t dependencyHash = 0;
    uint32_t backendMask = 0;

    bool operator==(const CompileCacheKey&) const = default;

    [[nodiscard]] std::string ToString() const
    {
        return std::to_string(sourceHash)+":"+std::to_string(compilerVersion)+":"+
            std::to_string(shaderLayoutVersion)+":"+std::to_string(cacheSchema)+":"+
            std::to_string(dependencyHash)+":"+std::to_string(backendMask);
    }
};

inline CompileCacheKey MakeCompileCacheKey(const SystemAsset& source,
    std::span<const uint32_t> backendMask,uint64_t dependencyHash = 0)
{
    CompileCacheKey key;
    key.sourceHash = HashSystemAsset(source);
    key.dependencyHash = dependencyHash;
    for (const auto backend : backendMask) key.backendMask |= backend == 0 ? 1u : backend;
    return key;
}

struct CompileCacheKeyHash
{
    size_t operator()(const CompileCacheKey& key) const noexcept
    {
        uint64_t value = key.sourceHash ^ (key.compilerVersion*0x9e3779b97f4a7c15ull);
        value ^= key.shaderLayoutVersion*0xbf58476d1ce4e5b9ull;
        value ^= key.cacheSchema*0x94d049bb133111ebull;
        value ^= key.dependencyHash*0x9e3779b97f4a7c15ull;
        value ^= key.backendMask;
        return static_cast<size_t>(value ^ (value >> 32));
    }
};

struct CompileCacheStats
{
    uint64_t hits = 0;
    uint64_t misses = 0;
};

class CompileCache
{
public:
    // An empty directory keeps the cache process-local. A non-empty directory
    // enables a small disk DDC using the same deterministic cache key.
    explicit CompileCache(std::filesystem::path directory = {})
        : m_directory(std::move(directory)) {}

    [[nodiscard]] std::shared_ptr<const CompiledSystem> Get(const CompileCacheKey& key)
    {
        std::lock_guard lock(m_mutex);
        if (const auto found = m_entries.find(key); found != m_entries.end())
        {
            ++m_stats.hits;
            return found->second;
        }
        if (m_directory.empty())
        {
            ++m_stats.misses;
            return {};
        }
        const auto path = PathFor(key);
        std::ifstream input(path,std::ios::binary);
        if (!input)
        {
            ++m_stats.misses;
            return {};
        }
        std::error_code sizeError;
        const auto length = std::filesystem::file_size(path,sizeError);
        if (sizeError || length > 128u*1024u*1024u) { ++m_stats.misses; return {}; }
        std::string signature,storedKey,digest;
        if (!std::getline(input, signature) || signature != "NullusVfxDDC3" || !std::getline(input, storedKey) || storedKey != key.ToString() || !std::getline(input, digest))
        { ++m_stats.misses; return {}; }
        std::string text((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        if (text.empty() || digest != std::to_string(HashBytes(text))) { ++m_stats.misses; return {}; }
        auto program = std::make_shared<CompiledSystem>();
        std::string error;
        if (!DeserializeCompiledSystem(text,*program,error)) { ++m_stats.misses; return {}; }
        m_entries.emplace(key,program);
        ++m_stats.hits;
        return program;
    }

    void Put(const CompileCacheKey& key,std::shared_ptr<const CompiledSystem> program)
    {
        if (!program) return;
        std::lock_guard lock(m_mutex);
        m_entries.insert_or_assign(key,program);
        if (m_directory.empty()) return;
        std::error_code error;
        std::filesystem::create_directories(m_directory,error);
        if (error) return;
        const auto payload = SerializeCompiledSystem(*program);
        const auto text = "NullusVfxDDC3\n" + key.ToString() + "\n" + std::to_string(HashBytes(payload)) + "\n" + payload;
        std::string diagnostics;
        (void)Render::ShaderCompiler::WriteShaderArtifactTextAtomic(PathFor(key).string(),text,&diagnostics);
    }

    void Clear()
    {
        std::lock_guard lock(m_mutex);
        m_entries.clear();
    }

    [[nodiscard]] size_t Size() const
    {
        std::lock_guard lock(m_mutex);
        return m_entries.size();
    }

    [[nodiscard]] CompileCacheStats Stats() const
    {
        std::lock_guard lock(m_mutex);
        return m_stats;
    }

    [[nodiscard]] const std::filesystem::path& Directory() const { return m_directory; }

private:
    [[nodiscard]] std::filesystem::path PathFor(const CompileCacheKey& key) const
    {
        return m_directory/(std::to_string(HashBytes(key.ToString()))+".vfxprogram");
    }

    mutable std::mutex m_mutex;
    std::filesystem::path m_directory;
    std::unordered_map<CompileCacheKey,std::shared_ptr<const CompiledSystem>,CompileCacheKeyHash> m_entries;
    CompileCacheStats m_stats;
};
} // namespace NLS::VFX
