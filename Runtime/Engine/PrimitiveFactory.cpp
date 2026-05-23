#include "PrimitiveFactory.h"

#include <algorithm>
#include <array>
#include <cctype>

#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "GameObject.h"

namespace NLS::Engine
{
namespace
{
struct PrimitiveDefinition
{
    PrimitiveType type;
    const char* name;
};

constexpr std::array kPrimitiveDefinitions{
    PrimitiveDefinition{PrimitiveType::Cube, "Cube"},
    PrimitiveDefinition{PrimitiveType::Sphere, "Sphere"},
    PrimitiveDefinition{PrimitiveType::Cone, "Cone"},
    PrimitiveDefinition{PrimitiveType::Cylinder, "Cylinder"},
    PrimitiveDefinition{PrimitiveType::Plane, "Plane"},
    PrimitiveDefinition{PrimitiveType::Gear, "Gear"},
    PrimitiveDefinition{PrimitiveType::Helix, "Helix"},
    PrimitiveDefinition{PrimitiveType::Pipe, "Pipe"},
    PrimitiveDefinition{PrimitiveType::Pyramid, "Pyramid"},
    PrimitiveDefinition{PrimitiveType::Torus, "Torus"}
};

const PrimitiveDefinition& GetPrimitiveDefinition(const PrimitiveType type)
{
    const auto found = std::find_if(
        kPrimitiveDefinitions.begin(),
        kPrimitiveDefinitions.end(),
        [type](const PrimitiveDefinition& definition)
        {
            return definition.type == type;
        });
    return found != kPrimitiveDefinitions.end() ? *found : kPrimitiveDefinitions.front();
}

std::string NormalizePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(
        path.begin(),
        path.end(),
        path.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return path;
}
}

const char* GetPrimitiveName(const PrimitiveType type)
{
    return GetPrimitiveDefinition(type).name;
}

std::string GetPrimitiveMeshResourcePath(const PrimitiveType type)
{
    return std::string("builtin:Primitive/") + GetPrimitiveDefinition(type).name;
}

std::optional<PrimitiveType> TryGetPrimitiveTypeFromMeshResourcePath(const std::string& path)
{
    const auto normalizedPath = NormalizePath(path);
    for (const auto& definition : kPrimitiveDefinitions)
    {
        const auto primitivePath = NormalizePath(std::string("builtin:Primitive/") + definition.name);
        if (normalizedPath == primitivePath)
            return definition.type;
    }
    return std::nullopt;
}

bool ConfigurePrimitiveGameObject(
    GameObject& gameObject,
    const PrimitiveType type,
    Render::Resources::Material* material)
{
    auto* meshFilter = gameObject.GetComponent<Components::MeshFilter>();
    if (meshFilter == nullptr)
        meshFilter = gameObject.AddComponent<Components::MeshFilter>();

    auto* meshRenderer = gameObject.GetComponent<Components::MeshRenderer>();
    if (meshRenderer == nullptr)
        meshRenderer = gameObject.AddComponent<Components::MeshRenderer>();

    if (meshFilter != nullptr)
        meshFilter->SetMeshPath(GetPrimitiveMeshResourcePath(type));

    if (meshRenderer != nullptr && material != nullptr)
        meshRenderer->FillWithMaterial(*material);

    return meshFilter != nullptr && meshFilter->ResolveMesh() != nullptr;
}
}
