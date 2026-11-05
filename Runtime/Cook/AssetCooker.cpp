#include "CookTypes.h"
#include "ECSCooker.h"
#include <Engine/ECS/ECSAssets.h>
#include <Engine/Assets/PhysicsAssetCompiler.h>
#include <AnimationCompiler/AnimationSourceCompiler.h>
#include <Core/Inputs/InputSerialization.h>

#include "ModelCooker.h"
#include "ShaderCooker.h"
#include "TextureEncoder/DirectXTexCookEncoder.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Debug/Logger.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Serialize/ObjectGraphReader.h"
#include "Platform/Process/Process.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Assets/SpriteArtifact.h"
#include "Assets/SpriteSheet.h"
#include "Rendering/Assets/DistanceFieldBuilder.h"
#include "Rendering/Assets/DistanceFieldBuildService.h"
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/Assets/TextureBuildSettings.h"
#include "Rendering/Assets/TextureMipGenerator.h"
#include "Rendering/Resources/MaterialSerialization.h"
#include "Serialize/PPtr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

namespace NLS::Cook
{
namespace
{
using NLS::Core::Assets::ArtifactDatabase;
using NLS::Core::Assets::ArtifactDatabaseRecord;
using NLS::Core::Assets::ArtifactManifest;
using NLS::Core::Assets::ArtifactPayload;
using NLS::Core::Assets::ArtifactRecordStatus;
using NLS::Core::Assets::ArtifactType;
using NLS::Core::Assets::ArtifactWriteRequest;
using NLS::Core::Assets::ArtifactWriter;
using NLS::Core::Assets::AssetDependencyKind;
using NLS::Core::Assets::AssetDependencyRecord;
using NLS::Core::Assets::AssetId;
using NLS::Core::Assets::AssetMeta;
using NLS::Core::Assets::AssetType;

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool IsCancelled(const CookOptions& options)
{
    return options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed);
}

std::filesystem::path NormalizeRelative(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& path)
{
    std::error_code error;
    const auto relative = std::filesystem::relative(path, projectRoot, error);
    if (error)
        return path.filename();
    return relative;
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto end = input.tellg();
    if (end < 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

std::string ReadAllText(const std::filesystem::path& path)
{
    const auto bytes = ReadAllBytes(path);
    return std::string(bytes.begin(), bytes.end());
}

std::string HashBytes(const std::vector<uint8_t>& bytes)
{
    return NLS::Core::Assets::ToHexString(
        NLS::Core::Assets::ComputeSha256(bytes.data(), bytes.size()));
}

std::string HashFile(const std::filesystem::path& path)
{
    return HashBytes(ReadAllBytes(path));
}

bool ParseBoolSetting(
    const std::map<std::string, std::string>& settings,
    const char* key,
    const bool fallback)
{
    const auto found = settings.find(key);
    if (found == settings.end())
        return fallback;
    const auto value = ToLower(found->second);
    return value == "true" || value == "1" || value == "yes";
}

uint32_t ParseUIntSetting(
    const std::map<std::string, std::string>& settings,
    const char* key,
    const uint32_t fallback)
{
    const auto found = settings.find(key);
    if (found == settings.end())
        return fallback;
    try
    {
        const auto value = std::stoul(found->second);
        return value <= std::numeric_limits<uint32_t>::max()
            ? static_cast<uint32_t>(value)
            : fallback;
    }
    catch (const std::exception&)
    {
        return fallback;
    }
}

struct AssetTypeInfo
{
    ArtifactType artifactType = ArtifactType::Unknown;
    std::string loaderId;
    std::string importerId;
    std::string subAssetKey;
    bool supported = false;
};

std::string StemOf(const std::filesystem::path& path)
{
    return path.stem().generic_string();
}

AssetTypeInfo MakeTypeInfo(const AssetType type, const std::filesystem::path& source)
{
    AssetTypeInfo info;
    const auto stem = StemOf(source);
    if(type==AssetType::AnimationClip||type==AssetType::AnimationGraph||type==AssetType::BlendSpace||type==AssetType::BoneMask)
    {
        info.artifactType=type==AssetType::AnimationClip?ArtifactType::AnimationClip:type==AssetType::AnimationGraph?ArtifactType::AnimationGraph:type==AssetType::BlendSpace?ArtifactType::BlendSpace:ArtifactType::BoneMask;
        info.loaderId=type==AssetType::AnimationClip?"animation":type==AssetType::AnimationGraph?"animation-graph":type==AssetType::BlendSpace?"blend-space":"bone-mask";
        info.importerId=info.loaderId;info.subAssetKey=info.loaderId+":"+stem;info.supported=true;return info;
    }
    switch (type)
    {
    case AssetType::PhysicsMaterial:
    case AssetType::CollisionMesh:
    case AssetType::PhysicsShape:
        info.artifactType=type==AssetType::PhysicsMaterial?ArtifactType::PhysicsMaterial:
            type==AssetType::CollisionMesh?ArtifactType::CollisionMesh:ArtifactType::PhysicsShape;
        info.loaderId=Core::Assets::ToString(type);
        info.importerId=info.loaderId; info.subAssetKey=info.loaderId+":main"; info.supported=true;
        break;
    case AssetType::ECSScene:
    case AssetType::ECSPrefab:
        info.artifactType = type == AssetType::ECSScene ? ArtifactType::ECSScene : ArtifactType::ECSPrefab;
        info.loaderId = type == AssetType::ECSScene ? "ecs-scene" : "ecs-prefab";
        info.importerId = info.loaderId; info.subAssetKey = info.loaderId + ":main"; info.supported = true;
        break;
    case AssetType::Texture:
        info.artifactType = ArtifactType::Texture;
        info.loaderId = "texture";
        info.importerId = "texture";
        info.subAssetKey = "texture:main";
        info.supported = true;
        break;
    case AssetType::Prefab:
        info.artifactType = ArtifactType::Prefab;
        info.loaderId = "prefab";
        info.importerId = "prefab";
        info.subAssetKey = "prefab:" + stem;
        info.supported = true;
        break;
    case AssetType::Material:
        info.artifactType = ArtifactType::Material;
        info.loaderId = "material";
        info.importerId = "material";
        info.subAssetKey = "material:main";
        info.supported = true;
        break;
    case AssetType::Shader:
        info.artifactType = ArtifactType::Shader;
        info.loaderId = "shader";
        info.importerId = "shader";
        info.subAssetKey = "shader:" + stem;
        info.supported = true;
        break;
    case AssetType::ModelScene:
        info.artifactType = ArtifactType::Prefab;
        info.loaderId = "prefab";
        info.importerId = "model";
        info.subAssetKey = "prefab:" + stem;
        info.supported = true;
        break;
    case AssetType::UIDocument:
        info.artifactType = ArtifactType::UIDocument;
        info.loaderId = "ui-document";
        info.importerId = "ui-document";
        info.subAssetKey = "ui-document:" + stem;
        info.supported = true;
        break;
    case AssetType::InputActions:
    case AssetType::InputProfile:
        info.artifactType = type == AssetType::InputActions ? ArtifactType::InputActions : ArtifactType::InputProfile;
        info.loaderId = type == AssetType::InputActions ? "input-actions" : "input-profile";
        info.importerId = info.loaderId;
        info.subAssetKey = info.loaderId + ":" + stem;
        info.supported = true;
        break;
    case AssetType::UIStyleSheet:
        info.artifactType = ArtifactType::UIStyleSheet;
        info.loaderId = "ui-style-sheet";
        info.importerId = "ui-style-sheet";
        info.subAssetKey = "ui-style-sheet:" + stem;
        info.supported = true;
        break;
    case AssetType::LocalizationTable:
        info.artifactType = ArtifactType::LocalizationTable;
        info.loaderId = "localization";
        info.importerId = "localization";
        info.subAssetKey = "localization:" + stem;
        info.supported = true;
        break;
    default:
        break;
    }
    return info;
}

const ArtifactDatabaseRecord* FindRecordForReference(
    const ArtifactDatabase& database,
    const NLS::Guid& guid,
    const std::string& targetPlatform)
{
    if (!guid.IsValid())
        return nullptr;
    const auto assetId = AssetId(guid);
    const ArtifactDatabaseRecord* primary = nullptr;
    for (const auto* record : database.FindBySource(assetId))
    {
        if (record == nullptr ||
            record->targetPlatform != targetPlatform ||
            record->status != ArtifactRecordStatus::UpToDate ||
            record->artifactPath.empty())
        {
            continue;
        }
        if (record->primarySubAssetKey == record->subAssetKey)
            return record;
        if (primary == nullptr)
            primary = record;
    }
    return primary;
}

// Version 4 retains the exact logical sub-resource in Prefab dependencies.
constexpr const char* kNullusCookPostprocessorVersion = "4";
std::string DistanceFieldCookVersion(const AssetMeta& meta,const std::filesystem::path& projectRoot)
{
    const auto settings=NLS::Render::DistanceField::ReadDistanceFieldBuildSettings(meta.settings,projectRoot);
    return NLS::Render::DistanceField::ComputeDistanceFieldBuildIdentity("cook-settings",settings)+":"+std::to_string(settings.Enabled());
}

bool HasUpToDateCook(
    const ArtifactDatabase& database,
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& artifactRoot,
    const AssetId& assetId,
    const std::string& targetPlatform,
    const std::string& sourceRelative,
    const std::string& sourceHash,
    const std::string& metaRelative,
    const std::string& metaHash,
    const uint32_t maxTextureSize,
    const uint32_t importerVersion,
    const AssetType sourceType,const AssetMeta& sourceMeta,bool ignoreDistanceFields=false)
{
    const auto manifest = database.BuildManifestForSource(assetId, targetPlatform);
    if (!manifest.has_value())
        return false;
    if (manifest->importerVersion != importerVersion) return false;
    if(!HasCurrentSpriteCompilerDependency(*manifest,sourceMeta))return false;

    bool sourceMatches = false;
    bool metaMatches = false;
    bool targetMatches = false;
    bool postprocessorMatches = false;
    bool textureCapMatches = false;
    bool physicsMatches = sourceType!=AssetType::PhysicsMaterial && sourceType!=AssetType::CollisionMesh && sourceType!=AssetType::PhysicsShape;
    const bool isModel = NLS::Core::Assets::InferAssetType(sourceRelative) == AssetType::ModelScene;
    bool distanceFieldMatches = !isModel || ignoreDistanceFields;
    const auto distanceFieldVersion = isModel?DistanceFieldCookVersion(sourceMeta,projectRoot):std::string{};
    for (const auto& dependency : manifest->dependencies)
    {
        if(isModel && dependency.kind==AssetDependencyKind::PostprocessorVersion && dependency.value=="nullus-model-import-settings" &&
            dependency.hashOrVersion==NLS::Core::Assets::ComputeModelImportSettingsFingerprint(sourceMeta))metaMatches=true;
        if (dependency.kind==AssetDependencyKind::PostprocessorVersion && dependency.value=="nullus-physics-cooking" &&
            dependency.hashOrVersion==Engine::Assets::PhysicsCookingFingerprint()) physicsMatches=true;
        if (dependency.kind == AssetDependencyKind::PostprocessorVersion &&
            dependency.value == "nullus-cook-distance-field" && dependency.hashOrVersion == distanceFieldVersion)
            distanceFieldMatches = true;
        if (dependency.kind == AssetDependencyKind::SourceFileHash)
        {
            if (dependency.value == sourceRelative && dependency.hashOrVersion == sourceHash)
                sourceMatches = true;
            if (!isModel && dependency.value == metaRelative && dependency.hashOrVersion == metaHash)
                metaMatches = true;
        }
        if (dependency.kind == AssetDependencyKind::BuildTarget &&
            dependency.value == targetPlatform)
        {
            targetMatches = true;
        }
        if (dependency.kind == AssetDependencyKind::PostprocessorVersion &&
            dependency.value == "nullus-cook" &&
            dependency.hashOrVersion == kNullusCookPostprocessorVersion)
        {
            postprocessorMatches = true;
        }
        if (dependency.kind == AssetDependencyKind::PostprocessorVersion &&
            dependency.value == "nullus-cook-texture-max" &&
            dependency.hashOrVersion == std::to_string(maxTextureSize))
        {
            textureCapMatches = true;
        }
    }
    if (!sourceMatches || !metaMatches || !targetMatches || !postprocessorMatches ||
        !textureCapMatches || !distanceFieldMatches || !physicsMatches)
    {
        return false;
    }

    if(isModel && !ignoreDistanceFields && NLS::Render::DistanceField::ReadDistanceFieldBuildSettings(sourceMeta.settings,projectRoot).Enabled())
    {
        namespace df=NLS::Render::DistanceField;
        const auto resolve=[&](const auto& artifact)
        {
            auto path=std::filesystem::path(artifact.artifactPath);if(path.is_absolute())return path;
            return path.generic_string().starts_with("Library/Artifacts/")?artifactRoot/path.lexically_relative("Library/Artifacts"):projectRoot/path;
        };
        const auto* input=manifest->FindSubAsset(df::DistanceFieldSourceKey);
        const auto description=input?df::DeserializeDistanceFieldSource(ReadAllBytes(resolve(*input))):std::nullopt;
        if(!description)return false;
        const auto complete=[&](const std::string& key)
        {
            for(uint32_t mip=0;mip<3;++mip)
            {
                const auto* entry=manifest->FindSubAsset(df::DistanceFieldSubAssetKey(key,mip==2?df::MipCount:mip));
                if(!entry)return false;std::error_code error;
                if(!std::filesystem::is_regular_file(resolve(*entry),error) || std::filesystem::file_size(resolve(*entry),error)<=NLS::Core::Assets::NativeArtifactContainerHeaderSize())return false;
            }
            return true;
        };
        for(const auto& mesh:description->meshes)if(mesh.participates && !complete(mesh.key))return false;
        for(const auto& group:description->groups)if(!complete(group.key))return false;
    }

    for (const auto* record : database.FindBySource(assetId))
    {
        if (record == nullptr ||
            record->targetPlatform != targetPlatform ||
            record->status != ArtifactRecordStatus::UpToDate)
        {
            continue;
        }
        if (record->primarySubAssetKey != record->subAssetKey || record->artifactPath.empty())
            continue;
        auto artifactPath = std::filesystem::path(record->artifactPath);
        if (artifactPath.is_relative())
        {
            const auto portable = artifactPath.generic_string();
            artifactPath = portable.rfind("Library/Artifacts/",0) == 0
                ? artifactRoot / artifactPath.lexically_relative("Library/Artifacts") : projectRoot / artifactPath;
        }
        std::error_code error;
        if (std::filesystem::is_regular_file(artifactPath, error))
            return true;
    }
    return false;
}

bool CanCookModelDistanceFields(const CookOptions& options,const ArtifactDatabase& database,const ArtifactManifest& previous,
    const AssetMeta& meta,const std::string& sourceRelative,const std::string& sourceHash,const std::string& metaRelative,
    const std::string& metaHash,uint32_t importerVersion)
{
    if(!previous.FindSubAsset(NLS::Render::DistanceField::DistanceFieldSourceKey) ||
        !HasUpToDateCook(database,options.projectRoot,options.artifactRoot,meta.id,options.target.artifactPlatform,
            sourceRelative,sourceHash,metaRelative,metaHash,options.maxTextureSize,importerVersion,AssetType::ModelScene,meta,true))return false;
    for(const auto& dependency:previous.dependencies)
        if(dependency.kind==AssetDependencyKind::SourceFileHash && dependency.value!=sourceRelative && dependency.value!=metaRelative)
        {
            const auto path=std::filesystem::path(dependency.value);
            if(HashFile(path.is_absolute()?path:options.projectRoot/path)!=dependency.hashOrVersion)return false;
        }
    for(const auto& artifact:previous.subAssets)
    {
        if(artifact.artifactType==ArtifactType::MeshDistanceField)continue;
        auto path=std::filesystem::path(artifact.artifactPath);
        if(path.is_relative())path=path.generic_string().starts_with("Library/Artifacts/")?
            options.artifactRoot/path.lexically_relative("Library/Artifacts"):options.projectRoot/path;
        std::error_code error;if(!std::filesystem::is_regular_file(path,error))return false;
    }
    return true;
}

void AddCommonDependencies(
    std::vector<AssetDependencyRecord>& dependencies,
    const std::string& sourceRelative,
    const std::string& sourceHash,
    const std::string& metaRelative,
    const std::string& metaHash,
    const AssetTypeInfo& typeInfo,
    const CookTarget& target,
    const uint32_t importerVersion,
    const uint32_t maxTextureSize)
{
    dependencies.push_back({AssetDependencyKind::SourceFileHash, sourceRelative, sourceHash});
    dependencies.push_back({AssetDependencyKind::SourceFileHash, metaRelative, metaHash});
    dependencies.push_back({AssetDependencyKind::BuildTarget, target.artifactPlatform, {}});
    dependencies.push_back({
        AssetDependencyKind::ImporterVersion,
        typeInfo.importerId,
        std::to_string(importerVersion)});
    dependencies.push_back({AssetDependencyKind::PostprocessorVersion, "nullus-cook", kNullusCookPostprocessorVersion});
    dependencies.push_back({
        AssetDependencyKind::PostprocessorVersion,
        "nullus-cook-texture-max",
        std::to_string(maxTextureSize)});
}

std::string InferTextureIntent(const AssetMeta& meta, const std::filesystem::path& source)
{
    if (const auto found = meta.settings.find("TEXTURE_TYPE"); found != meta.settings.end())
    {
        const auto value = ToLower(found->second);
        if (value.find("normal") != std::string::npos)
            return "normal";
        if (value.find("mask") != std::string::npos)
            return "mask";
        if (value.find("ui") != std::string::npos)
            return "ui";
    }
    if (ParseBoolSetting(meta.settings, "TEXTURE_NORMAL_MAP", false))
        return "normal";
    if (ParseBoolSetting(meta.settings, "TEXTURE_MASK", false))
        return "mask";

    const auto stem = ToLower(source.stem().string());
    const auto contains = [&stem](const std::string_view needle)
    {
        return stem.find(needle) != std::string::npos;
    };
    const bool endsWithNormalSuffix =
        stem.size() >= 2u && stem.compare(stem.size() - 2u, 2u, "_n") == 0;
    if (contains("normal") || contains("bump") || endsWithNormalSuffix)
        return "normal";
    if (contains("rough") || contains("metal") || contains("occlusion") ||
        contains("ambientocclusion") || contains("_ao") || contains("mask") ||
        contains("height") || contains("displace") || contains("specular"))
    {
        return "mask";
    }
    return "default";
}

bool CookTexture(
    const CookOptions& options,
    const AssetMeta& meta,
    const std::filesystem::path& source,
    const AssetTypeInfo& typeInfo,
    std::vector<uint8_t>& payload,
    std::vector<AssetDependencyRecord>& dependencies,
    std::string& error, uint32_t& sourceWidth, uint32_t& sourceHeight)
{
    (void)dependencies;
    const auto sourceBytes = ReadAllBytes(source);
    if (sourceBytes.empty())
    {
        error = "texture source is empty or unreadable";
        return false;
    }

    NLS::Render::Assets::TextureMipGeneratorSettings settings;
    const auto intent = InferTextureIntent(meta, source);

    settings.intent = intent == "normal"
        ? NLS::Render::Assets::TextureMipIntent::Normal
        : intent == "mask"
            ? NLS::Render::Assets::TextureMipIntent::Mask
            : intent == "ui"
                ? NLS::Render::Assets::TextureMipIntent::UI
                : NLS::Render::Assets::TextureMipIntent::Color;
    const bool defaultSrgb = intent != "normal" && intent != "mask";
    settings.colorSpace = ParseBoolSetting(meta.settings, "TEXTURE_SRGB", defaultSrgb)
        ? NLS::Render::Assets::TextureArtifactColorSpace::Srgb
        : NLS::Render::Assets::TextureArtifactColorSpace::Linear;
    settings.format = NLS::Render::RHI::TextureFormat::RGBA8;
    settings.mipmapEnabled = ParseBoolSetting(meta.settings, "TEXTURE_MIPMAP_ENABLED", !IsSpriteTexture(meta));

    const uint32_t maxTextureSize = options.maxTextureSize != 0u
        ? options.maxTextureSize
        : ParseUIntSetting(meta.settings, "TEXTURE_MAX_SIZE", 0u);

    auto decodeSettings = settings;
    decodeSettings.mipmapEnabled = false;
    auto texture = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        sourceBytes.data(),
        sourceBytes.size(),
        decodeSettings,
        false);
    if (!texture.has_value())
    {
        error = "texture could not be decoded by the cook decoders (png/jpg/tga/bmp/hdr)";
        return false;
    }
    sourceWidth = texture->width;
    sourceHeight = texture->height;

    if (maxTextureSize > 0u &&
        texture->format == NLS::Render::RHI::TextureFormat::RGBA8 &&
        !texture->mips.empty() &&
        (texture->width > maxTextureSize || texture->height > maxTextureSize))
    {
        const auto scale = static_cast<float>(maxTextureSize) /
            static_cast<float>((std::max)(texture->width, texture->height));
        const uint32_t targetWidth = (std::max)(
            1u,
            static_cast<uint32_t>(std::lround(texture->width * scale)));
        const uint32_t targetHeight = (std::max)(
            1u,
            static_cast<uint32_t>(std::lround(texture->height * scale)));
        auto resized = NLS::Render::Assets::Detail::GenerateNextTextureMip(
            texture->format,
            settings.intent,
            settings.colorSpace,
            texture->mips.front().pixels,
            texture->width,
            texture->height,
            targetWidth,
            targetHeight);
        if (resized.has_value())
        {
            auto mipChain = NLS::Render::Assets::GenerateTextureMipChain(
                targetWidth,
                targetHeight,
                std::move(*resized),
                settings);
            if (mipChain.has_value())
                texture = std::move(*mipChain);
        }
    }
    else if (settings.mipmapEnabled && texture->mips.size() <= 1u)
    {
        auto mipChain = NLS::Render::Assets::GenerateTextureMipChain(
            texture->width,
            texture->height,
            texture->mips.empty() ? std::vector<uint8_t> {} : texture->mips.front().pixels,
            settings);
        if (mipChain.has_value())
            texture = std::move(*mipChain);
    }

    // Choose the platform-compressed format when the cook encoder exists.
    // Falling back to RGBA8 keeps every platform functional.
    bool hasAlpha = false;
    if (!texture->mips.empty() && texture->format == NLS::Render::RHI::TextureFormat::RGBA8)
    {
        const auto& pixels = texture->mips.front().pixels;
        for (size_t alphaIndex = 3u; alphaIndex < pixels.size(); alphaIndex += 4u)
        {
            if (pixels[alphaIndex] != 255u)
            {
                hasAlpha = true;
                break;
            }
        }
    }
    bool compressed = false;
    if (auto encoder = NLS::Cook::CreateDirectXTexCookEncoder(); encoder != nullptr && !IsSpriteTexture(meta))
    {
        NLS::Render::Assets::TextureBuildSettings buildSettings;
        buildSettings.sourceAssetPath = source.generic_string();
        buildSettings.sourceAssetIdentity = meta.id.ToString();
        buildSettings.sourceContentHash = HashBytes(sourceBytes);
        buildSettings.targetPlatform = options.target.artifactPlatform;
        buildSettings.textureIntent = intent;
        buildSettings.mipmapEnabled = settings.mipmapEnabled;
        buildSettings.maxTextureSize = maxTextureSize;
        buildSettings.colorSpace = settings.colorSpace == NLS::Render::Assets::TextureArtifactColorSpace::Srgb
            ? NLS::Render::RHI::TextureColorSpace::SRGB
            : NLS::Render::RHI::TextureColorSpace::Linear;
        buildSettings.encoderId = std::string(encoder->GetId());
        buildSettings.encoderVersion = encoder->GetVersion();
        buildSettings.resolvedFormat = intent == "normal"
            ? NLS::Render::RHI::TextureFormat::BC5
            : hasAlpha
                ? NLS::Render::RHI::TextureFormat::BC7
                : NLS::Render::RHI::TextureFormat::BC1;
        if (!encoder->SupportsFormat(buildSettings.resolvedFormat))
            buildSettings.resolvedFormat = NLS::Render::RHI::TextureFormat::RGBA8;

        if (buildSettings.resolvedFormat != NLS::Render::RHI::TextureFormat::RGBA8)
        {
            NLS::Render::Assets::TextureEncodeRequest encodeRequest;
            encodeRequest.buildSettings = &buildSettings;
            encodeRequest.sourceMips = &*texture;
            auto encodeResult = encoder->Encode(encodeRequest);
            if (encodeResult.succeeded)
            {
                texture = std::move(encodeResult.artifact);
                compressed = true;
            }
            else
            {
                std::string detail;
                for (const auto& diagnostic : encodeResult.diagnostics)
                    detail += " " + diagnostic.message;
                NLS_LOG_WARNING(
                    "Cook texture compression failed for '" + source.string() + "':" + detail +
                    " (falling back to RGBA8)");
                std::fprintf(
                    stderr,
                    "[cook] texture '%s' compression failed:%s\n",
                    source.string().c_str(),
                    detail.c_str());
            }
        }
    }

    if (!compressed)
    {
        texture->targetPlatform = options.target.artifactPlatform;
        texture->encoderId = "rgba8-passthrough";
        texture->encoderVersion = 1;
        texture->buildIdentity =
            "rgba8|srgb=" + std::string(settings.colorSpace == NLS::Render::Assets::TextureArtifactColorSpace::Srgb ? "1" : "0") +
            "|mips=" + std::string(settings.mipmapEnabled ? "1" : "0") +
            "|target=" + options.target.artifactPlatform;
    }
    else
    {
        texture->targetPlatform = options.target.artifactPlatform;
    }
    payload = NLS::Render::Assets::SerializeTextureArtifact(*texture);
    if (payload.empty())
    {
        error = "texture artifact serialization failed";
        return false;
    }
    return true;
}

bool CookMaterial(
    const CookOptions& options,
    const ArtifactDatabase& database,
    const AssetMeta& meta,
    const std::filesystem::path& source,
    std::vector<uint8_t>& payload,
    std::vector<AssetDependencyRecord>& dependencies,
    std::string& error)
{
    const auto bytes = ReadAllBytes(source);
    if (bytes.empty())
    {
        error = "material source is empty or unreadable";
        return false;
    }

    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
        bytes,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    if (!container.has_value() || container->metadata.schemaName != "material")
    {
        error = "material source must be an authoritative native .mat container";
        return false;
    }

    const std::string text(container->payload.begin(), container->payload.end());
    if (text.rfind("shaderLabMaterialVersion=1", 0u) != 0u)
    {
        error = "material source must contain a ShaderLab material payload";
        return false;
    }

    // The runtime loader accepts the ShaderLab payload directly. Record every
    // referenced asset so the packaging closure includes its shader and
    // textures even though the payload keeps resource paths.
    const auto addReferences = [&](std::string_view referencePayload)
    {
        for (const auto& reference : NLS::Render::Resources::CollectSerializedMaterialAssetReferences(referencePayload))
        {
            if (reference.path.empty())
                continue;

            std::filesystem::path referencePath(reference.path);
            if (referencePath.is_absolute())
                continue;
            if (reference.path.front() == ':')
                continue; // engine-bundled resources are delivered with the player

            std::vector<std::filesystem::path> candidates;
            const bool engineReference = IsEngineVirtualAssetPath(reference.path);
            if (engineReference)
            {
                const auto engineRelative = referencePath.lexically_relative("Assets/Engine");
                const auto resourceRoots = NLS::Platform::Process::ResolveInstallResourceRoots();
                candidates.push_back(resourceRoots.engineAssetsRoot / engineRelative);
            }
            else
            {
                candidates.push_back(options.projectRoot / referencePath);
                candidates.push_back(options.assetsRoot / referencePath);
                if (referencePath.generic_string().rfind("Assets/", 0u) == 0u)
                    candidates.push_back(options.projectRoot / referencePath);
            }

            for (const auto& candidate : candidates)
            {
                std::error_code pathError;
                if (!std::filesystem::is_regular_file(candidate, pathError))
                    continue;
                AssetMeta referenceMeta;
                if (engineReference)
                {
                    referenceMeta.id = AssetId(MakeEngineAssetGuid(reference.path));
                    referenceMeta.assetType = NLS::Core::Assets::InferAssetType(candidate);
                    referenceMeta.importerId = "engine-asset";
                    referenceMeta.importerVersion = 1u;
                }
                else
                {
                    const auto loadedMeta = AssetMeta::Load(
                        NLS::Core::Assets::GetAssetMetaPath(candidate));
                    if (!loadedMeta.has_value() || !loadedMeta->id.IsValid())
                        continue;
                    referenceMeta = *loadedMeta;
                }
                if (!referenceMeta.id.IsValid())
                    continue;

                std::string subAssetKey;
                if (const auto* record = FindRecordForReference(
                        database,
                        referenceMeta.id.GetGuid(),
                        options.target.artifactPlatform);
                    record != nullptr)
                {
                    subAssetKey = record->subAssetKey;
                }
                else
                {
                    const auto referenceType = referenceMeta.assetType != NLS::Core::Assets::AssetType::Unknown
                        ? referenceMeta.assetType
                        : NLS::Core::Assets::InferAssetType(candidate);
                    switch (referenceType)
                    {
                    case NLS::Core::Assets::AssetType::Texture: subAssetKey = "texture:main"; break;
                    case NLS::Core::Assets::AssetType::Shader:
                    case NLS::Core::Assets::AssetType::ShaderGraph:
                        subAssetKey = "shader:" + candidate.stem().generic_string();
                        break;
                    case NLS::Core::Assets::AssetType::Material: subAssetKey = "material:main"; break;
                    default: break;
                    }
                }
                if (subAssetKey.empty())
                    continue;
                dependencies.push_back({
                    AssetDependencyKind::ImportedArtifact,
                    referenceMeta.id.ToString(),
                    subAssetKey});
                break;
            }
        }
    };

    addReferences(text);
    // Material inheritance stores the parent asset source inline; scan it too
    // so parent-provided textures and shaders stay in the closure.
    {
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("materialAssetSource=", 0u) == 0u)
            {
                addReferences(NLS::Render::Resources::UnescapeMaterialField(
                    line.substr(std::string("materialAssetSource=").size())));
                break;
            }
        }
    }

