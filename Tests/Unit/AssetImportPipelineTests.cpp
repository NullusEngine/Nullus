#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "Assets/AssetId.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/AssetImporterSettings.h"
#include "Assets/ExternalAssetImporter.h"
#include "Assets/ModelTextureReferenceResolver.h"
#include "Assets/ModelTextureResolutionReport.h"
#include "Panels/AssetProperties.h"
#include "Assets/TextureEncoding/DirectXTexTextureEncoder.h"
#include "Guid.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Assets/ImportedScene.h"
#include "Rendering/Assets/MaterialConversion.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/Assets/TextureBuildSettings.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Assets/TextureEncoder.h"
#include "Rendering/Assets/TextureFormatResolver.h"
#include "Rendering/Assets/TextureMipGenerator.h"
#include "Rendering/Resources/Parsers/AssimpParser.h"
#include "Rendering/Resources/Parsers/IModelParser.h"
#include "Serialize/ObjectGraphReader.h"

#include <Json/json.hpp>

#ifndef NLS_HAS_ASSIMP_FBX_IMPORTER
#define NLS_HAS_ASSIMP_FBX_IMPORTER 0
#endif

#ifndef NLS_HAS_AUTODESK_FBX_SDK
#define NLS_HAS_AUTODESK_FBX_SDK 0
#endif

#ifndef NLS_HAS_DIRECTXTEX
#define NLS_HAS_DIRECTXTEX 0
#endif

namespace
{
std::string JoinDiagnosticSummaries(const NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
    std::string summary;
    for (const auto& diagnostic : diagnostics)
    {
        if (!summary.empty())
            summary += " | ";
        summary += diagnostic.code + ": " + diagnostic.message;
    }
    return summary;
}

std::vector<std::string> ExtractKeys(
    const std::vector<NLS::Render::Assets::GeneratedSceneSubAsset>& records)
{
    std::vector<std::string> keys;
    keys.reserve(records.size());
    for (const auto& record : records)
        keys.push_back(record.key);
    return keys;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected)
{
    return std::find(values.begin(), values.end(), expected) != values.end();
}

bool ContainsDiagnosticCode(
    const NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const std::string& expectedCode)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [&expectedCode](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == expectedCode;
        });
}

bool ContainsDiagnostic(
    const NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const std::string& expectedCode,
    const NLS::Core::Assets::AssetDiagnosticSeverity expectedSeverity)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [&expectedCode, expectedSeverity](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == expectedCode && diagnostic.severity == expectedSeverity;
        });
}

const NLS::Core::Assets::AssetDependencyRecord* FindDependency(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::AssetDependencyKind kind,
    const std::string& value)
{
    const auto found = std::find_if(
        manifest.dependencies.begin(),
        manifest.dependencies.end(),
        [kind, &value](const NLS::Core::Assets::AssetDependencyRecord& dependency)
        {
            return dependency.kind == kind && dependency.value == value;
        });
    return found != manifest.dependencies.end() ? &*found : nullptr;
}

bool ContainsDiagnosticSeverity(
    const NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const NLS::Core::Assets::AssetDiagnosticSeverity expectedSeverity)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [expectedSeverity](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.severity == expectedSeverity;
        });
}

uint32_t CurrentModelSceneImporterVersion()
{
    return NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
}

const NLS::Core::Assets::AssetDiagnostic* FindDiagnosticByCode(
    const NLS::Core::Assets::AssetDiagnostics& diagnostics,
    const std::string& expectedCode)
{
    const auto found = std::find_if(
        diagnostics.begin(),
        diagnostics.end(),
        [&expectedCode](const NLS::Core::Assets::AssetDiagnostic& diagnostic)
        {
            return diagnostic.code == expectedCode;
        });
    return found != diagnostics.end() ? &*found : nullptr;
}

size_t CountArtifactTelemetryStage(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
{
    return static_cast<size_t>(std::count_if(
        records.begin(),
        records.end(),
        [stage](const NLS::Core::Assets::ArtifactLoadTelemetryRecord& record)
        {
            return record.stage == stage;
        }));
}

void AppendU32(std::vector<uint8_t>& bytes, const uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
}

void PadToFour(std::vector<uint8_t>& bytes, const uint8_t pad)
{
    while ((bytes.size() % 4u) != 0u)
        bytes.push_back(pad);
}

std::vector<uint8_t> MakeGlb(const std::string& json, const std::vector<uint8_t>& binaryChunk)
{
    std::vector<uint8_t> jsonChunk(json.begin(), json.end());
    PadToFour(jsonChunk, static_cast<uint8_t>(' '));

    std::vector<uint8_t> binChunk = binaryChunk;
    PadToFour(binChunk, 0u);

    std::vector<uint8_t> glb;
    AppendU32(glb, 0x46546C67u);
    AppendU32(glb, 2u);
    AppendU32(glb, static_cast<uint32_t>(12u + 8u + jsonChunk.size() + 8u + binChunk.size()));
    AppendU32(glb, static_cast<uint32_t>(jsonChunk.size()));
    AppendU32(glb, 0x4E4F534Au);
    glb.insert(glb.end(), jsonChunk.begin(), jsonChunk.end());
    AppendU32(glb, static_cast<uint32_t>(binChunk.size()));
    AppendU32(glb, 0x004E4942u);
    glb.insert(glb.end(), binChunk.begin(), binChunk.end());
    return glb;
}

class FakeModelParser final : public NLS::Render::Resources::Parsers::IModelParser
{
public:
    bool LoadModel(
        const std::string&,
        std::vector<NLS::Render::Resources::Mesh*>& meshes,
        std::vector<std::string>& materials,
        NLS::Render::Resources::Parsers::EModelParserFlags) override
    {
        loadCalled = true;
        if (!this->meshes.empty())
            meshes = this->meshes;
        else
            meshes.resize(meshCount, nullptr);
        materials = materialNames;
        return result;
    }

    bool result = true;
    bool loadCalled = false;
    size_t meshCount = 0u;
    std::vector<NLS::Render::Resources::Mesh*> meshes;
    std::vector<std::string> materialNames;
};

class DetailedFakeModelParser final :
    public NLS::Render::Resources::Parsers::IModelParser,
    public NLS::Render::Assets::IImportedSceneParserDataProvider
{
public:
    bool LoadModel(
        const std::string&,
        std::vector<NLS::Render::Resources::Mesh*>& meshes,
        std::vector<std::string>& materials,
        NLS::Render::Resources::Parsers::EModelParserFlags) override
    {
        loadCalled = true;
        meshes.resize(meshCount, nullptr);
        materials = materialNames;
        return result;
    }

    bool PopulateImportedSceneData(
        const std::filesystem::path&,
        NLS::Render::Assets::SceneModelSourceFormat,
        NLS::Render::Assets::ImportedScene& scene) override
    {
        scene.nodes.insert(scene.nodes.end(), nodes.begin(), nodes.end());
        scene.meshes.insert(scene.meshes.end(), meshes.begin(), meshes.end());
        scene.materials.insert(scene.materials.end(), materials.begin(), materials.end());
        scene.textures.insert(scene.textures.end(), textures.begin(), textures.end());
        scene.skeletons.insert(scene.skeletons.end(), skeletons.begin(), skeletons.end());
        scene.skins.insert(scene.skins.end(), skins.begin(), skins.end());
        scene.animations.insert(scene.animations.end(), animations.begin(), animations.end());
        scene.morphTargets.insert(scene.morphTargets.end(), morphTargets.begin(), morphTargets.end());
        scene.diagnostics.insert(scene.diagnostics.end(), diagnostics.begin(), diagnostics.end());
        return detailedResult;
    }

    bool result = true;
    bool detailedResult = true;
    bool loadCalled = false;
    size_t meshCount = 0u;
    std::vector<std::string> materialNames;
    std::vector<NLS::Render::Assets::ImportedSceneNode> nodes;
    std::vector<NLS::Render::Assets::ImportedSceneNamedRecord> meshes;
    std::vector<NLS::Render::Assets::ImportedSceneNamedRecord> materials;
    std::vector<NLS::Render::Assets::ImportedSceneNamedRecord> textures;
    std::vector<NLS::Render::Assets::ImportedSceneNamedRecord> skeletons;
    std::vector<NLS::Render::Assets::ImportedSceneNamedRecord> skins;
    std::vector<NLS::Render::Assets::ImportedSceneNamedRecord> animations;
    std::vector<NLS::Render::Assets::ImportedSceneNamedRecord> morphTargets;
    std::vector<NLS::Render::Assets::ImportedSceneDiagnostic> diagnostics;
};

const NLS::Render::Assets::ImportedSceneMaterialChannel* FindMaterialChannel(
    const NLS::Render::Assets::ImportedSceneNamedRecord& material,
    const std::string& name)
{
    const auto found = std::find_if(
        material.materialChannels.begin(),
        material.materialChannels.end(),
        [&name](const NLS::Render::Assets::ImportedSceneMaterialChannel& channel)
        {
            return channel.name == name;
        });
    return found != material.materialChannels.end() ? &*found : nullptr;
}

std::filesystem::path MakeImportTestRoot()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("nullus_asset_import_" + NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    return root;
}

class ScopedImportTestRoot final
{
public:
    explicit ScopedImportTestRoot(std::filesystem::path path)
        : m_path(std::move(path))
    {
    }

    ScopedImportTestRoot(const ScopedImportTestRoot&) = delete;
    ScopedImportTestRoot& operator=(const ScopedImportTestRoot&) = delete;
    ScopedImportTestRoot(ScopedImportTestRoot&&) = delete;
    ScopedImportTestRoot& operator=(ScopedImportTestRoot&&) = delete;

    ~ScopedImportTestRoot()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string SliceBetween(
    const std::string& source,
    const std::string& beginMarker,
    const std::string& endMarker)
{
    const auto begin = source.find(beginMarker);
    if (begin == std::string::npos)
        return {};

    const auto end = source.find(endMarker, begin + beginMarker.size());
    if (end == std::string::npos)
        return source.substr(begin);
    return source.substr(begin, end - begin);
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

std::filesystem::path ResolveTestArtifactPath(
    const std::filesystem::path& root,
    const std::string& artifactPath)
{
    const auto path = std::filesystem::path(artifactPath);
    if (path.is_absolute())
        return path.lexically_normal();
    if (path == "Library" || path.generic_string().rfind("Library/", 0u) == 0u)
        return (root / path).lexically_normal();
    return (root / path).lexically_normal();
}

std::vector<uint8_t> ReadArtifactFile(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ImportedArtifact& artifact)
{
    return ReadBinaryFile(ResolveTestArtifactPath(root, artifact.artifactPath));
}

std::string ReadArtifactPayloadText(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto bytes = ReadBinaryFile(path);
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(bytes, artifactType, schemaVersion);
    if (!container.has_value())
        return {};

    return std::string(container->payload.begin(), container->payload.end());
}

std::string ReadArtifactPayloadText(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ImportedArtifact& artifact,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    return ReadArtifactPayloadText(
        ResolveTestArtifactPath(root, artifact.artifactPath),
        artifactType,
        schemaVersion);
}

std::string ToPortableArtifactPath(
    const std::filesystem::path& root,
    const std::filesystem::path& artifactPath)
{
    const auto normalized = artifactPath.lexically_normal();
    if (normalized.is_absolute())
        return normalized.lexically_relative(root).generic_string();
    return normalized.generic_string();
}

void ExpectMaterialTextureSlot(
    const std::string& payload,
    const std::string& shaderProperty,
    const std::filesystem::path& root,
    const std::filesystem::path& artifactPath)
{
    const auto portablePath = ToPortableArtifactPath(root, artifactPath);
    EXPECT_NE(payload.find("shaderLabMaterialVersion=1"), std::string::npos);
    EXPECT_NE(payload.find("property " + shaderProperty + " Texture2D " + portablePath), std::string::npos)
        << payload;
    EXPECT_NE(payload.find("textureSlot " + shaderProperty), std::string::npos);
    EXPECT_NE(payload.find("resourcePath=" + portablePath), std::string::npos)
        << payload;
}

void ExpectNoLegacyTextureArtifactReference(const std::string& payload, const std::string& encodedTextureKey)
{
    EXPECT_EQ(payload.find("textures/" + encodedTextureKey + ".ntex"), std::string::npos);
}

size_t CountRegularFilesRecursive(const std::filesystem::path& root)
{
    if (!std::filesystem::exists(root))
        return 0u;

    size_t count = 0u;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, error), end;
        !error && it != end;
        it.increment(error))
    {
        if (it->is_regular_file(error) && !error)
            ++count;
        error.clear();
    }
    return count;
}

const NLS::Core::Assets::ImportedArtifact* FindFirstArtifactOfType(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const NLS::Core::Assets::ArtifactType artifactType)
{
    const auto found = std::find_if(
        manifest.subAssets.begin(),
        manifest.subAssets.end(),
        [artifactType](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            return artifact.artifactType == artifactType;
        });
    return found != manifest.subAssets.end() ? &*found : nullptr;
}

const NLS::Core::Assets::ImportedArtifact* FindAutoImportedTextureArtifact(
    const NLS::Editor::Assets::ExternalModelImportResult& result,
    const std::string& sourcePathFragment = {})
{
    for (const auto& dependency : result.autoImportedDependencies)
    {
        if (!sourcePathFragment.empty() &&
            dependency.sourcePath.generic_string().find(sourcePathFragment) == std::string::npos)
        {
            continue;
        }

        if (const auto* artifact = dependency.manifest.FindSubAsset("texture:main");
            artifact != nullptr &&
            artifact->artifactType == NLS::Core::Assets::ArtifactType::Texture)
        {
            return artifact;
        }
    }

    return nullptr;
}

std::optional<NLS::Render::Assets::TextureArtifactData> ReadAutoImportedTextureArtifact(
    const NLS::Editor::Assets::ExternalModelImportResult& result,
    const std::filesystem::path& root,
    const std::string& sourcePathFragment = {})
{
    const auto* artifact = FindAutoImportedTextureArtifact(result, sourcePathFragment);
    if (artifact == nullptr)
        return std::nullopt;
    return NLS::Render::Assets::DeserializeTextureArtifact(
        ReadBinaryFile(ResolveTestArtifactPath(root, artifact->artifactPath)));
}

void EraseFbxConnectionBlock(std::string& fbx, const std::string& connectionComment)
{
    const auto commentBegin = fbx.find(connectionComment);
    ASSERT_NE(commentBegin, std::string::npos);
    const auto eraseBegin = fbx.rfind('\n', commentBegin);
    const auto connectionEnd = fbx.find('\n', fbx.find("C: ", commentBegin));
    ASSERT_NE(connectionEnd, std::string::npos);
    fbx.erase(
        eraseBegin == std::string::npos ? commentBegin : eraseBegin,
        connectionEnd + 1u - (eraseBegin == std::string::npos ? commentBegin : eraseBegin));
}

void ReplaceAllText(std::string& text, const std::string& oldValue, const std::string& newValue)
{
    ASSERT_FALSE(oldValue.empty());
    size_t offset = 0u;
    while ((offset = text.find(oldValue, offset)) != std::string::npos)
    {
        text.replace(offset, oldValue.size(), newValue);
        offset += newValue.size();
    }
}

std::vector<uint8_t> ReadArtifactPayloadBytes(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto bytes = ReadBinaryFile(path);
    const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(bytes, artifactType, schemaVersion);
    if (!container.has_value())
        return {};

    return container->payload;
}

std::vector<uint8_t> ReadArtifactPayloadBytes(
    const std::filesystem::path& root,
    const NLS::Core::Assets::ImportedArtifact& artifact,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    return ReadArtifactPayloadBytes(
        ResolveTestArtifactPath(root, artifact.artifactPath),
        artifactType,
        schemaVersion);
}

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

std::string ArtifactTypeManifestKey(const NLS::Core::Assets::ArtifactType type)
{
    switch (type)
    {
    case NLS::Core::Assets::ArtifactType::Model: return "model";
    case NLS::Core::Assets::ArtifactType::Mesh: return "mesh";
    case NLS::Core::Assets::ArtifactType::Material: return "material";
    case NLS::Core::Assets::ArtifactType::Texture: return "texture";
    case NLS::Core::Assets::ArtifactType::Skeleton: return "skeleton";
    case NLS::Core::Assets::ArtifactType::Skin: return "skin";
    case NLS::Core::Assets::ArtifactType::AnimationClip: return "animation-clip";
    case NLS::Core::Assets::ArtifactType::MorphTarget: return "morph-target";
    case NLS::Core::Assets::ArtifactType::Prefab: return "prefab";
    case NLS::Core::Assets::ArtifactType::Scene: return "scene";
    case NLS::Core::Assets::ArtifactType::Shader: return "shader";
    case NLS::Core::Assets::ArtifactType::Audio: return "audio";
    case NLS::Core::Assets::ArtifactType::Unknown:
    case NLS::Core::Assets::ArtifactType::Count:
        break;
    }
    return "unknown";
}

NLS::Core::Assets::ArtifactType ArtifactTypeFromManifestKey(const std::string& key)
{
    using NLS::Core::Assets::ArtifactType;
    if (key == "model" || key == "Model") return ArtifactType::Model;
    if (key == "mesh" || key == "Mesh") return ArtifactType::Mesh;
    if (key == "material" || key == "Material") return ArtifactType::Material;
    if (key == "texture" || key == "Texture") return ArtifactType::Texture;
    if (key == "skeleton" || key == "Skeleton") return ArtifactType::Skeleton;
    if (key == "skin" || key == "Skin") return ArtifactType::Skin;
    if (key == "animation-clip" || key == "AnimationClip") return ArtifactType::AnimationClip;
    if (key == "morph-target" || key == "MorphTarget") return ArtifactType::MorphTarget;
    if (key == "prefab" || key == "Prefab") return ArtifactType::Prefab;
    if (key == "scene" || key == "Scene") return ArtifactType::Scene;
    if (key == "shader" || key == "Shader") return ArtifactType::Shader;
    if (key == "audio" || key == "Audio") return ArtifactType::Audio;
    return ArtifactType::Unknown;
}

void WriteArtifactManifestFile(
    const std::filesystem::path& artifactRoot,
    const NLS::Core::Assets::ArtifactManifest& manifest)
{
    const auto normalizedRoot = artifactRoot.lexically_normal();
    std::filesystem::path libraryPath;
    for (auto it = normalizedRoot.begin(); it != normalizedRoot.end(); ++it)
    {
        libraryPath /= *it;
        if (*it == "Library")
            break;
    }
    if (libraryPath.empty() || libraryPath.filename() != "Library")
        libraryPath = normalizedRoot.parent_path();
    NLS::Core::Assets::ArtifactDatabase database;
    const auto databasePath = libraryPath / "ArtifactDB";
    if (std::filesystem::exists(databasePath))
        (void)database.Load(databasePath);
    database.UpsertManifest(manifest, {}, NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    EXPECT_TRUE(database.Save(databasePath));
}

std::filesystem::path WriteTextureArtifactBlobForTest(
    const std::filesystem::path& root,
    const NLS::Core::Assets::AssetMeta& textureMeta,
    const std::string& subAssetKey,
    const std::string& displayName,
    const std::string& targetPlatform,
    const NLS::Render::Assets::TextureArtifactData& texture)
{
    const auto textureContainer = NLS::Render::Assets::SerializeTextureArtifact(texture);
    const auto texturePayload = NLS::Core::Assets::ReadNativeArtifactContainer(
        textureContainer,
        NLS::Core::Assets::ArtifactType::Texture,
        4u);
    EXPECT_TRUE(texturePayload.has_value());
    if (!texturePayload.has_value())
        return {};

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Texture;
    metadata.schemaName = "texture";
    metadata.schemaVersion = 4u;
    metadata.sourceAssetId = textureMeta.id;
    metadata.subAssetKey = subAssetKey;
    metadata.displayName = displayName;
    metadata.importerId = textureMeta.importerId;
    metadata.importerVersion = textureMeta.importerVersion;
    metadata.targetPlatform = targetPlatform;
    const auto storedTexture = NLS::Core::Assets::WriteNativeArtifactContainer(
        std::move(metadata),
        texturePayload->payload);
    const auto textureArtifactRelativePath = std::filesystem::path("Library") /
        "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(
            NLS::Core::Assets::BuildArtifactStorageFileName(storedTexture.data(), storedTexture.size()));
    WriteBinaryFile(root / textureArtifactRelativePath, storedTexture);
    return textureArtifactRelativePath;
}

std::string TextureArtifactTargetPlatformForTest();

std::optional<NLS::Core::Assets::ArtifactManifest> ReadArtifactManifestFile(
    const std::filesystem::path& manifestPath)
{
    auto normalized = manifestPath.lexically_normal();
    std::filesystem::path libraryPath;
    for (auto it = normalized.begin(); it != normalized.end(); ++it)
    {
        libraryPath /= *it;
        if (*it == "Library")
            break;
    }
    if (libraryPath.empty() || libraryPath.filename() != "Library")
        return std::nullopt;

    NLS::Core::Assets::ArtifactDatabase database;
    if (!database.Load(libraryPath / "ArtifactDB"))
        return std::nullopt;

    std::optional<NLS::Core::Assets::AssetId> requestedSourceAssetId;
    for (auto it = normalized.begin(); it != normalized.end(); ++it)
    {
        if (*it == "Artifacts")
        {
            const auto next = std::next(it);
            if (next != normalized.end())
            {
                if (const auto guid = NLS::Guid::TryParse(next->string()))
                    requestedSourceAssetId = NLS::Core::Assets::AssetId(*guid);
            }
            break;
        }
    }
    if (requestedSourceAssetId.has_value())
    {
        if (auto manifest = database.BuildManifestForSource(*requestedSourceAssetId, "editor"))
            return manifest;
        if (auto manifest = database.BuildManifestForSource(*requestedSourceAssetId, TextureArtifactTargetPlatformForTest()))
            return manifest;
    }

    for (const auto& record : database.GetRecords())
    {
        if (record.sourceAssetId.IsValid())
            return database.BuildManifestForSource(record.sourceAssetId, record.targetPlatform);
    }
    return std::nullopt;
}

void DisableExternalModelTextureResolution(NLS::Core::Assets::AssetMeta& meta)
{
    NLS::Editor::Assets::ModelTextureResolutionSettings settings;
    settings.useExternalTextures = false;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(meta, settings);
}

std::vector<uint8_t> TinyPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C,
        0x02, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xFC, 0xFF, 0x1F, 0x00,
        0x03, 0x03, 0x02, 0x00, 0xEF, 0xBF, 0x4A, 0x3B,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

std::string TextureArtifactTargetPlatformForTest()
{
#if defined(_WIN32)
    return "win64-dx12";
#else
    return "editor";
#endif
}

std::filesystem::path WriteImportedTextureAssetForTest(
    const std::filesystem::path& root,
    const std::filesystem::path& assetPath,
    const char* assetIdText,
    const std::string& targetPlatform,
    const std::string& displayName,
    const NLS::Render::Assets::TextureArtifactColorSpace colorSpace)
{
    NLS::Core::Assets::AssetMeta textureMeta;
    textureMeta.id = NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetIdText));
    textureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    EXPECT_TRUE(textureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(assetPath)));

    auto decodedTexture = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        TinyPng().data(),
        TinyPng().size(),
        colorSpace,
        false);
    EXPECT_TRUE(decodedTexture.has_value());
    if (!decodedTexture.has_value())
        return {};

    const auto effectiveTargetPlatform = targetPlatform == "editor"
        ? TextureArtifactTargetPlatformForTest()
        : targetPlatform;
    decodedTexture->targetPlatform = effectiveTargetPlatform;
    decodedTexture->encoderId = "rgba8-passthrough";
    decodedTexture->encoderVersion = 1u;
    decodedTexture->buildIdentity = "unit-test-imported-texture:" + displayName;

    const auto textureArtifactRelativePath = WriteTextureArtifactBlobForTest(
        root,
        textureMeta,
        "texture:main",
        displayName,
        effectiveTargetPlatform,
        *decodedTexture);
    if (textureArtifactRelativePath.empty())
        return {};
    const auto textureArtifactPath = root / textureArtifactRelativePath;
    const auto textureArtifactRoot = root / "Library" / "Artifacts";

    NLS::Core::Assets::ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureMeta.id;
    textureManifest.importerId = textureMeta.importerId;
    textureManifest.importerVersion = textureMeta.importerVersion;
    textureManifest.targetPlatform = effectiveTargetPlatform;
    textureManifest.primarySubAssetKey = "texture:main";
    textureManifest.subAssets.push_back({
        textureMeta.id,
        "texture:main",
        NLS::Core::Assets::ArtifactType::Texture,
        "texture",
        effectiveTargetPlatform,
        textureArtifactRelativePath.generic_string(),
        "sha256:" + textureArtifactPath.filename().generic_string(),
        displayName
    });
    WriteArtifactManifestFile(textureArtifactRoot, textureManifest);
    return textureArtifactPath;
}

std::vector<uint8_t> TinyTransparentRgbaPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xF8, 0xFF, 0xFF, 0x7F,
        0x03, 0x00, 0x09, 0x7C, 0x03, 0x7E, 0x91, 0xE5,
        0x09, 0x4D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
    };
}

std::vector<uint8_t> TinyOpaqueRgbaPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
        0x89, 0x00, 0x00, 0x00, 0x0B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0xDA, 0x63, 0xF8, 0x0F, 0x04, 0x00,
        0x09, 0xFB, 0x03, 0xFD, 0x68, 0xFA, 0x1C, 0xCC,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

std::vector<uint8_t> TwoRowColorPng()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00,
        0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
        0x1F, 0x84, 0x19, 0xA0, 0xF4, 0x7F, 0x00, 0x43, 0xCE, 0x07, 0xF9, 0x00,
        0xF3, 0x98, 0x01, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
        0x42, 0x60, 0x82
    };
}

std::vector<uint8_t> NormalMap2x2Png()
{
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D,
        0x24, 0x00, 0x00, 0x00, 0x1B, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9C, 0x63, 0x68, 0x68, 0xF8, 0xFF,
        0xFF, 0x4C, 0xC3, 0xB3, 0xFF, 0x0C, 0x0D, 0x67,
        0x9E, 0xFD, 0x3F, 0x73, 0xE6, 0xF0, 0x7F, 0x00,
        0x6D, 0xB5, 0x0C, 0xBB, 0x8F, 0xD2, 0x8D, 0xD5,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
        0xAE, 0x42, 0x60, 0x82
    };
}

std::vector<uint8_t> UncompressedBmp(
    const uint32_t width,
    const uint32_t height,
    const uint8_t red,
    const uint8_t green,
    const uint8_t blue)
{
    const uint32_t bytesPerPixel = 3u;
    const uint32_t rowStride = ((width * bytesPerPixel + 3u) / 4u) * 4u;
    const uint32_t pixelBytes = rowStride * height;
    const uint32_t fileHeaderBytes = 14u;
    const uint32_t dibHeaderBytes = 40u;
    const uint32_t pixelOffset = fileHeaderBytes + dibHeaderBytes;
    const uint32_t fileSize = pixelOffset + pixelBytes;

    std::vector<uint8_t> bytes;
    bytes.reserve(fileSize);
    const auto appendU16 = [&bytes](const uint16_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
        bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    };
    const auto appendU32 = [&bytes](const uint32_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
        bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
        bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
        bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
    };

    appendU16(0x4D42u);
    appendU32(fileSize);
    appendU16(0u);
    appendU16(0u);
    appendU32(pixelOffset);
    appendU32(dibHeaderBytes);
    appendU32(width);
    appendU32(height);
    appendU16(1u);
    appendU16(24u);
    appendU32(0u);
    appendU32(pixelBytes);
    appendU32(2835u);
    appendU32(2835u);
    appendU32(0u);
    appendU32(0u);

    for (uint32_t y = 0u; y < height; ++y)
    {
        for (uint32_t x = 0u; x < width; ++x)
        {
            bytes.push_back(blue);
            bytes.push_back(green);
            bytes.push_back(red);
        }
        while ((bytes.size() - pixelOffset) % rowStride != 0u)
            bytes.push_back(0u);
    }
    return bytes;
}

float DecodeNormalComponent(const uint8_t component)
{
    return (static_cast<float>(component) / 255.0f) * 2.0f - 1.0f;
}

float MaxAbsMeshPosition(const NLS::Render::Assets::MeshArtifactData& mesh)
{
    float maxPosition = 0.0f;
    for (const auto& vertex : mesh.vertices)
    {
        maxPosition = std::max(maxPosition, std::fabs(vertex.position[0]));
        maxPosition = std::max(maxPosition, std::fabs(vertex.position[1]));
        maxPosition = std::max(maxPosition, std::fabs(vertex.position[2]));
    }
    return maxPosition;
}

const NLS::Engine::Serialize::ObjectRecord* FindRecord(
    const NLS::Engine::Serialize::ObjectGraphDocument& document,
    const std::string& debugName,
    const std::string& typeName)
{
    const auto found = std::find_if(
        document.objects.begin(),
        document.objects.end(),
        [&debugName, &typeName](const NLS::Engine::Serialize::ObjectRecord& record)
        {
            return record.debugName == debugName && record.typeName == typeName;
        });
    return found != document.objects.end() ? &*found : nullptr;
}

const NLS::Engine::Serialize::PropertyRecord* FindProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const std::string& name)
{
    const auto found = std::find_if(
        record.properties.begin(),
        record.properties.end(),
        [&name](const NLS::Engine::Serialize::PropertyRecord& property)
        {
            return property.name == name;
        });
    return found != record.properties.end() ? &*found : nullptr;
}

double GetObjectNumber(
    const NLS::Engine::Serialize::PropertyValue& value,
    const std::string& name)
{
    const auto& object = value.GetObject();
    const auto found = std::find_if(
        object.begin(),
        object.end(),
        [&name](const auto& property)
        {
            return property.first == name;
        });
    return found != object.end() ? found->second.GetNumber() : 0.0;
}

double MaxAbsLocalScale(const NLS::Engine::Serialize::PropertyValue& scale)
{
    double maxScale = 0.0;
    maxScale = std::max(maxScale, std::fabs(GetObjectNumber(scale, "x")));
    maxScale = std::max(maxScale, std::fabs(GetObjectNumber(scale, "y")));
    maxScale = std::max(maxScale, std::fabs(GetObjectNumber(scale, "z")));
    return maxScale;
}

std::string DescribeTransformLocalScales(
    const NLS::Engine::Serialize::ObjectGraphDocument& document,
    const std::string& transformTypeName)
{
    std::string description;
    for (const auto& object : document.objects)
    {
        if (object.typeName != transformTypeName)
            continue;

        const auto* scale = FindProperty(object, "localScale");
        if (!scale)
            continue;

        if (!description.empty())
            description += " | ";
        description += object.debugName;
        description += "=(";
        description += std::to_string(GetObjectNumber(scale->value, "x"));
        description += ",";
        description += std::to_string(GetObjectNumber(scale->value, "y"));
        description += ",";
        description += std::to_string(GetObjectNumber(scale->value, "z"));
        description += ")";
    }
    return description;
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

void AppendFloat32(std::vector<uint8_t>& bytes, const float value)
{
    const auto* raw = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(float));
}

void AppendU16(std::vector<uint8_t>& bytes, const uint16_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
}

void AppendTriangleMeshBytes(
    std::vector<uint8_t>& bytes,
    const float xOffset,
    const uint16_t firstIndex,
    const uint16_t secondIndex,
    const uint16_t thirdIndex)
{
    AppendFloat32(bytes, xOffset + 0.0f);
    AppendFloat32(bytes, 0.0f);
    AppendFloat32(bytes, 0.0f);
    AppendFloat32(bytes, xOffset + 1.0f);
    AppendFloat32(bytes, 0.0f);
    AppendFloat32(bytes, 0.0f);
    AppendFloat32(bytes, xOffset + 0.0f);
    AppendFloat32(bytes, 1.0f);
    AppendFloat32(bytes, 0.0f);
    AppendU16(bytes, firstIndex);
    AppendU16(bytes, secondIndex);
    AppendU16(bytes, thirdIndex);
}

class CancelAfterChecks final : public NLS::Core::Assets::IArtifactWriteCancellation
{
public:
    explicit CancelAfterChecks(size_t checksBeforeCancel)
        : m_checksBeforeCancel(checksBeforeCancel)
    {
    }

    bool IsCancellationRequested() const override
    {
        return ++m_checks >= m_checksBeforeCancel;
    }

private:
    size_t m_checksBeforeCancel = 0u;
    mutable size_t m_checks = 0u;
};
}

TEST(AssetImportPipelineTests, DefaultRegistrySelectsSceneImportersByExtension)
{
    auto registry = NLS::Render::Assets::SceneImporterRegistry::CreateDefault();
    const auto currentModelSceneImporterVersion =
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto* gltf = registry.FindImporterForPath("Assets/Models/Hero.gltf");
    ASSERT_NE(gltf, nullptr);
    EXPECT_EQ(gltf->importerId, "gltf-scene");
    EXPECT_EQ(gltf->importerVersion, currentModelSceneImporterVersion);

    const auto* glb = registry.FindImporterForPath("Assets/Models/Hero.GLB");
    ASSERT_NE(glb, nullptr);
    EXPECT_EQ(glb->importerId, "gltf-scene");
    EXPECT_EQ(glb->importerVersion, currentModelSceneImporterVersion);

    const auto* fbx = registry.FindImporterForPath("Assets/Models/Hero.fbx");
    ASSERT_NE(fbx, nullptr);
    EXPECT_EQ(fbx->importerId, "fbx-scene");
    EXPECT_EQ(fbx->importerVersion, currentModelSceneImporterVersion);

    const auto* obj = registry.FindImporterForPath("Assets/Models/Hero.obj");
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->importerId, "obj-scene");
    EXPECT_EQ(obj->importerVersion, currentModelSceneImporterVersion);

    EXPECT_EQ(registry.FindImporterForPath("Assets/Textures/Hero.png"), nullptr);
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesMaterialTextureSlotArtifacts)
{
    EXPECT_GE(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        11u)
        << "Model material artifacts before importer version 11 can miss ShaderLab texture slots such as _NormalMap.";
}

TEST(AssetImportPipelineTests, GeneratedSubAssetKeysAreDeterministicAcrossSourceOrder)
{
    NLS::Render::Assets::ImportedScene first;
    first.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));
    first.sceneKey = "HeroScene";
    first.meshes.push_back({"node/Body/mesh0", "Body"});
    first.meshes.push_back({"node/Sword/mesh0", "Sword"});
    first.materials.push_back({"material/Hero", "Hero"});
    first.textures.push_back({"image/Albedo", "Textures/Hero_Albedo.png"});
    first.skeletons.push_back({"skin/HeroSkeleton", "HeroSkeleton"});
    first.animations.push_back({"anim/Idle", "Idle"});
    first.morphTargets.push_back({"morph/Smile", "Smile"});

    NLS::Render::Assets::ImportedScene reordered = first;
    std::reverse(reordered.meshes.begin(), reordered.meshes.end());

    const auto firstRecords = NLS::Render::Assets::GenerateSceneSubAssets(first);
    const auto reorderedRecords = NLS::Render::Assets::GenerateSceneSubAssets(reordered);

    const std::vector<std::string> expectedKeys {
        "animation:anim/Idle",
        "material:material/Hero",
        "mesh:node/Body/mesh0",
        "mesh:node/Sword/mesh0",
        "morph-target:morph/Smile",
        "prefab:HeroScene",
        "skeleton:skin/HeroSkeleton",
        "texture:image/Albedo"
    };

    EXPECT_EQ(ExtractKeys(firstRecords), ExtractKeys(reorderedRecords));
    EXPECT_EQ(ExtractKeys(firstRecords), expectedKeys);
}

TEST(AssetImportPipelineTests, ArtifactManifestSelectsPrimaryArtifactAndFindsSubAssets)
{
    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("22222222-2222-4222-8222-222222222222"));
    manifest.importerId = "scene-model";
    manifest.targetPlatform = "editor-windows";

    NLS::Core::Assets::ImportedArtifact prefab;
    prefab.subAssetKey = "prefab:HeroScene";
    prefab.artifactType = NLS::Core::Assets::ArtifactType::Prefab;
    prefab.loaderId = "prefab";
    prefab.artifactPath = "Library/Artifacts/Hero/5d4b4d6c2b6c4a6c9b91d90753df2a8d";

    NLS::Core::Assets::ImportedArtifact mesh;
    mesh.subAssetKey = "mesh:Body";
    mesh.artifactType = NLS::Core::Assets::ArtifactType::Mesh;
    mesh.loaderId = "mesh";
    mesh.artifactPath = "Library/Artifacts/Hero/7e0aaf65f74245f291bdf6a0c3f6c4e8";

    manifest.primarySubAssetKey = prefab.subAssetKey;
    manifest.subAssets.push_back(prefab);
    manifest.subAssets.push_back(mesh);
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
        "Assets/Models/Hero.gltf",
        "sha256:source"
    });

    ASSERT_NE(manifest.FindPrimaryArtifact(), nullptr);
    EXPECT_EQ(manifest.FindPrimaryArtifact()->artifactPath, prefab.artifactPath);

    const auto* prefabArtifact = manifest.FindSubAsset("prefab:HeroScene");
    ASSERT_NE(prefabArtifact, nullptr);
    EXPECT_EQ(prefabArtifact->artifactType, NLS::Core::Assets::ArtifactType::Prefab);
    EXPECT_EQ(prefabArtifact->loaderId, "prefab");

    const auto* meshArtifact = manifest.FindSubAsset("mesh:Body");
    ASSERT_NE(meshArtifact, nullptr);
    EXPECT_EQ(meshArtifact->artifactType, NLS::Core::Assets::ArtifactType::Mesh);
    EXPECT_EQ(manifest.FindSubAsset("mesh:Missing"), nullptr);
    EXPECT_EQ(manifest.dependencies.front().kind, NLS::Core::Assets::AssetDependencyKind::SourceFileHash);
}

TEST(AssetImportPipelineTests, ArtifactDatabasePersistsCentralIndexBySourceSubAssetAndStatus)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_artifact_db_" + NLS::Guid::New().ToString());
    const auto databasePath = root / "Library" / "ArtifactDB";

    const auto sourceId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = sourceId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = CurrentModelSceneImporterVersion();
    manifest.targetPlatform = "editor-windows";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back({
        sourceId,
        "prefab:Hero",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        "editor-windows",
        (std::filesystem::path("Library") / "Artifacts" /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(std::string(64u, '1'))).generic_string(),
        "sha256:prefab"
    });
    manifest.subAssets.push_back({
        sourceId,
        "mesh:Body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        (std::filesystem::path("Library") / "Artifacts" /
            NLS::Core::Assets::BuildArtifactStorageRelativePath(std::string(64u, '2'))).generic_string(),
        "sha256:mesh"
    });
    manifest.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::SourceFileHash,
        "Assets/Models/Hero.gltf",
        "sha256:source"
    });

    NLS::Core::Assets::ArtifactDatabase database;
    database.UpsertManifest(
        manifest,
        "Assets/Models/Hero.gltf",
        NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    database.MarkStatus(sourceId, NLS::Core::Assets::ArtifactRecordStatus::Importing);
    database.MarkStatus(sourceId, NLS::Core::Assets::ArtifactRecordStatus::UpToDate);

    ASSERT_TRUE(database.Save(databasePath));
    EXPECT_TRUE(std::filesystem::is_directory(databasePath));
    EXPECT_TRUE(std::filesystem::exists(databasePath / "data.mdb"));
    EXPECT_TRUE(std::filesystem::exists(databasePath / "lock.mdb"));
    EXPECT_FALSE(std::filesystem::exists(databasePath / "index.tsv"));

    NLS::Core::Assets::ArtifactDatabase loaded;
    ASSERT_TRUE(loaded.Load(databasePath));

    const auto* mesh = loaded.Find(sourceId, "mesh:Body", "editor-windows");
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->sourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(mesh->artifactPath, (std::filesystem::path("Library") / "Artifacts" /
        NLS::Core::Assets::BuildArtifactStorageRelativePath(std::string(64u, '2'))).generic_string());
    EXPECT_EQ(mesh->loaderId, "mesh");
    EXPECT_EQ(mesh->status, NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    EXPECT_EQ(mesh->importerVersion, CurrentModelSceneImporterVersion());
    EXPECT_EQ(mesh->dependencyCount, 1u);

    const auto sourceRecords = loaded.FindBySource(sourceId);
    ASSERT_EQ(sourceRecords.size(), 2u);
    EXPECT_EQ(loaded.GetStats().upToDateRecords, 2u);

    loaded.RemoveSource(sourceId);
    EXPECT_TRUE(loaded.FindBySource(sourceId).empty());

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactDatabaseRejectsArtifactPayloadPathsWithSourceExtensions)
{
    using namespace NLS::Core::Assets;

    const auto sourceId = AssetId(NLS::Guid::Parse("12121212-1212-4212-8212-121212121212"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = sourceId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = CurrentModelSceneImporterVersion();
    manifest.targetPlatform = "editor-windows";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back({
        sourceId,
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "editor-windows",
        "Library/Artifacts/Hero/prefab.prefab",
        "sha256:prefab"
    });
    manifest.subAssets.push_back({
        sourceId,
        "material:Body",
        ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/body.mat",
        "sha256:material"
    });
    manifest.subAssets.push_back({
        sourceId,
        "shader:Body",
        ArtifactType::Shader,
        "shader",
        "editor-windows",
        "Library/Artifacts/Hero/body.shader",
        "sha256:shader"
    });

    ArtifactDatabase database;
    database.UpsertManifest(manifest, "Assets/Models/Hero.gltf", ArtifactRecordStatus::UpToDate);

    EXPECT_EQ(database.Find(sourceId, "prefab:Hero", "editor-windows"), nullptr);
    EXPECT_EQ(database.Find(sourceId, "material:Body", "editor-windows"), nullptr);
    EXPECT_EQ(database.Find(sourceId, "shader:Body", "editor-windows"), nullptr);
    EXPECT_TRUE(database.FindBySource(sourceId).empty());
    EXPECT_EQ(database.GetStats().totalRecords, 0u);
}

TEST(AssetImportPipelineTests, ArtifactDatabaseRejectsExtensionlessSemanticPayloadNames)
{
    using namespace NLS::Core::Assets;

    const auto sourceId = AssetId(NLS::Guid::Parse("18181818-1818-4818-8818-181818181818"));

    ArtifactManifest manifest;
    manifest.sourceAssetId = sourceId;
    manifest.importerId = "shaderlab";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor-windows";
    manifest.primarySubAssetKey = "material:Hero";
    manifest.subAssets.push_back({
        sourceId,
        "material:Hero",
        ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/material",
        "fnv1a64:123"
    });

    ArtifactDatabase database;
    database.UpsertManifest(manifest, "Assets/Materials/Hero.mat", ArtifactRecordStatus::UpToDate);

    EXPECT_EQ(database.Find(sourceId, "material:Hero", "editor-windows"), nullptr);
    EXPECT_TRUE(database.FindBySource(sourceId).empty());
    EXPECT_EQ(database.GetStats().totalRecords, 0u)
        << "Artifact payload filenames are content hashes only; type names live in ArtifactDB/importer/container metadata.";
}

TEST(AssetImportPipelineTests, ArtifactDatabaseUsesMetadataNotBlobExtensionToDistinguishAssetKinds)
{
    using namespace NLS::Core::Assets;

    const auto materialId = AssetId(NLS::Guid::Parse("19191919-1919-4919-8919-191919191919"));
    const auto shaderId = AssetId(NLS::Guid::Parse("20202020-2020-4020-8020-202020202020"));
    const std::string sharedBlobPath =
        (std::filesystem::path("Library") / "Artifacts" /
            BuildArtifactStorageRelativePath(BuildArtifactStorageFileName("identical native container bytes"))).generic_string();

    ArtifactManifest materialManifest;
    materialManifest.sourceAssetId = materialId;
    materialManifest.importerId = "material";
    materialManifest.importerVersion = 1u;
    materialManifest.targetPlatform = "editor";
    materialManifest.primarySubAssetKey = "material:Hero";
    materialManifest.subAssets.push_back({
        materialId,
        "material:Hero",
        ArtifactType::Material,
        "material",
        "editor",
        sharedBlobPath,
        "sha256:shared"
    });

    ArtifactManifest shaderManifest;
    shaderManifest.sourceAssetId = shaderId;
    shaderManifest.importerId = "shader";
    shaderManifest.importerVersion = 1u;
    shaderManifest.targetPlatform = "editor";
    shaderManifest.primarySubAssetKey = "shader:Hero";
    shaderManifest.subAssets.push_back({
        shaderId,
        "shader:Hero",
        ArtifactType::Shader,
        "shader",
        "editor",
        sharedBlobPath,
        "sha256:shared"
    });

    ArtifactDatabase database;
    database.UpsertManifest(materialManifest, "Assets/Materials/Hero.mat", ArtifactRecordStatus::UpToDate);
    database.UpsertManifest(shaderManifest, "Assets/Shaders/Hero.shader", ArtifactRecordStatus::UpToDate);

    const auto* material = database.Find(materialId, "material:Hero", "editor");
    const auto* shader = database.Find(shaderId, "shader:Hero", "editor");
    ASSERT_NE(material, nullptr);
    ASSERT_NE(shader, nullptr);

    EXPECT_EQ(material->artifactPath, sharedBlobPath);
    EXPECT_EQ(shader->artifactPath, sharedBlobPath);
    EXPECT_FALSE(std::filesystem::path(sharedBlobPath).filename().has_extension());
    EXPECT_EQ(material->artifactType, ArtifactType::Material);
    EXPECT_EQ(material->loaderId, "material");
    EXPECT_EQ(material->sourcePath, "Assets/Materials/Hero.mat");
    EXPECT_EQ(material->importerId, "material");
    EXPECT_EQ(shader->artifactType, ArtifactType::Shader);
    EXPECT_EQ(shader->loaderId, "shader");
    EXPECT_EQ(shader->sourcePath, "Assets/Shaders/Hero.shader");
    EXPECT_EQ(shader->importerId, "shader");
    EXPECT_EQ(database.GetStats().totalRecords, 2u);
}

TEST(AssetImportPipelineTests, ArtifactDatabaseLoadDropsArtifactPayloadPathsWithSourceExtensions)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto databasePath = root / "Library" / "ArtifactDB";

    const auto sourceId = AssetId(NLS::Guid::Parse("34343434-3434-4434-8434-343434343434"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = sourceId;
    manifest.importerId = "material";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor-windows";
    manifest.primarySubAssetKey = "material:Hero";
    manifest.subAssets.push_back({
        sourceId,
        "material:Hero",
        ArtifactType::Material,
        "material",
        "editor-windows",
        "Library/Artifacts/Hero/body.mat",
        "sha256:material",
        "Hero Material"
    });

    ArtifactDatabase writer;
    writer.UpsertManifest(manifest, "Assets/Materials/Hero.mat", ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(writer.Save(databasePath));

    ArtifactDatabase database;
    ASSERT_TRUE(database.Load(databasePath));

    EXPECT_EQ(database.Find(sourceId, "material:Hero", "editor-windows"), nullptr);
    EXPECT_TRUE(database.FindBySource(sourceId).empty());
    EXPECT_EQ(database.GetStats().totalRecords, 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactDatabaseGrowsLmdbMapForLargeProjects)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto databasePath = root / "Library" / "ArtifactDB";

    const auto sourceId = AssetId(NLS::Guid::Parse("45454545-4545-4545-8545-454545454545"));
    ArtifactManifest manifest;
    manifest.sourceAssetId = sourceId;
    manifest.importerId = "material";
    manifest.importerVersion = 1u;
    manifest.targetPlatform = "editor-windows";
    manifest.primarySubAssetKey = "material:Large0000";

    constexpr size_t kRecordCount = 1200u;
    const std::string largeDisplayName(64u * 1024u, 'x');
    manifest.subAssets.reserve(kRecordCount);
    for (size_t index = 0u; index < kRecordCount; ++index)
    {
        const auto subAssetKey = "material:Large" + std::to_string(index);
        manifest.subAssets.push_back({
            sourceId,
            subAssetKey,
            ArtifactType::Material,
            "material",
            "editor-windows",
            (std::filesystem::path("Library") / "Artifacts" /
                BuildArtifactStorageRelativePath(BuildArtifactStorageFileName(subAssetKey))).generic_string(),
            "sha256:" + subAssetKey,
            largeDisplayName
        });
    }

    ArtifactDatabase writer;
    writer.UpsertManifest(manifest, "Assets/Materials/Large.mat", ArtifactRecordStatus::UpToDate);
    ASSERT_TRUE(writer.Save(databasePath));

    ArtifactDatabase loaded;
    ASSERT_TRUE(loaded.Load(databasePath));
    EXPECT_EQ(loaded.GetStats().totalRecords, kRecordCount);
    EXPECT_NE(loaded.Find(sourceId, "material:Large1199", "editor-windows"), nullptr);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactDatabaseUpsertsUpdateIndexIncrementally)
{
    using namespace NLS::Core::Assets;

    ArtifactDatabase database;

    constexpr size_t manifestCount = 128u;
    for (size_t index = 0u; index < manifestCount; ++index)
    {
        const auto sourceId = AssetId::New();
        ArtifactManifest manifest;
        manifest.sourceAssetId = sourceId;
        manifest.importerId = "scene-model";
        manifest.importerVersion = CurrentModelSceneImporterVersion();
        manifest.targetPlatform = "editor";
        manifest.primarySubAssetKey = "mesh:Body";
        manifest.subAssets.push_back({
            sourceId,
            "mesh:Body",
            ArtifactType::Mesh,
            "mesh",
            "editor",
            (std::filesystem::path("Library") / "Artifacts" /
                NLS::Core::Assets::BuildArtifactStorageRelativePath(
                    NLS::Core::Assets::BuildArtifactStorageFileName("mesh:Body:" + std::to_string(index)))).generic_string(),
            "sha256:" + std::to_string(index)
        });

        database.UpsertManifest(
            manifest,
            "Assets/Models/Hero" + std::to_string(index) + ".gltf",
            ArtifactRecordStatus::UpToDate);

        ASSERT_NE(database.Find(sourceId, "mesh:Body", "editor"), nullptr);
    }

    EXPECT_EQ(database.GetStats().totalRecords, manifestCount);
}

TEST(AssetImportPipelineTests, ArtifactWriterStagesPayloadsAndCommitsManifestAtomically)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-8ddd-dddddddddddd"));
    request.importerId = "scene-model";
    request.importerVersion = CurrentModelSceneImporterVersion();
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab",
        std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'}
    });
    request.artifacts.push_back({
        "mesh:HeroBody",
        ArtifactType::Mesh,
        "mesh",
        "Hero/body",
        std::vector<uint8_t>{'m', 'e', 's', 'h'}
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);

    ASSERT_TRUE(result.committed);
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(result.manifest.sourceAssetId, request.sourceAssetId);
    EXPECT_EQ(result.manifest.importerId, "scene-model");
    EXPECT_EQ(result.manifest.importerVersion, CurrentModelSceneImporterVersion());
    EXPECT_EQ(result.manifest.primarySubAssetKey, "prefab:Hero");
    ASSERT_EQ(result.manifest.subAssets.size(), 2u);

    const auto* prefab = result.manifest.FindSubAsset("prefab:Hero");
    ASSERT_NE(prefab, nullptr);
    EXPECT_EQ(prefab->artifactType, ArtifactType::Prefab);
    EXPECT_EQ(prefab->loaderId, "prefab");
    EXPECT_FALSE(std::filesystem::path(prefab->artifactPath).filename().has_extension());
    EXPECT_EQ(prefab->artifactPath.find("/prefab"), std::string::npos);
    const auto prefabBytes = ReadArtifactFile(commitRoot, *prefab);
    const auto prefabContainer = ReadNativeArtifactContainer(prefabBytes, ArtifactType::Prefab, 1u);
    ASSERT_TRUE(prefabContainer.has_value());
    EXPECT_EQ(prefabContainer->metadata.sourceAssetId, request.sourceAssetId);
    EXPECT_EQ(prefabContainer->metadata.subAssetKey, "prefab:Hero");
    EXPECT_EQ(prefabContainer->metadata.importerId, "scene-model");
    EXPECT_EQ(prefabContainer->metadata.importerVersion, CurrentModelSceneImporterVersion());
    EXPECT_EQ(prefabContainer->metadata.targetPlatform, "editor-windows");
    EXPECT_EQ(std::string(prefabContainer->payload.begin(), prefabContainer->payload.end()), "prefab");

    const auto* mesh = result.manifest.FindSubAsset("mesh:HeroBody");
    ASSERT_NE(mesh, nullptr);
    EXPECT_FALSE(std::filesystem::path(mesh->artifactPath).filename().has_extension());
    EXPECT_FALSE(std::filesystem::path(mesh->artifactPath).filename().has_extension());
    const auto meshBytes = ReadArtifactFile(commitRoot, *mesh);
    const auto meshContainer = ReadNativeArtifactContainer(meshBytes, ArtifactType::Mesh, 3u);
    ASSERT_TRUE(meshContainer.has_value());
    EXPECT_EQ(std::string(meshContainer->payload.begin(), meshContainer->payload.end()), "mesh");
    EXPECT_FALSE(std::filesystem::exists(stagingRoot / "Hero" / "prefab"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterFileNameDependsOnStoredContentNotRequestedPath)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto payload = std::vector<uint8_t>{'s', 'a', 'm', 'e'};
    const auto makeRequest = [&](const char* relativePath, uint32_t importerVersion)
    {
        ArtifactWriteRequest request;
        request.sourceAssetId = AssetId(NLS::Guid::Parse("56565656-5656-4656-8656-565656565656"));
        request.importerId = "test-importer";
        request.importerVersion = importerVersion;
        request.targetPlatform = "editor";
        request.primarySubAssetKey = "material:Body";
        request.artifacts.push_back({
            "material:Body",
            ArtifactType::Material,
            "material",
            "Body",
            relativePath,
            payload
        });
        return request;
    };

    const auto firstRequest = makeRequest("materials/body", 1u);
    const auto movedRequest = makeRequest("renamed/folder/body", 1u);
    const auto changedMetadataRequest = makeRequest("materials/body", 2u);

    ArtifactWriter writer(root / "Staging", root / "Committed");
    const auto firstResult = writer.WriteAndCommit(firstRequest, nullptr);
    ASSERT_TRUE(firstResult.committed);
    ASSERT_EQ(firstResult.manifest.subAssets.size(), 1u);

    const auto movedResult = writer.WriteAndCommit(movedRequest, nullptr);
    ASSERT_TRUE(movedResult.committed);
    ASSERT_EQ(movedResult.manifest.subAssets.size(), 1u);

    const auto changedMetadataResult = writer.WriteAndCommit(changedMetadataRequest, nullptr);
    ASSERT_TRUE(changedMetadataResult.committed);
    ASSERT_EQ(changedMetadataResult.manifest.subAssets.size(), 1u);

    const auto firstFileName = std::filesystem::path(firstResult.manifest.subAssets.front().artifactPath).filename();
    const auto movedFileName = std::filesystem::path(movedResult.manifest.subAssets.front().artifactPath).filename();
    const auto changedMetadataFileName =
        std::filesystem::path(changedMetadataResult.manifest.subAssets.front().artifactPath).filename();
    EXPECT_EQ(firstFileName, movedFileName);
    EXPECT_NE(firstFileName, changedMetadataFileName);
    EXPECT_FALSE(firstFileName.has_extension());

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterCommitsDuplicateContentAddressedPayloadOnce)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("abababab-abab-4bab-8bab-abababababab"));
    request.importerId = "texture";
    request.importerVersion = 4u;
    request.targetPlatform = "editor";
    request.primarySubAssetKey = "texture:Shared";
    request.artifacts.push_back({
        "texture:Shared",
        ArtifactType::Texture,
        "texture",
        "Shared",
        "textures/shared-a",
        std::vector<uint8_t>{'s', 'a', 'm', 'e'}
    });
    request.artifacts.push_back({
        "texture:Shared",
        ArtifactType::Texture,
        "texture",
        "Shared",
        "textures/shared-b",
        std::vector<uint8_t>{'s', 'a', 'm', 'e'}
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);

    ASSERT_TRUE(result.committed) << JoinDiagnosticSummaries(result.diagnostics);
    ASSERT_EQ(result.manifest.subAssets.size(), 2u);
    EXPECT_EQ(result.manifest.subAssets[0].artifactPath, result.manifest.subAssets[1].artifactPath);

    const auto relative = std::filesystem::path(result.manifest.subAssets[0].artifactPath);
    ASSERT_FALSE(relative.empty());
    EXPECT_EQ(relative.begin()->generic_string().size(), 2u);
    EXPECT_TRUE(std::filesystem::is_regular_file(commitRoot / relative));
    EXPECT_FALSE(std::filesystem::path(result.manifest.subAssets[0].artifactPath).filename().has_extension());

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterSkipsPhysicalCommitWhenContentBlobAlreadyExists)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("acacacac-acac-4cac-8cac-acacacacacac"));
    request.importerId = "model";
    request.importerVersion = 5u;
    request.targetPlatform = "editor";
    request.primarySubAssetKey = "prefab:Root";
    request.artifacts.push_back({
        "prefab:Root",
        ArtifactType::Prefab,
        "prefab",
        "Root",
        "prefabs/root",
        std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'}
    });
    request.artifacts.push_back({
        "material:Body",
        ArtifactType::Material,
        "material",
        "Body",
        "materials/body",
        std::vector<uint8_t>{'m', 'a', 't'}
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto firstResult = writer.WriteAndCommit(request, nullptr);
    ASSERT_TRUE(firstResult.committed) << JoinDiagnosticSummaries(firstResult.diagnostics);

    const auto secondResult = writer.WriteAndCommit(request, &firstResult.manifest);
    ASSERT_TRUE(secondResult.committed) << JoinDiagnosticSummaries(secondResult.diagnostics);
    ASSERT_EQ(secondResult.manifest.subAssets.size(), firstResult.manifest.subAssets.size());
    EXPECT_EQ(secondResult.manifest.subAssets[0].artifactPath, firstResult.manifest.subAssets[0].artifactPath);
    EXPECT_EQ(secondResult.manifest.subAssets[1].artifactPath, firstResult.manifest.subAssets[1].artifactPath);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRewritesSameSizeCorruptContentAddressedBlob)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("abab1212-3434-4cac-8cac-acacacacacac"));
    request.importerId = "model";
    request.importerVersion = 5u;
    request.targetPlatform = "editor";
    request.primarySubAssetKey = "prefab:Root";
    request.artifacts.push_back({
        "prefab:Root",
        ArtifactType::Prefab,
        "prefab",
        "Root",
        "prefabs/root",
        std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'}
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto firstResult = writer.WriteAndCommit(request, nullptr);
    ASSERT_TRUE(firstResult.committed) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_EQ(firstResult.manifest.subAssets.size(), 1u);

    const auto storedPath = commitRoot / std::filesystem::path(firstResult.manifest.subAssets[0].artifactPath);
    auto corrupted = ReadBinaryFile(storedPath);
    ASSERT_FALSE(corrupted.empty());
    corrupted.back() ^= 0x7Fu;
    WriteBinaryFile(storedPath, corrupted);
    ASSERT_EQ(std::filesystem::file_size(storedPath), corrupted.size());

    const auto secondResult = writer.WriteAndCommit(request, nullptr);
    ASSERT_TRUE(secondResult.committed) << JoinDiagnosticSummaries(secondResult.diagnostics);
    ASSERT_EQ(secondResult.manifest.subAssets.size(), 1u);
    EXPECT_EQ(secondResult.manifest.subAssets[0].artifactPath, firstResult.manifest.subAssets[0].artifactPath);

    const auto repairedBytes = ReadArtifactFile(commitRoot, secondResult.manifest.subAssets[0]);
    const auto repairedContainer = ReadNativeArtifactContainer(repairedBytes, ArtifactType::Prefab, 1u);
    ASSERT_TRUE(repairedContainer.has_value())
        << "Content-addressed blobs must be validated by content, not only by same byte size.";
    EXPECT_EQ(std::string(repairedContainer->payload.begin(), repairedContainer->payload.end()), "prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterReportsWriteAndCommitPerformanceCounters)
{
    using namespace NLS::Core::Assets;
    using NLS::Base::Profiling::PerformanceStageDomain;
    using NLS::Base::Profiling::PerformanceStageEntry;
    using NLS::Base::Profiling::PerformanceStageStats;
    using NLS::Base::Profiling::PerformanceStageStatsCapture;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("afafafaf-afaf-4afa-8faf-afafafafafaf"));
    request.importerId = "model";
    request.importerVersion = 5u;
    request.targetPlatform = "editor";
    request.primarySubAssetKey = "prefab:Root";
    request.artifacts.push_back({
        "prefab:Root",
        ArtifactType::Prefab,
        "prefab",
        "Root",
        "prefabs/root",
        std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'}
    });
    request.artifacts.push_back({
        "material:Body",
        ArtifactType::Material,
        "material",
        "Body",
        "materials/body",
        std::vector<uint8_t>{'m', 'a', 't'}
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto firstResult = writer.WriteAndCommit(request, nullptr);
    ASSERT_TRUE(firstResult.committed) << JoinDiagnosticSummaries(firstResult.diagnostics);

    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        const auto secondResult = writer.WriteAndCommit(request, &firstResult.manifest);
        ASSERT_TRUE(secondResult.committed) << JoinDiagnosticSummaries(secondResult.diagnostics);
    }

    const auto snapshot = stats.Snapshot();
    const auto stage = std::find_if(
        snapshot.stages.begin(),
        snapshot.stages.end(),
        [](const PerformanceStageEntry& entry)
        {
            return entry.domain == PerformanceStageDomain::Prefab &&
                entry.stageName == "WriteAndCommitArtifactPayloads";
        });
    ASSERT_NE(stage, snapshot.stages.end());
    EXPECT_EQ(stage->counters.at("artifactCount"), 2u);
    EXPECT_EQ(stage->counters.at("destinationAlreadyCurrentCount"), 2u);
    EXPECT_EQ(stage->counters.at("stagedCount"), 0u);
    EXPECT_EQ(stage->counters.at("commitPlanCount"), 0u);
    EXPECT_EQ(stage->counters.at("committedMoveCount"), 0u);
    EXPECT_EQ(stage->counters.at("contentPathReusedCount"), 2u);
    EXPECT_EQ(stage->counters.at("contentPathHashedCount"), 0u);
    EXPECT_EQ(stage->counters.at("storedPayloadBypassCount"), 2u);
    EXPECT_EQ(stage->counters.at("storedPayloadBuiltCount"), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRebuildsStoredPayloadWhenPayloadChanges)
{
    using namespace NLS::Core::Assets;
    using NLS::Base::Profiling::PerformanceStageDomain;
    using NLS::Base::Profiling::PerformanceStageEntry;
    using NLS::Base::Profiling::PerformanceStageStats;
    using NLS::Base::Profiling::PerformanceStageStatsCapture;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    auto makeRequest = [](std::vector<uint8_t> payload)
    {
        ArtifactWriteRequest request;
        request.sourceAssetId = AssetId(NLS::Guid::Parse("bfbfbfbf-bfbf-4bfb-8fbf-bfbfbfbfbfbf"));
        request.importerId = "model";
        request.importerVersion = 5u;
        request.targetPlatform = "editor";
        request.primarySubAssetKey = "material:Body";
        request.artifacts.push_back({
            "material:Body",
            ArtifactType::Material,
            "material",
            "Body",
            "materials/body",
            std::move(payload)
        });
        return request;
    };

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto firstResult = writer.WriteAndCommit(makeRequest({'o', 'l', 'd'}), nullptr);
    ASSERT_TRUE(firstResult.committed) << JoinDiagnosticSummaries(firstResult.diagnostics);

    ArtifactWriteResult secondResult;
    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        secondResult = writer.WriteAndCommit(makeRequest({'n', 'e', 'w'}), &firstResult.manifest);
    }
    ASSERT_TRUE(secondResult.committed) << JoinDiagnosticSummaries(secondResult.diagnostics);
    ASSERT_EQ(firstResult.manifest.subAssets.size(), 1u);
    ASSERT_EQ(secondResult.manifest.subAssets.size(), 1u);
    EXPECT_NE(firstResult.manifest.subAssets[0].artifactPath, secondResult.manifest.subAssets[0].artifactPath);

    const auto secondBytes = ReadArtifactFile(commitRoot, secondResult.manifest.subAssets[0]);
    const auto secondContainer = ReadNativeArtifactContainer(secondBytes, ArtifactType::Material, 1u);
    ASSERT_TRUE(secondContainer.has_value());
    EXPECT_EQ(std::string(secondContainer->payload.begin(), secondContainer->payload.end()), "new");

    const auto snapshot = stats.Snapshot();
    const auto stage = std::find_if(
        snapshot.stages.begin(),
        snapshot.stages.end(),
        [](const PerformanceStageEntry& entry)
        {
            return entry.domain == PerformanceStageDomain::Prefab &&
                entry.stageName == "WriteAndCommitArtifactPayloads";
        });
    ASSERT_NE(stage, snapshot.stages.end());
    EXPECT_EQ(stage->counters.at("storedPayloadBypassCount"), 0u);
    EXPECT_EQ(stage->counters.at("storedPayloadBuiltCount"), 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterBypassesStoredPayloadForUnchangedNativePayloads)
{
    using namespace NLS::Core::Assets;
    using NLS::Base::Profiling::PerformanceStageDomain;
    using NLS::Base::Profiling::PerformanceStageEntry;
    using NLS::Base::Profiling::PerformanceStageStats;
    using NLS::Base::Profiling::PerformanceStageStatsCapture;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("cfcfcfcf-cfcf-4cfc-8fcf-cfcfcfcfcfcf"));
    request.importerId = "model";
    request.importerVersion = 5u;
    request.targetPlatform = "editor";
    request.primarySubAssetKey = "material:Body";

    NativeArtifactMetadata sourceMetadata;
    sourceMetadata.artifactType = ArtifactType::Material;
    sourceMetadata.schemaName = "material";
    sourceMetadata.schemaVersion = 1u;
    sourceMetadata.sourceAssetId = request.sourceAssetId;
    sourceMetadata.subAssetKey = "material:Body";
    sourceMetadata.displayName = "Body";
    sourceMetadata.importerId = request.importerId;
    sourceMetadata.importerVersion = request.importerVersion;
    sourceMetadata.targetPlatform = request.targetPlatform;
    const auto nativePayload = WriteNativeArtifactContainer(
        std::move(sourceMetadata),
        std::vector<uint8_t>{'n', 'a', 't', 'i', 'v', 'e'});
    ASSERT_FALSE(nativePayload.empty());

    request.artifacts.push_back({
        "material:Body",
        ArtifactType::Material,
        "material",
        "Body",
        "materials/body",
        nativePayload
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto firstResult = writer.WriteAndCommit(request, nullptr);
    ASSERT_TRUE(firstResult.committed) << JoinDiagnosticSummaries(firstResult.diagnostics);

    PerformanceStageStats stats;
    {
        PerformanceStageStatsCapture capture(stats);
        const auto secondResult = writer.WriteAndCommit(request, &firstResult.manifest);
        ASSERT_TRUE(secondResult.committed) << JoinDiagnosticSummaries(secondResult.diagnostics);
        ASSERT_EQ(secondResult.manifest.subAssets.size(), 1u);
        EXPECT_EQ(secondResult.manifest.subAssets[0].artifactPath, firstResult.manifest.subAssets[0].artifactPath);
    }

    const auto snapshot = stats.Snapshot();
    const auto stage = std::find_if(
        snapshot.stages.begin(),
        snapshot.stages.end(),
        [](const PerformanceStageEntry& entry)
        {
            return entry.domain == PerformanceStageDomain::Prefab &&
                entry.stageName == "WriteAndCommitArtifactPayloads";
        });
    ASSERT_NE(stage, snapshot.stages.end());
    EXPECT_EQ(stage->counters.at("storedPayloadBypassCount"), 1u);
    EXPECT_EQ(stage->counters.at("storedPayloadBuiltCount"), 0u);
    EXPECT_EQ(stage->counters.at("nativeContainerDirectHashCount"), 1u);
    EXPECT_EQ(stage->counters.at("nativeContainerDirectReuseCount"), 1u);

    const auto storedBytes = ReadArtifactFile(commitRoot, firstResult.manifest.subAssets[0]);
    const auto storedContainer = ReadNativeArtifactContainer(storedBytes, ArtifactType::Material, 1u);
    ASSERT_TRUE(storedContainer.has_value());
    EXPECT_EQ(std::string(storedContainer->payload.begin(), storedContainer->payload.end()), "native");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRejectsCorruptNativePayloadBeforeReuseBypass)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("caca3434-cfcf-4cfc-8fcf-cfcfcfcfcfcf"));
    request.importerId = "model";
    request.importerVersion = 5u;
    request.targetPlatform = "editor";
    request.primarySubAssetKey = "material:Body";

    NativeArtifactMetadata sourceMetadata;
    sourceMetadata.artifactType = ArtifactType::Material;
    sourceMetadata.schemaName = "material";
    sourceMetadata.schemaVersion = 1u;
    sourceMetadata.sourceAssetId = request.sourceAssetId;
    sourceMetadata.subAssetKey = "material:Body";
    sourceMetadata.displayName = "Body";
    sourceMetadata.importerId = request.importerId;
    sourceMetadata.importerVersion = request.importerVersion;
    sourceMetadata.targetPlatform = request.targetPlatform;
    auto nativePayload = WriteNativeArtifactContainer(
        std::move(sourceMetadata),
        std::vector<uint8_t>{'n', 'a', 't', 'i', 'v', 'e'});
    ASSERT_FALSE(nativePayload.empty());

    request.artifacts.push_back({
        "material:Body",
        ArtifactType::Material,
        "material",
        "Body",
        "materials/body",
        nativePayload
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto firstResult = writer.WriteAndCommit(request, nullptr);
    ASSERT_TRUE(firstResult.committed) << JoinDiagnosticSummaries(firstResult.diagnostics);

    request.artifacts[0].payload.back() ^= 0x7Fu;
    const auto corruptResult = writer.WriteAndCommit(request, &firstResult.manifest);
    EXPECT_FALSE(corruptResult.committed)
        << "Corrupt native-container inputs must not bypass stored payload build by trusting prefix metadata.";
    EXPECT_TRUE(ContainsDiagnosticCode(corruptResult.diagnostics, "artifact-container-write-failed"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, NativeArtifactContainerViewPreservesValidationAndAvoidsPayloadCopy)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;
    using NLS::Core::Assets::ArtifactType;
    using NLS::Core::Assets::ClearArtifactLoadTelemetry;
    using NLS::Core::Assets::ReadNativeArtifactContainerView;
    using NLS::Core::Assets::SnapshotArtifactLoadTelemetry;

    const std::vector<uint8_t> payload = {
        'l', 'o', 'w', '-', 'c', 'o', 'p', 'y', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd'
    };

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab";
    metadata.schemaVersion = 1u;
    metadata.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
        "test-importer",
        "1"
    });

    const auto bytes = NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
    ASSERT_FALSE(bytes.empty());

    ClearArtifactLoadTelemetry();
    const auto view = ReadNativeArtifactContainerView(bytes, ArtifactType::Prefab, 1u);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->metadata.artifactType, ArtifactType::Prefab);
    EXPECT_EQ(view->payloadSize, payload.size());
    ASSERT_NE(view->payloadData, nullptr);
    const auto payloadOffset = static_cast<size_t>(view->payloadData - bytes.data());
    EXPECT_LT(payloadOffset, bytes.size());
    EXPECT_EQ(payloadOffset + view->payloadSize, bytes.size())
        << "The low-copy view must point into the single file buffer instead of allocating a second payload vector.";
    const bool payloadMatches = std::equal(payload.begin(), payload.end(), view->payloadData);
    EXPECT_TRUE(payloadMatches);

    const auto telemetry = SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(telemetry, ArtifactLoadTelemetryStage::NativeContainerParseHash), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(telemetry, ArtifactLoadTelemetryStage::NativeArtifactLowCopyView), 1u)
        << "Phase 6d needs a counter proving the hot artifact path avoided the redundant payload copy.";

    auto corrupted = bytes;
    ASSERT_FALSE(corrupted.empty());
    corrupted.back() ^= 0x7Fu;
    const auto corruptView = ReadNativeArtifactContainerView(corrupted, ArtifactType::Prefab, 1u);
    EXPECT_FALSE(corruptView.has_value())
        << "The low-copy view must keep native container payload hash validation, not trust mapped bytes blindly.";

    auto corruptedDependencyHash = bytes;
    const std::string dependencyHashPrefix = "DEPENDENCY_HASH=fnv1a64:";
    const auto dependencyHashBegin = std::search(
        corruptedDependencyHash.begin(),
        corruptedDependencyHash.end(),
        dependencyHashPrefix.begin(),
        dependencyHashPrefix.end());
    ASSERT_NE(dependencyHashBegin, corruptedDependencyHash.end());
    const auto dependencyHashDigit = dependencyHashBegin + static_cast<std::ptrdiff_t>(dependencyHashPrefix.size());
    ASSERT_NE(dependencyHashDigit, corruptedDependencyHash.end());
    *dependencyHashDigit = *dependencyHashDigit == '0' ? '1' : '0';
    const auto corruptDependencyHashView =
        ReadNativeArtifactContainerView(corruptedDependencyHash, ArtifactType::Prefab, 1u);
    EXPECT_FALSE(corruptDependencyHashView.has_value())
        << "The low-copy view must also keep native container dependency hash validation.";
}

TEST(AssetImportPipelineTests, NativeArtifactPayloadTextFromFileValidatesAndAvoidsPayloadVectorCopy)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;
    using NLS::Core::Assets::ArtifactType;
    using NLS::Core::Assets::ClearArtifactLoadTelemetry;
    using NLS::Core::Assets::ReadNativeArtifactPayloadTextFromFile;
    using NLS::Core::Assets::SnapshotArtifactLoadTelemetry;

    const auto root = MakeImportTestRoot();
    ScopedImportTestRoot cleanup(root);
    const auto path = root / "Library" / "Artifacts" / "prefab.nasset";

    const std::string payloadText = "NULLUS_OBJECT_GRAPH=1\nobject prefab\n";
    const std::vector<uint8_t> payload(payloadText.begin(), payloadText.end());

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab";
    metadata.schemaVersion = 1u;
    metadata.dependencies.push_back({
        NLS::Core::Assets::AssetDependencyKind::ImporterVersion,
        "scene-model",
        "1"
    });

    const auto bytes = NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
    ASSERT_FALSE(bytes.empty());
    WriteBinaryFile(path, bytes);

    ClearArtifactLoadTelemetry();
    const auto payloadFromFile = ReadNativeArtifactPayloadTextFromFile(path, ArtifactType::Prefab, 1u);
    ASSERT_TRUE(payloadFromFile.has_value());
    EXPECT_EQ(payloadFromFile->payload, payloadText);
    ASSERT_EQ(payloadFromFile->metadata.dependencies.size(), 1u);
    EXPECT_EQ(payloadFromFile->metadata.dependencies[0].value, "scene-model");

    const auto telemetry = SnapshotArtifactLoadTelemetry();
    EXPECT_GE(CountArtifactTelemetryStage(telemetry, ArtifactLoadTelemetryStage::NativeArtifactFileRead), 1u);
    EXPECT_GE(CountArtifactTelemetryStage(telemetry, ArtifactLoadTelemetryStage::NativeContainerParseHash), 1u);
    EXPECT_EQ(CountArtifactTelemetryStage(telemetry, ArtifactLoadTelemetryStage::NativeArtifactPayloadCopy), 0u)
        << "Prefab artifact loads should read text payloads directly instead of allocating a payload vector first.";

    auto corrupted = bytes;
    ASSERT_FALSE(corrupted.empty());
    corrupted.back() ^= 0x7Fu;
    WriteBinaryFile(path, corrupted);
    EXPECT_FALSE(ReadNativeArtifactPayloadTextFromFile(path, ArtifactType::Prefab, 1u).has_value())
        << "The direct text path must keep payload hash validation.";
}

TEST(AssetImportPipelineTests, NativeArtifactContainerViewRejectsMissingPayloadWithDiagnostics)
{
    using NLS::Core::Assets::ArtifactType;
    using NLS::Core::Assets::ReadNativeArtifactContainerView;

    const std::vector<uint8_t> payload = { 'o', 'k' };
    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Mesh;
    metadata.schemaName = "mesh";
    metadata.schemaVersion = 3u;
    const auto bytes = NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
    ASSERT_GT(bytes.size(), payload.size());

    auto truncated = bytes;
    truncated.resize(truncated.size() - payload.size());

    std::string diagnostics;
    const auto view = ReadNativeArtifactContainerView(
        truncated,
        ArtifactType::Mesh,
        3u,
        &diagnostics);
    EXPECT_FALSE(view.has_value());
    EXPECT_NE(diagnostics.find("payload"), std::string::npos)
        << "Missing payload failures must be diagnosable so corrupt artifacts do not look like generic load misses.";
}

TEST(AssetImportPipelineTests, ArtifactWriterKeepsPreviousManifestWhenStagedPayloadFails)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();

    ArtifactManifest previous;
    previous.sourceAssetId = AssetId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"));
    previous.primarySubAssetKey = "prefab:Previous";
    previous.subAssets.push_back({
        previous.sourceAssetId,
        "prefab:Previous",
        ArtifactType::Prefab,
        "prefab",
        (root / "Committed" / "Previous" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d").string(),
        "previous"
    });

    ArtifactWriteRequest request;
    request.sourceAssetId = previous.sourceAssetId;
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Broken";
    request.artifacts.push_back({
        "prefab:Broken",
        ArtifactType::Prefab,
        "prefab",
        "../escape.prefab",
        std::vector<uint8_t>{'b', 'a', 'd'}
    });

    ArtifactWriter writer(root / "Staging", root / "Committed");
    const auto result = writer.WriteAndCommit(request, &previous);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-path-escape");
    EXPECT_EQ(result.manifest.primarySubAssetKey, previous.primarySubAssetKey);
    ASSERT_EQ(result.manifest.subAssets.size(), 1u);
    EXPECT_EQ(result.manifest.subAssets[0].subAssetKey, "prefab:Previous");
    EXPECT_FALSE(std::filesystem::exists(root / "5bae15e32d00d540ec65f9406d425a4033b591ab4bea5f2a0f9c1b95cc861fc1"));

    std::filesystem::remove_all(root);
}

#if defined(_WIN32)
TEST(AssetImportPipelineTests, ArtifactWriterRejectsWindowsRootedPayloadPaths)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("fafafafa-fafa-4afa-8afa-fafafafafafa"));
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Broken";
    request.artifacts.push_back({
        "prefab:Broken",
        ArtifactType::Prefab,
        "prefab",
        "C:/escaped/prefab",
        std::vector<uint8_t>{'b', 'a', 'd'}
    });

    ArtifactWriter writer(root / "Staging", root / "Committed");
    const auto result = writer.WriteAndCommit(request, nullptr);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-path-escape");

    std::filesystem::remove_all(root);
}
#endif

TEST(AssetImportPipelineTests, ArtifactWriterPreservesCommittedFilesWhenCommitFails)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    WriteTextFile(commitRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d", "old-prefab");

    ArtifactManifest previous;
    previous.sourceAssetId = AssetId(NLS::Guid::Parse("abababab-abab-4aba-8bab-abababababab"));
    previous.primarySubAssetKey = "prefab:Previous";
    previous.subAssets.push_back({
        previous.sourceAssetId,
        "prefab:Previous",
        ArtifactType::Prefab,
        "prefab",
        (commitRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d").string(),
        "previous"
    });

    ArtifactWriteRequest request;
    request.sourceAssetId = previous.sourceAssetId;
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });
    request.artifacts.push_back({
        "mesh:Bad",
        ArtifactType::Mesh,
        "mesh",
        "Hero/bad",
        std::vector<uint8_t>{'b', 'a', 'd'}
    });
    std::filesystem::create_directories(
        commitRoot / BuildContentAddressedArtifactRelativePath(request, request.artifacts.back()));

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto result = writer.WriteAndCommit(request, &previous);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-commit-failed");
    EXPECT_EQ(result.manifest.primarySubAssetKey, previous.primarySubAssetKey);
    EXPECT_EQ(ReadTextFile(commitRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d"), "old-prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRollsBackCommittedFilesWhenCancelledDuringCommit)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    WriteTextFile(commitRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d", "old-prefab");
    WriteTextFile(commitRoot / "Hero" / "7e0aaf65f74245f291bdf6a0c3f6c4e8", "old-mesh");

    ArtifactManifest previous;
    previous.sourceAssetId = AssetId(NLS::Guid::Parse("acacacac-acac-4aca-8cac-acacacacacac"));
    previous.primarySubAssetKey = "prefab:Previous";
    previous.subAssets.push_back({
        previous.sourceAssetId,
        "prefab:Previous",
        ArtifactType::Prefab,
        "prefab",
        (commitRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d").string(),
        "previous"
    });

    ArtifactWriteRequest request;
    request.sourceAssetId = previous.sourceAssetId;
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });
    request.artifacts.push_back({
        "mesh:Body",
        ArtifactType::Mesh,
        "mesh",
        "Hero/body",
        std::vector<uint8_t>{'n', 'e', 'w', '-', 'm', 'e', 's', 'h'}
    });

    CancelAfterChecks cancellation(7u);
    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto result = writer.WriteAndCommit(request, &previous, &cancellation);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-write-cancelled");
    EXPECT_EQ(result.manifest.primarySubAssetKey, previous.primarySubAssetKey);
    EXPECT_EQ(ReadTextFile(commitRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d"), "old-prefab");
    EXPECT_EQ(ReadTextFile(commitRoot / "Hero" / "7e0aaf65f74245f291bdf6a0c3f6c4e8"), "old-mesh");
    EXPECT_FALSE(std::filesystem::exists(stagingRoot));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRejectsUnsafeRootsBeforeDeletingCommittedArtifacts)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto committedRoot = root / "Committed";
    WriteTextFile(committedRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d", "committed");

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("adadadad-adad-4ada-8dad-adadadadadad"));
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });

    ArtifactWriter writer(committedRoot, committedRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-root-unsafe");
    EXPECT_EQ(ReadTextFile(committedRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d"), "committed");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRejectsRollbackRootThatWouldDeleteCommittedArtifacts)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Artifacts";
    const auto committedRoot = root / "Artifacts.rollback";
    WriteTextFile(committedRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d", "committed");

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("aeaeaeae-aeae-4aea-8eae-aeaeaeaeaeae"));
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });

    ArtifactWriter writer(stagingRoot, committedRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-root-unsafe");
    EXPECT_EQ(ReadTextFile(committedRoot / "Hero" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d"), "committed");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, GltfImporterExtractsSceneContractData)
{
    const std::string gltf = R"(
    {
      "asset": { "version": "2.0" },
      "buffers": [
        { "uri": "Hero.bin", "byteLength": 24 },
        { "uri": "data:application/octet-stream;base64,AAAA", "byteLength": 4 }
      ],
      "images": [
        { "uri": "Textures/Hero_BaseColor.png", "name": "BaseColor" },
        { "bufferView": 0, "mimeType": "image/png", "name": "EmbeddedNormal" }
      ],
      "textures": [
        { "source": 0 },
        { "source": 1 }
      ],
      "materials": [
        {
          "name": "HeroMaterial",
          "doubleSided": true,
          "pbrMetallicRoughness": {
            "baseColorTexture": { "index": 0 },
            "metallicRoughnessTexture": { "index": 1 }
          },
          "normalTexture": { "index": 1 }
        }
      ],
      "meshes": [
        {
          "name": "Body",
          "primitives": [
            {
              "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 },
              "material": 0,
              "targets": [{ "POSITION": 3, "NORMAL": 4 }]
            }
          ]
        }
      ],
      "nodes": [
        { "name": "Root", "children": [1] },
        { "name": "BodyNode", "mesh": 0, "skin": 0 }
      ],
      "skins": [
        { "name": "HeroSkin", "skeleton": 0, "joints": [0, 1] }
      ],
      "animations": [
        {
          "name": "Idle",
          "channels": [
            { "sampler": 0, "target": { "node": 1, "path": "translation" } }
          ],
          "samplers": [
            { "input": 0, "output": 1, "interpolation": "LINEAR" }
          ]
        }
      ]
    })";

    const auto scene = NLS::Render::Assets::ImportGltfSceneJson(
        gltf,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("88888888-8888-4888-8888-888888888888")),
        "HeroGltf");

    EXPECT_TRUE(scene.diagnostics.empty());
    EXPECT_EQ(scene.sceneKey, "HeroGltf");
    ASSERT_EQ(scene.buffers.size(), 2u);
    EXPECT_EQ(scene.buffers[0].uri, "Hero.bin");
    EXPECT_FALSE(scene.buffers[0].embedded);
    EXPECT_TRUE(scene.buffers[1].embedded);

    ASSERT_EQ(scene.textures.size(), 2u);
    EXPECT_EQ(scene.textures[0].uri, "Textures/Hero_BaseColor.png");
    EXPECT_FALSE(scene.textures[0].embedded);
    EXPECT_TRUE(scene.textures[1].embedded);
    EXPECT_EQ(scene.textures[1].mimeType, "image/png");

    ASSERT_EQ(scene.materials.size(), 1u);
    EXPECT_EQ(scene.materials[0].name, "HeroMaterial");
    EXPECT_EQ(scene.materials[0].pbrWorkflow, "metallic-roughness");
    EXPECT_EQ(scene.materials[0].baseColorTextureKey, "image/0");
    EXPECT_EQ(scene.materials[0].metallicRoughnessTextureKey, "image/1");
    EXPECT_EQ(scene.materials[0].normalTextureKey, "image/1");
    EXPECT_TRUE(scene.materials[0].doubleSided);

    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].name, "Body");
    EXPECT_EQ(scene.meshes[0].primitiveCount, 1u);
    EXPECT_EQ(scene.meshes[0].morphTargetCount, 1u);
    ASSERT_EQ(scene.meshes[0].primitives.size(), 1u);
    EXPECT_EQ(scene.meshes[0].primitives[0].materialKey, "material/0");
    EXPECT_TRUE(Contains(scene.meshes[0].attributes, "POSITION"));
    EXPECT_TRUE(Contains(scene.meshes[0].attributes, "NORMAL"));
    EXPECT_TRUE(Contains(scene.meshes[0].attributes, "TEXCOORD_0"));

    ASSERT_EQ(scene.nodes.size(), 2u);
    EXPECT_EQ(scene.nodes[1].meshKey, "mesh/0");
    EXPECT_EQ(scene.nodes[1].skinKey, "skin/0");

    ASSERT_EQ(scene.skins.size(), 1u);
    EXPECT_EQ(scene.skins[0].skeletonKey, "node/0");
    EXPECT_EQ(scene.skins[0].joints.size(), 2u);

    ASSERT_EQ(scene.animations.size(), 1u);
    EXPECT_EQ(scene.animations[0].name, "Idle");
    ASSERT_EQ(scene.animations[0].targets.size(), 1u);
    EXPECT_EQ(scene.animations[0].targets[0], "node/1:translation");

    ASSERT_EQ(scene.morphTargets.size(), 1u);
    EXPECT_EQ(scene.morphTargets[0].meshKey, "mesh/0");
}

TEST(AssetImportPipelineTests, GlbImporterExtractsPayloadAccessorsAndVertexStreams)
{
    const std::string json = R"(
    {
      "asset": { "version": "2.0" },
      "buffers": [
        { "byteLength": 36 }
      ],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 24, "byteStride": 12, "target": 34962 },
        { "buffer": 0, "byteOffset": 24, "byteLength": 6, "target": 34963 },
        { "buffer": 0, "byteOffset": 32, "byteLength": 4 }
      ],
      "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 2, "type": "VEC3" },
        { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
      ],
      "images": [
        { "bufferView": 2, "mimeType": "image/png", "name": "EmbeddedImage" }
      ],
      "meshes": [
        {
          "name": "Triangle",
          "primitives": [
            {
              "attributes": { "POSITION": 0 },
              "indices": 1
            }
          ]
        }
      ]
    })";

    std::vector<uint8_t> binaryChunk(36u, 0u);
    binaryChunk[0] = 0x10u;
    binaryChunk[24] = 0x00u;
    binaryChunk[25] = 0x01u;
    binaryChunk[32] = 0x89u;
    binaryChunk[33] = 0x50u;
    binaryChunk[34] = 0x4Eu;
    binaryChunk[35] = 0x47u;

    const auto scene = NLS::Render::Assets::ImportGltfSceneBytes(
        MakeGlb(json, binaryChunk),
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")),
        "TriangleGlb");

    EXPECT_TRUE(scene.diagnostics.empty());
    ASSERT_EQ(scene.buffers.size(), 1u);
    EXPECT_TRUE(scene.buffers[0].embedded);
    EXPECT_EQ(scene.buffers[0].byteLength, 36u);
    EXPECT_EQ(scene.buffers[0].embeddedByteLength, 36u);

    ASSERT_EQ(scene.bufferViews.size(), 3u);
    EXPECT_EQ(scene.bufferViews[0].sourceKey, "bufferView/0");
    EXPECT_EQ(scene.bufferViews[0].bufferKey, "buffer/0");
    EXPECT_EQ(scene.bufferViews[0].byteOffset, 0u);
    EXPECT_EQ(scene.bufferViews[0].byteLength, 24u);
    EXPECT_EQ(scene.bufferViews[0].byteStride, 12u);
    EXPECT_EQ(scene.bufferViews[0].target, 34962u);

    ASSERT_EQ(scene.accessors.size(), 2u);
    EXPECT_EQ(scene.accessors[0].sourceKey, "accessor/0");
    EXPECT_EQ(scene.accessors[0].bufferViewKey, "bufferView/0");
    EXPECT_EQ(scene.accessors[0].componentType, 5126u);
    EXPECT_EQ(scene.accessors[0].count, 2u);
    EXPECT_EQ(scene.accessors[0].type, "VEC3");
    EXPECT_EQ(scene.accessors[1].bufferViewKey, "bufferView/1");
    EXPECT_EQ(scene.accessors[1].componentType, 5123u);

    ASSERT_EQ(scene.textures.size(), 1u);
    EXPECT_TRUE(scene.textures[0].embedded);
    EXPECT_EQ(scene.textures[0].bufferViewKey, "bufferView/2");
    EXPECT_EQ(scene.textures[0].mimeType, "image/png");

    ASSERT_EQ(scene.meshes.size(), 1u);
    ASSERT_EQ(scene.meshes[0].primitives.size(), 1u);
    ASSERT_EQ(scene.meshes[0].primitives[0].vertexStreams.size(), 1u);
    EXPECT_EQ(scene.meshes[0].primitives[0].vertexStreams[0].semantic, "POSITION");
    EXPECT_EQ(scene.meshes[0].primitives[0].vertexStreams[0].accessorKey, "accessor/0");
    EXPECT_EQ(scene.meshes[0].primitives[0].indexAccessorKey, "accessor/1");
}

TEST(AssetImportPipelineTests, GlbImporterReportsInvalidContainerDiagnostics)
{
    const auto sourceAsset = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc"));

    const auto invalidHeader = NLS::Render::Assets::ImportGltfSceneBytes(
        {0x67u, 0x6Cu, 0x54u},
        sourceAsset,
        "InvalidHeader");
    ASSERT_EQ(invalidHeader.diagnostics.size(), 1u);
    EXPECT_EQ(invalidHeader.diagnostics[0].code, "glb-invalid-header");

    std::vector<uint8_t> badLength;
    AppendU32(badLength, 0x46546C67u);
    AppendU32(badLength, 2u);
    AppendU32(badLength, 256u);
    AppendU32(badLength, 0u);
    AppendU32(badLength, 0x4E4F534Au);
    const auto outOfRangeLength = NLS::Render::Assets::ImportGltfSceneBytes(
        badLength,
        sourceAsset,
        "BadLength");
    ASSERT_EQ(outOfRangeLength.diagnostics.size(), 1u);
    EXPECT_EQ(outOfRangeLength.diagnostics[0].code, "glb-length-out-of-range");

    std::vector<uint8_t> badChunk;
    AppendU32(badChunk, 0x46546C67u);
    AppendU32(badChunk, 2u);
    AppendU32(badChunk, 20u);
    AppendU32(badChunk, 16u);
    AppendU32(badChunk, 0x4E4F534Au);
    const auto outOfRangeChunk = NLS::Render::Assets::ImportGltfSceneBytes(
        badChunk,
        sourceAsset,
        "BadChunk");
    ASSERT_EQ(outOfRangeChunk.diagnostics.size(), 1u);
    EXPECT_EQ(outOfRangeChunk.diagnostics[0].code, "glb-chunk-out-of-range");
}

TEST(AssetImportPipelineTests, ObjImporterConvertsParserMeshesAndReportsFormatLimits)
{
    FakeModelParser parser;
    parser.meshCount = 2u;
    parser.materialNames = {"BodyMat", "BladeMat"};

    const auto scene = NLS::Render::Assets::ImportParserModelScene(
        parser,
        "Assets/Models/Hero.obj",
        NLS::Render::Assets::SceneModelSourceFormat::Obj,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("99999999-9999-4999-8999-999999999999")),
        "HeroObj");

    EXPECT_TRUE(parser.loadCalled);
    EXPECT_EQ(scene.sceneKey, "HeroObj");
    EXPECT_EQ(scene.meshes.size(), 2u);
    EXPECT_EQ(scene.materials.size(), 2u);
    EXPECT_EQ(scene.materials[0].name, "BodyMat");
    EXPECT_EQ(scene.materials[1].name, "BladeMat");
    ASSERT_EQ(scene.diagnostics.size(), 1u);
    EXPECT_EQ(scene.diagnostics[0].code, "obj-no-skeleton-animation-support");
}

TEST(AssetImportPipelineTests, ObjImporterPreservesMtlMaterialsAndTextureDependencies)
{
    DetailedFakeModelParser parser;
    parser.meshCount = 1u;
    parser.materialNames = {"BodyPaint"};

    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "obj/mesh/body";
    mesh.name = "Body";
    mesh.primitiveCount = 1u;
    parser.meshes.push_back(mesh);

    NLS::Render::Assets::ImportedSceneNamedRecord diffuse;
    diffuse.sourceKey = "mtl/texture/body-diffuse";
    diffuse.name = "body_diffuse";
    diffuse.uri = "Textures/body_diffuse.png";
    parser.textures.push_back(diffuse);

    NLS::Render::Assets::ImportedSceneNamedRecord normal;
    normal.sourceKey = "mtl/texture/body-normal";
    normal.name = "body_normal";
    normal.uri = "Textures/body_normal.png";
    parser.textures.push_back(normal);

    NLS::Render::Assets::ImportedSceneNamedRecord material;
    material.sourceKey = "mtl/material/BodyPaint";
    material.name = "BodyPaint";
    material.materialChannels.push_back({"diffuse", diffuse.sourceKey, {0.8, 0.7, 0.6}, false, 0.0});
    material.materialChannels.push_back({"normal", normal.sourceKey, {}, false, 0.0});
    material.materialChannels.push_back({"shininess", {}, {}, true, 64.0});
    material.materialChannels.push_back({"opacity", {}, {}, true, 0.75});
    parser.materials.push_back(material);

    const auto scene = NLS::Render::Assets::ImportParserModelScene(
        parser,
        "Assets/Models/Hero.obj",
        NLS::Render::Assets::SceneModelSourceFormat::Obj,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("12121212-1212-4212-8212-121212121212")),
        "HeroObjMtl");

    EXPECT_TRUE(parser.loadCalled);
    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].sourceKey, "obj/mesh/body");
    ASSERT_EQ(scene.textures.size(), 2u);
    EXPECT_EQ(scene.textures[0].uri, "Textures/body_diffuse.png");
    EXPECT_EQ(scene.textures[1].uri, "Textures/body_normal.png");

    ASSERT_EQ(scene.materials.size(), 1u);
    const auto* diffuseChannel = FindMaterialChannel(scene.materials[0], "diffuse");
    ASSERT_NE(diffuseChannel, nullptr);
    EXPECT_EQ(diffuseChannel->textureKey, "mtl/texture/body-diffuse");
    ASSERT_EQ(diffuseChannel->values.size(), 3u);
    EXPECT_DOUBLE_EQ(diffuseChannel->values[0], 0.8);

    const auto* normalChannel = FindMaterialChannel(scene.materials[0], "normal");
    ASSERT_NE(normalChannel, nullptr);
    EXPECT_EQ(normalChannel->textureKey, "mtl/texture/body-normal");

    const auto* opacityChannel = FindMaterialChannel(scene.materials[0], "opacity");
    ASSERT_NE(opacityChannel, nullptr);
    EXPECT_TRUE(opacityChannel->hasScalar);
    EXPECT_DOUBLE_EQ(opacityChannel->scalar, 0.75);

    const auto generated = NLS::Render::Assets::GenerateSceneSubAssets(scene);
    const auto keys = ExtractKeys(generated);
    EXPECT_TRUE(Contains(keys, "texture:mtl/texture/body-diffuse"));
    EXPECT_TRUE(Contains(keys, "texture:mtl/texture/body-normal"));
    ASSERT_EQ(scene.diagnostics.size(), 1u);
    EXPECT_EQ(scene.diagnostics[0].code, "obj-no-skeleton-animation-support");
}

TEST(AssetImportPipelineTests, AssimpParserReportsMaterialTextureDependencies)
{
    const auto root = MakeImportTestRoot();
    WriteTextFile(root / "Assets" / "Textures" / "HeroDiffuse.png", "texture-bytes");
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
map_Kd ../Textures/HeroDiffuse.png
)");
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.obj",
        R"(
mtllib Hero.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> externalDependencies;
    ASSERT_TRUE(parser.LoadModelData(
        (root / "Assets" / "Models" / "Hero.obj").string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        &externalDependencies));

    EXPECT_TRUE(Contains(externalDependencies, "../Textures/HeroDiffuse.png"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, AssimpParserPopulatesImportedSceneMaterialChannelsAndTextureUris)
{
    const auto root = MakeImportTestRoot();
    WriteTextFile(root / "Assets" / "Textures" / "HeroDiffuse.png", "texture-bytes");
    WriteTextFile(root / "Assets" / "Textures" / "HeroNormal.png", "texture-bytes");
    WriteTextFile(root / "Assets" / "Textures" / "HeroShininess.png", "texture-bytes");
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
Kd 0.8 0.7 0.6
Ks 0.3 0.4 0.5
Ke 0.1 0.2 0.3
Ns 32
d 0.65
map_Kd ../Textures/HeroDiffuse.png
map_Bump ../Textures/HeroNormal.png
map_Ns ../Textures/HeroShininess.png
)");
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.obj",
        R"(
mtllib Hero.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> externalDependencies;
    ASSERT_TRUE(parser.LoadModelData(
        (root / "Assets" / "Models" / "Hero.obj").string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        &externalDependencies));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929292"));
    scene.sceneKey = "HeroObj";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        root / "Assets" / "Models" / "Hero.obj",
        NLS::Render::Assets::SceneModelSourceFormat::Obj,
        scene));

    const auto material = std::find_if(
        scene.materials.begin(),
        scene.materials.end(),
        [](const NLS::Render::Assets::ImportedSceneNamedRecord& candidate)
        {
            return candidate.name == "HeroMaterial";
        });
    ASSERT_NE(material, scene.materials.end());

    ASSERT_EQ(scene.textures.size(), 3u);
    EXPECT_EQ(scene.textures[0].sourceKey, "parser/texture/0");
    EXPECT_EQ(scene.textures[0].uri, "../Textures/HeroDiffuse.png");
    EXPECT_EQ(scene.textures[1].sourceKey, "parser/texture/1");
    EXPECT_EQ(scene.textures[1].uri, "../Textures/HeroNormal.png");
    EXPECT_EQ(scene.textures[2].sourceKey, "parser/texture/2");
    EXPECT_EQ(scene.textures[2].uri, "../Textures/HeroShininess.png");

    const auto* diffuse = FindMaterialChannel(*material, "diffuse");
    ASSERT_NE(diffuse, nullptr);
    EXPECT_EQ(diffuse->textureKey, "parser/texture/0");
    ASSERT_GE(diffuse->values.size(), 3u);
    EXPECT_NEAR(diffuse->values[0], 0.8, 0.00001);
    EXPECT_NEAR(diffuse->values[1], 0.7, 0.00001);
    EXPECT_NEAR(diffuse->values[2], 0.6, 0.00001);

    const auto* bump = FindMaterialChannel(*material, "bump");
    ASSERT_NE(bump, nullptr);
    EXPECT_EQ(bump->textureKey, "parser/texture/1");

    const auto* emissive = FindMaterialChannel(*material, "emissive");
    ASSERT_NE(emissive, nullptr);
    ASSERT_GE(emissive->values.size(), 3u);
    EXPECT_NEAR(emissive->values[0], 0.1, 0.00001);

    const auto* specular = FindMaterialChannel(*material, "specular");
    ASSERT_NE(specular, nullptr);
    ASSERT_GE(specular->values.size(), 3u);
    EXPECT_NEAR(specular->values[2], 0.5, 0.00001);

    const auto* opacity = FindMaterialChannel(*material, "opacity");
    ASSERT_NE(opacity, nullptr);
    EXPECT_TRUE(opacity->hasScalar);
    EXPECT_NEAR(opacity->scalar, 0.65, 0.00001);

    const auto* shininess = FindMaterialChannel(*material, "shininess");
    ASSERT_NE(shininess, nullptr);
    EXPECT_TRUE(shininess->hasScalar);
    EXPECT_DOUBLE_EQ(shininess->scalar, 32.0);
    EXPECT_EQ(shininess->textureKey, "parser/texture/2")
        << "Shininess/gloss textures must retain their source semantic instead of masquerading as PBR roughness maps.";
    EXPECT_EQ(FindMaterialChannel(*material, "roughness"), nullptr)
        << "Assimp shininess textures are gloss data; treating them as roughness maps reverses their shader meaning.";

    EXPECT_TRUE(Contains(externalDependencies, "../Textures/HeroDiffuse.png"));
    EXPECT_TRUE(Contains(externalDependencies, "../Textures/HeroNormal.png"));
    EXPECT_TRUE(Contains(externalDependencies, "../Textures/HeroShininess.png"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, AssimpDetailedSceneCreatesStableParentNodeForMultiMeshSourceNodes)
{
    const auto root = MakeImportTestRoot();
    WriteTextFile(
        root / "Assets" / "Models" / "Multi.obj",
        R"(
o First
v 0 0 0
v 1 0 0
v 0 1 0
vn 0 0 1
f 1//1 2//1 3//1
o Second
v 0 0 1
v 1 0 1
v 0 1 1
vn 0 0 1
f 4//2 5//2 6//2
)");

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        (root / "Assets" / "Models" / "Multi.obj").string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("94949494-9494-4494-8494-949494949494"));
    scene.sceneKey = "Multi";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        root / "Assets" / "Models" / "Multi.obj",
        NLS::Render::Assets::SceneModelSourceFormat::Obj,
        scene));

    std::vector<std::string> nodeKeys;
    for (const auto& node : scene.nodes)
        nodeKeys.push_back(node.sourceKey);

    for (const auto& node : scene.nodes)
    {
        if (node.parentKey.empty())
            continue;
        EXPECT_TRUE(Contains(nodeKeys, node.parentKey));
    }

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, AssimpFbxParserSurfacesTemplateDiffuseAndTexture)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto root = MakeImportTestRoot();
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "embedded_ascii" /
        "box.FBX";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));

    auto fbx = ReadTextFile(sourceFixture);
    const auto materialBegin = fbx.find("Material: 2669981279872");
    ASSERT_NE(materialBegin, std::string::npos);
    const auto directDiffuseBegin = fbx.find(
        "P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.588235318660736,0.588235318660736,0.588235318660736",
        materialBegin);
    ASSERT_NE(directDiffuseBegin, std::string::npos);
    const auto lineBegin = fbx.rfind('\n', directDiffuseBegin);
    const auto eraseBegin = lineBegin == std::string::npos ? directDiffuseBegin : lineBegin + 1u;
    const auto lineEnd = fbx.find('\n', directDiffuseBegin);
    const auto eraseEnd = lineEnd == std::string::npos ? fbx.size() : lineEnd + 1u;
    fbx.erase(eraseBegin, eraseEnd - eraseBegin);

    const auto sourcePath = root / "Assets" / "Models" / "TemplateDiffuseBox.fbx";
    WriteTextFile(sourcePath, fbx);

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929293"));
    scene.sceneKey = "TemplateDiffuseBox";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.materials.size(), 1u);
    const auto* diffuse = FindMaterialChannel(scene.materials.front(), "diffuse");
    ASSERT_NE(diffuse, nullptr);
    ASSERT_GE(diffuse->values.size(), 3u);
    EXPECT_NEAR(diffuse->values[0], 0.8, 0.00001);
    EXPECT_NEAR(diffuse->values[1], 0.8, 0.00001);
    EXPECT_NEAR(diffuse->values[2], 0.8, 0.00001);
    EXPECT_FALSE(diffuse->textureKey.empty());

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserSurfacesNeutralTexturedDiffuse)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto root = MakeImportTestRoot();
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "embedded_ascii" /
        "box.FBX";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));

    auto fbx = ReadTextFile(sourceFixture);
    const auto materialBegin = fbx.find("Material: 2669981279872");
    ASSERT_NE(materialBegin, std::string::npos);
    const auto directDiffuseBegin = fbx.find(
        "P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.588235318660736,0.588235318660736,0.588235318660736",
        materialBegin);
    ASSERT_NE(directDiffuseBegin, std::string::npos);
    fbx.replace(
        directDiffuseBegin,
        std::string("P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.588235318660736,0.588235318660736,0.588235318660736").size(),
        "P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.5,0.5,0.5");

    const auto sourcePath = root / "Assets" / "Models" / "NeutralDiffuseBox.fbx";
    WriteTextFile(sourcePath, fbx);

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929294"));
    scene.sceneKey = "NeutralDiffuseBox";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.materials.size(), 1u);
    const auto* diffuse = FindMaterialChannel(scene.materials.front(), "diffuse");
    ASSERT_NE(diffuse, nullptr);
    ASSERT_GE(diffuse->values.size(), 3u);
    EXPECT_NEAR(diffuse->values[0], 0.5, 0.00001);
    EXPECT_NEAR(diffuse->values[1], 0.5, 0.00001);
    EXPECT_NEAR(diffuse->values[2], 0.5, 0.00001);
    EXPECT_FALSE(diffuse->textureKey.empty());

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserKeepsColoredTexturedDiffuseTintAuthored)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto root = MakeImportTestRoot();
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "embedded_ascii" /
        "box.FBX";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));

    auto fbx = ReadTextFile(sourceFixture);
    const auto materialBegin = fbx.find("Material: 2669981279872");
    ASSERT_NE(materialBegin, std::string::npos);
    const auto directDiffuseBegin = fbx.find(
        "P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.588235318660736,0.588235318660736,0.588235318660736",
        materialBegin);
    ASSERT_NE(directDiffuseBegin, std::string::npos);
    fbx.replace(
        directDiffuseBegin,
        std::string("P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.588235318660736,0.588235318660736,0.588235318660736").size(),
        "P: \"DiffuseColor\", \"Color\", \"\", \"A\",0.8,0.7,0.6");

    const auto sourcePath = root / "Assets" / "Models" / "ColoredDiffuseBox.fbx";
    WriteTextFile(sourcePath, fbx);

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929295"));
    scene.sceneKey = "ColoredDiffuseBox";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.materials.size(), 1u);
    const auto* diffuse = FindMaterialChannel(scene.materials.front(), "diffuse");
    ASSERT_NE(diffuse, nullptr);
    ASSERT_GE(diffuse->values.size(), 3u);
    EXPECT_NEAR(diffuse->values[0], 0.8, 0.00001);
    EXPECT_NEAR(diffuse->values[1], 0.7, 0.00001);
    EXPECT_NEAR(diffuse->values[2], 0.6, 0.00001);
    EXPECT_FALSE(diffuse->textureKey.empty());

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserSurfaces3dsMaxParametersOpacityMaps)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "maxPbrMaterial_metalRough.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));

    const auto sourceFbx = ReadTextFile(sourceFixture);
    const auto runCase = [&sourceFbx](const char* sceneKey, const char* fbxOpacityProperty)
    {
        const auto root = MakeImportTestRoot();
        auto fbx = sourceFbx;
        const std::string opacityDeclaration =
            "P: \"3dsMax|main|opacity_map\", \"Reference\", \"\", \"A\"";
        const auto opacityDeclarationBegin = fbx.find(opacityDeclaration);
        ASSERT_NE(opacityDeclarationBegin, std::string::npos);
        fbx.replace(
            opacityDeclarationBegin,
            opacityDeclaration.size(),
            "P: \"" + std::string(fbxOpacityProperty) + "\", \"Reference\", \"\", \"A\"");
        ReplaceAllText(
            fbx,
            "3dsMax|main|opacity_map",
            fbxOpacityProperty);

        const auto sourcePath = root / "Assets" / "Models" / (std::string(sceneKey) + ".fbx");
        WriteTextFile(sourcePath, fbx);
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / "albedo.png", TinyPng());
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / "metalness.png", TinyPng());
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / "roughness.png", TinyPng());
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / "occlusion.png", TinyPng());
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / "normal.png", TinyPng());
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / "emission.png", TinyPng());
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / "opacity.png", TinyPng());

        NLS::Render::Resources::Parsers::AssimpParser parser;
        std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
        std::vector<std::string> materials;
        std::vector<std::string> externalDependencies;
        ASSERT_TRUE(parser.LoadModelData(
            sourcePath.string(),
            meshes,
            materials,
            NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
            &externalDependencies));

        NLS::Render::Assets::ImportedScene scene;
        scene.sourceAssetId = NLS::Core::Assets::AssetId(
            NLS::Guid::Parse("92929292-9292-4292-8292-929292929296"));
        scene.sceneKey = sceneKey;
        ASSERT_TRUE(parser.PopulateImportedSceneData(
            sourcePath,
            NLS::Render::Assets::SceneModelSourceFormat::Fbx,
            scene));

        ASSERT_EQ(scene.materials.size(), 1u);
        const auto* opacity = FindMaterialChannel(scene.materials.front(), "opacity");
        ASSERT_NE(opacity, nullptr)
            << fbxOpacityProperty << " should be surfaced as the parser opacity channel.";
        ASSERT_FALSE(opacity->textureKey.empty());
        const auto texture = std::find_if(
            scene.textures.begin(),
            scene.textures.end(),
            [opacity](const NLS::Render::Assets::ImportedSceneNamedRecord& record)
            {
                return record.sourceKey == opacity->textureKey;
            });
        ASSERT_NE(texture, scene.textures.end());
        EXPECT_EQ(texture->uri, "Textures\\opacity.png");
        EXPECT_TRUE(Contains(externalDependencies, "Textures\\opacity.png"))
            << "Raw FBX opacity maps must still participate in import dependency tracking.";

        std::filesystem::remove_all(root);
    };

    runCase("FbxTransparencyMap", "3dsMax|Parameters|transparency_map");
    runCase("FbxCutoutMap", "3dsMax|Parameters|cutout_map");
#endif
}

void AppendU64(std::vector<uint8_t>& bytes, const uint64_t value)
{
    for (uint32_t shift = 0u; shift < 64u; shift += 8u)
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
}

std::vector<uint8_t> FbxBinaryLongProperty(const uint64_t value)
{
    std::vector<uint8_t> property = {static_cast<uint8_t>('L')};
    AppendU64(property, value);
    return property;
}

std::vector<uint8_t> FbxBinaryStringProperty(const std::string& value)
{
    std::vector<uint8_t> property = {static_cast<uint8_t>('S')};
    AppendU32(property, static_cast<uint32_t>(value.size()));
    property.insert(property.end(), value.begin(), value.end());
    return property;
}

struct TestFbxBinaryNode
{
    std::string name;
    std::vector<std::vector<uint8_t>> properties;
    std::vector<TestFbxBinaryNode> children;
};

size_t FbxBinaryNodeSize(const TestFbxBinaryNode& node, const bool wideOffsets)
{
    const size_t headerSize = wideOffsets ? 25u : 13u;
    size_t size = headerSize + node.name.size();
    for (const auto& property : node.properties)
        size += property.size();
    for (const auto& child : node.children)
        size += FbxBinaryNodeSize(child, wideOffsets);
    if (!node.children.empty())
        size += headerSize;
    return size;
}

void AppendFbxBinaryNode(
    std::vector<uint8_t>& bytes,
    const TestFbxBinaryNode& node,
    const size_t absoluteBaseOffset,
    const bool wideOffsets)
{
    size_t propertySize = 0u;
    for (const auto& property : node.properties)
        propertySize += property.size();

    const auto endOffset = absoluteBaseOffset + bytes.size() + FbxBinaryNodeSize(node, wideOffsets);
    ASSERT_LE(node.name.size(), static_cast<size_t>(std::numeric_limits<uint8_t>::max()));
    if (wideOffsets)
    {
        AppendU64(bytes, static_cast<uint64_t>(endOffset));
        AppendU64(bytes, static_cast<uint64_t>(node.properties.size()));
        AppendU64(bytes, static_cast<uint64_t>(propertySize));
    }
    else
    {
        ASSERT_LE(endOffset, static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
        ASSERT_LE(node.properties.size(), static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
        ASSERT_LE(propertySize, static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
        AppendU32(bytes, static_cast<uint32_t>(endOffset));
        AppendU32(bytes, static_cast<uint32_t>(node.properties.size()));
        AppendU32(bytes, static_cast<uint32_t>(propertySize));
    }
    bytes.push_back(static_cast<uint8_t>(node.name.size()));
    bytes.insert(bytes.end(), node.name.begin(), node.name.end());
    for (const auto& property : node.properties)
        bytes.insert(bytes.end(), property.begin(), property.end());
    for (const auto& child : node.children)
        AppendFbxBinaryNode(bytes, child, absoluteBaseOffset, wideOffsets);
    if (!node.children.empty())
        bytes.insert(bytes.end(), wideOffsets ? 25u : 13u, 0u);
}

uint32_t ReadFbxBinaryUint32(const std::vector<uint8_t>& bytes, const size_t offset)
{
    EXPECT_LE(offset + 4u, bytes.size());
    return static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

uint64_t ReadFbxBinaryUint64(const std::vector<uint8_t>& bytes, const size_t offset)
{
    EXPECT_LE(offset + 8u, bytes.size());
    uint64_t value = 0u;
    for (size_t index = 0u; index < 8u; ++index)
        value |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8u);
    return value;
}

std::string ReadFbxBinaryStringProperty(const std::vector<uint8_t>& bytes, const size_t offset)
{
    EXPECT_LT(offset, bytes.size());
    if (offset >= bytes.size() || bytes[offset] != static_cast<uint8_t>('S') || offset + 5u > bytes.size())
        return {};
    const auto length = static_cast<size_t>(ReadFbxBinaryUint32(bytes, offset + 1u));
    EXPECT_LE(offset + 5u + length, bytes.size());
    if (offset + 5u + length > bytes.size())
        return {};
    return std::string(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + 5u),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + 5u + length));
}

void WriteFbxBinaryUint32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value)
{
    ASSERT_LE(offset + 4u, bytes.size());
    for (size_t index = 0u; index < 4u; ++index)
        bytes[offset + index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xFFu);
}

void WriteFbxBinaryUint64(std::vector<uint8_t>& bytes, const size_t offset, const uint64_t value)
{
    ASSERT_LE(offset + 8u, bytes.size());
    for (size_t index = 0u; index < 8u; ++index)
        bytes[offset + index] = static_cast<uint8_t>((value >> (index * 8u)) & 0xFFu);
}

bool HasWideFbxBinaryOffsets(const std::vector<uint8_t>& bytes)
{
    return bytes.size() >= 27u && ReadFbxBinaryUint32(bytes, 23u) >= 7500u;
}

struct TestFbxBinaryNodeLocation
{
    size_t headerOffset = 0u;
    size_t endOffset = 0u;
    size_t propertyOffset = 0u;
    std::string name;
};

bool CollectFbxBinaryNodes(
    const std::vector<uint8_t>& bytes,
    size_t offset,
    const size_t parentEnd,
	const bool wideOffsets,
    std::vector<TestFbxBinaryNodeLocation>& nodes)
{
    const size_t headerSize = wideOffsets ? 25u : 13u;
    while (offset + headerSize <= parentEnd)
    {
        const auto endOffset = static_cast<size_t>(wideOffsets
            ? ReadFbxBinaryUint64(bytes, offset)
            : ReadFbxBinaryUint32(bytes, offset));
        if (endOffset == 0u)
            return true;
        const auto propertyLength = static_cast<size_t>(wideOffsets
            ? ReadFbxBinaryUint64(bytes, offset + 16u)
            : ReadFbxBinaryUint32(bytes, offset + 8u));
        const auto nameLength = static_cast<size_t>(bytes[offset + headerSize - 1u]);
        const auto propertyOffset = offset + headerSize + nameLength;
        if (endOffset <= offset || endOffset > parentEnd || propertyOffset + propertyLength > endOffset)
            return false;

        nodes.push_back({
            offset,
            endOffset,
            propertyOffset,
            std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset + headerSize),
                bytes.begin() + static_cast<std::ptrdiff_t>(propertyOffset))
        });
        const auto childOffset = propertyOffset + propertyLength;
        if (childOffset < endOffset && !CollectFbxBinaryNodes(bytes, childOffset, endOffset, wideOffsets, nodes))
            return false;
        offset = endOffset;
    }
    return offset == parentEnd;
}

bool InsertFbxBinaryNodes(
    std::vector<uint8_t>& bytes,
    const std::string& parentName,
    const std::vector<TestFbxBinaryNode>& insertedNodes)
{
    const bool wideOffsets = HasWideFbxBinaryOffsets(bytes);
    const size_t headerSize = wideOffsets ? 25u : 13u;
    std::vector<TestFbxBinaryNodeLocation> nodes;
    if (!CollectFbxBinaryNodes(bytes, 27u, bytes.size(), wideOffsets, nodes))
        return false;
    const auto parent = std::find_if(nodes.begin(), nodes.end(), [&parentName](const auto& node)
    {
        return node.name == parentName;
    });
    if (parent == nodes.end() || parent->endOffset < headerSize)
        return false;
    const auto insertionOffset = parent->endOffset - headerSize;
    if ((wideOffsets ? ReadFbxBinaryUint64(bytes, insertionOffset) : ReadFbxBinaryUint32(bytes, insertionOffset)) != 0u)
        return false;

    std::vector<uint8_t> encoded;
    for (const auto& node : insertedNodes)
        AppendFbxBinaryNode(encoded, node, insertionOffset, wideOffsets);
    if (encoded.empty() || (!wideOffsets && encoded.size() > std::numeric_limits<uint32_t>::max()))
        return false;
    const auto delta = static_cast<uint32_t>(encoded.size());
    for (const auto& node : nodes)
    {
        if (node.endOffset > insertionOffset)
        {
            const auto adjustedEnd = static_cast<uint64_t>(node.endOffset) + delta;
            if (wideOffsets)
                WriteFbxBinaryUint64(bytes, node.headerOffset, adjustedEnd);
            else
                WriteFbxBinaryUint32(bytes, node.headerOffset, static_cast<uint32_t>(adjustedEnd));
        }
    }
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(insertionOffset),
        encoded.begin(),
        encoded.end());
    return true;
}

std::string TestFbxDisplayName(const std::string& name)
{
    const auto separator = name.find("::");
    const auto begin = separator == std::string::npos ? 0u : separator + 2u;
    const auto end = name.find('\0', begin);
    return name.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::vector<uint8_t> MakeBinaryFbxBump2dFixture(
    const std::filesystem::path& sourceFixture,
    const bool duplicateTargetName = false)
{
    auto bytes = ReadBinaryFile(sourceFixture);
    const bool wideOffsets = HasWideFbxBinaryOffsets(bytes);
    std::vector<TestFbxBinaryNodeLocation> nodes;
    EXPECT_TRUE(CollectFbxBinaryNodes(bytes, 27u, bytes.size(), wideOffsets, nodes));
    std::vector<TestFbxBinaryNodeLocation> materials;
    std::copy_if(nodes.begin(), nodes.end(), std::back_inserter(materials), [](const auto& node)
    {
        return node.name == "Material";
    });
    EXPECT_FALSE(materials.empty());
    const TestFbxBinaryNodeLocation* targetMaterial = nullptr;
    if (duplicateTargetName)
    {
        for (size_t first = 0u; first < materials.size() && !targetMaterial; ++first)
        {
            const auto firstName = ReadFbxBinaryStringProperty(bytes, materials[first].propertyOffset + 9u);
            for (size_t second = first + 1u; second < materials.size(); ++second)
            {
                const auto secondName = ReadFbxBinaryStringProperty(bytes, materials[second].propertyOffset + 9u);
                if (!firstName.empty() && firstName.size() == secondName.size())
                {
                    targetMaterial = &materials[second];
                    std::copy(
                        firstName.begin(),
                        firstName.end(),
                        bytes.begin() + static_cast<std::ptrdiff_t>(targetMaterial->propertyOffset + 14u));
                    break;
                }
            }
        }
    }
    else
    {
        for (const auto& candidate : materials)
        {
            const auto candidateName = TestFbxDisplayName(
                ReadFbxBinaryStringProperty(bytes, candidate.propertyOffset + 9u));
            if (candidateName.empty())
                continue;
            const auto occurrenceCount = static_cast<size_t>(std::count_if(
                materials.begin(),
                materials.end(),
                [&bytes, &candidateName](const TestFbxBinaryNodeLocation& other)
                {
                    return TestFbxDisplayName(
                        ReadFbxBinaryStringProperty(bytes, other.propertyOffset + 9u)) == candidateName;
                }));
            if (occurrenceCount == 1u)
            {
                targetMaterial = &candidate;
                break;
            }
        }
    }
    EXPECT_NE(targetMaterial, nullptr);
    if (!targetMaterial || bytes[targetMaterial->propertyOffset] != static_cast<uint8_t>('L'))
        return {};
    const auto targetMaterialId = ReadFbxBinaryUint64(bytes, targetMaterial->propertyOffset + 1u);

    constexpr uint64_t bumpNodeId = 9188262659392ull;
    constexpr uint64_t normalTextureId = 9188262659393ull;
    const auto stringProperty = [](const char* value) { return FbxBinaryStringProperty(value); };
    const TestFbxBinaryNode bumpNode{"Texture", {
        FbxBinaryLongProperty(bumpNodeId), stringProperty("Texture::NormalBump2d"), stringProperty("")
    }, {
        {"FileName", {stringProperty("")}},
        {"RelativeFilename", {stringProperty("")}}
    }};
    const TestFbxBinaryNode normalTexture{"Texture", {
        FbxBinaryLongProperty(normalTextureId), stringProperty("Texture::Normal"), stringProperty("")
    }, {
        {"FileName", {stringProperty("Textures\\normal.png")}},
        {"RelativeFilename", {stringProperty("Textures\\normal.png")}}
    }};
    if (!InsertFbxBinaryNodes(bytes, "Objects", {bumpNode, normalTexture}))
        return {};

    const auto connection = [&stringProperty](
        const uint64_t source,
        const uint64_t destination,
        const char* property)
    {
        return TestFbxBinaryNode{"C", {
            stringProperty("OP"),
            FbxBinaryLongProperty(source),
            FbxBinaryLongProperty(destination),
            stringProperty(property)
        }};
    };
    if (!InsertFbxBinaryNodes(bytes, "Connections", {
        connection(bumpNodeId, targetMaterialId, "3dsMax|Parameters|bump_map"),
        connection(normalTextureId, bumpNodeId,
            "3dsMax|ai_bump2d Parameters/Connections|bump_map.shader")
    }))
        return {};
    return bytes;
}

std::vector<uint8_t> MakeSyntheticBinaryFbxMaterialGraph(
    const bool wideOffsets,
    const bool negativeObjectIds = false)
{
    std::vector<uint8_t> bytes = {
        'K', 'a', 'y', 'd', 'a', 'r', 'a', ' ', 'F', 'B', 'X', ' ', 'B', 'i', 'n', 'a', 'r', 'y',
        ' ', ' ', 0u, 0x1au, 0u
    };
    AppendU32(bytes, wideOffsets ? 7500u : 7400u);

    const uint64_t materialId = negativeObjectIds ? std::numeric_limits<uint64_t>::max() - 99u : 100u;
    const uint64_t bumpNodeId = negativeObjectIds ? std::numeric_limits<uint64_t>::max() - 199u : 200u;
    const uint64_t normalTextureId = negativeObjectIds ? std::numeric_limits<uint64_t>::max() - 200u : 201u;
    const auto stringProperty = [](const char* value) { return FbxBinaryStringProperty(value); };
    const auto connection = [&stringProperty](
        const uint64_t source,
        const uint64_t destination,
        const char* property)
    {
        return TestFbxBinaryNode{"C", {
            stringProperty("OP"),
            FbxBinaryLongProperty(source),
            FbxBinaryLongProperty(destination),
            stringProperty(property)
        }};
    };

    const TestFbxBinaryNode objects{"Objects", {}, {
        {"Material", {
            FbxBinaryLongProperty(materialId), stringProperty("Material::WideMaterial"), stringProperty("")
        }},
        {"Texture", {
            FbxBinaryLongProperty(bumpNodeId), stringProperty("Texture::NormalBump2d"), stringProperty("")
        }, {
            {"FileName", {stringProperty("")}},
            {"RelativeFilename", {stringProperty("")}}
        }},
        {"Texture", {
            FbxBinaryLongProperty(normalTextureId), stringProperty("Texture::Normal"), stringProperty("")
        }, {
            {"FileName", {stringProperty("Textures\\normal.png")}},
            {"RelativeFilename", {stringProperty("Textures\\normal.png")}}
        }}
    }};
    const TestFbxBinaryNode connections{"Connections", {}, {
        connection(bumpNodeId, materialId, "3dsMax|Parameters|bump_map"),
        connection(normalTextureId, bumpNodeId,
            "3dsMax|ai_bump2d Parameters/Connections|bump_map.shader")
    }};

    AppendFbxBinaryNode(bytes, objects, 0u, wideOffsets);
    AppendFbxBinaryNode(bytes, connections, 0u, wideOffsets);
    bytes.insert(bytes.end(), wideOffsets ? 25u : 13u, 0u);
    return bytes;
}

bool ShortenInjectedFbxFilenamePropertyBoundary(std::vector<uint8_t>& bytes)
{
    const bool wideOffsets = HasWideFbxBinaryOffsets(bytes);
    std::vector<TestFbxBinaryNodeLocation> nodes;
    if (!CollectFbxBinaryNodes(bytes, 27u, bytes.size(), wideOffsets, nodes))
        return false;
    const auto filename = std::find_if(
        nodes.begin(),
        nodes.end(),
        [&bytes](const TestFbxBinaryNodeLocation& node)
        {
            return (node.name == "FileName" || node.name == "RelativeFilename") &&
                ReadFbxBinaryStringProperty(bytes, node.propertyOffset) == "Textures\\normal.png";
        });
    if (filename == nodes.end())
        return false;

    const auto propertyLengthOffset = filename->headerOffset + (wideOffsets ? 16u : 8u);
    if (wideOffsets)
        WriteFbxBinaryUint64(bytes, propertyLengthOffset, 5u);
    else
        WriteFbxBinaryUint32(bytes, propertyLengthOffset, 5u);
    return true;
}

TEST(AssetImportPipelineTests, AssimpFbxParserResolves3dsMaxBump2dNormalTextureChain)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "maxPbrMaterial_metalRough.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));

    auto fbx = ReadTextFile(sourceFixture);
    const auto replaceOnce = [&fbx](const std::string& oldValue, const std::string& newValue)
    {
        const auto position = fbx.find(oldValue);
        if (position == std::string::npos)
            return false;
        fbx.replace(position, oldValue.size(), newValue);
        return true;
    };

    ASSERT_TRUE(replaceOnce("Count: 18", "Count: 19"));
    const auto textureDefinitions = fbx.find("ObjectType: \"Texture\"");
    ASSERT_NE(textureDefinitions, std::string::npos);
    const auto textureCount = fbx.find("Count: 7", textureDefinitions);
    ASSERT_NE(textureCount, std::string::npos);
    fbx.replace(textureCount, std::string("Count: 7").size(), "Count: 8");
    ReplaceAllText(fbx, "3dsMax|main|norm_map", "3dsMax|Parameters|bump_map");

    constexpr uint64_t bumpNodeId = 9188262659392ull;
    const std::string emissionTextureMarker = "\tTexture: 2188262700192, \"Texture::Emission\", \"\" {";
    const auto emissionTexturePosition = fbx.find(emissionTextureMarker);
    ASSERT_NE(emissionTexturePosition, std::string::npos);
    fbx.insert(
        emissionTexturePosition,
        "\tTexture: " + std::to_string(bumpNodeId) + R"(, "Texture::NormalBump2d", "" {
		Type: "TextureVideoClip"
		Version: 202
		TextureName: "Texture::NormalBump2d"
		Properties70:  {
			P: "3dsMax|ai_bump2d Parameters/Connections", "Compound", "", ""
			P: "3dsMax|ai_bump2d Parameters/Connections|bump_map.connected", "Bool", "", "A",1
			P: "3dsMax|ai_bump2d Parameters/Connections|bump_map.shader", "Reference", "", "A"
		}
		Media: ""
		FileName: ""
		RelativeFilename: ""
		ModelUVTranslation: 0,0
		ModelUVScaling: 1,1
		Texture_Alpha_Source: "None"
		Cropping: 0,0,0,0
	}
)"
    );

    ASSERT_TRUE(replaceOnce(
        "C: \"OP\",2188262659392,2188102329504, \"3dsMax|Parameters|bump_map\"",
        "C: \"OP\"," + std::to_string(bumpNodeId) + ",2188102329504, \"3dsMax|Parameters|bump_map\"\n\t\n"
        "\tC: \"OP\"," + std::to_string(bumpNodeId) + ",2188102329504, \"3dsMax|main|norm_map\"\n\t\n"
        "\t;Texture::Normal, Texture::NormalBump2d\n\t"
        "C: \"OP\",9188262659394," + std::to_string(bumpNodeId) +
        ", \"3dsMax|ai_bump2d Parameters/Connections|bump_map.shader\"\n\t\n\t"
        "C: \"OP\",2188262659392," + std::to_string(bumpNodeId) +
        ", \"3dsMax|ai_bump2d Parameters/Connections|bump_map.shader\""));

    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "Bump2dNormal.fbx";
    WriteTextFile(sourcePath, fbx);
    WriteBinaryFile(sourcePath.parent_path() / "Textures" / "normal.png", TinyPng());

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> externalDependencies;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        &externalDependencies));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929297"));
    scene.sceneKey = "Bump2dNormal";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.materials.size(), 1u);
    const auto* bump = FindMaterialChannel(scene.materials.front(), "bump");
    ASSERT_NE(bump, nullptr);
    ASSERT_FALSE(bump->textureKey.empty());
    EXPECT_EQ(FindMaterialChannel(scene.materials.front(), "normal"), nullptr)
        << "A 3ds Max bump_map remains parser bump input until shared texture identity classification promotes it.";

    const auto texture = std::find_if(
        scene.textures.begin(),
        scene.textures.end(),
        [bump](const NLS::Render::Assets::ImportedSceneNamedRecord& record)
        {
            return record.sourceKey == bump->textureKey;
        });
    ASSERT_NE(texture, scene.textures.end());
    EXPECT_EQ(texture->uri, "Textures\\normal.png");
    EXPECT_TRUE(Contains(externalDependencies, "Textures\\normal.png"));

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        scene.materials.front(),
        NLS::Render::Assets::MaterialSourceModel::FbxParserMaterial);
    const auto normalSlot = std::find_if(
        converted.textureSlots.begin(),
        converted.textureSlots.end(),
        [](const NLS::Render::Assets::ConvertedMaterialTextureSlot& slot)
        {
            return slot.slot == "Normal";
        });
    ASSERT_NE(normalSlot, converted.textureSlots.end());
    EXPECT_EQ(normalSlot->textureKey, bump->textureKey);
    EXPECT_EQ(normalSlot->colorSpace, NLS::Render::Assets::MaterialTextureColorSpace::Linear);

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserResolvesBinary3dsMaxBump2dNormalTextureChain)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "spider.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "BinaryBump2dNormal.fbx";
    const auto binaryFbx = MakeBinaryFbxBump2dFixture(sourceFixture);
    ASSERT_FALSE(binaryFbx.empty());
    WriteBinaryFile(sourcePath, binaryFbx);
    WriteBinaryFile(sourcePath.parent_path() / "Textures" / "normal.png", TinyPng());

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> externalDependencies;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        &externalDependencies));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929299"));
    scene.sceneKey = "BinaryBump2dNormal";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_GE(scene.materials.size(), 2u);
    const auto targetMaterial = std::find_if(
        scene.materials.begin(),
        scene.materials.end(),
        [](const NLS::Render::Assets::ImportedSceneNamedRecord& material)
        {
            const auto* channel = FindMaterialChannel(material, "bump");
            return channel && !channel->textureKey.empty();
    });
    ASSERT_NE(targetMaterial, scene.materials.end());
    EXPECT_EQ(
        std::count_if(
            scene.materials.begin(),
            scene.materials.end(),
            [&targetMaterial](const NLS::Render::Assets::ImportedSceneNamedRecord& material)
            {
                return material.name == targetMaterial->name;
            }),
        1)
        << "FBX bump2d recovery is only safe when the imported material display name is unique.";

    const auto* bump = FindMaterialChannel(*targetMaterial, "bump");
    ASSERT_NE(bump, nullptr);
    ASSERT_FALSE(bump->textureKey.empty());
    const auto texture = std::find_if(
        scene.textures.begin(),
        scene.textures.end(),
        [bump](const NLS::Render::Assets::ImportedSceneNamedRecord& record)
        {
            return record.sourceKey == bump->textureKey;
        });
    ASSERT_NE(texture, scene.textures.end());
    EXPECT_EQ(texture->uri, "Textures\\normal.png");
    EXPECT_TRUE(Contains(externalDependencies, "Textures\\normal.png"));

    const auto converted = NLS::Render::Assets::ConvertImportedSceneMaterial(
        scene,
        *targetMaterial,
        NLS::Render::Assets::MaterialSourceModel::FbxParserMaterial);
    EXPECT_TRUE(std::any_of(
        converted.textureSlots.begin(),
        converted.textureSlots.end(),
        [bump](const NLS::Render::Assets::ConvertedMaterialTextureSlot& slot)
        {
            return slot.slot == "Normal" && slot.textureKey == bump->textureKey;
        }));

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserReadsBinary7500WideOffsetBump2dGraph)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "Binary7500Bump2dNormal.fbx";
    const auto binaryFbx = MakeSyntheticBinaryFbxMaterialGraph(true);
    ASSERT_FALSE(binaryFbx.empty());
    ASSERT_TRUE(HasWideFbxBinaryOffsets(binaryFbx));
    WriteBinaryFile(sourcePath, binaryFbx);

    NLS::Render::Resources::Parsers::AssimpParser parser;
    EXPECT_TRUE(parser.CanReadFbxMaterialGraphForTesting(sourcePath));

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserReadsSignedObjectIdsAcrossAsciiAndBinaryGraphs)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto root = MakeImportTestRoot();
    const auto binaryPath = root / "Assets" / "Models" / "SignedIdsBinary7500.fbx";
    const auto binaryFbx = MakeSyntheticBinaryFbxMaterialGraph(true, true);
    ASSERT_FALSE(binaryFbx.empty());
    WriteBinaryFile(binaryPath, binaryFbx);

    const auto asciiPath = root / "Assets" / "Models" / "SignedIdsAscii.fbx";
    WriteTextFile(asciiPath, R"(Objects:  {
	Material: -100, "Material::SignedMaterial", "" {
	}
	Texture: -200, "Texture::SignedNormal", "" {
		RelativeFilename: "Textures\normal.png"
	}
}
Connections:  {
	C: "OP",-200,-100, "3dsMax|Parameters|bump_map"
}
)");

    NLS::Render::Resources::Parsers::AssimpParser parser;
    EXPECT_TRUE(parser.CanReadFbxMaterialGraphForTesting(binaryPath));
    EXPECT_TRUE(parser.CanReadFbxMaterialGraphForTesting(asciiPath));

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserRefusesAmbiguousDuplicateMaterialRecovery)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "spider.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "AmbiguousBinaryBump2dNormal.fbx";
    const auto binaryFbx = MakeBinaryFbxBump2dFixture(sourceFixture, true);
    ASSERT_FALSE(binaryFbx.empty());
    WriteBinaryFile(sourcePath, binaryFbx);

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> externalDependencies;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        &externalDependencies));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929302"));
    scene.sceneKey = "AmbiguousBinaryBump2dNormal";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    bool hasDuplicateMaterialName = false;
    for (size_t first = 0u; first < scene.materials.size() && !hasDuplicateMaterialName; ++first)
    {
        for (size_t second = first + 1u; second < scene.materials.size(); ++second)
        {
            if (scene.materials[first].name == scene.materials[second].name)
            {
                hasDuplicateMaterialName = true;
                break;
            }
        }
    }
    EXPECT_TRUE(hasDuplicateMaterialName);
    EXPECT_FALSE(Contains(externalDependencies, "Textures\\normal.png"));
    EXPECT_TRUE(std::none_of(
        scene.textures.begin(),
        scene.textures.end(),
        [](const NLS::Render::Assets::ImportedSceneNamedRecord& texture)
        {
            return texture.uri == "Textures\\normal.png";
        }));

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserRejectsStringThatCrossesBinaryPropertyBoundary)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    auto binaryFbx = MakeSyntheticBinaryFbxMaterialGraph(false);
    ASSERT_FALSE(binaryFbx.empty());
    ASSERT_TRUE(ShortenInjectedFbxFilenamePropertyBoundary(binaryFbx));

    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "MalformedPropertyBoundary.fbx";
    WriteBinaryFile(sourcePath, binaryFbx);

    NLS::Render::Resources::Parsers::AssimpParser parser;
    EXPECT_FALSE(parser.CanReadFbxMaterialGraphForTesting(sourcePath));

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserPreservesExplicit3dsMaxNormalTexture)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto sourcePath =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "maxPbrMaterial_metalRough.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourcePath));

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> externalDependencies;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        &externalDependencies));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929298"));
    scene.sceneKey = "Explicit3dsMaxNormal";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.materials.size(), 1u);
    const auto* normal = FindMaterialChannel(scene.materials.front(), "normal");
    ASSERT_NE(normal, nullptr);
    ASSERT_FALSE(normal->textureKey.empty());
    EXPECT_EQ(FindMaterialChannel(scene.materials.front(), "bump"), nullptr);

    const auto texture = std::find_if(
        scene.textures.begin(),
        scene.textures.end(),
        [normal](const NLS::Render::Assets::ImportedSceneNamedRecord& record)
        {
            return record.sourceKey == normal->textureKey;
        });
    ASSERT_NE(texture, scene.textures.end());
    EXPECT_EQ(texture->uri, "Textures\\normal.png");
    EXPECT_TRUE(Contains(externalDependencies, "Textures\\normal.png"));
#endif
}

TEST(AssetImportPipelineTests, AssimpFbxParserPreservesStandardNormalMapTexture)
{
#if !NLS_HAS_ASSIMP_FBX_IMPORTER
    GTEST_SKIP() << "Assimp FBX import is not enabled in this build.";
#else
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "maxPbrMaterial_metalRough.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));
    auto fbx = ReadTextFile(sourceFixture);
    ReplaceAllText(fbx, "3dsMax|main|norm_map", "NormalMap");

    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "UnrecognizedNormalCamera.fbx";
    WriteTextFile(sourcePath, fbx);

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> externalDependencies;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        &externalDependencies));

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929300"));
    scene.sceneKey = "UnrecognizedNormalCamera";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        scene));

    ASSERT_EQ(scene.materials.size(), 1u);
    const auto* normal = FindMaterialChannel(scene.materials.front(), "normal");
    ASSERT_NE(normal, nullptr);
    ASSERT_FALSE(normal->textureKey.empty());
    const auto texture = std::find_if(
        scene.textures.begin(),
        scene.textures.end(),
        [normal](const NLS::Render::Assets::ImportedSceneNamedRecord& record)
        {
            return record.sourceKey == normal->textureKey;
        });
    ASSERT_NE(texture, scene.textures.end());
    EXPECT_EQ(texture->uri, "Textures\\normal.png");
    EXPECT_TRUE(Contains(externalDependencies, "Textures\\normal.png"));

    std::filesystem::remove_all(root);
#endif
}

TEST(AssetImportPipelineTests, AssimpParserKeepsSharedMeshPayloadInSourceSpaceAndStoresNodeTransforms)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "SharedMesh.gltf";
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0, 1] }],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA",
                    "byteLength": 66
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 60, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "SharedTriangle",
                    "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1 }]
                }
            ],
            "nodes": [
                { "name": "InstanceA", "mesh": 0, "translation": [10, 0, 0] },
                { "name": "InstanceB", "mesh": 0, "translation": [0, 5, 0] }
            ]
        })");

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE));

    ASSERT_EQ(meshes.size(), 1u);
    EXPECT_EQ(meshes[0].sourceKey, "parser/mesh/0");
    ASSERT_GE(meshes[0].vertices.size(), 2u);
    EXPECT_FLOAT_EQ(meshes[0].vertices[0].position[0], 0.0f);
    EXPECT_FLOAT_EQ(meshes[0].vertices[0].position[1], 0.0f);
    EXPECT_FLOAT_EQ(meshes[0].vertices[1].position[0], 1.0f);

    NLS::Render::Assets::ImportedScene scene;
    scene.sourceAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("96969696-9696-4696-8696-969696969696"));
    scene.sceneKey = "SharedMesh";
    ASSERT_TRUE(parser.PopulateImportedSceneData(
        sourcePath,
        NLS::Render::Assets::SceneModelSourceFormat::Gltf,
        scene));

    const auto instanceA = std::find_if(
        scene.nodes.begin(),
        scene.nodes.end(),
        [](const NLS::Render::Assets::ImportedSceneNode& node)
        {
            return node.name == "InstanceA";
        });
    ASSERT_NE(instanceA, scene.nodes.end());
    EXPECT_EQ(instanceA->meshKey, "parser/mesh/0");
    ASSERT_GE(instanceA->translation.size(), 3u);
    EXPECT_DOUBLE_EQ(instanceA->translation[0], 10.0);

    const auto instanceB = std::find_if(
        scene.nodes.begin(),
        scene.nodes.end(),
        [](const NLS::Render::Assets::ImportedSceneNode& node)
        {
            return node.name == "InstanceB";
        });
    ASSERT_NE(instanceB, scene.nodes.end());
    EXPECT_EQ(instanceB->meshKey, "parser/mesh/0");
    ASSERT_GE(instanceB->translation.size(), 3u);
    EXPECT_DOUBLE_EQ(instanceB->translation[1], 5.0);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, AssimpBakedNodeTransformsDoNotTranslateDirectionStreams)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "BakedDirections.gltf";
    std::vector<uint8_t> meshBytes;
    const std::array<std::array<float, 3>, 3> positions {{
        {{0.0f, 0.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}}
    }};
    const std::array<std::array<float, 3>, 3> normals {{
        {{0.0f, 0.0f, 1.0f}},
        {{0.0f, 0.0f, 1.0f}},
        {{0.0f, 0.0f, 1.0f}}
    }};
    const std::array<std::array<float, 3>, 3> tangents {{
        {{1.0f, 0.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}}
    }};
    const std::array<std::array<float, 2>, 3> uvs {{
        {{0.0f, 0.0f}},
        {{1.0f, 0.0f}},
        {{0.0f, 1.0f}}
    }};

    for (const auto& position : positions)
        for (const auto value : position)
            AppendFloat32(meshBytes, value);
    for (const auto& normal : normals)
        for (const auto value : normal)
            AppendFloat32(meshBytes, value);
    for (const auto& tangent : tangents)
    {
        for (const auto value : tangent)
            AppendFloat32(meshBytes, value);
        AppendFloat32(meshBytes, 1.0f);
    }
    for (const auto& uv : uvs)
        for (const auto value : uv)
            AppendFloat32(meshBytes, value);
    AppendU16(meshBytes, 0u);
    AppendU16(meshBytes, 1u);
    AppendU16(meshBytes, 2u);

    WriteBinaryFile(root / "Assets" / "Models" / "baked-directions.bin", meshBytes);
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "buffers": [
                { "uri": "baked-directions.bin", "byteLength": 150 }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 72, "byteLength": 48, "target": 34962 },
                { "buffer": 0, "byteOffset": 120, "byteLength": 24, "target": 34962 },
                { "buffer": 0, "byteOffset": 144, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4" },
                { "bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2" },
                { "bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "BakedDirectionTriangle",
                    "primitives": [
                        {
                            "attributes": {
                                "POSITION": 0,
                                "NORMAL": 1,
                                "TANGENT": 2,
                                "TEXCOORD_0": 3
                            },
                            "indices": 4
                        }
                    ]
                }
            ],
            "nodes": [
                {
                    "name": "TranslatedNode",
                    "mesh": 0,
                    "translation": [10.0, 20.0, 30.0]
                }
            ]
        })");

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        nullptr,
        true));

    ASSERT_EQ(meshes.size(), 1u);
    ASSERT_FALSE(meshes[0].vertices.empty());
    const auto& vertex = meshes[0].vertices[0];
    EXPECT_FLOAT_EQ(vertex.position[0], 10.0f);
    EXPECT_FLOAT_EQ(vertex.position[1], 20.0f);
    EXPECT_FLOAT_EQ(vertex.position[2], 30.0f);
    EXPECT_NEAR(vertex.normals[0], 0.0f, 1e-5f);
    EXPECT_NEAR(vertex.normals[1], 0.0f, 1e-5f);
    EXPECT_NEAR(vertex.normals[2], 1.0f, 1e-5f);
    EXPECT_NEAR(vertex.tangent[0], 1.0f, 1e-5f);
    EXPECT_NEAR(vertex.tangent[1], 0.0f, 1e-5f);
    EXPECT_NEAR(vertex.tangent[2], 0.0f, 1e-5f);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, AssimpBakedNormalMatrixIsPrecomputedPerMesh)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime" /
        "Rendering" /
        "Resources" /
        "Parsers" /
        "AssimpParser.cpp");

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("BuildDirectionTransforms(meshTransformation)"), std::string::npos)
        << "Assimp baked transform import must precompute normal/direction matrices once per mesh.";
    EXPECT_NE(source.find("copyDirectionStreams"), std::string::npos)
        << "Identity and positive-uniform-scale transforms must bypass per-vertex direction normalization on large meshes.";
    EXPECT_NE(source.find("ShouldCopyDirectionStreams"), std::string::npos)
        << "The baked transform path must classify direction-stream work once per mesh instead of per vertex.";
    EXPECT_NE(source.find("TransformNormalDirection(directionTransforms"), std::string::npos);
    EXPECT_EQ(source.find("TransformNormalDirection(meshTransformation"), std::string::npos)
        << "Normal matrix inverse/transpose must not run once per vertex in ProcessMesh.";
}

TEST(AssetImportPipelineTests, AssimpRawFbxOpacityCompatibilityIsFbxScoped)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "Runtime" /
        "Rendering" /
        "Resources" /
        "Parsers" /
        "AssimpParser.cpp");

    ASSERT_FALSE(source.empty());
    const auto buildMaterials = SliceBetween(source, "void BuildMaterials(", "void BuildMeshRecord");
    ASSERT_FALSE(buildMaterials.empty());
    EXPECT_NE(buildMaterials.find("sourceFormat == SceneModelSourceFormat::Fbx"), std::string::npos)
        << "Raw 3ds Max opacity compatibility is an FBX-only parser shim.";
    EXPECT_NE(buildMaterials.find("AddRawTextureChannel"), std::string::npos);

    const auto processMaterials = SliceBetween(source, "void AssimpParser::ProcessMaterials(", "bool AssimpParser::PopulateImportedSceneData");
    ASSERT_FALSE(processMaterials.empty());
    EXPECT_NE(processMaterials.find("p_sourceFormat == SceneModelSourceFormat::Fbx"), std::string::npos)
        << "Raw FBX opacity texture dependencies must not be collected for non-FBX parser inputs.";
    EXPECT_NE(processMaterials.find("AddRawTextureDependency"), std::string::npos);
}

TEST(AssetImportPipelineTests, AssimpBakedDirectionStreamsStayFiniteForScaledNodes)
{
    const auto root = MakeImportTestRoot();
    const auto writeModel = [&root](
        const std::string& modelName,
        const std::string& nodeTransformJson,
        const std::array<float, 3>& normal)
    {
        const auto sourcePath = root / "Assets" / "Models" / (modelName + ".gltf");
        const auto binName = modelName + ".bin";

        std::vector<uint8_t> meshBytes;
        const std::array<std::array<float, 3>, 3> positions {{
            {{0.0f, 0.0f, 0.0f}},
            {{1.0f, 0.0f, 0.0f}},
            {{0.0f, 1.0f, 0.0f}}
        }};
        const std::array<std::array<float, 3>, 3> tangents {{
            {{1.0f, 0.0f, 0.0f}},
            {{1.0f, 0.0f, 0.0f}},
            {{1.0f, 0.0f, 0.0f}}
        }};
        const std::array<std::array<float, 2>, 3> uvs {{
            {{0.0f, 0.0f}},
            {{1.0f, 0.0f}},
            {{0.0f, 1.0f}}
        }};

        for (const auto& position : positions)
            for (const auto value : position)
                AppendFloat32(meshBytes, value);
        for (size_t vertex = 0u; vertex < positions.size(); ++vertex)
            for (const auto value : normal)
                AppendFloat32(meshBytes, value);
        for (const auto& tangent : tangents)
        {
            for (const auto value : tangent)
                AppendFloat32(meshBytes, value);
            AppendFloat32(meshBytes, 1.0f);
        }
        for (const auto& uv : uvs)
            for (const auto value : uv)
                AppendFloat32(meshBytes, value);
        AppendU16(meshBytes, 0u);
        AppendU16(meshBytes, 1u);
        AppendU16(meshBytes, 2u);

        WriteBinaryFile(root / "Assets" / "Models" / binName, meshBytes);
        WriteTextFile(
            sourcePath,
            R"({
                "asset": { "version": "2.0" },
                "scene": 0,
                "scenes": [{ "nodes": [0] }],
                "buffers": [
                    { "uri": ")" + binName + R"(", "byteLength": 150 }
                ],
                "bufferViews": [
                    { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                    { "buffer": 0, "byteOffset": 36, "byteLength": 36, "target": 34962 },
                    { "buffer": 0, "byteOffset": 72, "byteLength": 48, "target": 34962 },
                    { "buffer": 0, "byteOffset": 120, "byteLength": 24, "target": 34962 },
                    { "buffer": 0, "byteOffset": 144, "byteLength": 6, "target": 34963 }
                ],
                "accessors": [
                    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
                    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4" },
                    { "bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2" },
                    { "bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR" }
                ],
                "meshes": [
                    {
                        "name": "ScaledDirectionTriangle",
                        "primitives": [
                            {
                                "attributes": {
                                    "POSITION": 0,
                                    "NORMAL": 1,
                                    "TANGENT": 2,
                                    "TEXCOORD_0": 3
                                },
                                "indices": 4
                            }
                        ]
                    }
                ],
                "nodes": [
                    {
                        "name": "ScaledNode",
                        "mesh": 0,
)" + nodeTransformJson + R"(
                    }
                ]
            })");
        return sourcePath;
    };
    const auto loadFirstVertex = [](
        const std::filesystem::path& sourcePath)
    {
        NLS::Render::Resources::Parsers::AssimpParser parser;
        std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
        std::vector<std::string> materials;
        EXPECT_TRUE(parser.LoadModelData(
            sourcePath.string(),
            meshes,
            materials,
            NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
            nullptr,
            true));
        EXPECT_EQ(meshes.size(), 1u);
        EXPECT_FALSE(meshes.empty() || meshes[0].vertices.empty());
        return meshes.empty() || meshes[0].vertices.empty()
            ? NLS::Render::Geometry::Vertex{}
            : meshes[0].vertices[0];
    };

    constexpr float inverseSqrt2 = 0.70710678f;
    const auto nonUniform = loadFirstVertex(writeModel(
        "NonUniformDirectionScale",
        R"(                        "scale": [2.0, 1.0, 1.0])",
        {inverseSqrt2, inverseSqrt2, 0.0f}));
    EXPECT_NEAR(nonUniform.normals[0], 0.4472136f, 1e-4f);
    EXPECT_NEAR(nonUniform.normals[1], 0.8944272f, 1e-4f);
    EXPECT_NEAR(nonUniform.normals[2], 0.0f, 1e-4f);

    const auto singular = loadFirstVertex(writeModel(
        "SingularDirectionScale",
        R"(                        "scale": [0.0, 2.0, 3.0])",
        {1.0f, 0.0f, 0.0f}));
    EXPECT_TRUE(std::isfinite(singular.normals[0]));
    EXPECT_TRUE(std::isfinite(singular.normals[1]));
    EXPECT_TRUE(std::isfinite(singular.normals[2]));
    EXPECT_NEAR(singular.normals[0], 1.0f, 1e-4f)
        << "A singular normal matrix should fall back to the authored finite source direction instead of emitting zero or NaN.";
    EXPECT_NEAR(singular.normals[1], 0.0f, 1e-4f);
    EXPECT_NEAR(singular.normals[2], 0.0f, 1e-4f);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportWritesMeshArtifactsBySourceMeshIndexWhenNodesReferenceMeshesOutOfOrder)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "OutOfOrder.gltf";
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0, 1] }],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIAAAAAACBBAAAAAAAAAAAAADBBAAAAAAAAAAAAACBBAACAPwAAAAAAAIA+AACAPgAAQD8AAIA+AACAPgAAQD8AAAEAAgAAAA==",
                    "byteLength": 136
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 24, "target": 34962 },
                { "buffer": 0, "byteOffset": 60, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 68, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 104, "byteLength": 24, "target": 34962 },
                { "buffer": 0, "byteOffset": 128, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
                { "bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC2" },
                { "bufferView": 5, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "meshes": [
                {
                    "name": "LeftMesh",
                    "primitives": [{ "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2 }]
                },
                {
                    "name": "RightMesh",
                    "primitives": [{ "attributes": { "POSITION": 3, "TEXCOORD_0": 4 }, "indices": 5 }]
                }
            ],
            "nodes": [
                { "name": "RightFirst", "mesh": 1, "translation": [100, 0, 0] },
                { "name": "LeftSecond", "mesh": 0, "translation": [0, 5, 0] }
            ]
        })");

    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    std::vector<std::string> materials;
    ASSERT_TRUE(parser.LoadModelData(
        sourcePath.string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE));
    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("95959595-9595-4595-8595-959595959595"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "OutOfOrder",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* leftMesh = result.manifest.FindSubAsset("mesh:mesh/0");
    ASSERT_NE(leftMesh, nullptr);
    const auto* rightMesh = result.manifest.FindSubAsset("mesh:mesh/1");
    ASSERT_NE(rightMesh, nullptr);

    const auto leftArtifact = NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *leftMesh));
    ASSERT_TRUE(leftArtifact.has_value());
    ASSERT_EQ(leftArtifact->vertices.size(), 3u);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[0].position[0], 0.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[0].position[1], 0.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[1].position[1], 0.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[1].texCoords[0], 1.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[2].texCoords[1], 1.0f);

    const auto rightArtifact = NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *rightMesh));
    ASSERT_TRUE(rightArtifact.has_value());
    ASSERT_EQ(rightArtifact->vertices.size(), 3u);
    EXPECT_FLOAT_EQ(rightArtifact->vertices[0].position[0], 10.0f);
    EXPECT_FLOAT_EQ(rightArtifact->vertices[1].position[0], 11.0f);
    EXPECT_FLOAT_EQ(rightArtifact->vertices[0].texCoords[0], 0.25f);
    EXPECT_FLOAT_EQ(rightArtifact->vertices[2].texCoords[1], 0.75f);

    const auto* prefabArtifact = result.manifest.FindSubAsset("prefab:OutOfOrder");
    ASSERT_NE(prefabArtifact, nullptr);
    const auto prefabPayload = ReadArtifactPayloadText(
        root,
        *prefabArtifact,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    const auto prefabGraph = NLS::Engine::Serialize::ObjectGraphReader::Read(prefabPayload);
    ASSERT_TRUE(prefabGraph.has_value());

    const auto* rightTransform = FindRecord(
        *prefabGraph,
        "RightFirst Transform",
        "NLS::Engine::Components::TransformComponent");
    ASSERT_NE(rightTransform, nullptr);
    const auto* rightPosition = FindProperty(*rightTransform, "localPosition");
    ASSERT_NE(rightPosition, nullptr);
    EXPECT_DOUBLE_EQ(GetObjectNumber(rightPosition->value, "x"), 100.0);
    EXPECT_DOUBLE_EQ(GetObjectNumber(rightPosition->value, "y"), 0.0);

    const auto* leftTransform = FindRecord(
        *prefabGraph,
        "LeftSecond Transform",
        "NLS::Engine::Components::TransformComponent");
    ASSERT_NE(leftTransform, nullptr);
    const auto* leftPosition = FindProperty(*leftTransform, "localPosition");
    ASSERT_NE(leftPosition, nullptr);
    EXPECT_DOUBLE_EQ(GetObjectNumber(leftPosition->value, "x"), 0.0);
    EXPECT_DOUBLE_EQ(GetObjectNumber(leftPosition->value, "y"), 5.0);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, StaticMeshImportBuildsFormalLODsFromExplicitLODGroup)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "SmallProp.obj";
    WriteTextFile(
        sourcePath,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("9a9a9a9a-9a9a-49a9-89a9-9a9a9a9a9a9a"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = CurrentModelSceneImporterVersion();
    meta.settings["LOD_GROUP"] = "SmallProp";

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "SmallProp",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* meshArtifact = result.manifest.FindSubAsset("mesh:parser/mesh/0");
    ASSERT_NE(meshArtifact, nullptr);
    const auto bundle = NLS::Render::Assets::DeserializeMeshArtifactBundle(
        ReadArtifactFile(root, *meshArtifact));
    ASSERT_TRUE(bundle.has_value());
    ASSERT_EQ(bundle->lodResources.size(), 4u);
    EXPECT_FLOAT_EQ(bundle->lodResources[0].screenSize, 1.0f);
    EXPECT_FLOAT_EQ(bundle->lodResources[1].screenSize, 0.5f);
    EXPECT_FLOAT_EQ(bundle->lodResources[2].screenSize, 0.25f);
    EXPECT_FLOAT_EQ(bundle->lodResources[3].screenSize, 0.125f);
    const auto* lodSettingsDependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::RuntimeComponentCapability,
        "static-mesh-lod-settings");
    ASSERT_NE(lodSettingsDependency, nullptr);
    EXPECT_NE(lodSettingsDependency->hashOrVersion.find("lodGroup=SmallProp"), std::string::npos);

    const auto firstContentHash = meshArtifact->contentHash;
    WriteTextFile(
        sourcePath,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 1 1 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n"
        "f 1 3 4\n");
    const auto reimportResult = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "ReimportStaging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "SmallProp",
        "editor",
        &result.manifest,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(reimportResult.imported) << JoinDiagnosticSummaries(reimportResult.diagnostics);
    const auto* reimportedMesh = reimportResult.manifest.FindSubAsset("mesh:parser/mesh/0");
    ASSERT_NE(reimportedMesh, nullptr);
    EXPECT_NE(reimportedMesh->contentHash, firstContentHash);
    const auto reimportedBundle = NLS::Render::Assets::DeserializeMeshArtifactBundle(
        ReadArtifactFile(root, *reimportedMesh));
    ASSERT_TRUE(reimportedBundle.has_value());
    EXPECT_EQ(reimportedBundle->lodResources.size(), 4u);
    EXPECT_EQ(meta.settings.at("LOD_GROUP"), "SmallProp");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, StaticMeshImportRecognizesAuthoredLODSuffixesOnlyWhenEnabled)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "AuthoredLODs.gltf";
    std::vector<uint8_t> meshBytes;
    AppendTriangleMeshBytes(meshBytes, 0.0f, 0u, 1u, 2u);
    AppendTriangleMeshBytes(meshBytes, 10.0f, 0u, 1u, 2u);
    WriteBinaryFile(root / "Assets" / "Models" / "mesh.bin", meshBytes);
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "buffers": [{ "uri": "mesh.bin", "byteLength": 84 }],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 42, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 78, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0, 1] }],
            "nodes": [
                { "name": "Rock_LOD0", "mesh": 0 },
                { "name": "Rock_LOD1", "mesh": 1 }
            ],
            "meshes": [
                { "name": "Rock_LOD0", "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1 }] },
                { "name": "Rock_LOD1", "primitives": [{ "attributes": { "POSITION": 2 }, "indices": 3 }] }
            ]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("9b9b9b9b-9b9b-49b9-89b9-9b9b9b9b9b9b"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = CurrentModelSceneImporterVersion();
    meta.settings["IMPORT_MESH_LODS"] = "true";

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "AuthoredLODs",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* lod0Artifact = result.manifest.FindSubAsset("mesh:mesh/0");
    ASSERT_NE(lod0Artifact, nullptr);
    EXPECT_EQ(result.manifest.FindSubAsset("mesh:mesh/1"), nullptr);
    const auto bundle = NLS::Render::Assets::DeserializeMeshArtifactBundle(
        ReadArtifactFile(root, *lod0Artifact));
    ASSERT_TRUE(bundle.has_value());
    ASSERT_EQ(bundle->lodResources.size(), 2u);
    EXPECT_FLOAT_EQ(bundle->lodResources[0].mesh.vertices[0].position[0], 0.0f);
    EXPECT_FLOAT_EQ(bundle->lodResources[1].mesh.vertices[0].position[0], 10.0f);

    auto defaultMeta = meta;
    defaultMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("9c9c9c9c-9c9c-49c9-89c9-9c9c9c9c9c9c"));
    defaultMeta.settings.erase("IMPORT_MESH_LODS");
    const auto defaultResult = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "DefaultStaging",
        root / "Library" / "Artifacts" / defaultMeta.id.ToString(),
        defaultMeta,
        "AuthoredLODsDefault",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(defaultResult.imported) << JoinDiagnosticSummaries(defaultResult.diagnostics);
    const auto* defaultLOD0 = defaultResult.manifest.FindSubAsset("mesh:mesh/0");
    const auto* defaultLOD1 = defaultResult.manifest.FindSubAsset("mesh:mesh/1");
    ASSERT_NE(defaultLOD0, nullptr);
    ASSERT_NE(defaultLOD1, nullptr);
    EXPECT_TRUE(NLS::Render::Assets::DeserializeMeshArtifact(
        ReadArtifactFile(root, *defaultLOD0)).has_value());
    EXPECT_TRUE(NLS::Render::Assets::DeserializeMeshArtifact(
        ReadArtifactFile(root, *defaultLOD1)).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalObjModelImportWritesMaterialTextureUniforms)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "Hero.obj";
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroDiffuse.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroNormal.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
Kd 0.8 0.7 0.6
d 0.65
map_Kd ../Textures/HeroDiffuse.png
map_Bump ../Textures/HeroNormal.png
)");
    WriteTextFile(
        sourcePath,
        R"(
mtllib Hero.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("93939393-9393-4393-8393-939393939393"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    DisableExternalModelTextureResolution(meta);

    std::vector<std::string> progressMessages;
    NLS::Editor::Assets::ImportProgressTracker progressTracker;
    progressTracker.Subscribe(
        [&progressMessages](const NLS::Editor::Assets::ImportProgressEvent& event)
        {
            progressMessages.push_back(event.message);
        });
    const auto progressJob = progressTracker.BeginJob(
        meta.id,
        "Assets/Models/Hero.obj",
        "win64-dx12",
        1u);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "Hero",
        "win64-dx12",
        nullptr,
        &progressTracker,
        progressJob,
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    const auto* materialArtifact = result.manifest.FindSubAsset("material:parser/material/1");
    ASSERT_NE(materialArtifact, nullptr);

    const auto payload = ReadArtifactPayloadText(
        root,
        *materialArtifact,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    const auto* diffuseTextureArtifact = result.manifest.FindSubAsset("texture:parser/texture/0");
    ASSERT_NE(diffuseTextureArtifact, nullptr);
    const auto textureArtifactPath = std::filesystem::path(diffuseTextureArtifact->artifactPath)
        .lexically_normal();
    const auto textureResourcePath = textureArtifactPath.generic_string();
    const auto physicalTexturePath = ResolveTestArtifactPath(root, diffuseTextureArtifact->artifactPath).generic_string();
    EXPECT_NE(payload.find("_BaseMap"), std::string::npos);
    EXPECT_NE(payload.find(textureResourcePath), std::string::npos);
    EXPECT_EQ(payload.find(physicalTexturePath), std::string::npos);
    EXPECT_FALSE(textureArtifactPath.filename().has_extension());
    EXPECT_EQ(payload.find("Assets/Textures/HeroDiffuse.png"), std::string::npos);
    EXPECT_NE(payload.find("_NormalMap"), std::string::npos);
    EXPECT_NE(payload.find("keyword _NORMALMAP"), std::string::npos);
    EXPECT_NE(
        payload.find("property _BaseColor Color 0.800000 0.700000 0.600000 0.650000"),
        std::string::npos);
    const auto reusedFinalDependencies = std::find_if(
        progressMessages.begin(),
        progressMessages.end(),
        [](const std::string& message)
        {
            return message.rfind("Reused final import dependencies |", 0u) == 0u;
        });
    ASSERT_NE(reusedFinalDependencies, progressMessages.end());
    EXPECT_NE(reusedFinalDependencies->find("reused=1"), std::string::npos) << *reusedFinalDependencies;

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalObjModelImportWritesTextureArtifactPayloads)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "Hero.obj";
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroDiffuse.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
map_Kd ../Textures/HeroDiffuse.png
)");
    WriteTextFile(
        sourcePath,
        R"(
mtllib Hero.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("94949494-9494-4494-8494-949494949494"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    DisableExternalModelTextureResolution(meta);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "Hero",
        "win64-dx12",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    const auto* textureArtifact = result.manifest.FindSubAsset("texture:parser/texture/0");
    ASSERT_NE(textureArtifact, nullptr);
    EXPECT_EQ(textureArtifact->artifactType, NLS::Core::Assets::ArtifactType::Texture);
    EXPECT_EQ(textureArtifact->loaderId, "texture");

    const auto payload = ReadArtifactPayloadBytes(
        root,
        *textureArtifact,
        NLS::Core::Assets::ArtifactType::Texture,
        4u);
    ASSERT_GE(payload.size(), 4u);
    EXPECT_EQ(payload[0], static_cast<uint8_t>('N'));
    EXPECT_EQ(payload[1], static_cast<uint8_t>('T'));
    EXPECT_EQ(payload[2], static_cast<uint8_t>('E'));
    EXPECT_EQ(payload[3], static_cast<uint8_t>('X'));
    const std::string payloadText(payload.begin(), payload.end());
    EXPECT_EQ(payloadText.find("PAYLOAD_BEGIN\n"), std::string::npos);
    EXPECT_EQ(payloadText.find("PAYLOAD_BEGIN"), std::string::npos);
    EXPECT_EQ(payloadText.find("NULLUS_IMPORTED_SCENE_ARTIFACT=1"), std::string::npos);

    const auto nativeTexture = NLS::Render::Assets::DeserializeTextureArtifact(
        ReadArtifactFile(root, *textureArtifact));
    ASSERT_TRUE(nativeTexture.has_value());
#if NLS_HAS_DIRECTXTEX
    EXPECT_EQ(nativeTexture->format, NLS::Render::RHI::TextureFormat::BC1);
    EXPECT_EQ(nativeTexture->targetPlatform, "win64-dx12");
    EXPECT_EQ(nativeTexture->encoderId, "directxtex-bc");
    EXPECT_EQ(nativeTexture->encoderVersion, 1u);
#else
    EXPECT_EQ(nativeTexture->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(nativeTexture->targetPlatform, "win64-dx12");
    EXPECT_EQ(nativeTexture->encoderId, "rgba8-passthrough");
    EXPECT_EQ(nativeTexture->encoderVersion, 1u);
#endif
    EXPECT_FALSE(nativeTexture->buildIdentity.empty());
    EXPECT_NE(nativeTexture->buildIdentity.find("|sourceId="), std::string::npos);
    EXPECT_EQ(nativeTexture->buildIdentity.find("|sourceId=|"), std::string::npos);
    EXPECT_NE(nativeTexture->buildIdentity.find("|sourceHash="), std::string::npos);
    EXPECT_EQ(nativeTexture->buildIdentity.find("|sourceHash=|"), std::string::npos);
    EXPECT_NE(nativeTexture->buildIdentity.find("|importer=" + std::to_string(meta.importerVersion)), std::string::npos);
    const auto hasTexturePipelineDependency = std::any_of(
        result.manifest.dependencies.begin(),
        result.manifest.dependencies.end(),
        [](const NLS::Core::Assets::AssetDependencyRecord& dependency)
        {
            return dependency.kind == NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion &&
                dependency.value == "external-texture-build-pipeline" &&
                dependency.hashOrVersion == std::to_string(NLS::Editor::Assets::kExternalTexturePostprocessorVersion);
        });
    EXPECT_TRUE(hasTexturePipelineDependency);
    ASSERT_FALSE(nativeTexture->mips.empty());
    EXPECT_EQ(nativeTexture->mips.front().level, 0u);
    EXPECT_EQ(nativeTexture->mips.front().rowPitch, NLS::Render::RHI::CalculateTextureRowPitch(nativeTexture->format, nativeTexture->width));
    EXPECT_EQ(nativeTexture->mips.front().slicePitch, NLS::Render::RHI::CalculateTextureSlicePitch(nativeTexture->format, nativeTexture->width, nativeTexture->height, 1u));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalEditorModelImportBuildsTexturesForDx12WhileKeepingEditorManifest)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "Hero.obj";
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroDiffuse.png", TinyPng());
    WriteTextFile(
        root / "Assets" / "Models" / "Hero.mtl",
        R"(
newmtl HeroMaterial
map_Kd ../Textures/HeroDiffuse.png
)");
    WriteTextFile(
        sourcePath,
        R"(
mtllib Hero.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("95959595-9595-4595-8595-959595959595"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    DisableExternalModelTextureResolution(meta);
    DisableExternalModelTextureResolution(meta);
    DisableExternalModelTextureResolution(meta);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "Hero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    EXPECT_EQ(result.manifest.targetPlatform, "editor");
    const auto* textureArtifact = result.manifest.FindSubAsset("texture:parser/texture/0");
    ASSERT_NE(textureArtifact, nullptr);
    EXPECT_EQ(textureArtifact->targetPlatform, "editor");

    const auto nativeTexture = NLS::Render::Assets::DeserializeTextureArtifact(
        ReadArtifactFile(root, *textureArtifact));
    ASSERT_TRUE(nativeTexture.has_value());
#if defined(_WIN32)
    EXPECT_EQ(nativeTexture->targetPlatform, "win64-dx12");
#if NLS_HAS_DIRECTXTEX
    EXPECT_EQ(nativeTexture->format, NLS::Render::RHI::TextureFormat::BC1);
    EXPECT_EQ(nativeTexture->encoderId, "directxtex-bc");
#else
    EXPECT_EQ(nativeTexture->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(nativeTexture->encoderId, "rgba8-passthrough");
#endif
#else
    EXPECT_EQ(nativeTexture->targetPlatform, "editor");
    EXPECT_EQ(nativeTexture->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(nativeTexture->encoderId, "rgba8-passthrough");
#endif

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, TextureFormatResolverMapsWindowsDx12CommonCases)
{
    NLS::Render::Assets::TextureImportSettingsSnapshot settings;
    settings.textureType = "default";
    settings.compressionIntent = "default";

    NLS::Render::Assets::TextureSourceDescriptor source;
    source.assetPath = "Assets/Textures/HeroBaseColor.png";
    source.width = 1024u;
    source.height = 1024u;
    source.hasAlpha = false;
    source.isHDR = false;

    NLS::Render::Assets::TextureBackendCapabilities capabilities;
    capabilities.targetPlatform = "win64-dx12";
    const auto sampledUpload = [](const NLS::Render::RHI::TextureFormat format, const bool supportsSrgbView = false)
    {
        NLS::Render::RHI::TextureFormatCapability capability;
        capability.format = format;
        capability.sampled = true;
        capability.upload = true;
        capability.supportsSrgbView = supportsSrgbView;
        return capability;
    };
    capabilities.supportedFormats = {
        {NLS::Render::RHI::TextureFormat::RGBA8, sampledUpload(NLS::Render::RHI::TextureFormat::RGBA8, true)},
        {NLS::Render::RHI::TextureFormat::RGBA16F, sampledUpload(NLS::Render::RHI::TextureFormat::RGBA16F)},
        {NLS::Render::RHI::TextureFormat::BC1, sampledUpload(NLS::Render::RHI::TextureFormat::BC1, true)},
        {NLS::Render::RHI::TextureFormat::BC3, sampledUpload(NLS::Render::RHI::TextureFormat::BC3, true)},
        {NLS::Render::RHI::TextureFormat::BC5, sampledUpload(NLS::Render::RHI::TextureFormat::BC5)},
        {NLS::Render::RHI::TextureFormat::BC7, sampledUpload(NLS::Render::RHI::TextureFormat::BC7, true)}
    };

    const auto color = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(color.has_value());
    EXPECT_EQ(color->resolvedFormat, NLS::Render::RHI::TextureFormat::BC1);
    EXPECT_TRUE(color->mipmapEnabled);

    const auto incompatibleSettings = settings;
    auto incompatibleSource = source;
    incompatibleSource.hasAlpha = true;
    const auto incompatibleEncoder = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        incompatibleSettings,
        std::nullopt,
        incompatibleSource,
        capabilities,
        "rgba8-passthrough",
        1u);
    EXPECT_FALSE(incompatibleEncoder.settings.has_value());
    ASSERT_FALSE(incompatibleEncoder.diagnostics.empty());
    EXPECT_NE(incompatibleEncoder.diagnostics.back().reason.find("does not support"), std::string::npos);

    settings.compressionIntent = "uncompressed";
    const auto uncompressed = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(uncompressed.has_value());
    EXPECT_EQ(uncompressed->resolvedFormat, NLS::Render::RHI::TextureFormat::RGBA8);
    settings.compressionIntent = "default";

    source.hasAlpha = true;
    const auto alphaColor = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(alphaColor.has_value());
    EXPECT_EQ(alphaColor->resolvedFormat, NLS::Render::RHI::TextureFormat::BC3);

    settings.textureType = "normal";
    source.hasAlpha = false;
    const auto normal = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(normal->resolvedFormat, NLS::Render::RHI::TextureFormat::BC5);

    settings.textureType = "mask";
    settings.srgbTexture = false;
    const auto mask = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(mask.has_value());
    EXPECT_EQ(mask->resolvedFormat, NLS::Render::RHI::TextureFormat::BC1);
    EXPECT_EQ(mask->colorSpace, NLS::Render::RHI::TextureColorSpace::Linear);
    settings.srgbTexture = true;

    settings.textureType = "hdr";
    source.isHDR = true;
    const auto hdr = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(hdr.has_value());
    EXPECT_EQ(hdr->resolvedFormat, NLS::Render::RHI::TextureFormat::RGBA16F);

    auto capabilitiesWithoutHdr = capabilities;
    capabilitiesWithoutHdr.supportedFormats.erase(NLS::Render::RHI::TextureFormat::RGBA16F);
    const auto hdrWithoutPreservingFormat = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilitiesWithoutHdr,
        "directxtex-bc",
        1u);
    EXPECT_FALSE(hdrWithoutPreservingFormat.has_value());

    settings.textureType = "default";
    settings.explicitFormat = "bc3";
    source.isHDR = false;
    source.hasAlpha = false;
    const auto explicitBc3 = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(explicitBc3.has_value());
    EXPECT_EQ(explicitBc3->resolvedFormat, NLS::Render::RHI::TextureFormat::BC3);

    settings.explicitFormat = "bc5";
    settings.srgbTexture = true;
    const auto explicitBc5 = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(explicitBc5.settings.has_value());
    EXPECT_EQ(explicitBc5.settings->resolvedFormat, NLS::Render::RHI::TextureFormat::BC5);
    EXPECT_EQ(explicitBc5.settings->colorSpace, NLS::Render::RHI::TextureColorSpace::Linear);
    ASSERT_FALSE(explicitBc5.diagnostics.empty());
    EXPECT_NE(explicitBc5.diagnostics.back().reason.find("linear"), std::string::npos);

    settings.explicitFormat = "bc3";
    auto capabilitiesWithoutBc3 = capabilities;
    capabilitiesWithoutBc3.supportedFormats.erase(NLS::Render::RHI::TextureFormat::BC3);
    const auto unavailableExplicitBc3 = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilitiesWithoutBc3,
        "directxtex-bc",
        1u);
    EXPECT_FALSE(unavailableExplicitBc3.has_value());

    NLS::Render::Assets::TexturePlatformOverrideSettings overrideSettings;
    overrideSettings.platform = "win64-dx12";
    overrideSettings.format = "bc3";
    overrideSettings.maxTextureSize = 512u;
    overrideSettings.resizePolicy = "scale-down";
    overrideSettings.mipmapEnabled = false;
    const auto platformOverride = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        overrideSettings,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(platformOverride.has_value());
    EXPECT_EQ(platformOverride->resolvedFormat, NLS::Render::RHI::TextureFormat::BC3);
    EXPECT_EQ(platformOverride->maxTextureSize, 512u);
    EXPECT_EQ(platformOverride->resizePolicy, "scale-down");
    EXPECT_FALSE(platformOverride->mipmapEnabled);

    settings.explicitFormat = "bc1";
    source.width = 2u;
    source.height = 2u;
    const auto tinyFallback = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(tinyFallback.has_value());
    EXPECT_EQ(tinyFallback->resolvedFormat, NLS::Render::RHI::TextureFormat::RGBA8);

    settings.explicitFormat = "does-not-exist";
    source.width = 1024u;
    source.height = 1024u;
    const auto unsupportedOverride = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(unsupportedOverride.has_value());
    EXPECT_EQ(unsupportedOverride->resolvedFormat, NLS::Render::RHI::TextureFormat::BC1);

    capabilities.supportedFormats.erase(NLS::Render::RHI::TextureFormat::BC1);
    settings.explicitFormat.clear();
    const auto capabilityFallback = NLS::Render::Assets::ResolveTextureBuildSettings(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(capabilityFallback.has_value());
    EXPECT_EQ(capabilityFallback->resolvedFormat, NLS::Render::RHI::TextureFormat::RGBA8);
}

TEST(AssetImportPipelineTests, TextureFormatResolverReportsFallbackAndFailureDiagnostics)
{
    NLS::Render::Assets::TextureImportSettingsSnapshot settings;
    settings.textureType = "default";
    settings.compressionIntent = "default";
    settings.mipmapEnabled = true;

    NLS::Render::Assets::TextureSourceDescriptor source;
    source.assetPath = "Assets/Textures/HeroBaseColor.png";
    source.width = 1024u;
    source.height = 1024u;
    source.hasAlpha = false;

    const auto sampledUpload = [](const NLS::Render::RHI::TextureFormat format, const bool supportsSrgbView = false)
    {
        NLS::Render::RHI::TextureFormatCapability capability;
        capability.format = format;
        capability.sampled = true;
        capability.upload = true;
        capability.supportsSrgbView = supportsSrgbView;
        return capability;
    };

    NLS::Render::Assets::TextureBackendCapabilities capabilities;
    capabilities.targetPlatform = "win64-dx12";
    capabilities.supportedFormats = {
        {NLS::Render::RHI::TextureFormat::RGBA8, sampledUpload(NLS::Render::RHI::TextureFormat::RGBA8, true)}
    };

    const auto fallback = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(fallback.settings.has_value());
    EXPECT_EQ(fallback.settings->resolvedFormat, NLS::Render::RHI::TextureFormat::RGBA8);
    ASSERT_EQ(fallback.diagnostics.size(), 1u);
    EXPECT_EQ(fallback.diagnostics[0].assetPath, source.assetPath);
    EXPECT_EQ(fallback.diagnostics[0].targetPlatform, capabilities.targetPlatform);
    EXPECT_EQ(fallback.diagnostics[0].requestedFormat, NLS::Render::RHI::TextureFormat::BC1);
    EXPECT_EQ(fallback.diagnostics[0].resolvedFormat, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_NE(fallback.diagnostics[0].reason.find("unavailable"), std::string::npos);

    settings.explicitFormat = "bc3";
    const auto explicitFailure = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    EXPECT_FALSE(explicitFailure.settings.has_value());
    ASSERT_EQ(explicitFailure.diagnostics.size(), 1u);
    EXPECT_EQ(explicitFailure.diagnostics[0].requestedFormat, NLS::Render::RHI::TextureFormat::BC3);
    EXPECT_EQ(explicitFailure.diagnostics[0].resolvedFormat, NLS::Render::RHI::TextureFormat::Count);
    EXPECT_NE(explicitFailure.diagnostics[0].reason.find("explicit"), std::string::npos);

    settings.explicitFormat.clear();
    settings.textureType = "hdr";
    source.isHDR = true;
    const auto hdrFailure = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        settings,
        std::nullopt,
        source,
        capabilities,
        "rgba8-passthrough",
        1u);
    EXPECT_FALSE(hdrFailure.settings.has_value());
    ASSERT_EQ(hdrFailure.diagnostics.size(), 1u);
    EXPECT_EQ(hdrFailure.diagnostics[0].requestedFormat, NLS::Render::RHI::TextureFormat::RGBA16F);
    EXPECT_EQ(hdrFailure.diagnostics[0].resolvedFormat, NLS::Render::RHI::TextureFormat::Count);
    EXPECT_NE(hdrFailure.diagnostics[0].reason.find("HDR"), std::string::npos);

    settings.textureType = "default";
    settings.explicitFormat = "does-not-exist";
    source.isHDR = false;
    capabilities.supportedFormats.emplace(
        NLS::Render::RHI::TextureFormat::BC1,
        sampledUpload(NLS::Render::RHI::TextureFormat::BC1, true));
    const auto unknownExplicit = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        settings,
        std::nullopt,
        source,
        capabilities,
        "directxtex-bc",
        1u);
    ASSERT_TRUE(unknownExplicit.settings.has_value());
    EXPECT_EQ(unknownExplicit.settings->resolvedFormat, NLS::Render::RHI::TextureFormat::BC1);
    ASSERT_EQ(unknownExplicit.diagnostics.size(), 1u);
    EXPECT_EQ(unknownExplicit.diagnostics[0].requestedFormat, NLS::Render::RHI::TextureFormat::Count);
    EXPECT_EQ(unknownExplicit.diagnostics[0].resolvedFormat, NLS::Render::RHI::TextureFormat::BC1);
    EXPECT_NE(unknownExplicit.diagnostics[0].reason.find("unknown explicit"), std::string::npos);
    EXPECT_NE(unknownExplicit.diagnostics[0].reason.find("does-not-exist"), std::string::npos);
}

TEST(AssetImportPipelineTests, TextureFormatResolverKeepsReservedPlatformsAndFormatsExplicitlyUnsupported)
{
    NLS::Render::Assets::TextureImportSettingsSnapshot settings;
    settings.textureType = "default";
    settings.compressionIntent = "default";
    settings.mipmapEnabled = true;

    NLS::Render::Assets::TextureSourceDescriptor source;
    source.assetPath = "Assets/Textures/ReservedPlatform.png";
    source.width = 1024u;
    source.height = 1024u;
    source.hasAlpha = false;

    const auto sampledUpload = [](const NLS::Render::RHI::TextureFormat format, const bool supportsSrgbView = false)
    {
        NLS::Render::RHI::TextureFormatCapability capability;
        capability.format = format;
        capability.sampled = true;
        capability.upload = true;
        capability.supportsSrgbView = supportsSrgbView;
        return capability;
    };

    NLS::Render::Assets::TextureBackendCapabilities vulkanCapabilities;
    vulkanCapabilities.targetPlatform = "linux-vulkan";
    vulkanCapabilities.supportedFormats = {
        {NLS::Render::RHI::TextureFormat::RGBA8, sampledUpload(NLS::Render::RHI::TextureFormat::RGBA8, true)},
        {NLS::Render::RHI::TextureFormat::RGBA16F, sampledUpload(NLS::Render::RHI::TextureFormat::RGBA16F)}
    };

    const auto vulkanFallback = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        settings,
        std::nullopt,
        source,
        vulkanCapabilities,
        "rgba8-passthrough",
        1u);
    ASSERT_TRUE(vulkanFallback.settings.has_value());
    EXPECT_EQ(vulkanFallback.settings->targetPlatform, "linux-vulkan");
    EXPECT_EQ(vulkanFallback.settings->resolvedFormat, NLS::Render::RHI::TextureFormat::RGBA8);
    ASSERT_EQ(vulkanFallback.diagnostics.size(), 1u);
    EXPECT_EQ(vulkanFallback.diagnostics[0].targetPlatform, "linux-vulkan");
    EXPECT_EQ(vulkanFallback.diagnostics[0].requestedFormat, NLS::Render::RHI::TextureFormat::BC1);

    for (const auto& reservedFormat : {
        std::pair{"astc4x4", NLS::Render::RHI::TextureFormat::ASTC4x4},
        std::pair{"etc2-rgba8", NLS::Render::RHI::TextureFormat::ETC2RGBA8},
        std::pair{"bc6h", NLS::Render::RHI::TextureFormat::BC6H}
    })
    {
        SCOPED_TRACE(reservedFormat.first);
        settings.explicitFormat = reservedFormat.first;
        const auto explicitReserved = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
            settings,
            std::nullopt,
            source,
            vulkanCapabilities,
            "rgba8-passthrough",
            1u);
        EXPECT_FALSE(explicitReserved.settings.has_value());
        ASSERT_EQ(explicitReserved.diagnostics.size(), 1u);
        EXPECT_EQ(explicitReserved.diagnostics[0].requestedFormat, reservedFormat.second);
        EXPECT_NE(explicitReserved.diagnostics[0].reason.find("explicit"), std::string::npos);
    }

    settings.explicitFormat.clear();
    const auto missingEncoder = NLS::Render::Assets::ResolveTextureBuildSettingsWithDiagnostics(
        settings,
        std::nullopt,
        source,
        vulkanCapabilities,
        "",
        0u);
    EXPECT_FALSE(missingEncoder.settings.has_value());
    ASSERT_EQ(missingEncoder.diagnostics.size(), 1u);
    EXPECT_NE(missingEncoder.diagnostics[0].reason.find("encoder"), std::string::npos);
}

TEST(AssetImportPipelineTests, TextureEncoderRegistryFindsEncodersByIdAndFormat)
{
    class FakeTextureEncoder final : public NLS::Render::Assets::ITextureEncoder
    {
    public:
        std::string_view GetId() const override { return "fake-bc"; }
        uint32_t GetVersion() const override { return 3u; }
        bool SupportsFormat(const NLS::Render::RHI::TextureFormat format) const override
        {
            return format == NLS::Render::RHI::TextureFormat::BC7;
        }
        NLS::Render::Assets::TextureEncodeResult Encode(
            const NLS::Render::Assets::TextureEncodeRequest&) const override
        {
            return {};
        }
    };

    NLS::Render::Assets::TextureEncoderRegistry registry;
    EXPECT_TRUE(registry.IsEmpty());
    EXPECT_TRUE(registry.Register(std::make_shared<FakeTextureEncoder>()));
    EXPECT_FALSE(registry.IsEmpty());
    EXPECT_FALSE(registry.Register(std::make_shared<FakeTextureEncoder>()));

    const auto* byId = registry.Find("fake-bc");
    ASSERT_NE(byId, nullptr);
    EXPECT_EQ(byId->GetVersion(), 3u);
    EXPECT_EQ(registry.Find("missing"), nullptr);
    EXPECT_EQ(registry.FindForFormat(NLS::Render::RHI::TextureFormat::BC7), byId);
    EXPECT_EQ(registry.FindForFormat(NLS::Render::RHI::TextureFormat::ASTC4x4), nullptr);
}

TEST(AssetImportPipelineTests, DirectXTexTextureEncoderProducesBC7ArtifactWhenDependencyAvailable)
{
    auto encoder = NLS::Editor::Assets::CreateDirectXTexTextureEncoder();
    ASSERT_NE(encoder, nullptr);
    EXPECT_EQ(encoder->GetId(), "directxtex-bc");
    EXPECT_TRUE(encoder->SupportsFormat(NLS::Render::RHI::TextureFormat::BC7));

    NLS::Render::Assets::TextureMipGeneratorSettings mipSettings;
    mipSettings.intent = NLS::Render::Assets::TextureMipIntent::Color;
    mipSettings.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    mipSettings.format = NLS::Render::RHI::TextureFormat::RGBA8;
    mipSettings.mipmapEnabled = true;

    std::vector<uint8_t> basePixels;
    basePixels.reserve(8u * 8u * 4u);
    for (uint32_t y = 0u; y < 8u; ++y)
    {
        for (uint32_t x = 0u; x < 8u; ++x)
        {
            basePixels.push_back(static_cast<uint8_t>(x * 31u));
            basePixels.push_back(static_cast<uint8_t>(y * 31u));
            basePixels.push_back(static_cast<uint8_t>((x + y) * 15u));
            basePixels.push_back(255u);
        }
    }

    auto sourceMips = NLS::Render::Assets::GenerateTextureMipChain(
        8u,
        8u,
        std::move(basePixels),
        mipSettings);
    ASSERT_TRUE(sourceMips.has_value());
    ASSERT_EQ(sourceMips->mips.size(), 4u);

    NLS::Render::Assets::TextureBuildSettings buildSettings;
    buildSettings.targetPlatform = "win64-dx12";
    buildSettings.resolvedFormat = NLS::Render::RHI::TextureFormat::BC7;
    buildSettings.mipmapEnabled = true;
    buildSettings.colorSpace = NLS::Render::RHI::TextureColorSpace::SRGB;
    buildSettings.encoderId = "directxtex-bc";
    buildSettings.encoderVersion = encoder->GetVersion();
    buildSettings.toolVersion = "directxtex:jul2025";

    const auto result = encoder->Encode({ &buildSettings, &*sourceMips });

#if NLS_HAS_DIRECTXTEX
    ASSERT_TRUE(result.succeeded) << (result.diagnostics.empty() ? "" : result.diagnostics.front().message);
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(result.artifact.width, 8u);
    EXPECT_EQ(result.artifact.height, 8u);
    EXPECT_EQ(result.artifact.format, NLS::Render::RHI::TextureFormat::BC7);
    EXPECT_EQ(result.artifact.colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    EXPECT_EQ(result.artifact.encoderId, "directxtex-bc");
    EXPECT_EQ(result.artifact.encoderVersion, encoder->GetVersion());
    ASSERT_EQ(result.artifact.mips.size(), sourceMips->mips.size());
    for (const auto& mip : result.artifact.mips)
    {
        EXPECT_EQ(mip.rowPitch, NLS::Render::RHI::CalculateTextureRowPitch(result.artifact.format, mip.width));
        EXPECT_EQ(mip.slicePitch, NLS::Render::RHI::CalculateTextureSlicePitch(result.artifact.format, mip.width, mip.height, 1u));
        EXPECT_EQ(mip.pixels.size(), mip.slicePitch);
    }

    const auto serialized = NLS::Render::Assets::SerializeTextureArtifact(result.artifact);
    ASSERT_FALSE(serialized.empty());
    const auto decoded = NLS::Render::Assets::DeserializeTextureArtifact(serialized);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->format, NLS::Render::RHI::TextureFormat::BC7);
#else
    EXPECT_FALSE(result.succeeded);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_NE(result.diagnostics.front().message.find("unavailable"), std::string::npos);
#endif
}

TEST(AssetImportPipelineTests, DirectXTexTextureEncoderProducesFirstScopeBCArtifactsWhenDependencyAvailable)
{
    auto encoder = NLS::Editor::Assets::CreateDirectXTexTextureEncoder();
    ASSERT_NE(encoder, nullptr);

    NLS::Render::Assets::TextureMipGeneratorSettings mipSettings;
    mipSettings.intent = NLS::Render::Assets::TextureMipIntent::Color;
    mipSettings.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    mipSettings.format = NLS::Render::RHI::TextureFormat::RGBA8;
    mipSettings.mipmapEnabled = true;

    std::vector<uint8_t> basePixels;
    basePixels.reserve(8u * 8u * 4u);
    for (uint32_t y = 0u; y < 8u; ++y)
    {
        for (uint32_t x = 0u; x < 8u; ++x)
        {
            basePixels.push_back(static_cast<uint8_t>(x * 29u));
            basePixels.push_back(static_cast<uint8_t>(y * 23u));
            basePixels.push_back(static_cast<uint8_t>((x * y) * 5u));
            basePixels.push_back(x < 4u ? 255u : 128u);
        }
    }

    auto sourceMips = NLS::Render::Assets::GenerateTextureMipChain(
        8u,
        8u,
        std::move(basePixels),
        mipSettings);
    ASSERT_TRUE(sourceMips.has_value());

    for (const auto format : {
        NLS::Render::RHI::TextureFormat::BC1,
        NLS::Render::RHI::TextureFormat::BC3,
        NLS::Render::RHI::TextureFormat::BC5,
        NLS::Render::RHI::TextureFormat::BC7
    })
    {
        SCOPED_TRACE(NLS::Render::RHI::GetTextureFormatName(format));
        NLS::Render::Assets::TextureBuildSettings buildSettings;
        buildSettings.targetPlatform = "win64-dx12";
        buildSettings.textureIntent = format == NLS::Render::RHI::TextureFormat::BC5 ? "normal" : "default";
        buildSettings.resolvedFormat = format;
        buildSettings.mipmapEnabled = true;
        buildSettings.colorSpace = format == NLS::Render::RHI::TextureFormat::BC5
            ? NLS::Render::RHI::TextureColorSpace::Linear
            : NLS::Render::RHI::TextureColorSpace::SRGB;
        buildSettings.encoderId = "directxtex-bc";
        buildSettings.encoderVersion = encoder->GetVersion();
        buildSettings.toolVersion = "directxtex:jul2025";

        const auto result = encoder->Encode({ &buildSettings, &*sourceMips });
#if NLS_HAS_DIRECTXTEX
        ASSERT_TRUE(result.succeeded) << (result.diagnostics.empty() ? "" : result.diagnostics.front().message);
        ASSERT_EQ(result.artifact.mips.size(), sourceMips->mips.size());
        EXPECT_EQ(result.artifact.format, format);
        for (const auto& mip : result.artifact.mips)
            EXPECT_EQ(mip.pixels.size(), NLS::Render::RHI::CalculateTextureSlicePitch(format, mip.width, mip.height, 1u));
#else
        EXPECT_FALSE(result.succeeded);
        ASSERT_FALSE(result.diagnostics.empty());
        EXPECT_NE(result.diagnostics.front().message.find("unavailable"), std::string::npos);
#endif
    }
}

TEST(AssetImportPipelineTests, TextureBuildIdentityChangesWhenKeyInputsChange)
{
    constexpr uint32_t baselineImporterVersion = 41u;
    constexpr uint32_t changedImporterVersion = baselineImporterVersion + 1u;

    NLS::Render::Assets::TextureBuildSettings base;
    base.sourceAssetPath = "Assets/Textures/HeroBaseColor.png";
    base.sourceAssetIdentity = "guid:hero-basecolor";
    base.sourceContentHash = "sha256:source";
    base.normalizedSettingsHash = "settings:base";
    base.platformOverrideHash = "override:sorted";
    base.importerVersion = baselineImporterVersion;
    base.postprocessorVersion = 3u;
    base.dependencyHash = "deps:v1";
    base.targetPlatform = "win64-dx12";
    base.resolvedFormat = NLS::Render::RHI::TextureFormat::BC7;
    base.mipmapEnabled = true;
    base.colorSpace = NLS::Render::RHI::TextureColorSpace::SRGB;
    base.encoderId = "directxtex-bc";
    base.encoderVersion = 2u;
    base.encoderOptionsHash = "flags:srgb";
    base.toolVersion = "directxtex:jan-2026";
    base.artifactSchemaVersion = 4u;

    const auto baseline = NLS::Render::Assets::BuildTextureBuildIdentity(base);
    EXPECT_FALSE(baseline.empty());

    auto changed = base;
    changed.sourceAssetIdentity = "guid:hero-basecolor-variant";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.sourceContentHash = "sha256:changed-source";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.normalizedSettingsHash = "settings:changed";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.platformOverrideHash = "override:reordered";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.importerVersion = changedImporterVersion;
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.postprocessorVersion = 4u;
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.dependencyHash = "deps:v2";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.targetPlatform = "linux-vulkan";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.resolvedFormat = NLS::Render::RHI::TextureFormat::RGBA8;
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.mipmapEnabled = false;
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.colorSpace = NLS::Render::RHI::TextureColorSpace::Linear;
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.encoderId = "rgba8-passthrough";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.encoderVersion = 3u;
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.encoderOptionsHash = "flags:uniform";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.toolVersion = "directxtex:feb-2026";
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));

    changed = base;
    changed.artifactSchemaVersion = 5u;
    EXPECT_NE(baseline, NLS::Render::Assets::BuildTextureBuildIdentity(changed));
}

TEST(AssetImportPipelineTests, ExternalModelImportUsesMetaModelImporterSettings)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "SettingsHero.gltf";
    std::vector<uint8_t> meshBytes;
    AppendTriangleMeshBytes(meshBytes, 0.0f, 0u, 1u, 2u);
    PadToFour(meshBytes, 0u);
    const auto normalsOffset = meshBytes.size();
    for (size_t index = 0u; index < 3u; ++index)
    {
        AppendFloat32(meshBytes, 0.0f);
        AppendFloat32(meshBytes, 0.0f);
        AppendFloat32(meshBytes, 1.0f);
    }
    const auto tangentsOffset = meshBytes.size();
    for (size_t index = 0u; index < 3u; ++index)
    {
        AppendFloat32(meshBytes, 1.0f);
        AppendFloat32(meshBytes, 0.0f);
        AppendFloat32(meshBytes, 0.0f);
        AppendFloat32(meshBytes, 1.0f);
    }
    const auto uvsOffset = meshBytes.size();
    for (size_t index = 0u; index < 3u; ++index)
    {
        AppendFloat32(meshBytes, index == 1u ? 1.0f : 0.0f);
        AppendFloat32(meshBytes, index == 2u ? 1.0f : 0.0f);
    }
    WriteBinaryFile(root / "Assets" / "Models" / "SettingsHero.bin", meshBytes);
    std::ostringstream gltf;
    gltf << R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "SettingsHero.bin", "byteLength": )" << meshBytes.size() << R"( }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
                { "buffer": 0, "byteOffset": )" << normalsOffset << R"(, "byteLength": 36 },
                { "buffer": 0, "byteOffset": )" << tangentsOffset << R"(, "byteLength": 48 },
                { "buffer": 0, "byteOffset": )" << uvsOffset << R"(, "byteLength": 24 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4" },
                { "bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC2" }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "materials": [{ "name": "Body" }],
            "meshes": [
                {
                    "name": "BodyMesh",
                    "primitives": [
                        {
                            "attributes": {
                                "POSITION": 0,
                                "NORMAL": 1,
                                "TANGENT": 2,
                                "TEXCOORD_0": 3
                            },
                            "material": 0
                        }
                    ]
                }
            ],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })";
    WriteTextFile(sourcePath, gltf.str());

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("96969696-9696-4696-8696-969696969696"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    meta.settings["MODEL_IMPORT_MATERIALS"] = "false";
    meta.settings["MODEL_IMPORT_NORMALS"] = "false";
    meta.settings["MODEL_IMPORT_TANGENTS"] = "false";
    meta.settings["MODEL_IMPORT_UVS"] = "false";

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "SettingsHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    EXPECT_EQ(result.manifest.FindSubAsset("material:material/0"), nullptr);

    const auto* meshArtifact = result.manifest.FindSubAsset("mesh:mesh/0");
    ASSERT_NE(meshArtifact, nullptr);
    const auto mesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *meshArtifact));
    ASSERT_TRUE(mesh.has_value());
    ASSERT_FALSE(mesh->vertices.empty());
    for (const auto& vertex : mesh->vertices)
    {
        EXPECT_FLOAT_EQ(vertex.normals[0], 0.0f);
        EXPECT_FLOAT_EQ(vertex.normals[1], 0.0f);
        EXPECT_FLOAT_EQ(vertex.normals[2], 0.0f);
        EXPECT_FLOAT_EQ(vertex.tangent[0], 0.0f);
        EXPECT_FLOAT_EQ(vertex.tangent[1], 0.0f);
        EXPECT_FLOAT_EQ(vertex.tangent[2], 0.0f);
        EXPECT_FLOAT_EQ(vertex.bitangent[0], 0.0f);
        EXPECT_FLOAT_EQ(vertex.bitangent[1], 0.0f);
        EXPECT_FLOAT_EQ(vertex.bitangent[2], 0.0f);
        EXPECT_FLOAT_EQ(vertex.texCoords[0], 0.0f);
        EXPECT_FLOAT_EQ(vertex.texCoords[1], 0.0f);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportPreservesPreviousArtifactsWhenCancellationRequested)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "CancelledHero.gltf";
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "Body", "primitives": [{ "attributes": {} }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("97979797-9797-4797-8797-979797979797"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto committedRoot = root / "Library" / "Artifacts" / meta.id.ToString();
    WriteTextFile(committedRoot / "old" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d", "previous");

    NLS::Core::Assets::ArtifactManifest previousManifest;
    previousManifest.sourceAssetId = meta.id;
    previousManifest.importerId = meta.importerId;
    previousManifest.importerVersion = meta.importerVersion;
    previousManifest.targetPlatform = "editor";
    previousManifest.primarySubAssetKey = "prefab:Previous";
    previousManifest.subAssets.push_back({
        meta.id,
        "prefab:Previous",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        "editor",
        (committedRoot / "old" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d").string(),
        "previous"
    });

    NLS::Editor::Assets::ImportProgressTracker tracker;
    const auto job = tracker.BeginJob(meta.id, "Assets/Models/CancelledHero.gltf", "editor", 1u);
    auto token = tracker.GetCancellationToken(job);
    ASSERT_TRUE(token.has_value());
    token->get().Cancel();

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        committedRoot,
        meta,
        "CancelledHero",
        "editor",
        &previousManifest,
        &tracker,
        job,
        std::filesystem::path("Models"),
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_TRUE(ContainsDiagnosticCode(result.diagnostics, "artifact-write-cancelled"));
    EXPECT_EQ(result.manifest.primarySubAssetKey, "prefab:Previous");
    EXPECT_EQ(ReadTextFile(committedRoot / "old" / "5d4b4d6c2b6c4a6c9b91d90753df2a8d"), "previous");
    EXPECT_FALSE(std::filesystem::exists(root / "Staging"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalGltfModelTextureArtifactsPreserveEncodedRowOrder)
{
    const auto root = MakeImportTestRoot();
    WriteBinaryFile(root / "Assets" / "Textures" / "TwoRows.png", TwoRowColorPng());
    const std::string gltfDocument =
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/TwoRows.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })";
    struct ImportCase
    {
        const char* fileName;
        const char* assetId;
        bool writeGlb;
    };
    const ImportCase importCases[] = {
        {"TextureOrientation.gltf", "93939393-9393-4393-8393-939393939393", false},
        {"TextureOrientationUpper.GLB", "94949494-9494-4494-8494-949494949494", true}
    };

    for (const auto& importCase : importCases)
    {
        SCOPED_TRACE(importCase.fileName);
        const auto sourcePath = root / "Assets" / "Models" / importCase.fileName;
        if (importCase.writeGlb)
            WriteBinaryFile(sourcePath, MakeGlb(gltfDocument, {}));
        else
            WriteTextFile(sourcePath, gltfDocument);

        NLS::Core::Assets::AssetMeta meta;
        meta.id = NLS::Core::Assets::AssetId(NLS::Guid::Parse(importCase.assetId));
        meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
        meta.importerId = "scene-model";
        meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    DisableExternalModelTextureResolution(meta);

        const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
            sourcePath,
            root / "Staging",
            root / "Library" / "Artifacts" / meta.id.ToString(),
            meta,
            "TextureOrientation",
            "unit-test",
            nullptr,
            nullptr,
            {},
            std::filesystem::path("Models"),
            root,
            {}
        });

        ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
        const auto* textureArtifact = result.manifest.FindSubAsset("texture:image/0");
        ASSERT_NE(textureArtifact, nullptr);
        const auto texture = NLS::Render::Assets::DeserializeTextureArtifact(
            ReadArtifactFile(root, *textureArtifact));
        ASSERT_TRUE(texture.has_value());
        EXPECT_EQ(texture->width, 2u);
        EXPECT_EQ(texture->height, 2u);
        EXPECT_EQ(texture->format, NLS::Render::RHI::TextureFormat::RGBA8);
        ASSERT_FALSE(texture->mips.empty());
        const auto& baseMip = texture->mips.front();
        ASSERT_EQ(baseMip.width, 2u);
        ASSERT_EQ(baseMip.height, 2u);
        ASSERT_GE(baseMip.rowPitch, 8u);
        ASSERT_GE(baseMip.pixels.size(), static_cast<size_t>(baseMip.rowPitch) * baseMip.height);

        const auto expectPixel = [&baseMip](const uint32_t x, const uint32_t y, const uint8_t r, const uint8_t g, const uint8_t b)
        {
            const size_t offset = static_cast<size_t>(y) * baseMip.rowPitch + static_cast<size_t>(x) * 4u;
            EXPECT_EQ(baseMip.pixels[offset + 0u], r);
            EXPECT_EQ(baseMip.pixels[offset + 1u], g);
            EXPECT_EQ(baseMip.pixels[offset + 2u], b);
            EXPECT_EQ(baseMip.pixels[offset + 3u], 255u);
        };
        expectPixel(0u, 0u, 255u, 0u, 0u);
        expectPixel(1u, 0u, 255u, 0u, 0u);
        expectPixel(0u, 1u, 0u, 0u, 255u);
        expectPixel(1u, 1u, 0u, 0u, 255u);
        EXPECT_NE(
            texture->buildIdentity.find(
                "|post=" + std::to_string(NLS::Editor::Assets::kExternalTexturePostprocessorVersion)),
            std::string::npos);
    }

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalObjModelTextureArtifactsKeepLegacyFlippedRowOrder)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "TextureOrientation.obj";
    WriteBinaryFile(root / "Assets" / "Textures" / "TwoRows.png", TwoRowColorPng());
    WriteTextFile(
        root / "Assets" / "Models" / "TextureOrientation.mtl",
        R"(
newmtl Body
map_Kd ../Textures/TwoRows.png
)");
    WriteTextFile(
        sourcePath,
        R"(
mtllib TextureOrientation.mtl
o Body
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl Body
f 1/1/1 2/2/1 3/3/1
)");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929292"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    DisableExternalModelTextureResolution(meta);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "TextureOrientationObj",
        "unit-test",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* textureArtifact = result.manifest.FindSubAsset("texture:parser/texture/0");
    ASSERT_NE(textureArtifact, nullptr);
    const auto texture = NLS::Render::Assets::DeserializeTextureArtifact(
        ReadArtifactFile(root, *textureArtifact));
    ASSERT_TRUE(texture.has_value());
    ASSERT_FALSE(texture->mips.empty());
    const auto& baseMip = texture->mips.front();
    ASSERT_EQ(baseMip.width, 2u);
    ASSERT_EQ(baseMip.height, 2u);
    ASSERT_GE(baseMip.rowPitch, 8u);
    ASSERT_GE(baseMip.pixels.size(), static_cast<size_t>(baseMip.rowPitch) * baseMip.height);

    const auto expectPixel = [&baseMip](const uint32_t x, const uint32_t y, const uint8_t r, const uint8_t g, const uint8_t b)
    {
        const size_t offset = static_cast<size_t>(y) * baseMip.rowPitch + static_cast<size_t>(x) * 4u;
        EXPECT_EQ(baseMip.pixels[offset + 0u], r);
        EXPECT_EQ(baseMip.pixels[offset + 1u], g);
        EXPECT_EQ(baseMip.pixels[offset + 2u], b);
        EXPECT_EQ(baseMip.pixels[offset + 3u], 255u);
    };
    expectPixel(0u, 0u, 0u, 0u, 255u);
    expectPixel(1u, 0u, 0u, 0u, 255u);
    expectPixel(0u, 1u, 255u, 0u, 0u);
    expectPixel(1u, 1u, 255u, 0u, 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportReusesProjectTextureAssetBySourcePath)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "SharedTextureHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "SharedAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));
    NLS::Core::Assets::AssetMeta textureMeta;
    textureMeta.id = textureAssetId;
    textureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(textureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(texturePath)));

    const auto textureArtifactRoot = root / "Library" / "Artifacts";
    auto decodedTexture = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        TinyPng().data(),
        TinyPng().size(),
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        false);
    ASSERT_TRUE(decodedTexture.has_value());
    const auto textureTargetPlatform = TextureArtifactTargetPlatformForTest();
    const auto wrongTextureTargetPlatform = textureTargetPlatform == "editor"
        ? std::string("win64-dx12")
        : std::string("editor");
    decodedTexture->targetPlatform = textureTargetPlatform;
    decodedTexture->encoderId = "rgba8-passthrough";
    decodedTexture->encoderVersion = 1u;
    decodedTexture->buildIdentity = "unit-test-shared-texture";
    const auto textureArtifactRelativePath = WriteTextureArtifactBlobForTest(
        root,
        textureMeta,
        "texture:main",
        "SharedAlbedo",
        textureTargetPlatform,
        *decodedTexture);
    ASSERT_FALSE(textureArtifactRelativePath.empty());
    const auto textureArtifactPath = root / textureArtifactRelativePath;
    decodedTexture->targetPlatform = wrongTextureTargetPlatform;
    decodedTexture->buildIdentity = "unit-test-shared-texture-wrong-platform";
    const auto wrongPlatformTextureArtifactRelativePath = WriteTextureArtifactBlobForTest(
        root,
        textureMeta,
        "texture:wrong-platform",
        "SharedAlbedoWrongPlatform",
        wrongTextureTargetPlatform,
        *decodedTexture);
    ASSERT_FALSE(wrongPlatformTextureArtifactRelativePath.empty());
    const auto wrongPlatformTextureArtifactPath = root / wrongPlatformTextureArtifactRelativePath;

    NLS::Core::Assets::ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureAssetId;
    textureManifest.importerId = textureMeta.importerId;
    textureManifest.importerVersion = textureMeta.importerVersion;
    textureManifest.targetPlatform = textureTargetPlatform;
    textureManifest.primarySubAssetKey = "texture:main";
    NLS::Core::Assets::ArtifactManifest wrongPlatformTextureManifest;
    wrongPlatformTextureManifest.sourceAssetId = textureAssetId;
    wrongPlatformTextureManifest.importerId = textureMeta.importerId;
    wrongPlatformTextureManifest.importerVersion = textureMeta.importerVersion;
    wrongPlatformTextureManifest.targetPlatform = wrongTextureTargetPlatform;
    wrongPlatformTextureManifest.primarySubAssetKey = "texture:wrong-platform";

    textureManifest.subAssets.push_back({
        textureAssetId,
        "texture:main",
        NLS::Core::Assets::ArtifactType::Texture,
        "texture",
        textureTargetPlatform,
        textureArtifactRelativePath.generic_string(),
        "sha256:" + textureArtifactPath.filename().generic_string(),
        "SharedAlbedo"
    });
    wrongPlatformTextureManifest.subAssets.push_back({
        textureAssetId,
        "texture:wrong-platform",
        NLS::Core::Assets::ArtifactType::Texture,
        "texture",
        wrongTextureTargetPlatform,
        wrongPlatformTextureArtifactRelativePath.generic_string(),
        "sha256:" + wrongPlatformTextureArtifactPath.filename().generic_string(),
        "SharedAlbedoWrongPlatform"
    });
    WriteArtifactManifestFile(textureArtifactRoot, textureManifest);
    WriteArtifactManifestFile(textureArtifactRoot, wrongPlatformTextureManifest);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/SharedAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("12121212-1212-4121-8121-121212121212"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "SharedTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(
        std::count_if(
            result.manifest.subAssets.begin(),
            result.manifest.subAssets.end(),
            [](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
            }),
        0);

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    const auto wrongPlatformResourcePath = ToPortableArtifactPath(root, wrongPlatformTextureArtifactPath);
    ExpectMaterialTextureSlot(payload, "_BaseMap", root, textureArtifactPath);
    EXPECT_EQ(payload.find(wrongPlatformResourcePath), std::string::npos);
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aimage%2F0");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportMatchesProjectTexturesCaseInsensitively)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "CaseTextureHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "CaseAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureArtifactPath = WriteImportedTextureAssetForTest(
        root,
        texturePath,
        "15151515-1515-4151-8151-151515151515",
        "editor",
        "CaseAlbedo",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../textures/casealbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "buffers": [
                {
                    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA",
                    "byteLength": 66
                }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 60, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("16161616-1616-4161-8161-161616161616"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "CaseTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(
        std::count_if(
            result.manifest.subAssets.begin(),
            result.manifest.subAssets.end(),
            [](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
            }),
        0);

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    EXPECT_NE(payload.find(textureArtifactPath.lexically_relative(root).generic_string()), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportAutoImportsMissingProjectTextureAssetWhenEnabled)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "AutoImportTextureHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "AutoImportedAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(texturePath)));

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/AutoImportedAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("13131313-1313-4131-8131-131313131313"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    NLS::Editor::Assets::ModelTextureResolutionSettings textureResolutionSettings;
    textureResolutionSettings.autoImportMissingTextureFiles = true;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, textureResolutionSettings);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "AutoImportTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(
        std::count_if(
            result.manifest.subAssets.begin(),
            result.manifest.subAssets.end(),
            [](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
            }),
        0);

    const auto importedTextureMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(texturePath));
    ASSERT_TRUE(importedTextureMeta.has_value());
    EXPECT_TRUE(importedTextureMeta->id.IsValid());
    EXPECT_EQ(importedTextureMeta->assetType, NLS::Core::Assets::AssetType::Texture);
    EXPECT_EQ(importedTextureMeta->importerId, "texture");
    EXPECT_EQ(
        importedTextureMeta->importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::Texture));

    ASSERT_EQ(result.autoImportedDependencies.size(), 1u);
    const auto& importedTextureManifest = result.autoImportedDependencies.front().manifest;
    EXPECT_EQ(result.autoImportedDependencies.front().sourcePath.lexically_normal(), texturePath.lexically_normal());
    ASSERT_EQ(importedTextureManifest.sourceAssetId, importedTextureMeta->id);
    ASSERT_EQ(importedTextureManifest.targetPlatform, TextureArtifactTargetPlatformForTest());
    const auto* importedTextureArtifact = importedTextureManifest.FindPrimaryArtifact();
    ASSERT_NE(importedTextureArtifact, nullptr);
    EXPECT_EQ(importedTextureArtifact->artifactType, NLS::Core::Assets::ArtifactType::Texture);
    EXPECT_EQ(importedTextureArtifact->targetPlatform, TextureArtifactTargetPlatformForTest());
    const auto importedTextureArtifactPath = ResolveTestArtifactPath(root, importedTextureArtifact->artifactPath);
    EXPECT_TRUE(std::filesystem::is_regular_file(importedTextureArtifactPath));

    const auto decodedTexture = NLS::Render::Assets::DeserializeTextureArtifact(
        ReadBinaryFile(importedTextureArtifactPath));
    ASSERT_TRUE(decodedTexture.has_value());
#if defined(_WIN32) && NLS_HAS_DIRECTXTEX
    EXPECT_EQ(decodedTexture->targetPlatform, "win64-dx12");
    EXPECT_EQ(decodedTexture->format, NLS::Render::RHI::TextureFormat::BC1);
    EXPECT_EQ(decodedTexture->encoderId, "directxtex-bc");
#else
    EXPECT_EQ(decodedTexture->targetPlatform, "editor");
    EXPECT_EQ(decodedTexture->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(decodedTexture->encoderId, "rgba8-passthrough");
#endif

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    const auto importedTextureResourcePath =
        ToPortableArtifactPath(root, importedTextureArtifact->artifactPath);
    EXPECT_NE(payload.find("_BaseMap"), std::string::npos);
    EXPECT_NE(payload.find(importedTextureResourcePath), std::string::npos);
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aimage%2F0");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportRecordsProjectTextureFreshnessDependencies)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "FreshnessHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "SharedAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureAssetIdText = "15151515-1515-4151-8151-151515151515";
    const auto textureAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(textureAssetIdText));
    const auto textureArtifactPath = WriteImportedTextureAssetForTest(
        root,
        texturePath,
        textureAssetIdText,
        "editor",
        "freshness-shared",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    ASSERT_FALSE(textureArtifactPath.empty());

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/SharedAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("16161616-1616-4161-8161-161616161616"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    NLS::Editor::Assets::ModelTextureResolutionSettings textureResolutionSettings;
    textureResolutionSettings.autoImportMissingTextureFiles = false;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, textureResolutionSettings);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "FreshnessHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);

    const auto* sourceAssetDependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::SourceAssetGuid,
        textureAssetId.ToString());
    ASSERT_NE(sourceAssetDependency, nullptr);
    EXPECT_FALSE(sourceAssetDependency->hashOrVersion.empty());

    const auto* artifactDependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::ImportedArtifact,
        textureAssetId.ToString() + "#texture:main@" + TextureArtifactTargetPlatformForTest());
    ASSERT_NE(artifactDependency, nullptr);
    const auto textureArtifactContentHash = "sha256:" + textureArtifactPath.filename().generic_string();
    EXPECT_EQ(artifactDependency->hashOrVersion, textureArtifactContentHash);

    const auto* mappingDependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        "project|Assets/Textures/SharedAlbedo.png|source-path");
    ASSERT_NE(mappingDependency, nullptr);
    EXPECT_FALSE(mappingDependency->hashOrVersion.empty());
    EXPECT_NE(mappingDependency->hashOrVersion.find(textureAssetId.ToString()), std::string::npos);
    EXPECT_NE(mappingDependency->hashOrVersion.find("texture:main"), std::string::npos);
    EXPECT_NE(mappingDependency->hashOrVersion.find(textureArtifactContentHash), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportFallsBackToArtifactDatabaseWhenManifestSnapshotMissesTexture)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "SnapshotMissHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "SnapshotMissAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureAssetIdText = "25252525-2525-4252-8252-252525252525";
    const auto textureArtifactPath = WriteImportedTextureAssetForTest(
        root,
        texturePath,
        textureAssetIdText,
        "editor",
        "snapshot-miss-albedo",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    ASSERT_FALSE(textureArtifactPath.empty());

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/SnapshotMissAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("26262626-2626-4262-8262-262626262626"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    NLS::Editor::Assets::ModelTextureResolutionSettings settings;
    settings.autoImportMissingTextureFiles = false;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, settings);

    NLS::Editor::Assets::ExternalModelImportRequest request;
    request.sourcePath = sourcePath;
    request.stagingRoot = root / "Staging";
    request.committedRoot = root / "Library" / "Artifacts" / modelMeta.id.ToString();
    request.meta = modelMeta;
    request.sceneKey = "SnapshotMissHero";
    request.targetPlatform = "editor";
    request.textureResourcePathPrefix = "Models";
    request.projectRoot = root;
    request.loadArtifactManifest = [](
        const NLS::Core::Assets::AssetId,
        const std::string&) -> std::optional<NLS::Core::Assets::ArtifactManifest>
    {
        return std::nullopt;
    };

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset(request);
    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);

    const auto* mappingDependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        "project|Assets/Textures/SnapshotMissAlbedo.png|source-path");
    ASSERT_NE(mappingDependency, nullptr);
    EXPECT_NE(mappingDependency->hashOrVersion.find(textureAssetIdText), std::string::npos);
    EXPECT_NE(mappingDependency->hashOrVersion.find("texture:main"), std::string::npos);
    EXPECT_NE(mappingDependency->hashOrVersion.find("imported"), std::string::npos);
    EXPECT_EQ(mappingDependency->hashOrVersion.find("unimported"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportRecordsPathMappingDependencyWhenTextureArtifactIsMissing)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "MissingArtifactHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "MissingArtifactAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureAssetIdText = "33333333-3333-4333-8333-333333333333";
    const auto textureAssetId = NLS::Core::Assets::AssetId(NLS::Guid::Parse(textureAssetIdText));
    NLS::Core::Assets::AssetMeta textureMeta;
    textureMeta.id = textureAssetId;
    textureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(textureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(texturePath)));

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/MissingArtifactAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("34343434-3434-4343-8343-343434343434"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    NLS::Editor::Assets::ModelTextureResolutionSettings settings;
    settings.autoImportMissingTextureFiles = false;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, settings);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "MissingArtifactHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_TRUE(ContainsDiagnostic(
        result.diagnostics,
        "model-texture-artifact-missing",
        NLS::Core::Assets::AssetDiagnosticSeverity::Warning))
        << JoinDiagnosticSummaries(result.diagnostics);

    const auto* mappingDependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        "project|Assets/Textures/MissingArtifactAlbedo.png|source-path");
    ASSERT_NE(mappingDependency, nullptr);
    EXPECT_NE(mappingDependency->hashOrVersion.find(textureAssetIdText), std::string::npos);
    EXPECT_NE(mappingDependency->hashOrVersion.find("unimported"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportRecordsNameSearchCandidateFreshnessDependencies)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "NameSearchHero.gltf";
    const auto sourceTexturePath = root / "Assets" / "Textures" / "SourceAlbedo.png";
    const auto textureAPath = root / "Assets" / "Textures" / "SharedWood.png";
    WriteBinaryFile(sourceTexturePath, TinyPng());
    WriteBinaryFile(textureAPath, TinyPng());

    const auto textureAIdText = "17171717-1717-4171-8171-171717171717";
    WriteImportedTextureAssetForTest(
        root,
        textureAPath,
        textureAIdText,
        "editor",
        "candidate-a",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "name": "SharedWood", "uri": "../Textures/SourceAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("19191919-1919-4191-8191-191919191919"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    NLS::Editor::Assets::ModelTextureResolutionSettings textureResolutionSettings;
    textureResolutionSettings.autoImportMissingTextureFiles = false;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, textureResolutionSettings);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "NameSearchHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);

    const auto* mappingDependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        "project|SharedWood|name-search");
    ASSERT_NE(mappingDependency, nullptr);
    EXPECT_NE(mappingDependency->hashOrVersion.find(textureAIdText), std::string::npos);
    EXPECT_NE(mappingDependency->hashOrVersion.find("texture:main"), std::string::npos);

    const auto textureBPath = root / "Assets" / "Alternate" / "SharedWood.png";
    WriteBinaryFile(textureBPath, TinyPng());
    const auto textureBIdText = "18181818-1818-4181-8181-181818181818";
    WriteImportedTextureAssetForTest(
        root,
        textureBPath,
        textureBIdText,
        "editor",
        "candidate-b",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);

    const auto changedCandidateSet = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "StagingChanged",
        root / "Library" / "Artifacts" / "changed-name-search-probe",
        modelMeta,
        "NameSearchHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(changedCandidateSet.imported) << JoinDiagnosticSummaries(changedCandidateSet.diagnostics);
    const auto* changedMappingDependency = FindDependency(
        changedCandidateSet.manifest,
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        "project|SharedWood|name-search");
    ASSERT_NE(changedMappingDependency, nullptr);
    EXPECT_NE(changedMappingDependency->hashOrVersion.find(textureAIdText), std::string::npos);
    EXPECT_NE(changedMappingDependency->hashOrVersion.find(textureBIdText), std::string::npos);
    EXPECT_NE(changedMappingDependency->hashOrVersion, mappingDependency->hashOrVersion);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportScansProjectTextureNamesOnceForBatchNameSearch)
{
    const ScopedImportTestRoot tempRoot(MakeImportTestRoot());
    const auto& root = tempRoot.Path();
    const auto sourcePath = root / "Assets" / "Models" / "BatchNameSearchHero.gltf";
    const auto sourceTextureAPath = root / "Assets" / "Textures" / "SourceWoodA.png";
    const auto sourceTextureBPath = root / "Assets" / "Textures" / "SourceWoodB.png";
    const auto candidateAPath = root / "Assets" / "Shared" / "BatchWoodA.png";
    const auto candidateBPath = root / "Assets" / "Shared" / "BatchWoodB.png";
    WriteBinaryFile(sourceTextureAPath, TinyPng());
    WriteBinaryFile(sourceTextureBPath, TinyPng());
    WriteBinaryFile(candidateAPath, TinyPng());
    WriteBinaryFile(candidateBPath, TinyPng());

    const auto candidateAIdText = "2c2c2c2c-2c2c-42c2-82c2-2c2c2c2c2c2c";
    const auto candidateBIdText = "2d2d2d2d-2d2d-42d2-82d2-2d2d2d2d2d2d";
    WriteImportedTextureAssetForTest(
        root,
        candidateAPath,
        candidateAIdText,
        "editor",
        "batch-name-search-a",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    WriteImportedTextureAssetForTest(
        root,
        candidateBPath,
        candidateBIdText,
        "editor",
        "batch-name-search-b",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "name": "BatchWoodA", "uri": "../Textures/SourceWoodA.png", "mimeType": "image/png" },
                { "name": "BatchWoodB", "uri": "../Textures/SourceWoodB.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 },
                { "source": 1 }
            ],
            "materials": [
                {
                    "name": "BodyA",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                },
                {
                    "name": "BodyB",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 1 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{
                "name": "BodyMesh",
                "primitives": [
                    { "attributes": {}, "material": 0 },
                    { "attributes": {}, "material": 1 }
                ]
            }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("2e2e2e2e-2e2e-42e2-82e2-2e2e2e2e2e2e"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    NLS::Editor::Assets::ModelTextureResolutionSettings textureResolutionSettings;
    textureResolutionSettings.autoImportMissingTextureFiles = false;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, textureResolutionSettings);

    std::vector<std::string> progressMessages;
    NLS::Editor::Assets::ImportProgressTracker progressTracker;
    progressTracker.Subscribe(
        [&progressMessages](const NLS::Editor::Assets::ImportProgressEvent& event)
        {
            progressMessages.push_back(event.message);
        });
    const auto progressJob = progressTracker.BeginJob(
        modelMeta.id,
        "Assets/Models/BatchNameSearchHero.gltf",
        "editor",
        1u);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "BatchNameSearchHero",
        "editor",
        nullptr,
        &progressTracker,
        progressJob,
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* candidateADependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        "project|BatchWoodA|name-search");
    ASSERT_NE(candidateADependency, nullptr);
    EXPECT_NE(candidateADependency->hashOrVersion.find(candidateAIdText), std::string::npos);
    const auto* candidateBDependency = FindDependency(
        result.manifest,
        NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping,
        "project|BatchWoodB|name-search");
    ASSERT_NE(candidateBDependency, nullptr);
    EXPECT_NE(candidateBDependency->hashOrVersion.find(candidateBIdText), std::string::npos);

    const auto nameSearchProgress = std::find_if(
        progressMessages.begin(),
        progressMessages.end(),
        [](const std::string& message)
        {
            return message.rfind("Resolved model texture name candidates |", 0u) == 0u;
        });
    ASSERT_NE(nameSearchProgress, progressMessages.end());
    EXPECT_NE(nameSearchProgress->find("processed=2"), std::string::npos) << *nameSearchProgress;
    EXPECT_NE(nameSearchProgress->find("total=2"), std::string::npos) << *nameSearchProgress;
    EXPECT_NE(nameSearchProgress->find("candidates=2"), std::string::npos) << *nameSearchProgress;
    EXPECT_NE(nameSearchProgress->find("scanCount=1"), std::string::npos) << *nameSearchProgress;
}

TEST(AssetImportPipelineTests, ExternalModelImportKeepsPreviousTextureResolutionReportWhenReimportFails)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "ReportHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "ReportAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureAssetIdText = "1a1a1a1a-1a1a-41a1-81a1-1a1a1a1a1a1a";
    WriteImportedTextureAssetForTest(
        root,
        texturePath,
        textureAssetIdText,
        "editor",
        "report-albedo",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);

    const auto validGltf = R"({
        "asset": { "version": "2.0" },
        "images": [
            { "uri": "../Textures/ReportAlbedo.png", "mimeType": "image/png" }
        ],
        "textures": [
            { "source": 0 }
        ],
        "materials": [
            {
                "name": "Body",
                "pbrMetallicRoughness": {
                    "baseColorTexture": { "index": 0 }
                }
            }
        ],
        "scene": 0,
        "scenes": [{ "nodes": [0] }],
        "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
        "nodes": [{ "name": "Root", "mesh": 0 }]
    })";
    WriteTextFile(sourcePath, validGltf);

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("1b1b1b1b-1b1b-41b1-81b1-1b1b1b1b1b1b"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto committedRoot = root / "Library" / "Artifacts" / modelMeta.id.ToString();
    const auto reportPath = NLS::Editor::Assets::ModelTextureResolutionReportPath(committedRoot);
    const auto firstResult = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "StagingA",
        committedRoot,
        modelMeta,
        "ReportHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(firstResult.imported) << JoinDiagnosticSummaries(firstResult.diagnostics);
    ASSERT_TRUE(std::filesystem::is_regular_file(reportPath));
    const auto originalReportText = ReadTextFile(reportPath);
    const auto originalReport = NLS::Editor::Assets::ParseModelTextureResolutionReport(originalReportText);
    ASSERT_TRUE(originalReport.has_value());
    ASSERT_EQ(originalReport->entries.size(), 1u);
    EXPECT_EQ(originalReport->entries[0].kind, NLS::Editor::Assets::ModelTextureResolutionKind::SourcePath);

    const auto missingObjPath = root / "Assets" / "Models" / "ReportHeroMissing.obj";
    const auto failedResult = NLS::Editor::Assets::ImportExternalModelAsset({
        missingObjPath,
        root / "StagingB",
        committedRoot,
        modelMeta,
        "ReportHero",
        "editor",
        &firstResult.manifest,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    EXPECT_FALSE(failedResult.imported);
    EXPECT_EQ(ReadTextFile(reportPath), originalReportText);

    const auto secondResult = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "StagingC",
        committedRoot,
        modelMeta,
        "ReportHero",
        "editor",
        &firstResult.manifest,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(secondResult.imported) << JoinDiagnosticSummaries(secondResult.diagnostics);
    const auto replacedReport = NLS::Editor::Assets::ParseModelTextureResolutionReport(ReadTextFile(reportPath));
    ASSERT_TRUE(replacedReport.has_value());
    EXPECT_EQ(replacedReport->modelAssetId, modelMeta.id.ToString());
    EXPECT_EQ(replacedReport->targetPlatform, "editor");
    EXPECT_EQ(replacedReport->importerVersion, modelMeta.importerVersion);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalGltfImportDefersExternalTexturePayloadReadsWhenProjectTextureResolutionIsEnabled)
{
    const ScopedImportTestRoot tempRoot(MakeImportTestRoot());
    const auto& root = tempRoot.Path();
    const auto sourcePath = root / "Assets" / "Models" / "DeferredTexturePayloadHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "DeferredAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/DeferredAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("71717171-7171-4171-8171-717171717171"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    NLS::Editor::Assets::ModelTextureResolutionSettings textureResolutionSettings;
    textureResolutionSettings.autoImportMissingTextureFiles = true;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, textureResolutionSettings);

    std::vector<std::string> progressMessages;
    NLS::Editor::Assets::ImportProgressTracker progressTracker;
    progressTracker.Subscribe(
        [&progressMessages](const NLS::Editor::Assets::ImportProgressEvent& event)
        {
            progressMessages.push_back(event.message);
        });
    const auto progressJob = progressTracker.BeginJob(
        modelMeta.id,
        "Assets/Models/DeferredTexturePayloadHero.gltf",
        "editor",
        1u);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "DeferredTexturePayloadHero",
        "editor",
        nullptr,
        &progressTracker,
        progressJob,
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);

    const auto loadedPayloads = std::find_if(
        progressMessages.begin(),
        progressMessages.end(),
        [](const std::string& message)
        {
            return message.rfind("Loaded texture payloads |", 0u) == 0u;
        });
    ASSERT_NE(loadedPayloads, progressMessages.end());
    EXPECT_NE(loadedPayloads->find("bytes=0"), std::string::npos) << *loadedPayloads;
    EXPECT_NE(loadedPayloads->find("externalFile=0/0"), std::string::npos) << *loadedPayloads;

    const auto importedTextureMeta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(texturePath));
    ASSERT_TRUE(importedTextureMeta.has_value());
    EXPECT_EQ(importedTextureMeta->assetType, NLS::Core::Assets::AssetType::Texture);
    ASSERT_EQ(result.autoImportedDependencies.size(), 1u);
    const auto* importedTextureArtifact = result.autoImportedDependencies.front().manifest.FindPrimaryArtifact();
    ASSERT_NE(importedTextureArtifact, nullptr);
    const auto importedTextureArtifactPath = ResolveTestArtifactPath(root, importedTextureArtifact->artifactPath);
    EXPECT_TRUE(std::filesystem::is_regular_file(importedTextureArtifactPath));
}

TEST(AssetImportPipelineTests, ExternalGltfImportDefersBulkProjectTextureAutoImport)
{
    const ScopedImportTestRoot tempRoot(MakeImportTestRoot());
    const auto& root = tempRoot.Path();
    const auto sourcePath = root / "Assets" / "Models" / "BulkTextureHero.gltf";

    std::ostringstream images;
    std::ostringstream textures;
    std::ostringstream materials;
    constexpr size_t kTextureCount = 9u;
    for (size_t index = 0u; index < kTextureCount; ++index)
    {
        const auto textureName = "BulkAlbedo" + std::to_string(index) + ".png";
        WriteBinaryFile(root / "Assets" / "Textures" / textureName, TwoRowColorPng());

        if (index > 0u)
        {
            images << ',';
            textures << ',';
            materials << ',';
        }
        images << "{ \"uri\": \"../Textures/" << textureName << "\", \"mimeType\": \"image/png\" }";
        textures << "{ \"source\": " << index << " }";
        materials <<
            "{ \"name\": \"Body" << index << "\","
            " \"pbrMetallicRoughness\": { \"baseColorTexture\": { \"index\": " << index << " } } }";
    }

    std::ostringstream gltf;
    gltf <<
        "{"
        "\"asset\": { \"version\": \"2.0\" },"
        "\"images\": [" << images.str() << "],"
        "\"textures\": [" << textures.str() << "],"
        "\"materials\": [" << materials.str() << "],"
        "\"scene\": 0,"
        "\"scenes\": [{ \"nodes\": [0] }],"
        "\"meshes\": [{ \"name\": \"BodyMesh\", \"primitives\": [{ \"attributes\": {}, \"material\": 0 }] }],"
        "\"nodes\": [{ \"name\": \"Root\", \"mesh\": 0 }]"
        "}";
    WriteTextFile(sourcePath, gltf.str());

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("72727272-7272-4272-8272-727272727272"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    NLS::Editor::Assets::ModelTextureResolutionSettings textureResolutionSettings;
    textureResolutionSettings.autoImportMissingTextureFiles = true;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, textureResolutionSettings);

    std::vector<std::string> progressMessages;
    NLS::Editor::Assets::ImportProgressTracker progressTracker;
    progressTracker.Subscribe(
        [&progressMessages](const NLS::Editor::Assets::ImportProgressEvent& event)
        {
            progressMessages.push_back(event.message);
        });
    const auto progressJob = progressTracker.BeginJob(
        modelMeta.id,
        "Assets/Models/BulkTextureHero.gltf",
        "editor",
        1u);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "BulkTextureHero",
        "editor",
        nullptr,
        &progressTracker,
        progressJob,
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_TRUE(result.autoImportedDependencies.empty())
        << "Bulk external texture resolution should keep the model import path bounded.";
    for (size_t index = 0u; index < kTextureCount; ++index)
    {
        const auto texturePath =
            root / "Assets" / "Textures" / ("BulkAlbedo" + std::to_string(index) + ".png");
        EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(texturePath)));
    }

    const auto textureArtifactCount = std::count_if(
        result.manifest.subAssets.begin(),
        result.manifest.subAssets.end(),
        [](const NLS::Core::Assets::ImportedArtifact& artifact)
        {
            return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
        });
    EXPECT_EQ(textureArtifactCount, 0)
        << "Bulk external glTF textures should not be synchronously decoded into model-local artifacts.";
    size_t bulkTextureSourceHashDependencyCount = 0u;
    size_t bulkTextureMappingDependencyCount = 0u;
    for (const auto& dependency : result.manifest.dependencies)
    {
        if (dependency.value.find("BulkAlbedo") == std::string::npos)
            continue;
        if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::SourceFileHash)
            ++bulkTextureSourceHashDependencyCount;
        if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping)
            ++bulkTextureMappingDependencyCount;
    }
    EXPECT_EQ(bulkTextureSourceHashDependencyCount, 0u)
        << "Deferred texture contents are not imported into this model artifact and must not be hashed during prefab validation.";
    EXPECT_EQ(bulkTextureMappingDependencyCount, kTextureCount)
        << "Deferred texture path mappings still need to invalidate when those textures become imported project assets.";
    for (const auto& artifact : result.manifest.subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Material)
            continue;

        const auto materialPayload = ReadArtifactPayloadText(
            root,
            artifact,
            NLS::Core::Assets::ArtifactType::Material,
            1u);
        EXPECT_EQ(materialPayload.find("BulkAlbedo"), std::string::npos)
            << artifact.subAssetKey << "\n" << materialPayload;
        EXPECT_EQ(materialPayload.find("property _BaseMap Texture2D"), std::string::npos)
            << artifact.subAssetKey << "\n" << materialPayload;
        EXPECT_NE(materialPayload.find("resourcePath= colorSpace=sRGB"), std::string::npos)
            << artifact.subAssetKey << "\n" << materialPayload;
    }

    const auto deferredBulkAutoImport = std::find_if(
        progressMessages.begin(),
        progressMessages.end(),
        [](const std::string& message)
        {
            return message.rfind("Deferred bulk project texture auto-import |", 0u) == 0u;
        });
    EXPECT_NE(deferredBulkAutoImport, progressMessages.end());

    const auto deferredBulkTextureArtifacts = std::find_if(
        progressMessages.begin(),
        progressMessages.end(),
        [](const std::string& message)
        {
            return message.rfind("Deferred bulk model-local texture artifacts |", 0u) == 0u;
        });
    EXPECT_NE(deferredBulkTextureArtifacts, progressMessages.end());
}

TEST(AssetImportPipelineTests, ExternalModelImportAutoImportsTextureWithExistingMetaButMissingManifest)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "AutoImportFailureHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "BlockedAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("14141414-1414-4141-8141-141414141414"));
    NLS::Core::Assets::AssetMeta textureMeta;
    textureMeta.id = textureAssetId;
    textureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(textureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(texturePath)));

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/BlockedAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("15151515-1515-4151-8151-151515151515"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    NLS::Editor::Assets::ModelTextureResolutionSettings textureResolutionSettings;
    textureResolutionSettings.autoImportMissingTextureFiles = true;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, textureResolutionSettings);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "AutoImportExistingMetaHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_FALSE(ContainsDiagnostic(
        result.diagnostics,
        "model-texture-auto-import-failed",
        NLS::Core::Assets::AssetDiagnosticSeverity::Warning))
        << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_FALSE(ContainsDiagnosticSeverity(result.diagnostics, NLS::Core::Assets::AssetDiagnosticSeverity::Error))
        << JoinDiagnosticSummaries(result.diagnostics);

    EXPECT_EQ(result.manifest.FindSubAsset("texture:image/0"), nullptr);
    ASSERT_EQ(result.autoImportedDependencies.size(), 1u);
    const auto* textureArtifact = result.autoImportedDependencies[0].manifest.FindSubAsset("texture:main");
    ASSERT_NE(textureArtifact, nullptr);
    EXPECT_EQ(textureArtifact->artifactType, NLS::Core::Assets::ArtifactType::Texture);
    EXPECT_TRUE(std::filesystem::is_regular_file(ResolveTestArtifactPath(root, textureArtifact->artifactPath)));
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "ArtifactDB"))
        << "The low-level external importer returns auto-imported dependency manifests; "
           "AssetDatabaseFacade owns ArtifactDB persistence so facade cache writes cannot be overwritten.";

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    ExpectMaterialTextureSlot(payload, "_BaseMap", root, textureArtifact->artifactPath);
    EXPECT_EQ(payload.find("Library/Artifacts/" + textureAssetId.ToString() + "/texture.ntex"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportDoesNotWriteArtifactDatabaseDirectlyForAutoImportedTextures)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "AutoImportCorruptDbHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "CorruptDbAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("14141414-1414-4141-8141-151515151515"));
    NLS::Core::Assets::AssetMeta textureMeta;
    textureMeta.id = textureAssetId;
    textureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(textureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(texturePath)));

    const auto artifactDatabasePath = root / "Library" / "ArtifactDB";
    WriteTextFile(artifactDatabasePath, "artifact database sentinel");

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/CorruptDbAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("15151515-1515-4151-8151-161616161616"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    NLS::Editor::Assets::ModelTextureResolutionSettings textureResolutionSettings;
    textureResolutionSettings.autoImportMissingTextureFiles = true;
    NLS::Editor::Assets::StoreModelTextureResolutionSettings(modelMeta, textureResolutionSettings);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "AutoImportCorruptDbHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_FALSE(ContainsDiagnostic(
        result.diagnostics,
        "model-texture-auto-import-manifest-persist-failed",
        NLS::Core::Assets::AssetDiagnosticSeverity::Warning))
        << JoinDiagnosticSummaries(result.diagnostics);
    ASSERT_EQ(result.autoImportedDependencies.size(), 1u);
    EXPECT_EQ(ReadTextFile(artifactDatabasePath), "artifact database sentinel")
        << "ImportExternalModelAsset must not bypass AssetDatabaseFacade's ArtifactDB cache.";

    std::set<std::filesystem::path> expectedArtifacts;
    for (const auto& artifact : result.manifest.subAssets)
        expectedArtifacts.insert(ResolveTestArtifactPath(root, artifact.artifactPath).lexically_normal());
    expectedArtifacts.insert(
        NLS::Editor::Assets::ModelTextureResolutionReportPath(
            root / "Library" / "Artifacts" / modelMeta.id.ToString()).lexically_normal());
    for (const auto& dependency : result.autoImportedDependencies)
    {
        for (const auto& artifact : dependency.manifest.subAssets)
            expectedArtifacts.insert(ResolveTestArtifactPath(root, artifact.artifactPath).lexically_normal());
    }

    std::set<std::filesystem::path> actualArtifacts;
    const auto libraryArtifacts = root / "Library" / "Artifacts";
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(libraryArtifacts, error), end;
        !error && it != end;
        it.increment(error))
    {
        if (it->is_regular_file(error) && !error)
            actualArtifacts.insert(it->path().lexically_normal());
        error.clear();
    }
    EXPECT_EQ(actualArtifacts, expectedArtifacts);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportRollsBackAutoImportedTextureWhenModelImportFails)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "RollbackAutoImportHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "RollbackAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/RollbackAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("23232323-2323-4323-8323-232323232323"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    DisableExternalModelTextureResolution(modelMeta);
    const auto blockedModelArtifactRoot = root / "BlockedModelArtifactRoot";
    WriteTextFile(blockedModelArtifactRoot, "not a directory");

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        blockedModelArtifactRoot,
        modelMeta,
        "RollbackAutoImportHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(texturePath)));

    const auto libraryArtifacts = root / "Library" / "Artifacts";
    EXPECT_EQ(CountRegularFilesRecursive(libraryArtifacts), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportRollsBackAutoImportedTextureWhenEarlyConversionFails)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "RollbackEarlyAutoImportHero.gltf";
    const auto validTexturePath = root / "Assets" / "Textures" / "RollbackGoodAlbedo.png";
    const auto invalidTexturePath = root / "Assets" / "Textures" / "RollbackBadAlbedo.png";
    WriteBinaryFile(validTexturePath, TinyPng());
    WriteTextFile(invalidTexturePath, "not an image");

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/RollbackGoodAlbedo.png", "mimeType": "image/png" },
                { "uri": "../Textures/RollbackBadAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 },
                { "source": 1 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("24242424-2424-4242-8424-242424242424"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "RollbackEarlyAutoImportHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(validTexturePath)));
    EXPECT_FALSE(std::filesystem::exists(NLS::Core::Assets::GetAssetMetaPath(invalidTexturePath)));

    const auto libraryArtifacts = root / "Library" / "Artifacts";
    EXPECT_EQ(CountRegularFilesRecursive(libraryArtifacts), 0u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportKeepsModelLocalTextureForEmbeddedFallback)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "EmbeddedTextureHero.glb";
    WriteBinaryFile(root / "Assets" / "Textures" / "EmbeddedAlbedo.png", TinyPng());

    std::vector<uint8_t> binaryChunk = TinyPng();
    PadToFour(binaryChunk, 0u);
    std::ostringstream gltf;
    gltf << R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "byteLength": )" << binaryChunk.size() << R"( }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": )" << TinyPng().size() << R"( }
            ],
            "images": [
                { "bufferView": 0, "mimeType": "image/png", "name": "EmbeddedAlbedo" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })";
    WriteBinaryFile(sourcePath, MakeGlb(gltf.str(), binaryChunk));

    const auto externalTextureAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("16161616-1616-4161-8161-161616161616"));
    NLS::Core::Assets::AssetMeta externalTextureMeta;
    externalTextureMeta.id = externalTextureAssetId;
    externalTextureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    externalTextureMeta.importerId = "texture";
    externalTextureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(externalTextureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(
        root / "Assets" / "Textures" / "EmbeddedAlbedo.png")));

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("17171717-1717-4171-8171-171717171717"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "EmbeddedTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* textureArtifact = result.manifest.FindSubAsset("texture:image/0");
    ASSERT_NE(textureArtifact, nullptr);
    EXPECT_EQ(textureArtifact->artifactType, NLS::Core::Assets::ArtifactType::Texture);

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    ExpectMaterialTextureSlot(payload, "_BaseMap", root, textureArtifact->artifactPath);
    EXPECT_EQ(payload.find("Library/Artifacts/" + externalTextureAssetId.ToString()), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalGltfModelImportKeepsBaseColorAndNormalMaterialTextureKeysWhenExternallyResolved)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "ExternalGltfTextureSlots.gltf";
    const auto baseColorPath = root / "Assets" / "Textures" / "HeroBaseColor.png";
    const auto normalPath = root / "Assets" / "Textures" / "HeroNormal.png";
    WriteBinaryFile(baseColorPath, TinyPng());
    WriteBinaryFile(normalPath, TinyPng());

    const auto baseColorArtifactPath = WriteImportedTextureAssetForTest(
        root,
        baseColorPath,
        "18181818-1818-4181-8181-181818181818",
        "editor",
        "HeroBaseColor",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    const auto normalArtifactPath = WriteImportedTextureAssetForTest(
        root,
        normalPath,
        "19191919-1919-4191-8191-191919191919",
        "editor",
        "HeroNormal",
        NLS::Render::Assets::TextureArtifactColorSpace::Linear);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" },
                { "uri": "../Textures/HeroNormal.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 },
                { "source": 1 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    },
                    "normalTexture": { "index": 1 }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("20202020-2020-4202-8202-202020202020"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "ExternalGltfTextureSlots",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(result.manifest.FindSubAsset("texture:image/0"), nullptr);
    EXPECT_EQ(result.manifest.FindSubAsset("texture:image/1"), nullptr);

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    ExpectMaterialTextureSlot(payload, "_BaseMap", root, baseColorArtifactPath);
    ExpectMaterialTextureSlot(payload, "_NormalMap", root, normalArtifactPath);
    EXPECT_NE(payload.find("keyword _NORMALMAP"), std::string::npos);
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aimage%2F0");
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aimage%2F1");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalGltfModelImportFallsBackWhenProjectNormalTextureIsSrgb)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "SrgbProjectNormal.gltf";
    const auto baseColorPath = root / "Assets" / "Textures" / "HeroBaseColor.png";
    const auto normalPath = root / "Assets" / "Textures" / "HeroNormal.png";
    WriteBinaryFile(baseColorPath, TinyPng());
    WriteBinaryFile(normalPath, TinyPng());

    const auto baseColorArtifactPath = WriteImportedTextureAssetForTest(
        root,
        baseColorPath,
        "41414141-4141-4141-8141-414141414141",
        "editor",
        "HeroBaseColor",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    const auto incompatibleNormalArtifactPath = WriteImportedTextureAssetForTest(
        root,
        normalPath,
        "42424242-4242-4242-8242-424242424242",
        "editor",
        "HeroNormal",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" },
                { "uri": "../Textures/HeroNormal.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 },
                { "source": 1 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    },
                    "normalTexture": { "index": 1 }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("43434343-4343-4343-8343-434343434343"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "SrgbProjectNormal",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(result.manifest.FindSubAsset("texture:image/0"), nullptr);
    const auto* normalArtifact = result.manifest.FindSubAsset("texture:image/1");
    ASSERT_NE(normalArtifact, nullptr);

    const auto normal = NLS::Render::Assets::DeserializeTextureArtifact(
        ReadArtifactFile(root, *normalArtifact));
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(normal->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Linear);

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto materialPayload = ReadArtifactPayloadText(
        root,
        *materialArtifact,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    ExpectMaterialTextureSlot(materialPayload, "_BaseMap", root, baseColorArtifactPath);
    ExpectMaterialTextureSlot(materialPayload, "_NormalMap", root, normalArtifact->artifactPath);
    EXPECT_EQ(
        materialPayload.find(ToPortableArtifactPath(root, incompatibleNormalArtifactPath)),
        std::string::npos);

    const auto* diagnostic = FindDiagnosticByCode(
        result.diagnostics,
        "model-texture-artifact-color-space-mismatch");
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(diagnostic->message.find("expected Linear"), std::string::npos);
    EXPECT_NE(diagnostic->message.find("found sRGB"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportReportsUnsupportedExternallyResolvedTextureEncodingWithoutDuplicateSubAssets)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "UnsupportedExternalTextureHero.gltf";
    const auto unsupportedPath = root / "Assets" / "Textures" / "HeroUnsupported.bmp";
    WriteBinaryFile(unsupportedPath, TinyPng());

    const auto unsupportedArtifactPath = WriteImportedTextureAssetForTest(
        root,
        unsupportedPath,
        "33333333-3333-4333-8333-333333333333",
        "editor",
        "HeroUnsupported",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/HeroUnsupported.bmp", "mimeType": "image/bmp" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("34343434-3434-4343-8343-343434343434"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto committedRoot = root / "Library" / "Artifacts" / modelMeta.id.ToString();
    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        committedRoot,
        modelMeta,
        "UnsupportedExternalTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(result.manifest.FindSubAsset("texture:image/0"), nullptr);

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    EXPECT_NE(payload.find(unsupportedArtifactPath.lexically_relative(root).generic_string()), std::string::npos);
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aimage%2F0");

    const auto reportText = ReadTextFile(NLS::Editor::Assets::ModelTextureResolutionReportPath(committedRoot));
    const auto report = NLS::Editor::Assets::ParseModelTextureResolutionReport(reportText);
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(report->entries.size(), 1u);

    const auto unsupportedEntry = std::find_if(
        report->entries.begin(),
        report->entries.end(),
        [](const NLS::Editor::Assets::ResolvedModelTextureReference& entry)
        {
            return entry.source.normalizedUri == "Assets/Textures/HeroUnsupported.bmp";
        });
    ASSERT_NE(unsupportedEntry, report->entries.end());
    EXPECT_EQ(unsupportedEntry->kind, NLS::Editor::Assets::ModelTextureResolutionKind::SourcePath);
    EXPECT_TRUE(std::any_of(
        unsupportedEntry->diagnostics.begin(),
        unsupportedEntry->diagnostics.end(),
        [](const NLS::Editor::Assets::ModelTextureDiagnostic& diagnostic)
        {
            return diagnostic.code == "material-unsupported-texture-encoding";
        }));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelReimportPrunesOnlyExternallyResolvedLegacyTextureSubAssets)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "LegacyTextureReimport.glb";
    const auto baseColorPath = root / "Assets" / "Textures" / "LegacyBaseColor.png";
    WriteBinaryFile(baseColorPath, TinyPng());

    std::vector<uint8_t> binaryChunk = TinyPng();
    PadToFour(binaryChunk, 0u);
    std::ostringstream gltf;
    gltf << R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "byteLength": )" << binaryChunk.size() << R"( }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": )" << TinyPng().size() << R"( }
            ],
            "images": [
                { "uri": "../Textures/LegacyBaseColor.png", "mimeType": "image/png" },
                { "bufferView": 0, "mimeType": "image/png", "name": "EmbeddedNormal" }
            ],
            "textures": [
                { "source": 0 },
                { "source": 1 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    },
                    "normalTexture": { "index": 1 }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })";
    WriteBinaryFile(sourcePath, MakeGlb(gltf.str(), binaryChunk));

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("35353535-3535-4353-8353-353535353535"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    auto legacyMeta = modelMeta;
    DisableExternalModelTextureResolution(legacyMeta);

    const auto committedRoot = root / "Library" / "Artifacts" / modelMeta.id.ToString();
    const auto legacyResult = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "LegacyStaging",
        committedRoot,
        legacyMeta,
        "LegacyTextureReimport",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(legacyResult.imported) << JoinDiagnosticSummaries(legacyResult.diagnostics);
    ASSERT_NE(legacyResult.manifest.FindSubAsset("texture:image/0"), nullptr);
    ASSERT_NE(legacyResult.manifest.FindSubAsset("texture:image/1"), nullptr);
    ASSERT_NE(legacyResult.manifest.FindSubAsset("mesh:mesh/0"), nullptr);
    ASSERT_NE(legacyResult.manifest.FindSubAsset("material:material/0"), nullptr);
    ASSERT_NE(legacyResult.manifest.FindSubAsset("prefab:LegacyTextureReimport"), nullptr);

    const auto projectTextureArtifactPath = WriteImportedTextureAssetForTest(
        root,
        baseColorPath,
        "36363636-3636-4363-8363-363636363636",
        "editor",
        "LegacyBaseColor",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);

    const auto reimportResult = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "ReimportStaging",
        committedRoot,
        modelMeta,
        "LegacyTextureReimport",
        "editor",
        &legacyResult.manifest,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(reimportResult.imported) << JoinDiagnosticSummaries(reimportResult.diagnostics);
    EXPECT_EQ(reimportResult.manifest.FindSubAsset("texture:image/0"), nullptr);
    ASSERT_NE(reimportResult.manifest.FindSubAsset("texture:image/1"), nullptr);
    EXPECT_NE(reimportResult.manifest.FindSubAsset("mesh:mesh/0"), nullptr);
    EXPECT_NE(reimportResult.manifest.FindSubAsset("material:material/0"), nullptr);
    EXPECT_NE(reimportResult.manifest.FindSubAsset("prefab:LegacyTextureReimport"), nullptr);

    const auto* materialArtifact = reimportResult.manifest.FindSubAsset("material:material/0");
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    EXPECT_NE(payload.find(projectTextureArtifactPath.lexically_relative(root).generic_string()), std::string::npos);
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aimage%2F0");
    EXPECT_NE(payload.find("_NormalMap"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalObjModelImportKeepsDiffuseOpacityAndNormalMaterialTextureKeysWhenExternallyResolved)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "ExternalObjTextureSlots.obj";
    const auto diffusePath = root / "Assets" / "Textures" / "HeroDiffuse.png";
    const auto opacityPath = root / "Assets" / "Textures" / "HeroOpacity.png";
    const auto normalPath = root / "Assets" / "Textures" / "HeroNormal.png";
    WriteBinaryFile(diffusePath, TinyPng());
    WriteBinaryFile(opacityPath, TinyPng());
    WriteBinaryFile(normalPath, TinyPng());

    const auto diffuseArtifactPath = WriteImportedTextureAssetForTest(
        root,
        diffusePath,
        "21212121-2121-4212-8212-212121212121",
        "editor",
        "HeroDiffuse",
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    const auto opacityArtifactPath = WriteImportedTextureAssetForTest(
        root,
        opacityPath,
        "22222222-2222-4222-8222-222222222222",
        "editor",
        "HeroOpacity",
        NLS::Render::Assets::TextureArtifactColorSpace::Linear);
    const auto normalArtifactPath = WriteImportedTextureAssetForTest(
        root,
        normalPath,
        "23232323-2323-4232-8232-232323232323",
        "editor",
        "HeroNormal",
        NLS::Render::Assets::TextureArtifactColorSpace::Linear);

    WriteTextFile(
        root / "Assets" / "Models" / "ExternalObjTextureSlots.mtl",
        R"(
newmtl HeroMaterial
map_Kd ../Textures/HeroDiffuse.png
map_d ../Textures/HeroOpacity.png
map_Bump ../Textures/HeroNormal.png
)");
    WriteTextFile(
        sourcePath,
        R"(
mtllib ExternalObjTextureSlots.mtl
o Hero
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
vn 0 0 1
usemtl HeroMaterial
f 1/1/1 2/2/1 3/3/1
)");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("24242424-2424-4242-8242-242424242424"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "ExternalObjTextureSlots",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(
        std::count_if(
            result.manifest.subAssets.begin(),
            result.manifest.subAssets.end(),
            [](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
            }),
        0);

    const auto* materialArtifact = result.manifest.FindSubAsset("material:parser/material/1");
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    ExpectMaterialTextureSlot(payload, "_BaseMap", root, diffuseArtifactPath);
    ExpectMaterialTextureSlot(payload, "_NormalMap", root, normalArtifactPath);
    EXPECT_NE(payload.find("keyword _NORMALMAP"), std::string::npos);
    ExpectMaterialTextureSlot(payload, "_OpacityMap", root, opacityArtifactPath);
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aparser%2Ftexture%2F0");
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aparser%2Ftexture%2F1");
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aparser%2Ftexture%2F2");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalGltfModelTextureArtifactsUseMaterialSlotColorSpace)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "NormalMapHero.gltf";
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroBaseColor.png", TinyPng());
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroNormal.png", TinyPng());
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/HeroBaseColor.png", "mimeType": "image/png" },
                { "uri": "../Textures/HeroNormal.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 },
                { "source": 1 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    },
                    "normalTexture": { "index": 1 }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("98989898-9898-4898-8898-989898989898"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    DisableExternalModelTextureResolution(meta);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "NormalMapHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    const auto* baseColorArtifact = result.manifest.FindSubAsset("texture:image/0");
    const auto* normalArtifact = result.manifest.FindSubAsset("texture:image/1");
    ASSERT_NE(baseColorArtifact, nullptr);
    ASSERT_NE(normalArtifact, nullptr);

    const auto baseColor = NLS::Render::Assets::DeserializeTextureArtifact(ReadArtifactFile(root, *baseColorArtifact));
    const auto normal = NLS::Render::Assets::DeserializeTextureArtifact(ReadArtifactFile(root, *normalArtifact));
    ASSERT_TRUE(baseColor.has_value());
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(baseColor->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    EXPECT_EQ(normal->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Linear);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportFailsClosedWhenTextureArtifactPayloadCannotBeSerialized)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "BrokenTextureHero.gltf";
    WriteTextFile(root / "Assets" / "Textures" / "BrokenBaseColor.png", "not-a-decodable-image");
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/BrokenBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("96969696-9696-4696-8696-969696969696"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "BrokenTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    EXPECT_TRUE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-texture-decode-failed"))
        << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "Artifacts" / meta.id.ToString() / "textures"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportKeepsOversizedReferencedTexturesReadableByDefault)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "LargeTextureHero.gltf";
    WriteBinaryFile(
        root / "Assets" / "Textures" / "LargeBaseColor.bmp",
        UncompressedBmp(2049u, 1u, 200u, 120u, 40u));
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/LargeBaseColor.bmp", "mimeType": "image/bmp" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("94949494-9494-4494-8494-949494949494"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "LargeTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_FALSE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-texture-size-limit"))
        << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(result.manifest.FindSubAsset("texture:image/0"), nullptr);
    const auto texture = ReadAutoImportedTextureArtifact(result, root, "LargeBaseColor.bmp");
    ASSERT_TRUE(texture.has_value());
    EXPECT_EQ(texture->width, 2049u);
    EXPECT_EQ(texture->height, 1u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportFailsClosedForPathologicalTextureDimensions)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "PathologicalTextureHero.gltf";
    WriteBinaryFile(
        root / "Assets" / "Textures" / "TooWideBaseColor.bmp",
        UncompressedBmp(16385u, 1u, 64u, 64u, 64u));
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/TooWideBaseColor.bmp", "mimeType": "image/bmp" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("92929292-9292-4292-8292-929292929292"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "PathologicalTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    EXPECT_TRUE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-texture-safety-limit"))
        << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_FALSE(std::filesystem::exists(root / "Library" / "Artifacts" / meta.id.ToString() / "textures"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalGltfNormalTexturesUseNormalMipIntent)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "NormalMipHero.gltf";
    WriteBinaryFile(root / "Assets" / "Textures" / "HeroNormal.png", NormalMap2x2Png());
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/HeroNormal.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "normalTexture": { "index": 0 }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("97979797-9797-4797-8797-979797979797"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    DisableExternalModelTextureResolution(meta);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "NormalMipHero",
        "win64-dx12",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* diagnostic = FindDiagnosticByCode(
        result.diagnostics,
        "external-model-importer-texture-format-resolution");
#if NLS_HAS_DIRECTXTEX
    EXPECT_EQ(diagnostic, nullptr);
#else
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_EQ(diagnostic->severity, NLS::Core::Assets::AssetDiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostic->assetId, meta.id);
    EXPECT_EQ(diagnostic->path, sourcePath);
    EXPECT_NE(diagnostic->message.find("Texture texture:image/0 format resolution"), std::string::npos)
        << diagnostic->message;
    EXPECT_NE(diagnostic->message.find("requested bc5 resolved rgba8"), std::string::npos)
        << diagnostic->message;
    EXPECT_NE(diagnostic->message.find("preferred format unavailable"), std::string::npos)
        << diagnostic->message;
    EXPECT_NE(diagnostic->message.find("source=../Textures/HeroNormal.png"), std::string::npos)
        << diagnostic->message;
#endif

    const auto* normalArtifact = result.manifest.FindSubAsset("texture:image/0");
    ASSERT_NE(normalArtifact, nullptr);
    const auto normal = NLS::Render::Assets::DeserializeTextureArtifact(ReadArtifactFile(root, *normalArtifact));
    ASSERT_TRUE(normal.has_value());
#if NLS_HAS_DIRECTXTEX
    EXPECT_EQ(normal->format, NLS::Render::RHI::TextureFormat::BC5);
    EXPECT_EQ(normal->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Linear);
    ASSERT_GE(normal->mips.size(), 2u);
    EXPECT_EQ(normal->mips[1].rowPitch, NLS::Render::RHI::CalculateTextureRowPitch(normal->format, normal->mips[1].width));
    EXPECT_EQ(normal->mips[1].slicePitch, NLS::Render::RHI::CalculateTextureSlicePitch(normal->format, normal->mips[1].width, normal->mips[1].height, 1u));
    EXPECT_EQ(normal->encoderId, "directxtex-bc");
    EXPECT_NE(normal->buildIdentity.find("|encoderOptions=directxtex:parallel,linear,uniform"), std::string::npos);
    EXPECT_NE(normal->buildIdentity.find(std::string("|toolVersion=") + NLS::Editor::Assets::GetDirectXTexTextureEncoderToolVersion()), std::string::npos);
    EXPECT_EQ(std::string(NLS::Editor::Assets::GetDirectXTexTextureEncoderToolVersion()).find("unavailable"), std::string::npos);
#else
    EXPECT_EQ(normal->format, NLS::Render::RHI::TextureFormat::RGBA8);
    EXPECT_EQ(normal->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Linear);
    ASSERT_GE(normal->mips.size(), 2u);
    EXPECT_EQ(normal->encoderId, "rgba8-passthrough");
    EXPECT_NE(normal->buildIdentity.find("|encoder=rgba8-passthrough"), std::string::npos);
#endif

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportResolvesProjectRelativeTextureUrisFromAssetsRoot)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Model" / "main_sponza" / "Hero.gltf";
    WriteTextFile(root / "Assets" / "Model" / "main_sponza" / "mesh.bin", "mesh-binary");
    WriteBinaryFile(
        root / "Assets" / "Model" / "main_sponza" / "textures" / "HeroBaseColor.png",
        TinyPng());
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "mesh.bin", "byteLength": 11 }
            ],
            "images": [
                { "uri": "Model/main_sponza/textures/HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "HeroRoot", "mesh": 0 }],
            "meshes": [{ "primitives": [{ "attributes": {}, "material": 0 }] }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("95959595-9595-4595-8595-959595959595"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "Hero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Model/main_sponza"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    const auto* textureArtifact = FindAutoImportedTextureArtifact(result, "HeroBaseColor.png");
    ASSERT_NE(textureArtifact, nullptr);
    const auto texturePayload = ReadArtifactFile(root, *textureArtifact);
    EXPECT_TRUE(NLS::Render::Assets::IsNativeTextureArtifact(texturePayload));
    EXPECT_TRUE(NLS::Render::Assets::DeserializeTextureArtifact(texturePayload).has_value());

    const auto* materialArtifact = result.manifest.FindSubAsset("material:material/0");
    ASSERT_NE(materialArtifact, nullptr);
    const auto materialPayload = ReadArtifactPayloadText(
        root,
        *materialArtifact,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    const auto textureResourcePath = std::filesystem::path(textureArtifact->artifactPath)
        .lexically_relative(root)
        .generic_string();
    EXPECT_NE(materialPayload.find("Library/Artifacts/"), std::string::npos);
    EXPECT_NE(materialPayload.find(textureResourcePath), std::string::npos);
    EXPECT_FALSE(std::filesystem::path(textureArtifact->artifactPath).filename().has_extension());
    EXPECT_EQ(materialPayload.find("Model/main_sponza/textures/HeroBaseColor.png"), std::string::npos);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportDecodesPercentEncodedTextureUris)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Model" / "main_sponza" / "EncodedTextureHero.gltf";
    WriteTextFile(root / "Assets" / "Model" / "main_sponza" / "mesh.bin", "mesh-binary");
    WriteBinaryFile(
        root / "Assets" / "Model" / "main_sponza" / "textures" / "Hero BaseColor.png",
        TinyPng());
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "mesh.bin", "byteLength": 11 }
            ],
            "images": [
                { "uri": "Model/main_sponza/textures/Hero%20BaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "HeroRoot", "mesh": 0 }],
            "meshes": [{ "primitives": [{ "attributes": {}, "material": 0 }] }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("97979797-9797-4797-8797-979797979797"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "EncodedTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Model/main_sponza"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    const auto* textureArtifact = FindAutoImportedTextureArtifact(result, "Hero BaseColor.png");
    ASSERT_NE(textureArtifact, nullptr);
    const auto texturePayload = ReadArtifactFile(root, *textureArtifact);
    EXPECT_TRUE(NLS::Render::Assets::IsNativeTextureArtifact(texturePayload));
    EXPECT_TRUE(NLS::Render::Assets::DeserializeTextureArtifact(texturePayload).has_value());

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportResolvesBackslashTextureUrisOnPortableAssetPaths)
{
    const ScopedImportTestRoot tempRoot(MakeImportTestRoot());
    const auto& root = tempRoot.Path();
    const auto sourcePath = root / "Assets" / "Model" / "main_sponza" / "BackslashTextureHero.gltf";
    WriteTextFile(root / "Assets" / "Model" / "main_sponza" / "mesh.bin", "mesh-binary");
    WriteBinaryFile(
        root / "Assets" / "Model" / "main_sponza" / "textures" / "HeroBaseColor.png",
        TinyPng());
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "mesh.bin", "byteLength": 11 }
            ],
            "images": [
                { "uri": "textures\\HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "HeroRoot", "mesh": 0 }],
            "meshes": [{ "primitives": [{ "attributes": {}, "material": 0 }] }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("96969696-9696-4696-8696-969696969696"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "BackslashTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Model/main_sponza"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* textureArtifact = FindAutoImportedTextureArtifact(result, "HeroBaseColor.png");
    ASSERT_NE(textureArtifact, nullptr);
    const auto texturePayload = ReadArtifactFile(root, *textureArtifact);
    EXPECT_TRUE(NLS::Render::Assets::IsNativeTextureArtifact(texturePayload));
    EXPECT_TRUE(NLS::Render::Assets::DeserializeTextureArtifact(texturePayload).has_value());
}

TEST(AssetImportPipelineTests, ExternalModelImportResolvesProjectRelativeBackslashTextureUrisFromAssetsRoot)
{
    const ScopedImportTestRoot tempRoot(MakeImportTestRoot());
    const auto& root = tempRoot.Path();
    const auto sourcePath = root / "Assets" / "Model" / "main_sponza" / "BackslashProjectTextureHero.gltf";
    WriteTextFile(root / "Assets" / "Model" / "main_sponza" / "mesh.bin", "mesh-binary");
    WriteBinaryFile(
        root / "Assets" / "Model" / "main_sponza" / "textures" / "HeroBaseColor.png",
        TinyPng());
    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "mesh.bin", "byteLength": 11 }
            ],
            "images": [
                { "uri": "Model\\main_sponza\\textures\\HeroBaseColor.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "HeroMaterial",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "HeroRoot", "mesh": 0 }],
            "meshes": [{ "primitives": [{ "attributes": {}, "material": 0 }] }]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("93939393-9393-4393-8393-939393939393"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "BackslashProjectTextureHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Model/main_sponza"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* textureArtifact = FindAutoImportedTextureArtifact(result, "HeroBaseColor.png");
    ASSERT_NE(textureArtifact, nullptr);
    const auto texturePayload = ReadArtifactFile(root, *textureArtifact);
    EXPECT_TRUE(NLS::Render::Assets::IsNativeTextureArtifact(texturePayload));
    EXPECT_TRUE(NLS::Render::Assets::DeserializeTextureArtifact(texturePayload).has_value());

    const auto* materialArtifact = result.manifest.FindSubAsset("material:material/0");
    ASSERT_NE(materialArtifact, nullptr);
    const auto materialPayload = ReadArtifactPayloadText(
        root,
        *materialArtifact,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    const auto textureResourcePath = std::filesystem::path(textureArtifact->artifactPath)
        .lexically_relative(root)
        .generic_string();
    EXPECT_NE(materialPayload.find("Library/Artifacts/"), std::string::npos);
    EXPECT_NE(materialPayload.find(textureResourcePath), std::string::npos);
    EXPECT_FALSE(std::filesystem::path(textureArtifact->artifactPath).filename().has_extension());
    EXPECT_EQ(materialPayload.find("Model/main_sponza/textures/HeroBaseColor.png"), std::string::npos);
    EXPECT_EQ(materialPayload.find("Model\\main_sponza\\textures\\HeroBaseColor.png"), std::string::npos);
}

TEST(AssetImportPipelineTests, ExternalModelImportResolvesProjectRelativeBufferUrisFromAssetsRoot)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Model" / "main_sponza" / "HeroMesh.gltf";

    std::vector<uint8_t> meshBytes;
    const auto appendFloat = [&meshBytes](const float value)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        meshBytes.insert(meshBytes.end(), bytes, bytes + sizeof(float));
    };
    const auto appendU16 = [&meshBytes](const uint16_t value)
    {
        meshBytes.push_back(static_cast<uint8_t>(value & 0xFFu));
        meshBytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    };

    appendFloat(0.0f);
    appendFloat(0.0f);
    appendFloat(0.0f);
    appendFloat(1.0f);
    appendFloat(0.0f);
    appendFloat(0.0f);
    appendFloat(0.0f);
    appendFloat(1.0f);
    appendFloat(0.0f);
    appendU16(0u);
    appendU16(1u);
    appendU16(2u);
    WriteBinaryFile(root / "Assets" / "Model" / "main_sponza" / "mesh.bin", meshBytes);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "Model/main_sponza/mesh.bin", "byteLength": 42 }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "HeroRoot", "mesh": 0 }],
            "meshes": [
                { "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1 }] }
            ]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("96969696-9696-4696-8696-969696969696"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "HeroMesh",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Model/main_sponza"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    const auto* meshArtifact = result.manifest.FindSubAsset("mesh:mesh/0");
    ASSERT_NE(meshArtifact, nullptr);
    const auto mesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *meshArtifact));
    ASSERT_TRUE(mesh.has_value());
    ASSERT_EQ(mesh->vertices.size(), 3u);
    ASSERT_EQ(mesh->indices.size(), 3u);
    EXPECT_FLOAT_EQ(mesh->vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ(mesh->vertices[2].position[1], 1.0f);
    EXPECT_EQ(mesh->indices[2], 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportExportsGltfMultiPrimitiveMeshesAsPrimitiveArtifacts)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "TwoMeshFirstSplit.gltf";

    std::vector<uint8_t> meshBytes;
    AppendTriangleMeshBytes(meshBytes, 0.0f, 0u, 1u, 2u);
    AppendTriangleMeshBytes(meshBytes, 10.0f, 0u, 1u, 2u);
    AppendTriangleMeshBytes(meshBytes, 20.0f, 0u, 1u, 2u);
    WriteBinaryFile(root / "Assets" / "Models" / "mesh.bin", meshBytes);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "materials": [{ "name": "First" }, { "name": "Second" }],
            "buffers": [
                { "uri": "mesh.bin", "byteLength": 126 }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 42, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 78, "byteLength": 6, "target": 34963 },
                { "buffer": 0, "byteOffset": 84, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 120, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" },
                { "bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 5, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0, 1] }],
            "nodes": [
                { "name": "SplitMeshNode", "mesh": 0 },
                { "name": "SecondMeshNode", "mesh": 1 }
            ],
            "meshes": [
                {
                    "name": "SplitMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 },
                        { "attributes": { "POSITION": 2 }, "indices": 3, "material": 1 }
                    ]
                },
                {
                    "name": "SecondMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 4 }, "indices": 5, "material": 0 }
                    ]
                }
            ]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("97979797-9797-4797-8797-979797979797"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "TwoMeshFirstSplit",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    EXPECT_EQ(result.manifest.FindSubAsset("mesh:mesh/0"), nullptr);

    const auto* firstPrimitiveArtifact = result.manifest.FindSubAsset("mesh:mesh/0/primitive/0");
    ASSERT_NE(firstPrimitiveArtifact, nullptr);
    const auto firstPrimitive =
        NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *firstPrimitiveArtifact));
    ASSERT_TRUE(firstPrimitive.has_value());
    ASSERT_EQ(firstPrimitive->vertices.size(), 3u);
    EXPECT_EQ(firstPrimitive->materialIndex, 0u);
    EXPECT_FLOAT_EQ(firstPrimitive->vertices[0].position[0], 0.0f);

    const auto* secondPrimitiveArtifact = result.manifest.FindSubAsset("mesh:mesh/0/primitive/1");
    ASSERT_NE(secondPrimitiveArtifact, nullptr);
    const auto secondPrimitive =
        NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *secondPrimitiveArtifact));
    ASSERT_TRUE(secondPrimitive.has_value());
    ASSERT_EQ(secondPrimitive->vertices.size(), 3u);
    EXPECT_EQ(secondPrimitive->materialIndex, 1u);
    EXPECT_FLOAT_EQ(secondPrimitive->vertices[0].position[0], 10.0f);

    const auto* secondMeshArtifact = result.manifest.FindSubAsset("mesh:mesh/1");
    ASSERT_NE(secondMeshArtifact, nullptr);
    const auto secondMesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *secondMeshArtifact));
    ASSERT_TRUE(secondMesh.has_value());
    ASSERT_EQ(secondMesh->vertices.size(), 3u);
    EXPECT_EQ(secondMesh->materialIndex, 0u);
    EXPECT_FLOAT_EQ(secondMesh->vertices[0].position[0], 20.0f);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, PrimitiveMeshSourceKeyParserRejectsMalformedKeys)
{
    constexpr uint32_t primitiveIndex = 7u;
    const auto key = NLS::Render::Assets::BuildPrimitiveMeshSourceKey("mesh/Body", primitiveIndex);
    EXPECT_EQ(key, "mesh/Body/primitive/7");

    const auto parsed = NLS::Render::Assets::ParsePrimitiveMeshSourceKey(key);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first, "mesh/Body");
    EXPECT_EQ(parsed->second, primitiveIndex);

    EXPECT_FALSE(NLS::Render::Assets::ParsePrimitiveMeshSourceKey("mesh/Body").has_value());
    EXPECT_FALSE(NLS::Render::Assets::ParsePrimitiveMeshSourceKey("/primitive/1").has_value());
    EXPECT_FALSE(NLS::Render::Assets::ParsePrimitiveMeshSourceKey("mesh/Body/primitive/").has_value());
    EXPECT_FALSE(NLS::Render::Assets::ParsePrimitiveMeshSourceKey("mesh/Body/primitive/not-a-number").has_value());
    EXPECT_FALSE(NLS::Render::Assets::ParsePrimitiveMeshSourceKey(
        "mesh/Body/primitive/999999999999999999999999999999999999999").has_value());
}

TEST(AssetImportPipelineTests, ExternalModelImportDecodesPercentEncodedBufferUris)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Model" / "main_sponza" / "EncodedBufferHero.gltf";

    std::vector<uint8_t> meshBytes;
    const auto appendFloat = [&meshBytes](const float value)
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        meshBytes.insert(meshBytes.end(), bytes, bytes + sizeof(float));
    };
    const auto appendU16 = [&meshBytes](const uint16_t value)
    {
        meshBytes.push_back(static_cast<uint8_t>(value & 0xFFu));
        meshBytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    };

    appendFloat(0.0f);
    appendFloat(0.0f);
    appendFloat(0.0f);
    appendFloat(1.0f);
    appendFloat(0.0f);
    appendFloat(0.0f);
    appendFloat(0.0f);
    appendFloat(1.0f);
    appendFloat(0.0f);
    appendU16(0u);
    appendU16(1u);
    appendU16(2u);
    WriteBinaryFile(root / "Assets" / "Model" / "main_sponza" / "mesh data.bin", meshBytes);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "buffers": [
                { "uri": "Model/main_sponza/mesh%20data.bin", "byteLength": 42 }
            ],
            "bufferViews": [
                { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
                { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
            ],
            "accessors": [
                { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
                { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "nodes": [{ "name": "HeroRoot", "mesh": 0 }],
            "meshes": [
                { "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1 }] }
            ]
        })");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("98989898-9898-4898-8898-989898989898"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "EncodedBufferHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Model/main_sponza"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    const auto* meshArtifact = result.manifest.FindSubAsset("mesh:mesh/0");
    ASSERT_NE(meshArtifact, nullptr);
    const auto mesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *meshArtifact));
    ASSERT_TRUE(mesh.has_value());
    ASSERT_EQ(mesh->vertices.size(), 3u);
    EXPECT_FLOAT_EQ(mesh->vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ(mesh->vertices[2].position[1], 1.0f);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, AssimpParserClearsOutputsWhenLoadFails)
{
    const auto root = MakeImportTestRoot();
    NLS::Render::Resources::Parsers::AssimpParser parser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes(1u);
    std::vector<std::string> materials {"stale-material"};
    std::vector<std::string> externalDependencies {"stale-texture.png"};

    EXPECT_FALSE(parser.LoadModelData(
        (root / "Assets" / "Models" / "Missing.fbx").string(),
        meshes,
        materials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE,
        &externalDependencies));

    EXPECT_TRUE(meshes.empty());
    EXPECT_TRUE(materials.empty());
    EXPECT_TRUE(externalDependencies.empty());

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ModelImporterSettingsResolveMissingFbxReaderToAutodeskWithAssimpFallback)
{
    const std::map<std::string, std::string> settings;

    const auto parsed = NLS::Editor::Assets::ModelImporterSettingsFromSerialized(settings);

    EXPECT_EQ(
        parsed.fbxReaderSelection,
        NLS::Editor::Assets::FbxReaderSelection::AutodeskWithAssimpFallback);
}

TEST(AssetImportPipelineTests, ModelTextureResolutionSettingsUseUnityAlignedDefaults)
{
    NLS::Core::Assets::AssetMeta meta;

    const auto settings = NLS::Editor::Assets::LoadModelTextureResolutionSettings(meta);

    EXPECT_EQ(settings.settingsVersion, 1u);
    EXPECT_TRUE(settings.useExternalTextures);
    EXPECT_TRUE(settings.searchByName);
    EXPECT_TRUE(settings.autoImportMissingTextureFiles);
    EXPECT_EQ(
        settings.embeddedTextureMode,
        NLS::Editor::Assets::ModelEmbeddedTextureMode::ModelSubAsset);
}

TEST(AssetImportPipelineTests, ModelTextureResolutionSettingsRejectMalformedEncodedValues)
{
    NLS::Core::Assets::AssetMeta meta;
    meta.settings["MODEL_TEXTURE_USE_EXTERNAL_TEXTURES"] = "false";
    meta.settings["MODEL_TEXTURE_SEARCH_BY_NAME"] = "0";
    meta.settings["MODEL_TEXTURE_AUTO_IMPORT_MISSING"] = "not-a-bool";
    meta.settings["MODEL_TEXTURE_EMBEDDED_MODE"] = "Extract";
    meta.settings["MODEL_TEXTURE_REMAP.mtxsrc%ZZ"] = "bad";

    const auto settings = NLS::Editor::Assets::LoadModelTextureResolutionSettings(meta);
    const auto remaps = NLS::Editor::Assets::LoadModelTextureRemapSettings(meta);

    EXPECT_FALSE(settings.useExternalTextures);
    EXPECT_FALSE(settings.searchByName);
    EXPECT_TRUE(settings.autoImportMissingTextureFiles);
    EXPECT_EQ(
        settings.embeddedTextureMode,
        NLS::Editor::Assets::ModelEmbeddedTextureMode::ModelSubAsset);
    EXPECT_TRUE(remaps.empty());
}

TEST(AssetImportPipelineTests, ModelTextureRemapSettingsRoundTripStableSourceKeys)
{
    NLS::Core::Assets::AssetMeta meta;
    const std::string stableKey = "mtxsrc:v1:kind=ExternalFile;uri=Assets/Textures/Wood.png";
    const auto textureId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("10101010-1010-4010-8010-101010101010"));

    NLS::Editor::Assets::StoreModelTextureRemapSetting(
        meta,
        { stableKey, textureId, "texture:main", "Assets/Textures/Wood.png" });

    auto remaps = NLS::Editor::Assets::LoadModelTextureRemapSettings(meta);
    ASSERT_EQ(remaps.size(), 1u);
    EXPECT_EQ(remaps[0].sourceStableKey, stableKey);
    EXPECT_EQ(remaps[0].targetAssetId, textureId);
    EXPECT_EQ(remaps[0].targetSubAssetKey, "texture:main");
    EXPECT_EQ(remaps[0].targetEditorPath, "Assets/Textures/Wood.png");

    NLS::Editor::Assets::ClearModelTextureRemapSetting(meta, stableKey);
    remaps = NLS::Editor::Assets::LoadModelTextureRemapSettings(meta);
    EXPECT_TRUE(remaps.empty());
}

TEST(AssetImportPipelineTests, ModelTextureRemapSettingsRoundTripEscapedStableKeysAndRejectMalformedValues)
{
    NLS::Core::Assets::AssetMeta meta;
    const std::string stableKey =
        "mtxsrc:v1:kind=ExternalFile;uri=Assets/Textures/Wood;Oak|A.png;source=line\nsnowman-\xE2\x98\x83";
    const auto textureId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("20202020-2020-4020-8020-202020202020"));

    NLS::Editor::Assets::StoreModelTextureRemapSetting(
        meta,
        { stableKey, textureId, "texture:main;slot|0", "Assets/Textures/Wood;Oak|A.png" });
    meta.settings["MODEL_TEXTURE_REMAP.mtxsrc%ZZ"] = "not-a-guid#texture";

    NLS::Core::Assets::AssetDiagnostics diagnostics;
    const auto remaps = NLS::Editor::Assets::LoadModelTextureRemapSettings(meta, &diagnostics);

    ASSERT_EQ(remaps.size(), 1u);
    EXPECT_EQ(remaps[0].sourceStableKey, stableKey);
    EXPECT_EQ(remaps[0].targetAssetId, textureId);
    EXPECT_EQ(remaps[0].targetSubAssetKey, "texture:main;slot|0");
    EXPECT_EQ(remaps[0].targetEditorPath, "Assets/Textures/Wood;Oak|A.png");
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics[0].severity, NLS::Core::Assets::AssetDiagnosticSeverity::Warning);
    EXPECT_EQ(diagnostics[0].code, "model-texture-remap-malformed");
}

TEST(AssetImportPipelineTests, ModelTextureRemapSettingsClearReturnsToAutomaticResolution)
{
    NLS::Core::Assets::AssetMeta meta;
    const std::string stableKey =
        "mtxsrc:v1:kind=ExternalFile;uri=Assets/Textures/Wood.png";
    const auto textureId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("30303030-3030-4030-8030-303030303030"));

    NLS::Editor::Assets::StoreModelTextureRemapSetting(
        meta,
        { stableKey, textureId, "texture:main", "Assets/Textures/Wood.png" });
    EXPECT_FALSE(NLS::Editor::Assets::LoadModelTextureRemapSettings(meta).empty());

    NLS::Editor::Assets::ClearModelTextureRemapSetting(meta, stableKey);
    EXPECT_TRUE(NLS::Editor::Assets::LoadModelTextureRemapSettings(meta).empty());
}

TEST(AssetImportPipelineTests, ModelTextureRemapSettingsEscapesStableKeysAndRejectsMalformedValues)
{
    NLS::Core::Assets::AssetMeta meta;
    const std::string stableKey = "mtxsrc:v1:kind=ExternalFile;uri=Assets/Textures/Wood;Oak|A.png;source=line\nsnowman-\xE2\x98\x83";
    const auto textureId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("20202020-2020-4020-8020-202020202020"));

    NLS::Editor::Assets::StoreModelTextureRemapSetting(
        meta,
        { stableKey, textureId, "texture:main;slot|0", {} });
    meta.settings["MODEL_TEXTURE_REMAP.mtxsrc%3Av1%3Akind%3DExternalFile%3Buri%3Dbad"] =
        "not-a-guid#texture";

    NLS::Core::Assets::AssetDiagnostics diagnostics;
    const auto remaps = NLS::Editor::Assets::LoadModelTextureRemapSettings(meta, &diagnostics);

    ASSERT_EQ(remaps.size(), 1u);
    EXPECT_EQ(remaps[0].sourceStableKey, stableKey);
    EXPECT_EQ(remaps[0].targetAssetId, textureId);
    EXPECT_EQ(remaps[0].targetSubAssetKey, "texture:main;slot|0");
    EXPECT_EQ(remaps[0].targetEditorPath, "");
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics[0].code, "model-texture-remap-malformed");
}

TEST(AssetImportPipelineTests, ExternalModelImportExplicitTextureRemapOverridesSourcePath)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "ExplicitRemapHero.gltf";
    const auto sourceTexturePath = root / "Assets" / "Textures" / "SharedAlbedo.png";
    const auto remapTexturePath = root / "Assets" / "Textures" / "OverrideAlbedo.png";
    WriteBinaryFile(sourceTexturePath, TinyPng());
    WriteBinaryFile(remapTexturePath, TinyPng());

    const auto sourceTextureId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("40404040-4040-4040-8040-404040404040"));
    NLS::Core::Assets::AssetMeta sourceTextureMeta;
    sourceTextureMeta.id = sourceTextureId;
    sourceTextureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    sourceTextureMeta.importerId = "texture";
    sourceTextureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(sourceTextureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(sourceTexturePath)));

    const auto remapTextureId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("41414141-4141-4141-8141-414141414141"));
    NLS::Core::Assets::AssetMeta remapTextureMeta;
    remapTextureMeta.id = remapTextureId;
    remapTextureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    remapTextureMeta.importerId = "texture";
    remapTextureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(remapTextureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(remapTexturePath)));

    auto remapTextureArtifact = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        TinyPng().data(),
        TinyPng().size(),
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        false);
    ASSERT_TRUE(remapTextureArtifact.has_value());
    const auto textureTargetPlatform = TextureArtifactTargetPlatformForTest();
    remapTextureArtifact->targetPlatform = textureTargetPlatform;
    remapTextureArtifact->encoderId = "rgba8-passthrough";
    remapTextureArtifact->encoderVersion = 1u;
    remapTextureArtifact->buildIdentity = "unit-test-remap-texture";
    const auto remapTextureArtifactRoot = root / "Library" / "Artifacts";
    const auto remapTextureArtifactRelativePath = WriteTextureArtifactBlobForTest(
        root,
        remapTextureMeta,
        "texture:main",
        "OverrideAlbedo",
        textureTargetPlatform,
        *remapTextureArtifact);
    ASSERT_FALSE(remapTextureArtifactRelativePath.empty());
    const auto remapTextureArtifactPath = root / remapTextureArtifactRelativePath;
    NLS::Core::Assets::ArtifactManifest remapTextureManifest;
    remapTextureManifest.sourceAssetId = remapTextureId;
    remapTextureManifest.importerId = remapTextureMeta.importerId;
    remapTextureManifest.importerVersion = remapTextureMeta.importerVersion;
    remapTextureManifest.targetPlatform = textureTargetPlatform;
    remapTextureManifest.primarySubAssetKey = "texture:main";
    remapTextureManifest.subAssets.push_back({
        remapTextureId,
        "texture:main",
        NLS::Core::Assets::ArtifactType::Texture,
        "texture",
        textureTargetPlatform,
        remapTextureArtifactRelativePath.generic_string(),
        "sha256:" + remapTextureArtifactPath.filename().generic_string(),
        "OverrideAlbedo"
    });
    WriteArtifactManifestFile(remapTextureArtifactRoot, remapTextureManifest);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/SharedAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("42424242-4242-4242-8242-424242424242"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    const auto stableKey = "mtxsrc:v1:kind=ExternalFile;source=image/0;uri=Assets/Textures/SharedAlbedo.png";
    NLS::Editor::Assets::StoreModelTextureRemapSetting(
        modelMeta,
        { stableKey, remapTextureId, "texture:main", "Assets/Textures/OverrideAlbedo.png" });

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "ExplicitRemapHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    EXPECT_NE(payload.find(remapTextureArtifactPath.lexically_relative(root).generic_string()), std::string::npos);
    EXPECT_EQ(payload.find(sourceTexturePath.lexically_relative(root).generic_string()), std::string::npos);
    EXPECT_EQ(
        std::count_if(
            result.manifest.subAssets.begin(),
            result.manifest.subAssets.end(),
            [](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
            }),
        0);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportExplicitTextureRemapSurvivesTargetMoveByGuid)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "MovedRemapHero.gltf";
    const auto sourceTexturePath = root / "Assets" / "Textures" / "SharedAlbedo.png";
    const auto oldRemapTexturePath = root / "Assets" / "Textures" / "MovedOverride.png";
    const auto movedRemapTexturePath = root / "Assets" / "Moved" / "MovedOverride.png";
    WriteBinaryFile(sourceTexturePath, TinyPng());
    WriteBinaryFile(movedRemapTexturePath, TinyPng());

    const auto remapTextureId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("46464646-4646-4646-8646-464646464646"));
    NLS::Core::Assets::AssetMeta remapTextureMeta;
    remapTextureMeta.id = remapTextureId;
    remapTextureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    remapTextureMeta.importerId = "texture";
    remapTextureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(remapTextureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(movedRemapTexturePath)));

    auto remapTextureArtifact = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        TinyPng().data(),
        TinyPng().size(),
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        false);
    ASSERT_TRUE(remapTextureArtifact.has_value());
    const auto textureTargetPlatform = TextureArtifactTargetPlatformForTest();
    remapTextureArtifact->targetPlatform = textureTargetPlatform;
    remapTextureArtifact->encoderId = "rgba8-passthrough";
    remapTextureArtifact->encoderVersion = 1u;
    remapTextureArtifact->buildIdentity = "unit-test-moved-remap-texture";
    const auto remapTextureArtifactRoot = root / "Library" / "Artifacts";
    const auto remapTextureArtifactRelativePath = WriteTextureArtifactBlobForTest(
        root,
        remapTextureMeta,
        "texture:main",
        "MovedOverride",
        textureTargetPlatform,
        *remapTextureArtifact);
    ASSERT_FALSE(remapTextureArtifactRelativePath.empty());
    const auto remapTextureArtifactPath = root / remapTextureArtifactRelativePath;

    NLS::Core::Assets::ArtifactManifest remapTextureManifest;
    remapTextureManifest.sourceAssetId = remapTextureId;
    remapTextureManifest.importerId = remapTextureMeta.importerId;
    remapTextureManifest.importerVersion = remapTextureMeta.importerVersion;
    remapTextureManifest.targetPlatform = textureTargetPlatform;
    remapTextureManifest.primarySubAssetKey = "texture:main";
    remapTextureManifest.subAssets.push_back({
        remapTextureId,
        "texture:main",
        NLS::Core::Assets::ArtifactType::Texture,
        "texture",
        textureTargetPlatform,
        remapTextureArtifactRelativePath.generic_string(),
        "sha256:" + remapTextureArtifactPath.filename().generic_string(),
        "MovedOverride"
    });
    WriteArtifactManifestFile(remapTextureArtifactRoot, remapTextureManifest);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/SharedAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("47474747-4747-4747-8747-474747474747"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    NLS::Editor::Assets::StoreModelTextureRemapSetting(
        modelMeta,
        {
            "mtxsrc:v1:kind=ExternalFile;source=image/0;uri=Assets/Textures/SharedAlbedo.png",
            remapTextureId,
            "texture:main",
            oldRemapTexturePath.lexically_relative(root).generic_string()
        });

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "MovedRemapHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    EXPECT_NE(payload.find(remapTextureArtifactPath.lexically_relative(root).generic_string()), std::string::npos);

    const auto reportText = ReadTextFile(
        NLS::Editor::Assets::ModelTextureResolutionReportPath(root / "Library" / "Artifacts" / modelMeta.id.ToString()));
    const auto report = NLS::Editor::Assets::ParseModelTextureResolutionReport(reportText);
    ASSERT_TRUE(report.has_value());
    ASSERT_EQ(report->entries.size(), 1u);
    EXPECT_EQ(report->entries[0].kind, NLS::Editor::Assets::ModelTextureResolutionKind::ExplicitRemap);
    EXPECT_EQ(report->entries[0].targetEditorPath.generic_string(), movedRemapTexturePath.lexically_relative(root).generic_string());

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportInvalidTextureRemapWarnsAndFallsBack)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "InvalidRemapHero.gltf";
    const auto texturePath = root / "Assets" / "Textures" / "SharedAlbedo.png";
    WriteBinaryFile(texturePath, TinyPng());

    const auto textureAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("43434343-4343-4343-8343-434343434343"));
    NLS::Core::Assets::AssetMeta textureMeta;
    textureMeta.id = textureAssetId;
    textureMeta.assetType = NLS::Core::Assets::AssetType::Texture;
    textureMeta.importerId = "texture";
    textureMeta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(
        NLS::Core::Assets::AssetType::Texture);
    ASSERT_TRUE(textureMeta.Save(NLS::Core::Assets::GetAssetMetaPath(texturePath)));

    auto decodedTexture = NLS::Render::Assets::DecodeTextureArtifactFromEncodedImage(
        TinyPng().data(),
        TinyPng().size(),
        NLS::Render::Assets::TextureArtifactColorSpace::Srgb,
        false);
    ASSERT_TRUE(decodedTexture.has_value());
    const auto textureTargetPlatform = TextureArtifactTargetPlatformForTest();
    decodedTexture->targetPlatform = textureTargetPlatform;
    decodedTexture->encoderId = "rgba8-passthrough";
    decodedTexture->encoderVersion = 1u;
    decodedTexture->buildIdentity = "unit-test-remap-fallback";
    const auto textureArtifactRoot = root / "Library" / "Artifacts";
    const auto textureArtifactRelativePath = WriteTextureArtifactBlobForTest(
        root,
        textureMeta,
        "texture:main",
        "SharedAlbedo",
        textureTargetPlatform,
        *decodedTexture);
    ASSERT_FALSE(textureArtifactRelativePath.empty());
    const auto textureArtifactPath = root / textureArtifactRelativePath;
    NLS::Core::Assets::ArtifactManifest textureManifest;
    textureManifest.sourceAssetId = textureAssetId;
    textureManifest.importerId = textureMeta.importerId;
    textureManifest.importerVersion = textureMeta.importerVersion;
    textureManifest.targetPlatform = textureTargetPlatform;
    textureManifest.primarySubAssetKey = "texture:main";
    textureManifest.subAssets.push_back({
        textureAssetId,
        "texture:main",
        NLS::Core::Assets::ArtifactType::Texture,
        "texture",
        textureTargetPlatform,
        textureArtifactRelativePath.generic_string(),
        "sha256:" + textureArtifactPath.filename().generic_string(),
        "SharedAlbedo"
    });
    WriteArtifactManifestFile(textureArtifactRoot, textureManifest);

    WriteTextFile(
        sourcePath,
        R"({
            "asset": { "version": "2.0" },
            "images": [
                { "uri": "../Textures/SharedAlbedo.png", "mimeType": "image/png" }
            ],
            "textures": [
                { "source": 0 }
            ],
            "materials": [
                {
                    "name": "Body",
                    "pbrMetallicRoughness": {
                        "baseColorTexture": { "index": 0 }
                    }
                }
            ],
            "scene": 0,
            "scenes": [{ "nodes": [0] }],
            "meshes": [{ "name": "BodyMesh", "primitives": [{ "attributes": {}, "material": 0 }] }],
            "nodes": [{ "name": "Root", "mesh": 0 }]
        })");

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("44444444-4444-4444-8444-444444444444"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    NLS::Editor::Assets::StoreModelTextureRemapSetting(
        modelMeta,
        {
            "mtxsrc:v1:kind=ExternalFile;source=image/0;uri=Assets/Textures/SharedAlbedo.png",
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("45454545-4545-4545-8545-454545454545")),
            "texture:main",
            "Assets/Textures/SharedAlbedo.png"
        });

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "InvalidRemapHero",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_TRUE(ContainsDiagnostic(
        result.diagnostics,
        "model-texture-remap-invalid-target",
        NLS::Core::Assets::AssetDiagnosticSeverity::Warning))
        << JoinDiagnosticSummaries(result.diagnostics);

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    EXPECT_NE(payload.find(textureArtifactPath.lexically_relative(root).generic_string()), std::string::npos);
    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ModelTextureResolutionReportRoundTripsEntriesAndDiagnostics)
{
    NLS::Editor::Assets::ModelTextureResolutionReport report;
    report.modelAssetId = "30303030-3030-4030-8030-303030303030";
    report.targetPlatform = "editor-windows";
    report.importerVersion = 9u;
    report.settingsFingerprint = "settings;hash|v1";

    NLS::Editor::Assets::ResolvedModelTextureReference sourcePath;
    sourcePath.source.sourceKey = "image/0";
    sourcePath.source.materialTextureKey = "image/0";
    sourcePath.source.stableKey = "mtxsrc:v1:kind=ExternalFile;uri=Textures/Wood.png";
    sourcePath.source.displayName = "Wood=Oak";
    sourcePath.source.uri = "Textures/Wood.png";
    sourcePath.source.normalizedUri = "Textures/Wood.png";
    sourcePath.source.kind = NLS::Editor::Assets::TextureSourceKind::ExternalFile;
    sourcePath.source.stableKeyStatus = NLS::Editor::Assets::ModelTextureStableKeyStatus::Stable;
    sourcePath.kind = NLS::Editor::Assets::ModelTextureResolutionKind::SourcePath;
    sourcePath.materialTextureKey = "image/0";
    sourcePath.targetAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("40404040-4040-4040-8040-404040404040"));
    sourcePath.targetSubAssetKey = "texture:main";
    sourcePath.resourcePath = "Library/Artifacts/texture.ntex";
    sourcePath.diagnostics.push_back({
        "Warning",
        "model-texture-name-ambiguous",
        "Wood;Oak matched multiple candidates"
    });

    NLS::Editor::Assets::ResolvedModelTextureReference fallback;
    fallback.source.sourceKey = "bufferView/1";
    fallback.source.materialTextureKey = "bufferView/1";
    fallback.source.stableKey = "mtxsrc:v1:kind=BufferView;bufferView=bufferView/1";
    fallback.source.kind = NLS::Editor::Assets::TextureSourceKind::BufferView;
    fallback.kind = NLS::Editor::Assets::ModelTextureResolutionKind::ModelEmbeddedFallback;
    fallback.materialTextureKey = "bufferView/1";
    fallback.modelSubAssetKey = "texture:bufferView/1";

    report.entries = { sourcePath, fallback };

    const auto parsed = NLS::Editor::Assets::ParseModelTextureResolutionReport(
        NLS::Editor::Assets::SerializeModelTextureResolutionReport(report));

    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->modelAssetId, report.modelAssetId);
    EXPECT_EQ(parsed->targetPlatform, report.targetPlatform);
    EXPECT_EQ(parsed->importerVersion, report.importerVersion);
    EXPECT_EQ(parsed->settingsFingerprint, report.settingsFingerprint);
    ASSERT_EQ(parsed->entries.size(), 2u);
    EXPECT_EQ(parsed->entries[0].source.stableKey, sourcePath.source.stableKey);
    EXPECT_EQ(parsed->entries[0].source.sourceKey, "image/0");
    EXPECT_EQ(parsed->entries[0].source.materialTextureKey, "image/0");
    EXPECT_EQ(parsed->entries[0].source.displayName, "Wood=Oak");
    EXPECT_EQ(parsed->entries[0].source.kind, NLS::Editor::Assets::TextureSourceKind::ExternalFile);
    EXPECT_EQ(parsed->entries[0].kind, NLS::Editor::Assets::ModelTextureResolutionKind::SourcePath);
    EXPECT_EQ(parsed->entries[0].targetAssetId, sourcePath.targetAssetId);
    EXPECT_EQ(parsed->entries[0].resourcePath.generic_string(), "Library/Artifacts/texture.ntex");
    ASSERT_EQ(parsed->entries[0].diagnostics.size(), 1u);
    EXPECT_EQ(parsed->entries[0].diagnostics[0].code, "model-texture-name-ambiguous");
    EXPECT_EQ(parsed->entries[1].kind, NLS::Editor::Assets::ModelTextureResolutionKind::ModelEmbeddedFallback);
    EXPECT_EQ(parsed->entries[1].modelSubAssetKey, "texture:bufferView/1");
}

TEST(AssetImportPipelineTests, ModelTextureResolutionReportRejectsMalformedPayload)
{
    EXPECT_FALSE(NLS::Editor::Assets::ParseModelTextureResolutionReport("not a report").has_value());
    EXPECT_FALSE(NLS::Editor::Assets::ParseModelTextureResolutionReport(
        "NULLUS_MODEL_TEXTURE_RESOLUTION_REPORT=1\nreportVersion=abc\n").has_value());
}

TEST(AssetImportPipelineTests, ModelTextureResolutionReportRejectsStaleContext)
{
    NLS::Editor::Assets::ModelTextureResolutionReport report;
    report.modelAssetId = "50505050-5050-4050-8050-505050505050";
    report.targetPlatform = "editor-windows";
    report.importerVersion = 9u;
    report.settingsFingerprint = "settings:v1";

    EXPECT_TRUE(NLS::Editor::Assets::IsModelTextureResolutionReportCurrent(
        report,
        { report.modelAssetId, report.targetPlatform, report.importerVersion, report.settingsFingerprint }));
    EXPECT_FALSE(NLS::Editor::Assets::IsModelTextureResolutionReportCurrent(
        report,
        { "60606060-6060-4060-8060-606060606060", report.targetPlatform, report.importerVersion, report.settingsFingerprint }));
    EXPECT_FALSE(NLS::Editor::Assets::IsModelTextureResolutionReportCurrent(
        report,
        { report.modelAssetId, "win64-dx12", report.importerVersion, report.settingsFingerprint }));
    EXPECT_FALSE(NLS::Editor::Assets::IsModelTextureResolutionReportCurrent(
        report,
        { report.modelAssetId, report.targetPlatform, report.importerVersion + 1u, report.settingsFingerprint }));
    EXPECT_FALSE(NLS::Editor::Assets::IsModelTextureResolutionReportCurrent(
        report,
        { report.modelAssetId, report.targetPlatform, report.importerVersion, "settings:v2" }));
}

TEST(AssetImportPipelineTests, ModelTextureResolutionReportEscapesSpecialCharacters)
{
    NLS::Editor::Assets::ModelTextureResolutionReport report;
    report.modelAssetId = "70707070-7070-4070-8070-707070707070";
    report.targetPlatform = "editor;windows|debug";
    report.importerVersion = 9u;
    report.settingsFingerprint = "hash=with;reserved|chars";

    NLS::Editor::Assets::ResolvedModelTextureReference entry;
    entry.source.sourceKey = "image;0|slot";
    entry.source.materialTextureKey = "image;0|slot";
    entry.source.stableKey = "mtxsrc:v1:kind=ExternalFile;uri=Textures/A;B|C.png";
    entry.source.uri = "Textures/A;B|C.png";
    entry.source.kind = NLS::Editor::Assets::TextureSourceKind::ExternalFile;
    entry.kind = NLS::Editor::Assets::ModelTextureResolutionKind::Missing;
    entry.diagnostics.push_back({"Warning", "model-texture-source-path-missing", "missing=A;B|C\nline2"});
    report.entries.push_back(entry);

    const auto serialized = NLS::Editor::Assets::SerializeModelTextureResolutionReport(report);
    EXPECT_NE(serialized.find("%3B"), std::string::npos);
    EXPECT_NE(serialized.find("%7C"), std::string::npos);

    const auto parsed = NLS::Editor::Assets::ParseModelTextureResolutionReport(serialized);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->entries.size(), 1u);
    EXPECT_EQ(parsed->targetPlatform, report.targetPlatform);
    EXPECT_EQ(parsed->entries[0].source.sourceKey, entry.source.sourceKey);
    EXPECT_EQ(parsed->entries[0].source.stableKey, entry.source.stableKey);
    ASSERT_EQ(parsed->entries[0].diagnostics.size(), 1u);
    EXPECT_EQ(parsed->entries[0].diagnostics[0].message, "missing=A;B|C\nline2");
}

TEST(AssetImportPipelineTests, AssetPropertiesModelTextureReportRowsHideMissingStaleAndMalformedReports)
{
    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("51515151-5151-4151-8151-515151515151"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    const auto currentFingerprint = NLS::Editor::Assets::ComputeModelTextureSettingsFingerprint(modelMeta);

    const auto missing = NLS::Editor::Panels::BuildModelTextureAssetPropertiesView(
        modelMeta,
        "editor",
        std::nullopt);
    EXPECT_FALSE(missing.hasCurrentReport);
    EXPECT_FALSE(missing.reportMalformed);
    EXPECT_FALSE(missing.reimportRequired);
    EXPECT_TRUE(missing.rows.empty());

    const auto malformed = NLS::Editor::Panels::BuildModelTextureAssetPropertiesView(
        modelMeta,
        "editor",
        std::optional<std::string>("not a texture report"));
    EXPECT_FALSE(malformed.hasCurrentReport);
    EXPECT_TRUE(malformed.reportMalformed);
    EXPECT_FALSE(malformed.reimportRequired);
    EXPECT_TRUE(malformed.rows.empty());

    NLS::Editor::Assets::ModelTextureResolutionReport report;
    report.modelAssetId = modelMeta.id.ToString();
    report.targetPlatform = "editor";
    report.importerVersion = modelMeta.importerVersion;
    report.settingsFingerprint = currentFingerprint;

    auto staleReport = report;
    staleReport.settingsFingerprint = "stale-settings";
    const auto stale = NLS::Editor::Panels::BuildModelTextureAssetPropertiesView(
        modelMeta,
        "editor",
        NLS::Editor::Assets::SerializeModelTextureResolutionReport(staleReport));
    EXPECT_FALSE(stale.hasCurrentReport);
    EXPECT_FALSE(stale.reportMalformed);
    EXPECT_TRUE(stale.reportStale);
    EXPECT_TRUE(stale.reimportRequired);
    EXPECT_TRUE(stale.rows.empty());
}

TEST(AssetImportPipelineTests, AssetPropertiesModelTextureReportRowsExposeWarningsAndResolutionDetails)
{
    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("52525252-5252-4252-8252-525252525252"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    NLS::Editor::Assets::ModelTextureResolutionReport report;
    report.modelAssetId = modelMeta.id.ToString();
    report.targetPlatform = "editor";
    report.importerVersion = modelMeta.importerVersion;
    report.settingsFingerprint = NLS::Editor::Assets::ComputeModelTextureSettingsFingerprint(modelMeta);

    NLS::Editor::Assets::ResolvedModelTextureReference invalidTarget;
    invalidTarget.source.stableKey = "mtxsrc:v1:kind=ExternalFile;source=image/0;uri=Assets/Textures/A.png";
    invalidTarget.source.sourceKey = "image/0";
    invalidTarget.source.materialTextureKey = "image/0";
    invalidTarget.source.displayName = "A.png";
    invalidTarget.source.uri = "Assets/Textures/A.png";
    invalidTarget.source.normalizedUri = "Assets/Textures/A.png";
    invalidTarget.source.kind = NLS::Editor::Assets::TextureSourceKind::ExternalFile;
    invalidTarget.source.stableKeyStatus = NLS::Editor::Assets::ModelTextureStableKeyStatus::Stable;
    invalidTarget.kind = NLS::Editor::Assets::ModelTextureResolutionKind::Missing;
    invalidTarget.materialTextureKey = "image/0";
    invalidTarget.diagnostics.push_back({
        "Warning",
        "model-texture-remap-invalid-target",
        "Texture remap target is missing or invalid."
    });

    NLS::Editor::Assets::ResolvedModelTextureReference orderDerived;
    orderDerived.source.stableKey = "mtxsrc:v1:kind=Missing;name=Texture;dup=0";
    orderDerived.source.displayName = "Texture";
    orderDerived.source.kind = NLS::Editor::Assets::TextureSourceKind::Missing;
    orderDerived.source.stableKeyStatus = NLS::Editor::Assets::ModelTextureStableKeyStatus::OrderDerived;
    orderDerived.kind = NLS::Editor::Assets::ModelTextureResolutionKind::ModelEmbeddedFallback;
    orderDerived.modelSubAssetKey = "texture:image/1";

    NLS::Editor::Assets::ResolvedModelTextureReference unsupportedEncoding;
    unsupportedEncoding.source.stableKey = "mtxsrc:v1:kind=EmbeddedData;source=image/2";
    unsupportedEncoding.source.sourceKey = "image/2";
    unsupportedEncoding.source.kind = NLS::Editor::Assets::TextureSourceKind::EmbeddedData;
    unsupportedEncoding.kind = NLS::Editor::Assets::ModelTextureResolutionKind::ModelEmbeddedFallback;
    unsupportedEncoding.modelSubAssetKey = "texture:image/2";
    unsupportedEncoding.diagnostics.push_back({
        "Warning",
        "model-texture-unsupported-encoding",
        "Texture encoding is unsupported."
    });

    NLS::Editor::Assets::ResolvedModelTextureReference sourcePath;
    sourcePath.source.stableKey = "mtxsrc:v1:kind=ExternalFile;source=image/3;uri=Assets/Textures/B.png";
    sourcePath.source.sourceKey = "image/3";
    sourcePath.source.materialTextureKey = "image/3";
    sourcePath.source.displayName = "B.png";
    sourcePath.source.kind = NLS::Editor::Assets::TextureSourceKind::ExternalFile;
    sourcePath.kind = NLS::Editor::Assets::ModelTextureResolutionKind::SourcePath;
    sourcePath.materialTextureKey = "image/3";
    sourcePath.targetAssetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("53535353-5353-4353-8353-535353535353"));
    sourcePath.targetSubAssetKey = "texture:main";
    sourcePath.resourcePath = "Library/Artifacts/B/texture.ntex";
    sourcePath.targetEditorPath = "Assets/Textures/B.png";

    report.entries = { invalidTarget, orderDerived, unsupportedEncoding, sourcePath };

    const auto view = NLS::Editor::Panels::BuildModelTextureAssetPropertiesView(
        modelMeta,
        "editor",
        NLS::Editor::Assets::SerializeModelTextureResolutionReport(report));

    ASSERT_TRUE(view.hasCurrentReport);
    ASSERT_EQ(view.rows.size(), 4u);
    EXPECT_EQ(view.rows[0].resolutionKind, "Missing");
    EXPECT_TRUE(view.rows[0].hasWarnings);
    EXPECT_TRUE(view.rows[0].hasInvalidTargetWarning);
    EXPECT_EQ(view.rows[0].diagnosticCodes[0], "model-texture-remap-invalid-target");
    EXPECT_TRUE(view.rows[1].usesOrderDerivedStableKey);
    EXPECT_TRUE(view.rows[1].hasWarnings);
    EXPECT_TRUE(view.rows[2].hasUnsupportedEncodingWarning);
    EXPECT_EQ(view.rows[3].resolutionKind, "SourcePath");
    EXPECT_EQ(view.rows[3].targetEditorPath, "Assets/Textures/B.png");
    EXPECT_EQ(view.rows[3].targetAssetId, "53535353-5353-4353-8353-535353535353");
}

TEST(AssetImportPipelineTests, AssetPropertiesModelTextureSettingAndRemapChangesRequireReimport)
{
    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("54545454-5454-4454-8454-545454545454"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();

    NLS::Editor::Assets::ModelTextureResolutionReport report;
    report.modelAssetId = modelMeta.id.ToString();
    report.targetPlatform = "editor";
    report.importerVersion = modelMeta.importerVersion;
    report.settingsFingerprint = NLS::Editor::Assets::ComputeModelTextureSettingsFingerprint(modelMeta);

    const auto clean = NLS::Editor::Panels::BuildModelTextureAssetPropertiesView(
        modelMeta,
        "editor",
        NLS::Editor::Assets::SerializeModelTextureResolutionReport(report));
    EXPECT_FALSE(clean.reimportRequired);

    auto settings = clean.settings;
    settings.searchByName = false;
    NLS::Editor::Panels::StoreModelTextureAssetPropertiesSettings(modelMeta, settings);
    EXPECT_FALSE(NLS::Editor::Assets::LoadModelTextureResolutionSettings(modelMeta).searchByName);

    const auto changedSettings = NLS::Editor::Panels::BuildModelTextureAssetPropertiesView(
        modelMeta,
        "editor",
        NLS::Editor::Assets::SerializeModelTextureResolutionReport(report));
    EXPECT_TRUE(changedSettings.reimportRequired);

    const std::string stableKey = "mtxsrc:v1:kind=ExternalFile;source=image/0;uri=Assets/Textures/A.png";
    const auto remapTargetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("55555555-5555-4555-8555-555555555555"));
    NLS::Editor::Panels::StoreModelTextureAssetPropertiesRemap(
        modelMeta,
        stableKey,
        remapTargetId,
        "texture:main",
        "Assets/Textures/A.png");

    auto remaps = NLS::Editor::Assets::LoadModelTextureRemapSettings(modelMeta);
    ASSERT_EQ(remaps.size(), 1u);
    EXPECT_EQ(remaps[0].sourceStableKey, stableKey);
    EXPECT_EQ(remaps[0].targetAssetId, remapTargetId);
    EXPECT_EQ(remaps[0].targetEditorPath, "Assets/Textures/A.png");

    NLS::Editor::Panels::ClearModelTextureAssetPropertiesRemap(modelMeta, stableKey);
    EXPECT_TRUE(NLS::Editor::Assets::LoadModelTextureRemapSettings(modelMeta).empty());
}

TEST(AssetImportPipelineTests, ModelTextureStableKeyUsesVersionedKindSourceAndNormalizedUri)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.kind = NLS::Editor::Assets::TextureSourceKind::ExternalFile;
    source.sourceKey = "image/0";
    source.normalizedUri = "Textures/Wood.png";
    source.displayName = "Display Name";

    const auto key = NLS::Editor::Assets::MakeModelTextureStableKey(source);

    EXPECT_EQ(key, "mtxsrc:v1:kind=ExternalFile;source=image/0;uri=Textures/Wood.png");
}

TEST(AssetImportPipelineTests, ModelTextureStableKeyHandlesEmptySourceKeyDataUriBufferViewAndEmbeddedIndex)
{
    NLS::Editor::Assets::ModelTextureSourceReference dataUri;
    dataUri.kind = NLS::Editor::Assets::TextureSourceKind::EmbeddedData;
    dataUri.normalizedUri = "data:image/png;base64,AAAA";
    EXPECT_EQ(
        NLS::Editor::Assets::MakeModelTextureStableKey(dataUri),
        "mtxsrc:v1:kind=EmbeddedData;uri=data:image/png%3Bbase64,AAAA");

    NLS::Editor::Assets::ModelTextureSourceReference bufferView;
    bufferView.kind = NLS::Editor::Assets::TextureSourceKind::BufferView;
    bufferView.bufferViewKey = "bufferView/3";
    EXPECT_EQ(
        NLS::Editor::Assets::MakeModelTextureStableKey(bufferView),
        "mtxsrc:v1:kind=BufferView;bufferView=bufferView/3");

    NLS::Editor::Assets::ModelTextureSourceReference embedded;
    embedded.kind = NLS::Editor::Assets::TextureSourceKind::EmbeddedData;
    embedded.embeddedIndex = "2";
    EXPECT_EQ(
        NLS::Editor::Assets::MakeModelTextureStableKey(embedded),
        "mtxsrc:v1:kind=EmbeddedData;embedded=2");
}

TEST(AssetImportPipelineTests, ModelTextureStableKeyAddsDeterministicCollisionSuffixes)
{
    NLS::Editor::Assets::ModelTextureSourceReference first;
    first.kind = NLS::Editor::Assets::TextureSourceKind::ExternalFile;
    first.normalizedUri = "Textures/Wood.png";
    first.materialTextureKey = "material/Hero/baseColor";

    auto second = first;
    second.materialTextureKey = "material/Hero/normal";

    const auto assigned = NLS::Editor::Assets::AssignModelTextureStableKeys({ first, second });

    ASSERT_EQ(assigned.size(), 2u);
    EXPECT_NE(assigned[0].stableKey, assigned[1].stableKey);
    EXPECT_NE(assigned[0].stableKey.find("discriminator=material/Hero/baseColor"), std::string::npos);
    EXPECT_NE(assigned[1].stableKey.find("discriminator=material/Hero/normal"), std::string::npos);
    EXPECT_EQ(assigned[0].stableKeyStatus, NLS::Editor::Assets::ModelTextureStableKeyStatus::Stable);
    EXPECT_EQ(assigned[1].stableKeyStatus, NLS::Editor::Assets::ModelTextureStableKeyStatus::Stable);
}

TEST(AssetImportPipelineTests, ModelTextureStableKeyKeepsUriIdentityWhenDisplayNameChanges)
{
    NLS::Editor::Assets::ModelTextureSourceReference first;
    first.kind = NLS::Editor::Assets::TextureSourceKind::ExternalFile;
    first.normalizedUri = "Textures/Wood.png";
    first.displayName = "Wood A";

    auto second = first;
    second.displayName = "Wood B";

    EXPECT_EQ(
        NLS::Editor::Assets::MakeModelTextureStableKey(first),
        NLS::Editor::Assets::MakeModelTextureStableKey(second));
}

TEST(AssetImportPipelineTests, ModelTextureStableKeyEscapesSemicolonAndPipeSeparators)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.kind = NLS::Editor::Assets::TextureSourceKind::ExternalFile;
    source.normalizedUri = "Textures/Wood;Oak|A.png";

    const auto key = NLS::Editor::Assets::MakeModelTextureStableKey(source);

    EXPECT_NE(key.find("%3B"), std::string::npos);
    EXPECT_NE(key.find("%7C"), std::string::npos);
}

TEST(AssetImportPipelineTests, ModelTextureStableKeyMarksOrderDerivedFallback)
{
    NLS::Editor::Assets::ModelTextureSourceReference first;
    first.kind = NLS::Editor::Assets::TextureSourceKind::Missing;
    first.displayName = "Texture";

    auto second = first;

    const auto assigned = NLS::Editor::Assets::AssignModelTextureStableKeys({ first, second });

    ASSERT_EQ(assigned.size(), 2u);
    EXPECT_NE(assigned[0].stableKey, assigned[1].stableKey);
    EXPECT_EQ(assigned[0].stableKeyStatus, NLS::Editor::Assets::ModelTextureStableKeyStatus::OrderDerived);
    EXPECT_EQ(assigned[1].stableKeyStatus, NLS::Editor::Assets::ModelTextureStableKeyStatus::OrderDerived);
}

TEST(AssetImportPipelineTests, ModelTextureStableKeyIsStableAcrossRepeatedImports)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.kind = NLS::Editor::Assets::TextureSourceKind::BufferView;
    source.sourceKey = "image/4";
    source.bufferViewKey = "bufferView/2";
    source.stableDiscriminator = "imageIndex/4";

    EXPECT_EQ(
        NLS::Editor::Assets::MakeModelTextureStableKey(source),
        NLS::Editor::Assets::MakeModelTextureStableKey(source));
}

TEST(AssetImportPipelineTests, ModelTextureResolverUsesExplicitRemapBeforePath)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;uri=Textures/Wood.png";
    source.normalizedUri = "Assets/Textures/Wood.png";
    source.materialTextureKey = "image/0";

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.remaps.push_back({
        source.stableKey,
        {
            NLS::Core::Assets::AssetId(NLS::Guid::Parse("80808080-8080-4080-8080-808080808080")),
            "texture:remap",
            "Assets/Textures/Override.png",
            "Library/Artifacts/Override.ntex",
            "Override",
            NLS::Core::Assets::AssetType::Texture,
            true
        }
    });
    request.pathCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("81818181-8181-4181-8181-818181818181")),
        "texture:path",
        "Assets/Textures/Wood.png",
        "Library/Artifacts/Wood.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::ExplicitRemap);
    EXPECT_EQ(resolved.targetSubAssetKey, "texture:remap");
    EXPECT_EQ(resolved.resourcePath.generic_string(), "Library/Artifacts/Override.ntex");
}

TEST(AssetImportPipelineTests, ModelTextureResolverUsesSourcePathBeforeNameSearch)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;uri=Textures/Wood.png";
    source.normalizedUri = "Assets/Textures/Wood.png";

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.pathCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("82828282-8282-4282-8282-828282828282")),
        "texture:path",
        "Assets/Textures/Wood.png",
        "Library/Artifacts/Wood.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838383")),
        "texture:name",
        "Assets/Other/Wood.png",
        "Library/Artifacts/OtherWood.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::SourcePath);
    EXPECT_EQ(resolved.targetSubAssetKey, "texture:path");
}

TEST(AssetImportPipelineTests, ModelTextureResolverMatchesSourcePathCaseInsensitively)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;uri=Assets/Textures/casealbedo.png";
    source.normalizedUri = "Assets/Textures/casealbedo.png";

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.pathCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("81818181-8181-4181-8181-818181818181")),
        "texture:path",
        "Assets/Textures/CaseAlbedo.png",
        "Library/Artifacts/CaseAlbedo.ntex",
        "CaseAlbedo",
        NLS::Core::Assets::AssetType::Texture,
        true
    });
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("83838383-8383-4383-8383-838383838383")),
        "texture:name",
        "Assets/Other/CaseAlbedo.png",
        "Library/Artifacts/OtherCaseAlbedo.ntex",
        "CaseAlbedo",
        NLS::Core::Assets::AssetType::Texture,
        true
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::SourcePath);
    EXPECT_EQ(resolved.targetSubAssetKey, "texture:path");
}

TEST(AssetImportPipelineTests, ModelTextureResolverDoesNotBindAmbiguousNameMatches)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;name=Wood";
    source.hasModelLocalPayload = false;

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("84848484-8484-4484-8484-848484848484")),
        "texture:a",
        "Assets/A/Wood.png",
        "Library/Artifacts/A.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("85858585-8585-4585-8585-858585858585")),
        "texture:b",
        "Assets/B/Wood.png",
        "Library/Artifacts/B.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::Missing);
    ASSERT_FALSE(resolved.diagnostics.empty());
    EXPECT_EQ(resolved.diagnostics[0].code, "model-texture-name-ambiguous");
}

TEST(AssetImportPipelineTests, ModelTextureResolverUsesModelLocalFallbackWhenExternalDisabled)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;uri=Textures/Wood.png";
    source.sourceKey = "image/0";
    source.hasModelLocalPayload = true;

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.settings.useExternalTextures = false;
    request.pathCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("86868686-8686-4686-8686-868686868686")),
        "texture:path",
        "Assets/Textures/Wood.png",
        "Library/Artifacts/Wood.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::ModelEmbeddedFallback);
    EXPECT_EQ(resolved.modelSubAssetKey, "texture:image/0");
    ASSERT_FALSE(resolved.diagnostics.empty());
    EXPECT_EQ(resolved.diagnostics[0].code, "model-texture-external-resolution-disabled");
}

TEST(AssetImportPipelineTests, ModelTextureResolverOrdersMultiRootNameCandidatesDeterministically)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;name=Wood";

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("87878787-8787-4787-8787-878787878787")),
        "texture:later-root",
        "Packages/Shared/Wood.png",
        "Library/Artifacts/LaterRoot.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true,
        1u
    });
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("88888888-8888-4888-8888-888888888888")),
        "texture:first-root",
        "Assets/Textures/Wood.png",
        "Library/Artifacts/FirstRoot.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true,
        0u
    });
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("89898989-8989-4989-8989-898989898989")),
        "material:not-texture",
        "Assets/Materials/Wood.mat",
        "Library/Artifacts/Wood.nmat",
        "Wood",
        NLS::Core::Assets::AssetType::Material,
        true,
        0u
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::Missing);
    ASSERT_FALSE(resolved.diagnostics.empty());
    EXPECT_EQ(resolved.diagnostics[0].code, "model-texture-name-ambiguous");
}

TEST(AssetImportPipelineTests, ModelTextureResolverBindsUniqueTextureNameAfterRejectingNonTextures)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;name=Wood";

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("90909090-9090-4090-9090-909090909090")),
        "material:wood",
        "Assets/Materials/Wood.mat",
        "Library/Artifacts/Wood.nmat",
        "Wood",
        NLS::Core::Assets::AssetType::Material,
        true
    });
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("91919191-9191-4191-9191-919191919191")),
        "texture:wood",
        "Assets/Textures/Wood.png",
        "Library/Artifacts/Wood.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::NameSearch);
    EXPECT_EQ(resolved.targetSubAssetKey, "texture:wood");
}

TEST(AssetImportPipelineTests, ModelTextureResolverTreatsCaseCollisionAsAmbiguous)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;name=wood.png";

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("92929292-9292-4292-9292-929292929292")),
        "texture:lower",
        "Assets/A/wood.png",
        "Library/Artifacts/lower.ntex",
        "wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("93939393-9393-4393-9393-939393939393")),
        "texture:upper",
        "Assets/B/Wood.png",
        "Library/Artifacts/upper.ntex",
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        true
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::Missing);
    ASSERT_FALSE(resolved.diagnostics.empty());
    EXPECT_EQ(resolved.diagnostics[0].code, "model-texture-name-ambiguous");
}

TEST(AssetImportPipelineTests, ModelTextureResolverSkipsUnimportedNameCandidatesWhenAutoImportDisabled)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;name=Wood";
    source.hasModelLocalPayload = true;
    source.sourceKey = "image/0";

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.settings.autoImportMissingTextureFiles = false;
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("94949494-9494-4494-9494-949494949494")),
        "texture:wood",
        "Assets/Textures/Wood.png",
        {},
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        false
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::ModelEmbeddedFallback);
    EXPECT_EQ(resolved.modelSubAssetKey, "texture:image/0");
}

TEST(AssetImportPipelineTests, ModelTextureResolverWarnsForUnimportedNameCandidateUntilArtifactExists)
{
    NLS::Editor::Assets::ModelTextureSourceReference source;
    source.stableKey = "mtxsrc:v1:kind=ExternalFile;name=Wood";

    NLS::Editor::Assets::ModelTextureResolveRequest request;
    request.settings.autoImportMissingTextureFiles = true;
    request.nameCandidates.push_back({
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("95959595-9595-4595-9595-959595959595")),
        "texture:wood",
        "Assets/Textures/Wood.png",
        {},
        "Wood",
        NLS::Core::Assets::AssetType::Texture,
        false
    });

    const auto resolved = NLS::Editor::Assets::ResolveModelTextureReference(source, request);

    EXPECT_EQ(resolved.kind, NLS::Editor::Assets::ModelTextureResolutionKind::Missing);
    EXPECT_TRUE(resolved.resourcePath.empty());
    ASSERT_FALSE(resolved.diagnostics.empty());
    EXPECT_EQ(resolved.diagnostics[0].code, "model-texture-artifact-missing");
}

TEST(AssetImportPipelineTests, ModelImporterSettingsRejectUnknownFbxReaderToAutodeskWithAssimpFallback)
{
    const std::map<std::string, std::string> settings {
        {"MODEL_FBX_READER", "mystery-reader"}
    };

    const auto parsed = NLS::Editor::Assets::ModelImporterSettingsFromSerialized(settings);

    EXPECT_EQ(
        parsed.fbxReaderSelection,
        NLS::Editor::Assets::FbxReaderSelection::AutodeskWithAssimpFallback);
}

TEST(AssetImportPipelineTests, ModelImporterSettingsResolveFallbackFbxReaderSelection)
{
    const std::map<std::string, std::string> settings {
        {"MODEL_FBX_READER", "autodesk-with-assimp-fallback"}
    };

    const auto parsed = NLS::Editor::Assets::ModelImporterSettingsFromSerialized(settings);

    EXPECT_EQ(
        parsed.fbxReaderSelection,
        NLS::Editor::Assets::FbxReaderSelection::AutodeskWithAssimpFallback);
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesFbxParityVersion6Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        6u)
        << "Importer version 6 artifacts can still contain dark FBX albedo, invalid roughness, and opaque decal metadata.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesFbxDecalRootNameVersion7Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        7u)
        << "Importer version 7 artifacts can still contain FBX dirt_decal as Opaque and generated prefab roots named RootNode.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesFbxRawOpacityVersion8Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        8u)
        << "Importer version 8 artifacts can still miss FBX 3dsMax Parameters transparency/cutout maps.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesPrefabValidationProofVersion9Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        9u)
        << "Importer version 9 generated prefab artifacts can still carry path-sensitive validation proofs that block prepared runtime graph caching.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesFbxUvOriginVersion11Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        11u)
        << "Importer version 11 FBX mesh artifacts can sample shared textures with an inverted UV origin.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesGltfPackedMaterialChannelVersion12Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        12u)
        << "Importer version 12 glTF materials can sample packed metallic and roughness values from the red channel.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesFbxTexturedMetallicVersion13Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        13u)
        << "Importer version 13 FBX materials can multiply authored metalness maps by Assimp's default zero factor.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesFbxNearZeroMetallicVersion14Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        14u)
        << "Importer version 14 FBX materials can preserve Assimp's near-zero metallic default and disable authored metalness maps.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesFbxMissingMetallicFactorVersion15Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        15u)
        << "Importer version 15 FBX materials can default a texture-only metalness channel to a zero multiplier.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesFbxLegacySpecularVersion16Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        16u)
        << "Importer version 16 FBX PBR materials can retain an untextured white legacy specular color and render too bright.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesCrossFormatNormalMapVersion17Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        17u)
        << "Importer version 17 FBX materials can omit normal-named bump textures and OBJ materials can misdecode height maps as tangent-space normals.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesIncompleteFbxBump2dVersion18Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        18u)
        << "Importer version 18 FBX materials can still omit file textures connected through a 3ds Max ai_bump2d node.";
}

TEST(AssetImportPipelineTests, ModelSceneImporterVersionInvalidatesSrgbProjectNormalVersion19Artifacts)
{
    EXPECT_GT(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        19u)
        << "Importer version 19 materials can bind sRGB project texture artifacts to linear normal and mask slots.";
}

#if !NLS_HAS_AUTODESK_FBX_SDK && NLS_HAS_ASSIMP_FBX_IMPORTER
TEST(AssetImportPipelineTests, ExternalModelImportDefaultFbxReaderFallsBackToAssimpWhenAutodeskSdkUnavailable)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "DefaultAutodeskFallback.fbx";
    WriteTextFile(sourcePath, "not a valid fbx");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a7a7a7a7-a7a7-4a7a-8a7a-a7a7a7a7a7a7"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    DisableExternalModelTextureResolution(meta);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "DefaultAutodeskUnavailable",
        "editor",
        nullptr,
        nullptr,
        {},
        {},
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    EXPECT_TRUE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-fbx-reader-fallback"))
        << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_FALSE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-autodesk-fbx-unavailable"))
        << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_FALSE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-assimp-fbx-unavailable"))
        << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_TRUE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-source-parse-failed"))
        << JoinDiagnosticSummaries(result.diagnostics);

    std::filesystem::remove_all(root);
}
#endif

#if !NLS_HAS_ASSIMP_FBX_IMPORTER
TEST(AssetImportPipelineTests, ExternalModelImportDefaultFbxReaderDoesNotFallbackToAssimp)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "DefaultNoFallback.fbx";
    WriteTextFile(sourcePath, "not a valid fbx");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a5a5a5a5-a5a5-4a5a-8a5a-a5a5a5a5a5a5"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "DefaultNoFallback",
        "editor",
        nullptr,
        nullptr,
        {},
        {},
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    EXPECT_FALSE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-fbx-reader-fallback"));
    EXPECT_FALSE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-assimp-fbx-unavailable"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportReportsUnavailableAssimpFbxWhenExplicitlySelected)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "AssimpUnavailable.fbx";
    WriteTextFile(sourcePath, "not a valid fbx");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a3a3a3a3-a3a3-4a3a-8a3a-a3a3a3a3a3a3"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    meta.settings["MODEL_FBX_READER"] = "assimp";

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "AssimpUnavailable",
        "editor",
        nullptr,
        nullptr,
        {},
        {},
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    EXPECT_TRUE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-assimp-fbx-unavailable"));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportFallbackModeReportsFallbackBeforeUnavailableAssimp)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "FallbackUnavailable.fbx";
    WriteTextFile(sourcePath, "not a valid fbx");

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a4a4a4a4-a4a4-4a4a-8a4a-a4a4a4a4a4a4"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    meta.settings["MODEL_FBX_READER"] = "autodesk-with-assimp-fallback";

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "FallbackUnavailable",
        "editor",
        nullptr,
        nullptr,
        {},
        {},
        root,
        {}
    });

    EXPECT_FALSE(result.imported);
    EXPECT_TRUE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-fbx-reader-fallback"));
    EXPECT_TRUE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-assimp-fbx-unavailable"));
    const auto* fallbackDiagnostic = FindDiagnosticByCode(
        result.diagnostics,
        "external-model-importer-fbx-reader-fallback");
    ASSERT_NE(fallbackDiagnostic, nullptr);
    EXPECT_EQ(fallbackDiagnostic->message.find("imported with Assimp"), std::string::npos);
    EXPECT_NE(fallbackDiagnostic->message.find("attempting Assimp"), std::string::npos);

    std::filesystem::remove_all(root);
}
#endif

#if NLS_HAS_ASSIMP_FBX_IMPORTER
TEST(AssetImportPipelineTests, ExternalModelImportExplicitAssimpFbxBuildsArtifactsWhenEnabled)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "AssimpCube.fbx";
    std::filesystem::create_directories(sourcePath.parent_path());
    std::filesystem::copy_file(
        std::filesystem::path(NLS_ROOT_DIR) / "App" / "Assets" / "Engine" / "Models" / "Cube.fbx",
        sourcePath,
        std::filesystem::copy_options::overwrite_existing);

    NLS::Core::Assets::AssetMeta meta;
    meta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("a6a6a6a6-a6a6-4a6a-8a6a-a6a6a6a6a6a6"));
    meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    meta.importerId = "scene-model";
    meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
    meta.settings["MODEL_FBX_READER"] = "assimp";

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / meta.id.ToString(),
        meta,
        "AssimpCube",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported);
    EXPECT_FALSE(ContainsDiagnosticCode(
        result.diagnostics,
        "external-model-importer-assimp-fbx-unavailable"));

    const auto* meshArtifact = result.manifest.FindSubAsset("mesh:parser/mesh/0");
    ASSERT_NE(meshArtifact, nullptr);
    const auto mesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadArtifactFile(root, *meshArtifact));
    ASSERT_TRUE(mesh.has_value());
    EXPECT_FALSE(mesh->vertices.empty());
    EXPECT_FALSE(mesh->indices.empty());

    NLS::Render::Resources::Parsers::AssimpParser uvReferenceParser;
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> uvReferenceMeshes;
    std::vector<std::string> uvReferenceMaterials;
    ASSERT_TRUE(uvReferenceParser.LoadModelData(
        sourcePath.string(),
        uvReferenceMeshes,
        uvReferenceMaterials,
        NLS::Render::Resources::Parsers::EModelParserFlags::TRIANGULATE |
            NLS::Render::Resources::Parsers::EModelParserFlags::GLOBAL_SCALE |
            NLS::Render::Resources::Parsers::EModelParserFlags::FLIP_UVS));
    ASSERT_FALSE(uvReferenceMeshes.empty());
    ASSERT_EQ(mesh->vertices.size(), uvReferenceMeshes.front().vertices.size());
    for (size_t vertexIndex = 0u; vertexIndex < mesh->vertices.size(); ++vertexIndex)
    {
        EXPECT_FLOAT_EQ(
            mesh->vertices[vertexIndex].texCoords[1],
            uvReferenceMeshes.front().vertices[vertexIndex].texCoords[1])
            << "FBX artifacts must normalize the UV origin so shared textures sample consistently with glTF.";
    }
    const float maxMeshPosition = MaxAbsMeshPosition(*mesh);
    EXPECT_LT(maxMeshPosition, 2.0f)
        << "FBX import should preserve the same meter-scale convention as glTF imports.";

    const auto* prefabArtifact = result.manifest.FindSubAsset("prefab:AssimpCube");
    ASSERT_NE(prefabArtifact, nullptr);
    const auto prefabPayload = ReadArtifactPayloadText(
        root,
        *prefabArtifact,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    const auto prefabGraph = NLS::Engine::Serialize::ObjectGraphReader::Read(prefabPayload);
    ASSERT_TRUE(prefabGraph.has_value());
    const std::string transformTypeName = "NLS::Engine::Components::TransformComponent";
    const auto* authoredScaleTransform = FindRecord(
        *prefabGraph,
        "pCube1 Transform",
        transformTypeName);
    ASSERT_NE(authoredScaleTransform, nullptr)
        << "The FBX authored mesh node should remain present after synthetic-root normalization. "
        << DescribeTransformLocalScales(*prefabGraph, transformTypeName);
    const auto* authoredScale = FindProperty(*authoredScaleTransform, "localScale");
    ASSERT_NE(authoredScale, nullptr);
    EXPECT_DOUBLE_EQ(GetObjectNumber(authoredScale->value, "x"), 100.0);
    EXPECT_DOUBLE_EQ(GetObjectNumber(authoredScale->value, "y"), 100.0);
    EXPECT_DOUBLE_EQ(GetObjectNumber(authoredScale->value, "z"), 100.0);
    const double maxInstantiatedPosition =
        static_cast<double>(maxMeshPosition) * MaxAbsLocalScale(authoredScale->value);
    EXPECT_GT(maxInstantiatedPosition, 0.25)
        << "FBX import should not shrink the generated prefab below the expected cube scale. "
        << DescribeTransformLocalScales(
            *prefabGraph,
            transformTypeName);
    EXPECT_LT(maxInstantiatedPosition, 2.0)
        << "FBX prefab transforms should not make the generated mesh instantiate 100x larger than glTF scale. "
        << DescribeTransformLocalScales(
            *prefabGraph,
            transformTypeName);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalAssimpFbxNeutralDiffusePolicyAffectsMaterialArtifactIdentity)
{
    const auto root = MakeImportTestRoot();
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "maxPbrMaterial_metalRough.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));

    auto fbx = ReadTextFile(sourceFixture);
    const std::string diffuseColorLine =
        "P: \"DiffuseColor\", \"ColorRGB\", \"Color\", \"\",0,1,1";
    const auto diffuseColorBegin = fbx.find(diffuseColorLine);
    ASSERT_NE(diffuseColorBegin, std::string::npos);
    fbx.replace(
        diffuseColorBegin,
        diffuseColorLine.size(),
        "P: \"DiffuseColor\", \"ColorRGB\", \"Color\", \"\",0.5,0.5,0.5");
    const std::string baseColorLine =
        "P: \"3dsMax|main|basecolor\", \"ColorAndAlpha\", \"\", \"A\",0,1,1,1";
    const auto baseColorBegin = fbx.find(baseColorLine);
    ASSERT_NE(baseColorBegin, std::string::npos);
    fbx.replace(
        baseColorBegin,
        baseColorLine.size(),
        "P: \"3dsMax|main|basecolor\", \"ColorAndAlpha\", \"\", \"A\",0.5,0.5,0.5,1");

    const auto sourcePath = root / "Assets" / "Models" / "NeutralPbrMaterial.fbx";
    WriteTextFile(sourcePath, fbx);
    for (const auto* textureName : {
        "albedo.png",
        "metalness.png",
        "roughness.png",
        "occlusion.png",
        "normal.png",
        "emission.png",
        "opacity.png"
    })
    {
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / textureName, TinyPng());
    }

    const auto importMaterial = [&](
        const char* assetId,
        const char* sceneKey,
        const std::optional<bool> ignoreNeutralTint)
    {
        NLS::Core::Assets::AssetMeta meta;
        meta.id = NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetId));
        meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
        meta.importerId = "scene-model";
        meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
        meta.settings["MODEL_FBX_READER"] = "assimp";
        if (ignoreNeutralTint.has_value())
        {
            meta.settings[NLS::Editor::Assets::kModelFbxIgnoreTexturedNeutralDiffuseTintSetting] =
                *ignoreNeutralTint ? "true" : "false";
        }

        auto result = NLS::Editor::Assets::ImportExternalModelAsset({
            sourcePath,
            root / "Staging" / meta.id.ToString(),
            root / "Library" / "Artifacts" / meta.id.ToString(),
            meta,
            sceneKey,
            "editor",
            nullptr,
            nullptr,
            {},
            std::filesystem::path("Models"),
            root,
            {}
        });
        EXPECT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
        return result;
    };

    const auto defaultResult = importMaterial(
        "b6b6b6b6-b6b6-4b6b-8b6b-b6b6b6b6b6b6",
        "NeutralPbrDefault",
        std::nullopt);
    ASSERT_TRUE(defaultResult.imported);
    const auto* defaultMaterial = FindFirstArtifactOfType(
        defaultResult.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(defaultMaterial, nullptr);
    const auto defaultPayload = ReadArtifactPayloadText(
        root,
        *defaultMaterial,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    EXPECT_NE(
        defaultPayload.find("property _BaseColor Color 1.000000 1.000000 1.000000 1.000000"),
        std::string::npos);
    EXPECT_EQ(defaultPayload.find("0.500000 0.500000 0.500000 1.000000"), std::string::npos);

    const auto preservedResult = importMaterial(
        "c6c6c6c6-c6c6-4c6c-8c6c-c6c6c6c6c6c6",
        "NeutralPbrPreserved",
        false);
    ASSERT_TRUE(preservedResult.imported);
    const auto* preservedMaterial = FindFirstArtifactOfType(
        preservedResult.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(preservedMaterial, nullptr);
    const auto preservedPayload = ReadArtifactPayloadText(
        root,
        *preservedMaterial,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    EXPECT_NE(
        preservedPayload.find("property _BaseColor Color 0.500000 0.500000 0.500000 1.000000"),
        std::string::npos);
    EXPECT_NE(defaultMaterial->contentHash, preservedMaterial->contentHash)
        << "Changing FBX neutral-diffuse compatibility policy must change generated material identity.";

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalAssimpFbxModelImportKeepsDiffuseOpacityAndNormalMaterialTextureKeysWhenExternallyResolved)
{
    const auto root = MakeImportTestRoot();
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "maxPbrMaterial_metalRough.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));

    const auto sourcePath = root / "Assets" / "Models" / "ExternalFbxTextureSlots.fbx";
    WriteTextFile(sourcePath, ReadTextFile(sourceFixture));

    struct TextureFixture
    {
        const char* fileName;
        const char* assetId;
        const char* displayName;
        NLS::Render::Assets::TextureArtifactColorSpace colorSpace;
    };
    const TextureFixture textureFixtures[] = {
        {"albedo.png", "25252525-2525-4252-8252-252525252525", "FbxAlbedo", NLS::Render::Assets::TextureArtifactColorSpace::Srgb},
        {"metalness.png", "26262626-2626-4262-8262-262626262626", "FbxMetalness", NLS::Render::Assets::TextureArtifactColorSpace::Linear},
        {"roughness.png", "27272727-2727-4272-8272-272727272727", "FbxRoughness", NLS::Render::Assets::TextureArtifactColorSpace::Linear},
        {"occlusion.png", "28282828-2828-4282-8282-282828282828", "FbxOcclusion", NLS::Render::Assets::TextureArtifactColorSpace::Linear},
        {"normal.png", "29292929-2929-4292-8292-292929292929", "FbxNormal", NLS::Render::Assets::TextureArtifactColorSpace::Linear},
        {"emission.png", "30303030-3030-4303-8303-303030303030", "FbxEmission", NLS::Render::Assets::TextureArtifactColorSpace::Srgb},
        {"opacity.png", "31313131-3131-4313-8313-313131313131", "FbxOpacity", NLS::Render::Assets::TextureArtifactColorSpace::Linear}
    };

    std::filesystem::path albedoArtifactPath;
    std::filesystem::path normalArtifactPath;
    std::filesystem::path opacityArtifactPath;
    for (const auto& texture : textureFixtures)
    {
        const auto texturePath = sourcePath.parent_path() / "Textures" / texture.fileName;
        WriteBinaryFile(texturePath, TinyPng());
        const auto artifactPath = WriteImportedTextureAssetForTest(
            root,
            texturePath,
            texture.assetId,
            "editor",
            texture.displayName,
            texture.colorSpace);
        if (std::string(texture.fileName) == "albedo.png")
            albedoArtifactPath = artifactPath;
        else if (std::string(texture.fileName) == "normal.png")
            normalArtifactPath = artifactPath;
        else if (std::string(texture.fileName) == "opacity.png")
            opacityArtifactPath = artifactPath;
    }

    NLS::Core::Assets::AssetMeta modelMeta;
    modelMeta.id = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("32323232-3232-4323-8323-323232323232"));
    modelMeta.assetType = NLS::Core::Assets::AssetType::ModelScene;
    modelMeta.importerId = "scene-model";
    modelMeta.importerVersion = CurrentModelSceneImporterVersion();
    modelMeta.settings["MODEL_FBX_READER"] = "assimp";

    const auto result = NLS::Editor::Assets::ImportExternalModelAsset({
        sourcePath,
        root / "Staging",
        root / "Library" / "Artifacts" / modelMeta.id.ToString(),
        modelMeta,
        "ExternalFbxTextureSlots",
        "editor",
        nullptr,
        nullptr,
        {},
        std::filesystem::path("Models"),
        root,
        {}
    });

    ASSERT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
    EXPECT_EQ(
        std::count_if(
            result.manifest.subAssets.begin(),
            result.manifest.subAssets.end(),
            [](const NLS::Core::Assets::ImportedArtifact& artifact)
            {
                return artifact.artifactType == NLS::Core::Assets::ArtifactType::Texture;
            }),
        0);

    const auto* materialArtifact = FindFirstArtifactOfType(
        result.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(materialArtifact, nullptr);
    const auto payload = ReadArtifactPayloadText(root, *materialArtifact, NLS::Core::Assets::ArtifactType::Material, 1u);
    ExpectMaterialTextureSlot(payload, "_BaseMap", root, albedoArtifactPath);
    ExpectMaterialTextureSlot(payload, "_NormalMap", root, normalArtifactPath);
    ExpectMaterialTextureSlot(payload, "_OpacityMap", root, opacityArtifactPath);
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aparser%2Ftexture%2F0");
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aparser%2Ftexture%2F4");
    ExpectNoLegacyTextureArtifactReference(payload, "texture%3Aparser%2Ftexture%2F6");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalAssimpFbxBaseColorAlphaDecalImportsAsDecalMaterial)
{
    const auto root = MakeImportTestRoot();
    const auto sourceFixture =
        std::filesystem::path(NLS_ROOT_DIR) /
        "ThirdParty" /
        "assimp" /
        "test" /
        "models" /
        "FBX" /
        "maxPbrMaterial_metalRough.fbx";
    ASSERT_TRUE(std::filesystem::exists(sourceFixture));

    auto fbx = ReadTextFile(sourceFixture);
    const std::string materialName = "Material::PBR Material";
    const auto materialNameBegin = fbx.find(materialName);
    ASSERT_NE(materialNameBegin, std::string::npos);
    fbx.replace(materialNameBegin, materialName.size(), "Material::dirt_decal");
    EraseFbxConnectionBlock(fbx, ";Texture::Opacity, Material::PBR Material");
    EraseFbxConnectionBlock(fbx, ";Video::Opacity, Texture::Opacity");

    const auto importDecal = [&](
        const char* assetGuid,
        const char* sceneKey,
        const std::vector<uint8_t>& albedoPng)
    {
        const auto sourcePath = root / "Assets" / "Models" / (std::string(sceneKey) + ".fbx");
        WriteTextFile(sourcePath, fbx);
        for (const auto* textureName : {
            "metalness.png",
            "roughness.png",
            "occlusion.png",
            "normal.png",
            "emission.png",
            "opacity.png"
        })
        {
            WriteBinaryFile(sourcePath.parent_path() / "Textures" / textureName, TinyPng());
        }
        WriteBinaryFile(sourcePath.parent_path() / "Textures" / "albedo.png", albedoPng);

        NLS::Core::Assets::AssetMeta meta;
        meta.id = NLS::Core::Assets::AssetId(NLS::Guid::Parse(assetGuid));
        meta.assetType = NLS::Core::Assets::AssetType::ModelScene;
        meta.importerId = "scene-model";
        meta.importerVersion = NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene);
        meta.settings["MODEL_FBX_READER"] = "assimp";

        auto result = NLS::Editor::Assets::ImportExternalModelAsset({
            sourcePath,
            root / "Staging" / meta.id.ToString(),
            root / "Library" / "Artifacts" / meta.id.ToString(),
            meta,
            sceneKey,
            "editor",
            nullptr,
            nullptr,
            {},
            std::filesystem::path("Models"),
            root,
            {}
        });
        EXPECT_TRUE(result.imported) << JoinDiagnosticSummaries(result.diagnostics);
        return result;
    };

    const auto transparentResult = importDecal(
        "d6d6d6d6-d6d6-4d6d-8d6d-d6d6d6d6d6d6",
        "BaseColorAlphaDecal",
        TinyTransparentRgbaPng());
    ASSERT_TRUE(transparentResult.imported);
    const auto* transparentMaterial = FindFirstArtifactOfType(
        transparentResult.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(transparentMaterial, nullptr);
    const auto transparentPayload = ReadArtifactPayloadText(
        root,
        *transparentMaterial,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    EXPECT_NE(transparentPayload.find("name=dirt_decal"), std::string::npos);
    EXPECT_NE(transparentPayload.find("alphaMode=Blend"), std::string::npos);
    EXPECT_NE(transparentPayload.find("surfaceMode=Decal"), std::string::npos);
    EXPECT_NE(transparentPayload.find("depthWrite=false"), std::string::npos);
    EXPECT_EQ(transparentPayload.find("_OpacityMap"), std::string::npos)
        << "This regression must be covered by base-color alpha evidence, not the explicit opacity slot path.";

    const auto opaqueResult = importDecal(
        "e6e6e6e6-e6e6-4e6e-8e6e-e6e6e6e6e6e6",
        "BaseColorOpaqueDecal",
        TinyOpaqueRgbaPng());
    ASSERT_TRUE(opaqueResult.imported);
    const auto* opaqueMaterial = FindFirstArtifactOfType(
        opaqueResult.manifest,
        NLS::Core::Assets::ArtifactType::Material);
    ASSERT_NE(opaqueMaterial, nullptr);
    const auto opaquePayload = ReadArtifactPayloadText(
        root,
        *opaqueMaterial,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    EXPECT_NE(opaquePayload.find("name=dirt_decal"), std::string::npos);
    EXPECT_NE(opaquePayload.find("alphaMode=Opaque"), std::string::npos);
    EXPECT_NE(opaquePayload.find("surfaceMode=Opaque"), std::string::npos);
    EXPECT_EQ(opaquePayload.find("surfaceMode=Decal"), std::string::npos);

    std::filesystem::remove_all(root);
}
#endif

TEST(AssetImportPipelineTests, FbxImporterConvertsParserDataAndReportsParserExposureLimits)
{
    FakeModelParser parser;
    std::vector<NLS::Render::Resources::Mesh*> parserMeshes;
    parserMeshes.push_back(new NLS::Render::Resources::Mesh({}, {}, 1u));
    parser.meshes = parserMeshes;
    parser.materialNames = {"HeroFbxMat", "Stone"};

    const auto scene = NLS::Render::Assets::ImportParserModelScene(
        parser,
        "Assets/Models/Hero.fbx",
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        "HeroFbx");

    EXPECT_TRUE(parser.loadCalled);
    EXPECT_EQ(scene.sceneKey, "HeroFbx");
    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].sourceKey, "parser/mesh/0");
    ASSERT_EQ(scene.meshes[0].primitives.size(), 1u);
    EXPECT_EQ(scene.meshes[0].primitives[0].materialKey, "parser/material/1");
    ASSERT_EQ(scene.materials.size(), 2u);
    EXPECT_EQ(scene.materials[0].sourceKey, "parser/material/0");
    ASSERT_EQ(scene.nodes.size(), 1u);
    EXPECT_EQ(scene.nodes[0].sourceKey, "parser/node/0");
    EXPECT_EQ(scene.nodes[0].meshKey, "parser/mesh/0");
    ASSERT_EQ(scene.diagnostics.size(), 1u);
    EXPECT_EQ(scene.diagnostics[0].code, "fbx-parser-limited-scene-data");
}

TEST(AssetImportPipelineTests, ParsedFbxMeshDataBuildsImporterSceneWithoutSecondParserLoad)
{
    std::vector<NLS::Render::Resources::Parsers::ParsedMeshData> meshes;
    NLS::Render::Resources::Parsers::ParsedMeshData mesh;
    mesh.materialIndex = 1u;
    meshes.push_back(std::move(mesh));

    const auto scene = NLS::Render::Assets::ImportParsedModelScene(
        meshes,
        {"Unused", "HeroFbxMat"},
        "Assets/Models/Hero.fbx",
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("a1a1a1a1-a1a1-4a1a-8a1a-a1a1a1a1a1a1")),
        "HeroFbx");

    EXPECT_EQ(scene.sceneKey, "HeroFbx");
    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].sourceKey, "parser/mesh/0");
    ASSERT_EQ(scene.meshes[0].primitives.size(), 1u);
    EXPECT_EQ(scene.meshes[0].primitives[0].materialKey, "parser/material/1");
    ASSERT_EQ(scene.materials.size(), 2u);
    EXPECT_EQ(scene.materials[1].name, "HeroFbxMat");
    ASSERT_EQ(scene.nodes.size(), 1u);
    EXPECT_EQ(scene.nodes[0].meshKey, "parser/mesh/0");
    ASSERT_EQ(scene.diagnostics.size(), 1u);
    EXPECT_EQ(scene.diagnostics[0].code, "fbx-parser-limited-scene-data");
}

TEST(AssetImportPipelineTests, FbxImporterConsumesParserExposedSceneHierarchyAndRigData)
{
    DetailedFakeModelParser parser;
    parser.meshCount = 1u;
    parser.materialNames = {"HeroSurface"};

    parser.nodes.push_back({"fbx/node/root", "HeroRoot", "", "", ""});
    parser.nodes.push_back({"fbx/node/body", "HeroBody", "fbx/node/root", "fbx/mesh/body", "fbx/skin/body"});

    NLS::Render::Assets::ImportedSceneNamedRecord mesh;
    mesh.sourceKey = "fbx/mesh/body";
    mesh.name = "HeroBodyMesh";
    mesh.primitiveCount = 2u;
    mesh.attributes = {"POSITION", "NORMAL", "TANGENT", "TEXCOORD_0", "JOINTS_0", "WEIGHTS_0"};
    parser.meshes.push_back(mesh);

    parser.textures.push_back({"fbx/texture/diffuse", "HeroDiffuse", "Textures/Hero_D.png"});
    parser.textures.push_back({"fbx/texture/normal", "HeroNormal", "Textures/Hero_N.png"});

    NLS::Render::Assets::ImportedSceneNamedRecord material;
    material.sourceKey = "fbx/material/HeroSurface";
    material.name = "HeroSurface";
    material.materialChannels.push_back({"diffuse", "fbx/texture/diffuse", {1.0, 0.9, 0.8}, false, 0.0});
    material.materialChannels.push_back({"normal", "fbx/texture/normal", {}, false, 0.0});
    material.materialChannels.push_back({"roughness", {}, {}, true, 0.35});
    parser.materials.push_back(material);

    parser.skeletons.push_back({"fbx/skeleton/hero", "HeroSkeleton"});

    NLS::Render::Assets::ImportedSceneNamedRecord skin;
    skin.sourceKey = "fbx/skin/body";
    skin.name = "BodySkin";
    skin.skeletonKey = "fbx/skeleton/hero";
    skin.joints = {"fbx/node/root", "fbx/node/body"};
    parser.skins.push_back(skin);

    NLS::Render::Assets::ImportedSceneNamedRecord animation;
    animation.sourceKey = "fbx/animation/run";
    animation.name = "Run";
    animation.targets = {"fbx/node/body:translation", "fbx/node/body:rotation"};
    parser.animations.push_back(animation);

    NLS::Render::Assets::ImportedSceneNamedRecord morph;
    morph.sourceKey = "fbx/morph/smile";
    morph.name = "Smile";
    morph.meshKey = "fbx/mesh/body";
    parser.morphTargets.push_back(morph);

    const auto scene = NLS::Render::Assets::ImportParserModelScene(
        parser,
        "Assets/Models/Hero.fbx",
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("34343434-3434-4434-8434-343434343434")),
        "HeroFbxDetailed");

    EXPECT_TRUE(parser.loadCalled);
    ASSERT_EQ(scene.nodes.size(), 2u);
    EXPECT_EQ(scene.nodes[1].parentKey, "fbx/node/root");
    EXPECT_EQ(scene.nodes[1].meshKey, "fbx/mesh/body");
    EXPECT_EQ(scene.nodes[1].skinKey, "fbx/skin/body");

    ASSERT_EQ(scene.materials.size(), 1u);
    const auto* diffuseChannel = FindMaterialChannel(scene.materials[0], "diffuse");
    ASSERT_NE(diffuseChannel, nullptr);
    EXPECT_EQ(diffuseChannel->textureKey, "fbx/texture/diffuse");
    const auto* roughnessChannel = FindMaterialChannel(scene.materials[0], "roughness");
    ASSERT_NE(roughnessChannel, nullptr);
    EXPECT_TRUE(roughnessChannel->hasScalar);
    EXPECT_DOUBLE_EQ(roughnessChannel->scalar, 0.35);

    ASSERT_EQ(scene.textures.size(), 2u);
    EXPECT_EQ(scene.textures[0].uri, "Textures/Hero_D.png");
    EXPECT_EQ(scene.textures[1].uri, "Textures/Hero_N.png");

    ASSERT_EQ(scene.skeletons.size(), 1u);
    EXPECT_EQ(scene.skeletons[0].sourceKey, "fbx/skeleton/hero");
    ASSERT_EQ(scene.skins.size(), 1u);
    EXPECT_EQ(scene.skins[0].skeletonKey, "fbx/skeleton/hero");
    ASSERT_EQ(scene.skins[0].joints.size(), 2u);
    ASSERT_EQ(scene.animations.size(), 1u);
    EXPECT_EQ(scene.animations[0].targets[1], "fbx/node/body:rotation");
    ASSERT_EQ(scene.morphTargets.size(), 1u);
    EXPECT_EQ(scene.morphTargets[0].meshKey, "fbx/mesh/body");
    EXPECT_TRUE(scene.diagnostics.empty());
}

TEST(AssetImportPipelineTests, ParserDetailedSceneFailureFallsBackWithoutPartialData)
{
    DetailedFakeModelParser parser;
    parser.meshCount = 1u;
    parser.materialNames = {"FallbackMaterial"};
    parser.detailedResult = false;
    parser.nodes.push_back({"broken/node", "BrokenPartial", "", "", ""});
    parser.meshes.push_back({"broken/mesh", "BrokenMesh"});
    parser.diagnostics.push_back({"parser-detail-broken", "Partial detailed data should not escape."});

    const auto scene = NLS::Render::Assets::ImportParserModelScene(
        parser,
        "Assets/Models/Hero.fbx",
        NLS::Render::Assets::SceneModelSourceFormat::Fbx,
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("56565656-5656-4656-8656-565656565656")),
        "HeroFbxFallback");

    ASSERT_EQ(scene.meshes.size(), 1u);
    EXPECT_EQ(scene.meshes[0].sourceKey, "parser/mesh/0");
    ASSERT_EQ(scene.materials.size(), 1u);
    EXPECT_EQ(scene.materials[0].name, "FallbackMaterial");
    ASSERT_EQ(scene.nodes.size(), 1u);
    EXPECT_EQ(scene.nodes[0].sourceKey, "parser/node/0");
    EXPECT_EQ(scene.nodes[0].name, "Mesh 0");
    EXPECT_EQ(scene.nodes[0].meshKey, "parser/mesh/0");
    ASSERT_EQ(scene.diagnostics.size(), 2u);
    EXPECT_EQ(scene.diagnostics[0].code, "parser-detailed-scene-data-failed");
    EXPECT_EQ(scene.diagnostics[1].code, "fbx-parser-limited-scene-data");
}

TEST(AssetImportPipelineTests, ArtifactLoadTelemetrySummarizesStageBaselineForPrefabPathComparison)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;
    using NLS::Core::Assets::ClearArtifactLoadTelemetry;
    using NLS::Core::Assets::RecordArtifactLoadTelemetry;
    using NLS::Core::Assets::SummarizeArtifactLoadTelemetry;

    struct ScopedArtifactTelemetryClear
    {
        ScopedArtifactTelemetryClear() { ClearArtifactLoadTelemetry(); }
        ~ScopedArtifactTelemetryClear() { ClearArtifactLoadTelemetry(); }
    } telemetryScope;

    constexpr auto prefabPath = "Library/Artifacts/model/5d4b4d6c2b6c4a6c9b91d90753df2a8d";
    constexpr auto sceneLoadPath = "Library/Artifacts/scene/0b2fe6f9f8a0418bbdb2266023d8fd5f";

    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::PrefabGraphLoad,
        std::chrono::microseconds(120),
        16u,
        prefabPath });
    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::PrefabGraphLoad,
        std::chrono::microseconds(80),
        24u,
        prefabPath });
    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::PrefabGraphLoad,
        std::chrono::microseconds(300),
        64u,
        sceneLoadPath });
    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::CacheHit,
        std::chrono::microseconds(5),
        0u,
        prefabPath });

    const auto summary = SummarizeArtifactLoadTelemetry();
    const auto prefabGraph = std::find_if(
        summary.begin(),
        summary.end(),
        [](const auto& stage)
        {
            return stage.stage == ArtifactLoadTelemetryStage::PrefabGraphLoad &&
                stage.path == prefabPath;
        });
    ASSERT_NE(prefabGraph, summary.end());
    EXPECT_EQ(prefabGraph->path, prefabPath);
    EXPECT_EQ(prefabGraph->recordCount, 2u);
    EXPECT_EQ(prefabGraph->totalElapsed, std::chrono::microseconds(200));
    EXPECT_EQ(prefabGraph->totalBytes, 40u);

    const auto sceneLoadGraph = std::find_if(
        summary.begin(),
        summary.end(),
        [](const auto& stage)
        {
            return stage.stage == ArtifactLoadTelemetryStage::PrefabGraphLoad &&
                stage.path == sceneLoadPath;
        });
    ASSERT_NE(sceneLoadGraph, summary.end());
    EXPECT_EQ(sceneLoadGraph->recordCount, 1u)
        << "Stage summaries must stay path-bucketed so scene-load and drag/drop baselines do not mix.";
    EXPECT_EQ(sceneLoadGraph->totalElapsed, std::chrono::microseconds(300));

    const auto cacheHit = std::find_if(
        summary.begin(),
        summary.end(),
        [](const auto& stage)
        {
            return stage.stage == ArtifactLoadTelemetryStage::CacheHit &&
                stage.path == prefabPath;
        });
    ASSERT_NE(cacheHit, summary.end());
    EXPECT_EQ(cacheHit->recordCount, 1u);
    EXPECT_EQ(cacheHit->totalElapsed, std::chrono::microseconds(5));
}

TEST(AssetImportPipelineTests, ArtifactLoadTelemetryStageNameCoversThumbnailLatencyStages)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;
    using NLS::Core::Assets::ArtifactLoadTelemetryStageName;

    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender),
        "ThumbnailGpuPreviewRender");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources),
        "ThumbnailGpuPreviewPrepareResources");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareMaterialResources),
        "ThumbnailGpuPreviewPrepareMaterialResources");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects),
        "ThumbnailGpuPreviewPrepareSceneObjects");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies),
        "ThumbnailGpuPreviewPumpDependencies");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshDependencies),
        "ThumbnailGpuPreviewPumpMeshDependencies");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialDependencies),
        "ThumbnailGpuPreviewPumpMaterialDependencies");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureDependencies),
        "ThumbnailGpuPreviewPumpTextureDependencies");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialPathBuild),
        "ThumbnailGpuPreviewPumpMaterialPathBuild");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialPromote),
        "ThumbnailGpuPreviewPumpMaterialPromote");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialReadyScan),
        "ThumbnailGpuPreviewPumpMaterialReadyScan");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialFutureGet),
        "ThumbnailGpuPreviewPumpMaterialFutureGet");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialRuntimeCreate),
        "ThumbnailGpuPreviewPumpMaterialRuntimeCreate");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialShaderPassResolve),
        "ThumbnailGpuPreviewPumpMaterialShaderPassResolve");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialShaderPassLoad),
        "ThumbnailGpuPreviewPumpMaterialShaderPassLoad");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewBackgroundMaterialShaderPassLoad),
        "ThumbnailGpuPreviewBackgroundMaterialShaderPassLoad");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialRegister),
        "ThumbnailGpuPreviewPumpMaterialRegister");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRecord),
        "ThumbnailGpuPreviewRecord");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewSubmit),
        "ThumbnailGpuPreviewSubmit");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewDrain),
        "ThumbnailGpuPreviewDrain");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewCleanup),
        "ThumbnailGpuPreviewCleanup");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewReadback),
        "ThumbnailGpuPreviewReadback");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPollReadback),
        "ThumbnailGpuPreviewPollReadback");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureDecode),
        "ThumbnailTextureDecode");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUploadEnqueue),
        "ThumbnailTextureUploadEnqueue");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUpload),
        "ThumbnailTextureUpload");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreate),
        "ThumbnailTextureUploadCreate");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUploadPreparePixels),
        "ThumbnailTextureUploadPreparePixels");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUploadSubmit),
        "ThumbnailTextureUploadSubmit");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreateView),
        "ThumbnailTextureUploadCreateView");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUploadPublish),
        "ThumbnailTextureUploadPublish");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUploadResolveUiId),
        "ThumbnailTextureUploadResolveUiId");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpConsumeCompleted),
        "ThumbnailTexturePumpConsumeCompleted");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadPoll),
        "ThumbnailTexturePumpPendingUploadPoll");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadConsumeResult),
        "ThumbnailTexturePumpPendingUploadConsumeResult");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadResolveUiId),
        "ThumbnailTexturePumpPendingUploadResolveUiId");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadWrapTexture),
        "ThumbnailTexturePumpPendingUploadWrapTexture");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadCachePublish),
        "ThumbnailTexturePumpPendingUploadCachePublish");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodePoll),
        "ThumbnailTexturePumpReadyDecodePoll");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodeLoad),
        "ThumbnailTexturePumpReadyDecodeLoad");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpStartDecodes),
        "ThumbnailTexturePumpStartDecodes");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpBuildResidentSet),
        "ThumbnailTexturePumpBuildResidentSet");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpSelectDecodeCandidates),
        "ThumbnailTexturePumpSelectDecodeCandidates");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePumpScheduleDecodeJobs),
        "ThumbnailTexturePumpScheduleDecodeJobs");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDraw),
        "ThumbnailUiDraw");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGridVisibleRows),
        "ThumbnailUiDrawGridVisibleRows");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemInteractions),
        "ThumbnailUiDrawGridItemInteractions");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemThumbnail),
        "ThumbnailUiDrawGridItemThumbnail");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemLabel),
        "ThumbnailUiDrawGridItemLabel");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSet),
        "ThumbnailUiDrawVisibleSet");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHash),
        "ThumbnailUiDrawVisibleSetHash");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetApply),
        "ThumbnailUiDrawVisibleSetApply");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHotCacheFlush),
        "ThumbnailUiDrawVisibleSetHotCacheFlush");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScope),
        "ThumbnailUiDrawGenerationScope");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeSelectItems),
        "ThumbnailUiDrawGenerationScopeSelectItems");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildKey),
        "ThumbnailUiDrawGenerationScopeBuildKey");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeItemKey),
        "ThumbnailUiDrawGenerationScopeItemKey");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeResultLookup),
        "ThumbnailUiDrawGenerationScopeResultLookup");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequest),
        "ThumbnailUiDrawGenerationScopeBuildRequest");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestValidate),
        "ThumbnailUiDrawGenerationScopeBuildRequestValidate");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestMetaId),
        "ThumbnailUiDrawGenerationScopeBuildRequestMetaId");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup),
        "ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity),
        "ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness),
        "ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessResolve),
        "ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessResolve");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessFileStamp),
        "ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessFileStamp");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessMetaStamp),
        "ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessMetaStamp");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness),
        "ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp),
        "ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview),
        "ThumbnailUiDrawGenerationScopeRequestPreview");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup),
        "ThumbnailServiceRequestStableLookup");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailServiceRequestCacheEvaluate),
        "ThumbnailServiceRequestCacheEvaluate");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue),
        "ThumbnailServiceRequestQueue");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTexturePump),
        "ThumbnailTexturePump");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailTextureUploadDeferred),
        "ThumbnailTextureUploadDeferred");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPump),
        "ThumbnailUiPostDrawPump");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpConsumeCompleted),
        "ThumbnailUiPostDrawPumpConsumeCompleted");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpCreatePreviewRenderer),
        "ThumbnailUiPostDrawPumpCreatePreviewRenderer");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartLightGpu),
        "ThumbnailUiPostDrawPumpStartLightGpu");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartHeavyGpu),
        "ThumbnailUiPostDrawPumpStartHeavyGpu");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartBackground),
        "ThumbnailUiPostDrawPumpStartBackground");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryBuild),
        "ThumbnailCacheEvaluateResolveEntryBuild");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentKey),
        "ThumbnailCacheEvaluateResolveEntryContainmentKey");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentStamp),
        "ThumbnailCacheEvaluateResolveEntryContainmentStamp");
    EXPECT_STREQ(
        ArtifactLoadTelemetryStageName(ArtifactLoadTelemetryStage::ThumbnailCacheEvaluateResolveEntryContainmentValidate),
        "ThumbnailCacheEvaluateResolveEntryContainmentValidate");
}

TEST(AssetImportPipelineTests, ArtifactLoadTelemetryCanBeRuntimeEnabledForReleaseDiagnostics)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;
    using NLS::Core::Assets::ClearArtifactLoadTelemetry;
    using NLS::Core::Assets::IsArtifactLoadTelemetryEnabled;
    using NLS::Core::Assets::RecordArtifactLoadTelemetry;
    using NLS::Core::Assets::SetArtifactLoadTelemetryEnabled;
    using NLS::Core::Assets::SnapshotArtifactLoadTelemetry;

    const bool previous = IsArtifactLoadTelemetryEnabled();

    SetArtifactLoadTelemetryEnabled(true);
    ClearArtifactLoadTelemetry();
    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureDecode,
        std::chrono::microseconds(15),
        64u,
        "enabled"});
    EXPECT_EQ(SnapshotArtifactLoadTelemetry().size(), 1u);

    SetArtifactLoadTelemetryEnabled(false);
    ClearArtifactLoadTelemetry();
    RecordArtifactLoadTelemetry({
        ArtifactLoadTelemetryStage::ThumbnailTextureDecode,
        std::chrono::microseconds(20),
        128u,
        "disabled"});
    EXPECT_TRUE(SnapshotArtifactLoadTelemetry().empty());

    SetArtifactLoadTelemetryEnabled(true);
    EXPECT_TRUE(SnapshotArtifactLoadTelemetry().empty())
        << "ClearArtifactLoadTelemetry must clear stale records even while telemetry is disabled.";

    SetArtifactLoadTelemetryEnabled(previous);
    ClearArtifactLoadTelemetry();
}
