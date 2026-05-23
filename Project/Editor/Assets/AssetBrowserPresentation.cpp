#include "Assets/AssetBrowserPresentation.h"

#include <algorithm>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
std::vector<EditorAssetRoot> g_objectReferencePickerAssetRoots;
std::vector<ObjectReferencePickerEntry> g_objectReferencePickerEntries;

bool IsInspectorReferenceableSubAsset(const NLS::Core::Assets::ArtifactType p_type)
{
    using NLS::Core::Assets::ArtifactType;
    return p_type == ArtifactType::Mesh ||
           p_type == ArtifactType::Material ||
           p_type == ArtifactType::Texture ||
           p_type == ArtifactType::Shader;
}

std::string MakeSubAssetDisplayName(const std::string& p_subAssetKey)
{
    const auto separator = p_subAssetKey.find(':');
    if (separator == std::string::npos || separator + 1u >= p_subAssetKey.size())
        return p_subAssetKey;

    auto name = p_subAssetKey.substr(separator + 1u);
    const auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos && slash + 1u < name.size())
        name = name.substr(slash + 1u);
    return name;
}
}

void SetObjectReferencePickerAssetRoots(std::vector<EditorAssetRoot> roots)
{
    g_objectReferencePickerAssetRoots = std::move(roots);
}

std::vector<EditorAssetRoot> GetObjectReferencePickerAssetRoots()
{
    return g_objectReferencePickerAssetRoots;
}

void SetObjectReferencePickerEntries(std::vector<ObjectReferencePickerEntry> entries)
{
    g_objectReferencePickerEntries = std::move(entries);
}

std::vector<ObjectReferencePickerEntry> GetObjectReferencePickerEntries()
{
    return g_objectReferencePickerEntries;
}

std::vector<AssetBrowserSubAssetEntry> BuildAssetBrowserSubAssetEntries(
    const AssetDatabaseFacade& database,
    const std::string& sourceAssetPath)
{
    std::vector<AssetBrowserSubAssetEntry> entries;
    const auto records = database.LoadAllAssetsAtPath(sourceAssetPath);
    for (const auto& record : records)
    {
        if (!IsInspectorReferenceableSubAsset(record.artifactType) || record.subAssetKey.empty())
            continue;

        entries.push_back({
            MakeSubAssetDisplayName(record.subAssetKey),
            record.assetPath,
            record.subAssetKey,
            record.assetPath,
            record.assetId,
            record.artifactType,
            true
        });
    }

    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](const AssetBrowserSubAssetEntry& p_left, const AssetBrowserSubAssetEntry& p_right)
        {
            if (p_left.artifactType != p_right.artifactType)
                return p_left.artifactType < p_right.artifactType;
            return p_left.subAssetKey < p_right.subAssetKey;
        });
    return entries;
}

std::vector<ObjectReferencePickerEntry> BuildObjectReferencePickerEntries(
    const AssetDatabaseFacade& database)
{
    std::vector<ObjectReferencePickerEntry> entries;
    for (const auto& assetPath : database.FindAssets({}, {}))
    {
        for (const auto& subAsset : BuildAssetBrowserSubAssetEntries(database, assetPath))
        {
            if (!CanStoreEditorAssetDragPayload(
                    subAsset.dragResourcePath,
                    subAsset.assetId,
                    subAsset.subAssetKey))
            {
                continue;
            }

            entries.push_back({
                subAsset.sourceAssetPath + " / " + subAsset.subAssetKey,
                MakeEditorAssetDragPayload(
                    subAsset.dragResourcePath,
                    subAsset.assetId,
                    subAsset.subAssetKey,
                    subAsset.artifactType,
                    false,
                    true)
            });
        }
    }

    std::stable_sort(
        entries.begin(),
        entries.end(),
        [](const ObjectReferencePickerEntry& p_left, const ObjectReferencePickerEntry& p_right)
        {
            return p_left.displayName < p_right.displayName;
        });
    entries.erase(
        std::unique(
            entries.begin(),
            entries.end(),
            [](const ObjectReferencePickerEntry& p_left, const ObjectReferencePickerEntry& p_right)
            {
                return p_left.displayName == p_right.displayName;
            }),
        entries.end());
    return entries;
}

AssetWatcherStartupReport BuildAssetWatcherStartupReport(
    const std::filesystem::path& engineAssetsPath,
    const bool engineWatcherStarted,
    const std::filesystem::path& projectAssetsPath,
    const bool projectWatcherStarted)
{
    AssetWatcherStartupReport report;
    if (!engineWatcherStarted)
    {
        report.diagnostics.push_back({
            NLS::Core::Assets::AssetDiagnosticSeverity::Error,
            "asset-watcher-start-failed",
            {},
            engineAssetsPath,
            "Engine asset watcher could not be started."
        });
    }
    if (!projectWatcherStarted)
    {
        report.diagnostics.push_back({
            NLS::Core::Assets::AssetDiagnosticSeverity::Error,
            "asset-watcher-start-failed",
            {},
            projectAssetsPath,
            "Project asset watcher could not be started."
        });
    }
    report.succeeded = report.diagnostics.empty();
    return report;
}
}
