#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"

#include "Assets/ArtifactLoadTelemetry.h"

#include <algorithm>
#include <filesystem>

namespace NLS::Core::ResourceManagement
{
namespace
{
std::string ToGenericPath(const std::string& path)
{
    return std::filesystem::path(path).lexically_normal().generic_string();
}
}

ResourceLifetimeRegistry::ResourceLifetimeRegistry() = default;

ResourceLifetimeRegistry::~ResourceLifetimeRegistry() = default;

std::string ResourceLifetimeRegistry::NormalizeResourcePath(const std::string& path)
{
    auto normalized = ToGenericPath(path);
    const auto libraryMarker = std::string("/Library/");
    if (const auto marker = normalized.find(libraryMarker); marker != std::string::npos)
        normalized = normalized.substr(marker + 1u);

    if (normalized.rfind("Library/", 0) == 0)
        return normalized;

    return normalized;
}

ResourceLifetimeRegistry::ResourceEntry* ResourceLifetimeRegistry::FindEntry(const ResourceKey& key)
{
    const auto found = m_resources.find(key);
    return found != m_resources.end() ? &found->second : nullptr;
}

const ResourceLifetimeRegistry::ResourceEntry* ResourceLifetimeRegistry::FindEntry(const ResourceKey& key) const
{
    const auto found = m_resources.find(key);
    return found != m_resources.end() ? &found->second : nullptr;
}

size_t ResourceLifetimeRegistry::CountActiveLeases(const ResourceEntry& entry)
{
    size_t count = 0u;
    for (const auto& lease : entry.ownerLeases)
        count += lease.count;
    return count;
}

void RecordResourceLifetimeTelemetry(
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
    const std::string& path,
    const size_t byteCount)
{
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        stage,
        {},
        byteCount,
        path });
}

ResourceId ResourceLifetimeRegistry::Acquire(const ResourceLifetimeAcquireRequest& request)
{
    if (request.ownerToken.empty() || request.path.empty())
        return {};

    std::lock_guard lock(m_mutex);
    const ResourceKey key { request.type, NormalizeResourcePath(request.path) };
    ++m_useCounter;

    auto* resource = FindEntry(key);
    if (resource == nullptr)
    {
        const auto [inserted, _] = m_resources.emplace(
            key,
            ResourceEntry { key, request.estimatedBytes, m_useCounter, 1u, false, false, {} });
        resource = &inserted->second;
    }
    if (resource->evictionPending)
        resource->evictionPending = false;

    resource->evicted = false;
    resource->estimatedBytes = std::max(resource->estimatedBytes, request.estimatedBytes);
    resource->lastUsed = m_useCounter;
    auto resourceLease = std::find_if(
        resource->ownerLeases.begin(),
        resource->ownerLeases.end(),
        [&request, resource](const ResourceEntry::OwnerLease& lease)
        {
            return lease.ownerToken == request.ownerToken && lease.generation == resource->generation;
        });
    if (resourceLease == resource->ownerLeases.end())
    {
        resource->ownerLeases.push_back({ request.ownerToken, resource->generation, 1u });
    }
    else
    {
        ++resourceLease->count;
    }

    auto owner = m_owners.find(request.ownerToken);
    if (owner == m_owners.end())
        owner = m_owners.emplace(request.ownerToken, OwnerEntry { {}, request.ownerKind }).first;

    auto ownerLease = std::find_if(
        owner->second.resources.begin(),
        owner->second.resources.end(),
        [&key, resource](const OwnerEntry::ResourceLease& lease)
        {
            return lease.key == key && lease.generation == resource->generation;
        });
    if (ownerLease == owner->second.resources.end())
    {
        owner->second.resources.push_back({ key, resource->generation, 1u });
    }
    else
    {
        ++ownerLease->count;
    }

    RecordResourceLifetimeTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::LifetimeAcquire,
        resource->key.normalizedPath,
        resource->estimatedBytes);

    return { key.type, key.normalizedPath, resource->generation };
}

