#pragma once

#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"

#include <functional>
#include <utility>

namespace NLS::Core::ResourceManagement
{
template <typename T>
class ResourceHandle
{
public:
    using Resolver = std::function<T*(const ResourceId&)>;

    ResourceHandle() = default;

    ResourceHandle(
        ResourceLifetimeRegistry& registry,
        const ResourceLifetimeAcquireRequest& request,
        Resolver resolver)
        : m_registry(&registry)
        , m_resourceId(registry.Acquire(request))
        , m_ownerToken(request.ownerToken)
        , m_resolver(std::move(resolver))
    {
    }

    ResourceHandle(
        ResourceLifetimeRegistry& registry,
        ResourceId resourceId,
        std::string ownerToken,
        Resolver resolver)
        : m_registry(&registry)
        , m_resourceId(std::move(resourceId))
        , m_ownerToken(std::move(ownerToken))
        , m_resolver(std::move(resolver))
    {
    }

    ResourceHandle(const ResourceHandle&) = delete;
    ResourceHandle& operator=(const ResourceHandle&) = delete;

    ResourceHandle(ResourceHandle&& other) noexcept
    {
        MoveFrom(std::move(other));
    }

    ResourceHandle& operator=(ResourceHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            MoveFrom(std::move(other));
        }
        return *this;
    }

    ~ResourceHandle()
    {
        Reset();
    }

    T* Get() const
    {
        if (m_registry == nullptr || !m_registry->IsGenerationCurrent(m_resourceId) || !m_resolver)
            return nullptr;
        return m_resolver(m_resourceId);
    }

    bool IsValid() const
    {
        return Get() != nullptr;
    }

    explicit operator bool() const
    {
        return IsValid();
    }

    const ResourceId& Id() const
    {
        return m_resourceId;
    }

    void Reset()
    {
        if (m_registry != nullptr && !m_ownerToken.empty())
            m_registry->Release(m_resourceId, m_ownerToken);

        m_registry = nullptr;
        m_resourceId = {};
        m_ownerToken.clear();
        m_resolver = {};
    }

private:
    void MoveFrom(ResourceHandle&& other)
    {
        m_registry = other.m_registry;
        m_resourceId = std::move(other.m_resourceId);
        m_ownerToken = std::move(other.m_ownerToken);
        m_resolver = std::move(other.m_resolver);

        other.m_registry = nullptr;
        other.m_resourceId = {};
        other.m_ownerToken.clear();
        other.m_resolver = {};
    }

    ResourceLifetimeRegistry* m_registry = nullptr;
    ResourceId m_resourceId;
    std::string m_ownerToken;
    Resolver m_resolver;
};
}