    payload.assign(text.begin(), text.end());
    return true;
}

bool CookPrefab(
    const CookOptions& options,
    const ArtifactDatabase& database,
    const AssetMeta& meta,
    const std::filesystem::path& source,
    std::vector<uint8_t>& payload,
    std::vector<AssetDependencyRecord>& dependencies,
    std::string& error)
{
    const auto text = ReadAllText(source);
    if (text.empty())
    {
        error = "prefab source is empty or unreadable";
        return false;
    }

    // Validate the document shape and collect referenced assets. The editor
    // performs full prefab semantics validation; cooking must at least reject
    // malformed documents and keep the artifact equal to the source bytes.
    auto document = NLS::Engine::Serialize::ObjectGraphReader::Read(text);
    if (!document.has_value() || document->format != "Nullus.ObjectGraph.Prefab")
    {
        error = "prefab document is invalid";
        return false;
    }

    for (const auto& reference : NLS::Engine::Assets::CollectPrefabAssetReferences(*document))
    {
        const auto key = NLS::Engine::Assets::ExtractPrefabAssetReferenceSubAssetKeyHint(reference.filePath);
        const ArtifactDatabaseRecord* record = nullptr;
        for (const auto* candidate : database.FindBySource(AssetId(reference.guid)))
        {
            if (!candidate || candidate->targetPlatform != options.target.artifactPlatform ||
                candidate->status != ArtifactRecordStatus::UpToDate || candidate->artifactPath.empty())
                continue;
            if ((!key.empty() && candidate->subAssetKey == key) ||
                (key.empty() && NLS::Engine::Serialize::MakeLocalIdentifierInFile(reference.guid, candidate->subAssetKey) == reference.localIdentifierInFile))
            {
                record = candidate;
                break;
            }
        }
        if (!record && key.empty())
            record = FindRecordForReference(database, reference.guid, options.target.artifactPlatform);
        if (!record && !key.empty())
        {
            error = "prefab dependency was not cooked: " + reference.guid.ToString() + "#" + key;
            return false;
        }
        if (record == nullptr)
            continue;
        dependencies.push_back({
            AssetDependencyKind::ImportedArtifact,
            record->sourceAssetId.ToString(),
            record->subAssetKey});
    }

    payload.assign(text.begin(), text.end());
    return true;
}

} // namespace
size_t CookSummary::ErrorCount() const
{
    return static_cast<size_t>(std::count_if(
        diagnostics.begin(),
        diagnostics.end(),
        [](const CookDiagnostic& diagnostic) { return diagnostic.severity == CookSeverity::Error; }));
}

