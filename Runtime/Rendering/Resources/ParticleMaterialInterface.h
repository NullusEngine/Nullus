#pragma once

#include "ShaderReflection.h"

namespace NLS::Render::Resources
{
inline constexpr uint32_t kParticleSpriteMaterialAbi = 7;
inline constexpr uint32_t kParticleSpriteViewBytes = 320;
// ParticleSprite shaders consume the engine Sprite vertex ABI and immutable
// material resources in space2. No scene/object pointers or writable resources.
NLS_RENDER_API bool ValidateParticleSpriteReflection(const ShaderReflection& reflection, std::string& error);
}
