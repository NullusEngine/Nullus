#include "ModelCooker.h"

#include "Engine/Assets/ModelPrefabBuilder.h"
#include "Engine/Serialize/ObjectGraphWriter.h"
#include "Rendering/Assets/MaterialConversion.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/AnimationArtifacts.h"
#include "Rendering/Assets/SceneModelReader.h"
#include "Rendering/Assets/DistanceFieldBuilder.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Assets/TextureMipGenerator.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/Resources/Parsers/AssimpParser.h"
#include "Rendering/Resources/Parsers/EModelParserFlags.h"
#include "Rendering/Resources/Parsers/IModelParser.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace NLS::Cook::Detail
{
namespace
{
using NLS::Core::Assets::ArtifactDatabase;
using NLS::Core::Assets::ArtifactManifest;
using NLS::Core::Assets::ArtifactPayload;
using NLS::Core::Assets::ArtifactRecordStatus;
using NLS::Core::Assets::ArtifactType;
using NLS::Core::Assets::AssetDependencyKind;
using NLS::Core::Assets::AssetDependencyRecord;
using NLS::Core::Assets::AssetId;
using NLS::Core::Assets::AssetMeta;
using NLS::Render::Assets::GeneratedSceneSubAsset;
using NLS::Render::Assets::ImportedScene;
using NLS::Render::Assets::ImportedSceneNamedRecord;
using NLS::Render::Assets::ImportedSceneSubAssetType;
using NLS::Render::Assets::MaterialConversionContext;
using NLS::Render::Assets::MaterialSourceModel;
using NLS::Render::Assets::SceneModelSourceFormat;
using NLS::Render::Resources::Parsers::EModelParserFlags;
using NLS::Render::Resources::Parsers::ParsedMeshData;

std::string ToLower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::optional<SceneModelSourceFormat> FormatForExtension(const std::filesystem::path& path)
{
    const auto extension = ToLower(path.extension().string());
    if (extension == ".gltf" || extension == ".glb")
        return SceneModelSourceFormat::Gltf;
    if (extension == ".fbx")
        return SceneModelSourceFormat::Fbx;
    if (extension == ".obj")
        return SceneModelSourceFormat::Obj;
    return std::nullopt;
}

MaterialSourceModel SourceModelForFormat(const SceneModelSourceFormat format)
{
    switch (format)
    {
    case SceneModelSourceFormat::Gltf: return MaterialSourceModel::GltfPbrMetallicRoughness;
    case SceneModelSourceFormat::Fbx: return MaterialSourceModel::FbxParserMaterial;
    case SceneModelSourceFormat::Obj: return MaterialSourceModel::ObjMtl;
    }
    return MaterialSourceModel::GltfPbrMetallicRoughness;
}

const NLS::Core::Assets::ArtifactDatabaseRecord* FindTextureRecord(
    const ArtifactDatabase& database,
    const AssetId& assetId,
    const std::string& targetPlatform)
{
    const NLS::Core::Assets::ArtifactDatabaseRecord* primary = nullptr;
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

bool IsEmbeddedTexture(const ImportedSceneNamedRecord& texture)
{
    return texture.embedded ||
        texture.uri.empty() ||
        texture.uri.rfind("data:", 0u) == 0u;
}

std::optional<std::filesystem::path> ResolveTextureSource(
    const CookOptions& options,
    const std::filesystem::path& modelSource,
    const std::string& uri)
{
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path uriPath(uri);
    if (uriPath.is_absolute())
    {
        candidates.push_back(uriPath);
    }
    else
    {
        candidates.push_back(modelSource.parent_path() / uriPath);
        candidates.push_back(options.assetsRoot / uriPath);
        candidates.push_back(options.projectRoot / uriPath);
    }
    std::error_code error;
    for (const auto& candidate : candidates)
    {
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate.lexically_normal();
    }
    return std::nullopt;
}

const ParsedMeshData* FindParsedMesh(
    const std::unordered_map<std::string, std::vector<const ParsedMeshData*>>& meshesByKey,
    const std::vector<ParsedMeshData>& meshes,
    const std::string& meshKey)
{
    if (const auto found = meshesByKey.find(meshKey); found != meshesByKey.end() && !found->second.empty())
        return found->second.front();

    if (const auto primitive = NLS::Render::Assets::ParsePrimitiveMeshSourceKey(meshKey);
        primitive.has_value())
    {
        if (const auto found = meshesByKey.find(primitive->first); found != meshesByKey.end())
        {
            if (primitive->second < found->second.size())
                return found->second[primitive->second];
            if (!found->second.empty())
                return found->second.front();
        }
    }

    for (const auto& mesh : meshes)
    {
        if (mesh.sourceKey == meshKey)
            return &mesh;
    }
    if (meshes.size() == 1u)
        return &meshes.front();
    return nullptr;
}

int Base64Value(const char character)
{
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
}

bool DecodeBase64(const std::string_view text, std::vector<uint8_t>& bytes)
{
    bytes.clear();
    bytes.reserve(text.size() * 3u / 4u);
    uint32_t accumulator = 0u;
    uint32_t bits = 0u;
    for (const auto character : text)
    {
        if (character == '=')
            break;
        const auto value = Base64Value(character);
        if (value < 0)
        {
            if (std::isspace(static_cast<unsigned char>(character)))
                continue;
            return false;
        }
        accumulator = (accumulator << 6u) | static_cast<uint32_t>(value);
        bits += 6u;
        if (bits >= 8u)
        {
            bits -= 8u;
            bytes.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFFu));
        }
    }
    return !bytes.empty();
}

bool DecodeEmbeddedDataUri(
    const std::string_view uri,
    std::vector<uint8_t>& bytes,
    std::string& format)
{
    if (uri.rfind("data:", 0u) != 0u)
        return false;
    const auto separator = uri.find(";base64,");
    if (separator == std::string_view::npos)
        return false;
    const auto media = uri.substr(5u, separator - 5u);
    const auto slash = media.find('/');
    format = slash == std::string_view::npos
        ? std::string(media)
        : std::string(media.substr(slash + 1u));
    return DecodeBase64(uri.substr(separator + 8u), bytes);
}

std::string EmbeddedTextureIntent(
    const ImportedScene& scene,
    const std::string& textureKey)
{
    for (const auto& material : scene.materials)
    {
        for (const auto& channel : material.materialChannels)
        {
            if (channel.textureKey != textureKey)
                continue;
            const auto name = ToLower(channel.name);
            if (name.find("normal") != std::string::npos ||
                name.find("bump") != std::string::npos)
            {
                return "normal";
            }
            if (name.find("rough") != std::string::npos ||
                name.find("metal") != std::string::npos ||
                name.find("occlusion") != std::string::npos ||
                name.find("mask") != std::string::npos)
            {
                return "mask";
            }
        }
    }
    return "default";
}

std::optional<std::filesystem::path> CookEmbeddedTexture(
    const CookOptions& options,
    const std::filesystem::path& modelSource,
    const AssetMeta& modelMeta,
    ArtifactDatabase& database,
    const ImportedScene& scene,
    const ImportedSceneNamedRecord& texture,
    std::string& error)
{
    std::vector<uint8_t> encodedBytes = texture.embeddedBytes;
    std::string format = texture.embeddedFormat;
    if (encodedBytes.empty() && texture.uri.rfind("data:", 0u) == 0u)
    {
        if (!DecodeEmbeddedDataUri(texture.uri, encodedBytes, format))
        {
            error = "embedded texture data URI could not be decoded";
            return std::nullopt;
        }
    }
    if (encodedBytes.empty())
    {
        error = "embedded texture has no readable payload";
        return std::nullopt;
    }

    NLS::Render::Assets::TextureMipGeneratorSettings settings;
    const auto intent = EmbeddedTextureIntent(scene, texture.sourceKey);
    settings.intent = intent == "normal"
        ? NLS::Render::Assets::TextureMipIntent::Normal
        : intent == "mask"
            ? NLS::Render::Assets::TextureMipIntent::Mask
            : NLS::Render::Assets::TextureMipIntent::Color;
    settings.colorSpace = (intent == "normal" || intent == "mask")
        ? NLS::Render::Assets::TextureArtifactColorSpace::Linear
        : NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    settings.format = NLS::Render::RHI::TextureFormat::RGBA8;
    settings.mipmapEnabled = true;

    std::optional<NLS::Render::Assets::TextureArtifactData> artifact;
    if (texture.embeddedCompressed)
    {
        artifact = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
            encodedBytes.data(),
            encodedBytes.size(),
            settings,
            false);
    }
    else if (texture.embeddedWidth > 0u && texture.embeddedHeight > 0u)
    {
        artifact = NLS::Render::Assets::GenerateTextureMipChain(
            texture.embeddedWidth,
            texture.embeddedHeight,
            std::move(encodedBytes),
            settings);
    }
    if (!artifact.has_value())
    {
        error = "embedded texture could not be decoded (" + format + ")";
        return std::nullopt;
    }

    artifact->targetPlatform = options.target.artifactPlatform;
    artifact->encoderId = "embedded-rgba8";
    artifact->encoderVersion = 1u;
    artifact->buildIdentity =
        "embedded|" + texture.sourceKey + "|" + options.target.artifactPlatform;

    auto payload = NLS::Render::Assets::SerializeTextureArtifact(*artifact);
    if (payload.empty())
    {
        error = "embedded texture artifact serialization failed";
        return std::nullopt;
    }

    const auto textureId = AssetId(NLS::Guid::NewDeterministic(
        "embedded-texture:" + modelMeta.id.ToString() + ":" + texture.sourceKey));
    NLS::Core::Assets::ArtifactWriteRequest request;
    request.sourceAssetId = textureId;
    request.importerId = "embedded-texture";
    request.importerVersion = 1u;
    request.targetPlatform = options.target.artifactPlatform;
    request.primarySubAssetKey = "texture:main";
    request.artifacts.emplace_back(
        "texture:main",
        ArtifactType::Texture,
        "texture",
        std::filesystem::path("embedded") / (texture.name.empty() ? "texture" : texture.name),
        std::move(payload));

    NLS::Core::Assets::ArtifactWriter writer(
        options.stagingRoot / ("Embedded-" + modelMeta.id.ToString()),
        options.artifactRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);
    if (!result.committed)
    {
        error = "embedded texture artifact could not be committed";
        return std::nullopt;
    }
    const auto* primary = result.manifest.FindPrimaryArtifact();
    if (primary == nullptr || primary->artifactPath.empty())
    {
        error = "embedded texture artifact has no primary payload";
        return std::nullopt;
    }
    database.UpsertManifest(
        result.manifest,
        "Assets/Embedded/" + modelSource.stem().generic_string() + "/" + texture.sourceKey,
        ArtifactRecordStatus::UpToDate);
    return std::filesystem::path(primary->artifactPath);
}
} // namespace

