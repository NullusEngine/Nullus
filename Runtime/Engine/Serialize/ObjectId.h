#pragma once

#include "Guid.h"

namespace NLS::Engine::Serialize
{
    class ObjectId
    {
    public:
        ObjectId() = default;
        explicit ObjectId(NLS::Guid guid)
            : m_guid(guid)
        {
        }

        bool IsValid() const
        {
            return m_guid.IsValid();
        }

        const NLS::Guid& GetGuid() const
        {
            return m_guid;
        }

        friend bool operator==(const ObjectId& lhs, const ObjectId& rhs) = default;
        friend bool operator!=(const ObjectId& lhs, const ObjectId& rhs) = default;
        friend bool operator<(const ObjectId& lhs, const ObjectId& rhs)
        {
            return lhs.m_guid.GetBytes() < rhs.m_guid.GetBytes();
        }

    private:
        NLS::Guid m_guid;
    };

    class AssetId
    {
    public:
        AssetId() = default;
        explicit AssetId(NLS::Guid guid)
            : m_guid(guid)
        {
        }

        bool IsValid() const
        {
            return m_guid.IsValid();
        }

        const NLS::Guid& GetGuid() const
        {
            return m_guid;
        }

        friend bool operator==(const AssetId& lhs, const AssetId& rhs) = default;
        friend bool operator!=(const AssetId& lhs, const AssetId& rhs) = default;
        friend bool operator<(const AssetId& lhs, const AssetId& rhs)
        {
            return lhs.m_guid.GetBytes() < rhs.m_guid.GetBytes();
        }

    private:
        NLS::Guid m_guid;
    };
}

namespace std
{
    template<>
    struct hash<NLS::Engine::Serialize::ObjectId>
    {
        size_t operator()(const NLS::Engine::Serialize::ObjectId& id) const noexcept
        {
            return hash<NLS::Guid> {}(id.GetGuid());
        }
    };

    template<>
    struct hash<NLS::Engine::Serialize::AssetId>
    {
        size_t operator()(const NLS::Engine::Serialize::AssetId& id) const noexcept
        {
            return hash<NLS::Guid> {}(id.GetGuid());
        }
    };
}
