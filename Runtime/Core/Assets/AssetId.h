#pragma once

#include "CoreDef.h"
#include "Guid.h"

namespace NLS::Core::Assets
{
class NLS_CORE_API AssetId
{
public:
    AssetId() = default;
    explicit AssetId(NLS::Guid guid)
        : m_guid(guid)
    {
    }

    static AssetId New()
    {
        return AssetId(NLS::Guid::New());
    }

    bool IsValid() const
    {
        return m_guid.IsValid();
    }

    const NLS::Guid& GetGuid() const
    {
        return m_guid;
    }

    std::string ToString() const
    {
        return m_guid.ToString();
    }

    friend bool operator==(const AssetId& lhs, const AssetId& rhs) = default;
    friend bool operator!=(const AssetId& lhs, const AssetId& rhs) = default;
    friend bool operator<(const AssetId& lhs, const AssetId& rhs)
    {
        return lhs.m_guid < rhs.m_guid;
    }

private:
    NLS::Guid m_guid;
};
}

namespace std
{
template<>
struct hash<NLS::Core::Assets::AssetId>
{
    size_t operator()(const NLS::Core::Assets::AssetId& id) const noexcept
    {
        return std::hash<NLS::Guid>{}(id.GetGuid());
    }
};
}
