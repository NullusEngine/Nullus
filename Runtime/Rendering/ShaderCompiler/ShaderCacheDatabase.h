#pragma once

#include "Rendering/RenderDef.h"
#include "Rendering/ShaderCompiler/ShaderCompilationTypes.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Render::ShaderCompiler
{
struct ShaderCacheRecord
{
    std::string cacheKey;
    std::string assetPath;
    ShaderStage stage = ShaderStage::Vertex;
    ShaderTargetPlatform targetPlatform = ShaderTargetPlatform::Unknown;
    std::string entryPoint;
    std::string targetProfile;
    ShaderCompilationStatus status = ShaderCompilationStatus::NotCompiled;
    std::string artifactPath;
    std::string diagnostics;
    std::string sourceFingerprint;
    std::string toolchainIdentity;
    size_t bytecodeSize = 0u;
    size_t dependencyCount = 0u;
};

struct ShaderCacheDatabaseStats
{
    size_t totalRecords = 0u;
    size_t succeededRecords = 0u;
    size_t failedRecords = 0u;
};

class NLS_RENDER_API ShaderCacheDatabase
{
public:
    ShaderCacheDatabase() = default;
    ~ShaderCacheDatabase() = default;

    void Clear();
    void Upsert(
        const ShaderCompilationInput& input,
        const ShaderCompilationOutput& output,
        std::string sourceFingerprint,
        std::string toolchainIdentity);
    void RemoveByAssetPath(const std::string& assetPath);

    const ShaderCacheRecord* Find(
        const std::string& cacheKey,
        ShaderStage stage,
        ShaderTargetPlatform targetPlatform) const;
    ShaderCacheDatabaseStats GetStats() const;

    bool Save(const std::filesystem::path& path) const;
    bool Load(const std::filesystem::path& path);

private:
    void RebuildIndex();
    static std::string MakeKey(
        const std::string& cacheKey,
        ShaderStage stage,
        ShaderTargetPlatform targetPlatform);

    std::vector<ShaderCacheRecord> m_records;
    std::unordered_map<std::string, size_t> m_indexByKey;
};
}
