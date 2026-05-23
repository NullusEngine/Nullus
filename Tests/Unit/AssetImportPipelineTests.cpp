#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Assets/AssetId.h"
#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/ArtifactWriter.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/ExternalAssetImporter.h"
#include "Guid.h"
#include "Rendering/Assets/ImportedScene.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/SceneImportPipeline.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Resources/Parsers/AssimpParser.h"
#include "Rendering/Resources/Parsers/IModelParser.h"
#include "Serialize/ObjectGraphReader.h"

namespace
{
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

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
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

void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
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
    EXPECT_EQ(
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene),
        5u);

    const auto* gltf = registry.FindImporterForPath("Assets/Models/Hero.gltf");
    ASSERT_NE(gltf, nullptr);
    EXPECT_EQ(gltf->importerId, "gltf-scene");
    EXPECT_EQ(
        gltf->importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene));

    const auto* glb = registry.FindImporterForPath("Assets/Models/Hero.GLB");
    ASSERT_NE(glb, nullptr);
    EXPECT_EQ(glb->importerId, "gltf-scene");
    EXPECT_EQ(
        glb->importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene));

    const auto* fbx = registry.FindImporterForPath("Assets/Models/Hero.fbx");
    ASSERT_NE(fbx, nullptr);
    EXPECT_EQ(fbx->importerId, "fbx-scene");
    EXPECT_EQ(
        fbx->importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene));

    const auto* obj = registry.FindImporterForPath("Assets/Models/Hero.obj");
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->importerId, "obj-scene");
    EXPECT_EQ(
        obj->importerVersion,
        NLS::Core::Assets::GetCurrentImporterVersion(NLS::Core::Assets::AssetType::ModelScene));

    EXPECT_EQ(registry.FindImporterForPath("Assets/Textures/Hero.png"), nullptr);
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
    prefab.artifactPath = "Library/Artifacts/Hero/prefab.nprefab";

    NLS::Core::Assets::ImportedArtifact mesh;
    mesh.subAssetKey = "mesh:Body";
    mesh.artifactType = NLS::Core::Assets::ArtifactType::Mesh;
    mesh.loaderId = "mesh";
    mesh.artifactPath = "Library/Artifacts/Hero/body.nmesh";

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
    const auto databasePath = root / "Library" / "ArtifactDB" / "index.tsv";

    const auto sourceId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("11111111-1111-4111-8111-111111111111"));

    NLS::Core::Assets::ArtifactManifest manifest;
    manifest.sourceAssetId = sourceId;
    manifest.importerId = "scene-model";
    manifest.importerVersion = 7u;
    manifest.targetPlatform = "editor-windows";
    manifest.primarySubAssetKey = "prefab:Hero";
    manifest.subAssets.push_back({
        sourceId,
        "prefab:Hero",
        NLS::Core::Assets::ArtifactType::Prefab,
        "prefab",
        "editor-windows",
        "Library/Artifacts/Hero/prefab.nprefab",
        "sha256:prefab"
    });
    manifest.subAssets.push_back({
        sourceId,
        "mesh:Body",
        NLS::Core::Assets::ArtifactType::Mesh,
        "mesh",
        "editor-windows",
        "Library/Artifacts/Hero/body.nmesh",
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

    NLS::Core::Assets::ArtifactDatabase loaded;
    ASSERT_TRUE(loaded.Load(databasePath));

    const auto* mesh = loaded.Find(sourceId, "mesh:Body", "editor-windows");
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->sourcePath, "Assets/Models/Hero.gltf");
    EXPECT_EQ(mesh->artifactPath, "Library/Artifacts/Hero/body.nmesh");
    EXPECT_EQ(mesh->loaderId, "mesh");
    EXPECT_EQ(mesh->status, NLS::Core::Assets::ArtifactRecordStatus::UpToDate);
    EXPECT_EQ(mesh->importerVersion, 7u);
    EXPECT_EQ(mesh->dependencyCount, 1u);

    const auto sourceRecords = loaded.FindBySource(sourceId);
    ASSERT_EQ(sourceRecords.size(), 2u);
    EXPECT_EQ(loaded.GetStats().upToDateRecords, 2u);

    loaded.RemoveSource(sourceId);
    EXPECT_TRUE(loaded.FindBySource(sourceId).empty());

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
        manifest.importerVersion = 7u;
        manifest.targetPlatform = "editor";
        manifest.primarySubAssetKey = "mesh:Body";
        manifest.subAssets.push_back({
            sourceId,
            "mesh:Body",
            ArtifactType::Mesh,
            "mesh",
            "editor",
            "Library/Artifacts/" + sourceId.ToString() + "/body.nmesh",
            "sha256:" + std::to_string(index)
        });

        database.UpsertManifest(
            manifest,
            "Assets/Models/Hero" + std::to_string(index) + ".gltf",
            ArtifactRecordStatus::UpToDate);

        ASSERT_NE(database.Find(sourceId, "mesh:Body", "editor"), nullptr);
    }

    EXPECT_EQ(database.GetStats().totalRecords, manifestCount);
    EXPECT_EQ(database.GetIndexRebuildCountForTesting(), 0u);
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
    request.importerVersion = 7u;
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab.nprefab",
        std::vector<uint8_t>{'p', 'r', 'e', 'f', 'a', 'b'}
    });
    request.artifacts.push_back({
        "mesh:HeroBody",
        ArtifactType::Mesh,
        "mesh",
        "Hero/body.nmesh",
        std::vector<uint8_t>{'m', 'e', 's', 'h'}
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);

    ASSERT_TRUE(result.committed);
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(result.manifest.sourceAssetId, request.sourceAssetId);
    EXPECT_EQ(result.manifest.importerId, "scene-model");
    EXPECT_EQ(result.manifest.importerVersion, 7u);
    EXPECT_EQ(result.manifest.primarySubAssetKey, "prefab:Hero");
    ASSERT_EQ(result.manifest.subAssets.size(), 2u);

    const auto* prefab = result.manifest.FindSubAsset("prefab:Hero");
    ASSERT_NE(prefab, nullptr);
    EXPECT_EQ(prefab->artifactType, ArtifactType::Prefab);
    EXPECT_EQ(prefab->loaderId, "prefab");
    const auto prefabBytes = ReadBinaryFile(prefab->artifactPath);
    const auto prefabContainer = ReadNativeArtifactContainer(prefabBytes, ArtifactType::Prefab, 1u);
    ASSERT_TRUE(prefabContainer.has_value());
    EXPECT_EQ(prefabContainer->metadata.sourceAssetId, request.sourceAssetId);
    EXPECT_EQ(prefabContainer->metadata.subAssetKey, "prefab:Hero");
    EXPECT_EQ(prefabContainer->metadata.importerId, "scene-model");
    EXPECT_EQ(prefabContainer->metadata.importerVersion, 7u);
    EXPECT_EQ(prefabContainer->metadata.targetPlatform, "editor-windows");
    EXPECT_EQ(std::string(prefabContainer->payload.begin(), prefabContainer->payload.end()), "prefab");

    const auto* mesh = result.manifest.FindSubAsset("mesh:HeroBody");
    ASSERT_NE(mesh, nullptr);
    const auto meshBytes = ReadBinaryFile(mesh->artifactPath);
    const auto meshContainer = ReadNativeArtifactContainer(meshBytes, ArtifactType::Mesh, 3u);
    ASSERT_TRUE(meshContainer.has_value());
    EXPECT_EQ(std::string(meshContainer->payload.begin(), meshContainer->payload.end()), "mesh");
    EXPECT_FALSE(std::filesystem::exists(stagingRoot / "Hero" / "prefab.nprefab"));

    std::filesystem::remove_all(root);
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
        (root / "Committed" / "Previous" / "prefab.nprefab").string(),
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
        "../escape.nprefab",
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
    EXPECT_FALSE(std::filesystem::exists(root / "escape.nprefab"));

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
        "C:escape.nprefab",
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

    WriteTextFile(commitRoot / "Hero" / "prefab.nprefab", "old-prefab");
    std::filesystem::create_directories(commitRoot / "Hero" / "bad.nmesh");

    ArtifactManifest previous;
    previous.sourceAssetId = AssetId(NLS::Guid::Parse("abababab-abab-4aba-8bab-abababababab"));
    previous.primarySubAssetKey = "prefab:Previous";
    previous.subAssets.push_back({
        previous.sourceAssetId,
        "prefab:Previous",
        ArtifactType::Prefab,
        "prefab",
        (commitRoot / "Hero" / "prefab.nprefab").string(),
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
        "Hero/prefab.nprefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });
    request.artifacts.push_back({
        "mesh:Bad",
        ArtifactType::Mesh,
        "mesh",
        "Hero/bad.nmesh",
        std::vector<uint8_t>{'b', 'a', 'd'}
    });

    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto result = writer.WriteAndCommit(request, &previous);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-commit-failed");
    EXPECT_EQ(result.manifest.primarySubAssetKey, previous.primarySubAssetKey);
    EXPECT_EQ(ReadTextFile(commitRoot / "Hero" / "prefab.nprefab"), "old-prefab");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRollsBackCommittedFilesWhenCancelledDuringCommit)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Staging";
    const auto commitRoot = root / "Committed";

    WriteTextFile(commitRoot / "Hero" / "prefab.nprefab", "old-prefab");
    WriteTextFile(commitRoot / "Hero" / "body.nmesh", "old-mesh");

    ArtifactManifest previous;
    previous.sourceAssetId = AssetId(NLS::Guid::Parse("acacacac-acac-4aca-8cac-acacacacacac"));
    previous.primarySubAssetKey = "prefab:Previous";
    previous.subAssets.push_back({
        previous.sourceAssetId,
        "prefab:Previous",
        ArtifactType::Prefab,
        "prefab",
        (commitRoot / "Hero" / "prefab.nprefab").string(),
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
        "Hero/prefab.nprefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });
    request.artifacts.push_back({
        "mesh:Body",
        ArtifactType::Mesh,
        "mesh",
        "Hero/body.nmesh",
        std::vector<uint8_t>{'n', 'e', 'w', '-', 'm', 'e', 's', 'h'}
    });

    CancelAfterChecks cancellation(7u);
    ArtifactWriter writer(stagingRoot, commitRoot);
    const auto result = writer.WriteAndCommit(request, &previous, &cancellation);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-write-cancelled");
    EXPECT_EQ(result.manifest.primarySubAssetKey, previous.primarySubAssetKey);
    EXPECT_EQ(ReadTextFile(commitRoot / "Hero" / "prefab.nprefab"), "old-prefab");
    EXPECT_EQ(ReadTextFile(commitRoot / "Hero" / "body.nmesh"), "old-mesh");
    EXPECT_FALSE(std::filesystem::exists(stagingRoot));

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRejectsUnsafeRootsBeforeDeletingCommittedArtifacts)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto committedRoot = root / "Committed";
    WriteTextFile(committedRoot / "Hero" / "prefab.nprefab", "committed");

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("adadadad-adad-4ada-8dad-adadadadadad"));
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab.nprefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });

    ArtifactWriter writer(committedRoot, committedRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-root-unsafe");
    EXPECT_EQ(ReadTextFile(committedRoot / "Hero" / "prefab.nprefab"), "committed");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterRejectsRollbackRootThatWouldDeleteCommittedArtifacts)
{
    using namespace NLS::Core::Assets;

    const auto root = MakeImportTestRoot();
    const auto stagingRoot = root / "Artifacts";
    const auto committedRoot = root / "Artifacts.rollback";
    WriteTextFile(committedRoot / "Hero" / "prefab.nprefab", "committed");

    ArtifactWriteRequest request;
    request.sourceAssetId = AssetId(NLS::Guid::Parse("aeaeaeae-aeae-4aea-8eae-aeaeaeaeaeae"));
    request.importerId = "scene-model";
    request.targetPlatform = "editor-windows";
    request.primarySubAssetKey = "prefab:Hero";
    request.artifacts.push_back({
        "prefab:Hero",
        ArtifactType::Prefab,
        "prefab",
        "Hero/prefab.nprefab",
        std::vector<uint8_t>{'n', 'e', 'w'}
    });

    ArtifactWriter writer(stagingRoot, committedRoot);
    const auto result = writer.WriteAndCommit(request, nullptr);

    EXPECT_FALSE(result.committed);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, "artifact-root-unsafe");
    EXPECT_EQ(ReadTextFile(committedRoot / "Hero" / "prefab.nprefab"), "committed");

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ArtifactWriterChecksPayloadStreamAfterWriteAndClose)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Core/Assets/ArtifactWriter.cpp");

    const auto stagedWrite = source.substr(source.find("output.write("));
    ASSERT_FALSE(stagedWrite.empty());
    EXPECT_NE(stagedWrite.find("output.close()"), std::string::npos);
    EXPECT_NE(stagedWrite.find("artifact-write-failed"), std::string::npos);
    EXPECT_NE(stagedWrite.find("!output"), std::string::npos);
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

    ASSERT_EQ(scene.textures.size(), 2u);
    EXPECT_EQ(scene.textures[0].sourceKey, "parser/texture/0");
    EXPECT_EQ(scene.textures[0].uri, "../Textures/HeroDiffuse.png");
    EXPECT_EQ(scene.textures[1].sourceKey, "parser/texture/1");
    EXPECT_EQ(scene.textures[1].uri, "../Textures/HeroNormal.png");

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

    EXPECT_TRUE(Contains(externalDependencies, "../Textures/HeroDiffuse.png"));
    EXPECT_TRUE(Contains(externalDependencies, "../Textures/HeroNormal.png"));

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

    ASSERT_TRUE(result.imported);
    const auto* leftMesh = result.manifest.FindSubAsset("mesh:mesh/0");
    ASSERT_NE(leftMesh, nullptr);
    const auto* rightMesh = result.manifest.FindSubAsset("mesh:mesh/1");
    ASSERT_NE(rightMesh, nullptr);

    const auto leftArtifact = NLS::Render::Assets::DeserializeMeshArtifact(ReadBinaryFile(leftMesh->artifactPath));
    ASSERT_TRUE(leftArtifact.has_value());
    ASSERT_EQ(leftArtifact->vertices.size(), 3u);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[0].position[0], 0.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[0].position[1], 0.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[1].position[1], 0.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[1].texCoords[0], 1.0f);
    EXPECT_FLOAT_EQ(leftArtifact->vertices[2].texCoords[1], 1.0f);

    const auto rightArtifact = NLS::Render::Assets::DeserializeMeshArtifact(ReadBinaryFile(rightMesh->artifactPath));
    ASSERT_TRUE(rightArtifact.has_value());
    ASSERT_EQ(rightArtifact->vertices.size(), 3u);
    EXPECT_FLOAT_EQ(rightArtifact->vertices[0].position[0], 10.0f);
    EXPECT_FLOAT_EQ(rightArtifact->vertices[1].position[0], 11.0f);
    EXPECT_FLOAT_EQ(rightArtifact->vertices[0].texCoords[0], 0.25f);
    EXPECT_FLOAT_EQ(rightArtifact->vertices[2].texCoords[1], 0.75f);

    const auto* prefabArtifact = result.manifest.FindSubAsset("prefab:OutOfOrder");
    ASSERT_NE(prefabArtifact, nullptr);
    const auto prefabPayload = ReadArtifactPayloadText(
        prefabArtifact->artifactPath,
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

TEST(AssetImportPipelineTests, ExternalObjModelImportWritesMaterialTextureUniforms)
{
    const auto root = MakeImportTestRoot();
    const auto sourcePath = root / "Assets" / "Models" / "Hero.obj";
    WriteTextFile(root / "Assets" / "Textures" / "HeroDiffuse.png", "texture-bytes");
    WriteTextFile(root / "Assets" / "Textures" / "HeroNormal.png", "texture-bytes");
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
    const auto* materialArtifact = result.manifest.FindSubAsset("material:parser/material/1");
    ASSERT_NE(materialArtifact, nullptr);

    const auto payload = ReadArtifactPayloadText(
        materialArtifact->artifactPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    const auto* diffuseTextureArtifact = result.manifest.FindSubAsset("texture:parser/texture/0");
    ASSERT_NE(diffuseTextureArtifact, nullptr);
    const auto textureArtifactPath = std::filesystem::path(diffuseTextureArtifact->artifactPath)
        .lexically_normal();
    const auto textureResourcePath = textureArtifactPath.lexically_relative(root).generic_string();
    EXPECT_NE(payload.find("u_AlbedoMap"), std::string::npos);
    EXPECT_NE(payload.find(textureResourcePath), std::string::npos);
    EXPECT_EQ(payload.find(textureArtifactPath.generic_string()), std::string::npos);
    EXPECT_NE(payload.find(".ntex"), std::string::npos);
    EXPECT_EQ(payload.find("Assets/Textures/HeroDiffuse.png"), std::string::npos);
    EXPECT_NE(payload.find("u_NormalMap"), std::string::npos);
    EXPECT_NE(payload.find(".ntex"), std::string::npos);
    EXPECT_NE(payload.find("u_EnableNormalMapping\" type=\"float\" value=\"1.000000\""), std::string::npos);
    EXPECT_NE(
        payload.find("<uniform name=\"u_Albedo\" type=\"vec4\" value=\"0.800000 0.700000 0.600000 0.650000\"/>"),
        std::string::npos);

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
    const auto* textureArtifact = result.manifest.FindSubAsset("texture:parser/texture/0");
    ASSERT_NE(textureArtifact, nullptr);
    EXPECT_EQ(textureArtifact->artifactType, NLS::Core::Assets::ArtifactType::Texture);
    EXPECT_EQ(textureArtifact->loaderId, "texture");

    const auto payload = ReadArtifactPayloadBytes(
        textureArtifact->artifactPath,
        NLS::Core::Assets::ArtifactType::Texture,
        3u);
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
        ReadBinaryFile(textureArtifact->artifactPath));
    ASSERT_TRUE(nativeTexture.has_value());
    EXPECT_EQ(nativeTexture->format, NLS::Render::RHI::TextureFormat::RGBA8);
    ASSERT_FALSE(nativeTexture->mips.empty());
    EXPECT_EQ(nativeTexture->mips.front().level, 0u);

    std::filesystem::remove_all(root);
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
    const auto mesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadBinaryFile(meshArtifact->artifactPath));
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
    WriteTextFile(committedRoot / "old" / "prefab.nprefab", "previous");

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
        (committedRoot / "old" / "prefab.nprefab").string(),
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
    EXPECT_EQ(ReadTextFile(committedRoot / "old" / "prefab.nprefab"), "previous");
    EXPECT_FALSE(std::filesystem::exists(root / "Staging"));

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

    const auto baseColor = NLS::Render::Assets::DeserializeTextureArtifact(ReadBinaryFile(baseColorArtifact->artifactPath));
    const auto normal = NLS::Render::Assets::DeserializeTextureArtifact(ReadBinaryFile(normalArtifact->artifactPath));
    ASSERT_TRUE(baseColor.has_value());
    ASSERT_TRUE(normal.has_value());
    EXPECT_EQ(baseColor->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Srgb);
    EXPECT_EQ(normal->colorSpace, NLS::Render::Assets::TextureArtifactColorSpace::Linear);

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
    const auto* textureArtifact = result.manifest.FindSubAsset("texture:image/0");
    ASSERT_NE(textureArtifact, nullptr);
    const auto texturePayload = ReadBinaryFile(textureArtifact->artifactPath);
    EXPECT_TRUE(NLS::Render::Assets::IsNativeTextureArtifact(texturePayload));
    EXPECT_TRUE(NLS::Render::Assets::DeserializeTextureArtifact(texturePayload).has_value());

    const auto* materialArtifact = result.manifest.FindSubAsset("material:material/0");
    ASSERT_NE(materialArtifact, nullptr);
    const auto materialPayload = ReadArtifactPayloadText(
        materialArtifact->artifactPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    EXPECT_NE(materialPayload.find("Library/Artifacts/"), std::string::npos);
    EXPECT_NE(materialPayload.find(".ntex"), std::string::npos);
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
    const auto* textureArtifact = result.manifest.FindSubAsset("texture:image/0");
    ASSERT_NE(textureArtifact, nullptr);
    const auto texturePayload = ReadBinaryFile(textureArtifact->artifactPath);
    EXPECT_TRUE(NLS::Render::Assets::IsNativeTextureArtifact(texturePayload));
    EXPECT_TRUE(NLS::Render::Assets::DeserializeTextureArtifact(texturePayload).has_value());

    std::filesystem::remove_all(root);
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
    const auto mesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadBinaryFile(meshArtifact->artifactPath));
    ASSERT_TRUE(mesh.has_value());
    ASSERT_EQ(mesh->vertices.size(), 3u);
    ASSERT_EQ(mesh->indices.size(), 3u);
    EXPECT_FLOAT_EQ(mesh->vertices[1].position[0], 1.0f);
    EXPECT_FLOAT_EQ(mesh->vertices[2].position[1], 1.0f);
    EXPECT_EQ(mesh->indices[2], 2u);

    std::filesystem::remove_all(root);
}

