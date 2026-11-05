#pragma once
#include "ComponentRegistry.h"
#include "Entity.h"
#include <span>

namespace NLS::ECS::Detail
{
struct Column
{
    const ComponentTypeInfo* type = nullptr;
    size_t offset = 0;
    uint64_t changeVersion = 0;
    std::vector<uint8_t> enabled;
    std::unique_ptr<ComponentValue> shared;
};

struct Chunk
{
    explicit Chunk(std::span<const ComponentTypeInfo* const> types, size_t budget);
    ~Chunk();
    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    size_t count = 0, capacity = 0, bytes = 0, alignment = alignof(Entity);
    void* memory = nullptr;
    std::vector<Column> columns;
    Entity* Entities() { return static_cast<Entity*>(memory); }
    const Entity* Entities() const { return static_cast<const Entity*>(memory); }
    Column* Find(ComponentId id);
    const Column* Find(ComponentId id) const;
    void* At(Column& column, size_t row);
    const void* At(const Column& column, size_t row) const;
};

struct Archetype
{
    size_t id = 0;
    std::vector<ComponentId> signature;
    std::vector<const ComponentTypeInfo*> types;
    std::vector<std::unique_ptr<Chunk>> chunks;
    Chunk* allocationHint = nullptr;
    size_t entityCount = 0;
    [[nodiscard]] bool Has(ComponentId id) const;
};

struct Location { Archetype* archetype = nullptr; Chunk* chunk = nullptr; size_t row = 0; };
struct EntitySlot
{
    uint32_t generation = 1;
    bool alive = false;
    uint32_t nextFree = ~0u;
    Location location;
};
}
