#pragma once
#include "Storage.h"
#include "ECS/Query/QueryTypes.h"
#include <atomic>
#include <map>
#include <thread>


namespace NLS::ECS
{
class QueryPlan;
class ChunkView;
class SystemScheduler;
class EntityCommandBuffer;
class WorldArchive;
template<class... Terms> class EntityQuery;

struct WorldConfig
{
    std::string name = "ECS World";
    size_t chunkBytes = 16 * 1024;
    size_t retainedEmptyChunksPerArchetype = 1;
    bool singleThreaded = false;
    bool enableStatistics = true;
    size_t minimumParallelEntities = 2048;
    size_t chunksPerJob = 8;
};

struct WorldStatistics
{
    size_t entities = 0, archetypes = 0, chunks = 0, emptyChunks = 0, capacity = 0;
    size_t componentInstances = 0, chunkBytes = 0, metadataBytes = 0, sharedBytes = 0;
    uint64_t chunkAllocations = 0, structuralChanges = 0, queryExecutions = 0, jobs = 0;
    double queryMilliseconds = 0, jobWaitMilliseconds = 0;
    double Occupancy() const { return capacity ? double(entities) / double(capacity) : 0; }
};

struct ArchetypeStatistics
{
    size_t id = 0, entities = 0, chunks = 0, capacity = 0, bytes = 0;
    std::vector<ComponentId> components;
};

class NLS_ECS_API World final
{
public:
    explicit World(WorldConfig config = {}, std::shared_ptr<ComponentRegistry> registry = {});
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    uint64_t GetId() const noexcept { return m_id; }
    const WorldConfig& GetConfig() const noexcept { return m_config; }
    ComponentRegistry& GetRegistry() noexcept { return *m_registry; }
    const ComponentRegistry& GetRegistry() const noexcept { return *m_registry; }
    std::shared_ptr<ComponentRegistry> GetRegistryLifetime() const noexcept { return m_registry; }
    std::weak_ptr<const void> GetLifetime() const noexcept { return m_lifetime; }
    bool IsClosed() const noexcept { return m_closed; }

    template<class T> const ComponentTypeInfo& RegisterComponent(std::string name,
        ComponentKind kind = ComponentKind::Data, uint32_t version = 1)
    { CheckStructural(); return m_registry->Register<T>(std::move(name), kind, version); }
    template<class T> const ComponentTypeInfo& GetComponentType() const { return m_registry->Get<T>(); }

    Entity CreateEntity(std::span<const ComponentId> components = {});
    std::vector<Entity> CreateEntities(size_t count, std::span<const ComponentId> components = {});
    bool DestroyEntity(Entity entity);
    size_t DestroyEntities(std::span<const Entity> entities);
    bool IsAlive(Entity entity) const;
    size_t GetEntityCount() const { CheckReadThread(); return m_entityCount; }
    std::vector<Entity> GetEntities() const;
    std::vector<ComponentId> GetEntityComponents(Entity entity) const;
    size_t GetEntityArchetype(Entity entity) const;
    void SetScene(Entity entity, uint64_t scene);
    uint64_t GetScene(Entity entity) const;
    size_t UnloadScene(uint64_t scene);

    bool HasComponent(Entity entity, ComponentId type) const;
    template<class T> bool HasComponent(Entity entity) const { return HasComponent(entity, m_registry->Get<T>().id); }
    template<class T> void AddComponent(Entity entity, T value = {})
    { AddMoved(entity, m_registry->Get<T>(), &value); }
    void AddComponent(Entity entity, ComponentId type, const void* value = nullptr);
    bool RemoveComponent(Entity entity, ComponentId type);
    template<class T> bool RemoveComponent(Entity entity) { return RemoveComponent(entity, m_registry->Get<T>().id); }
    template<class T> const T* TryGetComponent(Entity entity) const
    { return static_cast<const T*>(ReadRaw(entity, m_registry->Get<T>().id, false)); }
    template<class T> const T& ReadComponent(Entity entity) const
    { return *static_cast<const T*>(ReadRaw(entity, m_registry->Get<T>().id, true)); }
    template<class T> T& WriteComponent(Entity entity)
    { return *static_cast<T*>(WriteRaw(entity, m_registry->Get<T>().id)); }
    template<class T> const T& GetComponent(Entity entity) const { return ReadComponent<T>(entity); }
    template<class T> void SetComponent(Entity entity, T value)
    { SetMoved(entity, m_registry->Get<T>(), &value); }
    void SetComponent(Entity entity, ComponentId type, const void* value);
    const void* ReadComponent(Entity entity, ComponentId type) const { return ReadRaw(entity, type, true); }
    void SetEnabled(Entity entity, ComponentId type, bool enabled);
    bool IsEnabled(Entity entity, ComponentId type) const;
    template<class T> void SetEnabled(Entity entity, bool enabled) { SetEnabled(entity, m_registry->Get<T>().id, enabled); }
    template<class T> bool IsEnabled(Entity entity) const { return IsEnabled(entity, m_registry->Get<T>().id); }

