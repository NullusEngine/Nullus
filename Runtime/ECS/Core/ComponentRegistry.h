#pragma once
#include "ECS/ECSDef.h"
#include <concepts>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace NLS::ECS
{
struct ComponentCodec;
using ComponentId = uint64_t;
enum class ComponentKind : uint8_t { Data, Tag, Buffer, Shared, Enableable };

constexpr ComponentId StableComponentId(std::string_view name) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    for (const auto character : name)
    { hash ^= static_cast<unsigned char>(character); hash *= 1099511628211ull; }
    return hash;
}

struct ComponentRegistration
{
    std::string name;
    ComponentKind kind = ComponentKind::Data;
    uint32_t version = 1;
    // Normally zero: derive from the name. An explicit ID supports schema tools.
    ComponentId stableId = 0;
    std::string reflectionTypeName;
    std::string editorDescription;
};

struct ComponentTypeInfo
{
    ComponentId id = 0;
    std::string name;
    ComponentKind kind = ComponentKind::Data;
    size_t size = 0;
    size_t alignment = 1;
    uint32_t version = 1;
    bool triviallyRelocatable = false;
    std::atomic<bool> serializable{false};
    std::atomic<bool> transient{false}; // Explicitly excluded derived/runtime data.
    std::string reflectionTypeName;
    std::string editorDescription;
    void (*construct)(void*) = nullptr;
    void (*move)(void*, void*) noexcept = nullptr;
    void (*copy)(void*, const void*) = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    bool (*equals)(const void*, const void*) = nullptr;
    std::type_index nativeType{typeid(void)}; // Process lookup only; never serialized.
    [[nodiscard]] bool HasColumn() const noexcept
    { return kind != ComponentKind::Tag && kind != ComponentKind::Shared; }
};

// Explicit registration avoids compiler-generated type names and order-based IDs.
// Metadata addresses remain stable for the entire registry lifetime.
class NLS_ECS_API ComponentRegistry final
{
public:
    template<class T> const ComponentTypeInfo& Register(ComponentRegistration registration)
    {
        static_assert(std::is_default_constructible_v<T>, "ECS values must be default constructible");
        static_assert(std::is_nothrow_move_constructible_v<T>, "ECS values require noexcept move construction; use an owning indirect value otherwise");
        static_assert(std::is_nothrow_destructible_v<T>, "ECS destructors must be noexcept");
        if (registration.name.empty() || registration.version == 0)
            throw std::invalid_argument("ECS component needs a stable name and nonzero version");
        if (registration.kind == ComponentKind::Tag && !std::is_empty_v<T>)
            throw std::invalid_argument("ECS tag components must be empty types");
        auto info = std::make_unique<ComponentTypeInfo>();
        info->id = registration.stableId ? registration.stableId : StableComponentId(registration.name);
        if (!info->id) throw std::invalid_argument("ECS component ID zero is reserved");
        info->name = std::move(registration.name);
        info->kind = registration.kind;
        info->serializable = registration.kind == ComponentKind::Tag;
        info->version = registration.version;
        info->reflectionTypeName = std::move(registration.reflectionTypeName);
        info->editorDescription = std::move(registration.editorDescription);
        info->size = info->kind == ComponentKind::Tag ? 0 : sizeof(T);
        info->alignment = alignof(T);
        info->nativeType = typeid(T);
        info->triviallyRelocatable = std::is_trivially_copyable_v<T>;
        info->construct = [](void* destination) { std::construct_at(static_cast<T*>(destination)); };
        info->move = [](void* destination, void* source) noexcept
        { std::construct_at(static_cast<T*>(destination), std::move(*static_cast<T*>(source))); };
        info->destroy = [](void* value) noexcept { std::destroy_at(static_cast<T*>(value)); };
        if constexpr (std::is_copy_constructible_v<T>)
            info->copy = [](void* destination, const void* source)
            { std::construct_at(static_cast<T*>(destination), *static_cast<const T*>(source)); };
        if constexpr (requires(const T& a, const T& b) { { a == b } -> std::convertible_to<bool>; })
            info->equals = [](const void* a, const void* b)
            { return *static_cast<const T*>(a) == *static_cast<const T*>(b); };
        if (info->kind == ComponentKind::Shared && (!info->copy || !info->equals))
            throw std::invalid_argument("ECS shared components require copying and value equality");
        return Insert(std::move(info));
    }

    template<class T> const ComponentTypeInfo& Register(std::string name,
        ComponentKind kind = ComponentKind::Data, uint32_t version = 1)
    { return Register<T>(ComponentRegistration{std::move(name), kind, version}); }

    template<class T> const ComponentTypeInfo& Get() const { return Get(std::type_index(typeid(T))); }
    const ComponentTypeInfo& Get(ComponentId id) const;
    const ComponentTypeInfo* Find(ComponentId id) const;
    std::vector<const ComponentTypeInfo*> GetTypes() const;
    // Register schemas before running worlds. A schema cannot be replaced while
    // components or jobs could retain callbacks from its defining module.
    void RegisterCodec(ComponentId id, std::shared_ptr<const ComponentCodec> codec);
    std::shared_ptr<const ComponentCodec> GetCodec(ComponentId id) const;
    void RegisterTransient(ComponentId id);

private:
    const ComponentTypeInfo& Insert(std::unique_ptr<ComponentTypeInfo> info);
    const ComponentTypeInfo& Get(std::type_index type) const;
    mutable std::mutex m_mutex;
    std::unordered_map<ComponentId, std::unique_ptr<ComponentTypeInfo>> m_types;
    std::unordered_map<std::type_index, ComponentId> m_nativeTypes;
    std::unordered_map<std::string, ComponentId> m_names;
    std::unordered_map<ComponentId, std::shared_ptr<const ComponentCodec>> m_codecs;
};

template<class T> struct DynamicBuffer
{
    std::vector<T> values;
    bool operator==(const DynamicBuffer&) const = default;
};

// Owning type-erased value, also used by commands and shared chunk keys.
class NLS_ECS_API ComponentValue final
{
public:
    struct MoveValue {};
    explicit ComponentValue(const ComponentTypeInfo& type);
    ComponentValue(const ComponentTypeInfo& type, const void* copyFrom);
    ComponentValue(const ComponentTypeInfo& type, void* moveFrom, MoveValue);
    ComponentValue(ComponentValue&& other) noexcept;
    ComponentValue& operator=(ComponentValue&& other) noexcept;
    ComponentValue(const ComponentValue&) = delete;
    ComponentValue& operator=(const ComponentValue&) = delete;
    ~ComponentValue();
    const ComponentTypeInfo& Type() const { return *m_type; }
    void* Data() { return m_data; }
    const void* Data() const { return m_data; }
private:
    void Release() noexcept;
    const ComponentTypeInfo* m_type = nullptr;
    void* m_data = nullptr;
};
}
