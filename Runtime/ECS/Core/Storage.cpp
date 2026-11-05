#include "Storage.h"
#include <algorithm>
#include <limits>

namespace NLS::ECS::Detail
{
namespace
{
bool Append(size_t& offset, size_t alignment, size_t elementSize, size_t count, size_t budget)
{
    if (!alignment || offset > budget || alignment - 1 > budget - offset) return false;
    offset = (offset + alignment - 1) & ~(alignment - 1);
    if (offset > budget || (elementSize && count > (budget - offset) / elementSize)) return false;
    offset += elementSize * count;
    return true;
}
}
Chunk::Chunk(std::span<const ComponentTypeInfo* const> types, size_t budget)
{
    if (budget < sizeof(Entity) || budget > size_t(1) << 30)
        throw std::invalid_argument("ECS chunk budget must be between one Entity and 1 GiB");
    for (const auto* type : types) if (type->HasColumn()) alignment = std::max(alignment, type->alignment);
    const auto fits = [&](size_t n)
    {
        size_t offset = 0;
        if (!Append(offset, alignof(Entity), sizeof(Entity), n, budget)) return false;
        for (const auto* type : types)
            if (type->HasColumn() && !Append(offset, type->alignment, type->size, n, budget)) return false;
        return true;
    };
    size_t low = 0, high = budget / sizeof(Entity);
    while (low < high)
    {
        const auto middle = low + (high - low + 1) / 2;
        if (fits(middle)) low = middle; else high = middle - 1;
    }
    if (!low) throw std::length_error("ECS component layout exceeds the configured chunk budget");
    capacity = low;
    bytes = budget;
    size_t offset = capacity * sizeof(Entity);
    columns.reserve(types.size());
    for (const auto* type : types)
    {
        Column column;
        column.type = type;
        if (type->HasColumn())
        {
            offset = (offset + type->alignment - 1) & ~(type->alignment - 1);
            column.offset = offset;
            offset += type->size * capacity;
        }
        if (type->kind == ComponentKind::Enableable) column.enabled.resize(capacity, 1);
        columns.push_back(std::move(column));
    }
    memory = ::operator new(bytes, std::align_val_t(alignment));
}
Chunk::~Chunk()
{
    for (auto& column : columns)
        if (column.type->HasColumn())
            for (size_t row = 0; row < count; ++row) column.type->destroy(At(column, row));
    ::operator delete(memory, std::align_val_t(alignment));
}
Column* Chunk::Find(ComponentId id)
{
    const auto iterator = std::lower_bound(columns.begin(), columns.end(), id,
        [](const Column& column, ComponentId value) { return column.type->id < value; });
    return iterator != columns.end() && iterator->type->id == id ? &*iterator : nullptr;
}
const Column* Chunk::Find(ComponentId id) const { return const_cast<Chunk*>(this)->Find(id); }
void* Chunk::At(Column& column, size_t row)
{
    if (column.type->kind == ComponentKind::Shared) return column.shared ? column.shared->Data() : nullptr;
    if (!column.type->HasColumn()) return nullptr;
    return static_cast<std::byte*>(memory) + column.offset + row * column.type->size;
}
const void* Chunk::At(const Column& column, size_t row) const
{ return const_cast<Chunk*>(this)->At(const_cast<Column&>(column), row); }
bool Archetype::Has(ComponentId typeId) const { return std::binary_search(signature.begin(), signature.end(), typeId); }
}
