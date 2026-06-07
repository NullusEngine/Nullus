#pragma once

#include "CoreDef.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace NLS::Core::ResourceManagement
{
enum class ResourceLifetimeResourceType : uint8_t
{
    Mesh,
    Material,
    Texture
};

enum class ResourceLifetimeOwnerKind : uint8_t
{
    SceneInstance,
    Preview,
    Inspector,
    AsyncPrewarm
};

struct ResourceLifetimeAcquireRequest
{
    std::string ownerToken;
    ResourceLifetimeResourceType type = ResourceLifetimeResourceType::Mesh;
    std::string path;
    size_t estimatedBytes = 0u;
    ResourceLifetimeOwnerKind ownerKind = ResourceLifetimeOwnerKind::SceneInstance;
};

struct ResourceLifetimeTrimOptions
{
    size_t maxCandidates = 0u;
    size_t maxBytes = 0u;
};

struct ResourceLifetimeTrimCandidate
{
    ResourceLifetimeResourceType type = ResourceLifetimeResourceType::Mesh;
    std::string normalizedPath;
    size_t estimatedBytes = 0u;
};

struct ResourceLifetimeDiagnosticSnapshot
{
    size_t resourceCount = 0u;
    size_t ownerCount = 0u;
    size_t activeLeaseCount = 0u;
    size_t zeroOwnerResourceCount = 0u;
    size_t trimCandidateCount = 0u;
    size_t totalEstimatedBytes = 0u;
    size_t activeEstimatedBytes = 0u;
    size_t zeroOwnerEstimatedBytes = 0u;
};

struct ResourceId
{
    ResourceLifetimeResourceType type = ResourceLifetimeResourceType::Mesh;
    std::string normalizedPath;
    uint64_t generation = 0u;
};

struct ResourceOwnerToken
{
    std::string id;
    ResourceLifetimeOwnerKind kind = ResourceLifetimeOwnerKind::SceneInstance;
};

class NLS_CORE_API ResourceLifetimeRegistry
{
public:
    ResourceLifetimeRegistry();
    ~ResourceLifetimeRegistry();

    ResourceId Acquire(const ResourceLifetimeAcquireRequest& request);
    void Release(const ResourceId& resourceId, const std::string& ownerToken);
    void ReleaseOwner(const std::string& ownerToken);
    void ReleaseOwnersByKind(ResourceLifetimeOwnerKind ownerKind);
    void InvalidateResource(ResourceLifetimeResourceType type, const std::string& path);

    bool HasActiveOwners(ResourceLifetimeResourceType type, const std::string& path) const;
    size_t GetActiveOwnerCount(ResourceLifetimeResourceType type, const std::string& path) const;
    size_t GetEstimatedBytes(ResourceLifetimeResourceType type, const std::string& path) const;
    bool IsGenerationCurrent(const ResourceId& resourceId) const;
    bool CanEvict(const ResourceLifetimeTrimCandidate& candidate) const;
    bool TryBeginEviction(const ResourceLifetimeTrimCandidate& candidate);
    void EndEviction(ResourceLifetimeResourceType type, const std::string& path);
    bool CompleteEviction(ResourceLifetimeResourceType type, const std::string& path);
    ResourceLifetimeDiagnosticSnapshot CreateDiagnosticSnapshot() const;
    std::vector<ResourceLifetimeTrimCandidate> CollectTrimCandidates(
        const ResourceLifetimeTrimOptions& options) const;

    static std::string NormalizeResourcePath(const std::string& path);

private:
    struct ResourceKey
    {
        ResourceLifetimeResourceType type = ResourceLifetimeResourceType::Mesh;
        std::string normalizedPath;

        bool operator==(const ResourceKey& other) const
        {
            return type == other.type && normalizedPath == other.normalizedPath;
        }
    };

    struct ResourceKeyHash
    {
        size_t operator()(const ResourceKey& key) const
        {
            const auto typeHash = std::hash<uint8_t>()(static_cast<uint8_t>(key.type));
            const auto pathHash = std::hash<std::string>()(key.normalizedPath);
            return typeHash ^ (pathHash + 0x9e3779b9u + (typeHash << 6u) + (typeHash >> 2u));
        }
    };

    struct ResourceEntry
    {
        ResourceKey key;
        size_t estimatedBytes = 0u;
        uint64_t lastUsed = 0u;
        uint64_t generation = 1u;
        bool evictionPending = false;
        bool evicted = false;
        struct OwnerLease
        {
            std::string ownerToken;
            uint64_t generation = 0u;
            size_t count = 0u;
        };
        std::vector<OwnerLease> ownerLeases;
    };

    struct OwnerEntry
    {
        struct ResourceLease
        {
            ResourceKey key;
            uint64_t generation = 0u;
            size_t count = 0u;
        };
        std::vector<ResourceLease> resources;
        ResourceLifetimeOwnerKind ownerKind = ResourceLifetimeOwnerKind::SceneInstance;
    };

    ResourceEntry* FindEntry(const ResourceKey& key);
    const ResourceEntry* FindEntry(const ResourceKey& key) const;
    void ReleaseOwnerLocked(const std::string& ownerToken);
    static size_t CountActiveLeases(const ResourceEntry& entry);

    mutable std::mutex m_mutex;
    uint64_t m_useCounter = 0u;
    std::unordered_map<ResourceKey, ResourceEntry, ResourceKeyHash> m_resources;
    std::unordered_map<std::string, OwnerEntry> m_owners;
};
}
