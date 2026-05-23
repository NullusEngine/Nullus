#pragma once

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetDiagnostics.h"
#include "Assets/EditorAssetDragPayload.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
struct AssetBrowserSubAssetEntry
{
    std::string displayName;
    std::string sourceAssetPath;
    std::string subAssetKey;
    std::string dragResourcePath;
    NLS::Core::Assets::AssetId assetId;
    NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
    bool generatedReadOnly = false;
};

std::vector<AssetBrowserSubAssetEntry> BuildAssetBrowserSubAssetEntries(
    const AssetDatabaseFacade& database,
    const std::string& sourceAssetPath);

struct ObjectReferencePickerEntry
{
    std::string displayName;
    EditorAssetDragPayload payload {};
};

std::vector<ObjectReferencePickerEntry> BuildObjectReferencePickerEntries(
    const AssetDatabaseFacade& database);

void SetObjectReferencePickerAssetRoots(std::vector<EditorAssetRoot> roots);
std::vector<EditorAssetRoot> GetObjectReferencePickerAssetRoots();
void SetObjectReferencePickerEntries(std::vector<ObjectReferencePickerEntry> entries);
std::vector<ObjectReferencePickerEntry> GetObjectReferencePickerEntries();

struct AssetWatcherStartupReport
{
    bool succeeded = true;
    NLS::Core::Assets::AssetDiagnostics diagnostics;
};

AssetWatcherStartupReport BuildAssetWatcherStartupReport(
    const std::filesystem::path& engineAssetsPath,
    bool engineWatcherStarted,
    const std::filesystem::path& projectAssetsPath,
    bool projectWatcherStarted);
}
