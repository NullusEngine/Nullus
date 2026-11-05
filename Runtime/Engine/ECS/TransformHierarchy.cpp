#include "TransformHierarchy.h"
#include <ECS/Serialization/ComponentCodec.h>
#include <ECS/Jobs/JobBatch.h>
#include <Math/Matrix4.h>
#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace NLS::Engine::ECSIntegration
{
namespace
{
void Validate(const LocalTransform& value)
{
    for (const auto v : value.position) if (!std::isfinite(v)) throw std::invalid_argument("Nonfinite ECS local position");
    for (const auto v : value.scale) if (!std::isfinite(v)) throw std::invalid_argument("Nonfinite ECS local scale");
    double length = 0;
    for (const auto v : value.rotation) { if (!std::isfinite(v)) throw std::invalid_argument("Nonfinite ECS local rotation"); length += double(v) * v; }
    if (length < 1e-12) throw std::invalid_argument("Zero ECS local rotation quaternion");
}
Maths::Matrix4 LocalMatrix(const LocalTransform& value)
{
    double length = 0; for (const auto v : value.rotation) length += double(v) * v;
    const auto inverse = 1.0 / std::sqrt(length);
    const Maths::Quaternion rotation{float(value.rotation[0] * inverse), float(value.rotation[1] * inverse),
        float(value.rotation[2] * inverse), float(value.rotation[3] * inverse)};
    return Maths::Matrix4::Translation({value.position[0], value.position[1], value.position[2]}) *
        Maths::Matrix4::Rotation(rotation) * Maths::Matrix4::Scaling({value.scale[0], value.scale[1], value.scale[2]});
}
using SourceQuery = ECS::EntityQuery<ECS::Read<LocalTransform>, ECS::OptionalRead<Parent>, ECS::OptionalRead<WorldTransform>>;
constexpr size_t None = std::numeric_limits<size_t>::max();
}
void RegisterTransformComponents(ECS::World& world)
{
    world.RegisterComponent<LocalTransform>("Nullus.ECS.LocalTransform");
    world.RegisterComponent<Parent>("Nullus.ECS.Parent");
    const auto& output = world.RegisterComponent<WorldTransform>("Nullus.ECS.WorldTransform");
    if (!output.transient) world.GetRegistry().RegisterTransient(output.id);
    if (!world.GetRegistry().GetCodec(world.GetComponentType<LocalTransform>().id))
        ECS::RegisterComponentCodec<LocalTransform>(world.GetRegistry(),
            [](const LocalTransform& value, const auto&)
            {
                Validate(value); return ECS::ComponentJson{{"position", value.position}, {"rotation", value.rotation}, {"scale", value.scale}};
            },
            [](LocalTransform& value, const ECS::ComponentJson& json, const auto&)
            {
                value = {json.at("position").get<std::array<float, 3>>(), json.at("rotation").get<std::array<float, 4>>(), json.at("scale").get<std::array<float, 3>>()};
                Validate(value);
            });
    if (!world.GetRegistry().GetCodec(world.GetComponentType<Parent>().id))
        ECS::RegisterComponentCodec<Parent>(world.GetRegistry(),
            [](const Parent& value, const auto& context) { return ECS::ComponentJson{{"entity", context.EntityId(value.entity)}}; },
            [](Parent& value, const ECS::ComponentJson& json, const auto& context) { value.entity = context.Resolve(json.at("entity").get<std::string>()); });
}
struct TransformHierarchySystem::Impl
{
    struct Cached { LocalTransform local; ECS::Entity parent; WorldTransform world; };
    struct Node
    {
        ECS::Entity entity, parent;
        LocalTransform local;
        WorldTransform world;
        bool hasWorld = false, staleParent = false, dirty = false;
        size_t parentIndex = None, firstChild = None, nextSibling = None;
    };
    std::unique_ptr<SourceQuery> source;
    std::unordered_map<ECS::Entity, Cached, ECS::EntityHash> cache;
    std::vector<Node> nodes;
    std::unordered_map<ECS::Entity, size_t, ECS::EntityHash> indices;
    std::vector<size_t> order, levelEnds;
    TransformHierarchyStatistics statistics;
};
TransformHierarchySystem::TransformHierarchySystem() : m_impl(std::make_unique<Impl>()) {}
TransformHierarchySystem::~TransformHierarchySystem() = default;
void TransformHierarchySystem::OnCreate(ECS::World& world) { m_impl->source = std::make_unique<SourceQuery>(world); }
void TransformHierarchySystem::OnStop(ECS::World&) noexcept { m_impl->cache.clear(); }
TransformHierarchyStatistics TransformHierarchySystem::GetStatistics() const { return m_impl->statistics; }
void TransformHierarchySystem::OnUpdate(ECS::SystemContext& context)
{
    auto& state = *m_impl; auto& world = context.world;
    state.statistics = {}; state.nodes.clear(); state.indices.clear(); state.order.clear(); state.levelEnds.clear();
    state.source->Each([&](ECS::Entity entity, const LocalTransform& local, const Parent* parent, const WorldTransform* output)
    {
        Validate(local);
        Impl::Node node; node.entity = entity; node.local = local; node.parent = parent ? parent->entity : ECS::Entity{};
        if (output) { node.world = *output; node.hasWorld = true; }
        state.indices.emplace(entity, state.nodes.size()); state.nodes.push_back(node);
    });
    auto& nodes = state.nodes; state.statistics.entities = nodes.size(); state.order.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        auto& node = nodes[i];
        if (node.parent != ECS::Entity{})
        {
            const auto found = state.indices.find(node.parent);
            if (found == state.indices.end()) { node.parent = {}; node.staleParent = true; ++state.statistics.staleParents; }
            else
            {
                node.parentIndex = found->second; node.nextSibling = nodes[node.parentIndex].firstChild; nodes[node.parentIndex].firstChild = i;
            }
        }
        if (node.parentIndex == None) state.order.push_back(i);
    }
    state.statistics.roots = state.order.size();
    for (size_t begin = 0; begin < state.order.size();)
    {
        const auto end = state.order.size(); state.levelEnds.push_back(end);
        for (size_t i = begin; i < end; ++i)
            for (auto child = nodes[state.order[i]].firstChild; child != None; child = nodes[child].nextSibling) state.order.push_back(child);
        begin = end;
    }
    if (state.order.size() != nodes.size()) throw std::invalid_argument("ECS Transform hierarchy contains a cycle");
    state.statistics.levels = state.levelEnds.size();
    const auto compute = [&](size_t index)
    {
        if (world.IsCancellationRequested()) throw std::runtime_error("ECS transform update cancelled");
        auto& node = nodes[index]; auto matrix = LocalMatrix(node.local);
        if (node.parentIndex != None)
        {
            Maths::Matrix4 parent;
            std::copy(nodes[node.parentIndex].world.matrix.begin(), nodes[node.parentIndex].world.matrix.end(), parent.data);
            matrix = parent * matrix;
        }
        for (const auto value : matrix.data) if (!std::isfinite(value)) throw std::overflow_error("Nonfinite ECS world transform");
        std::copy(std::begin(matrix.data), std::end(matrix.data), node.world.matrix.begin());
    };
    for (size_t begin = 0, level = 0; level < state.levelEnds.size(); ++level)
    {
        const auto end = state.levelEnds[level]; std::vector<size_t> dirty; dirty.reserve(end - begin);
        for (size_t i = begin; i < end; ++i)
        {
            auto& node = nodes[state.order[i]]; const auto old = state.cache.find(node.entity);
            node.dirty = !node.hasWorld || old == state.cache.end() || old->second.local != node.local || old->second.parent != node.parent ||
                old->second.world != node.world || (node.parentIndex != None && nodes[node.parentIndex].dirty);
            if (node.dirty)
            {
                const auto revision = old == state.cache.end() ? uint64_t(0) : old->second.world.revision;
                if (revision == UINT64_MAX) throw std::overflow_error("ECS transform revision exhausted");
                node.world.revision = revision + 1; dirty.push_back(state.order[i]);
            }
        }
        state.statistics.recomputed += dirty.size();
        if (context.jobsEnabled && dirty.size() >= world.GetConfig().minimumParallelEntities)
        {
            const auto grain = std::max(size_t(1), std::min(world.GetConfig().chunksPerJob, size_t(64))) * 16;
            ECS::Detail::JobBatch jobs;
            jobs.AddParallelFor(dirty.size(), grain, [&](size_t index) { compute(dirty[index]); }, "ECS transform hierarchy level");
            jobs.Wait(); state.statistics.jobs += jobs.GetJobCount();
        }
        else for (const auto index : dirty) compute(index);
        begin = end;
    }
    // Validation and all level jobs finish before any world output is changed.
    for (const auto& node : nodes)
    {
        if (node.staleParent) context.commands.RemoveComponent<Parent>(node.entity);
        if (node.dirty)
        {
            if (node.hasWorld) world.SetComponent<WorldTransform>(node.entity, node.world);
            else context.commands.AddComponent<WorldTransform>(node.entity, node.world);
        }
        state.cache.insert_or_assign(node.entity, Impl::Cached{node.local, node.parent, node.world});
    }
    std::erase_if(state.cache, [&](const auto& item) { return !state.indices.contains(item.first); });
}
TransformHierarchySystem& RegisterTransformSystem(ECS::World& world, ECS::SystemPhase phase, std::string name)
{
    RegisterTransformComponents(world);
    ECS::SystemDescription description; description.name = std::move(name); description.phase = phase;
    description.Reads<LocalTransform>(world).Writes<Parent>(world).Writes<WorldTransform>(world);
    return world.Systems().RegisterSystem<TransformHierarchySystem>(std::move(description));
}
void SetTransformParent(ECS::World& world, ECS::Entity child, ECS::Entity parent)
{
    (void)world.GetChangeToken();
    if (!world.IsAlive(child) || !world.HasComponent<LocalTransform>(child)) throw std::invalid_argument("ECS child needs a local transform");
    if (parent == ECS::Entity{}) { world.RemoveComponent<Parent>(child); return; }
    std::unordered_set<ECS::Entity, ECS::EntityHash> seen;
    for (auto ancestor = parent; ancestor != ECS::Entity{};)
    {
        if (ancestor == child || !seen.insert(ancestor).second) throw std::invalid_argument("ECS reparent would create or attach to a cycle");
        if (!world.IsAlive(ancestor) || !world.HasComponent<LocalTransform>(ancestor)) throw std::invalid_argument("ECS parent must be a live transform in the same World");
        const auto* link = world.TryGetComponent<Parent>(ancestor); ancestor = link ? link->entity : ECS::Entity{};
    }
    if (world.HasComponent<Parent>(child)) world.SetComponent<Parent>(child, {parent}); else world.AddComponent<Parent>(child, {parent});
}
void AddTransform(ECS::World& world, ECS::Entity entity, LocalTransform local, ECS::Entity parent)
{
    Validate(local); (void)world.GetChangeToken();
    if (world.HasComponent<LocalTransform>(entity)) throw std::invalid_argument("ECS entity already has a local transform");
    if (parent != ECS::Entity{} && (!world.IsAlive(parent) || !world.HasComponent<LocalTransform>(parent))) throw std::invalid_argument("Invalid ECS transform parent");
    world.AddComponent<LocalTransform>(entity, local);
    try { if (parent != ECS::Entity{}) SetTransformParent(world, entity, parent); }
    catch (...) { world.RemoveComponent<LocalTransform>(entity); throw; }
}
}
