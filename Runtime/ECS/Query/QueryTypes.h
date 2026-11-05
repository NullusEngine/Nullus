#pragma once
#include "ECS/Core/ComponentRegistry.h"
#include "ECS/Core/Entity.h"
#include <span>

namespace NLS::ECS
{
struct Access
{
    ComponentId type = 0;
    bool write = false;
    bool resource = false;
    bool operator==(const Access&) const = default;
};

template<class T> struct Read { using Type = T; static constexpr bool write = false, optional = false; };
template<class T> struct Write { using Type = T; static constexpr bool write = true, optional = false; };
template<class T> struct OptionalRead { using Type = T; static constexpr bool write = false, optional = true; };
template<class T> struct OptionalWrite { using Type = T; static constexpr bool write = true, optional = true; };

struct QueryDescription
{
    std::vector<ComponentId> all, any, none, enabled, disabled;
    std::vector<Access> access;
    std::vector<std::pair<ComponentId, ChangeToken>> changed;
    std::vector<Entity> entities;
    bool includeDisabled = false;
};
}
