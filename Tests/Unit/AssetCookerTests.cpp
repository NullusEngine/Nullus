#include <gtest/gtest.h>

#include "CookTypes.h"

#include "Assets/ArtifactDatabase.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetMeta.h"
#include "Assets/NativeArtifactContainer.h"
#include "Guid.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
using NLS::Cook::AssetCooker;
using NLS::Cook::CookOptions;
using NLS::Core::Assets::ArtifactDatabase;
using NLS::Core::Assets::ArtifactRecordStatus;
using NLS::Core::Assets::ArtifactType;
using NLS::Core::Assets::AssetId;
using NLS::Core::Assets::AssetMeta;
using NLS::Core::Assets::ReadNativeArtifactContainer;

std::filesystem::path MakeTemporaryDirectory(const std::string& name)
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("nullus-cook-" + name + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    return directory;
}

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

const std::array<uint8_t, 70u> kTinyPng {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,
    0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,
    0x00,0x1F,0x15,0xC4,0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,
    0xDA,0x63,0x64,0xF8,0xCF,0x50,0x0F,0x00,0x03,0x86,0x01,0x80,0x5A,0x34,
    0x7D,0x6B,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82
};

struct CookFixture
{
    std::filesystem::path root;
    std::filesystem::path assets;
    std::filesystem::path database;
    std::filesystem::path artifacts;
    std::filesystem::path texturePath;
    std::filesystem::path prefabPath;
    std::filesystem::path materialPath;
    std::filesystem::path uiPath;
};

CookFixture MakeCookFixture(const std::string& name)
{
    CookFixture fixture;
    fixture.root = MakeTemporaryDirectory(name);
    fixture.assets = fixture.root / "Assets";
    fixture.database = fixture.root / "Library" / "ArtifactDB";
    fixture.artifacts = fixture.root / "Library" / "Artifacts";
    std::filesystem::create_directories(fixture.assets);
    std::filesystem::create_directories(fixture.database);

    fixture.texturePath = fixture.assets / "Textures" / "White.png";
    std::filesystem::create_directories(fixture.texturePath.parent_path());
    {
        std::ofstream texture(fixture.texturePath, std::ios::binary | std::ios::trunc);
        texture.write(reinterpret_cast<const char*>(kTinyPng.data()), static_cast<std::streamsize>(kTinyPng.size()));
    }
    WriteFile(
        fixture.texturePath.string() + ".meta",
        "GUID=" + NLS::Guid::NewDeterministic("cook-texture-" + name).ToString() +
            "\nASSET_TYPE=texture\nIMPORTER_ID=texture\nIMPORTER_VERSION=1\nTEXTURE_SRGB=true\nTEXTURE_MIPMAP_ENABLED=true\n");

    fixture.prefabPath = fixture.assets / "Prefabs" / "Prop.prefab";
    WriteFile(
        fixture.prefabPath,
        "{\n"
        "  \"documentId\": \"" + NLS::Guid::NewDeterministic("cook-prefab-doc-" + name).ToString() + "\",\n"
        "  \"root\": \"" + NLS::Guid::NewDeterministic("cook-prefab-root-" + name).ToString() + "\",\n"
        "  \"format\": \"Nullus.ObjectGraph.Prefab\",\n"
        "  \"objects\": []\n"
        "}\n");
    WriteFile(
        fixture.prefabPath.string() + ".meta",
        "GUID=" + NLS::Guid::NewDeterministic("cook-prefab-" + name).ToString() +
            "\nASSET_TYPE=prefab\nIMPORTER_ID=prefab\nIMPORTER_VERSION=1\n");

    fixture.materialPath = fixture.assets / "Materials" / "Paint.mat";
    {
        NLS::Core::Assets::NativeArtifactMetadata metadata;
        metadata.artifactType = NLS::Core::Assets::ArtifactType::Material;
        metadata.schemaName = "material";
        metadata.schemaVersion = 1u;
        metadata.displayName = "Paint";
        const std::string payload =
            "shaderLabMaterialVersion=1\n"
            "shader=Assets/Engine/Shaders/ShaderLab/StandardPBR.shader\n"
            "surfaceMode=Opaque\n";
        const auto bytes = NLS::Core::Assets::WriteNativeArtifactContainer(
            metadata,
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(payload.data()),
                payload.size()));
        std::filesystem::create_directories(fixture.materialPath.parent_path());
        std::ofstream material(fixture.materialPath, std::ios::binary | std::ios::trunc);
        material.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    WriteFile(
        fixture.materialPath.string() + ".meta",
        "GUID=" + NLS::Guid::NewDeterministic("cook-material-" + name).ToString() +
            "\nASSET_TYPE=material\nIMPORTER_ID=material\nIMPORTER_VERSION=1\n");

    fixture.uiPath = fixture.assets / "UI" / "Hud.ui";
    WriteFile(fixture.uiPath, "{\"format\":\"Nullus.UI.Document\",\"root\":{}}");
    WriteFile(
        fixture.uiPath.string() + ".meta",
        "GUID=" + NLS::Guid::NewDeterministic("cook-ui-" + name).ToString() +
            "\nASSET_TYPE=ui-document\nIMPORTER_ID=ui-document\nIMPORTER_VERSION=1\n");
    return fixture;
}

