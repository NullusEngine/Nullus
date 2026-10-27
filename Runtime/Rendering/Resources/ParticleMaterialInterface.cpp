#include "ParticleMaterialInterface.h"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace NLS::Render::Resources
{
bool ValidateParticleSpriteReflection(const ShaderReflection& reflection, std::string& error)
{
    const auto view = std::find_if(reflection.constantBuffers.begin(),reflection.constantBuffers.end(),[](const auto& buffer)
    { return buffer.name == "VfxSpriteView" && buffer.bindingSpace == 1 && buffer.bindingIndex == 0; });
    if (view == reflection.constantBuffers.end() || view->byteSize != kParticleSpriteViewBytes)
    {
        error = "ParticleSprite requires ABI 7 VfxSpriteView (320 bytes at b0, space1)";
        for (const auto& buffer : reflection.constantBuffers) error += "; "+buffer.name+" bytes="+std::to_string(buffer.byteSize)+" space="+std::to_string(buffer.bindingSpace)+" slot="+std::to_string(buffer.bindingIndex);
        return false;
    }
    const std::array<std::pair<const char*,uint32_t>,17> members{{
        {"VfxViewProjection",0},{"VfxLocalToWorld",64},{"VfxCameraRight",128},{"VfxCameraUp",144},
        {"VfxTint",160},{"VfxUVTransform",176},{"VfxDepthProjection",192},{"VfxShading",208},
        {"VfxCapacity",224},{"VfxSorted",228},{"VfxAlignment",232},{"VfxBlend",236},
        {"VfxSubImagesX",240},{"VfxSubImagesY",244},{"VfxBlendSubImages",248},{"VfxCircularMask",252},
        {"VfxDynamicDefaults",256}}};
    for (const auto& [name,offset] : members)
        if (std::none_of(view->members.begin(),view->members.end(),[&](const auto& member) { return member.name == name && member.byteOffset == offset; }))
        { error = std::string("ParticleSprite ABI member mismatch: ")+name; return false; }
    uint64_t bytes = 0;
    for (const auto& buffer : reflection.constantBuffers)
    {
        if (&buffer == &*view) continue;
        if (buffer.bindingSpace != 2 || buffer.bindingIndex >= 16 || buffer.byteSize == 0 || buffer.byteSize > 65536)
        { error = "ParticleSprite material constants must use b0-b15, space2, with at most 64 KiB per buffer"; return false; }
        if (std::any_of(buffer.members.begin(),buffer.members.end(),[](const auto& member) { return member.arraySize != 1; }))
        { error = "ParticleSprite material constant arrays are not supported"; return false; }
        bytes += buffer.byteSize;
    }
    if (bytes > 65536) { error = "ParticleSprite material constants exceed 64 KiB"; return false; }
    std::unordered_set<uint64_t> slots;
    for (const auto& property : reflection.properties)
    {
        if (property.kind == ShaderResourceKind::UniformBuffer) continue;
        if (property.kind == ShaderResourceKind::Value)
        {
            if (property.parentConstantBuffer.empty()) { error = "ParticleSprite shader contains an unsupported global or writable resource: "+property.name; return false; }
            continue;
        }
        const bool sampler = property.kind == ShaderResourceKind::Sampler;
        const bool texture = property.kind == ShaderResourceKind::SampledTexture && property.type == UniformType::UNIFORM_SAMPLER_2D;
        const bool passBinding = property.bindingSpace == 1 && ((sampler && property.bindingIndex == 0) || (texture && (property.bindingIndex == 2 || property.bindingIndex == 3)));
        if ((!passBinding && property.bindingSpace != 2) || (!sampler && !texture) || property.bindingIndex >= 16 || property.arraySize != 1 ||
            !slots.insert((uint64_t(property.bindingSpace)<<32)|(uint64_t(property.kind)<<16)|property.bindingIndex).second)
        { error = "ParticleSprite permits scalar 2D textures and samplers in space2, plus the fixed Sprite pass resources"; return false; }
    }
    error.clear(); return true;
}
}
