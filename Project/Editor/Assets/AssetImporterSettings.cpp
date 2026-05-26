#include "Assets/AssetImporterSettings.h"

#include "Debug/Assertion.h"

#include <utility>

namespace NLS::Editor::Assets
{
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
    return result;
}

NLS::Render::Assets::SceneImportSettings ToSceneImportSettings(
    const std::map<std::string, std::string>& settings)
{
    return ToSceneImportSettings(ModelImporterSettingsFromSerialized(settings));
}
}
