#include "Assets/AssetMeta.h"

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
const char* ToString(const AssetType type)
{
    switch (type)
    {
    case AssetType::ModelScene: return "model-scene";
    case AssetType::Texture: return "texture";
    case AssetType::Shader: return "shader";
    case AssetType::Material: return "material";
    case AssetType::Audio: return "audio";
    case AssetType::Scene: return "scene";
    case AssetType::Script: return "script";
    case AssetType::Prefab: return "prefab";
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
    case NLS::Utils::PathParser::EFileType::UNKNOWN:
    case NLS::Utils::PathParser::EFileType::COUNT:
        break;
    }
    return AssetType::Unknown;
}

std::string InferImporterId(const AssetType type)
{
    switch (type)
    {
    case AssetType::ModelScene: return "scene-model";
    case AssetType::Texture: return "texture";
    case AssetType::Shader: return "shader";
    case AssetType::Material: return "material";
    case AssetType::Audio: return "audio";
    case AssetType::Scene: return "scene";
    case AssetType::Script: return "script";
    case AssetType::Prefab: return "prefab";
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
        return 9u;
    case AssetType::Texture:
    case AssetType::Shader:
    case AssetType::Material:
    case AssetType::Audio:
    case AssetType::Scene:
    case AssetType::Script:
    case AssetType::Prefab:
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
