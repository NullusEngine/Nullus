#pragma once

#include "VfxGraph.h"

namespace NLS::VFX
{
class NLS_VFX_API ParticleDataSet
{
public:
    ParticleDataSet() = default;
    ParticleDataSet(const ParticleDataSet& other);
    ParticleDataSet& operator=(const ParticleDataSet& other);
    ParticleDataSet(ParticleDataSet&&) noexcept = default;
    ParticleDataSet& operator=(ParticleDataSet&&) noexcept = default;
    void Initialize(std::vector<Attribute> attributes, uint32_t capacity, uint32_t initialCapacity = 0);
    bool Reserve(uint32_t capacity);
    [[nodiscard]] uint32_t Count() const { return static_cast<uint32_t>(m_ids.size()); }
    [[nodiscard]] uint32_t Capacity() const { return m_capacity; }
    [[nodiscard]] uint32_t AllocatedCapacity() const { return m_allocatedCapacity; }
    [[nodiscard]] const std::vector<Attribute>& Attributes() const { return m_attributes; }
    [[nodiscard]] int32_t FindAttribute(std::string_view name) const;
    [[nodiscard]] uint64_t Id(uint32_t particle) const { return m_ids.at(particle); }
    [[nodiscard]] Value Read(uint32_t attribute, uint32_t particle) const;
    void Write(uint32_t attribute, uint32_t particle, const Value& value);
    [[nodiscard]] bool Spawn(uint64_t id);
    void Remove(uint32_t particle);
    void Compact(std::span<const uint8_t> alive);
    void Clear();

private:
    std::vector<Attribute> m_attributes;
    std::vector<std::array<std::vector<float>, 4>> m_columns;
    std::vector<uint64_t> m_ids;
    uint32_t m_capacity = 0;
    uint32_t m_allocatedCapacity = 0;
};

struct ExecutionContext
{
    float deltaTime = 0;
    uint32_t seed = 0;
    uint64_t tick = 0;
    const std::unordered_map<std::string, Value>* parameters = nullptr;
    uint32_t stageIndex = 0, stageIteration = 0;
    // Birth ordinal of the first particle in this bytecode range. The
    // spawn path sets it per particle so SpawnIndex is exact; the update path
    // leaves it at zero because a particle's birth ordinal is not meaningful
    // there.
    uint32_t spawnIndex = 0;
};

NLS_VFX_API uint32_t RandomHash(uint32_t value);
NLS_VFX_API float RandomFloat(uint64_t particle, uint32_t seed, uint32_t stream, uint64_t tick, uint32_t component);
} // namespace NLS::VFX
