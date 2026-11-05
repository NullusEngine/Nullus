#include "EntityCommandBuffer.h"
#include "ECS/Serialization/WorldArchive.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <shared_mutex>
#include <tuple>

namespace NLS::ECS
{
struct EntityCommandBuffer::PrefabCommand
{
    WorldDocument document;
    ArchiveLoadOptions options;
    std::vector<EntityReference> references;
};
struct EntityCommandBuffer::Command
{
    Operation operation;
    EntityReference target;
    ComponentId type = 0;
    std::unique_ptr<ComponentValue> value;
    bool enabled = true;
    std::vector<ComponentId> signature;
    std::unique_ptr<PrefabCommand> prefab;
};
struct EntityCommandBuffer::Stream
{
    uint64_t key;
    std::mutex mutex;
    std::thread::id owner;
    std::vector<Command> commands;
    void CheckThread()
    {
        if (owner == std::thread::id{}) owner = std::this_thread::get_id();
        else if (owner != std::this_thread::get_id()) throw std::logic_error("Use a distinct ECS command writer per logical job");
    }
};
struct EntityCommandBuffer::State
{
    World* world;
    std::weak_ptr<const void> lifetime;
    std::shared_ptr<ComponentRegistry> registry;
    uint64_t id, epoch = 1;
    mutable std::shared_mutex mutex;
    std::mutex streamsMutex;
    std::map<uint64_t, std::shared_ptr<Stream>> streams;
    bool consumed = false, closed = false;
    void Check(uint64_t expectedEpoch) const
    {
        if (closed || consumed || epoch != expectedEpoch || lifetime.expired())
            throw std::logic_error("Expired, consumed, or reset ECS command buffer/writer");
    }
};
namespace
{
std::atomic<uint64_t> nextBuffer{1};
uint64_t BufferId()
{
    auto id = nextBuffer.load();
    for (;;)
    {
        if (id == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("ECS command buffer IDs exhausted");
        if (nextBuffer.compare_exchange_weak(id, id + 1)) return id;
    }
}
}
EntityCommandBuffer::EntityCommandBuffer(World& world) : m_state(std::make_shared<State>())
{
    m_state->world = &world; m_state->lifetime = world.GetLifetime();
    m_state->registry = world.GetRegistryLifetime(); m_state->id = BufferId();
}
EntityCommandBuffer::~EntityCommandBuffer()
{
    std::unique_lock lock(m_state->mutex);
    m_state->closed = true;
    for (auto& [key, stream] : m_state->streams) stream->commands.clear();
    m_state->streams.clear();
}
EntityCommandBuffer::Writer EntityCommandBuffer::GetWriter(uint64_t sortKey)
{
    std::shared_lock lock(m_state->mutex);
    m_state->Check(m_state->epoch);
    std::lock_guard streamsLock(m_state->streamsMutex);
    auto& stream = m_state->streams[sortKey];
    if (!stream) { stream = std::make_shared<Stream>(); stream->key = sortKey; }
    return Writer(m_state, stream, m_state->registry, m_state->epoch);
}
void EntityCommandBuffer::Writer::Record(Operation operation, EntityReference target, ComponentId type,
    std::unique_ptr<ComponentValue> value, bool enabled)
{
    std::shared_lock lock(m_state->mutex); m_state->Check(m_epoch);
    if (target.IsDeferred() && (target.buffer != m_state->id || target.epoch != m_epoch))
        throw std::invalid_argument("Foreign or stale deferred ECS entity");
    std::lock_guard streamLock(m_stream->mutex); m_stream->CheckThread();
    m_stream->commands.push_back(Command{operation, target, type, std::move(value), enabled, {}});
}
EntityCommandBuffer::Writer EntityCommandBuffer::Writer::Fork(uint32_t sortKey) const
{
    std::shared_lock lock(m_state->mutex); m_state->Check(m_epoch);
    if ((m_stream->key & 0xffffffffull) != 0 || sortKey == std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("ECS parallel writer requires a parent key domain and a local key below UINT32_MAX");
    const auto key = m_stream->key | (uint64_t(sortKey) + 1);
    std::lock_guard streamsLock(m_state->streamsMutex);
    auto& stream = m_state->streams[key];
    if (!stream) { stream = std::make_shared<Stream>(); stream->key = key; }
    return Writer(m_state, stream, m_registry, m_epoch);
}
void EntityCommandBuffer::Writer::Value(Operation operation, EntityReference target,
    const ComponentTypeInfo& type, void* value)
{ Record(operation, target, type.id, std::make_unique<ComponentValue>(type, value, ComponentValue::MoveValue{})); }
EntityReference EntityCommandBuffer::Writer::CreateEntity(std::span<const ComponentId> signature)
{
    std::shared_lock lock(m_state->mutex); m_state->Check(m_epoch);
    std::lock_guard streamLock(m_stream->mutex); m_stream->CheckThread();
    if (m_stream->commands.size() >= std::numeric_limits<uint64_t>::max()) throw std::overflow_error("ECS command sequence exhausted");
    EntityReference reference;
    reference.buffer = m_state->id; reference.epoch = m_epoch; reference.writer = m_stream->key;
    reference.sequence = m_stream->commands.size() + 1;
    Command command{Operation::Create, reference, 0, {}, true, {signature.begin(), signature.end()}};
    m_stream->commands.push_back(std::move(command));
    return reference;
}
RecordedPrefab EntityCommandBuffer::Writer::InstantiatePrefab(const WorldDocument& prefab, uint64_t sceneOwner)
{
    ArchiveLoadOptions options; options.sceneOwner = sceneOwner;
    return InstantiatePrefab(prefab, options);
}
RecordedPrefab EntityCommandBuffer::Writer::InstantiatePrefab(const WorldDocument& prefab,
    const ArchiveLoadOptions& options, std::span<const PrefabOverride> overrides)
{
    auto payload = std::make_unique<PrefabCommand>();
    payload->document = WorldArchive::ApplyOverrides(prefab, overrides); payload->options = options;
    if (payload->document.entities.size() > options.limits.maxEntities) throw std::length_error("ECS recorded prefab entity limit exceeded");
    payload->references.reserve(payload->document.entities.size());
    RecordedPrefab result;
    std::shared_lock lock(m_state->mutex); m_state->Check(m_epoch);
    std::lock_guard streamLock(m_stream->mutex); m_stream->CheckThread();
    if (m_stream->commands.size() >= std::numeric_limits<uint64_t>::max()) throw std::overflow_error("ECS command sequence exhausted");
    for (const auto& entity : payload->document.entities)
    {
        EntityReference reference; reference.buffer = m_state->id; reference.epoch = m_epoch;
        reference.writer = m_stream->key; reference.sequence = m_stream->commands.size() + 1; reference.element = payload->references.size() + 1;
        if (entity.id.empty() || !result.entities.emplace(entity.id, reference).second) throw std::invalid_argument("Invalid recorded prefab entity IDs");
        payload->references.push_back(reference);
    }
    Command command{Operation::Instantiate, {}, 0, {}, true, {}, std::move(payload)};
    m_stream->commands.push_back(std::move(command)); return result;
}
void EntityCommandBuffer::Writer::DestroyEntity(EntityReference target) { Record(Operation::Destroy, target); }
void EntityCommandBuffer::Writer::AddComponent(EntityReference target, ComponentId type, const void* value)
{
    const auto& info = m_registry->Get(type);
    auto owned = value ? std::make_unique<ComponentValue>(info, value) : std::make_unique<ComponentValue>(info);
    Record(Operation::Add, target, type, std::move(owned));
}
void EntityCommandBuffer::Writer::SetComponent(EntityReference target, ComponentId type, const void* value)
{ Record(Operation::Set, target, type, std::make_unique<ComponentValue>(m_registry->Get(type), value)); }
CommandPlaybackResult EntityCommandBuffer::Playback()
{
    std::unique_lock lock(m_state->mutex); m_state->Check(m_state->epoch);
    auto& world = *m_state->world;
    world.CheckStructural();
    const auto started = world.GetConfig().enableStatistics ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    CommandPlaybackResult result;
    size_t count = 0, creates = 0;
    for (const auto& [key, stream] : m_state->streams)
    {
        count += stream->commands.size();
        for (const auto& command : stream->commands)
        {
            const auto additional = command.operation == Operation::Create ? size_t(1) : command.prefab ? command.prefab->references.size() : size_t(0);
            if (creates > std::numeric_limits<size_t>::max() - additional) throw std::length_error("ECS playback entity count overflow");
            creates += additional;
        }
    }
    result.created.reserve(creates); result.errors.reserve(std::min(count, size_t(64)));
    // Once begun, a playback is consumed even when some commands fail. Errors
    // are per-command and later commands still run in the defined order.
    m_state->consumed = true;
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, Entity> resolved;
    for (auto& [key, stream] : m_state->streams)
    {
        uint64_t sequence = 0;
        for (auto& command : stream->commands)
        {
            ++sequence;
            try
            {
                if (command.operation == Operation::Create)
                {
                    // Allocate the map node first; creation cannot leak if map
                    // bookkeeping allocation fails after creating the entity.
                    auto [iterator, inserted] = resolved.try_emplace({key, sequence, 0});
                    iterator->second = world.CreateEntity(command.signature);
                    result.created.emplace_back(command.target, iterator->second);
                }
                else if (command.operation == Operation::Instantiate)
                {
                    const auto& payload = *command.prefab;
                    // All map nodes and result capacity exist before creation,
                    // so bookkeeping cannot leak a successfully loaded prefab.
                    for (const auto& reference : payload.references) resolved.try_emplace({key, sequence, reference.element});
                    const auto instance = WorldArchive::Instantiate(world, payload.document, payload.options);
                    for (size_t i = 0; i < payload.references.size(); ++i)
                    {
                        const auto& reference = payload.references[i]; const auto entity = instance.entities[i];
                        resolved.at({key, sequence, reference.element}) = entity; result.created.emplace_back(reference, entity);
                    }
                }
                else
                {
                    Entity entity = command.target.entity;
                    if (command.target.IsDeferred())
                    {
                        const auto iterator = resolved.find({command.target.writer, command.target.sequence, command.target.element});
                        if (iterator == resolved.end()) throw std::invalid_argument("Deferred ECS create must precede its use in playback order");
                        entity = iterator->second;
                    }
                    if (!world.IsAlive(entity)) throw std::invalid_argument("Command target is foreign, destroyed, or its create failed");
                    switch (command.operation)
                    {
                    case Operation::Destroy: world.DestroyEntity(entity); break;
                    case Operation::Add: world.AddMoved(entity, command.value->Type(), command.value->Data()); break;
                    case Operation::Remove:
                        if (!world.RemoveComponent(entity, command.type)) throw std::invalid_argument("Command removes a missing ECS component");
                        break;
                    case Operation::Set: world.SetMoved(entity, command.value->Type(), command.value->Data()); break;
                    case Operation::Enable: world.SetEnabled(entity, command.type, command.enabled); break;
                    default: break;
                    }
                }
                ++result.applied;
            }
            catch (const std::exception& exception) { result.errors.push_back({key, sequence, exception.what()}); }
        }
        stream->commands.clear();
    }
    if (world.GetConfig().enableStatistics)
        result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return result;
}
void EntityCommandBuffer::Reset()
{
    std::unique_lock lock(m_state->mutex);
    if (m_state->closed || m_state->lifetime.expired()) throw std::logic_error("ECS command buffer World no longer exists");
    if (m_state->epoch == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("ECS command buffer epoch exhausted");
    for (auto& [key, stream] : m_state->streams) stream->commands.clear();
    m_state->streams.clear(); ++m_state->epoch; m_state->consumed = false;
}
size_t EntityCommandBuffer::GetCommandCount() const
{
    std::shared_lock lock(m_state->mutex);
    std::lock_guard streamsLock(m_state->streamsMutex);
    size_t count = 0;
    for (const auto& [key, stream] : m_state->streams)
    { std::lock_guard streamLock(stream->mutex); count += stream->commands.size(); }
    return count;
}
}
