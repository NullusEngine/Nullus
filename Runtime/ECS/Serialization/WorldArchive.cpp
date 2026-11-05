#include "WorldArchive.h"
#include "ECS/Core/World.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace NLS::ECS
{
namespace
{
std::string Hex(uint64_t value)
{
    std::string result(16, '0');
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) { result[15 - i] = digits[value & 15]; value >>= 4; }
    return result;
}
uint64_t ReadHex(const ComponentJson& json)
{
    const auto text = json.get<std::string>(); uint64_t result = 0;
    if (text.size() != 16) throw std::invalid_argument("ECS stable ID must contain 16 hexadecimal digits");
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !result)
        throw std::invalid_argument("Invalid ECS stable component ID");
    return result;
}
uint64_t Unsigned(const ComponentJson& value, uint64_t maximum)
{
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::invalid_argument("Expected an unsigned ECS schema integer");
    const auto result = value.get<uint64_t>();
    if (result > maximum) throw std::invalid_argument("ECS schema integer is out of range");
    return result;
}
const char* KindName(ComponentKind kind)
{
    switch (kind)
    {
    case ComponentKind::Data: return "data";
    case ComponentKind::Tag: return "tag";
    case ComponentKind::Buffer: return "buffer";
    case ComponentKind::Shared: return "shared";
    case ComponentKind::Enableable: return "enableable";
    }
    throw std::invalid_argument("Unknown ECS component storage kind");
}
ComponentKind ReadKind(const std::string& name)
{
    for (const auto kind : {ComponentKind::Data, ComponentKind::Tag, ComponentKind::Buffer, ComponentKind::Shared, ComponentKind::Enableable})
        if (name == KindName(kind)) return kind;
    throw std::invalid_argument("Unknown ECS component storage kind: " + name);
}
void StringLimit(const std::string& text, bool required = true)
{
    if ((required && text.empty()) || text.size() > 4096) throw std::invalid_argument("Invalid or excessively long ECS schema identifier");
}
void ValidateJson(const ComponentJson& value, size_t maximumDepth)
{
    ValidateComponentJson(value, maximumDepth);
}
void Validate(const WorldDocument& document, const ArchiveLimits& limits)
{
    if (document.kind != DocumentKind::Scene && document.kind != DocumentKind::Prefab)
        throw std::invalid_argument("Invalid ECS document kind");
    if (document.entities.size() > limits.maxEntities || document.resources.size() > limits.maxEntities)
        throw std::length_error("ECS document entity/dependency limit exceeded");
    std::set<std::string> ids, authoringIds, resources;
    for (const auto& entity : document.entities)
    {
        StringLimit(entity.id); StringLimit(entity.authoringId, false);
        if (!ids.insert(entity.id).second || (!entity.authoringId.empty() && !authoringIds.insert(entity.authoringId).second))
            throw std::invalid_argument("Duplicate ECS persistent entity/authoring ID");
        if (entity.components.size() > limits.maxComponentsPerEntity) throw std::length_error("ECS component count limit exceeded");
        std::set<ComponentId> types;
        for (const auto& component : entity.components)
        {
            StringLimit(component.name); (void)KindName(component.kind);
            if (!component.type || !component.version || !types.insert(component.type).second)
                throw std::invalid_argument("Invalid or duplicate ECS component schema");
            if (!component.enabled && component.kind != ComponentKind::Enableable)
                throw std::invalid_argument("Only enableable ECS components may store a disabled flag");
            if (component.kind == ComponentKind::Tag && (!component.value.is_object() || !component.value.empty()))
                throw std::invalid_argument("ECS tags cannot contain serialized fields");
            ValidateJson(component.value, limits.maxDepth);
        }
    }
    for (const auto& dependency : document.resources)
    {
        StringLimit(dependency.guid); StringLimit(dependency.hashOrVersion, false);
        if (!resources.insert(dependency.guid).second) throw std::invalid_argument("Duplicate ECS resource dependency");
    }
}
std::vector<Access> ReadAccess(const World& world)
{
    std::vector<Access> result;
    for (const auto* type : world.GetRegistry().GetTypes()) result.push_back({type->id, false, false});
    return result;
}
ComponentJson Payload(const WorldDocument& document)
{
    ComponentJson result{{"kind", document.kind == DocumentKind::Scene ? "scene" : "prefab"},
        {"entities", ComponentJson::array()}, {"archetypes", ComponentJson::array()}, {"resources", ComponentJson::array()}};
    std::map<std::vector<ComponentId>, size_t> layouts;
    for (const auto& entity : document.entities)
    {
        std::vector<ComponentId> signature;
        for (const auto& component : entity.components) signature.push_back(component.type);
        std::sort(signature.begin(), signature.end());
        const auto [layout, inserted] = layouts.emplace(signature, layouts.size());
        if (inserted)
        {
            auto types = ComponentJson::array(); for (const auto type : signature) types.push_back(Hex(type));
            result["archetypes"].push_back(std::move(types));
        }
        ComponentJson record{{"id", entity.id}, {"authoring_id", entity.authoringId},
            {"archetype", layout->second}, {"components", ComponentJson::array()}};
        for (const auto& component : entity.components)
            record["components"].push_back({{"type", Hex(component.type)}, {"name", component.name}, {"version", component.version},
                {"kind", KindName(component.kind)}, {"enabled", component.enabled}, {"value", component.value}});
        result["entities"].push_back(std::move(record));
    }
    for (const auto& dependency : document.resources)
        result["resources"].push_back({{"guid", dependency.guid}, {"hash_or_version", dependency.hashOrVersion}});
    return result;
}
// The bundled JSON callback DOM parser scans its parent array at every object
// end to remove discarded values, even when the callback never discards. Avoid
// quadratic entity-array parsing: validate with SAX, then use the ordinary DOM.
class ArchiveJsonValidator
{
public:
    explicit ArchiveJsonValidator(size_t maximumDepth) : m_maximumDepth(maximumDepth) {}
    bool null() { return Scalar(); }
    bool boolean(bool) { return Scalar(); }
    bool number_integer(ComponentJson::number_integer_t) { return Scalar(); }
    bool number_unsigned(ComponentJson::number_unsigned_t) { return Scalar(); }
    bool number_float(ComponentJson::number_float_t value, const ComponentJson::string_t&)
    { if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite ECS JSON number"); return Scalar(); }
    bool string(ComponentJson::string_t&) { return Scalar(); }
    bool binary(ComponentJson::binary_t&) { throw std::invalid_argument("Binary values are not supported in ECS JSON archives"); }
    bool start_object(size_t) { Scalar(); m_frames.push_back({true, {}}); return true; }
    bool start_array(size_t) { Scalar(); m_frames.push_back({false, {}}); return true; }
    bool key(ComponentJson::string_t& name)
    {
        Scalar();
        if (m_frames.empty() || !m_frames.back().object || !m_frames.back().keys.insert(name).second)
            throw std::invalid_argument("Duplicate or invalid ECS JSON object key");
        return true;
    }
    bool end_object() { return End(true); }
    bool end_array() { return End(false); }
    template<class Exception> bool parse_error(size_t, const std::string&, const Exception& error)
    { throw std::invalid_argument(std::string("Invalid ECS JSON: ") + error.what()); }
private:
    bool Scalar() const
    {
        if (m_frames.size() > m_maximumDepth) throw std::length_error("ECS JSON nesting limit exceeded");
        return true;
    }
    bool End(bool object)
    {
        if (m_frames.empty() || m_frames.back().object != object) throw std::invalid_argument("Invalid ECS JSON container nesting");
        m_frames.pop_back(); return true;
    }
    struct Frame { bool object; std::set<std::string> keys; };
    size_t m_maximumDepth;
    std::vector<Frame> m_frames;
};
}
Entity ArchiveInstance::Find(std::string_view id) const
{
    const auto found = byId.find(std::string(id)); return found == byId.end() ? Entity{} : found->second;
}
size_t ArchiveInstance::Destroy(World& target)
{
    if (target.GetId() != world) throw std::invalid_argument("ECS archive instance belongs to another World");
    const auto destroyed = target.DestroyEntities(entities);
    entities.clear(); byId.clear(); byAuthoringId.clear(); return destroyed;
}
WorldDocument WorldArchive::Capture(World& world, std::span<const PersistentEntity> entities, DocumentKind kind, ArchiveLimits limits)
{
    world.CheckStructural();
    if (entities.size() > limits.maxEntities) throw std::length_error("ECS capture entity limit exceeded");
    WorldDocument result; result.kind = kind; result.entities.reserve(entities.size());
    std::unordered_map<Entity, std::string, EntityHash> ids;
    std::set<std::string> uniqueIds;
    for (const auto& record : entities)
    {
        StringLimit(record.id);
        if (!world.IsAlive(record.entity) || !ids.emplace(record.entity, record.id).second || !uniqueIds.insert(record.id).second)
            throw std::invalid_argument("ECS capture requires unique IDs and live same-World entities");
    }
    EntityWriteContext context(ids, result.resources);
    auto lease = world.AcquireAccess(ReadAccess(world)); World::AccessScope scope(std::move(lease));
    for (const auto& record : entities)
    {
        ArchivedEntity entity{record.id, record.authoringId, {}};
        for (const auto id : world.GetEntityComponents(record.entity))
        {
            const auto& type = world.GetRegistry().Get(id);
            if (type.transient.load(std::memory_order_acquire)) continue;
            ArchivedComponent component{id, type.name, type.version, type.kind, true, ComponentJson::object()};
            if (type.kind != ComponentKind::Tag)
            {
                const auto codec = world.GetRegistry().GetCodec(id);
                if (!codec) throw std::invalid_argument("ECS component has no explicit codec or transient policy: " + type.name);
                component.value = codec->encode(world.ReadComponent(record.entity, id), context);
            }
            if (type.kind == ComponentKind::Enableable) component.enabled = world.IsEnabled(record.entity, id);
            entity.components.push_back(std::move(component));
        }
        result.entities.push_back(std::move(entity));
    }
    std::sort(result.resources.begin(), result.resources.end(), [](const auto& a, const auto& b) { return a.guid < b.guid; });
    Validate(result, limits); return result;
}
WorldDocument WorldArchive::CaptureAll(World& world, DocumentKind kind, ArchiveLimits limits)
{
    world.CheckStructural();
    if (world.GetEntityCount() > limits.maxEntities) throw std::length_error("ECS capture entity limit exceeded");
    std::vector<PersistentEntity> records;
    for (const auto entity : world.GetEntities()) records.push_back({entity, "e" + std::to_string(records.size() + 1), {}});
    return Capture(world, records, kind, limits);
}
ArchiveInstance WorldArchive::Instantiate(World& world, const WorldDocument& document, const ArchiveLoadOptions& options)
{
    world.CheckStructural(); Validate(document, options.limits);
    struct Prepared { const ComponentTypeInfo* type; std::shared_ptr<const ComponentCodec> codec; std::unique_ptr<ComponentValue> value; };
    std::vector<std::vector<Prepared>> prepared(document.entities.size());
    for (size_t i = 0; i < document.entities.size(); ++i)
        for (const auto& component : document.entities[i].components)
        {
            const auto& type = world.GetRegistry().Get(component.type);
            if (type.name != component.name || type.kind != component.kind || type.transient)
                throw std::invalid_argument("ECS component schema identity mismatch: " + component.name);
            auto codec = world.GetRegistry().GetCodec(type.id);
            if (component.version > type.version || (component.version != type.version && (!codec || !codec->migrate)))
                throw std::invalid_argument("Unsupported ECS component version: " + component.name);
            if (type.kind != ComponentKind::Tag && !codec) throw std::invalid_argument("Missing ECS codec: " + component.name);
            prepared[i].push_back({&type, std::move(codec), {}});
        }
    {
        auto lease = world.AcquireAccess(ReadAccess(world)); World::AccessScope scope(std::move(lease));
        for (const auto& dependency : document.resources)
            if (!options.resolveResource || !options.resolveResource(dependency))
                throw std::invalid_argument("Unresolved ECS resource dependency: " + dependency.guid);
    }
    ArchiveInstance result; result.world = world.GetId(); result.entities.reserve(document.entities.size());
    for (const auto& entity : document.entities)
    {
        result.byId.emplace(entity.id, Entity{});
        if (!entity.authoringId.empty()) result.byAuthoringId.emplace(entity.authoringId, Entity{});
    }
    try
    {
        for (size_t i = 0; i < document.entities.size(); ++i)
        {
            std::vector<ComponentId> signature;
            for (const auto& item : prepared[i]) signature.push_back(item.type->id);
            const auto entity = world.CreateEntity(signature); result.entities.push_back(entity);
            result.byId.at(document.entities[i].id) = entity;
            if (!document.entities[i].authoringId.empty()) result.byAuthoringId.at(document.entities[i].authoringId) = entity;
            world.SetScene(entity, options.sceneOwner);
        }
        {
            EntityReadContext context(result.byId, document.resources);
            auto lease = world.AcquireAccess(ReadAccess(world)); World::AccessScope scope(std::move(lease));
            for (size_t i = 0; i < prepared.size(); ++i)
                for (size_t j = 0; j < prepared[i].size(); ++j)
                {
                    auto& item = prepared[i][j]; if (item.type->kind == ComponentKind::Tag) continue;
                    const auto& component = document.entities[i].components[j];
                    auto value = component.version == item.type->version ? component.value : item.codec->migrate(component.version, component.value);
                    ValidateJson(value, options.limits.maxDepth);
                    item.value = std::make_unique<ComponentValue>(*item.type);
                    try { item.codec->decode(item.value->Data(), value, context); }
                    catch (const std::exception& error)
                    { throw std::invalid_argument("ECS decode " + document.entities[i].id + "/" + component.name + ": " + error.what()); }
                }
        }
        for (size_t i = 0; i < prepared.size(); ++i)
            for (size_t j = 0; j < prepared[i].size(); ++j)
            {
                auto& item = prepared[i][j];
                if (item.value) world.SetMoved(result.entities[i], *item.type, item.value->Data());
                if (item.type->kind == ComponentKind::Enableable && !document.entities[i].components[j].enabled)
                    world.SetEnabled(result.entities[i], item.type->id, false);
            }
    }
    catch (...) { world.DestroyEntities(result.entities); throw; }
    return result;
}
WorldDocument WorldArchive::ApplyOverrides(const WorldDocument& prefab, std::span<const PrefabOverride> overrides)
{
    if (prefab.kind != DocumentKind::Prefab) throw std::invalid_argument("Expected an ECS prefab document");
    auto result = prefab;
    for (const auto& change : overrides)
    {
        const auto entity = std::find_if(result.entities.begin(), result.entities.end(), [&](const auto& e) { return e.id == change.entityId; });
        if (entity == result.entities.end()) throw std::invalid_argument("Prefab override references an unknown entity");
        auto component = std::find_if(entity->components.begin(), entity->components.end(), [&](const auto& c) { return c.type == change.type; });
        if (change.operation == PrefabOverrideKind::AddComponent)
        {
            if (component != entity->components.end() || change.component.type != change.type) throw std::invalid_argument("Invalid prefab AddComponent override");
            entity->components.push_back(change.component); continue;
        }
        if (component == entity->components.end()) throw std::invalid_argument("Prefab override references a missing component");
        switch (change.operation)
        {
        case PrefabOverrideKind::SetField:
            if (change.field.empty()) throw std::invalid_argument("Prefab field override needs a nonempty JSON pointer");
            component->value.at(ComponentJson::json_pointer(change.field)) = change.value; break;
        case PrefabOverrideKind::SetComponent:
            if (change.component.type != change.type) throw std::invalid_argument("Prefab replacement component identity mismatch");
            *component = change.component; break;
        case PrefabOverrideKind::RemoveComponent: entity->components.erase(component); break;
        case PrefabOverrideKind::SetEnabled: component->enabled = change.enabled; break;
        default: throw std::invalid_argument("Unknown prefab override operation");
        }
    }
    return result;
}
ArchiveInstance WorldArchive::InstantiatePrefab(World& world, const WorldDocument& prefab,
    std::span<const PrefabOverride> overrides, const ArchiveLoadOptions& options)
{ return Instantiate(world, ApplyOverrides(prefab, overrides), options); }
std::vector<ArchiveInstance> WorldArchive::InstantiatePrefabs(World& world, const WorldDocument& prefab,
    size_t count, const ArchiveLoadOptions& options)
{
    world.CheckStructural();
    if (prefab.kind != DocumentKind::Prefab) throw std::invalid_argument("Expected an ECS prefab document");
    if (count > options.limits.maxEntities / std::max(size_t(1), prefab.entities.size())) throw std::length_error("ECS prefab batch limit exceeded");
    std::vector<ArchiveInstance> result; result.reserve(count);
    try { for (size_t i = 0; i < count; ++i) result.push_back(Instantiate(world, prefab, options)); }
    catch (...) { for (auto& instance : result) instance.Destroy(world); throw; }
    return result;
}
std::string WorldArchive::Encode(const WorldDocument& document, ArchiveLimits limits)
{
    Validate(document, limits);
    auto data = Payload(document); ValidateJson(data, limits.maxDepth);
    ComponentJson envelope{{"format", "Nullus.ECS"}, {"version", 1},
        {"checksum", Hex(StableComponentId(data.dump()))}, {"checksum_algorithm", "fnv1a64"}, {"data", std::move(data)}};
    ValidateJson(envelope, limits.maxDepth);
    auto text = envelope.dump(2);
    if (text.size() > limits.maxBytes) throw std::length_error("ECS archive byte limit exceeded");
    return text;
}
WorldDocument WorldArchive::Parse(std::string_view text, ArchiveLimits limits)
{
    if (text.size() > limits.maxBytes) throw std::length_error("ECS archive byte limit exceeded");
    ArchiveJsonValidator validator(limits.maxDepth);
    if (!ComponentJson::sax_parse(text.begin(), text.end(), &validator)) throw std::invalid_argument("Invalid ECS JSON archive");
    const auto root = ComponentJson::parse(text.begin(), text.end());
    if (root.at("format") != "Nullus.ECS" || Unsigned(root.at("version"), UINT32_MAX) != 1 || root.at("checksum_algorithm") != "fnv1a64")
        throw std::invalid_argument("Unsupported ECS archive format/version");
    const auto& data = root.at("data"); ValidateJson(data, limits.maxDepth);
    if (root.at("checksum") != Hex(StableComponentId(data.dump()))) throw std::invalid_argument("ECS archive integrity check failed");
    WorldDocument result;
    const auto kind = data.at("kind").get<std::string>();
    if (kind != "scene" && kind != "prefab") throw std::invalid_argument("Invalid ECS document kind");
    result.kind = kind == "scene" ? DocumentKind::Scene : DocumentKind::Prefab;
    const auto& entities = data.at("entities"); const auto& archetypes = data.at("archetypes"); const auto& resources = data.at("resources");
    if (!entities.is_array() || !archetypes.is_array() || !resources.is_array() || entities.size() > limits.maxEntities ||
        archetypes.size() > limits.maxEntities || resources.size() > limits.maxEntities) throw std::invalid_argument("Invalid ECS schema arrays/limits");
    std::vector<std::vector<ComponentId>> layouts; std::set<std::vector<ComponentId>> uniqueLayouts;
    for (const auto& layout : archetypes)
    {
        if (!layout.is_array() || layout.size() > limits.maxComponentsPerEntity) throw std::invalid_argument("Invalid ECS archetype signature");
        std::vector<ComponentId> signature; for (const auto& id : layout) signature.push_back(ReadHex(id));
        if (!std::is_sorted(signature.begin(), signature.end()) || std::adjacent_find(signature.begin(), signature.end()) != signature.end() ||
            !uniqueLayouts.insert(signature).second) throw std::invalid_argument("Noncanonical or duplicate ECS archetype signature");
        layouts.push_back(std::move(signature));
    }
    for (const auto& record : entities)
    {
        ArchivedEntity entity{record.at("id").get<std::string>(), record.at("authoring_id").get<std::string>(), {}};
        const auto& components = record.at("components");
        if (!components.is_array() || components.size() > limits.maxComponentsPerEntity) throw std::invalid_argument("Invalid ECS component array");
        std::vector<ComponentId> signature;
        for (const auto& item : components)
        {
            ArchivedComponent component;
            component.type = ReadHex(item.at("type")); component.name = item.at("name").get<std::string>();
            component.version = static_cast<uint32_t>(Unsigned(item.at("version"), UINT32_MAX));
            component.kind = ReadKind(item.at("kind").get<std::string>()); component.enabled = item.at("enabled").get<bool>();
            component.value = item.at("value"); signature.push_back(component.type); entity.components.push_back(std::move(component));
        }
        std::sort(signature.begin(), signature.end());
        const auto layout = Unsigned(record.at("archetype"), std::numeric_limits<size_t>::max());
        if (layout >= layouts.size() || layouts[static_cast<size_t>(layout)] != signature)
            throw std::invalid_argument("ECS entity layout does not match its archetype");
        result.entities.push_back(std::move(entity));
    }
    for (const auto& item : resources) result.resources.push_back({item.at("guid").get<std::string>(), item.at("hash_or_version").get<std::string>()});
    Validate(result, limits); return result;
}
WorldDocument WorldArchive::ReadFile(const std::filesystem::path& path, ArchiveLimits limits)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open ECS archive");
    const auto length = stream.tellg();
    if (length < 0 || static_cast<uintmax_t>(length) > limits.maxBytes) throw std::length_error("ECS archive file byte limit exceeded");
    std::string text(static_cast<size_t>(length), '\0'); stream.seekg(0);
    if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("Cannot read complete ECS archive");
    return Parse(text, limits);
}
void WorldArchive::WriteFile(const std::filesystem::path& path, const WorldDocument& document, ArchiveLimits limits)
{
    const auto text = Encode(document, limits);
    if (path.filename().empty()) throw std::invalid_argument("ECS archive needs a file path");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    static std::atomic<uint64_t> nextTemporary{0};
    std::filesystem::path temporary;
    bool reserved = false;
    for (size_t attempt = 0; attempt < 100 && !reserved; ++attempt)
    {
        temporary = path; temporary += ".ecs-tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(nextTemporary.fetch_add(1));
        reserved = std::filesystem::create_directory(temporary);
    }
    if (!reserved) throw std::runtime_error("Cannot reserve ECS archive temporary directory");
    struct Cleanup
    {
        std::filesystem::path directory;
        ~Cleanup() { std::error_code error; std::filesystem::remove(directory / "data", error); std::filesystem::remove(directory, error); }
    } cleanup{temporary};
    const auto pending = temporary / "data";
    {
        std::ofstream stream(pending, std::ios::binary | std::ios::trunc); stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.write(text.data(), static_cast<std::streamsize>(text.size())); stream.flush(); stream.close();
    }
#if defined(_WIN32)
    if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot publish ECS archive");
#else
    std::filesystem::rename(pending, path);
#endif
}
}
