#pragma once

#include "BaseDef.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "JsonFwd.h"

namespace NLS
{
    class NLS_BASE_API Guid
    {
    public:
        using Bytes = std::array<uint8_t, 16>;

        Guid() = default;
        explicit Guid(const Bytes& bytes);

        static Guid New();
        static Guid NewDeterministic(std::string_view label);
        static Guid Empty();
        static std::optional<Guid> TryParse(std::string_view text);
        static Guid Parse(std::string_view text);

        bool IsValid() const;
        std::string ToString() const;

        const Bytes& GetBytes() const;

        friend bool operator==(const Guid& lhs, const Guid& rhs) = default;
        friend bool operator!=(const Guid& lhs, const Guid& rhs) = default;
        friend bool operator<(const Guid& lhs, const Guid& rhs);

    private:
        Bytes m_bytes {};
    };

    NLS_BASE_API void to_json(nlohmann::json& json, const Guid& guid);
    NLS_BASE_API void from_json(const nlohmann::json& json, Guid& guid);
}

namespace std
{
    template<>
    struct hash<NLS::Guid>
    {
        size_t operator()(const NLS::Guid& guid) const noexcept
        {
            size_t hash = 1469598103934665603ull;
            for (const auto byte : guid.GetBytes())
            {
                hash ^= static_cast<size_t>(byte);
                hash *= 1099511628211ull;
            }
            return hash;
        }
    };
}