CookOptions MakeOptions(const CookFixture& fixture)
{
    CookOptions options;
    options.projectRoot = fixture.root;
    options.assetsRoot = fixture.assets;
    options.artifactDatabasePath = fixture.database;
    options.artifactRoot = fixture.artifacts;
    options.stagingRoot = fixture.root / "Library" / "ArtifactStaging" / "Cook";
    options.target.artifactPlatform = "win64";
    options.target.platform = "windows";
    options.target.architecture = "x64";
    options.target.graphicsBackend = "dx12";
    return options;
}
} // namespace

TEST(AssetCookerTests, CooksTexturesPrefabsAndUiArtifacts)
{
    const auto fixture = MakeCookFixture("supported");
    const auto summary = AssetCooker::CookProject(MakeOptions(fixture));
    ASSERT_TRUE(summary.Succeeded()) << summary.FormatSummary();
    EXPECT_EQ(summary.total, 4u);
    EXPECT_EQ(summary.cooked, 4u);
    EXPECT_EQ(summary.failed, 0u);
    EXPECT_EQ(summary.unsupported, 0u);

    ArtifactDatabase database;
    ASSERT_TRUE(database.Load(fixture.database));
    EXPECT_EQ(database.GetRecords().size(), 4u);

    const auto findRecord = [&database](const std::filesystem::path& source)
    {
        const auto meta = AssetMeta::Load(source.string() + ".meta");
        if (!meta.has_value())
            return static_cast<const NLS::Core::Assets::ArtifactDatabaseRecord*>(nullptr);
        for (const auto* record : database.FindBySource(meta->id))
        {
            if (record != nullptr && record->targetPlatform == "win64" &&
                record->status == ArtifactRecordStatus::UpToDate)
            {
                return record;
            }
        }
        return static_cast<const NLS::Core::Assets::ArtifactDatabaseRecord*>(nullptr);
    };

    const auto* texture = findRecord(fixture.texturePath);
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->artifactType, ArtifactType::Texture);
    EXPECT_EQ(texture->loaderId, "texture");
    EXPECT_FALSE(texture->artifactPath.empty());
    {
        std::ifstream input(fixture.root / texture->artifactPath, std::ios::binary);
        ASSERT_TRUE(input.good());
        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        const auto container = ReadNativeArtifactContainer(bytes, ArtifactType::Texture, 4u);
        ASSERT_TRUE(container.has_value());
        EXPECT_GT(container->payload.size(), 0u);
    }
    {
        const auto textureArtifact = NLS::Render::Assets::LoadTextureArtifact(
            fixture.root / texture->artifactPath);
        ASSERT_TRUE(textureArtifact.has_value());
#if defined(_WIN32)
        EXPECT_NE(textureArtifact->format, NLS::Render::RHI::TextureFormat::RGBA8);
        EXPECT_EQ(textureArtifact->encoderId, "directxtex-bc");
#else
        EXPECT_EQ(textureArtifact->format, NLS::Render::RHI::TextureFormat::RGBA8);
#endif
    }

    const auto* prefab = findRecord(fixture.prefabPath);
    ASSERT_NE(prefab, nullptr);
    EXPECT_EQ(prefab->artifactType, ArtifactType::Prefab);
    EXPECT_EQ(prefab->loaderId, "prefab");

    const auto* ui = findRecord(fixture.uiPath);
    ASSERT_NE(ui, nullptr);
    EXPECT_EQ(ui->artifactType, ArtifactType::UIDocument);
    EXPECT_EQ(ui->loaderId, "ui-document");

    const auto* material = findRecord(fixture.materialPath);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->artifactType, ArtifactType::Material);
    EXPECT_EQ(material->loaderId, "material");
    {
        std::ifstream input(fixture.root / material->artifactPath, std::ios::binary);
        ASSERT_TRUE(input.good());
        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        const auto container = ReadNativeArtifactContainer(bytes, ArtifactType::Material, 1u);
        ASSERT_TRUE(container.has_value());
        const std::string payload(container->payload.begin(), container->payload.end());
        EXPECT_EQ(payload.rfind("shaderLabMaterialVersion=1", 0u), 0u);
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}

