#pragma once
#include "ECS/Core/ComponentRegistry.h"
#include "ECS/Core/Entity.h"
#include <Base/Reflection/JsonConfig.h>
#include <functional>
#include <span>

namespace NLS::ECS
{
using ComponentJson = NLS::json;
NLS_ECS_API void ValidateComponentJson(const ComponentJson& value, size_t maximumDepth = 128);

struct ResourceDependency
{
    std::string guid;
    std::string hashOrVersion;
    bool operator==(const ResourceDependency&) const = default;
};

// Callbacks receive mappings, never raw addresses in a source/target world.
class NLS_ECS_API EntityWriteContext final
{
public:
    EntityWriteContext(const std::unordered_map<Entity, std::string, EntityHash>& entities,
        std::vector<ResourceDependency>& dependencies) : m_entities(entities), m_dependencies(dependencies) {}
    std::string EntityId(Entity entity) const;
    std::string Resource(std::string guid, std::string hashOrVersion = {}) const;
private:
    const std::unordered_map<Entity, std::string, EntityHash>& m_entities;
    std::vector<ResourceDependency>& m_dependencies;
};
class NLS_ECS_API EntityReadContext final
{
public:
    EntityReadContext(const std::unordered_map<std::string, Entity>& entities,
        std::span<const ResourceDependency> dependencies) : m_entities(entities), m_dependencies(dependencies) {}
    Entity Resolve(std::string_view localId) const;
    std::string Resource(std::string_view guid) const;
private:
    const std::unordered_map<std::string, Entity>& m_entities;
    std::span<const ResourceDependency> m_dependencies;
};

struct ComponentCodec
{
    std::type_index nativeType{typeid(void)};
    std::function<ComponentJson(const void*, const EntityWriteContext&)> encode;
    // Destination is already default-constructed. Decode fields or assign a
    // value normally; placement-new over a live object is not permitted.
    std::function<void(void*, const ComponentJson&, const EntityReadContext&)> decode;
    // Required for older versions. Future versions are always rejected.
    std::function<ComponentJson(uint32_t sourceVersion, const ComponentJson&)> migrate;
};

template<class T, class Encode, class Decode>
void RegisterComponentCodec(ComponentRegistry& registry, Encode encode, Decode decode,
    std::function<ComponentJson(uint32_t, const ComponentJson&)> migrate = {})
{
    auto codec = std::make_shared<ComponentCodec>();
    codec->nativeType = typeid(T);
    codec->encode = [encode = std::move(encode)](const void* value, const EntityWriteContext& context)
    { return encode(*static_cast<const T*>(value), context); };
    codec->decode = [decode = std::move(decode)](void* value, const ComponentJson& json, const EntityReadContext& context)
    { decode(*static_cast<T*>(value), json, context); };
    codec->migrate = std::move(migrate);
    registry.RegisterCodec(registry.Get<T>().id, std::move(codec));
}
}
