#pragma once
#include <cstdint>
#include <functional>
#include <limits>

namespace NLS::ECS
{
struct Entity
{
    uint64_t world = 0;
    uint32_t index = std::numeric_limits<uint32_t>::max();
    uint32_t generation = 0;
    [[nodiscard]] constexpr bool IsValid() const noexcept
    { return world != 0 && generation != 0 && index != std::numeric_limits<uint32_t>::max(); }
    bool operator==(const Entity&) const = default;
};

struct EntityHash
{
    size_t operator()(Entity entity) const noexcept
    {
        const auto local = (uint64_t(entity.index) << 32) | entity.generation;
        return std::hash<uint64_t>{}(entity.world) ^ (std::hash<uint64_t>{}(local) << 1);
    }
};

// A change cursor is scoped to one World. Counters saturate with a diagnostic.
struct ChangeToken
{
    uint64_t world = 0;
    uint64_t version = 0;
    bool operator==(const ChangeToken&) const = default;
};
}
