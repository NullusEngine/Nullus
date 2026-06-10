#pragma once

#include "Engine/Assets/PrefabAsset.h"
#include "Serialize/ObjectId.h"

#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
struct ImportedPrefabRendererDependencyTemplate
{
    NLS::Engine::Serialize::ObjectId sourceObject;
    std::string meshPath;
    std::vector<std::string> materialPaths;
};

std::vector<ImportedPrefabRendererDependencyTemplate> BuildImportedPrefabRendererDependencyTemplates(
    const NLS::Engine::Assets::PrefabArtifact& prefab);
}