void ResourceLifetimeRegistry::Release(
    const ResourceId& resourceId,
    const std::string& ownerToken)
{
    if (ownerToken.empty())
        return;

    std::lock_guard lock(m_mutex);
    const ResourceKey key { resourceId.type, resourceId.normalizedPath };
    auto* resource = FindEntry(key);
    if (resource != nullptr)
    {
        auto lease = std::find_if(
            resource->ownerLeases.begin(),
            resource->ownerLeases.end(),
            [&resourceId, &ownerToken](const ResourceEntry::OwnerLease& candidate)
            {
                return candidate.ownerToken == ownerToken && candidate.generation == resourceId.generation;
            });
        if (lease != resource->ownerLeases.end())
        {
            if (lease->count > 1u)
                --lease->count;
            else
                resource->ownerLeases.erase(lease);
        }
        resource->lastUsed = ++m_useCounter;
        RecordResourceLifetimeTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::LifetimeRelease,
            resource->key.normalizedPath,
            resource->estimatedBytes);
    }

    auto owner = m_owners.find(ownerToken);
    if (owner == m_owners.end())
        return;

    auto resourceLease = std::find_if(
        owner->second.resources.begin(),
        owner->second.resources.end(),
        [&key, &resourceId](const OwnerEntry::ResourceLease& candidate)
        {
            return candidate.key == key && candidate.generation == resourceId.generation;
        });
    if (resourceLease != owner->second.resources.end())
    {
        if (resourceLease->count > 1u)
            --resourceLease->count;
        else
            owner->second.resources.erase(resourceLease);
    }
    if (owner->second.resources.empty())
        m_owners.erase(owner);
}

void ResourceLifetimeRegistry::InvalidateResource(
    const ResourceLifetimeResourceType type,
    const std::string& path)
{
    std::lock_guard lock(m_mutex);
    const ResourceKey key { type, NormalizeResourcePath(path) };
    auto* resource = FindEntry(key);
    if (resource == nullptr)
    {
        m_resources.emplace(
            key,
            ResourceEntry { key, 0u, ++m_useCounter, 2u, false, false, {} });
        return;
    }

    ++resource->generation;
    resource->evictionPending = false;
    resource->evicted = false;
    resource->lastUsed = ++m_useCounter;
}

void ResourceLifetimeRegistry::ReleaseOwner(const std::string& ownerToken)
{
    std::lock_guard lock(m_mutex);
    ReleaseOwnerLocked(ownerToken);
}

void ResourceLifetimeRegistry::ReleaseOwnerLocked(const std::string& ownerToken)
{
    const auto owner = m_owners.find(ownerToken);
    if (owner == m_owners.end())
        return;

    ++m_useCounter;
    for (const auto& ownerLease : owner->second.resources)
    {
        auto* resource = FindEntry(ownerLease.key);
        if (resource == nullptr)
            continue;

        resource->ownerLeases.erase(
            std::remove_if(
                resource->ownerLeases.begin(),
                resource->ownerLeases.end(),
                [&ownerToken, &ownerLease](const ResourceEntry::OwnerLease& resourceLease)
                {
                    return resourceLease.ownerToken == ownerToken &&
                        resourceLease.generation == ownerLease.generation;
                }),
            resource->ownerLeases.end());
        resource->lastUsed = m_useCounter;
        RecordResourceLifetimeTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::LifetimeRelease,
            resource->key.normalizedPath,
            resource->estimatedBytes);
    }
    m_owners.erase(owner);
}

void ResourceLifetimeRegistry::ReleaseOwnersByKind(const ResourceLifetimeOwnerKind ownerKind)
{
    std::lock_guard lock(m_mutex);
    std::vector<std::string> ownersToRelease;
    ownersToRelease.reserve(m_owners.size());
    for (const auto& owner : m_owners)
    {
        if (owner.second.ownerKind == ownerKind)
            ownersToRelease.push_back(owner.first);
    }

    for (const auto& ownerToken : ownersToRelease)
        ReleaseOwnerLocked(ownerToken);
}

bool ResourceLifetimeRegistry::HasActiveOwners(
    const ResourceLifetimeResourceType type,
    const std::string& path) const
{
    return GetActiveOwnerCount(type, path) > 0u;
}

size_t ResourceLifetimeRegistry::GetActiveOwnerCount(
    const ResourceLifetimeResourceType type,
    const std::string& path) const
{
    std::lock_guard lock(m_mutex);
    const ResourceKey key { type, NormalizeResourcePath(path) };
    const auto* resource = FindEntry(key);
    return resource != nullptr ? CountActiveLeases(*resource) : 0u;
}

size_t ResourceLifetimeRegistry::GetEstimatedBytes(
    const ResourceLifetimeResourceType type,
    const std::string& path) const
{
    std::lock_guard lock(m_mutex);
    const ResourceKey key { type, NormalizeResourcePath(path) };
    const auto* resource = FindEntry(key);
    return resource != nullptr ? resource->estimatedBytes : 0u;
}

bool ResourceLifetimeRegistry::IsGenerationCurrent(const ResourceId& resourceId) const
{
    std::lock_guard lock(m_mutex);
    const ResourceKey key { resourceId.type, resourceId.normalizedPath };
    const auto* resource = FindEntry(key);
    return resource != nullptr && resource->generation == resourceId.generation;
}

