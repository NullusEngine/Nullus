#include <gtest/gtest.h>

#include "Assets/NativeArtifactContainer.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"

#include <cstdint>

using namespace NLS::Core::Assets;

namespace
{
std::vector<uint8_t> MakePayload()
{
    return {0x10u, 0x20u, 0x30u, 0x40u, 0x50u};
}

void WriteU64(std::vector<uint8_t>& bytes, const size_t offset, const uint64_t value)
{
    ASSERT_LE(offset + sizeof(uint64_t), bytes.size());
    for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
        bytes[offset + byteIndex] = static_cast<uint8_t>((value >> (byteIndex * 8u)) & 0xFFu);
}
}

TEST(NativeArtifactContainerTests, WritesMagicHeaderMetadataAndPayloadHash)
{
    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Mesh;
    metadata.schemaName = "mesh";
    metadata.schemaVersion = 7u;
    metadata.sourceAssetId = AssetId(NLS::Guid::Parse("11111111-2222-4333-8444-555555555555"));
    metadata.subAssetKey = "mesh:body";
    metadata.importerId = "scene-model";
    metadata.importerVersion = 5u;
    metadata.targetPlatform = "editor";
    metadata.dependencies.push_back({
        AssetDependencyKind::SourceFileHash,
        "Assets/Models/Hero.fbx",
        "123:456"
    });

    const auto payload = MakePayload();
    const auto bytes = WriteNativeArtifactContainer(metadata, payload);

    ASSERT_GE(bytes.size(), NativeArtifactContainerHeaderSize());
    EXPECT_EQ(bytes[0], 'N');
    EXPECT_EQ(bytes[1], 'L');
    EXPECT_EQ(bytes[2], 'S');
    EXPECT_EQ(bytes[3], 'A');

    const auto parsed = ReadNativeArtifactContainer(bytes, ArtifactType::Mesh, 7u);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->metadata.artifactType, ArtifactType::Mesh);
    EXPECT_EQ(parsed->metadata.schemaName, "mesh");
    EXPECT_EQ(parsed->metadata.schemaVersion, 7u);
    EXPECT_EQ(parsed->metadata.sourceAssetId, metadata.sourceAssetId);
    EXPECT_EQ(parsed->metadata.subAssetKey, "mesh:body");
    EXPECT_EQ(parsed->metadata.importerId, "scene-model");
    EXPECT_EQ(parsed->metadata.importerVersion, 5u);
    EXPECT_EQ(parsed->metadata.targetPlatform, "editor");
    ASSERT_EQ(parsed->metadata.dependencies.size(), 1u);
    EXPECT_EQ(parsed->metadata.dependencies[0].kind, AssetDependencyKind::SourceFileHash);
    EXPECT_EQ(parsed->metadata.dependencies[0].value, "Assets/Models/Hero.fbx");
    EXPECT_EQ(parsed->metadata.dependencies[0].hashOrVersion, "123:456");
    EXPECT_EQ(parsed->metadata.payloadHash, ComputeNativeArtifactPayloadHash(payload));
    EXPECT_EQ(parsed->metadata.dependencyHash, ComputeNativeArtifactDependencyHash(metadata.dependencies));
    EXPECT_EQ(parsed->payload, payload);
}

TEST(NativeArtifactContainerTests, RoundTripsPrefabValidationDependency)
{
    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Prefab;
    metadata.schemaName = "prefab";
    metadata.schemaVersion = 1u;
    metadata.dependencies.push_back({
        AssetDependencyKind::PrefabValidation,
        "prefab:main",
        "123456789"
    });

    const auto payload = MakePayload();
    const auto bytes = WriteNativeArtifactContainer(metadata, payload);
    const auto parsed = ReadNativeArtifactContainer(bytes, ArtifactType::Prefab, 1u);

    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->metadata.dependencies.size(), 1u);
    EXPECT_EQ(parsed->metadata.dependencies[0].kind, AssetDependencyKind::PrefabValidation);
    EXPECT_EQ(parsed->metadata.dependencies[0].value, "prefab:main");
    EXPECT_EQ(parsed->metadata.dependencies[0].hashOrVersion, "123456789");
}

TEST(NativeArtifactContainerTests, RejectsPayloadTampering)
{
    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Texture;
    metadata.schemaName = "texture";
    metadata.schemaVersion = 2u;

    auto bytes = WriteNativeArtifactContainer(metadata, MakePayload());
    ASSERT_FALSE(bytes.empty());
    bytes.back() ^= 0x7Fu;

    EXPECT_FALSE(ReadNativeArtifactContainer(bytes, ArtifactType::Texture, 2u).has_value());
}

TEST(NativeArtifactContainerTests, ContentAddressedStorageMaySkipRedundantPayloadHash)
{
    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Texture;
    metadata.schemaName = "texture";
    metadata.schemaVersion = 2u;

    auto bytes = WriteNativeArtifactContainer(metadata, MakePayload());
    ASSERT_FALSE(bytes.empty());
    bytes.back() ^= 0x7Fu;

    EXPECT_FALSE(ReadNativeArtifactContainerView(
        bytes,
        ArtifactType::Texture,
        2u,
        NativeArtifactPayloadValidation::VerifyHash).has_value());
    EXPECT_TRUE(ReadNativeArtifactContainerView(
        bytes,
        ArtifactType::Texture,
        2u,
        NativeArtifactPayloadValidation::TrustContentAddressedStorage).has_value());
}

