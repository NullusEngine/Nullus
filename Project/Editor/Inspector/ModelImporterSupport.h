#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "Assets/AssetImporterSettings.h"
#include "Assets/AssetMeta.h"

namespace NLS::Editor::Inspector
{
// Importer-specific state belongs to the unified Inspector rather than the
// retired Asset Properties panel. These are intentionally UI-neutral views so
// adapters and tests can share the same normalization rules.
struct ModelLODInspectorView
{
    std::map<int, std::string> lodGroupChoices;
    int selectedLODGroup = 0;
    bool importMeshLODs = false;
    int minLOD = 0;
    bool autoComputeLODScreenSize = true;
};

[[nodiscard]] ModelLODInspectorView BuildModelLODInspectorView(
    const std::map<std::string, std::string>& serializedSettings);
void StoreModelLODInspectorSettings(
    std::map<std::string, std::string>& serializedSettings,
    const ModelLODInspectorView& view);

struct ModelTextureInspectorRow
{
    std::string sourceStableKey;
    std::string resolutionKind;
    std::string targetAssetId;
    std::string targetSubAssetKey;
    std::string targetEditorPath;
    std::vector<std::string> diagnosticCodes;
    bool hasWarnings = false;
    bool hasInvalidTargetWarning = false;
    bool hasUnsupportedEncodingWarning = false;
    bool usesOrderDerivedStableKey = false;
};

struct ModelTextureInspectorView
{
    NLS::Editor::Assets::ModelTextureResolutionSettings settings;
    bool hasCurrentReport = false;
    bool reportMalformed = false;
    bool reportStale = false;
    bool reimportRequired = false;
    std::vector<ModelTextureInspectorRow> rows;
};

[[nodiscard]] ModelTextureInspectorView BuildModelTextureInspectorView(
    const NLS::Core::Assets::AssetMeta& modelMeta,
    const std::string& targetPlatform,
    const std::optional<std::string>& reportText);
void StoreModelTextureInspectorSettings(
    NLS::Core::Assets::AssetMeta& modelMeta,
    const NLS::Editor::Assets::ModelTextureResolutionSettings& settings);
void StoreModelTextureInspectorRemap(
    NLS::Core::Assets::AssetMeta& modelMeta,
    const std::string& stableSourceKey,
    const NLS::Core::Assets::AssetId& targetAssetId,
    const std::string& targetSubAssetKey,
    const std::string& targetEditorPath);
void ClearModelTextureInspectorRemap(
    NLS::Core::Assets::AssetMeta& modelMeta,
    const std::string& stableSourceKey);
} // namespace NLS::Editor::Inspector
