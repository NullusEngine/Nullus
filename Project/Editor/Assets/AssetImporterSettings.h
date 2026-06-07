#pragma once

#include "Rendering/Assets/ImportedScene.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
inline constexpr const char* kModelFbxIgnoreTexturedNeutralDiffuseTintSetting =
    "MODEL_FBX_IGNORE_TEXTURED_NEUTRAL_DIFFUSE_TINT";

enum class FbxReaderSelection
{
    Autodesk,
    Assimp,
    AutodeskWithAssimpFallback
};

struct ModelImporterSettings
{
    double globalScale = 1.0;
    std::string axisConversion;
    std::string unitConversion;
    std::string hierarchyPolicy = "preserve";
    FbxReaderSelection fbxReaderSelection = FbxReaderSelection::Assimp;
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
    bool mipmapEnabled = false;
    std::string wrapMode = "repeat";
    std::string filterMode = "bilinear";
    uint32_t maxTextureSize = 2048u;
    std::string resizePolicy = "keep";
    std::string compressionIntent = "default";
    std::string explicitFormat;
    std::vector<TexturePlatformOverride> platformOverrides;
};

std::string BoolToImporterSettingString(bool value);
std::string FbxReaderSelectionToImporterSettingString(FbxReaderSelection value);
FbxReaderSelection FbxReaderSelectionFromImporterSettingString(
    const std::string& value,
    FbxReaderSelection fallback = FbxReaderSelection::Assimp);
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
ModelImporterSettings ModelImporterSettingsFromSerialized(
    const std::map<std::string, std::string>& settings);
NLS::Render::Assets::SceneImportSettings ToSceneImportSettings(const ModelImporterSettings& settings);
NLS::Render::Assets::SceneImportSettings ToSceneImportSettings(
    const std::map<std::string, std::string>& settings);
}