TEST(NativeArtifactContainerTests, RejectsWrongKindOrSchema)
{
    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Material;
    metadata.schemaName = "material";
    metadata.schemaVersion = 3u;

    const auto bytes = WriteNativeArtifactContainer(metadata, MakePayload());
    EXPECT_FALSE(ReadNativeArtifactContainer(bytes, ArtifactType::Mesh, 3u).has_value());
    EXPECT_FALSE(ReadNativeArtifactContainer(bytes, ArtifactType::Material, 4u).has_value());
}

TEST(NativeArtifactContainerTests, RejectsOverflowingHeaderSizesBeforeSlicingPayload)
{
    NativeArtifactMetadata metadata;
    metadata.artifactType = ArtifactType::Mesh;
    metadata.schemaName = "mesh";
    metadata.schemaVersion = 3u;

    auto bytes = WriteNativeArtifactContainer(metadata, MakePayload());
    ASSERT_GE(bytes.size(), NativeArtifactContainerHeaderSize());

    constexpr size_t metadataSizeOffset = 24u;
    constexpr size_t payloadSizeOffset = 32u;
    constexpr size_t payloadOffsetOffset = 40u;

    auto overflowingMetadata = bytes;
    WriteU64(overflowingMetadata, metadataSizeOffset, UINT64_MAX);
    WriteU64(overflowingMetadata, payloadOffsetOffset, NativeArtifactContainerHeaderSize() - 1u);
    EXPECT_FALSE(ReadNativeArtifactContainer(overflowingMetadata, ArtifactType::Mesh, 3u).has_value());

    auto overflowingPayload = bytes;
    WriteU64(overflowingPayload, payloadSizeOffset, UINT64_MAX);
    EXPECT_FALSE(ReadNativeArtifactContainer(overflowingPayload, ArtifactType::Mesh, 3u).has_value());
}

TEST(NativeArtifactContainerTests, MeshAndTextureArtifactsUseNativeContainer)
{
    NLS::Render::Assets::MeshArtifactData mesh;
    mesh.indices = {0u, 1u, 2u};

    const auto meshBytes = NLS::Render::Assets::SerializeMeshArtifact(mesh);
    ASSERT_GE(meshBytes.size(), NativeArtifactContainerHeaderSize());
    EXPECT_EQ(meshBytes[0], 'N');
    EXPECT_EQ(meshBytes[1], 'L');
    EXPECT_EQ(meshBytes[2], 'S');
    EXPECT_EQ(meshBytes[3], 'A');
    EXPECT_TRUE(ReadNativeArtifactContainer(meshBytes, ArtifactType::Mesh, 3u).has_value());
    EXPECT_TRUE(NLS::Render::Assets::DeserializeMeshArtifact(meshBytes).has_value());

    NLS::Render::Assets::TextureArtifactData texture;
    texture.width = 1u;
    texture.height = 1u;
    texture.format = NLS::Render::RHI::TextureFormat::RGBA8;
    texture.colorSpace = NLS::Render::Assets::TextureArtifactColorSpace::Srgb;
    texture.mips.push_back({0u, 1u, 1u, 4u, 4u, {255u, 0u, 0u, 255u}});

    const auto textureBytes = NLS::Render::Assets::SerializeTextureArtifact(texture);
    ASSERT_GE(textureBytes.size(), NativeArtifactContainerHeaderSize());
    EXPECT_EQ(textureBytes[0], 'N');
    EXPECT_EQ(textureBytes[1], 'L');
    EXPECT_EQ(textureBytes[2], 'S');
    EXPECT_EQ(textureBytes[3], 'A');
    EXPECT_TRUE(ReadNativeArtifactContainer(textureBytes, ArtifactType::Texture, 4u).has_value());
    EXPECT_TRUE(NLS::Render::Assets::DeserializeTextureArtifact(textureBytes).has_value());
}

TEST(NativeArtifactContainerTests, ShaderArtifactUsesNativeContainerAndRejectsBarePayload)
{
    NLS::Render::Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Shaders/Hero.hlsl";
    artifact.subAssetKey = "shader:Hero";
    artifact.targetPlatform = "editor";

    const auto bytes = NLS::Render::Assets::SerializeShaderArtifact(artifact);
    ASSERT_GE(bytes.size(), NativeArtifactContainerHeaderSize());
    EXPECT_EQ(bytes[0], 'N');
    EXPECT_EQ(bytes[1], 'L');
    EXPECT_EQ(bytes[2], 'S');
    EXPECT_EQ(bytes[3], 'A');

    const auto container = ReadNativeArtifactContainer(bytes, ArtifactType::Shader, 1u);
    ASSERT_TRUE(container.has_value());
    EXPECT_EQ(container->metadata.schemaName, "shader");
    EXPECT_TRUE(NLS::Render::Assets::DeserializeShaderArtifact(bytes).has_value());

    const std::string barePayload =
        "NULLUS_IMPORTED_SHADER_ARTIFACT=1\n"
        "SOURCE=Assets/Shaders/Hero.hlsl\n"
        "SUB_ASSET=shader:Hero\n"
        "TARGET_PLATFORM=editor\n";
    const std::vector<uint8_t> bareBytes(barePayload.begin(), barePayload.end());
    EXPECT_FALSE(NLS::Render::Assets::DeserializeShaderArtifact(bareBytes).has_value());
}
