#include "Assets/AssetImporterSettings.h"

#include "Assets/ModelTextureTextCodec.h"
#include "Debug/Assertion.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
constexpr const char* kModelTextureSettingsVersionKey = "MODEL_TEXTURE_SETTINGS_VERSION";
constexpr const char* kModelTextureUseExternalTexturesKey = "MODEL_TEXTURE_USE_EXTERNAL_TEXTURES";
constexpr const char* kModelTextureSearchByNameKey = "MODEL_TEXTURE_SEARCH_BY_NAME";
constexpr const char* kModelTextureAutoImportMissingKey = "MODEL_TEXTURE_AUTO_IMPORT_MISSING";
constexpr const char* kModelTextureEmbeddedModeKey = "MODEL_TEXTURE_EMBEDDED_MODE";
constexpr const char* kModelTextureRemapSettingPrefix = "MODEL_TEXTURE_REMAP.";
constexpr uint32_t kCurrentModelTextureSettingsVersion = 1u;

std::optional<bool> ParseStrictBool(const std::string& value)
{
    if (value == "true" || value == "1")
        return true;
    if (value == "false" || value == "0")
        return false;
    return std::nullopt;
}

bool StrictBoolFromSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    const bool fallback)
{
    const auto found = settings.find(key);
    if (found == settings.end())
        return fallback;

    const auto parsed = ParseStrictBool(found->second);
    return parsed.value_or(fallback);
}

std::optional<uint32_t> ParseUInt32(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    uint32_t value = 0u;
    for (const char character : text)
    {
        if (character < '0' || character > '9')
            return std::nullopt;
        const uint32_t digit = static_cast<uint32_t>(character - '0');
        if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10u)
            return std::nullopt;
        value = value * 10u + digit;
    }
    return value;
}

std::string ToString(const ModelEmbeddedTextureMode mode)
{
    switch (mode)
    {
    case ModelEmbeddedTextureMode::ModelSubAsset:
        return "ModelSubAsset";
    }

    NLS_ASSERT(false, "Unhandled model embedded texture mode.");
    return "ModelSubAsset";
}

ModelEmbeddedTextureMode ModelEmbeddedTextureModeFromString(
    const std::string& value,
    const ModelEmbeddedTextureMode fallback)
{
    if (value == "ModelSubAsset")
        return ModelEmbeddedTextureMode::ModelSubAsset;
    return fallback;
}

uint64_t Fnv1a64(std::string_view text)
{
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : text)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string HashText(std::string_view text)
{
    std::ostringstream stream;
    stream << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << Fnv1a64(text);
    return stream.str();
}

bool HasPrefix(const std::string& value, const std::string_view prefix)
{
    return value.size() >= prefix.size() && value.compare(0u, prefix.size(), prefix) == 0;
}

void AddRemapMalformedDiagnostic(
    NLS::Core::Assets::AssetDiagnostics* diagnostics,
    const std::string& settingKey)
{
    if (diagnostics == nullptr)
        return;

    diagnostics->push_back({
        NLS::Core::Assets::AssetDiagnosticSeverity::Warning,
        "model-texture-remap-malformed",
        {},
        {},
        "Model texture remap setting " + settingKey + " is malformed and was ignored."
    });
}
}

std::string BoolToImporterSettingString(const bool value)
{
    return value ? "true" : "false";
}

std::string FbxReaderSelectionToImporterSettingString(const FbxReaderSelection value)
{
    switch (value)
    {
    case FbxReaderSelection::Assimp:
        return "assimp";
    case FbxReaderSelection::AutodeskWithAssimpFallback:
        return "autodesk-with-assimp-fallback";
    case FbxReaderSelection::Autodesk:
        return "autodesk";
    }

    NLS_ASSERT(false, "Unhandled FBX reader selection.");
    return "autodesk";
}

FbxReaderSelection FbxReaderSelectionFromImporterSettingString(
    const std::string& value,
    const FbxReaderSelection fallback)
{
    if (value == "autodesk")
        return FbxReaderSelection::Autodesk;
    if (value == "assimp")
        return FbxReaderSelection::Assimp;
    if (value == "autodesk-with-assimp-fallback")
        return FbxReaderSelection::AutodeskWithAssimpFallback;
    return fallback;
}

