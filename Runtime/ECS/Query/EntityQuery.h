#pragma once
#include "ECS/Core/World.h"
#include <functional>
#include <tuple>

namespace NLS::ECS
{
class QueryPlan;

// Borrowed for one callback only. Spans and references must not escape it.
class NLS_ECS_API ChunkView final
{
public:
    ChunkView(const ChunkView&) = delete;
    ChunkView& operator=(const ChunkView&) = delete;
    size_t Size() const;
    size_t Capacity() const;
    std::span<const Entity> Entities() const;
    bool Matches(size_t row) const;
    bool Has(ComponentId type) const;
    bool IsShared(ComponentId type) const;
    const void* ReadColumn(ComponentId type) const;
    void* WriteColumn(ComponentId type);
    template<class T> std::span<const T> ReadSpan() const
    {
        const auto id = Registry().Get<T>().id;
        if (IsShared(id)) throw std::invalid_argument("Use Shared<T>() for a chunk shared value");
        const auto* pointer = static_cast<const T*>(ReadColumn(id));
        return pointer ? std::span<const T>(pointer, Size()) : std::span<const T>{};
    }
    template<class T> std::span<T> WriteSpan()
    {
        const auto id = Registry().Get<T>().id;
        auto* pointer = static_cast<T*>(WriteColumn(id));
        return pointer ? std::span<T>(pointer, Size()) : std::span<T>{};
    }
    template<class T> const T& Shared() const
    {
        const auto id = Registry().Get<T>().id;
        if (!IsShared(id)) throw std::invalid_argument("Requested ECS component is not shared");
        return *static_cast<const T*>(ReadColumn(id));
    }
    const ComponentRegistry& Registry() const;
private:
    friend class QueryPlan;
    ChunkView(QueryPlan& plan, Detail::Chunk& chunk) : m_plan(plan), m_chunk(chunk) {}
    QueryPlan& m_plan;
    Detail::Chunk& m_chunk;
};

class NLS_ECS_API QueryPlan final
{
public:
    QueryPlan(World& world, QueryDescription description);
    QueryPlan(const QueryPlan&) = delete;
    QueryPlan& operator=(const QueryPlan&) = delete;
    void Configure(QueryDescription description);
    const QueryDescription& Description() const { return m_description; }
    void ForEachChunk(const std::function<void(ChunkView&)>& visitor);
    void ParallelForEachChunk(const std::function<void(ChunkView&)>& visitor);
    size_t GetCachedArchetypeCount() const { return m_matches.size(); }
    size_t Count();
    World& GetWorld() const;
private:
    friend class ChunkView;
    void Validate();
    void Refresh();
    bool Matches(const Detail::Archetype& archetype) const;
    bool Matches(const Detail::Chunk& chunk) const;
    bool Matches(const Detail::Chunk& chunk, size_t row) const;
    bool Allows(ComponentId type, bool write) const;
    void PrepareWrites(Detail::Chunk& chunk);
    World& m_world;
    std::weak_ptr<const void> m_lifetime;
    QueryDescription m_description;
    std::vector<Detail::Archetype*> m_matches;
    size_t m_scannedArchetypes = 0;
    std::atomic_flag m_busy = ATOMIC_FLAG_INIT;
};

template<class... Terms> class EntityQuery final
{
public:
    explicit EntityQuery(World& world)
    {
        QueryDescription description;
        ([&]
        {
            const auto& info = world.GetRegistry().template Get<typename Terms::Type>();
            if (info.kind == ComponentKind::Tag) throw std::invalid_argument("Filter tags with All/Any/None, without a data argument");
            if constexpr (Terms::write)
                if (info.kind == ComponentKind::Shared) throw std::invalid_argument("Shared ECS values are immutable query inputs");
            description.access.push_back({info.id, Terms::write, false});
            if constexpr (!Terms::optional) description.all.push_back(info.id);
        }(), ...);
        m_plan = std::make_unique<QueryPlan>(world, std::move(description));
    }
    EntityQuery(EntityQuery&&) = default;
    EntityQuery& operator=(EntityQuery&&) = delete;
    EntityQuery(const EntityQuery&) = delete;
    EntityQuery& operator=(const EntityQuery&) = delete;

    template<class... T> EntityQuery& All() { return Filter<T...>(&QueryDescription::all); }
    template<class... T> EntityQuery& Any() { return Filter<T...>(&QueryDescription::any); }
    template<class... T> EntityQuery& None() { return Filter<T...>(&QueryDescription::none); }
    template<class... T> EntityQuery& Enabled() { return Filter<T...>(&QueryDescription::enabled); }
    template<class... T> EntityQuery& Disabled() { return Filter<T...>(&QueryDescription::disabled); }
    template<class T> EntityQuery& Changed(ChangeToken token)
    {
        auto description = m_plan->Description();
        const auto id = m_plan->GetWorld().GetRegistry().template Get<T>().id;
        bool found = false;
        for (auto& entry : description.changed) if (entry.first == id) { entry.second = token; found = true; break; }
        if (!found) description.changed.emplace_back(id, token);
        bool hasAccess = false;
        for (const auto& access : description.access) if (access.type == id && !access.resource) { hasAccess = true; break; }
        if (!hasAccess) description.access.push_back({id, false, false});
        m_plan->Configure(std::move(description)); return *this;
    }
    EntityQuery& IncludeDisabled(bool value = true)
    { auto description = m_plan->Description(); description.includeDisabled = value; m_plan->Configure(std::move(description)); return *this; }
    EntityQuery& WithEntities(std::span<const Entity> entities)
    { auto description = m_plan->Description(); description.entities.assign(entities.begin(), entities.end()); m_plan->Configure(std::move(description)); return *this; }
    size_t Count() { return m_plan->Count(); }
    size_t GetCachedArchetypeCount() const { return m_plan->GetCachedArchetypeCount(); }
    const QueryDescription& Description() const { return m_plan->Description(); }
    template<class Function> void Each(Function&& function)
    { m_plan->ForEachChunk([&](ChunkView& chunk) { Visit(chunk, function); }); }
    template<class Function> void ParallelEach(Function&& function)
    { m_plan->ParallelForEachChunk([&](ChunkView& chunk) { Visit(chunk, function); }); }
    template<class Function> void EachChunk(Function&& function) { m_plan->ForEachChunk(std::forward<Function>(function)); }
    template<class Function> void ParallelForEachChunk(Function&& function)
    { m_plan->ParallelForEachChunk(std::forward<Function>(function)); }

private:
    template<class... T> EntityQuery& Filter(std::vector<ComponentId> QueryDescription::* member)
    {
        auto description = m_plan->Description();
        auto& world = m_plan->GetWorld();
        ((description.*member).push_back(world.GetRegistry().template Get<T>().id), ...);
        if (member != &QueryDescription::none)
            (description.access.push_back({world.GetRegistry().template Get<T>().id, false, false}), ...);
        m_plan->Configure(std::move(description)); return *this;
    }
    template<class Term> struct Argument
    {
        using T = typename Term::Type;
        using Pointer = std::conditional_t<Term::write, T*, const T*>;
        Pointer pointer;
        size_t stride;
        decltype(auto) At(size_t row) const
        {
            if constexpr (Term::optional) return pointer ? pointer + row * stride : nullptr;
            else return *(pointer + row * stride);
        }
    };
    template<class Term> static auto Column(ChunkView& chunk)
    {
        const auto id = chunk.Registry().template Get<typename Term::Type>().id;
        if constexpr (Term::write)
            return Argument<Term>{static_cast<typename Term::Type*>(chunk.WriteColumn(id)), 1};
        else
            return Argument<Term>{static_cast<const typename Term::Type*>(chunk.ReadColumn(id)), chunk.IsShared(id) ? size_t(0) : size_t(1)};
    }
    template<class Function> static void Visit(ChunkView& chunk, Function& function)
    {
        auto columns = std::tuple{Column<Terms>(chunk)...};
        for (size_t row = 0; row < chunk.Size(); ++row)
            if (chunk.Matches(row))
                std::apply([&](auto&... column) { std::invoke(function, chunk.Entities()[row], column.At(row)...); }, columns);
    }
    std::unique_ptr<QueryPlan> m_plan;
};

template<class... Terms> EntityQuery<Terms...> World::Query() { return EntityQuery<Terms...>(*this); }
}