TEST(AssetImportPipelineTests, ExternalModelImportDoesNotMergeFollowingGltfMeshesWhenFirstMeshHasMultiplePrimitives)
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
                        { "attributes": { "POSITION": 0 }, "indices": 1 },
                        { "attributes": { "POSITION": 2 }, "indices": 3 }
                    ]
                },
                {
                    "name": "SecondMesh",
                    "primitives": [
                        { "attributes": { "POSITION": 4 }, "indices": 5 }
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
    const auto* splitMeshArtifact = result.manifest.FindSubAsset("mesh:mesh/0");
    ASSERT_NE(splitMeshArtifact, nullptr);
    const auto splitMesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadBinaryFile(splitMeshArtifact->artifactPath));
    ASSERT_TRUE(splitMesh.has_value());
    ASSERT_EQ(splitMesh->vertices.size(), 6u);
    EXPECT_FLOAT_EQ(splitMesh->vertices[0].position[0], 0.0f);
    EXPECT_FLOAT_EQ(splitMesh->vertices[3].position[0], 10.0f);

    const auto* secondMeshArtifact = result.manifest.FindSubAsset("mesh:mesh/1");
    ASSERT_NE(secondMeshArtifact, nullptr);
    const auto secondMesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadBinaryFile(secondMeshArtifact->artifactPath));
    ASSERT_TRUE(secondMesh.has_value());
    ASSERT_EQ(secondMesh->vertices.size(), 3u);
    EXPECT_FLOAT_EQ(secondMesh->vertices[0].position[0], 20.0f);

    std::filesystem::remove_all(root);
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
    const auto mesh = NLS::Render::Assets::DeserializeMeshArtifact(ReadBinaryFile(meshArtifact->artifactPath));
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
