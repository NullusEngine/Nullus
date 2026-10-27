#pragma once
#include "ImportedScene.h"
#include <filesystem>

namespace NLS::Render::Assets
{
NLS_RENDER_API Animation::ImportSettings AnimationSettingsForScene(const ImportedScene& scene);
NLS_RENDER_API bool AttachAnimationSceneData(const std::filesystem::path& source,ImportedScene& scene,std::string& error);
NLS_RENDER_API std::vector<uint8_t> SerializeAnimationSubAsset(const ImportedScene& scene,const GeneratedSceneSubAsset& subAsset);
NLS_RENDER_API bool StabilizeAnimationIdentities(ImportedScene& scene,const NLS::Core::Assets::ArtifactManifest* previous,std::string& error);
}
