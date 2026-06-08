#pragma once

#include "Core/ServiceLocator.h"

namespace NLS::Tests
{
template<typename T>
class ScopedServiceOverride
{
public:
    explicit ScopedServiceOverride(T& service)
        : m_hadPrevious(NLS::Core::ServiceLocator::Contains<T>())
        , m_previous(m_hadPrevious ? &NLS::Core::ServiceLocator::Get<T>() : nullptr)
    {
        NLS::Core::ServiceLocator::Provide<T>(service);
    }

    ~ScopedServiceOverride()
    {
        if (m_hadPrevious && m_previous)
            NLS::Core::ServiceLocator::Provide<T>(*m_previous);
        else
            NLS::Core::ServiceLocator::Remove<T>();
    }

    ScopedServiceOverride(const ScopedServiceOverride&) = delete;
    ScopedServiceOverride& operator=(const ScopedServiceOverride&) = delete;

private:
    bool m_hadPrevious = false;
    T* m_previous = nullptr;
};
}