bool ResourceLifetimeRegistry::CanEvict(const ResourceLifetimeTrimCandidate& candidate) const
{
    std::lock_guard lock(m_mutex);
    const ResourceKey key { candidate.type, NormalizeResourcePath(candidate.normalizedPath) };
    const auto* resource = FindEntry(key);
    return resource != nullptr && !resource->evictionPending && CountActiveLeases(*resource) == 0u;
}

bool ResourceLifetimeRegistry::TryBeginEviction(const ResourceLifetimeTrimCandidate& candidate)
{
    std::lock_guard lock(m_mutex);
    const ResourceKey key { candidate.type, NormalizeResourcePath(candidate.normalizedPath) };
    auto* resource = FindEntry(key);
    if (resource == nullptr || resource->evictionPending || CountActiveLeases(*resource) != 0u)
        return false;

    resource->evictionPending = true;
    resource->lastUsed = ++m_useCounter;
    return true;
}

void ResourceLifetimeRegistry::EndEviction(
    const ResourceLifetimeResourceType type,
    const std::string& path)
{
    std::lock_guard lock(m_mutex);
    const ResourceKey key { type, NormalizeResourcePath(path) };
    auto* resource = FindEntry(key);
    if (resource == nullptr)
        return;

    resource->evictionPending = false;
    resource->lastUsed = ++m_useCounter;
}

bool ResourceLifetimeRegistry::CompleteEviction(
    const ResourceLifetimeResourceType type,
    const std::string& path)
{
    std::lock_guard lock(m_mutex);
    const ResourceKey key { type, NormalizeResourcePath(path) };
    auto* resource = FindEntry(key);
    if (resource == nullptr)
    {
        m_resources.emplace(
            key,
            ResourceEntry { key, 0u, ++m_useCounter, 2u, false, true, {} });
        return true;
    }

    if (CountActiveLeases(*resource) != 0u)
    {
        resource->evictionPending = false;
        resource->lastUsed = ++m_useCounter;
        return false;
    }

    ++resource->generation;
    resource->evictionPending = false;
    resource->evicted = true;
    resource->ownerLeases.clear();
    resource->lastUsed = ++m_useCounter;
    return true;
}

ResourceLifetimeDiagnosticSnapshot ResourceLifetimeRegistry::CreateDiagnosticSnapshot() const
{
    std::lock_guard lock(m_mutex);
    ResourceLifetimeDiagnosticSnapshot snapshot;
    snapshot.ownerCount = m_owners.size();

    for (const auto& resource : m_resources)
    {
        const auto& entry = resource.second;
        if (entry.evicted)
            continue;

        ++snapshot.resourceCount;
        const auto activeLeases = CountActiveLeases(entry);
        snapshot.activeLeaseCount += activeLeases;
        snapshot.totalEstimatedBytes += entry.estimatedBytes;

        if (activeLeases == 0u)
        {
            ++snapshot.zeroOwnerResourceCount;
            snapshot.zeroOwnerEstimatedBytes += entry.estimatedBytes;
            if (!entry.evicted && !entry.evictionPending)
                ++snapshot.trimCandidateCount;
            continue;
        }

        snapshot.activeEstimatedBytes += entry.estimatedBytes;
    }

    return snapshot;
}

std::vector<ResourceLifetimeTrimCandidate> ResourceLifetimeRegistry::CollectTrimCandidates(
    const ResourceLifetimeTrimOptions& options) const
{
    std::lock_guard lock(m_mutex);
    std::vector<const ResourceEntry*> zeroOwnerResources;
    for (const auto& resource : m_resources)
    {
        const auto& entry = resource.second;
        if (!entry.evicted && !entry.evictionPending && CountActiveLeases(entry) == 0u)
            zeroOwnerResources.push_back(&entry);
    }

    std::sort(
        zeroOwnerResources.begin(),
        zeroOwnerResources.end(),
        [](const ResourceEntry* lhs, const ResourceEntry* rhs)
        {
            return lhs->lastUsed < rhs->lastUsed;
        });

    std::vector<ResourceLifetimeTrimCandidate> candidates;
    size_t bytes = 0u;
    for (const auto* resource : zeroOwnerResources)
    {
        if (options.maxCandidates != 0u && candidates.size() >= options.maxCandidates)
            break;
        if (options.maxBytes != 0u &&
            !candidates.empty() &&
            bytes + resource->estimatedBytes > options.maxBytes)
            break;

        candidates.push_back({
            resource->key.type,
            resource->key.normalizedPath,
            resource->estimatedBytes });
        bytes += resource->estimatedBytes;
    }

    return candidates;
}
}