size_t CookSummary::WarningCount() const
{
    return static_cast<size_t>(std::count_if(
        diagnostics.begin(),
        diagnostics.end(),
        [](const CookDiagnostic& diagnostic) { return diagnostic.severity == CookSeverity::Warning; }));
}

void CookSummary::AddDiagnostic(
    const CookSeverity severity,
    std::string code,
    std::string message,
    std::string path)
{
    diagnostics.push_back({
        severity,
        std::move(code),
        std::move(message),
        std::move(path)});
}

std::string CookSummary::FormatSummary() const
{
    std::ostringstream stream;
    stream << "Cook: total=" << total
           << " cooked=" << cooked
           << " skipped=" << skipped
           << " unsupported=" << unsupported
           << " failed=" << failed << "\n";
    for (const auto& diagnostic : diagnostics)
    {
        const char* severity = diagnostic.severity == CookSeverity::Error ? "error"
            : diagnostic.severity == CookSeverity::Warning ? "warning" : "info";
        stream << "  [" << severity << "] " << diagnostic.code << ": " << diagnostic.message;
        if (!diagnostic.path.empty())
            stream << " (" << diagnostic.path << ")";
        stream << "\n";
    }
    return stream.str();
}

bool AssetCooker::IsSupportedAssetType(const AssetType type)
{
    if(type==AssetType::PhysicsMaterial || type==AssetType::CollisionMesh || type==AssetType::PhysicsShape) return true;
    if (type == AssetType::ECSScene || type == AssetType::ECSPrefab) return true;
    if (type == AssetType::InputActions || type == AssetType::InputProfile) return true;
    if(type==AssetType::AnimationClip||type==AssetType::AnimationGraph||type==AssetType::BlendSpace||type==AssetType::BoneMask)return true;
    switch (type)
    {
    case AssetType::Texture:
    case AssetType::Prefab:
    case AssetType::Material:
    case AssetType::Shader:
    case AssetType::ModelScene:
    case AssetType::UIDocument:
    case AssetType::UIStyleSheet:
    case AssetType::LocalizationTable:
        return true;
    default:
        return false;
    }
}

