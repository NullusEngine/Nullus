#include "ComponentRegistry.h"
#include "ECS/Serialization/ComponentCodec.h"
#include <algorithm>

namespace NLS::ECS
{
const ComponentTypeInfo& ComponentRegistry::Insert(std::unique_ptr<ComponentTypeInfo> info)
{
    std::lock_guard lock(m_mutex);
    if (const auto existing = m_types.find(info->id); existing != m_types.end())
    {
        const auto& old = *existing->second;
        if (old.name != info->name || old.nativeType != info->nativeType || old.size != info->size ||
            old.alignment != info->alignment || old.kind != info->kind || old.version != info->version ||
            old.reflectionTypeName != info->reflectionTypeName || old.editorDescription != info->editorDescription)
            throw std::invalid_argument("Conflicting ECS component registration: " + info->name);
        return old;
    }
    if (m_nativeTypes.contains(info->nativeType))
        throw std::invalid_argument("ECS C++ type already registered under another stable ID");
    if (m_names.contains(info->name))
        throw std::invalid_argument("ECS stable component name already registered under another ID");
    const auto id = info->id;
    const auto nativeType = info->nativeType;
    const auto name = info->name;
    auto [iterator, inserted] = m_types.emplace(id, std::move(info));
    try { m_nativeTypes.emplace(nativeType, id); m_names.emplace(name, id); }
    catch (...) { m_nativeTypes.erase(nativeType); m_types.erase(iterator); throw; }
    return *iterator->second;
}
const ComponentTypeInfo& ComponentRegistry::Get(std::type_index type) const
{
    std::lock_guard lock(m_mutex);
    const auto iterator = m_nativeTypes.find(type);
    if (iterator == m_nativeTypes.end()) throw std::invalid_argument("Unregistered ECS C++ component type");
    return *m_types.at(iterator->second);
}
const ComponentTypeInfo& ComponentRegistry::Get(ComponentId id) const
{
    const auto* info = Find(id);
    if (!info) throw std::invalid_argument("Unknown ECS component ID");
    return *info;
}
const ComponentTypeInfo* ComponentRegistry::Find(ComponentId id) const
{
    std::lock_guard lock(m_mutex);
    const auto iterator = m_types.find(id);
    return iterator == m_types.end() ? nullptr : iterator->second.get();
}
std::vector<const ComponentTypeInfo*> ComponentRegistry::GetTypes() const
{
    std::lock_guard lock(m_mutex);
    std::vector<const ComponentTypeInfo*> result;
    result.reserve(m_types.size());
    for (const auto& [id, info] : m_types) result.push_back(info.get());
    std::sort(result.begin(), result.end(), [](auto* a, auto* b) { return a->id < b->id; });
    return result;
}
void ComponentRegistry::RegisterCodec(ComponentId id, std::shared_ptr<const ComponentCodec> codec)
{
    if (!codec || !codec->encode || !codec->decode) throw std::invalid_argument("ECS codec needs encode and decode callbacks");
    std::lock_guard lock(m_mutex);
    const auto found = m_types.find(id);
    if (found == m_types.end()) throw std::invalid_argument("Cannot register a codec for an unknown ECS type");
    auto& type = *found->second;
    if (type.nativeType != codec->nativeType || type.kind == ComponentKind::Tag || type.transient)
        throw std::invalid_argument("ECS codec native type or persistence policy mismatch: " + type.name);
    if (m_codecs.contains(id)) throw std::invalid_argument("ECS codec is already registered: " + type.name);
    m_codecs.emplace(id, std::move(codec)); type.serializable.store(true, std::memory_order_release);
}
std::shared_ptr<const ComponentCodec> ComponentRegistry::GetCodec(ComponentId id) const
{
    std::lock_guard lock(m_mutex);
    const auto found = m_codecs.find(id);
    return found == m_codecs.end() ? nullptr : found->second;
}
void ComponentRegistry::RegisterTransient(ComponentId id)
{
    std::lock_guard lock(m_mutex);
    const auto found = m_types.find(id);
    if (found == m_types.end()) throw std::invalid_argument("Unknown transient ECS type");
    auto& type = *found->second;
    if (type.serializable) throw std::invalid_argument("Serializable ECS type cannot also be transient: " + type.name);
    type.transient.store(true, std::memory_order_release);
}
ComponentValue::ComponentValue(const ComponentTypeInfo& type) : m_type(&type)
{
    m_data = ::operator new(std::max(type.size, size_t(1)), std::align_val_t(type.alignment));
    try { type.construct(m_data); }
    catch (...) { ::operator delete(m_data, std::align_val_t(type.alignment)); m_data = nullptr; throw; }
}
ComponentValue::ComponentValue(const ComponentTypeInfo& type, const void* copyFrom) : m_type(&type)
{
    if (!copyFrom || !type.copy) throw std::invalid_argument("ECS value is not copyable");
    m_data = ::operator new(std::max(type.size, size_t(1)), std::align_val_t(type.alignment));
    try { type.copy(m_data, copyFrom); }
    catch (...) { ::operator delete(m_data, std::align_val_t(type.alignment)); m_data = nullptr; throw; }
}
ComponentValue::ComponentValue(const ComponentTypeInfo& type, void* moveFrom, MoveValue) : m_type(&type)
{
    if (!moveFrom) throw std::invalid_argument("Cannot move a null ECS value");
    m_data = ::operator new(std::max(type.size, size_t(1)), std::align_val_t(type.alignment));
    type.move(m_data, moveFrom);
}
ComponentValue::ComponentValue(ComponentValue&& other) noexcept
    : m_type(std::exchange(other.m_type, nullptr)), m_data(std::exchange(other.m_data, nullptr)) {}
ComponentValue& ComponentValue::operator=(ComponentValue&& other) noexcept
{
    if (this != &other)
    { Release(); m_type = std::exchange(other.m_type, nullptr); m_data = std::exchange(other.m_data, nullptr); }
    return *this;
}
ComponentValue::~ComponentValue() { Release(); }
void ComponentValue::Release() noexcept
{
    if (m_data) { m_type->destroy(m_data); ::operator delete(m_data, std::align_val_t(m_type->alignment)); }
    m_data = nullptr; m_type = nullptr;
}
}