TEST(AssetCookerTests, CooksObjModelIntoMeshMaterialAndPrefab)
{
    auto fixture = MakeCookFixture("model");
    const auto modelPath = fixture.assets / "Models" / "Triangle.obj";
    WriteFile(
        modelPath,
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0 1\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n");
    WriteFile(
        modelPath.string() + ".meta",
        "GUID=" + NLS::Guid::NewDeterministic("cook-model-" + std::string("obj")).ToString() +
            "\nASSET_TYPE=model-scene\nIMPORTER_ID=model\nIMPORTER_VERSION=1\n");

    const auto summary = AssetCooker::CookProject(MakeOptions(fixture));
    ASSERT_TRUE(summary.Succeeded()) << summary.FormatSummary();
    EXPECT_EQ(summary.cooked, 5u);

    ArtifactDatabase database;
    ASSERT_TRUE(database.Load(fixture.database));
    const auto meta = AssetMeta::Load(modelPath.string() + ".meta");
    ASSERT_TRUE(meta.has_value());
    bool hasMesh = false;
    bool hasMaterial = false;
    bool hasPrefab = false;
    std::string meshArtifactPath;
    for (const auto* record : database.FindBySource(meta->id))
    {
        if (record == nullptr || record->targetPlatform != "win64" ||
            record->status != ArtifactRecordStatus::UpToDate)
        {
            continue;
        }
        if (record->artifactType == ArtifactType::Mesh && meshArtifactPath.empty())
            meshArtifactPath = record->artifactPath;
        hasMesh = hasMesh || record->artifactType == ArtifactType::Mesh;
        hasMaterial = hasMaterial || record->artifactType == ArtifactType::Material;
        hasPrefab = hasPrefab ||
            (record->artifactType == ArtifactType::Prefab &&
                record->primarySubAssetKey == record->subAssetKey);
        EXPECT_FALSE(record->artifactPath.empty());
    }
    EXPECT_TRUE(hasMesh);
    EXPECT_TRUE(hasMaterial);
    EXPECT_TRUE(hasPrefab);

    ASSERT_FALSE(meshArtifactPath.empty());
    const auto mesh = NLS::Render::Assets::LoadMeshArtifact(
        fixture.root / meshArtifactPath);
    ASSERT_TRUE(mesh.has_value());
    EXPECT_EQ(mesh->vertices.size(), 3u);
    EXPECT_EQ(mesh->indices.size(), 3u);
    for (const auto& vertex : mesh->vertices)
    {
        EXPECT_NEAR(vertex.normals[0], 0.0f, 0.01f);
        EXPECT_NEAR(vertex.normals[1], 0.0f, 0.01f);
        EXPECT_NEAR(vertex.normals[2], 1.0f, 0.01f);
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}

TEST(AssetCookerTests, CooksShaderLabIntoShaderArtifact)
{
    const auto fixture = MakeCookFixture("shader");
    const auto shaderPath = fixture.assets / "Shaders" / "Simple.shader";
    WriteFile(
        shaderPath,
        "Shader \"Test/Simple\"\n"
        "{\n"
        "    Properties { _Color(\"Color\", Color) = (1, 1, 1, 1) }\n"
        "    SubShader\n"
        "    {\n"
        "        Pass\n"
        "        {\n"
        "            Name \"Forward\"\n"
        "            Tags { \"LightMode\" = \"Forward\" }\n"
        "            HLSLPROGRAM\n"
        "            #pragma vertex VSMain\n"
        "            #pragma fragment PSMain\n"
        "            float4 _Color;\n"
        "            struct Varyings { float4 positionCS : SV_POSITION; };\n"
        "            Varyings VSMain(float3 positionOS : POSITION)\n"
        "            { Varyings output; output.positionCS = float4(positionOS, 1); return output; }\n"
        "            float4 PSMain(Varyings input) : SV_Target0 { return _Color; }\n"
        "            ENDHLSL\n"
        "        }\n"
        "    }\n"
        "}\n");
    WriteFile(
        shaderPath.string() + ".meta",
        "GUID=" + NLS::Guid::NewDeterministic("cook-shader-simple").ToString() +
            "\nASSET_TYPE=shader\nIMPORTER_ID=shader\nIMPORTER_VERSION=1\n");

    auto options = MakeOptions(fixture);
    options.pathFilter = "Simple.shader";
    const auto summary = AssetCooker::CookProject(options);
    ASSERT_TRUE(summary.Succeeded()) << summary.FormatSummary();
    ASSERT_EQ(summary.cooked, 1u) << summary.FormatSummary();

    ArtifactDatabase database;
    ASSERT_TRUE(database.Load(fixture.database));
    const auto meta = AssetMeta::Load(shaderPath.string() + ".meta");
    ASSERT_TRUE(meta.has_value());
    const NLS::Core::Assets::ArtifactDatabaseRecord* shaderRecord = nullptr;
    for (const auto* candidate : database.FindBySource(meta->id))
    {
        if (candidate != nullptr &&
            candidate->artifactType == ArtifactType::Shader &&
            candidate->targetPlatform == "win64" &&
            candidate->primarySubAssetKey == candidate->subAssetKey)
        {
            shaderRecord = candidate;
        }
    }
    ASSERT_NE(shaderRecord, nullptr);
    EXPECT_EQ(shaderRecord->subAssetKey, "shader:Simple");

    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(
        fixture.root / shaderRecord->artifactPath);
    ASSERT_TRUE(artifact.has_value());
    EXPECT_EQ(artifact->targetPlatform, "win64");
    EXPECT_FALSE(artifact->stages.empty());

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}

TEST(AssetCookerTests, CooksEmbeddedGltfTextureIntoMaterialSlot)
{
    const auto fixture = MakeCookFixture("embedded-texture");
    const auto modelPath = fixture.assets / "Models" / "Embedded.gltf";
    const std::string bufferBase64 = "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA";
    const std::string imageBase64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lK3Q6wAAAABJRU5ErkJggg==";
    WriteFile(
        modelPath,
        "{\n"
        "  \"asset\": {\"version\": \"2.0\"},\n"
        "  \"buffers\": [{\"uri\": \"data:application/octet-stream;base64," + bufferBase64 + "\", \"byteLength\": 42}],\n"
        "  \"bufferViews\": [\n"
        "    {\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962},\n"
        "    {\"buffer\":0,\"byteOffset\":36,\"byteLength\":6,\"target\":34963}\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    {\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},\n"
        "    {\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}\n"
        "  ],\n"
        "  \"images\": [{\"uri\": \"data:image/png;base64," + imageBase64 + "\"}],\n"
        "  \"textures\": [{\"source\":0}],\n"
        "  \"materials\": [{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},"
        "\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0,\"roughnessFactor\":1}}],\n"
        "  \"meshes\": [{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]}],\n"
        "  \"nodes\": [{\"mesh\":0}],\n"
        "  \"scenes\": [{\"nodes\":[0]}],\n"
        "  \"scene\": 0\n"
        "}\n");
    WriteFile(
        modelPath.string() + ".meta",
        "GUID=" + NLS::Guid::NewDeterministic("cook-embedded-gltf").ToString() +
            "\nASSET_TYPE=model-scene\nIMPORTER_ID=model\nIMPORTER_VERSION=1\n");

    auto options = MakeOptions(fixture);
    options.pathFilter = "Embedded.gltf";
    const auto summary = AssetCooker::CookProject(options);
    ASSERT_TRUE(summary.Succeeded()) << summary.FormatSummary();
    ASSERT_EQ(summary.cooked, 1u) << summary.FormatSummary();

    ArtifactDatabase database;
    ASSERT_TRUE(database.Load(fixture.database));
    bool hasEmbeddedTexture = false;
    bool hasMaterialTextureSlot = false;
    for (const auto& record : database.GetRecords())
    {
        if (record.artifactType == ArtifactType::Texture &&
            record.sourcePath.find("Assets/Embedded/") != std::string::npos)
        {
            hasEmbeddedTexture = true;
            const auto texture = NLS::Render::Assets::LoadTextureArtifact(
                fixture.root / record.artifactPath);
            ASSERT_TRUE(texture.has_value());
            EXPECT_EQ(texture->width, 1u);
            EXPECT_EQ(texture->height, 1u);
        }
        if (record.artifactType == ArtifactType::Material)
        {
            std::ifstream input(fixture.root / record.artifactPath, std::ios::binary);
            const std::string payload(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());
            if (payload.find("textureSlot _BaseMap") != std::string::npos)
                hasMaterialTextureSlot = true;
        }
    }
    EXPECT_TRUE(hasEmbeddedTexture);
    EXPECT_TRUE(hasMaterialTextureSlot);

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}

TEST(AssetCookerTests, SecondRunSkipsUpToDateAssets)
{
    const auto fixture = MakeCookFixture("incremental");
    const auto options = MakeOptions(fixture);
    const auto first = AssetCooker::CookProject(options);
    ASSERT_TRUE(first.Succeeded()) << first.FormatSummary();
    EXPECT_EQ(first.cooked, 4u);

    const auto second = AssetCooker::CookProject(options);
    ASSERT_TRUE(second.Succeeded()) << second.FormatSummary();
    EXPECT_EQ(second.cooked, 0u);
    EXPECT_EQ(second.skipped, 4u);

    // A forced run bypasses the cache.
    CookOptions forced = options;
    forced.force = true;
    const auto third = AssetCooker::CookProject(forced);
    ASSERT_TRUE(third.Succeeded()) << third.FormatSummary();
    EXPECT_EQ(third.cooked, 4u);

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}

TEST(AssetCookerTests, UnsupportedAssetsAreWarnings)
{
    const auto fixture = MakeCookFixture("unsupported");
    const auto scriptPath = fixture.assets / "Scripts" / "Test.cs";
    WriteFile(scriptPath, "// script");
    WriteFile(
        scriptPath.string() + ".meta",
        "GUID=" + NLS::Guid::NewDeterministic("cook-script").ToString() +
            "\nASSET_TYPE=script\nIMPORTER_ID=script\nIMPORTER_VERSION=1\n");

    const auto summary = AssetCooker::CookProject(MakeOptions(fixture));
    EXPECT_TRUE(summary.Succeeded()) << summary.FormatSummary();
    EXPECT_EQ(summary.cooked, 4u);
    EXPECT_EQ(summary.unsupported, 1u);
    EXPECT_EQ(summary.WarningCount(), 1u);

    std::error_code cleanupError;
    std::filesystem::remove_all(fixture.root, cleanupError);
}