bool BoolFromImporterSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    const bool fallback)
{
    const auto found = settings.find(key);
    if (found == settings.end())
        return fallback;
    return found->second == "true" || found->second == "1";
}

double DoubleFromImporterSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    const double fallback)
{
    const auto found = settings.find(key);
    if (found == settings.end())
        return fallback;

    try
    {
        return std::stod(found->second);
    }
    catch (...)
    {
        return fallback;
    }
}

uint32_t UIntFromImporterSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    const uint32_t fallback)
{
    const auto found = settings.find(key);
    if (found == settings.end())
        return fallback;

    try
    {
        return static_cast<uint32_t>(std::stoul(found->second));
    }
    catch (...)
    {
        return fallback;
    }
}

std::string StringFromImporterSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    std::string fallback)
{
    const auto found = settings.find(key);
    return found != settings.end() ? found->second : std::move(fallback);
}

ModelTextureResolutionSettings LoadModelTextureResolutionSettings(
    const NLS::Core::Assets::AssetMeta& meta)
{
    ModelTextureResolutionSettings result;

    const auto versionFound = meta.settings.find(kModelTextureSettingsVersionKey);
    if (versionFound != meta.settings.end())
    {
        const auto parsedVersion = ParseUInt32(versionFound->second);
        if (!parsedVersion.has_value() || *parsedVersion != kCurrentModelTextureSettingsVersion)
            return result;
        result.settingsVersion = *parsedVersion;
    }

    result.useExternalTextures = StrictBoolFromSettings(
        meta.settings,
        kModelTextureUseExternalTexturesKey,
        result.useExternalTextures);
    result.searchByName = StrictBoolFromSettings(
        meta.settings,
        kModelTextureSearchByNameKey,
        result.searchByName);
    result.autoImportMissingTextureFiles = StrictBoolFromSettings(
        meta.settings,
        kModelTextureAutoImportMissingKey,
        result.autoImportMissingTextureFiles);
    result.embeddedTextureMode = ModelEmbeddedTextureModeFromString(
        StringFromImporterSettings(meta.settings, kModelTextureEmbeddedModeKey, ToString(result.embeddedTextureMode)),
        result.embeddedTextureMode);

    return result;
}

void StoreModelTextureResolutionSettings(
    NLS::Core::Assets::AssetMeta& meta,
    const ModelTextureResolutionSettings& settings)
{
    meta.settings[kModelTextureSettingsVersionKey] = std::to_string(kCurrentModelTextureSettingsVersion);
    meta.settings[kModelTextureUseExternalTexturesKey] =
        BoolToImporterSettingString(settings.useExternalTextures);
    meta.settings[kModelTextureSearchByNameKey] =
        BoolToImporterSettingString(settings.searchByName);
    meta.settings[kModelTextureAutoImportMissingKey] =
        BoolToImporterSettingString(settings.autoImportMissingTextureFiles);
    meta.settings[kModelTextureEmbeddedModeKey] = ToString(settings.embeddedTextureMode);
}

std::string MakeModelTextureRemapSettingKey(const std::string& stableSourceKey)
{
    return std::string(kModelTextureRemapSettingPrefix) + EncodeModelTextureTextField(stableSourceKey);
}

