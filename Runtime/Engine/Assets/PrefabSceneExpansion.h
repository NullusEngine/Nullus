#pragma once

#include "EngineDef.h"
#include "Serialize/ObjectGraphDocument.h"
#include <functional>

namespace NLS::Engine::Assets
{
using PrefabGraphResolver = std::function<std::optional<Serialize::ObjectGraphDocument>(
    const Serialize::ObjectIdentifier&, std::string&)>;

// Produces a self-contained scene graph for the Player without instantiating
// components or loading authoring files. Failure leaves the input unchanged.
NLS_ENGINE_API bool ExpandPrefabInstances(
    Serialize::ObjectGraphDocument& document, const PrefabGraphResolver& resolver, std::string& error);
}
