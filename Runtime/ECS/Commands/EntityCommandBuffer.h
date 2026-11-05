#pragma once
#include "ECS/Core/World.h"
#include <optional>

namespace NLS::ECS
{
struct WorldDocument;
struct ArchiveLoadOptions;
struct PrefabOverride;
struct EntityReference
{
    Entity entity;
    uint64_t buffer = 0, epoch = 0, writer = 0, sequence = 0;
    uint64_t element = 0; // Zero for CreateEntity; nonzero for prefab entities.
    EntityReference() = default;
    EntityReference(Entity value) : entity(value) {}
    bool IsDeferred() const noexcept { return buffer != 0; }
    bool operator==(const EntityReference&) const = default;
};
struct RecordedPrefab
{
    std::unordered_map<std::string, EntityReference> entities;
    EntityReference Find(std::string_view id) const { return entities.at(std::string(id)); }
};
struct CommandDiagnostic
{
    uint64_t writer = 0, sequence = 0;
    std::string message;
};
struct CommandPlaybackResult
{
    size_t applied = 0;
    double milliseconds = 0;
    std::vector<CommandDiagnostic> errors;
    std::vector<std::pair<EntityReference, Entity>> created;
    bool Succeeded() const { return errors.empty(); }
    std::optional<Entity> Resolve(EntityReference reference) const
    {
        if (!reference.IsDeferred()) return reference.entity;
        for (const auto& [key, entity] : created) if (key == reference) return entity;
        return std::nullopt;
    }
};

// Per-logical-job writer keys define playback order independently of worker
// timing. A writer binds to its first recording thread; use one writer per job.
class NLS_ECS_API EntityCommandBuffer final
{
    enum class Operation { Create, Destroy, Add, Remove, Set, Enable, Instantiate };
    struct PrefabCommand;
    struct Command;
    struct Stream;
    struct State;
public:
    class NLS_ECS_API Writer final
    {
    public:
        // A parent writer with a zero low 32-bit key owns a deterministic key
        // domain. Fork once per logical chunk job, using a unique local key.
        Writer Fork(uint32_t sortKey) const;
        EntityReference CreateEntity(std::span<const ComponentId> signature = {});
        RecordedPrefab InstantiatePrefab(const WorldDocument& prefab, uint64_t sceneOwner = 0);
        RecordedPrefab InstantiatePrefab(const WorldDocument& prefab, const ArchiveLoadOptions& options,
            std::span<const PrefabOverride> overrides = {});
        void DestroyEntity(EntityReference target);
        template<class T> void AddComponent(EntityReference target, T value = {})
        { Value(Operation::Add, target, m_registry->Get<T>(), &value); }
        template<class T> void SetComponent(EntityReference target, T value)
        { Value(Operation::Set, target, m_registry->Get<T>(), &value); }
        template<class T> void RemoveComponent(EntityReference target)
        { Record(Operation::Remove, target, m_registry->Get<T>().id); }
        template<class T> void SetEnabled(EntityReference target, bool enabled)
        { Record(Operation::Enable, target, m_registry->Get<T>().id, {}, enabled); }
        void AddComponent(EntityReference target, ComponentId type, const void* value = nullptr);
        void SetComponent(EntityReference target, ComponentId type, const void* value);
        void RemoveComponent(EntityReference target, ComponentId type) { Record(Operation::Remove, target, type); }
        void SetEnabled(EntityReference target, ComponentId type, bool enabled) { Record(Operation::Enable, target, type, {}, enabled); }
    private:
        friend class EntityCommandBuffer;
        Writer(std::shared_ptr<State> state, std::shared_ptr<Stream> stream,
            std::shared_ptr<ComponentRegistry> registry, uint64_t epoch)
            : m_state(std::move(state)), m_stream(std::move(stream)), m_registry(std::move(registry)), m_epoch(epoch) {}
        void Value(Operation operation, EntityReference target, const ComponentTypeInfo& type, void* value);
        void Record(Operation operation, EntityReference target, ComponentId type = 0,
            std::unique_ptr<ComponentValue> value = {}, bool enabled = true);
        std::shared_ptr<State> m_state;
        std::shared_ptr<Stream> m_stream;
        std::shared_ptr<ComponentRegistry> m_registry;
        uint64_t m_epoch;
    };

    explicit EntityCommandBuffer(World& world);
    ~EntityCommandBuffer();
    EntityCommandBuffer(const EntityCommandBuffer&) = delete;
    EntityCommandBuffer& operator=(const EntityCommandBuffer&) = delete;
    Writer GetWriter(uint64_t sortKey = 0);
    CommandPlaybackResult Playback();
    void Reset();
    size_t GetCommandCount() const;
private:
    std::shared_ptr<State> m_state;
};
}
