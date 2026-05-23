#pragma once

#include <optional>
#include <string>

#include "EngineDef.h"
#include "PrimitiveType.h"

namespace NLS::Engine
{
    class GameObject;
}

namespace NLS::Render::Resources
{
    class Material;
}

namespace NLS::Engine
{
    NLS_ENGINE_API std::string GetPrimitiveMeshResourcePath(PrimitiveType type);
    NLS_ENGINE_API std::optional<PrimitiveType> TryGetPrimitiveTypeFromMeshResourcePath(const std::string& path);
    NLS_ENGINE_API bool ConfigurePrimitiveGameObject(
        GameObject& gameObject,
        PrimitiveType type,
        Render::Resources::Material* material = nullptr);
}
