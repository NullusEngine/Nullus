#pragma once

#include "ParticleBounds.h"
#include "MeshRenderGeometry.h"
#include <Rendering/RHI/Core/RHIDevice.h>
#include <Rendering/Resources/ParticleMaterialInterface.h>

namespace NLS::VFX
{
inline constexpr uint32_t kParticleMaterialAbiVersion = Render::Resources::kParticleSpriteMaterialAbi;
struct SpriteMaterialProgram
{
    uint32_t abiVersion = kParticleMaterialAbiVersion;
    std::string identity;
    std::vector<Render::RHI::RHIShaderModuleDesc> fragments;
    Render::RHI::RHIBindingLayoutDesc layout;
};
struct SpriteMaterialConstant
{
    uint32_t binding = 0;
    std::vector<uint8_t> bytes;
    bool operator==(const SpriteMaterialConstant&) const = default;
};
struct SpriteMaterialTexture
{
    uint32_t binding = 0;
    std::shared_ptr<Render::RHI::RHITexture> texture;
    bool operator==(const SpriteMaterialTexture&) const = default;
};
struct SpriteMaterialSampler
{
    uint32_t binding = 0;
    Render::RHI::SamplerDesc sampler;
    bool operator==(const SpriteMaterialSampler& other) const
    {
        const auto& b = other.sampler;
        return binding == other.binding && sampler.minFilter == b.minFilter && sampler.magFilter == b.magFilter && sampler.mipFilter == b.mipFilter &&
            sampler.wrapU == b.wrapU && sampler.wrapV == b.wrapV && sampler.wrapW == b.wrapW && sampler.maxAnisotropy == b.maxAnisotropy &&
            sampler.minLod == b.minLod && sampler.maxLod == b.maxLod && sampler.mipLodBias == b.mipLodBias && sampler.compareEnabled == b.compareEnabled &&
            sampler.compareFunc == b.compareFunc && sampler.borderColor == b.borderColor;
    }
};
// Immutable draw values captured from an engine material; no raw asset pointers
// cross to the render thread. The built-in particle material evaluates unlit color.
struct SpriteMaterial
{
    Value tint{1,1,1,1};
    Value uvTransform{1,1,0,0};
    float emissive = 1;
    std::shared_ptr<Render::RHI::RHITexture> texture;
    Render::RHI::SamplerDesc sampler;
    std::array<Value,4> dynamicDefaults{{Value{1,1,1,1},Value{1,1,1,1},Value{1,1,1,1},Value{1,1,1,1}}};
    bool hasDynamicDefaults = false;
    std::shared_ptr<const SpriteMaterialProgram> program;
    std::vector<SpriteMaterialConstant> constants;
    std::vector<SpriteMaterialTexture> textures;
    std::vector<SpriteMaterialSampler> samplers;
    bool operator==(const SpriteMaterial& other) const
    {
        return tint == other.tint && uvTransform == other.uvTransform && emissive == other.emissive && texture == other.texture &&
            SpriteMaterialSampler{0,sampler} == SpriteMaterialSampler{0,other.sampler} && dynamicDefaults == other.dynamicDefaults &&
            hasDynamicDefaults == other.hasDynamicDefaults && program == other.program && constants == other.constants &&
            textures == other.textures && samplers == other.samplers;
    }
};
struct SpriteDrawContext
{
    SpriteMaterial material;
    // Mesh slots; one entry per captured subasset with its LOD chain.
    std::vector<std::shared_ptr<const MeshRenderGeometry>> meshes;
    // Optional per-slot material overrides; missing entries use material.
    std::vector<SpriteMaterial> meshMaterials;
    std::shared_ptr<Render::RHI::RHITextureView> sceneDepth;
    Render::RHI::ResourceState sceneDepthState = Render::RHI::ResourceState::ShaderRead;
    // Clip z=(A*z+B), clip w=(C*z+D). Sprite PS reconstructs view-space depth.
    Value depthProjection{1,0,0,1};
};
}
