#pragma once
#include "SceneImportPipeline.h"
#include <Assets/AssetMeta.h>
#include <Rendering/Resources/Parsers/IModelParser.h>

namespace NLS::Render::Assets
{
NLS_RENDER_API SceneImportSettings ReadSceneImportSettings(const std::map<std::string,std::string>& settings);
NLS_RENDER_API Animation::ImportSettings AnimationSettingsForAsset(const NLS::Core::Assets::AssetMeta& meta);
NLS_RENDER_API bool ReadModelSource(const std::filesystem::path& source,const NLS::Core::Assets::AssetMeta& meta,
    Resources::Parsers::EModelParserFlags flags,std::vector<Resources::Parsers::ParsedMeshData>& meshes,
    std::vector<std::string>& materials,std::vector<std::string>& dependencies,ImportedScene& scene,std::string& error);
}
