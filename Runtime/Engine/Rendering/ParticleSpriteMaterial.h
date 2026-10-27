#pragma once

#include "EngineDef.h"
#include <Core/Assets/ArtifactManifest.h>
#include <VFX/SpriteMaterial.h>
#include <functional>
#include <unordered_map>

namespace NLS::Engine::Rendering
{
// Editor resolves through its published asset database; packaged scenes use the
// runtime manifest. Both paths must produce typed imported artifacts, never source files.
using SpriteAssetResolver = std::function<bool(const VFX::AssetReference&, Core::Assets::ArtifactType, std::string&, std::string&)>;
NLS_ENGINE_API bool CaptureParticleSpriteMaterial(const VFX::SpriteSettings& settings, VFX::SpriteMaterial& result,
    std::string& error, const SpriteAssetResolver& resolver = {}, const std::unordered_map<std::string,VFX::Value>& parameters = {});
NLS_ENGINE_API bool CaptureParticleMesh(const VFX::MeshSettings& settings, std::vector<std::shared_ptr<const VFX::MeshRenderGeometry>>& result,
    std::string& error, const SpriteAssetResolver& resolver = {});
}