std::optional<std::string> AssetCooker::FindUpToDatePrimaryArtifact(
    const std::filesystem::path& artifactDatabasePath,
    const AssetId assetId,
    const std::string& targetPlatform)
{
    ArtifactDatabase database;
    if (!database.Load(artifactDatabasePath))
        return std::nullopt;
    for (const auto* record : database.FindBySource(assetId))
    {
        if (record != nullptr &&
            record->targetPlatform == targetPlatform &&
            record->status == ArtifactRecordStatus::UpToDate &&
            record->primarySubAssetKey == record->subAssetKey &&
            !record->artifactPath.empty())
        {
            return record->artifactPath;
        }
    }
    return std::nullopt;
}

CookSummary AssetCooker::CookProject(const CookOptions& options)
{
    std::vector<std::filesystem::path> assets;
    std::error_code error;
    const auto filter = ToLower(options.pathFilter);
    if (std::filesystem::is_directory(options.assetsRoot, error))
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(options.assetsRoot, error))
        {
            if (error)
                break;
            if (!entry.is_regular_file())
                continue;
            const auto extension = ToLower(entry.path().extension().string());
            if (extension == ".meta")
                continue;
            if (!filter.empty() &&
                ToLower(entry.path().generic_string()).find(filter) == std::string::npos)
            {
                continue;
            }
            assets.push_back(entry.path());
        }
    }
    std::sort(assets.begin(), assets.end());
    return CookAssets(options, assets);
}

