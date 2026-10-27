#include "VfxDataInterface.h"
#include "VfxProgram.h"
#include "VfxTypeRules.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace NLS::VFX
{
ParticleDataSet::ParticleDataSet(const ParticleDataSet& other)
    : m_attributes(other.m_attributes),m_columns(other.m_columns),m_ids(other.m_ids),m_capacity(other.m_capacity),m_allocatedCapacity(other.Count()) {}

ParticleDataSet& ParticleDataSet::operator=(const ParticleDataSet& other)
{
    if (this != &other) { ParticleDataSet copy(other); *this = std::move(copy); }
    return *this;
}

void ParticleDataSet::Initialize(std::vector<Attribute> attributes, uint32_t capacity, uint32_t initialCapacity)
{
    std::unordered_set<std::string> names;
    if (capacity > 10000000 || initialCapacity > capacity || attributes.size() > 1024) throw std::invalid_argument("Particle data set exceeds its limits");
    for (const auto& attribute : attributes)
        if (attribute.name.empty() || !IsValueType(attribute.type) || !names.insert(attribute.name).second || !ValidValue(attribute.defaultValue,attribute.type))
            throw std::invalid_argument("Invalid particle attribute schema");
    Clear();
    m_attributes = std::move(attributes);
    m_capacity = capacity;
    m_columns.clear();
    m_columns.resize(m_attributes.size());
    // Reserve a small working set; large configured limits must not allocate upfront.
    const auto initial = initialCapacity ? initialCapacity : (std::min)(capacity,256u);
    m_allocatedCapacity = initial;
    m_ids.reserve(initial);
    for (size_t index = 0; index < m_columns.size(); ++index)
        for (uint32_t component = 0; component < ComponentCount(m_attributes[index].type); ++component)
            m_columns[index][component].reserve(initial);
}

bool ParticleDataSet::Reserve(uint32_t capacity)
{
    if (capacity > m_capacity) return false;
    if (capacity <= m_allocatedCapacity) return true;
    try
    {
        auto ids = m_ids; auto columns = m_columns;
        ids.reserve(capacity);
        for (size_t attribute = 0; attribute < columns.size(); ++attribute)
            for (uint32_t component = 0; component < ComponentCount(m_attributes[attribute].type); ++component)
                columns[attribute][component].reserve(capacity);
        m_ids.swap(ids); m_columns.swap(columns); m_allocatedCapacity = capacity; return true;
    }
    catch (const std::bad_alloc&) { return false; }
}

int32_t ParticleDataSet::FindAttribute(std::string_view name) const
{
    for (size_t index = 0; index < m_attributes.size(); ++index)
        if (m_attributes[index].name == name) return static_cast<int32_t>(index);
    return -1;
}

Value ParticleDataSet::Read(uint32_t attribute, uint32_t particle) const
{
    Value value {};
    const auto& column = m_columns.at(attribute);
    for (uint32_t component = 0; component < ComponentCount(m_attributes[attribute].type); ++component)
        value[component] = column[component].at(particle);
    return value;
}

void ParticleDataSet::Write(uint32_t attribute, uint32_t particle, const Value& value)
{
    auto& column = m_columns.at(attribute);
    for (uint32_t component = 0; component < ComponentCount(m_attributes[attribute].type); ++component)
        column[component].at(particle) = value[component];
}

bool ParticleDataSet::Spawn(uint64_t id)
{
    if (Count() >= m_capacity) return false;
    if (Count() >= m_allocatedCapacity && !Reserve((std::min)(m_capacity,(std::max)(Count()+1,m_allocatedCapacity*2)))) return false;
    m_ids.push_back(id);
    for (size_t index = 0; index < m_attributes.size(); ++index)
        for (uint32_t component = 0; component < ComponentCount(m_attributes[index].type); ++component)
            m_columns[index][component].push_back(m_attributes[index].defaultValue[component]);
    return true;
}

void ParticleDataSet::Remove(uint32_t particle)
{
    if (particle >= Count()) throw std::out_of_range("Particle index");
    m_ids.erase(m_ids.begin() + particle);
    for (size_t index = 0; index < m_attributes.size(); ++index)
        for (uint32_t component = 0; component < ComponentCount(m_attributes[index].type); ++component)
            m_columns[index][component].erase(m_columns[index][component].begin() + particle);
}

void ParticleDataSet::Clear()
{
    m_ids.clear();
    for (auto& column : m_columns) for (auto& component : column) component.clear();
}

void ParticleDataSet::Compact(std::span<const uint8_t> alive)
{
    if (alive.size() != Count()) throw std::invalid_argument("Particle compaction mask size");
    uint32_t destination = 0;
    for (uint32_t source = 0; source < Count(); ++source)
    {
        if (!alive[source]) continue;
        if (source != destination)
        {
            m_ids[destination] = m_ids[source];
            for (size_t attribute = 0; attribute < m_attributes.size(); ++attribute)
                for (uint32_t component = 0; component < ComponentCount(m_attributes[attribute].type); ++component)
                    m_columns[attribute][component][destination] = m_columns[attribute][component][source];
        }
        ++destination;
    }
    m_ids.resize(destination);
    for (size_t attribute = 0; attribute < m_attributes.size(); ++attribute)
        for (uint32_t component = 0; component < ComponentCount(m_attributes[attribute].type); ++component)
            m_columns[attribute][component].resize(destination);
}

uint32_t RandomHash(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float RandomFloat(uint64_t particle, uint32_t seed, uint32_t stream, uint64_t tick, uint32_t component)
{
    const auto hash = RandomHash(static_cast<uint32_t>(particle) ^ RandomHash(static_cast<uint32_t>(particle >> 32)) ^
        RandomHash(seed) ^ RandomHash(stream) ^ RandomHash(static_cast<uint32_t>(tick)) ^
        RandomHash(static_cast<uint32_t>(tick >> 32)) ^ RandomHash(component + 1));
    return static_cast<float>(hash >> 8) * (1.0f / 16777216.0f);
}

bool EmitterScript::Initialize(std::vector<Attribute> attributes, const Program& update, const Program& spawn)
{
    std::string error;
    if (!Typed::ValidateBytecodeProgram(update, m_types, error) || !Typed::ValidateBytecodeProgram(spawn, m_types, error))
        return false;
    ParticleDataSet data;
    try
    {
        data.Initialize(attributes, 1, 1);
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    std::vector<int32_t> bindings, spawnBindings;
    if (!Typed::BindBytecodeAttributes(update, data, bindings, error) || !Typed::BindBytecodeAttributes(spawn, data, spawnBindings, error) || !data.Spawn(1))
        return false;
    if (!Typed::ExecuteParticleProgram(spawn, data, 0, 1, {}, error, spawnBindings))
        return false;
    std::set<std::string> writes;
    for (const auto* program : {&update, &spawn})
        for (const auto& slot : program->attributes)
            if (slot.write)
                writes.insert(slot.name);
    m_data = std::move(data);
    m_update = update;
    m_spawn = spawn;
    m_bindings = std::move(bindings);
    m_spawnBindings = std::move(spawnBindings);
    m_attributes = std::move(attributes);
    m_writes = std::move(writes);
    return true;
}
bool EmitterScript::Reset(const ExecutionContext& context)
{
    try
    {
        m_data.Initialize(m_attributes, 1, 1);
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    std::string error;
    return m_data.Spawn(1) && Typed::ExecuteParticleProgram(m_spawn, m_data, 0, 1, context, error, m_spawnBindings);
}
bool EmitterScript::HasWrite(std::string_view name) const
{
    return m_writes.contains(std::string(name));
}
bool EmitterScript::Tick(const ExecutionContext& context)
{
    if (m_update.instructions.empty())
        return true;
    std::string error;
    return Typed::ExecuteParticleProgram(m_update, m_data, 0, 1, context, error, m_bindings);
}
float EmitterScript::Read(std::string_view name,float fallback) const
{
    const auto index = m_data.FindAttribute(name);
    return index >= 0 && m_data.Count() ? m_data.Read(static_cast<uint32_t>(index),0)[0] : fallback;
}
Value EmitterScript::ReadValue(std::string_view name,const Value& fallback) const
{
    const auto index = m_data.FindAttribute(name);
    return index >= 0 && m_data.Count() ? m_data.Read(static_cast<uint32_t>(index),0) : fallback;
}
}