std::vector<ModelTextureExplicitRemapSetting> LoadModelTextureRemapSettings(
    const NLS::Core::Assets::AssetMeta& meta,
    NLS::Core::Assets::AssetDiagnostics* diagnostics)
{
    std::vector<ModelTextureExplicitRemapSetting> remaps;

    for (const auto& [key, value] : meta.settings)
    {
        if (!HasPrefix(key, kModelTextureRemapSettingPrefix))
            continue;

        const auto encodedSourceKey = std::string_view(key).substr(std::string_view(kModelTextureRemapSettingPrefix).size());
        const auto sourceKey = DecodeModelTextureTextField(encodedSourceKey);
        if (!sourceKey.has_value() || !HasPrefix(*sourceKey, "mtxsrc:v1"))
        {
            AddRemapMalformedDiagnostic(diagnostics, key);
            continue;
        }

        const auto separator = value.find('#');
        if (separator == std::string::npos)
        {
            AddRemapMalformedDiagnostic(diagnostics, key);
            continue;
        }

        const auto secondSeparator = value.find('#', separator + 1u);

        const auto guidText = DecodeModelTextureTextField(std::string_view(value).substr(0u, separator));
        const auto subAssetKey = DecodeModelTextureTextField(std::string_view(value).substr(
            separator + 1u,
            secondSeparator == std::string::npos ? std::string_view::npos : secondSeparator - separator - 1u));
        const auto editorPath = secondSeparator == std::string::npos
            ? std::optional<std::string> {}
            : DecodeModelTextureTextField(std::string_view(value).substr(secondSeparator + 1u));
        if (!guidText.has_value() || !subAssetKey.has_value() ||
            (secondSeparator != std::string::npos && !editorPath.has_value()))
        {
            AddRemapMalformedDiagnostic(diagnostics, key);
            continue;
        }

        const auto guid = NLS::Guid::TryParse(*guidText);
        if (!guid.has_value() || !guid->IsValid())
        {
            AddRemapMalformedDiagnostic(diagnostics, key);
            continue;
        }

        ModelTextureExplicitRemapSetting remap;
        remap.sourceStableKey = *sourceKey;
        remap.targetAssetId = NLS::Core::Assets::AssetId(*guid);
        remap.targetSubAssetKey = *subAssetKey;
        remap.targetEditorPath = editorPath.value_or(std::string {});
        remaps.push_back(std::move(remap));
    }

    return remaps;
}

void StoreModelTextureRemapSetting(
    NLS::Core::Assets::AssetMeta& meta,
    const ModelTextureExplicitRemapSetting& remap)
{
    if (remap.sourceStableKey.empty() || !remap.targetAssetId.IsValid())
        return;

    meta.settings[MakeModelTextureRemapSettingKey(remap.sourceStableKey)] =
        EncodeModelTextureTextField(remap.targetAssetId.ToString()) +
        "#" +
        EncodeModelTextureTextField(remap.targetSubAssetKey) +
        "#" +
        EncodeModelTextureTextField(remap.targetEditorPath);
}

void ClearModelTextureRemapSetting(
    NLS::Core::Assets::AssetMeta& meta,
    const std::string& stableSourceKey)
{
    meta.settings.erase(MakeModelTextureRemapSettingKey(stableSourceKey));
}

std::string ComputeModelTextureSettingsFingerprint(const NLS::Core::Assets::AssetMeta& meta)
{
    const auto settings = LoadModelTextureResolutionSettings(meta);
    std::vector<std::string> parts {
        "version=" + std::to_string(settings.settingsVersion),
        std::string("useExternal=") + BoolToImporterSettingString(settings.useExternalTextures),
        std::string("searchByName=") + BoolToImporterSettingString(settings.searchByName),
        std::string("autoImportMissing=") + BoolToImporterSettingString(settings.autoImportMissingTextureFiles),
        "embeddedMode=" + ToString(settings.embeddedTextureMode)
    };

    auto remaps = LoadModelTextureRemapSettings(meta);
    std::sort(
        remaps.begin(),
        remaps.end(),
        [](const ModelTextureExplicitRemapSetting& lhs, const ModelTextureExplicitRemapSetting& rhs)
        {
            return lhs.sourceStableKey < rhs.sourceStableKey;
        });

    for (const auto& remap : remaps)
    {
        parts.push_back(
            "remap=" +
            EncodeModelTextureTextField(remap.sourceStableKey) +
            "#" +
            EncodeModelTextureTextField(remap.targetAssetId.ToString()) +
            "#" +
            EncodeModelTextureTextField(remap.targetSubAssetKey) +
            "#" +
            EncodeModelTextureTextField(remap.targetEditorPath));
    }

    std::string joined;
    for (const auto& part : parts)
    {
        joined += part;
        joined.push_back('\n');
    }
    return HashText(joined);
}