    template<class T> void SetResource(T value)
    {
        CheckStructural();
        const auto& info = m_registry->Get<T>();
        auto owned = std::make_unique<ComponentValue>(info, &value, ComponentValue::MoveValue{});
        m_resources.insert_or_assign(info.id, std::move(owned));
    }
    template<class T> const T& ReadResource() const
    { const auto id = m_registry->Get<T>().id; CheckAccess(id, false, true); return *static_cast<const T*>(m_resources.at(id)->Data()); }
    template<class T> T& WriteResource()
    { const auto id = m_registry->Get<T>().id; CheckAccess(id, true, true); return *static_cast<T*>(m_resources.at(id)->Data()); }
    template<class T> bool HasResource() const
    { const auto id = m_registry->Get<T>().id; CheckAccess(id, false, true); return m_resources.contains(id); }

    template<class... Terms> EntityQuery<Terms...> Query();
    // Capture cursors at safe points, after every mutable borrow/job has ended.
    ChangeToken GetChangeToken() const { CheckStructural(); return {m_id, m_version.load(std::memory_order_acquire)}; }
    WorldStatistics GetStatistics() const;
    std::vector<ArchetypeStatistics> GetArchetypeStatistics() const;
    void TrimEmptyChunks();
    void ResetFrameStatistics();
    SystemScheduler& Systems();
    void RequestCancellation() noexcept { m_cancelRequested.store(true, std::memory_order_release); }
    bool IsCancellationRequested() const noexcept { return m_cancelRequested.load(std::memory_order_acquire); }
    void ClearCancellation();
    void Close();

private:
    friend class QueryPlan;
    friend class ChunkView;
    friend class SystemScheduler;
    friend class EntityCommandBuffer;
    friend class WorldArchive;
    friend class WorldTestAccess;
    struct AccessLease;
    struct ThreadAccess;
    class AccessScope;
    static ThreadAccess*& CurrentAccess() noexcept;
    std::shared_ptr<AccessLease> AcquireAccess(std::vector<Access> access, bool scheduler = false);
    void ReleaseAccess(const std::vector<Access>& access) noexcept;
    void CheckOwner() const;
    void CheckReadThread() const;
    void CheckStructural() const;
    void CheckAccess(ComponentId type, bool write, bool resource = false) const;
    uint64_t NextVersion();
    Detail::EntitySlot& Require(Entity entity);
    const Detail::EntitySlot& Require(Entity entity) const;
    Detail::Archetype& GetArchetype(std::vector<ComponentId> signature);
    Detail::Chunk& Destination(Detail::Archetype& archetype, const Detail::Chunk* source,
        ComponentId replacementType = 0, const void* replacement = nullptr);
    void RemoveRow(Detail::Location location, uint64_t version) noexcept;
    void Migrate(Entity entity, Detail::Archetype& target, const ComponentTypeInfo* added,
        void* value, ComponentId replacement = 0);
    void AddMoved(Entity entity, const ComponentTypeInfo& type, void* value);
    void SetMoved(Entity entity, const ComponentTypeInfo& type, void* value);
    const void* ReadRaw(Entity entity, ComponentId type, bool required) const;
    void* WriteRaw(Entity entity, ComponentId type);

    WorldConfig m_config;
    std::shared_ptr<ComponentRegistry> m_registry;
    uint64_t m_id;
    std::thread::id m_owner;
    bool m_closed = false;
    bool m_closing = false;
    std::shared_ptr<const void> m_lifetime = std::make_shared<int>(0);
    std::vector<Detail::EntitySlot> m_slots;
    std::vector<uint64_t> m_scenes;
    uint32_t m_free = ~0u;
    size_t m_entityCount = 0;
    std::map<std::vector<ComponentId>, std::unique_ptr<Detail::Archetype>> m_archetypes;
    std::vector<Detail::Archetype*> m_archetypeOrder;
    std::unordered_map<ComponentId, std::unique_ptr<ComponentValue>> m_resources;
    std::unique_ptr<SystemScheduler> m_systems;
    std::atomic<uint64_t> m_version{1};
    std::atomic<bool> m_cancelRequested{false};
    mutable std::mutex m_accessMutex;
    size_t m_activeLeases = 0;
    struct AccessCount { size_t readers = 0; bool writer = false; };
    std::map<std::pair<ComponentId, bool>, AccessCount> m_accessCounts;
    uint64_t m_chunkAllocations = 0, m_structuralChanges = 0;
    std::atomic<uint64_t> m_queryExecutions{0}, m_jobs{0};
    std::atomic<double> m_queryMilliseconds{0}, m_jobWaitMilliseconds{0};
};

struct World::AccessLease
{
    World* world;
    std::vector<Access> access;
    ~AccessLease() { if (world) world->ReleaseAccess(access); }
};
struct World::ThreadAccess
{
    World* world;
    std::shared_ptr<AccessLease> lease;
    ThreadAccess* previous;
};
class World::AccessScope
{
public:
    explicit AccessScope(std::shared_ptr<AccessLease> lease)
        : m_context{lease->world, std::move(lease), CurrentAccess()} { CurrentAccess() = &m_context; }
    ~AccessScope() { CurrentAccess() = m_context.previous; }
    AccessScope(const AccessScope&) = delete;
    AccessScope& operator=(const AccessScope&) = delete;
private:
    ThreadAccess m_context;
};
}

#include "ECS/Query/EntityQuery.h"
