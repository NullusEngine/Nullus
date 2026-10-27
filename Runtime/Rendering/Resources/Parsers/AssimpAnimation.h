#pragma once
#include <Animation/ImportSettings.h>
#include "IModelParser.h"
struct aiScene;
namespace NLS::Render::Resources::Parsers
{
std::shared_ptr<Animation::Model> BuildAssimpAnimation(const aiScene& scene,
    const std::vector<ParsedMeshData>& meshes,const Animation::ImportSettings& settings,std::string& error);
}