ModelImporterSettings ModelImporterSettingsFromSerialized(
    const std::map<std::string, std::string>& settings)
{
    ModelImporterSettings result;
    result.globalScale = DoubleFromImporterSettings(settings, "MODEL_GLOBAL_SCALE", result.globalScale);
    result.axisConversion = StringFromImporterSettings(settings, "MODEL_AXIS_CONVERSION", result.axisConversion);
    result.unitConversion = StringFromImporterSettings(settings, "MODEL_UNIT_CONVERSION", result.unitConversion);
    result.hierarchyPolicy = StringFromImporterSettings(settings, "MODEL_HIERARCHY_POLICY", result.hierarchyPolicy);
    result.fbxReaderSelection = FbxReaderSelectionFromImporterSettingString(
        StringFromImporterSettings(settings, "MODEL_FBX_READER"),
        result.fbxReaderSelection);
    result.importNormals = BoolFromImporterSettings(settings, "MODEL_IMPORT_NORMALS", result.importNormals);
    result.importTangents = BoolFromImporterSettings(settings, "MODEL_IMPORT_TANGENTS", result.importTangents);
    result.importUvs = BoolFromImporterSettings(settings, "MODEL_IMPORT_UVS", result.importUvs);
    result.importMaterials = BoolFromImporterSettings(settings, "MODEL_IMPORT_MATERIALS", result.importMaterials);
    result.importSkeleton = BoolFromImporterSettings(settings, "MODEL_IMPORT_SKELETON", result.importSkeleton);
    result.importAnimations = BoolFromImporterSettings(settings, "MODEL_IMPORT_ANIMATIONS", result.importAnimations);
    result.importMorphTargets = BoolFromImporterSettings(settings, "MODEL_IMPORT_MORPH_TARGETS", result.importMorphTargets);
    result.importCameras = BoolFromImporterSettings(settings, "MODEL_IMPORT_CAMERAS", result.importCameras);
    result.importLights = BoolFromImporterSettings(settings, "MODEL_IMPORT_LIGHTS", result.importLights);
    result.ignoreFbxTexturedNeutralDiffuseTint = BoolFromImporterSettings(
        settings,
        kModelFbxIgnoreTexturedNeutralDiffuseTintSetting,
        result.ignoreFbxTexturedNeutralDiffuseTint);
    result.lodGroup = StringFromImporterSettings(settings, "LOD_GROUP", result.lodGroup);
    result.importMeshLODs = BoolFromImporterSettings(settings, "IMPORT_MESH_LODS", result.importMeshLODs);
    result.minLOD = UIntFromImporterSettings(settings, "MIN_LOD", result.minLOD);
    result.autoComputeLODScreenSize = BoolFromImporterSettings(
        settings,
        "AUTO_COMPUTE_LOD_SCREEN_SIZE",
        result.autoComputeLODScreenSize);
    return result;
}

NLS::Render::Assets::SceneImportSettings ToSceneImportSettings(const ModelImporterSettings& settings)
{
    NLS::Render::Assets::SceneImportSettings result;
    result.globalScale = settings.globalScale;
    result.axisConversion = settings.axisConversion;
    result.unitConversion = settings.unitConversion;
    result.hierarchyPolicy = settings.hierarchyPolicy;
    result.importNormals = settings.importNormals;
    result.importTangents = settings.importTangents;
    result.importUvs = settings.importUvs;
    result.importMaterials = settings.importMaterials;
    result.importSkeleton = settings.importSkeleton;
    result.importAnimations = settings.importAnimations;
    result.importMorphTargets = settings.importMorphTargets;
    result.importCameras = settings.importCameras;
    result.importLights = settings.importLights;
    result.ignoreFbxTexturedNeutralDiffuseTint = settings.ignoreFbxTexturedNeutralDiffuseTint;
    result.lodGroup = settings.lodGroup;
    result.importMeshLODs = settings.importMeshLODs;
    result.minLOD = settings.minLOD;
    result.autoComputeLODScreenSize = settings.autoComputeLODScreenSize;
    return result;
}

NLS::Render::Assets::SceneImportSettings ToSceneImportSettings(
    const std::map<std::string, std::string>& settings)
{
    return ToSceneImportSettings(ModelImporterSettingsFromSerialized(settings));
}
}
