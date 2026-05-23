#pragma once

#include <cstddef>

#include "Assets/ArtifactManifest.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"

#define NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(APPLY) \
    APPLY(NLS::Render::Resources::Texture, "Texture", NLS::Core::Assets::ArtifactType::Texture, "texture")     \
    APPLY(NLS::Render::Resources::Texture2D, "Texture2D", NLS::Core::Assets::ArtifactType::Texture, "texture") \
    APPLY(NLS::Render::Resources::TextureCube, "TextureCube", NLS::Core::Assets::ArtifactType::Texture, "texture") \
    APPLY(NLS::Render::Resources::Mesh, "Mesh", NLS::Core::Assets::ArtifactType::Mesh, "mesh")           \
    APPLY(NLS::Render::Resources::Shader, "Shader", NLS::Core::Assets::ArtifactType::Shader, "shader")       \
    APPLY(NLS::Render::Resources::Material, "Material", NLS::Core::Assets::ArtifactType::Material, "material")

namespace NLS::Engine::Serialize
{
#define NLS_ENGINE_SERIALIZE_COUNT_PPTR_RESOURCE_TARGET(type, label, artifactType, subAssetPrefix) + 1u
    inline constexpr std::size_t kPPtrResourceTargetCount =
        0u NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_ENGINE_SERIALIZE_COUNT_PPTR_RESOURCE_TARGET);
#undef NLS_ENGINE_SERIALIZE_COUNT_PPTR_RESOURCE_TARGET
}
