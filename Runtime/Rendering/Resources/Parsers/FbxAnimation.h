#pragma once
#include "IModelParser.h"
#include <Rendering/Assets/ImportedScene.h>
namespace NLS::Render::Resources::Parsers
{
std::shared_ptr<Animation::Model> BuildFbxSdkAnimation(void* sourceScene,
    const Assets::ImportedScene& scene,const std::vector<ParsedMeshData>& meshes,
    const Animation::ImportSettings& settings,double meshUnitScale,std::string& error);
}
