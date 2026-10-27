#pragma once

#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "EngineDef.h"

#include <optional>
#include <string>

namespace NLS::Engine::Assets
{
// Serialises the runtime asset manifest that shipped packages carry next to
// their content. The JSON form is the interface between the build pipeline and
// the player runtime; GUIDs stay canonical so both sides resolve identically.
NLS_ENGINE_API std::string SerializeRuntimeAssetManifest(const RuntimeAssetManifest& manifest);
NLS_ENGINE_API std::optional<RuntimeAssetManifest> DeserializeRuntimeAssetManifest(
    const std::string& json,
    std::string* diagnostics = nullptr);
} // namespace NLS::Engine::Assets
