#pragma once

#include "BaseDef.h"

#include <cstdint>
#include <functional>

namespace NLS::meta
{
    struct TypeKey
    {
        std::uint64_t high = 0;
        std::uint64_t low = 0;

        constexpr bool operator==(const TypeKey& rhs) const
        {
            return high == rhs.high && low == rhs.low;
        }

        constexpr bool operator!=(const TypeKey& rhs) const
        {
            return !(*this == rhs);
        }
    };

    constexpr TypeKey InvalidTypeKey {};

    constexpr TypeKey HashTypeKey(const char* value)
    {
        std::uint64_t high = 14695981039346656037ull;
        std::uint64_t low = 1099511628211ull;
        while (value != nullptr && *value != '\0')
        {
            const auto c = static_cast<unsigned char>(*value);
            high ^= c;
            high *= 1099511628211ull;

            low ^= static_cast<unsigned char>(c + 0x9d);
            low *= 14029467366897019727ull;
            low ^= high >> 29;
            ++value;
        }

        return { high, low };
    }

    NLS_BASE_API TypeKey MakeTypeKey(const char* stableName);
}

namespace std
{
    template<>
    struct hash<NLS::meta::TypeKey>
    {
        size_t operator()(const NLS::meta::TypeKey& key) const noexcept
        {
            return static_cast<size_t>(key.high ^ (key.low + 0x9e3779b97f4a7c15ull + (key.high << 6) + (key.high >> 2)));
        }
    };
}