CookSummary AssetCooker::CookAssets(
    const CookOptions& options,
    const std::vector<std::filesystem::path>& assetPaths)
{
    CookSummary summary;
    summary.total = assetPaths.size();

    if (options.projectRoot.empty() || options.assetsRoot.empty())
    {
        summary.AddDiagnostic(CookSeverity::Error, "cook-invalid-options", "Cook requires a project and Assets root.");
        summary.failed = 1u;
        return summary;
    }
    if (options.artifactDatabasePath.empty())
    {
        summary.AddDiagnostic(CookSeverity::Error, "cook-invalid-options", "Cook requires an ArtifactDB path.");
        summary.failed = 1u;
        return summary;
    }

    const auto artifactRoot = options.artifactRoot.empty()
        ? options.projectRoot / "Library" / "Artifacts"
        : options.artifactRoot;
    const auto stagingRoot = options.stagingRoot.empty()
        ? options.projectRoot / "Library" / "ArtifactStaging" / "Cook"
        : options.stagingRoot;
    std::error_code error;
    std::filesystem::create_directories(artifactRoot, error);
    std::filesystem::create_directories(stagingRoot, error);

    ArtifactDatabase database;
    if (!database.Load(options.artifactDatabasePath))
    {
        // A fresh project has no database yet; the first bake creates it.
        summary.AddDiagnostic(
            CookSeverity::Info,
            "cook-database-created",
            "ArtifactDB did not exist and will be created.",
            options.artifactDatabasePath.string());
    }

    size_t completed = 0u;
    // Cook dependencies in order: textures, shaders, materials, then models
    // and UI. This lets material/model cookers resolve already-published
    // artifacts during a cold build.
    std::vector<std::filesystem::path> ordered = assetPaths;
    const auto cookRank = [](const std::filesystem::path& path)
    {
        const auto meta = AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(path));
        auto type = meta.has_value() ? meta->assetType : AssetType::Unknown;
        if (type == AssetType::Unknown)
            type = NLS::Core::Assets::InferAssetType(path);
        switch (type)
        {
        case AssetType::Texture: return 0;
        case AssetType::Shader: return 1;
        case AssetType::Material: return 2;
        case AssetType::ModelScene: return 3;
        case AssetType::AnimationClip: return 4;
        case AssetType::BlendSpace:
        case AssetType::BoneMask: return 5;
        case AssetType::AnimationGraph: return 6;
        default: return 7;
        }
    };
    std::stable_sort(
        ordered.begin(),
        ordered.end(),
        [&cookRank](const auto& lhs, const auto& rhs)
        {
            return cookRank(lhs) < cookRank(rhs);
        });

    if (!Detail::OrderECSInputs(ordered, summary, options)) return summary;

    for (const auto& source : ordered)
    {
        if (IsCancelled(options))
        {
            summary.AddDiagnostic(CookSeverity::Error, "cook-cancelled", "Cooking was cancelled.");
            summary.failed += 1u;
            break;
        }
        ++completed;
        if (options.progress)
        {
            options.progress(
                NormalizeRelative(options.projectRoot, source).generic_string(),
                completed,
                summary.total);
        }

        CookAssetOutcome outcome;
        outcome.assetPath = NormalizeRelative(options.projectRoot, source).generic_string();

        const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(source);
        std::error_code metaError;
        if (!std::filesystem::is_regular_file(metaPath, metaError))
        {
            summary.AddDiagnostic(
                CookSeverity::Warning,
                "cook-meta-missing",
                "Source asset has no .meta sidecar and cannot be cooked.",
                source.string());
            outcome.unsupported = true;
            summary.unsupported += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        const auto meta = AssetMeta::Load(metaPath);
        if (!meta.has_value() || !meta->id.IsValid())
        {
            summary.AddDiagnostic(
                CookSeverity::Error,
                "cook-meta-invalid",
                "The .meta sidecar is invalid.",
                metaPath.string());
            outcome.error = "invalid meta";
            summary.failed += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        auto assetType = meta->assetType;
        if (assetType == AssetType::Unknown)
            assetType = NLS::Core::Assets::InferAssetType(source);

        if (!IsSupportedAssetType(assetType))
        {
            summary.AddDiagnostic(
                CookSeverity::Warning,
                "cook-type-unsupported",
                "This asset type has no shared cooker yet; the editor must cook it before packaging.",
                source.string());
            outcome.unsupported = true;
            summary.unsupported += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        const auto typeInfo = MakeTypeInfo(assetType, source);
        const auto importerVersion = (std::max)(meta->importerVersion,GetCurrentImporterVersion(assetType));
        if (!typeInfo.supported || typeInfo.artifactType == ArtifactType::Unknown)
        {
            summary.AddDiagnostic(
                CookSeverity::Error,
                "cook-type-dispatch",
                "Asset type is supported but has no artifact mapping.",
                source.string());
            outcome.error = "no artifact mapping";
            summary.failed += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        const auto sourceRelative = NormalizeRelative(options.projectRoot, source).generic_string();
        const auto metaRelative = NormalizeRelative(options.projectRoot, metaPath).generic_string();
        const bool isPhysics=assetType==AssetType::PhysicsMaterial || assetType==AssetType::CollisionMesh || assetType==AssetType::PhysicsShape;
        if(assetType==AssetType::CollisionMesh && !Engine::Assets::IsPhysicsCookingTargetSupported(options.target.artifactPlatform,options.target.architecture))
        {
            const std::string message="Collision cooking requires the requested native platform and CPU architecture; cook on the matching host.";
            summary.AddDiagnostic(CookSeverity::Error,"cook-physics-target-unsupported",message,source.string());
            outcome.error=message; ++summary.failed; summary.assets.push_back(std::move(outcome)); continue;
        }
        std::vector<uint8_t> physicsSource;
        std::string physicsReadError;
        if(isPhysics && !Engine::Assets::CapturePhysicsAssetSource(source,physicsSource,physicsReadError))
        {
            summary.AddDiagnostic(CookSeverity::Error,"cook-physics-source-unreadable",physicsReadError,source.string());
            outcome.error=physicsReadError; ++summary.failed; summary.assets.push_back(std::move(outcome)); continue;
        }
        const auto sourceHash = isPhysics ? HashBytes(physicsSource) : HashFile(source);
        const auto metaHash = HashFile(metaPath);
        const auto expectedDistanceFieldVersion=assetType==AssetType::ModelScene?DistanceFieldCookVersion(*meta,options.projectRoot):std::string{};
        if (sourceHash.empty() || metaHash.empty())
        {
            summary.AddDiagnostic(
                CookSeverity::Error,
                "cook-source-unreadable",
                "The source file or meta file could not be hashed.",
                source.string());
            outcome.error = "source unreadable";
            summary.failed += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        const bool isECS = assetType == AssetType::ECSScene || assetType == AssetType::ECSPrefab;
        const auto ecsManifest = isECS ? database.BuildManifestForSource(meta->id, options.target.artifactPlatform) : std::optional<ArtifactManifest>{};
        if (!options.force && (!isECS || (ecsManifest && Engine::ECSIntegration::AreECSAssetResourcesCurrent(
                *ecsManifest, database, options.target.artifactPlatform, options.projectRoot))) &&
            HasUpToDateCook(
                database,
                options.projectRoot,
                artifactRoot,
                meta->id,
                options.target.artifactPlatform,
                sourceRelative,
                sourceHash,
                metaRelative,
                metaHash,
                options.maxTextureSize,
                importerVersion,
                assetType,*meta))
        {
            outcome.succeeded = true;
            outcome.skipped = true;
            summary.skipped += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        std::vector<uint8_t> payload;
        std::vector<ArtifactPayload> payloads;
        std::vector<AssetDependencyRecord> dependencies;
        AddCommonDependencies(
            dependencies,
            sourceRelative,
            sourceHash,
            metaRelative,
            metaHash,
            typeInfo,
            options.target,
            importerVersion,
            options.maxTextureSize);

        std::string cookError;
        if (assetType == AssetType::ModelScene)
        {
            std::erase_if(dependencies,[&](const auto& d){return d.kind==AssetDependencyKind::SourceFileHash && d.value==metaRelative;});
            dependencies.push_back({AssetDependencyKind::PostprocessorVersion,"nullus-model-import-settings",
                NLS::Core::Assets::ComputeModelImportSettingsFingerprint(*meta)});
        }
        bool cooked = false;
        bool incrementalModel=false;
        std::vector<std::string> modelRemovals;
        auto primarySubAssetKey = typeInfo.subAssetKey;
        uint32_t spriteSourceWidth = 0, spriteSourceHeight = 0;
        switch (assetType)
        {
        case AssetType::PhysicsMaterial:
        case AssetType::CollisionMesh:
        case AssetType::PhysicsShape:
        {
            ArtifactPayload compiled;
            cooked=Engine::Assets::CompilePhysicsAsset(assetType,
                std::string_view(reinterpret_cast<const char*>(physicsSource.data()),physicsSource.size()),options.target.artifactPlatform,compiled,cookError);
            if(cooked)
            {
                payload=std::move(compiled.payload);
                dependencies.insert(dependencies.end(),compiled.dependencies.begin(),compiled.dependencies.end());
            }
            break;
        }
        case AssetType::ECSScene:
        case AssetType::ECSPrefab:
            try
            {
                auto compiled = Engine::ECSIntegration::CompileECSAsset(ReadAllText(source),
                    assetType == AssetType::ECSScene ? ECS::DocumentKind::Scene : ECS::DocumentKind::Prefab,
                    database, options.target.artifactPlatform, options.projectRoot);
                payload = std::move(compiled.payload);
                dependencies.insert(dependencies.end(), compiled.dependencies.begin(), compiled.dependencies.end());
                cooked = true;
            }
            catch (const std::exception& error) { cookError = error.what(); }
            break;
        case AssetType::Texture:
            cooked = CookTexture(options, *meta, source, typeInfo, payload, dependencies, cookError,
                spriteSourceWidth, spriteSourceHeight);
            break;
        case AssetType::Prefab:
            cooked = CookPrefab(options, database, *meta, source, payload, dependencies, cookError);
            break;
        case AssetType::AnimationClip:
        case AssetType::AnimationGraph:
        case AssetType::BlendSpace:
        case AssetType::BoneMask:
        {
            NLS::AnimationCompiler::SourceCompileResult compiled;
            cooked=NLS::AnimationCompiler::CompileSource(options.projectRoot,database,options.target.artifactPlatform,source,ReadAllText(source),compiled,cookError);
            if(cooked){payload=std::move(compiled.payload.payload);dependencies.insert(dependencies.end(),compiled.dependencies.begin(),compiled.dependencies.end());}
            break;
        }
        case AssetType::Material:
            cooked = CookMaterial(options, database, *meta, source, payload, dependencies, cookError);
            break;
        case AssetType::Shader:
        {
            NLS::Cook::Detail::ShaderCookOutput shaderOutput;
            cooked = NLS::Cook::Detail::CookShaderAsset(
                options,
                source,
                *meta,
                shaderOutput,
                cookError);
            if (cooked)
            {
                payloads = std::move(shaderOutput.payloads);
                dependencies.insert(
                    dependencies.end(),
                    shaderOutput.dependencies.begin(),
                    shaderOutput.dependencies.end());
                for (const auto& diagnostic : shaderOutput.diagnostics)
                {
                    summary.AddDiagnostic(
                        diagnostic.severity,
                        diagnostic.code,
                        diagnostic.message,
                        diagnostic.path);
                }
                if (!shaderOutput.primarySubAssetKey.empty())
                    primarySubAssetKey = shaderOutput.primarySubAssetKey;
            }
            break;
        }
        case AssetType::ModelScene:
        {
            NLS::Cook::Detail::ModelCookOutput modelOutput;
            const auto previous=database.BuildManifestForSource(meta->id,options.target.artifactPlatform);
            if(!options.force && previous && CanCookModelDistanceFields(options,database,*previous,*meta,sourceRelative,sourceHash,metaRelative,metaHash,importerVersion))
            {
                cooked=NLS::Cook::Detail::CookModelDistanceFields(options,*meta,*previous,modelOutput,cookError);
                incrementalModel=true;modelRemovals=std::move(modelOutput.removedSubAssets);
                modelOutput.dependencies=previous->dependencies;
                std::erase_if(modelOutput.dependencies,[&](const auto& d)
                {return (d.kind==AssetDependencyKind::PostprocessorVersion && d.value=="nullus-cook-distance-field") ||
                    std::any_of(dependencies.begin(),dependencies.end(),[&](const auto& current){return current.kind==d.kind && current.value==d.value;});});
            }
            else cooked = NLS::Cook::Detail::CookModelAsset(options,source,*meta,database,modelOutput,cookError);
            if (cooked)
            {
                payloads = std::move(modelOutput.payloads);
                dependencies.insert(
                    dependencies.end(),
                    modelOutput.dependencies.begin(),
                    modelOutput.dependencies.end());
                for (const auto& diagnostic : modelOutput.diagnostics)
                {
                    summary.AddDiagnostic(
                        diagnostic.severity,
                        diagnostic.code,
                        diagnostic.message,
                        diagnostic.path);
                }
                if (!modelOutput.primarySubAssetKey.empty())
                    primarySubAssetKey = modelOutput.primarySubAssetKey;
            }
            break;
        }
        case AssetType::InputActions:
        case AssetType::InputProfile:
        {
            std::vector<std::string> diagnostics;
            std::string canonical;
            if (assetType == AssetType::InputActions)
            {
                Input::InputDefinition definition;
                cooked = Core::Inputs::LoadInputDefinition(source, definition, diagnostics);
                if (cooked) canonical = Core::Inputs::SerializeInputDefinition(definition);
            }
            else
            {
                Input::InputProfile profile;
                cooked = Core::Inputs::LoadInputProfile(source, profile, diagnostics);
                if (cooked) canonical = Core::Inputs::SerializeInputProfile(profile);
            }
            if (cooked) payload.assign(canonical.begin(), canonical.end());
            else cookError = diagnostics.empty() ? "Invalid input resource" : diagnostics.front();
            break;
        }
        case AssetType::UIDocument:
        case AssetType::UIStyleSheet:
        case AssetType::LocalizationTable:
        {
            payload = ReadAllBytes(source);
            cooked = !payload.empty();
            if (!cooked)
                cookError = "source file is empty or unreadable";
            break;
        }
        default:
            cookError = "unsupported asset type";
            break;
        }

        if (!cooked)
        {
            summary.AddDiagnostic(
                CookSeverity::Error,
                "cook-failed",
                "Asset cooking failed: " + cookError,
                source.string());
            outcome.error = cookError;
            summary.failed += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        if (assetType != AssetType::ModelScene && assetType != AssetType::Shader)
        {
            ArtifactPayload artifact;
            artifact.subAssetKey = typeInfo.subAssetKey;
            artifact.artifactType = typeInfo.artifactType;
            artifact.loaderId = typeInfo.loaderId;
            artifact.displayName = StemOf(source);
            artifact.relativePath = std::filesystem::path(StemOf(source));
            artifact.payload = std::move(payload);
            artifact.dependencies = dependencies;
            payloads.push_back(std::move(artifact));
        }

        ArtifactWriteRequest request;
        request.sourceAssetId = meta->id;
        request.importerId = meta->importerId.empty() ? typeInfo.importerId : meta->importerId;
        request.importerVersion = importerVersion;
        request.targetPlatform = options.target.artifactPlatform;
        request.primarySubAssetKey = primarySubAssetKey;
        request.dependencies = dependencies;
        request.artifacts = std::move(payloads);
        if (assetType == AssetType::Texture && !AppendSpriteArtifacts(request, *meta,
            spriteSourceWidth, spriteSourceHeight, primarySubAssetKey, StemOf(source), cookError))
        {
            summary.AddDiagnostic(CookSeverity::Error, "cook-sprite-invalid", cookError, source.string());
            outcome.error = cookError;
            summary.failed += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        struct PhysicsPublicationGuard final : NLS::Core::Assets::IArtifactWriteCancellation
        {
            std::function<bool()> invalid;
            bool IsCancellationRequested() const override { return invalid(); }
        } physicsGuard;
        physicsGuard.invalid=[&]
        {
            if(IsCancelled(options)) return true;
            if(incrementalModel)
            {
                const auto current=AssetMeta::Load(metaPath);
                return !current || HashFile(source)!=sourceHash || HashFile(metaPath)!=metaHash ||
                    DistanceFieldCookVersion(*current,options.projectRoot)!=expectedDistanceFieldVersion;
            }
            std::vector<uint8_t> current; std::string readError;
            return !Engine::Assets::CapturePhysicsAssetSource(source,current,readError) || HashBytes(current)!=sourceHash || HashFile(metaPath)!=metaHash;
        };
        if((isPhysics || incrementalModel) && physicsGuard.IsCancellationRequested())
        {
            const std::string message="Physics source/meta revision changed or cooking was cancelled; previous artifacts are retained.";
            summary.AddDiagnostic(CookSeverity::Error,"cook-physics-stale-result",message,source.string());
            outcome.error=message; ++summary.failed; summary.assets.push_back(std::move(outcome)); continue;
        }
        const auto previous = database.BuildManifestForSource(meta->id, options.target.artifactPlatform);
        ArtifactWriter writer(stagingRoot / meta->id.ToString(), artifactRoot);
        auto writeResult = incrementalModel && previous?
            writer.WriteIncrementalAndCommit(request,*previous,modelRemovals,&physicsGuard):
            writer.WriteAndCommit(request,previous.has_value()?&*previous:nullptr,isPhysics?&physicsGuard:nullptr);

        if (!writeResult.committed || !writeResult.diagnostics.empty() || ((isPhysics || incrementalModel) && physicsGuard.IsCancellationRequested()))
        {
            std::string message = "Artifact write failed.";
            for (const auto& diagnostic : writeResult.diagnostics)
            {
                if (!diagnostic.message.empty())
                {
                    message = diagnostic.message;
                    break;
                }
            }
            summary.AddDiagnostic(CookSeverity::Error, "cook-write-failed", message, source.string());
            outcome.error = message;
            summary.failed += 1u;
            summary.assets.push_back(std::move(outcome));
            continue;
        }

        if(assetType==AssetType::ModelScene)
            writeResult.manifest.dependencies.push_back({AssetDependencyKind::PostprocessorVersion,"nullus-cook-distance-field",expectedDistanceFieldVersion});
        database.UpsertManifest(writeResult.manifest, sourceRelative, ArtifactRecordStatus::UpToDate);
        outcome.succeeded = true;
        outcome.subAssetKey = primarySubAssetKey;
        for (const auto& subAsset : writeResult.manifest.subAssets)
        {
            if (subAsset.subAssetKey == primarySubAssetKey)
            {
                outcome.artifactPath = subAsset.artifactPath;
                break;
            }
        }
        summary.cooked += 1u;
        summary.assets.push_back(std::move(outcome));
    }

    if (!database.Save(options.artifactDatabasePath))
    {
        summary.AddDiagnostic(
            CookSeverity::Error,
            "cook-database-save",
            "ArtifactDB could not be saved after cooking: " + database.GetLastError(),
            options.artifactDatabasePath.string());
        summary.failed += 1u;
    }

    return summary;
}
} // namespace NLS::Cook
