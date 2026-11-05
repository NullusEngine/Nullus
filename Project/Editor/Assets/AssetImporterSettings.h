#pragma once

#include "Assets/AssetMeta.h"
#include "Assets/AssetDiagnostics.h"
#include "Rendering/Assets/ImportedScene.h"
#include "Rendering/DistanceField/DistanceFieldTypes.h"
#include "Reflection/Macros.h"
#if !defined(__REFLECTION_PARSER__)
#include "Project/Editor/Assets/AssetImporterSettings.generated.h"
#endif

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace NLS::meta { class Type; class Variant; }

namespace NLS::Editor::Assets
{
inline constexpr const char* kModelFbxIgnoreTexturedNeutralDiffuseTintSetting =
    "MODEL_FBX_IGNORE_TEXTURED_NEUTRAL_DIFFUSE_TINT";

using FbxReaderSelection = NLS::Animation::FbxReaderSelection;

struct ModelImporterSettings
{
    double globalScale = 1.0;
    std::string axisConversion;
    std::string unitConversion;
    std::string hierarchyPolicy = "preserve";
    FbxReaderSelection fbxReaderSelection = FbxReaderSelection::AutodeskWithAssimpFallback;
    bool importNormals = true;
    bool importTangents = true;
    bool importUvs = true;
    bool importMaterials = true;
    bool importSkeleton = true;
    bool importAnimations = true;
    bool importMorphTargets = true;
    bool importCameras = false;
    bool importLights = false;
    bool ignoreFbxTexturedNeutralDiffuseTint = true;
    std::string lodGroup = "None";
    bool importMeshLODs = false;
    uint32_t minLOD = 0u;
    bool autoComputeLODScreenSize = true;
    double animationSampleRate = 0;
    std::vector<std::string> animationTakes;
    std::string animationTargetSkeleton;
};

ENUM(TextureImporterType) { Default, Sprite, NormalMap, Mask };
ENUM(SpriteImporterMode) { Single, Multiple };

// Types of the enum fields in the metadata-backed importer draft.
STRUCT(ImporterSettingsSchema)
{
    GENERATED_BODY()
    PROPERTY(DisplayName("Distance Field Generation"))
    NLS::Render::DistanceField::GenerationMode DISTANCE_FIELD_GENERATION = NLS::Render::DistanceField::GenerationMode::Inherit;
    PROPERTY(DisplayName("FBX Reader"))
    NLS::Animation::FbxReaderSelection MODEL_FBX_READER = NLS::Animation::FbxReaderSelection::AutodeskWithAssimpFallback;
    PROPERTY(DisplayName("Texture Type"))
    TextureImporterType TEXTURE_TYPE = TextureImporterType::Default;
    PROPERTY(DisplayName("Sprite Mode"))
    SpriteImporterMode SPRITE_MODE = SpriteImporterMode::Single;
};

struct TexturePlatformOverride
{
    std::string platform;
    uint32_t maxTextureSize = 0u;
    std::string format;
    std::string compressionQuality;
    std::string resizePolicy;
    std::optional<bool> mipmapEnabled;
};

struct TextureImporterSettings
{
    std::string textureType = "default";
    bool srgbTexture = true;
    bool alphaIsTransparency = false;
    bool mipmapEnabled = true;
    std::string wrapMode = "repeat";
    std::string filterMode = "bilinear";
    uint32_t maxTextureSize = 2048u;
    std::string resizePolicy = "keep";
    std::string compressionIntent = "default";
    std::string explicitFormat;
    std::vector<TexturePlatformOverride> platformOverrides;
};

enum class ModelEmbeddedTextureMode
{
    ModelSubAsset
};

struct ModelTextureResolutionSettings
{
    uint32_t settingsVersion = 1u;
    bool useExternalTextures = true;
    bool searchByName = true;
    bool autoImportMissingTextureFiles = true;
    ModelEmbeddedTextureMode embeddedTextureMode = ModelEmbeddedTextureMode::ModelSubAsset;
};

struct ModelTextureExplicitRemapSetting
{
    std::string sourceStableKey;
    NLS::Core::Assets::AssetId targetAssetId;
    std::string targetSubAssetKey;
    std::string targetEditorPath;
};

std::string BoolToImporterSettingString(bool value);
std::string FbxReaderSelectionToImporterSettingString(FbxReaderSelection value);
FbxReaderSelection FbxReaderSelectionFromImporterSettingString(
    const std::string& value,
    FbxReaderSelection fallback = FbxReaderSelection::Autodesk);
bool BoolFromImporterSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    bool fallback);
double DoubleFromImporterSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    double fallback);
uint32_t UIntFromImporterSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    uint32_t fallback);
std::string StringFromImporterSettings(
    const std::map<std::string, std::string>& settings,
    const char* key,
    std::string fallback = {});
ModelTextureResolutionSettings LoadModelTextureResolutionSettings(
    const NLS::Core::Assets::AssetMeta& meta);
void StoreModelTextureResolutionSettings(
    NLS::Core::Assets::AssetMeta& meta,
    const ModelTextureResolutionSettings& settings);
std::string MakeModelTextureRemapSettingKey(const std::string& stableSourceKey);
std::vector<ModelTextureExplicitRemapSetting> LoadModelTextureRemapSettings(
    const NLS::Core::Assets::AssetMeta& meta,
    NLS::Core::Assets::AssetDiagnostics* diagnostics = nullptr);
void StoreModelTextureRemapSetting(
    NLS::Core::Assets::AssetMeta& meta,
    const ModelTextureExplicitRemapSetting& remap);
void ClearModelTextureRemapSetting(
    NLS::Core::Assets::AssetMeta& meta,
    const std::string& stableSourceKey);
std::string ComputeModelTextureSettingsFingerprint(const NLS::Core::Assets::AssetMeta& meta);
ModelImporterSettings ModelImporterSettingsFromSerialized(
    const std::map<std::string, std::string>& settings);
NLS::meta::Type GetImporterSettingType(const std::string& name);
std::string ImporterEnumToSettingString(const NLS::meta::Variant& value);
NLS::meta::Variant ImporterEnumFromSettingString(const NLS::meta::Type& type, std::string_view value);
NLS::Render::Assets::SceneImportSettings ToSceneImportSettings(const ModelImporterSettings& settings);
NLS::Render::Assets::SceneImportSettings ToSceneImportSettings(
    const std::map<std::string, std::string>& settings);
}
