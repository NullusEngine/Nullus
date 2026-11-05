#pragma once

#include <cstddef>

#include "Assets/ArtifactManifest.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture.h"
#include "Rendering/Resources/Texture2D.h"
#include "Rendering/Resources/TextureCube.h"
#include "Assets/ParticleSystemAsset.h"
#include "Assets/AnimationAsset.h"
#include "Assets/AudioClip.h"
#include "Assets/InputActionsAsset.h"
#include "Assets/SpriteAsset.h"
#include "Assets/PhysicsMaterial.h"
#include "Assets/CollisionMeshAsset.h"
#include "Assets/PhysicsShapeAsset.h"

#define NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(APPLY) \
    APPLY(NLS::Render::Resources::Texture, "Texture", NLS::Core::Assets::ArtifactType::Texture, "texture")     \
    APPLY(NLS::Render::Resources::Texture2D, "Texture2D", NLS::Core::Assets::ArtifactType::Texture, "texture") \
    APPLY(NLS::Render::Resources::TextureCube, "TextureCube", NLS::Core::Assets::ArtifactType::Texture, "texture") \
    APPLY(NLS::Render::Resources::Mesh, "Mesh", NLS::Core::Assets::ArtifactType::Mesh, "mesh")           \
    APPLY(NLS::Render::Resources::Shader, "Shader", NLS::Core::Assets::ArtifactType::Shader, "shader")       \
    APPLY(NLS::Render::Resources::Material, "Material", NLS::Core::Assets::ArtifactType::Material, "material") \
    APPLY(NLS::Engine::Assets::ParticleSystemAsset, "ParticleSystem", NLS::Core::Assets::ArtifactType::VfxSystem, "vfx-system") \
    APPLY(NLS::Engine::Assets::AnimationAsset, "Animation", NLS::Core::Assets::ArtifactType::AnimationClip, "animation") \
    APPLY(NLS::Engine::Assets::AudioClip, "AudioClip", NLS::Core::Assets::ArtifactType::Audio, "audio") \
    APPLY(NLS::Engine::Assets::InputActionsAsset, "InputActions", NLS::Core::Assets::ArtifactType::InputActions, "input-actions") \
    APPLY(NLS::Engine::Assets::SpriteAsset, "Sprite", NLS::Core::Assets::ArtifactType::Sprite, "sprite") \
    APPLY(NLS::Engine::Assets::PhysicsMaterial, "PhysicsMaterial", NLS::Core::Assets::ArtifactType::PhysicsMaterial, "physics-material") \
    APPLY(NLS::Engine::Assets::CollisionMeshAsset, "CollisionMesh", NLS::Core::Assets::ArtifactType::CollisionMesh, "collision-mesh") \
    APPLY(NLS::Engine::Assets::PhysicsShapeAsset, "PhysicsShape", NLS::Core::Assets::ArtifactType::PhysicsShape, "physics-shape")

namespace NLS::Engine::Serialize
{
#define NLS_ENGINE_SERIALIZE_COUNT_PPTR_RESOURCE_TARGET(type, label, artifactType, subAssetPrefix) + 1u
    inline constexpr std::size_t kPPtrResourceTargetCount =
        0u NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS(NLS_ENGINE_SERIALIZE_COUNT_PPTR_RESOURCE_TARGET);
#undef NLS_ENGINE_SERIALIZE_COUNT_PPTR_RESOURCE_TARGET
}
