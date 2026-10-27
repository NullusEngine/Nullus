#include "Properties/PropertyDrawerRegistry.h"
#include <stdexcept>

namespace NLS::Editor::Properties
{
PropertyDrawerRegistry& PropertyDrawerRegistry::Get()
{
    static PropertyDrawerRegistry registry;
    return registry;
}
DrawerRegistrationToken PropertyDrawerRegistry::Register(PropertyDrawerRegistration registration)
{
    if (registration.identifier.empty() || !registration.draw)
        throw std::invalid_argument("Property drawer requires an identifier and draw callback");
    if (registration.scope == DrawerScope::Field && registration.ownerTypeName.empty())
        throw std::invalid_argument("Field drawer requires its declaring type");
    const auto identifier = registration.identifier;
    auto* registry = &m_registry;
    auto lifetime = registry->GetLifetime();
    registry->RegisterPropertyDrawer(std::move(registration));
    return DrawerRegistrationToken([registry, lifetime, identifier]
                                   {
        if (!lifetime.expired())
            registry->UnregisterPropertyDrawer(identifier); });
}
DrawerRegistrationToken PropertyDrawerRegistry::Register(PropertyDrawerRegistration registration,
                                                         std::shared_ptr<IPropertyDrawer> drawer)
{
    if (!drawer)
        throw std::invalid_argument("Property drawer must not be null");
    registration.draw = [drawer = std::move(drawer)](PropertyDrawerContext& context)
    {
        context.interaction = drawer->Draw(context);
        if (context.interaction.changed && !context.changed && !context.interaction.canceled)
            context.changed = context.preview(context.value, "Edit " + context.property.displayName);
        return true;
    };
    return Register(std::move(registration));
}
} // namespace NLS::Editor::Properties
