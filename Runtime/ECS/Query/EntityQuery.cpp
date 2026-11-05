#include "EntityQuery.h"
#include "ECS/Jobs/JobBatch.h"
#include <algorithm>
#include <chrono>
#include <exception>

namespace NLS::ECS
{
namespace
{
struct BusyGuard
{
    explicit BusyGuard(std::atomic_flag& value) : flag(value)
    { if (flag.test_and_set(std::memory_order_acquire)) throw std::logic_error("An ECS query cannot execute/configure concurrently with itself"); }
    ~BusyGuard() { flag.clear(std::memory_order_release); }
    std::atomic_flag& flag;
};
bool Contains(const std::vector<ComponentId>& values, ComponentId id)
{ return std::find(values.begin(), values.end(), id) != values.end(); }
using Clock = std::chrono::steady_clock;
}
QueryPlan::QueryPlan(World& world, QueryDescription description)
    : m_world(world), m_lifetime(world.GetLifetime()), m_description(std::move(description)) { Validate(); }
World& QueryPlan::GetWorld() const
{
    if (m_lifetime.expired()) throw std::logic_error("ECS query World was destroyed");
    m_world.CheckReadThread(); return m_world;
}
void QueryPlan::Configure(QueryDescription description)
{
    BusyGuard guard(m_busy);
    if (m_lifetime.expired()) throw std::logic_error("ECS query World was destroyed");
    m_world.CheckReadThread();
    auto old = std::move(m_description);
    m_description = std::move(description);
    try { Validate(); } catch (...) { m_description = std::move(old); throw; }
    bool sameChangedTypes = old.changed.size() == m_description.changed.size();
    if (sameChangedTypes)
        for (size_t index = 0; index < old.changed.size(); ++index)
            if (old.changed[index].first != m_description.changed[index].first) { sameChangedTypes = false; break; }
    if (old.all != m_description.all || old.any != m_description.any || old.none != m_description.none ||
        old.enabled != m_description.enabled || old.disabled != m_description.disabled || !sameChangedTypes)
    { m_matches.clear(); m_scannedArchetypes = 0; }
}
void QueryPlan::Validate()
{
    if (m_lifetime.expired()) throw std::logic_error("ECS query World was destroyed");
    m_world.CheckReadThread();
    const auto validate = [&](const auto& ids) { for (const auto id : ids) m_world.GetRegistry().Get(id); };
    validate(m_description.all); validate(m_description.any); validate(m_description.none);
    for (const auto& access : m_description.access)
    {
        const auto& info = m_world.GetRegistry().Get(access.type);
        if (access.resource || (access.write && !info.HasColumn())) throw std::invalid_argument("Invalid ECS query access declaration");
    }
    for (const auto& list : {m_description.enabled, m_description.disabled})
        for (const auto id : list)
            if (m_world.GetRegistry().Get(id).kind != ComponentKind::Enableable)
                throw std::invalid_argument("Enabled/Disabled filters require enableable ECS components");
    for (const auto& [type, token] : m_description.changed)
    {
        m_world.GetRegistry().Get(type);
        if (token.world != m_world.GetId() || token.version > m_world.m_version.load(std::memory_order_acquire))
            throw std::invalid_argument("Foreign or future ECS change token");
    }
    for (const auto entity : m_description.entities)
        if (entity.world != m_world.GetId()) throw std::invalid_argument("Cross-World ECS entity query filter");
}
bool QueryPlan::Matches(const Detail::Archetype& archetype) const
{
    for (const auto id : m_description.all) if (!archetype.Has(id)) return false;
    for (const auto id : m_description.none) if (archetype.Has(id)) return false;
    if (!m_description.any.empty() && std::none_of(m_description.any.begin(), m_description.any.end(),
        [&](ComponentId id) { return archetype.Has(id); })) return false;
    for (const auto id : m_description.enabled) if (!archetype.Has(id)) return false;
    for (const auto id : m_description.disabled) if (!archetype.Has(id)) return false;
    for (const auto& [id, version] : m_description.changed) if (!archetype.Has(id)) return false;
    return true;
}
void QueryPlan::Refresh()
{
    while (m_scannedArchetypes < m_world.m_archetypeOrder.size())
    {
        auto* archetype = m_world.m_archetypeOrder[m_scannedArchetypes++];
        if (Matches(*archetype)) m_matches.push_back(archetype);
    }
}
bool QueryPlan::Matches(const Detail::Chunk& chunk) const
{
    if (!chunk.count) return false;
    for (const auto& [id, token] : m_description.changed)
        if (chunk.Find(id)->changeVersion <= token.version) return false;
    for (size_t row = 0; row < chunk.count; ++row) if (Matches(chunk, row)) return true;
    return false;
}
bool QueryPlan::Matches(const Detail::Chunk& chunk, size_t row) const
{
    if (row >= chunk.count) throw std::out_of_range("ECS chunk row out of range");
    if (!m_description.entities.empty() && std::find(m_description.entities.begin(), m_description.entities.end(), chunk.Entities()[row]) == m_description.entities.end()) return false;
    const auto enabled = [&](ComponentId id)
    { const auto* column = chunk.Find(id); return column && (column->enabled.empty() || column->enabled[row]); };
    if (!m_description.includeDisabled)
        for (const auto id : m_description.all)
            if (!Contains(m_description.disabled, id) && !enabled(id)) return false;
    if (!m_description.any.empty() && std::none_of(m_description.any.begin(), m_description.any.end(),
        [&](ComponentId id) { return chunk.Find(id) && (m_description.includeDisabled || enabled(id)); })) return false;
    for (const auto id : m_description.enabled) if (!enabled(id)) return false;
    for (const auto id : m_description.disabled) if (enabled(id)) return false;
    return true;
}
bool QueryPlan::Allows(ComponentId type, bool write) const
{
    return std::any_of(m_description.access.begin(), m_description.access.end(),
        [&](const Access& access) { return access.type == type && (!write || access.write); });
}
void QueryPlan::PrepareWrites(Detail::Chunk& chunk)
{
    uint64_t version = 0;
    for (const auto& access : m_description.access)
        if (access.write)
            if (auto* column = chunk.Find(access.type))
            { if (!version) version = m_world.NextVersion(); column->changeVersion = version; }
}
void QueryPlan::ForEachChunk(const std::function<void(ChunkView&)>& visitor)
{
    BusyGuard guard(m_busy);
    if (m_lifetime.expired()) throw std::logic_error("ECS query World was destroyed");
    const auto started = m_world.m_config.enableStatistics ? Clock::now() : Clock::time_point{};
    auto lease = m_world.AcquireAccess(m_description.access);
    World::AccessScope scope(lease);
    Refresh();
    for (auto* archetype : m_matches)
        for (auto& chunk : archetype->chunks)
            if (Matches(*chunk))
            {
                if (m_world.IsCancellationRequested()) throw std::runtime_error("ECS query cancelled");
                PrepareWrites(*chunk);
                ChunkView view(*this, *chunk); visitor(view);
            }
    if (m_world.m_config.enableStatistics)
    {
        ++m_world.m_queryExecutions;
        m_world.m_queryMilliseconds.fetch_add(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    }
}
void QueryPlan::ParallelForEachChunk(const std::function<void(ChunkView&)>& visitor)
{
    if (m_lifetime.expired()) throw std::logic_error("ECS query World was destroyed");
    if (m_world.GetConfig().singleThreaded || m_world.GetEntityCount() < m_world.GetConfig().minimumParallelEntities)
    { ForEachChunk(visitor); return; }
    BusyGuard guard(m_busy);
    if (m_lifetime.expired()) throw std::logic_error("ECS query World was destroyed");
    const auto started = m_world.m_config.enableStatistics ? Clock::now() : Clock::time_point{};
    auto lease = m_world.AcquireAccess(m_description.access);
    Refresh();
    std::vector<Detail::Chunk*> chunks;
    for (auto* archetype : m_matches) for (auto& chunk : archetype->chunks) if (Matches(*chunk)) chunks.push_back(chunk.get());
    const size_t grain = m_world.GetConfig().chunksPerJob;
    Detail::JobBatch jobs;
    jobs.AddParallelFor(chunks.size(), grain, [&, lease](size_t index)
    {
        World::AccessScope scope(lease);
        if (m_world.IsCancellationRequested()) throw std::runtime_error("ECS chunk job cancelled");
        PrepareWrites(*chunks[index]);
        ChunkView view(*this, *chunks[index]); visitor(view);
    }, "ECS chunk range");
    const auto waiting = m_world.m_config.enableStatistics ? Clock::now() : Clock::time_point{};
    jobs.Wait();
    if (m_world.m_config.enableStatistics)
    {
        ++m_world.m_queryExecutions; m_world.m_jobs.fetch_add(jobs.GetJobCount());
        m_world.m_queryMilliseconds.fetch_add(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
        m_world.m_jobWaitMilliseconds.fetch_add(std::chrono::duration<double, std::milli>(Clock::now() - waiting).count());
    }
}
size_t QueryPlan::Count()
{
    BusyGuard guard(m_busy);
    if (m_lifetime.expired()) throw std::logic_error("ECS query World was destroyed");
    // Counting reuses the archetype cache and never marks a value changed.
    auto access = m_description.access;
    for (auto& item : access) item.write = false;
    auto lease = m_world.AcquireAccess(std::move(access));
    World::AccessScope scope(lease);
    Refresh();
    size_t count = 0;
    for (auto* archetype : m_matches)
        for (const auto& chunk : archetype->chunks)
            if (Matches(*chunk)) for (size_t row = 0; row < chunk->count; ++row) count += Matches(*chunk, row) ? 1 : 0;
    return count;
}
size_t ChunkView::Size() const { return m_chunk.count; }
size_t ChunkView::Capacity() const { return m_chunk.capacity; }
std::span<const Entity> ChunkView::Entities() const { return {m_chunk.Entities(), m_chunk.count}; }
bool ChunkView::Matches(size_t row) const { return m_plan.Matches(m_chunk, row); }
bool ChunkView::Has(ComponentId type) const { return m_chunk.Find(type) != nullptr; }
bool ChunkView::IsShared(ComponentId type) const
{ const auto* column = m_chunk.Find(type); return column && column->type->kind == ComponentKind::Shared; }
const ComponentRegistry& ChunkView::Registry() const { return m_plan.m_world.GetRegistry(); }
const void* ChunkView::ReadColumn(ComponentId type) const
{
    if (!m_plan.Allows(type, false)) throw std::logic_error("Undeclared ECS chunk read");
    m_plan.m_world.CheckAccess(type, false);
    const auto* column = m_chunk.Find(type);
    if (!column) return nullptr;
    if (column->type->kind == ComponentKind::Tag) throw std::invalid_argument("An ECS tag has no data column");
    return m_chunk.At(*column, 0);
}
void* ChunkView::WriteColumn(ComponentId type)
{
    if (!m_plan.Allows(type, true)) throw std::logic_error("Undeclared ECS chunk write");
    m_plan.m_world.CheckAccess(type, true);
    auto* column = m_chunk.Find(type);
    if (!column) return nullptr;
    if (!column->type->HasColumn()) throw std::invalid_argument("An ECS shared/tag component has no mutable column");
    column->changeVersion = m_plan.m_world.NextVersion();
    return m_chunk.At(*column, 0);
}
}
