#pragma once
#include "Assets/AssetDatabaseFacade.h"

namespace NLS::Editor::ECSAuthoring
{
// ImportAsset indexes logical Editor paths. Keep the conversion shared by the
// UI and non-graphical tools so a successful file write is not mistaken for a
// successful native asset import.
inline bool ImportBakedSceneAsset(Assets::AssetDatabaseFacade& database,
    const std::filesystem::path& path, std::string& error)
{
    error.clear();
    const auto assetPath = database.TryGetEditorAssetPath(path);
    if (!assetPath)
    {
        error = "The baked Scene is outside the configured Editor asset roots.";
        return false;
    }
    if (database.ImportAsset(*assetPath)) return true;
    error = "ECS Scene import failed for " + *assetPath;
    for (const auto& diagnostic : database.GetDiagnostics())
        error += "\n" + diagnostic.code + ": " + diagnostic.message;
    return false;
}
}