bool CookModelAsset(
    const CookOptions& options,
    const std::filesystem::path& source,
    const AssetMeta& meta,
    ArtifactDatabase& database,
    ModelCookOutput& output,
    std::string& error)
{
    output = {};

    const auto format = FormatForExtension(source);
    if (!format.has_value())
    {
        error = "model format is not supported by the shared cooker (gltf/glb/fbx/obj)";
        return false;
    }

    std::vector<ParsedMeshData> meshes;
    std::vector<std::string> materialNames;
    std::vector<std::string> externalDependencies;
    ImportedScene scene;
    const auto parserFlags =
        EModelParserFlags::TRIANGULATE |
        EModelParserFlags::CALC_TANGENT_SPACE |
        EModelParserFlags::JOIN_IDENTICAL_VERTICES |
        EModelParserFlags::GEN_SMOOTH_NORMALS |
        EModelParserFlags::FIND_INVALID_DATA |
        EModelParserFlags::IMPROVE_CACHE_LOCALITY |
        EModelParserFlags::OPTIMIZE_MESHES |
        EModelParserFlags::REMOVE_REDUNDANT_MATERIALS;
    if (!NLS::Render::Assets::ReadModelSource(source,meta,parserFlags,
            meshes,materialNames,externalDependencies,scene,error))
    {
        if(error.empty())error = "the model parser rejected the source file";
        return false;
    }
    if (!scene.diagnostics.empty())
    {
        for (const auto& diagnostic : scene.diagnostics)
        {
            output.diagnostics.push_back({
                CookSeverity::Warning,
                diagnostic.code,
                diagnostic.message,
                source.string()});
        }
    }

    output.dependencies.insert(output.dependencies.end(),scene.dependencies.begin(),scene.dependencies.end());

    // Assimp exposes glTF material textures as generic channels. Map those
    // channels back to the named PBR slots the shared material converter
    // serializes into ShaderLab materials.
    for (auto& material : scene.materials)
    {
        const auto findChannel = [&material](const std::string_view name)
            -> const NLS::Render::Assets::ImportedSceneMaterialChannel*
        {
            const auto lowered = ToLower(std::string(name));
            for (const auto& channel : material.materialChannels)
            {
                if (!channel.textureKey.empty() && ToLower(channel.name) == lowered)
                    return &channel;
            }
            return nullptr;
        };

        if (material.baseColorTextureKey.empty())
        {
            if (const auto* channel = findChannel("diffuse"))
                material.baseColorTextureKey = channel->textureKey;
            else if (const auto* channel = findChannel("basecolor"))
                material.baseColorTextureKey = channel->textureKey;
        }
        if (material.normalTextureKey.empty())
        {
            if (const auto* channel = findChannel("normal"))
                material.normalTextureKey = channel->textureKey;
        }
        if (material.occlusionTextureKey.empty())
        {
            if (const auto* channel = findChannel("occlusion"))
                material.occlusionTextureKey = channel->textureKey;
        }
        if (material.emissiveTextureKey.empty())
        {
            if (const auto* channel = findChannel("emissive"))
                material.emissiveTextureKey = channel->textureKey;
        }
        if (material.metallicRoughnessTextureKey.empty())
        {
            if (const auto* channel = findChannel("metallicroughness"))
                material.metallicRoughnessTextureKey = channel->textureKey;
            else
            {
                const auto* metallic = findChannel("metallic");
                const auto* roughness = findChannel("roughness");
                if (metallic != nullptr && roughness != nullptr &&
                    metallic->textureKey == roughness->textureKey)
                {
                    material.metallicRoughnessTextureKey = metallic->textureKey;
                }
            }
        }
    }

    const auto previousAnimationManifest=database.BuildManifestForSource(meta.id,options.target.artifactPlatform);
    if(!NLS::Render::Assets::StabilizeAnimationIdentities(scene,previousAnimationManifest?&*previousAnimationManifest:nullptr,error))return false;
    const auto subAssets = NLS::Render::Assets::GenerateSceneSubAssets(scene);
    for(const auto& subAsset:subAssets)
    {
        auto bytes=NLS::Render::Assets::SerializeAnimationSubAsset(scene,subAsset);
        if(bytes.empty())continue;
        ArtifactPayload payload;payload.subAssetKey=subAsset.key;
        payload.artifactType=subAsset.type==ImportedSceneSubAssetType::Skeleton?ArtifactType::Skeleton:
            subAsset.type==ImportedSceneSubAssetType::Skin?ArtifactType::Skin:
            subAsset.type==ImportedSceneSubAssetType::AnimationClip?ArtifactType::AnimationClip:ArtifactType::SkeletalMesh;
        payload.loaderId=NLS::Render::Assets::ToSubAssetPrefix(subAsset.type);
        payload.displayName=subAsset.displayName;payload.relativePath=std::filesystem::path("animation")/subAsset.sourceKey;
        payload.payload=std::move(bytes);payload.dependencies=output.dependencies;output.payloads.push_back(std::move(payload));
    }

    // Resolve external textures to their already-cooked artifacts. Embedded
    // textures are decoded and published as independent texture artifacts
    // before material conversion, so material payloads only reference paths
    // that will actually ship.
    MaterialConversionContext conversion;
    conversion.ignoreFbxTexturedNeutralDiffuseTint = true;
    for (const auto& texture : scene.textures)
    {
        if (IsEmbeddedTexture(texture))
        {
            std::string embeddedError;
            const auto cookedPath = CookEmbeddedTexture(
                options,
                source,
                meta,
                database,
                scene,
                texture,
                embeddedError);
            if (cookedPath.has_value())
            {
                conversion.importedTextureArtifactPaths[texture.sourceKey] = *cookedPath;
                const auto textureId = AssetId(NLS::Guid::NewDeterministic(
                    "embedded-texture:" + meta.id.ToString() + ":" + texture.sourceKey));
                output.dependencies.push_back({
                    AssetDependencyKind::ImportedArtifact,
                    textureId.ToString(),
                    "texture:main"});
            }
            else
            {
                conversion.deferredTextureSourceKeys.insert(texture.sourceKey);
                output.diagnostics.push_back({
                    CookSeverity::Warning,
                    "cook-model-embedded-texture",
                    "An embedded model texture could not be cooked: " + embeddedError,
                    texture.sourceKey});
            }
            continue;
        }

        const auto textureSource = ResolveTextureSource(options, source, texture.uri);
        if (!textureSource.has_value())
        {
            conversion.deferredTextureSourceKeys.insert(texture.sourceKey);
            output.diagnostics.push_back({
                CookSeverity::Warning,
                "cook-model-texture-missing",
                "A model texture could not be resolved; the material will omit that slot.",
                texture.uri});
            continue;
        }

        const auto textureMeta = AssetMeta::Load(
            NLS::Core::Assets::GetAssetMetaPath(*textureSource));
        if (!textureMeta.has_value() || !textureMeta->id.IsValid())
        {
            conversion.deferredTextureSourceKeys.insert(texture.sourceKey);
            output.diagnostics.push_back({
                CookSeverity::Warning,
                "cook-model-texture-meta",
                "A model texture has no valid .meta sidecar; the material will omit that slot.",
                textureSource->string()});
            continue;
        }

        const auto* record = FindTextureRecord(
            database,
            textureMeta->id,
            options.target.artifactPlatform);
        if (record == nullptr)
        {
            conversion.deferredTextureSourceKeys.insert(texture.sourceKey);
            output.diagnostics.push_back({
                CookSeverity::Warning,
                "cook-model-texture-not-cooked",
                "A model texture has no up-to-date cooked artifact; the material will omit that slot.",
                textureSource->string()});
            continue;
        }

        conversion.importedTextureArtifactPaths[texture.sourceKey] = record->artifactPath;
        output.dependencies.push_back({
            AssetDependencyKind::ImportedArtifact,
            record->sourceAssetId.ToString(),
            record->subAssetKey});
    }
    // Model materials use the engine StandardPBR shader. Add the cooked
    // shader artifact as a dependency so the package cannot silently rely on
    // an editor cache or a player-side compiler.
    {
        constexpr std::string_view defaultShaderPath =
            "Assets/Engine/Shaders/ShaderLab/StandardPBR.shader";
        const auto shaderId = AssetId(
            NLS::Cook::MakeEngineAssetGuid(defaultShaderPath));
        output.dependencies.push_back({
            AssetDependencyKind::ImportedArtifact,
            shaderId.ToString(),
            "shader:StandardPBR"});
    }

    // Materials first: the prefab graph references their sub-asset keys.
    {
        std::vector<NLS::Render::Assets::ConvertedMaterialArtifact> convertedMaterials;
        convertedMaterials.reserve(scene.materials.size());
        for (const auto& material : scene.materials)
        {
            convertedMaterials.push_back(NLS::Render::Assets::ConvertImportedSceneMaterial(
                scene,
                material,
                SourceModelForFormat(*format),
                conversion));
        }
        auto materialPayloads = NLS::Render::Assets::BuildMaterialArtifactPayloads(convertedMaterials);
        for (auto& payload : materialPayloads)
        {
            if (payload.payload.empty())
                continue;
            payload.dependencies = output.dependencies;
            output.payloads.push_back(std::move(payload));
        }
    }

    // Mesh sub-assets.
    std::unordered_map<std::string, std::vector<const ParsedMeshData*>> meshesByKey;
    for (size_t index = 0u; index < meshes.size(); ++index)
    {
        const auto key = meshes[index].sourceKey.empty()
            ? "parser/mesh/" + std::to_string(index)
            : meshes[index].sourceKey;
        meshesByKey[key].push_back(&meshes[index]);
    }

    std::map<std::string,NLS::Render::Assets::MeshArtifactData> distanceFieldMeshes;
    const auto fieldSettings=NLS::Render::DistanceField::ReadDistanceFieldBuildSettings(meta.settings,options.projectRoot);
    for (const auto& subAsset : subAssets)
    {
        if (subAsset.type != ImportedSceneSubAssetType::Mesh)
            continue;

        const auto* parsed = FindParsedMesh(meshesByKey, meshes, subAsset.sourceKey);
        if (parsed == nullptr)
        {
            error = "the model mesh sub-asset '" + subAsset.key + "' has no matching parsed mesh";
            return false;
        }

        NLS::Render::Assets::MeshArtifactData mesh;
        mesh.vertices = parsed->vertices;
        mesh.indices = parsed->indices;
        mesh.materialIndex = parsed->materialIndex;
        auto bytes = NLS::Render::Assets::SerializeMeshArtifact(mesh);
        if (bytes.empty())
        {
            error = "mesh artifact serialization failed for '" + subAsset.key + "'";
            return false;
        }

        ArtifactPayload payload;
        payload.subAssetKey = subAsset.key;
        payload.artifactType = ArtifactType::Mesh;
        payload.loaderId = "mesh";
        payload.displayName = subAsset.displayName;
        payload.relativePath = std::filesystem::path("meshes") / subAsset.sourceKey;
        payload.payload = std::move(bytes);
        output.payloads.push_back(std::move(payload));
        if(fieldSettings.Enabled()) distanceFieldMeshes.emplace(subAsset.key,std::move(mesh));
    }

    std::vector<std::string> distanceFieldDiagnostics;
    if(!NLS::Render::DistanceField::AppendDistanceFieldModelPayloads(scene,distanceFieldMeshes,
        fieldSettings,output.payloads,distanceFieldDiagnostics,options.cancel))
    {
        error=distanceFieldDiagnostics.empty()?"Distance field build failed":distanceFieldDiagnostics.back();
        return false;
    }
    for(auto& diagnostic:distanceFieldDiagnostics)
        output.diagnostics.push_back({CookSeverity::Warning,"mesh-distance-field-quality",std::move(diagnostic),source.string()});

    // Prefab graph references the mesh/material sub-assets.
    const auto prefabKey = "prefab:" + source.stem().generic_string();
    output.primarySubAssetKey = prefabKey;

    ArtifactManifest provisional;
    provisional.sourceAssetId = meta.id;
    provisional.importerId = meta.importerId;
    provisional.importerVersion = meta.importerVersion;
    provisional.targetPlatform = options.target.artifactPlatform;
    provisional.primarySubAssetKey = prefabKey;
    for (const auto& payload : output.payloads)
    {
        NLS::Core::Assets::ImportedArtifact imported;
        imported.sourceAssetId = meta.id;
        imported.subAssetKey = payload.subAssetKey;
        imported.artifactType = payload.artifactType;
        imported.loaderId = payload.loaderId;
        imported.targetPlatform = options.target.artifactPlatform;
        imported.artifactPath = "pending/" + payload.subAssetKey;
        imported.displayName = payload.displayName;
        provisional.subAssets.push_back(std::move(imported));
    }
    {
        NLS::Core::Assets::ImportedArtifact prefabPlaceholder;
        prefabPlaceholder.sourceAssetId = meta.id;
        prefabPlaceholder.subAssetKey = prefabKey;
        prefabPlaceholder.artifactType = ArtifactType::Prefab;
        prefabPlaceholder.loaderId = "prefab";
        prefabPlaceholder.targetPlatform = options.target.artifactPlatform;
        prefabPlaceholder.artifactPath = "pending/" + prefabKey;
        provisional.subAssets.push_back(std::move(prefabPlaceholder));
    }

    auto prefab = NLS::Engine::Assets::BuildGeneratedModelPrefab(scene, subAssets, provisional);
    if (prefab.diagnostics.HasErrors())
    {
        error = "generated model prefab validation failed";
        return false;
    }

    auto prefabBytes = NLS::Engine::Serialize::ObjectGraphWriter::Write(prefab.artifact.graph);
    if (prefabBytes.empty())
    {
        error = "generated model prefab serialization failed";
        return false;
    }

    ArtifactPayload prefabPayload;
    prefabPayload.subAssetKey = prefabKey;
    prefabPayload.artifactType = ArtifactType::Prefab;
    prefabPayload.loaderId = "prefab";
    prefabPayload.displayName = source.stem().generic_string();
    prefabPayload.relativePath = std::filesystem::path("prefabs") / prefabKey;
    prefabPayload.payload.assign(prefabBytes.begin(), prefabBytes.end());
    prefabPayload.dependencies = output.dependencies;
    output.payloads.push_back(std::move(prefabPayload));

    return true;
}
} // namespace NLS::Cook::Detail
