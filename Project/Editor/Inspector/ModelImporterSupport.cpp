#include "Inspector/ModelImporterSupport.h"

#include <algorithm>
#include <limits>

#include "Assets/ModelTextureResolutionReport.h"
#include "Rendering/Assets/StaticMeshLODSettings.h"

namespace NLS::Editor::Inspector
{
ModelLODInspectorView BuildModelLODInspectorView(
    const std::map<std::string, std::string>& serializedSettings)
{
    ModelLODInspectorView view;
    const NLS::Render::Assets::StaticMeshLODSettingsRegistry registry;
    const auto settings = NLS::Editor::Assets::ModelImporterSettingsFromSerialized(serializedSettings);
    const auto& presets = registry.GetPresets();

    for (size_t index = 0u; index < presets.size(); ++index)
        view.lodGroupChoices.emplace(static_cast<int>(index), presets[index].name);

    const auto selected = std::find_if(
        presets.begin(),
        presets.end(),
        [&settings](const auto& preset) { return preset.name == settings.lodGroup; });
    view.selectedLODGroup = selected == presets.end()
        ? 0
        : static_cast<int>(std::distance(presets.begin(), selected));
    view.importMeshLODs = settings.importMeshLODs;
    const auto serializedMinLOD = serializedSettings.find("MIN_LOD");
    view.minLOD = serializedMinLOD != serializedSettings.end() &&
        !serializedMinLOD->second.empty() &&
        serializedMinLOD->second.front() == '-'
        ? 0
        : static_cast<int>((std::min)(
            settings.minLOD,
            static_cast<uint32_t>((std::numeric_limits<int>::max)())));
    view.autoComputeLODScreenSize = settings.autoComputeLODScreenSize;
    return view;
}

void StoreModelLODInspectorSettings(
    std::map<std::string, std::string>& serializedSettings,
    const ModelLODInspectorView& view)
{
    const auto choice = view.lodGroupChoices.find(view.selectedLODGroup);
    serializedSettings["LOD_GROUP"] = choice == view.lodGroupChoices.end()
        ? "None"
        : choice->second;
    serializedSettings["IMPORT_MESH_LODS"] =
        NLS::Editor::Assets::BoolToImporterSettingString(view.importMeshLODs);
    serializedSettings["MIN_LOD"] = std::to_string((std::max)(0, view.minLOD));
    serializedSettings["AUTO_COMPUTE_LOD_SCREEN_SIZE"] =
        NLS::Editor::Assets::BoolToImporterSettingString(view.autoComputeLODScreenSize);
}

ModelTextureInspectorView BuildModelTextureInspectorView(
    const NLS::Core::Assets::AssetMeta& modelMeta,
    const std::string& targetPlatform,
    const std::optional<std::string>& reportText)
{
    ModelTextureInspectorView view;
    view.settings = NLS::Editor::Assets::LoadModelTextureResolutionSettings(modelMeta);

    if (!reportText.has_value())
        return view;

    const auto report = NLS::Editor::Assets::ParseModelTextureResolutionReport(*reportText);
    if (!report.has_value())
    {
        view.reportMalformed = true;
        return view;
    }

    const NLS::Editor::Assets::ModelTextureReportContext context {
        modelMeta.id.ToString(),
        targetPlatform,
        modelMeta.importerVersion,
        NLS::Editor::Assets::ComputeModelTextureSettingsFingerprint(modelMeta)
    };
    if (!NLS::Editor::Assets::IsModelTextureResolutionReportCurrent(*report, context))
    {
        view.reportStale = true;
        view.reimportRequired = true;
        return view;
    }

    view.hasCurrentReport = true;
    for (const auto& entry : report->entries)
    {
        ModelTextureInspectorRow row;
        row.sourceStableKey = entry.source.stableKey;
        row.resolutionKind = NLS::Editor::Assets::ToString(entry.kind);
        row.targetAssetId = entry.targetAssetId.IsValid() ? entry.targetAssetId.ToString() : std::string {};
        row.targetSubAssetKey = entry.targetSubAssetKey;
        row.targetEditorPath = !entry.targetEditorPath.empty()
            ? entry.targetEditorPath.generic_string()
            : entry.resourcePath.generic_string();
        row.usesOrderDerivedStableKey =
            entry.source.stableKeyStatus == NLS::Editor::Assets::ModelTextureStableKeyStatus::OrderDerived;

        for (const auto& diagnostic : entry.diagnostics)
        {
            row.diagnosticCodes.push_back(diagnostic.code);
            if (diagnostic.severity == "Warning" || diagnostic.severity == "Error")
                row.hasWarnings = true;
            if (diagnostic.code == "model-texture-remap-invalid-target" ||
                diagnostic.code == "model-texture-remap-non-texture-target" ||
                diagnostic.code == "model-texture-artifact-missing")
            {
                row.hasInvalidTargetWarning = true;
            }
            if (diagnostic.code == "model-texture-unsupported-encoding")
                row.hasUnsupportedEncodingWarning = true;
        }

        if (row.usesOrderDerivedStableKey)
            row.hasWarnings = true;

        view.rows.push_back(std::move(row));
    }

    return view;
}

void StoreModelTextureInspectorSettings(
    NLS::Core::Assets::AssetMeta& modelMeta,
    const NLS::Editor::Assets::ModelTextureResolutionSettings& settings)
{
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, settings);
}

void StoreModelTextureInspectorRemap(
    NLS::Core::Assets::AssetMeta& modelMeta,
    const std::string& stableSourceKey,
    const NLS::Core::Assets::AssetId& targetAssetId,
    const std::string& targetSubAssetKey,
    const std::string& targetEditorPath)
{
    NLS::Editor::Assets::StoreModelTextureRemapSetting(
        modelMeta,
        {stableSourceKey, targetAssetId, targetSubAssetKey, targetEditorPath});
}

void ClearModelTextureInspectorRemap(
    NLS::Core::Assets::AssetMeta& modelMeta,
    const std::string& stableSourceKey)
{
    NLS::Editor::Assets::ClearModelTextureRemapSetting(modelMeta, stableSourceKey);
}
} // namespace NLS::Editor::Inspector
