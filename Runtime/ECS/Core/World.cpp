#include "World.h"
#include "ECS/Systems/SystemScheduler.h"
#include <algorithm>
#include <limits>

namespace NLS::ECS
{
namespace
{
std::atomic<uint64_t> nextWorld{1};
uint64_t AllocateWorldId()
{
    auto id = nextWorld.load(std::memory_order_relaxed);
    for (;;)
    {
        if (id == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("ECS World identity space exhausted");
        if (nextWorld.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) return id;
    }
}
void Normalize(std::vector<ComponentId>& ids)
{ std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end()); }
}

World::ThreadAccess*& World::CurrentAccess() noexcept
{
    // TLS is owned inside the runtime DLL, accessed through an exported method.
    // MSVC cannot export a thread_local static data member of an exported class.
    static thread_local ThreadAccess* access = nullptr;
    return access;
}
World::World(WorldConfig config, std::shared_ptr<ComponentRegistry> registry)
    : m_config(std::move(config)), m_registry(registry ? std::move(registry) : std::make_shared<ComponentRegistry>()),
      m_id(AllocateWorldId()), m_owner(std::this_thread::get_id())
{
    if (m_config.chunkBytes < sizeof(Entity) || m_config.chunkBytes > (size_t(1) << 30) || !m_config.chunksPerJob)
        throw std::invalid_argument("Invalid ECS World chunk configuration");
}
World::~World()
{
    // Destruction on an unrelated thread or from inside a borrowed view is a
    // programming error: silently freeing active chunk storage would be unsafe.
    try { Close(); } catch (...) { std::terminate(); }
}
void World::CheckOwner() const
{
    if (m_owner != std::this_thread::get_id()) throw std::logic_error("ECS World operation requires its owning thread");
}
void World::CheckReadThread() const
{
    if (m_closed) throw std::logic_error("ECS World is closed");
    const auto* access = CurrentAccess();
    if (m_owner != std::this_thread::get_id() && (!access || access->world != this))
        throw std::logic_error("ECS worker access requires a scheduled access scope");
}
void World::CheckStructural() const
{
    CheckOwner();
    if (m_closed) throw std::logic_error("ECS World is closed");
    std::lock_guard lock(m_accessMutex);
    if (m_activeLeases) throw std::logic_error("Structural ECS mutation during iteration/system execution; use EntityCommandBuffer");
}
void World::CheckAccess(ComponentId type, bool write, bool resource) const
{
    if (m_closed) throw std::logic_error("ECS World is closed");
    const auto* access = CurrentAccess();
    if (access && access->world == this)
    {
        for (const auto& declared : access->lease->access)
            if (declared.type == type && declared.resource == resource && (!write || declared.write)) return;
        throw std::logic_error("Undeclared ECS component/resource access");
    }
    CheckOwner();
    std::lock_guard lock(m_accessMutex);
    if (m_activeLeases) throw std::logic_error("Direct ECS access while queries/jobs are active");
}
std::shared_ptr<World::AccessLease> World::AcquireAccess(std::vector<Access> access, bool scheduler)
{
    if (m_closed) throw std::logic_error("ECS World is closed");
    std::sort(access.begin(), access.end(), [](const Access& a, const Access& b)
    { return std::pair(a.type, a.resource) < std::pair(b.type, b.resource); });
    std::vector<Access> normalized;
    for (const auto& item : access)
    {
        const auto& type = m_registry->Get(item.type);
        if (!item.resource && item.write && (type.kind == ComponentKind::Shared || type.kind == ComponentKind::Tag))
            throw std::invalid_argument("Shared/tag ECS values cannot be accessed through mutable columns");
        if (!normalized.empty() && normalized.back().type == item.type && normalized.back().resource == item.resource)
            normalized.back().write = normalized.back().write || item.write;
        else normalized.push_back(item);
    }
    const auto* current = CurrentAccess();
    if (current && current->world == this)
    {
        for (const auto& item : normalized)
        {
            const auto& old = current->lease->access;
            if (std::none_of(old.begin(), old.end(), [&](const Access& held)
                { return held.type == item.type && held.resource == item.resource && (!item.write || held.write); }))
                throw std::logic_error("ECS query exceeds the enclosing system's declared access");
        }
        return current->lease;
    }
    if (!scheduler) CheckOwner();
    auto lease = std::make_shared<AccessLease>();
    lease->world = nullptr;
    lease->access = std::move(normalized);
    std::lock_guard lock(m_accessMutex);
    for (const auto& item : lease->access)
    {
        const auto iterator = m_accessCounts.find({item.type, item.resource});
        if (iterator != m_accessCounts.end() &&
            (iterator->second.writer || (item.write && iterator->second.readers)))
            throw std::logic_error("Conflicting concurrent ECS access");
    }
    // Allocate all map entries before reserving any access (strong guarantee).
    for (const auto& item : lease->access) m_accessCounts.try_emplace({item.type, item.resource});
    for (const auto& item : lease->access)
    {
        auto& count = m_accessCounts.at({item.type, item.resource});
        if (item.write) count.writer = true; else ++count.readers;
    }
    ++m_activeLeases;
    lease->world = this;
    return lease;
}
void World::ReleaseAccess(const std::vector<Access>& access) noexcept
{
    std::lock_guard lock(m_accessMutex);
    for (const auto& item : access)
    {
        auto& count = m_accessCounts.at({item.type, item.resource});
        if (item.write) count.writer = false; else --count.readers;
    }
    --m_activeLeases;
}
uint64_t World::NextVersion()
{
    auto version = m_version.load(std::memory_order_relaxed);
    for (;;)
    {
        if (version == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("ECS change counter exhausted; rebuild the World");
        if (m_version.compare_exchange_weak(version, version + 1, std::memory_order_acq_rel)) return version + 1;
    }
}
bool World::IsAlive(Entity entity) const
{
    if (m_closed || entity.world != m_id || !entity.IsValid()) return false;
    CheckReadThread();
    return entity.index < m_slots.size() && m_slots[entity.index].alive && m_slots[entity.index].generation == entity.generation;
}
Detail::EntitySlot& World::Require(Entity entity)
{
    if (!IsAlive(entity)) throw std::invalid_argument("Stale, foreign, or destroyed ECS Entity");
    return m_slots[entity.index];
}
const Detail::EntitySlot& World::Require(Entity entity) const { return const_cast<World*>(this)->Require(entity); }
Detail::Archetype& World::GetArchetype(std::vector<ComponentId> signature)
{
    Normalize(signature);
    if (const auto iterator = m_archetypes.find(signature); iterator != m_archetypes.end()) return *iterator->second;
    auto archetype = std::make_unique<Detail::Archetype>();
    archetype->id = m_archetypeOrder.size();
    archetype->signature = signature;
    for (const auto id : signature) archetype->types.push_back(&m_registry->Get(id));
    // Validate layouts now, before publishing the archetype to query caches.
    auto first = std::make_unique<Detail::Chunk>(archetype->types, m_config.chunkBytes);
    archetype->allocationHint = first.get();
    archetype->chunks.push_back(std::move(first));
    m_archetypeOrder.reserve(m_archetypeOrder.size() + 1);
    auto* pointer = archetype.get();
    m_archetypes.emplace(std::move(signature), std::move(archetype));
    m_archetypeOrder.push_back(pointer);
    ++m_chunkAllocations;
    return *pointer;
}
Detail::Chunk& World::Destination(Detail::Archetype& archetype, const Detail::Chunk* source,
    ComponentId replacementType, const void* replacement)
{
    struct SharedKey { const ComponentTypeInfo* type; const void* value; std::unique_ptr<ComponentValue> owned; };
    std::vector<SharedKey> keys;
    for (auto* type : archetype.types)
    {
        if (type->kind != ComponentKind::Shared) continue;
        SharedKey key{type, nullptr, {}};
        if (replacementType == type->id && replacement) key.value = replacement;
        else if (source && source->Find(type->id)) key.value = source->At(*source->Find(type->id), 0);
        else { key.owned = std::make_unique<ComponentValue>(*type); key.value = key.owned->Data(); }
        keys.push_back(std::move(key));
    }
    const auto matches = [&](Detail::Chunk& chunk)
    {
        if (chunk.count == chunk.capacity) return false;
        if (!chunk.count) return true;
        for (const auto& key : keys)
            if (!key.type->equals(chunk.Find(key.type->id)->shared->Data(), key.value)) return false;
        return true;
    };
    Detail::Chunk* destination = nullptr;
    if (archetype.allocationHint && matches(*archetype.allocationHint)) destination = archetype.allocationHint;
    if (!destination)
        for (auto& chunk : archetype.chunks) if (matches(*chunk)) { destination = chunk.get(); break; }
    if (!destination)
    {
        auto chunk = std::make_unique<Detail::Chunk>(archetype.types, m_config.chunkBytes);
        destination = chunk.get();
        archetype.chunks.push_back(std::move(chunk));
        ++m_chunkAllocations;
    }
    if (!destination->count && !keys.empty())
    {
        std::vector<std::unique_ptr<ComponentValue>> copies;
        copies.reserve(keys.size());
        for (const auto& key : keys) copies.push_back(std::make_unique<ComponentValue>(*key.type, key.value));
        for (size_t index = 0; index < keys.size(); ++index)
            destination->Find(keys[index].type->id)->shared = std::move(copies[index]);
    }
    archetype.allocationHint = destination;
    return *destination;
}
Entity World::CreateEntity(std::span<const ComponentId> components)
{
    CheckStructural();
    auto& archetype = GetArchetype({components.begin(), components.end()});
    auto& chunk = Destination(archetype, nullptr);
    const auto version = NextVersion();
    const bool reused = m_free != ~0u;
    if (!reused)
    {
        if (m_slots.size() >= std::numeric_limits<uint32_t>::max()) throw std::length_error("ECS entity index space exhausted");
        m_slots.emplace_back();
        try { m_scenes.push_back(0); } catch (...) { m_slots.pop_back(); throw; }
    }
    const auto index = reused ? m_free : static_cast<uint32_t>(m_slots.size() - 1);
    auto& slot = m_slots[index];
    const Entity entity{m_id, index, slot.generation};
    size_t constructed = 0;
    try
    {
        for (; constructed < chunk.columns.size(); ++constructed)
        {
            auto& column = chunk.columns[constructed];
            if (column.type->HasColumn()) column.type->construct(chunk.At(column, chunk.count));
        }
    }
    catch (...)
    {
        while (constructed) { auto& column = chunk.columns[--constructed]; if (column.type->HasColumn()) column.type->destroy(chunk.At(column, chunk.count)); }
        if (!reused) { m_slots.pop_back(); m_scenes.pop_back(); }
        throw;
    }
    if (reused) m_free = slot.nextFree;
    std::construct_at(chunk.Entities() + chunk.count, entity);
    for (auto& column : chunk.columns)
    {
        column.changeVersion = version;
        if (!column.enabled.empty()) column.enabled[chunk.count] = 1;
    }
    slot.alive = true;
    slot.location = {&archetype, &chunk, chunk.count++};
    m_scenes[index] = 0;
    ++archetype.entityCount; ++m_entityCount; ++m_structuralChanges;
    return entity;
}
std::vector<Entity> World::CreateEntities(size_t count, std::span<const ComponentId> components)
{
    CheckStructural();
    if (count > std::numeric_limits<uint32_t>::max() - m_entityCount) throw std::length_error("ECS entity batch too large");
    std::vector<Entity> result;
    result.reserve(count);
    try { for (size_t index = 0; index < count; ++index) result.push_back(CreateEntity(components)); }
    catch (...) { for (const auto entity : result) DestroyEntity(entity); throw; }
    return result;
}
void World::RemoveRow(Detail::Location location, uint64_t version) noexcept
{
    auto& chunk = *location.chunk;
    const auto last = chunk.count - 1;
    for (auto& column : chunk.columns)
    {
        if (column.type->HasColumn())
        {
            column.type->destroy(chunk.At(column, location.row));
            if (location.row != last)
            {
                column.type->move(chunk.At(column, location.row), chunk.At(column, last));
                column.type->destroy(chunk.At(column, last));
            }
        }
        if (!column.enabled.empty()) column.enabled[location.row] = column.enabled[last];
        column.changeVersion = version;
    }
    if (location.row != last)
    {
        const auto moved = chunk.Entities()[last];
        chunk.Entities()[location.row] = moved;
        m_slots[moved.index].location.row = location.row;
    }
    std::destroy_at(chunk.Entities() + last);
    --chunk.count; --location.archetype->entityCount;
    location.archetype->allocationHint = &chunk;
}
bool World::DestroyEntity(Entity entity)
{
    CheckStructural();
    if (!IsAlive(entity)) return false;
    auto& slot = m_slots[entity.index];
    RemoveRow(slot.location, NextVersion());
    slot.alive = false;
    slot.location = {};
    m_scenes[entity.index] = 0;
    if (slot.generation != std::numeric_limits<uint32_t>::max())
    { ++slot.generation; slot.nextFree = m_free; m_free = entity.index; }
    --m_entityCount; ++m_structuralChanges;
    return true;
}
size_t World::DestroyEntities(std::span<const Entity> entities)
{ CheckStructural(); size_t count = 0; for (const auto entity : entities) count += DestroyEntity(entity) ? 1 : 0; return count; }
void World::Migrate(Entity entity, Detail::Archetype& target, const ComponentTypeInfo* added,
    void* value, ComponentId replacement)
{
    auto& slot = Require(entity);
    const auto old = slot.location;
    auto& destination = Destination(target, old.chunk, replacement ? replacement : (added ? added->id : 0), value);
    if (&destination == old.chunk) return;
    const auto version = NextVersion();
    const auto row = destination.count;
    // All allocations, default/copy construction and shared comparisons finish
    // above this point. The remaining operations cannot throw.
    for (auto& column : destination.columns)
    {
        auto* source = old.chunk->Find(column.type->id);
        if (column.type->HasColumn())
        {
            if (added && added->id == column.type->id) column.type->move(destination.At(column, row), value);
            else column.type->move(destination.At(column, row), old.chunk->At(*source, old.row));
        }
        if (!column.enabled.empty()) column.enabled[row] = source ? source->enabled[old.row] : 1;
        column.changeVersion = version;
    }
    std::construct_at(destination.Entities() + row, entity);
    ++destination.count; ++target.entityCount;
    RemoveRow(old, version);
    slot.location = {&target, &destination, row};
    ++m_structuralChanges;
}
void World::AddMoved(Entity entity, const ComponentTypeInfo& type, void* value)
{
    CheckStructural();
    auto signature = Require(entity).location.archetype->signature;
    if (std::binary_search(signature.begin(), signature.end(), type.id)) throw std::invalid_argument("ECS component already exists");
    signature.push_back(type.id);
    auto& target = GetArchetype(std::move(signature));
    Migrate(entity, target, &type, value);
}
void World::AddComponent(Entity entity, ComponentId type, const void* value)
{
    CheckStructural();
    const auto& info = m_registry->Get(type);
    ComponentValue owned = value ? ComponentValue(info, value) : ComponentValue(info);
    AddMoved(entity, info, owned.Data());
}
bool World::RemoveComponent(Entity entity, ComponentId type)
{
    CheckStructural();
    auto signature = Require(entity).location.archetype->signature;
    const auto iterator = std::lower_bound(signature.begin(), signature.end(), type);
    if (iterator == signature.end() || *iterator != type) return false;
    signature.erase(iterator);
    auto& target = GetArchetype(std::move(signature));
    Migrate(entity, target, nullptr, nullptr);
    return true;
}
bool World::HasComponent(Entity entity, ComponentId type) const
{ return Require(entity).location.archetype->Has(type); }
const void* World::ReadRaw(Entity entity, ComponentId type, bool required) const
{
    CheckAccess(type, false);
    if (!required && !IsAlive(entity)) return nullptr;
    const auto& location = Require(entity).location;
    const auto* column = location.chunk->Find(type);
    if (!column)
    { if (required) throw std::invalid_argument("ECS component missing"); return nullptr; }
    if (column->type->kind == ComponentKind::Tag) throw std::invalid_argument("Use HasComponent for zero-storage ECS tags");
    return location.chunk->At(*column, location.row);
}
void* World::WriteRaw(Entity entity, ComponentId type)
{
    CheckAccess(type, true);
    const auto& location = Require(entity).location;
    auto* column = location.chunk->Find(type);
    if (!column) throw std::invalid_argument("ECS component missing");
    if (!column->type->HasColumn()) throw std::invalid_argument("Use structural SetComponent for shared values and HasComponent for tags");
    column->changeVersion = NextVersion();
    return location.chunk->At(*column, location.row);
}
void World::SetMoved(Entity entity, const ComponentTypeInfo& type, void* value)
{
    if (type.kind == ComponentKind::Shared)
    {
        CheckStructural();
        auto& archetype = *Require(entity).location.archetype;
        if (!archetype.Has(type.id)) throw std::invalid_argument("ECS shared component missing");
        Migrate(entity, archetype, nullptr, value, type.id);
        return;
    }
    auto* destination = WriteRaw(entity, type.id);
    type.destroy(destination);
    type.move(destination, value);
}
void World::SetComponent(Entity entity, ComponentId type, const void* value)
{
    const auto& info = m_registry->Get(type);
    ComponentValue owned(info, value);
    SetMoved(entity, info, owned.Data());
}
void World::SetEnabled(Entity entity, ComponentId type, bool enabled)
{
    CheckAccess(type, true);
    const auto location = Require(entity).location;
    auto* column = location.chunk->Find(type);
    if (!column || column->enabled.empty()) throw std::invalid_argument("ECS component is not enableable");
    if (bool(column->enabled[location.row]) != enabled)
    { const auto version = NextVersion(); column->enabled[location.row] = enabled ? 1 : 0; column->changeVersion = version; }
}
bool World::IsEnabled(Entity entity, ComponentId type) const
{
    CheckAccess(type, false);
    const auto location = Require(entity).location;
    const auto* column = location.chunk->Find(type);
    if (!column) return false;
    return column->enabled.empty() || column->enabled[location.row] != 0;
}
std::vector<Entity> World::GetEntities() const
{
    CheckReadThread();
    std::vector<Entity> entities;
    entities.reserve(m_entityCount);
    for (size_t index = 0; index < m_slots.size(); ++index)
        if (m_slots[index].alive) entities.push_back({m_id, static_cast<uint32_t>(index), m_slots[index].generation});
    return entities;
}
std::vector<ComponentId> World::GetEntityComponents(Entity entity) const { return Require(entity).location.archetype->signature; }
size_t World::GetEntityArchetype(Entity entity) const { return Require(entity).location.archetype->id; }
void World::SetScene(Entity entity, uint64_t scene) { CheckStructural(); Require(entity); m_scenes[entity.index] = scene; }
uint64_t World::GetScene(Entity entity) const { Require(entity); return m_scenes[entity.index]; }
size_t World::UnloadScene(uint64_t scene)
{
    CheckStructural();
    if (!scene) throw std::invalid_argument("Scene zero represents unowned ECS entities");
    size_t count = 0;
    for (uint32_t index = 0; index < m_slots.size(); ++index)
        if (m_slots[index].alive && m_scenes[index] == scene)
            count += DestroyEntity({m_id, index, m_slots[index].generation}) ? 1 : 0;
    return count;
}
WorldStatistics World::GetStatistics() const
{
    CheckStructural();
    WorldStatistics result;
    result.entities = m_entityCount; result.archetypes = m_archetypeOrder.size();
    result.metadataBytes = m_slots.capacity() * sizeof(Detail::EntitySlot) + m_scenes.capacity() * sizeof(uint64_t);
    for (auto* archetype : m_archetypeOrder)
    {
        result.componentInstances += archetype->entityCount * archetype->types.size();
        for (const auto& chunk : archetype->chunks)
        {
            ++result.chunks; result.capacity += chunk->capacity; result.chunkBytes += chunk->bytes;
            result.emptyChunks += chunk->count == 0 ? 1 : 0;
            result.metadataBytes += sizeof(Detail::Chunk) + chunk->columns.capacity() * sizeof(Detail::Column);
            for (const auto& column : chunk->columns)
            { result.metadataBytes += column.enabled.capacity(); if (column.shared) result.sharedBytes += column.type->size; }
        }
    }
    result.chunkAllocations = m_chunkAllocations; result.structuralChanges = m_structuralChanges;
    result.queryExecutions = m_queryExecutions.load(); result.jobs = m_jobs.load();
    result.queryMilliseconds = m_queryMilliseconds.load(); result.jobWaitMilliseconds = m_jobWaitMilliseconds.load();
    return result;
}
std::vector<ArchetypeStatistics> World::GetArchetypeStatistics() const
{
    CheckStructural();
    std::vector<ArchetypeStatistics> result;
    for (const auto* archetype : m_archetypeOrder)
    {
        ArchetypeStatistics entry;
        entry.id = archetype->id; entry.components = archetype->signature; entry.entities = archetype->entityCount;
        entry.chunks = archetype->chunks.size();
        for (const auto& chunk : archetype->chunks) { entry.capacity += chunk->capacity; entry.bytes += chunk->bytes; }
        result.push_back(std::move(entry));
    }
    return result;
}
void World::TrimEmptyChunks()
{
    CheckStructural();
    for (auto* archetype : m_archetypeOrder)
    {
        archetype->allocationHint = nullptr;
        size_t empty = 0;
        std::erase_if(archetype->chunks, [&](const auto& chunk)
        { return !chunk->count && empty++ >= m_config.retainedEmptyChunksPerArchetype; });
    }
}
void World::ResetFrameStatistics()
{
    CheckStructural(); m_structuralChanges = 0; m_queryExecutions = 0; m_jobs = 0;
    m_queryMilliseconds = 0; m_jobWaitMilliseconds = 0;
}
void World::ClearCancellation() { CheckStructural(); m_cancelRequested.store(false, std::memory_order_release); }
void World::Close()
{
    if (m_closed || m_closing) return;
    CheckStructural();
    m_closing = true;
    RequestCancellation();
    try { if (m_systems) { m_systems->Shutdown(); m_systems.reset(); } }
    catch (...) { m_closing = false; throw; }
    m_archetypes.clear(); m_archetypeOrder.clear(); m_slots.clear(); m_scenes.clear(); m_resources.clear();
    m_entityCount = 0; m_free = ~0u; m_closed = true; m_closing = false; m_lifetime.reset();
}
SystemScheduler& World::Systems()
{
    CheckStructural();
    if (!m_systems) m_systems = std::make_unique<SystemScheduler>(*this);
    return *m_systems;
}
}
