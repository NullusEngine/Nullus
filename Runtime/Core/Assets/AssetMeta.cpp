#include "Assets/AssetMeta.h"
#include "Assets/ArtifactManifest.h"

#include "Utils/PathParser.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace
{
constexpr const char* kGuidKey = "GUID";
constexpr const char* kImporterIdKey = "IMPORTER_ID";
constexpr const char* kImporterVersionKey = "IMPORTER_VERSION";
constexpr const char* kAssetTypeKey = "ASSET_TYPE";

std::string Trim(std::string value)
{
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin)))
        ++begin;

    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))))
        --end;

    return std::string(begin, end);
}

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

}

namespace NLS::Core::Assets
{
std::string ComputeModelImportSettingsFingerprint(const AssetMeta& meta)
{
    std::string value=meta.id.ToString()+"\n"+meta.importerId+"\n"+std::to_string(meta.importerVersion)+"\n"+std::to_string(static_cast<int>(meta.assetType));
    for(const auto& [key,setting]:meta.settings)
        if(!key.starts_with("DISTANCE_FIELD_") && key!="NULLUS_IMPORTER_DIRTY")value+="\n"+std::to_string(key.size())+":"+key+"="+std::to_string(setting.size())+":"+setting;
    return "model-import:"+ToHexString(ComputeSha256(reinterpret_cast<const uint8_t*>(value.data()),value.size()));
}
const char* ToString(const AssetType type)
{
    switch (type)
    {
    case AssetType::ModelScene: return "model-scene";
    case AssetType::Texture: return "texture";
    case AssetType::Shader: return "shader";
    case AssetType::ShaderGraph: return "shader-graph";
    case AssetType::ShaderFunction: return "shader-function";
    case AssetType::Material: return "material";
    case AssetType::Audio: return "audio";
    case AssetType::Scene: return "scene";
    case AssetType::Script: return "script";
    case AssetType::Prefab: return "prefab";
    case AssetType::UIDocument: return "ui-document";
    case AssetType::UIStyleSheet: return "ui-style-sheet";
    case AssetType::Font: return "font";
    case AssetType::LocalizationTable: return "localization";
    case AssetType::VfxSystem: return "vfx-system";
    case AssetType::VfxScript: return "vfx-script";
    case AssetType::VfxEmitter: return "vfx-emitter";
    case AssetType::AnimationClip: return "animation-clip";
    case AssetType::AnimationGraph: return "animation-graph";
    case AssetType::BlendSpace: return "blend-space";
    case AssetType::BoneMask: return "bone-mask";
    case AssetType::InputActions: return "input-actions";
    case AssetType::InputProfile: return "input-profile";
    case AssetType::ECSScene: return "ecs-scene";
    case AssetType::ECSPrefab: return "ecs-prefab";
    case AssetType::PhysicsMaterial: return "physics-material";
    case AssetType::CollisionMesh: return "collision-mesh";
    case AssetType::PhysicsShape: return "physics-shape";
    case AssetType::Unknown:
    case AssetType::Count:
        break;
    }
    return "unknown";
}

AssetType AssetTypeFromString(const std::string& text)
{
    const auto lowered = ToLower(text);
    if (lowered == "model-scene")
        return AssetType::ModelScene;
    if (lowered == "texture")
        return AssetType::Texture;
    if (lowered == "shader")
        return AssetType::Shader;
    if(lowered=="shader-graph")return AssetType::ShaderGraph;
    if(lowered=="shader-function")return AssetType::ShaderFunction;
    if (lowered == "material")
        return AssetType::Material;
    if (lowered == "audio")
        return AssetType::Audio;
    if (lowered == "scene")
        return AssetType::Scene;
    if (lowered == "script")
        return AssetType::Script;
    if (lowered == "prefab")
        return AssetType::Prefab;
    if (lowered == "ui-document")
        return AssetType::UIDocument;
    if (lowered == "ui-style-sheet")
        return AssetType::UIStyleSheet;
    if (lowered == "font")
        return AssetType::Font;
    if (lowered == "localization")
        return AssetType::LocalizationTable;
    if (lowered == "vfx-system") return AssetType::VfxSystem;
    if (lowered == "vfx-script") return AssetType::VfxScript;
    if (lowered == "vfx-emitter") return AssetType::VfxEmitter;
    if (lowered == "animation-clip") return AssetType::AnimationClip;
    if (lowered == "animation-graph") return AssetType::AnimationGraph;
    if (lowered == "blend-space") return AssetType::BlendSpace;
    if (lowered == "bone-mask") return AssetType::BoneMask;
    if (lowered == "input-actions") return AssetType::InputActions;
    if (lowered == "input-profile") return AssetType::InputProfile;
    if (lowered == "ecs-scene") return AssetType::ECSScene;
    if (lowered == "ecs-prefab") return AssetType::ECSPrefab;
    if (lowered == "physics-material") return AssetType::PhysicsMaterial;
    if (lowered == "collision-mesh") return AssetType::CollisionMesh;
    if (lowered == "physics-shape") return AssetType::PhysicsShape;
    return AssetType::Unknown;
}

std::filesystem::path GetAssetMetaPath(const std::filesystem::path& assetPath)
{
    auto result = assetPath;
    result += ".meta";
    return result;
}

AssetType InferAssetType(const std::filesystem::path& assetPath)
{
    const auto extension=ToLower(assetPath.extension().generic_string());
    if (extension == ".physmat") return AssetType::PhysicsMaterial;
    if (extension == ".collisionmesh") return AssetType::CollisionMesh;
    if (extension == ".physshape") return AssetType::PhysicsShape;
    if (extension == ".inputactions") return AssetType::InputActions;
    if (extension == ".inputprofile") return AssetType::InputProfile;
    if (extension == ".ecsscene") return AssetType::ECSScene;
    if (extension == ".ecsprefab") return AssetType::ECSPrefab;
    if(extension==".vfxscript")return AssetType::VfxScript;
    if(extension==".vfxemitter")return AssetType::VfxEmitter;
    if(extension==".shadergraph")return AssetType::ShaderGraph;
    if(extension==".shaderfunc")return AssetType::ShaderFunction;
    if(extension==".animclip")return AssetType::AnimationClip;
    if(extension==".animgraph")return AssetType::AnimationGraph;
    if(extension==".blendspace")return AssetType::BlendSpace;
    if(extension==".bonemask")return AssetType::BoneMask;
    switch (NLS::Utils::PathParser::GetFileType(assetPath.generic_string()))
    {
    case NLS::Utils::PathParser::EFileType::MODEL:
        return AssetType::ModelScene;
    case NLS::Utils::PathParser::EFileType::TEXTURE:
        return AssetType::Texture;
    case NLS::Utils::PathParser::EFileType::SHADER:
        return AssetType::Shader;
    case NLS::Utils::PathParser::EFileType::MATERIAL:
        return AssetType::Material;
    case NLS::Utils::PathParser::EFileType::SOUND:
        return AssetType::Audio;
    case NLS::Utils::PathParser::EFileType::SCENE:
        return AssetType::Scene;
    case NLS::Utils::PathParser::EFileType::SCRIPT:
        return AssetType::Script;
    case NLS::Utils::PathParser::EFileType::PREFAB:
        return AssetType::Prefab;
    case NLS::Utils::PathParser::EFileType::FONT:
        return AssetType::Font;
    case NLS::Utils::PathParser::EFileType::UI_DOCUMENT:
        return AssetType::UIDocument;
    case NLS::Utils::PathParser::EFileType::UI_STYLE:
        return AssetType::UIStyleSheet;
    case NLS::Utils::PathParser::EFileType::LOCALIZATION:
        return AssetType::LocalizationTable;
    case NLS::Utils::PathParser::EFileType::VFX:
        return AssetType::VfxSystem;
    case NLS::Utils::PathParser::EFileType::UNKNOWN:
    case NLS::Utils::PathParser::EFileType::COUNT:
        break;
    }
    return AssetType::Unknown;
}

std::string InferImporterId(const AssetType type)
{
    if (type == AssetType::PhysicsMaterial || type == AssetType::CollisionMesh || type == AssetType::PhysicsShape) return ToString(type);
    if (type == AssetType::InputActions || type == AssetType::InputProfile) return ToString(type);
    if (type == AssetType::ECSScene || type == AssetType::ECSPrefab) return ToString(type);
    if(type==AssetType::AnimationClip||type==AssetType::AnimationGraph||type==AssetType::BlendSpace||type==AssetType::BoneMask)return ToString(type);
    switch (type)
    {
    case AssetType::ModelScene: return "scene-model";
    case AssetType::Texture: return "texture";
    case AssetType::Shader: return "shader";
    case AssetType::ShaderGraph:return "material-graph";
    case AssetType::ShaderFunction:return "material-function";
    case AssetType::Material: return "material";
    case AssetType::Audio: return "audio";
    case AssetType::Scene: return "scene";
    case AssetType::Script: return "script";
    case AssetType::Prefab: return "prefab";
    case AssetType::UIDocument: return "ui-document";
    case AssetType::UIStyleSheet: return "ui-style-sheet";
    case AssetType::Font: return "font";
    case AssetType::LocalizationTable: return "localization";
    case AssetType::VfxSystem: return "vfx-system";
    case AssetType::VfxScript: return "vfx-script";
    case AssetType::VfxEmitter: return "vfx-emitter";
    case AssetType::Unknown:
    case AssetType::Count:
        break;
    }
    return "unknown";
}

uint32_t GetCurrentImporterVersion(const AssetType type)
{
    switch (type)
    {
    case AssetType::ModelScene:
        return 22u;
    case AssetType::Shader:
        // SceneDebug passes and ParticleSprite material ABI 8.
        return 6u;
    case AssetType::ShaderGraph:
        return 2u;
    case AssetType::ShaderFunction:
        return 1u;
    case AssetType::VfxSystem:
        return 18u;
    case AssetType::VfxScript:
    case AssetType::VfxEmitter:
        return 1u;
    case AssetType::Material:
        return 3u;
    case AssetType::Texture:
    case AssetType::Audio:
    case AssetType::Scene:
    case AssetType::Script:
    case AssetType::Prefab:
    case AssetType::UIDocument:
    case AssetType::UIStyleSheet:
    case AssetType::Font:
    case AssetType::LocalizationTable:
    case AssetType::ECSScene:
    case AssetType::ECSPrefab:
    case AssetType::Unknown:
    case AssetType::Count:
        return 1u;
    }
    return 1u;
}

std::optional<AssetMeta> AssetMeta::Load(const std::filesystem::path& metaPath)
{
    std::ifstream input(metaPath);
    if (!input)
        return std::nullopt;

    AssetMeta meta;
    std::string line;
    while (std::getline(input, line))
    {
        const auto equals = line.find('=');
        if (equals == std::string::npos || equals == 0u)
            continue;

        auto key = Trim(line.substr(0u, equals));
        auto value = Trim(line.substr(equals + 1u));
        if (key.empty())
            continue;

        if (key == kGuidKey)
        {
            if (auto parsed = NLS::Guid::TryParse(value); parsed.has_value())
                meta.id = AssetId(*parsed);
        }
        else if (key == kImporterIdKey)
        {
            meta.importerId = value.empty() ? "unknown" : value;
        }
        else if (key == kImporterVersionKey)
        {
            try
            {
                meta.importerVersion = static_cast<uint32_t>(std::stoul(value));
            }
            catch (...)
            {
                meta.importerVersion = 1u;
            }
        }
        else if (key == kAssetTypeKey)
        {
            meta.assetType = AssetTypeFromString(value);
        }
        else
        {
            meta.settings[key] = value;
        }
    }

    return meta;
}

AssetMeta AssetMeta::CreateForAsset(const std::filesystem::path& assetPath)
{
    AssetMeta meta;
    meta.id = AssetId::New();
    meta.assetType = InferAssetType(assetPath);
    meta.importerId = InferImporterId(meta.assetType);
    meta.importerVersion = GetCurrentImporterVersion(meta.assetType);
    // Folder-independent project defaults apply only when creating metadata.
    // Existing assets retain their authored settings on every reimport.
    if (meta.assetType == AssetType::Texture)
    {
        std::error_code error;
        auto directory = std::filesystem::absolute(assetPath, error).parent_path();
        for (size_t depth = 0; !error && !directory.empty() && depth < 64; ++depth)
        {
            if (directory.filename() == "Assets")
            {
                std::ifstream defaults(directory / "ImportDefaults.ini");
                std::string line;
                while (std::getline(defaults, line))
                {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    const auto separator = line.find('=');
                    if (separator == std::string::npos) continue;
                    const auto key = line.substr(0, separator);
                    if (key == "TEXTURE_TYPE" || key == "SPRITE_PIXELS_PER_UNIT" ||
                        key == "TEXTURE_FILTER_MODE" || key == "TEXTURE_MIPMAP_ENABLED")
                        meta.settings[key] = line.substr(separator + 1);
                }
                if (const auto type = meta.settings.find("TEXTURE_TYPE"); type != meta.settings.end() && type->second == "sprite")
                    meta.settings["SPRITE_ID"] = NLS::Guid::New().ToString();
                break;
            }
            const auto parent = directory.parent_path();
            if (parent == directory) break;
            directory = parent;
        }
    }
    return meta;
}

bool AssetMeta::Save(const std::filesystem::path& metaPath) const
{
    std::filesystem::create_directories(metaPath.parent_path());

    std::ofstream output(metaPath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output << kGuidKey << "=" << id.ToString() << '\n';
    output << kImporterIdKey << "=" << importerId << '\n';
    output << kImporterVersionKey << "=" << importerVersion << '\n';
    output << kAssetTypeKey << "=" << ToString(assetType) << '\n';

    for (const auto& [key, value] : settings)
        output << key << "=" << value << '\n';

    output.close();
    return static_cast<bool>(output);
}
}
