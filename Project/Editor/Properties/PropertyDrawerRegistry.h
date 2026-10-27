#pragma once
#include "Inspector/InspectorRegistry.h"

namespace NLS::Editor::Properties
{
using PropertyDrawerContext = Inspector::InspectorPropertyDrawerContext;
using PropertyDrawerRegistration = Inspector::PropertyDrawerRegistration;
using DrawerScope = Inspector::InspectorDrawerScope;

class IPropertyDrawer
{
public:
    virtual ~IPropertyDrawer() = default;
    virtual UI::Controls::ControlResult Draw(PropertyDrawerContext& context) = 0;
};

class DrawerRegistrationToken
{
public:
    DrawerRegistrationToken() = default;
    explicit DrawerRegistrationToken(std::function<void()> release)
        : m_release(std::move(release))
    {
    }
    ~DrawerRegistrationToken()
    {
        Reset();
    }
    DrawerRegistrationToken(const DrawerRegistrationToken&) = delete;
    DrawerRegistrationToken& operator=(const DrawerRegistrationToken&) = delete;
    DrawerRegistrationToken(DrawerRegistrationToken&& other) noexcept
        : m_release(std::exchange(other.m_release, {}))
    {
    }
    DrawerRegistrationToken& operator=(DrawerRegistrationToken&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_release = std::exchange(other.m_release, {});
        }
        return *this;
    }
    void Reset()
    {
        if (auto release = std::exchange(m_release, {}))
            release();
    }

private:
    std::function<void()> m_release;
};

// InspectorRegistry remains the single resolver for old and new registrations.
// Tokens unregister before a native module unloads its drawing code.
class PropertyDrawerRegistry
{
public:
    explicit PropertyDrawerRegistry(Inspector::InspectorRegistry& registry = Inspector::InspectorRegistry::Get())
        : m_registry(registry)
    {
    }
    static PropertyDrawerRegistry& Get();
    [[nodiscard]] DrawerRegistrationToken Register(PropertyDrawerRegistration registration);
    [[nodiscard]] DrawerRegistrationToken Register(PropertyDrawerRegistration registration,
                                                   std::shared_ptr<IPropertyDrawer> drawer);
    std::optional<PropertyDrawerRegistration> Resolve(const Inspector::PropertyNode& property) const
    {
        return m_registry.ResolvePropertyDrawer(property);
    }
    uint64_t Version() const
    {
        return m_registry.GetVersion();
    }

private:
    Inspector::InspectorRegistry& m_registry;
};
void EnsureBuiltinPropertyDrawers();
} // namespace NLS::Editor::Properties
