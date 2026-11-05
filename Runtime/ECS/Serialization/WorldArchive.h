#pragma once
#include "ComponentCodec.h"
#include <filesystem>

namespace NLS::ECS
{
class World;
enum class DocumentKind : uint8_t { Scene, Prefab };
struct ArchiveLimits
{
    size_t maxBytes = 256 * 1024 * 1024;
    size_t maxEntities = 2'000'000;
    size_t maxComponentsPerEntity = 1024;
    size_t maxDepth = 128;
};
struct ArchivedComponent
{
    ComponentId type = 0;
    std::string name;
    uint32_t version = 1;
    ComponentKind kind = ComponentKind::Data;
    bool enabled = true;
    ComponentJson value = ComponentJson::object();
};
struct ArchivedEntity
{
    std::string id;
    std::string authoringId;
    std::vector<ArchivedComponent> components;
};
struct WorldDocument
{
    DocumentKind kind = DocumentKind::Scene;
    std::vector<ArchivedEntity> entities;
    std::vector<ResourceDependency> resources;
};
struct PersistentEntity
{
    Entity entity;
    std::string id;
    std::string authoringId;
};
struct ArchiveLoadOptions
{
    uint64_t sceneOwner = 0;
    // Validate GUID and optional content version before creating any entities.
    std::function<bool(const ResourceDependency&)> resolveResource;
    ArchiveLimits limits;
};
struct NLS_ECS_API ArchiveInstance
{
    uint64_t world = 0;
    std::vector<Entity> entities;
    std::unordered_map<std::string, Entity> byId;
    std::unordered_map<std::string, Entity> byAuthoringId;
    Entity Find(std::string_view id) const;
    size_t Destroy(World& target);
};
enum class PrefabOverrideKind : uint8_t { SetField, SetComponent, AddComponent, RemoveComponent, SetEnabled };
struct PrefabOverride
{
    std::string entityId;
    ComponentId type = 0;
    PrefabOverrideKind operation = PrefabOverrideKind::SetField;
    std::string field; // RFC 6901 JSON pointer, applied to an existing field.
    ComponentJson value;
    ArchivedComponent component; // Required for AddComponent/SetComponent.
    bool enabled = true;
};

class NLS_ECS_API WorldArchive final
{
public:
    static WorldDocument Capture(World& world, std::span<const PersistentEntity> entities,
        DocumentKind kind = DocumentKind::Scene, ArchiveLimits limits = {});
    static WorldDocument CaptureAll(World& world, DocumentKind kind = DocumentKind::Scene, ArchiveLimits limits = {});
    static ArchiveInstance Instantiate(World& world, const WorldDocument& document, const ArchiveLoadOptions& options = {});
    static WorldDocument ApplyOverrides(const WorldDocument& prefab, std::span<const PrefabOverride> overrides);
    static ArchiveInstance InstantiatePrefab(World& world, const WorldDocument& prefab,
        std::span<const PrefabOverride> overrides = {}, const ArchiveLoadOptions& options = {});
    static std::vector<ArchiveInstance> InstantiatePrefabs(World& world, const WorldDocument& prefab,
        size_t count, const ArchiveLoadOptions& options = {});
    static std::string Encode(const WorldDocument& document, ArchiveLimits limits = {});
    // Pure data operations: Parse/ReadFile may run in BackgroundJobQueue jobs.
    // Instantiate is a main/owning-thread structural operation.
    static WorldDocument Parse(std::string_view text, ArchiveLimits limits = {});
    static WorldDocument ReadFile(const std::filesystem::path& path, ArchiveLimits limits = {});
    static void WriteFile(const std::filesystem::path& path, const WorldDocument& document, ArchiveLimits limits = {});
};
}
